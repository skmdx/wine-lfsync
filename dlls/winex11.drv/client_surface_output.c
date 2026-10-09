/*
 * Compositor output storage, checkpoints and native presentation
 *
 * Copyright 2026 Wine contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <assert.h>
#include <X11/extensions/shape.h>

#include "ntstatus.h"
#include "x11drv.h"
#include "xcomposite.h"
#include "client_surface_cache.h"
#include "wine/server.h"
#include "client_surface_compositor_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);
WINE_DECLARE_DEBUG_CHANNEL(csperf);

static void flush_client_surface_compositor_mailbox(
    struct client_surface_compositor_target *target );

static int compare_client_surface_compositor_pixmap( const void *key, const struct rb_entry *entry )
{
    const struct client_surface_compositor_frame *frame =
        CONTAINING_RECORD( entry, const struct client_surface_compositor_frame, pixmap_entry );
    Pixmap a = *(const Pixmap *)key, b = frame->pixmap;

    return (a > b) - (a < b);
}

static struct rb_tree client_surface_compositor_pixmaps = {compare_client_surface_compositor_pixmap};

struct client_surface_cache_image *client_surface_compositor_frame_image(
    const struct client_surface_compositor_frame *frame )
{
    return frame->retained_image ? frame->retained_image : frame->image;
}

void retain_client_surface_frame_image( struct client_surface_compositor_frame *frame,
                                       struct client_surface_cache_image *image )
{
    if (image) client_surface_cache_acquire( image );
    client_surface_cache_release( frame->retained_image );
    if (frame->retained_image || image) frame->revision = 0;
    frame->retained_image = image;
}

void set_client_surface_compositor_pixmap( struct client_surface_compositor_frame *frame, Pixmap pixmap,
                                                 struct client_surface_cache_image *image )
{
    assert( !frame->serial );
    retain_client_surface_frame_image( frame, NULL );
    if (frame->pixmap) rb_remove( &client_surface_compositor_pixmaps, &frame->pixmap_entry );
    frame->pixmap = pixmap;
    frame->image = image;
    if (pixmap)
    {
        assert( !rb_get( &client_surface_compositor_pixmaps, &pixmap ) );
        rb_put( &client_surface_compositor_pixmaps, &pixmap, &frame->pixmap_entry );
    }
}

/* Read leases are admitted only on the actor, after checking outstanding
 * writers. Selection and every explicit destination write run on that same
 * actor, so the shared check and native submission cannot race a new reader. */
BOOL client_surface_compositor_frame_writable( const struct client_surface_compositor_frame *frame )
{
    return frame->image && !client_surface_cache_shared( frame->image );
}
static struct client_surface_compositor_target *client_surface_compositor_targets;
static struct client_surface_compositor_target *client_surface_compositor_next_target;
static unsigned int client_surface_compositor_target_count;
static UINT64 client_surface_compositor_target_generation;

static int compare_client_surface_compositor_target( const void *key, const struct rb_entry *entry )
{
    const struct client_surface_compositor_target *target =
        CONTAINING_RECORD( entry, const struct client_surface_compositor_target, registry_entry );
    ULONG_PTR a = (ULONG_PTR)key, b = (ULONG_PTR)target->toplevel;

    return (a > b) - (a < b);
}

/* The actor alone publishes and removes targets. Keep lookup independent of
 * maintenance traversal, without adding an allocation or a metadata lock to
 * each source/event. Both intrusive indices share the target's accounting. */
static struct rb_tree client_surface_compositor_target_registry = {compare_client_surface_compositor_target};

static void register_client_surface_compositor_target( struct client_surface_compositor_target *target )
{
    assert( !rb_get( &client_surface_compositor_target_registry, target->toplevel ) );
    rb_put( &client_surface_compositor_target_registry, target->toplevel, &target->registry_entry );
    target->next = client_surface_compositor_targets;
    target->prev = &client_surface_compositor_targets;
    if (target->next) target->next->prev = &target->next;
    client_surface_compositor_targets = target;
    if (!client_surface_compositor_next_target) client_surface_compositor_next_target = target;
    ++client_surface_compositor_target_count;
    ++client_surface_compositor_target_generation;
}

static BOOL client_surface_output_is_window( const struct client_surface_output_allocation *allocation )
{
    switch (allocation->phase)
    {
    case CLIENT_SURFACE_OUTPUT_WINDOW_SCENE_WAIT:
    case CLIENT_SURFACE_OUTPUT_WINDOW_QUERY:
    case CLIENT_SURFACE_OUTPUT_WINDOW_COPY_WAIT:
    case CLIENT_SURFACE_OUTPUT_WINDOW_PRESENT:
        return TRUE;
    default:
        return FALSE;
    }
}

BOOL client_surface_output_waits_scene( const struct client_surface_output_allocation *allocation )
{
    return allocation->phase == CLIENT_SURFACE_OUTPUT_PAIR_SCENE_WAIT ||
           allocation->phase == CLIENT_SURFACE_OUTPUT_WINDOW_SCENE_WAIT;
}

static UINT64 client_surface_output_allocation_serial;
static struct list client_surface_seed_requests = LIST_INIT( client_surface_seed_requests );
static unsigned int client_surface_seed_request_count;
static struct list client_surface_output_notifications = LIST_INIT( client_surface_output_notifications );
static unsigned int client_surface_output_notification_count;

static struct client_surface_output_allocation *client_surface_output_allocations;

struct client_surface_compositor_frame *get_client_surface_compositor_pixmap(
    struct client_surface_compositor_target *target, Pixmap pixmap )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        if (target->frames[i].pixmap == pixmap) return &target->frames[i];
    return NULL;
}

void detach_client_surface_output_transform( struct client_surface_compositor_target *target )
{
    if (!target->transform) return;
    TRACE_(csperf)( "ticks=%llu event=output_transform_detach transform=%p hwnd=%p destination=%lx\n",
                   client_surface_perf_time(), target->transform, target->toplevel, target->window );
    abort_client_surface_output_transform_assembly( target, target->transform );
    target->transform = NULL;
    /* The worker owns the image write until its checked completion. Shared
     * storage cannot become a new destination even after logical removal. */
    target->replay_member = 0;
}

void note_client_surface_compositor_damage(
    struct client_surface_compositor_target *target,
    struct client_surface_compositor_frame *frame, const RECT *rect )
{
    struct client_surface_compositor_damage *damage;
    RECT bounds = {0, 0, target->window_width, target->window_height};

    if (!(++target->revision))
    {
        unsigned int i;

        target->revision = 1;
        memset( target->damages, 0, sizeof(target->damages) );
        for (i = 0; i < ARRAY_SIZE(target->frames); ++i) target->frames[i].revision = 0;
    }
    damage = &target->damages[target->revision % ARRAY_SIZE(target->damages)];
    damage->revision = target->revision;
    /* A child can extend beyond its owner. Only pixels inside the output
     * need catching up when this frame is reused as a later checkpoint. */
    intersect_rect( &damage->rect, rect, &bounds );
    frame->revision = target->revision;
    target->latest = frame->pixmap;
}

static void note_client_surface_compositor_snapshot(
    struct client_surface_compositor_target *target, Pixmap pixmap )
{
    struct client_surface_compositor_frame *frame;
    RECT rect = {0, 0, target->window_width, target->window_height};

    if ((frame = get_client_surface_compositor_pixmap( target, pixmap )))
    {
        /* A GUI checkpoint writes the installed pool image. Native requests
         * retain their own input if an older cache image is still in flight. */
        retain_client_surface_frame_image( frame, NULL );
        note_client_surface_compositor_damage( target, frame, &rect );
    }
}

static uint32_t client_surface_present_serial;

static struct client_surface_compositor_target *alloc_client_surface_compositor_target( HWND toplevel )
{
    struct client_surface_memory_scope memory = {0};
    struct client_surface_compositor_target *target;

    if (!(target = alloc_client_surface_compositor_metadata( toplevel, sizeof(*target), &memory ))) return NULL;
    target->memory = memory;
    target->toplevel = toplevel;
    if (!init_client_surface_target_notifications( target ))
    {
        client_surface_free_owned_metadata( &target->memory, target, sizeof(*target) );
        return NULL;
    }
    return target;
}

static void free_client_surface_compositor_target( struct client_surface_compositor_target *target )
{
    destroy_client_surface_target_notifications( target );
    assert( !target->native_presents.head );
    x11drv_native_window_release( target->window_owner );
    client_surface_free_owned_metadata( &target->memory, target, sizeof(*target) );
}

void update_client_surface_notification_plan( struct client_surface_compositor_target *target )
{
    struct client_surface_owner_notifications *notifications = target->notifications;

    pthread_mutex_lock( &client_surface_compositor_mutex );
    notifications->direct_plan = (struct client_surface_direct_completion){0};
    if (target->scene.valid && target->scene.strategy == DIRECT_ATTACH)
        notifications->direct_plan = (struct client_surface_direct_completion){
            .identity = target->scene.direct_identity, .scene_epoch = target->scene.epoch,
            .source = target->scene.direct_drawable};
    pthread_mutex_unlock( &client_surface_compositor_mutex );
}

void release_client_surface_output_checkpoint( struct client_surface_output_allocation *allocation )
{
    x11drv_native_window_release( allocation->geometry_query.child_owner );
    allocation->geometry_query.child_owner = NULL;
    client_surface_cache_release( allocation->source_image );
    allocation->source_image = NULL;
    if (allocation->content_owned)
    {
        x11drv_native_window_release_content( allocation->window_owner, allocation->content_epoch, FALSE );
        allocation->content_owned = FALSE;
    }
    if (allocation->window_owner) x11drv_native_window_release( allocation->window_owner );
    allocation->window_owner = NULL;
}

void free_client_surface_pending_allocation( struct client_surface_output_allocation *allocation )
{
    assert( !allocation->pending );
    if (allocation->phase == CLIENT_SURFACE_OUTPUT_PAIR_CHECKPOINT && !allocation->release.async)
    {
        /* The actor drops its scalar adoption identity before freeing this
         * admitted record. GUI cancellation never looks up an actor target. */
        pthread_mutex_lock( &client_surface_compositor_mutex );
        allocation->release.op = CLIENT_SURFACE_COMPOSITOR_DROP_SEED;
        allocation->release.async = TRUE;
        enqueue_client_surface_compositor_job( &allocation->release );
        pthread_mutex_unlock( &client_surface_compositor_mutex );
        wake_client_surface_compositor();
        return;
    }
    release_client_surface_output_checkpoint( allocation );
    client_surface_cache_release( allocation->images[0] );
    client_surface_cache_release( allocation->images[1] );
    if (!allocation->release.async)
    {
        release_client_surface_compositor_queue( allocation->release.queue );
        free_client_surface_compositor_release( &allocation->memory, allocation, 1, sizeof(*allocation) );
    }
}

void client_surface_output_allocation_complete( void *context, BOOL success )
{
    struct client_surface_output_allocation *allocation = context;
    BOOL abandoned = FALSE;

    pthread_mutex_lock( &client_surface_compositor_mutex );
    allocation->failed |= !success;
    assert( allocation->pending );
    if (!--allocation->pending)
    {
        if (!allocation->failed && !client_surface_output_is_window( allocation ))
        {
            allocation->pixmaps[0] = client_surface_cache_pixmap( allocation->images[0] );
            allocation->pixmaps[1] = client_surface_cache_pixmap( allocation->images[1] );
        }
        abandoned = allocation->abandoned;
        if (!abandoned)
        {
            list_add_tail( &client_surface_output_notifications, &allocation->notification_entry );
            ++client_surface_output_notification_count;
        }
    }
    TRACE_(csperf)( "ticks=%llu event=%s request=%p serial=%llu pending=%u failed=%u abandoned=%u\n",
                   client_surface_perf_time(), allocation->phase == CLIENT_SURFACE_OUTPUT_WINDOW_PRESENT ? "output_publication_complete" :
                   client_surface_output_is_window( allocation ) ? "geometry_query_complete" :
                   allocation->phase == CLIENT_SURFACE_OUTPUT_PAIR_CHECKPOINT ? "output_pair_copy_complete" : "output_pair_create_complete",
                   allocation, (unsigned long long)allocation->serial, allocation->pending,
                   allocation->failed, allocation->abandoned );
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    /* A cancelled GUI owner may release the object immediately after
     * unlocking. Only the abandoned callback retains final ownership here. */
    if (abandoned) free_client_surface_pending_allocation( allocation );
}

static struct client_surface_output_allocation *alloc_client_surface_output_request(
    struct client_surface_compositor_job *job, enum client_surface_output_phase phase )
{
    struct client_surface_memory_scope memory = {0};
    struct client_surface_output_allocation *allocation;

    if (!x11drv_reserve_release_capacity( 1, sizeof(*allocation) )) return NULL;
    if (!(allocation = alloc_client_surface_compositor_metadata( job->toplevel, sizeof(*allocation), &memory )))
    {
        release_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_RELEASE_CAPACITY, 1, sizeof(*allocation), NULL );
        return NULL;
    }
    allocation->memory = memory;
    allocation->phase = phase;
    list_init( &allocation->notification_entry );
    list_init( &allocation->seed_entry );
    allocation->release.queue = get_client_surface_compositor_queue( job->toplevel );
    assert( allocation->release.queue );
    allocation->release.toplevel = job->toplevel;
    allocation->width = job->u.pool.width;
    allocation->height = job->u.pool.height;
    allocation->depth = job->u.pool.depth;
    if (!++client_surface_output_allocation_serial) ++client_surface_output_allocation_serial;
    allocation->serial = client_surface_output_allocation_serial;
    return allocation;
}

static void query_client_surface_window( struct client_surface_native_work *work )
{
    struct client_surface_window_query *query = CONTAINING_RECORD( work, struct client_surface_window_query, work );

    query->waiting = !x11drv_native_window_read_ready( &query->read );
    if (query->child_owner) query->waiting |= !x11drv_native_window_read_ready( &query->child_read );
    if (query->waiting) return;
    query->success = client_surface_native_query_window( query->window, &query->width, &query->height,
                                                        &query->map_state, &query->error );
    if (query->success && query->child_owner && query->map_state == IsViewable &&
        query->width == query->direct_width && query->height == query->direct_height)
        query->direct_checked = client_surface_native_check_direct( x11drv_native_window_content( query->read.window ), query->child,
            query->direct_width, query->direct_height, &query->direct_rect );
    if (query->child_owner) x11drv_native_window_read_finish( &query->child_read );
    x11drv_native_window_read_finish( &query->read );
}

static void finish_client_surface_window_query( struct client_surface_native_work *work )
{
    struct client_surface_window_query *query = CONTAINING_RECORD( work, struct client_surface_window_query, work );
    void (*finished)( struct client_surface_window_query *query ) = query->finished;

    if (query->waiting)
    {
        client_surface_submit_native_work( work );
        return;
    }
    /* DIRECT's actor may release this request as soon as it observes the
     * completion. Publish all results last and never reload its callback. */
    WriteRelease( &query->complete, TRUE );
    if (finished) finished( query );
    wake_client_surface_compositor();
}

void start_client_surface_window_query( struct client_surface_window_query *query,
                                               struct x11drv_native_window *window,
                                               void (*finished)( struct client_surface_window_query *query ) )
{
    assert( !query->started );
    query->started = TRUE;
    query->map_state = -1;
    query->window = x11drv_native_window_read_init( &query->read, window );
    if (query->child_owner) query->child = x11drv_native_window_read_init( &query->child_read, query->child_owner );
    query->finished = finished;
    query->work.execute = query_client_surface_window;
    query->work.finished = finish_client_surface_window_query;
    client_surface_submit_native_work( &query->work );
}

static void complete_client_surface_geometry_query( struct client_surface_window_query *query )
{
    struct client_surface_output_allocation *allocation =
        CONTAINING_RECORD( query, struct client_surface_output_allocation, geometry_query );

    allocation->window_width = query->width;
    allocation->window_height = query->height;
    TRACE_(csperf)( "ticks=%llu event=geometry_query_reply request=%p serial=%llu width=%u height=%u success=%u\n",
                   client_surface_perf_time(), allocation, (unsigned long long)allocation->serial,
                   query->width, query->height, query->success );
    if (query->child_owner)
        TRACE_(csperf)( "ticks=%llu event=direct_query_reply request=%p serial=%llu previous=%llu child=%lx checked=%u\n",
                       client_surface_perf_time(), allocation, (unsigned long long)allocation->serial,
                       (unsigned long long)query->direct_epoch, query->child, query->direct_checked );
    client_surface_output_allocation_complete( allocation, query->success );
}

BOOL create_client_surface_window_query( struct client_surface_compositor_job *job )
{
    struct client_surface_output_allocation *allocation = job->u.pool.allocation;
    struct client_surface_window_query *query;
    struct client_surface_compositor_target *target;

    if (!allocation)
    {
        if (!(allocation = alloc_client_surface_output_request( job, CLIENT_SURFACE_OUTPUT_WINDOW_QUERY ))) return FALSE;
        allocation->window_owner = x11drv_native_window_acquire( job->u.pool.window_owner );
        allocation->window = job->u.pool.destination;
        allocation->source = job->u.pool.source;
        allocation->geometry_update = job->u.pool.geometry_update;
        job->u.pool.allocation = allocation;
    }
    assert( client_surface_output_is_window( allocation ) && !allocation->pending && list_empty( &allocation->notification_entry ) );
    allocation->scene = job->u.pool.scene;
    allocation->phase = allocation->scene.epoch ? CLIENT_SURFACE_OUTPUT_WINDOW_QUERY : CLIENT_SURFACE_OUTPUT_WINDOW_SCENE_WAIT;
    if (allocation->phase == CLIENT_SURFACE_OUTPUT_WINDOW_SCENE_WAIT)
    {
        /* Odd GUI epochs cannot identify native input. Admit the bounded
         * continuation now, then start its query after the scene wake. */
        pthread_mutex_lock( &client_surface_compositor_mutex );
        list_add_tail( &client_surface_output_notifications, &allocation->notification_entry );
        ++client_surface_output_notification_count;
        pthread_mutex_unlock( &client_surface_compositor_mutex );
        return TRUE;
    }
    query = &allocation->geometry_query;
    target = find_client_surface_compositor_target( job->toplevel );
    if (job->u.pool.geometry_update == WINE_PREPARE_CLIENT_SURFACES && target &&
        target->window == allocation->window && target->scene.valid && target->scene.strategy == DIRECT_ATTACH)
    {
        query->child_owner = x11drv_native_window_acquire( target->scene.direct_owner );
        query->direct_epoch = target->scene.epoch;
        query->direct_identity = target->scene.direct_identity;
        query->direct_rect = job->u.pool.geometry_rect;
        query->direct_width = job->u.pool.window_width;
        query->direct_height = job->u.pool.window_height;
    }
    allocation->pending = 1;
    TRACE_(csperf)( "ticks=%llu event=geometry_query_queue request=%p serial=%llu window=%lx epoch=%llu\n",
                   client_surface_perf_time(), allocation, (unsigned long long)allocation->serial,
                   allocation->window, (unsigned long long)allocation->scene.epoch );
    start_client_surface_window_query( &allocation->geometry_query, allocation->window_owner,
                                       complete_client_surface_geometry_query );
    return TRUE;
}

BOOL create_client_surface_output_allocation( struct client_surface_compositor_job *job )
{
    struct client_surface_output_allocation *allocation = alloc_client_surface_output_request( job, CLIENT_SURFACE_OUTPUT_PAIR_CREATE );

    if (!allocation) return FALSE;
    allocation->bytes = 2 * client_surface_pixmap_bytes( allocation->width, allocation->height, allocation->depth );
    if (!client_surface_cache_reserve_output_pair( &allocation->memory, allocation->bytes / 2,
                                                   wake_client_surface_compositor, allocation->images ))
    {
        free_client_surface_pending_allocation( allocation );
        return FALSE;
    }
    allocation->pending = 2;
    job->u.pool.allocation = allocation;
    client_surface_cache_create_output_pair( allocation->images, allocation->width, allocation->height,
                                              allocation->depth, client_surface_output_allocation_complete, allocation );
    return TRUE;
}

static void register_client_surface_output_allocation( struct client_surface_output_allocation *allocation )
{
    pthread_mutex_lock( &client_surface_compositor_mutex );
    allocation->next = client_surface_output_allocations;
    client_surface_output_allocations = allocation;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
}

BOOL release_client_surface_output_allocation( const Pixmap pixmaps[2] )
{
    struct client_surface_output_allocation **cursor, *allocation;

    pthread_mutex_lock( &client_surface_compositor_mutex );
    for (cursor = &client_surface_output_allocations; (allocation = *cursor); cursor = &allocation->next)
        if ((allocation->pixmaps[0] == pixmaps[0] && allocation->pixmaps[1] == pixmaps[1]) ||
            (allocation->pixmaps[0] == pixmaps[1] && allocation->pixmaps[1] == pixmaps[0])) break;
    assert( allocation );
    *cursor = allocation->next;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    /* Every copy and Present user has drained before dispatch. The worker
     * receives no target or release-job pointer, and its native reply can
     * arrive after this allocation's GUI/actor bookkeeping is gone. */
    client_surface_cache_release( allocation->images[0] );
    client_surface_cache_release( allocation->images[1] );
    /* A rollback has no queued node. Otherwise the dispatcher still owns
     * release, including its result and queue link, until it retires it. */
    if (!allocation->release.async)
    {
        release_client_surface_compositor_queue( allocation->release.queue );
        free_client_surface_compositor_release( &allocation->memory, allocation, 1, sizeof(*allocation) );
    }
    return TRUE;
}

static void hide_client_surface_present_window( struct client_surface_compositor_target *target )
{
    target->present_window = 0;
    if (!target->content_redirected) return;
    x11drv_native_window_release_content( target->window_owner, target->content_epoch, TRUE );
    target->content_redirected = FALSE;
}

static void finish_client_surface_compositor_frame(
    struct client_surface_compositor_target *target,
    struct client_surface_compositor_frame *frame )
{
    if (!frame->serial || frame->request_pending || !frame->complete || !frame->idle) return;
    TRACE( "%s serial %u pixmap %#lx completed and became idle\n",
           frame->native_present.copy ? "staged copy" : "X Present", frame->serial, frame->pixmap );
    frame->serial = 0;
    frame->complete = frame->idle = FALSE;
    /* Give completed sources a chance to replace a steady mailbox before it is
     * submitted. A reserved topology publication keeps its original order. */
    if (target->mailbox_publish_generation) flush_client_surface_compositor_mailbox( target );
}

static void complete_client_surface_compositor_frame(
    struct client_surface_compositor_target *target,
    struct client_surface_compositor_frame *frame )
{
    BOOL success = frame->last_complete_success;

    if (frame->request_pending || !frame->complete) return;
    client_surface_release_native_present( &frame->native_present );
    if (frame->waiter)
    {
        assert( frame->waiter->op == CLIENT_SURFACE_COMPOSITOR_PRESENT );
        frame->waiter->u.present.done = TRUE;
        frame->waiter->result = success;
        frame->waiter = NULL;
    }
    if (success && IsRectEmpty( &frame->native_present.copy_rect ))
    {
        target->published = frame->pixmap;
        target->published_width = frame->width;
        target->published_height = frame->height;
    }
    if (frame->publish_pending)
    {
        publish_client_surface_handoff_generation( target->toplevel,
            frame->publish_generation, frame->publish_epoch, success );
        frame->publish_pending = FALSE;
    }
}

static struct client_surface_compositor_frame *acquire_client_surface_compositor_frame(
    struct client_surface_compositor_target *target, Pixmap requested )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
    {
        struct client_surface_compositor_frame *frame = &target->frames[i];

        if (frame->pixmap == requested && !frame->serial) return frame;
    }
    return NULL;
}

unsigned int count_client_surface_compositor_frames(
    const struct client_surface_compositor_target *target )
{
    unsigned int count = 0, i;

    for (i = 0; i < ARRAY_SIZE(target->frames); ++i) count += !!target->frames[i].serial;
    return count;
}

static void process_client_surface_present_events( struct client_surface_compositor_target *target )
{
    RECT rect;

    if (target->present_window && x11drv_native_window_take_expose( target->window_owner, &rect ))
        add_bounds_rect( &target->restore_rect, &rect );
}

BOOL process_client_surface_native_present( struct client_surface_compositor_target *target )
{
    unsigned int i;
    BOOL progressed = FALSE;

    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
    {
        struct client_surface_compositor_frame *frame = &target->frames[i];
        struct client_surface_native_present *present = &frame->native_present;

        if (!frame->request_pending || !ReadAcquire( &present->complete )) continue;
        frame->request_pending = FALSE;
        if (present->copied || !present->success)
        {
            frame->last_complete_success = present->success;
            frame->last_complete_serial = frame->serial;
            frame->complete = frame->idle = TRUE;
        }
        TRACE_(csperf)( "ticks=%llu event=native_present_receipt window=%lx pixmap=%lx serial=%u copy=%u success=%u\n",
                       client_surface_perf_time(), present->window, present->pixmap, frame->serial,
                       present->copied, present->success );
        if (present->copy)
            TRACE_(csperf)( "ticks=%llu event=publish_copy_complete window=%lx pixmap=%lx serial=%u generation=%llu epoch=%llu success=%u\n",
                           client_surface_perf_time(), present->window, present->pixmap, frame->serial,
                           (unsigned long long)frame->publish_generation,
                           (unsigned long long)frame->publish_epoch, present->success );
        if (!present->success && !IsRectEmpty( &present->copy_rect ))
            NtUserPostMessage( target->toplevel, WM_WINE_UPDATEWINDOWSTATE,
                               WINE_UPDATE_CLIENT_SURFACE_HANDOFFS, 0 );
        complete_client_surface_compositor_frame( target, frame );
        finish_client_surface_compositor_frame( target, frame );
        progressed = TRUE;
    }
    /* Mailbox or scene work can replace a checkpoint without completing a
     * native Present in this scan. Return obsolete views in either case;
     * otherwise their source slots wait for unrelated future native work. */
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
    {
        struct client_surface_compositor_frame *frame = &target->frames[i];

        if (!frame->retained_image || frame->serial || frame->pixmap == target->backing ||
            frame->pixmap == target->latest || frame->pixmap == target->published ||
            (target->mailbox_pending && target->mailbox_frame == i) ||
            (target->assembly_generation && target->assembly_frame == i) ||
            client_surface_frame_copy_pending( frame )) continue;
        retain_client_surface_frame_image( frame, NULL );
        progressed = TRUE;
    }
    if (progressed) wake_client_surface_compositor_queues();
    return progressed;
}

BOOL submit_client_surface_present( struct client_surface_compositor_target *target,
                                           struct client_surface_compositor_frame *frame,
                                           UINT64 publish_generation, UINT64 publish_epoch,
                                           const RECT *copy_rect, uint32_t *serial_ret,
                                           const struct client_surface_compositor_binding *binding,
                                           const RECT *damage )
{
    XRectangle single_shape = {0}, *shape = NULL;
    unsigned int shape_count = 0, i, j, index = 0;
    unsigned int first = binding ? binding->scene_index : 0;
    unsigned int end = binding ? first + 1 : target->scene.count;
    uint32_t serial;

    if (!target->window_owner || frame->serial) return FALSE;
    if (!copy_rect && target->scene.valid && target->scene.strategy == OWNER_COMPOSITE)
    {
        for (i = first; i < end; ++i) shape_count += target->scene.layouts[i].clip->rdh.nCount;
        if (shape_count == 1) shape = &single_shape;
        else if (shape_count && !(shape = client_surface_alloc_owned_array( &target->memory, shape_count, sizeof(*shape) )))
            return FALSE;
        for (i = first; i < end; ++i)
        {
            const struct client_surface_scene_layout *layout = &target->scene.layouts[i];
            const XRectangle *rects = (XRectangle *)layout->clip->Buffer;

            for (j = 0; j < layout->clip->rdh.nCount; ++j)
            {
                shape[index] = rects[j];
                shape[index].x += layout->geometry.monitor_rect.left;
                shape[index++].y += layout->geometry.monitor_rect.top;
            }
        }
    }
    if (!(serial = ++client_surface_present_serial)) serial = ++client_surface_present_serial;
    frame->serial = serial;
    frame->last_complete_serial = 0;
    frame->last_complete_success = FALSE;
    frame->complete = frame->idle = FALSE;
    frame->publish_generation = publish_generation;
    frame->publish_epoch = publish_epoch;
    frame->publish_pending = !!publish_generation;
    frame->request_pending = TRUE;
    frame->native_present = (struct client_surface_native_present){
        .window_owner = target->window_owner,
        .window = target->present_window, .content = x11drv_native_window_content( target->window_owner ),
        .pixmap = client_surface_cache_pixmap( client_surface_compositor_frame_image( frame ) ), .serial = serial,
        .source_image = frame->retained_image ? client_surface_cache_acquire( frame->retained_image ) : NULL,
        .width = target->window_width, .height = target->window_height, .copy = TRUE,
        .generation = publish_generation, .epoch = publish_epoch,
        .shape = shape, .shape_count = shape_count,
        .commit_rect = damage ? *damage : (RECT){0, 0, target->window_width, target->window_height},
        .committing = !copy_rect,
        .wake = wake_client_surface_compositor};
    if (shape_count == 1)
    {
        frame->native_present.single_shape = single_shape;
        frame->native_present.shape = &frame->native_present.single_shape;
    }
    /* Serial ownership prevents pool/target release and image reuse through
     * both checked copies. The embedded
     * request is already covered by target admission; submit cannot allocate. */
    if (copy_rect) frame->native_present.copy_rect = *copy_rect;
    TRACE_(csperf)( "ticks=%llu event=native_commit_admit window=%lx pixmap=%lx serial=%u copy=1 commit=%u generation=%llu epoch=%llu\n",
                   client_surface_perf_time(), frame->native_present.content, frame->native_present.pixmap, serial,
                   frame->native_present.committing,
                   (unsigned long long)publish_generation, (unsigned long long)publish_epoch );
    client_surface_submit_native_present( &target->native_presents, &frame->native_present );
    if (serial_ret) *serial_ret = serial;
    return TRUE;
}

static void flush_client_surface_compositor_mailbox(
    struct client_surface_compositor_target *target )
{
    struct client_surface_compositor_frame *frame;

    if (target->quiescing || !target->mailbox_pending ||
        count_client_surface_compositor_frames( target ) >= CLIENT_SURFACE_COMPOSITOR_MAX_INFLIGHT)
        return;
    frame = &target->frames[target->mailbox_frame];
    if (submit_client_surface_present( target, frame,
                                       target->mailbox_publish_generation,
                                       target->mailbox_publish_epoch, NULL, NULL, NULL, NULL ))
    {
        target->mailbox_pending = FALSE;
        target->mailbox_publish_generation = 0;
        target->mailbox_publish_epoch = 0;
        return;
    }

    /* Admission failure cannot bypass an executing request with an actor
     * copy. Release this reservation and let the normal repair path retry. */
    if (target->mailbox_publish_generation)
        publish_client_surface_handoff_generation( target->toplevel,
            target->mailbox_publish_generation, target->mailbox_publish_epoch, FALSE );
    NtUserPostMessage( target->toplevel, WM_WINE_UPDATEWINDOWSTATE,
                       WINE_UPDATE_CLIENT_SURFACE_HANDOFFS, 0 );
    target->mailbox_pending = FALSE;
    target->mailbox_publish_generation = 0;
    target->mailbox_publish_epoch = 0;
}

static void release_client_surface_compositor_mailbox( struct client_surface_compositor_mailbox *mailbox )
{
    client_surface_cache_release( mailbox->image );
    client_surface_free_owned_metadata( &mailbox->memory, mailbox, sizeof(*mailbox) );
}

void retry_client_surface_compositor_mailbox( struct client_surface_compositor_target *target )
{
    struct client_surface_compositor_mailbox *mailbox = target->mailbox;

    /* A failed native allocation is retried after new source/scene input,
     * not on its own completion wake or another target's maintenance. */
    if (!mailbox || mailbox->pending || mailbox->image) return;
    target->mailbox = NULL;
    release_client_surface_compositor_mailbox( mailbox );
}

static void complete_client_surface_compositor_mailbox( void *context, BOOL success )
{
    struct client_surface_compositor_mailbox *mailbox = context;
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( mailbox->toplevel );
    BOOL current = target && target->mailbox == mailbox;

    assert( mailbox->pending );
    mailbox->pending = FALSE;
    TRACE_(csperf)( "ticks=%llu event=output_mailbox_complete mailbox=%p image=%p window=%lx success=%u current=%u\n",
                   client_surface_perf_time(), mailbox, mailbox->image, mailbox->window, success, current );
    /* The allocation object survives target removal and pool replacement.
     * An HWND or target address can be reused, but this live object cannot. */
    if (!current)
    {
        release_client_surface_compositor_mailbox( mailbox );
        return;
    }
    if (!success)
    {
        client_surface_cache_release( mailbox->image );
        mailbox->image = NULL;
        return;
    }
    assert( target->window == mailbox->window && target->width == mailbox->width &&
            target->height == mailbox->height && target->depth == mailbox->depth );
    set_client_surface_compositor_pixmap( &target->frames[2], client_surface_cache_pixmap( mailbox->image ), mailbox->image );
    target->next_frame = 0;
    target->replay_member = 0;
}

static struct client_surface_compositor_frame *alloc_client_surface_compositor_mailbox(
    struct client_surface_compositor_target *target )
{
    struct client_surface_compositor_mailbox *mailbox;
    struct client_surface_memory_scope memory = {0};
    UINT64 bytes;

    /* Keep both completed checkpoints. The empty third slot owns a pending
     * allocation, not permission to delay the actor on native storage work. */
    if (target->mailbox || !target->frames[0].pixmap || !target->frames[1].pixmap) return NULL;
    client_surface_memory_scope_copy( &memory, &target->memory, TRUE );
    if (!(mailbox = client_surface_alloc_scoped_metadata( &memory, 1, sizeof(*mailbox) )))
    {
        client_surface_memory_scope_destroy( &memory );
        return NULL;
    }
    mailbox->memory = memory;
    mailbox->toplevel = target->toplevel;
    mailbox->window = target->window;
    mailbox->width = target->width;
    mailbox->height = target->height;
    mailbox->depth = target->depth;
    bytes = client_surface_pixmap_bytes( target->width, target->height, target->depth );
    mailbox->image = client_surface_cache_create_output( &mailbox->memory, target->window,
        target->width, target->height, target->depth, bytes,
        wake_client_surface_compositor, complete_client_surface_compositor_mailbox, mailbox );
    if (!mailbox->image)
    {
        release_client_surface_compositor_mailbox( mailbox );
        return NULL;
    }
    mailbox->pending = TRUE;
    target->mailbox = mailbox;
    TRACE_(csperf)( "ticks=%llu event=output_mailbox_pending mailbox=%p image=%p window=%lx width=%u height=%u depth=%u\n",
                   client_surface_perf_time(), mailbox, mailbox->image, mailbox->window,
                   mailbox->width, mailbox->height, mailbox->depth );
    return NULL;
}

void free_client_surface_compositor_mailbox( struct client_surface_compositor_target *target )
{
    struct client_surface_compositor_frame *frame = &target->frames[2];
    struct client_surface_compositor_mailbox *mailbox = target->mailbox;

    if (!mailbox) return;
    assert( !frame->serial );
    target->mailbox = NULL;
    set_client_surface_compositor_pixmap( frame, 0, NULL );
    TRACE_(csperf)( "ticks=%llu event=output_mailbox_detach mailbox=%p image=%p window=%lx pending=%u\n",
                   client_surface_perf_time(), mailbox, mailbox->image, mailbox->window, mailbox->pending );
    if (!mailbox->pending) release_client_surface_compositor_mailbox( mailbox );
}

struct client_surface_compositor_frame *get_client_surface_compositor_frame(
    struct client_surface_compositor_target *target, BOOL preserve_backing )
{
    unsigned int i;

    process_client_surface_present_events( target );
    /* A reserved scene's complete image owns its publication ticket until
     * submission. New source images remain in the independent owner cache. */
    if (target->mailbox_pending && target->mailbox_publish_generation) return NULL;
    /* Prefer a private output over the GUI checkpoint. Imported source views
     * can be retained there without copying; the checkpoint must keep its own
     * storage because the GUI may hold it after newer frames are published. */
    for (i = 0; i < ARRAY_SIZE(target->frames) * (preserve_backing ? 1 : 2); ++i)
    {
        unsigned int index = (target->next_frame + i) % ARRAY_SIZE(target->frames);

        /* PUBLISHING can outlive the transaction's Present ticket. The GUI
         * still owns its prepared checkpoint until publication completes. */
        /* Neither a failed copy nor a rejected publication may damage the
         * native published image or the complete catchup checkpoint. When
         * all three images are owned, keep coalescing in the source caches;
         * the existing two Present credits and mailbox still make progress. */
        if (!target->frames[index].pixmap || target->frames[index].serial ||
            target->frames[index].pixmap == target->latest ||
            target->frames[index].pixmap == target->published ||
            ((target->frames[index].pixmap == target->backing) != (i >= ARRAY_SIZE(target->frames))) ||
            (target->mailbox_pending && index == target->mailbox_frame) ||
            (target->assembly_generation && index == target->assembly_frame) ||
            !client_surface_compositor_frame_writable( &target->frames[index] )) continue;
        target->next_frame = (index + 1) % ARRAY_SIZE(target->frames);
        retain_client_surface_frame_image( &target->frames[index], NULL );
        return &target->frames[index];
    }
    return alloc_client_surface_compositor_mailbox( target );
}

void finish_client_surface_compositor_assembly(
    struct client_surface_compositor_target *target, BOOL invalidate )
{
    struct client_surface_compositor_frame *frame;

    if (!target->assembly_generation) return;
    frame = &target->frames[target->assembly_frame];
    target->received = 0;
    if (target->receipts) memset( target->receipts, 0, target->scene.count * sizeof(*target->receipts) );
    if (invalidate)
    {
        SERVER_START_REQ( cancel_client_surface_handoffs )
        {
            req->handle = wine_server_user_handle( target->toplevel );
            req->generation = target->assembly_generation;
            req->scene_generation = target->assembly_epoch;
            wine_server_call( req );
        }
        SERVER_END_REQ;
        /* A cancelled transaction may already have overwritten arbitrary
         * regions of its private frame.  Remove it from the damage lineage so
         * its next use starts with a full copy of the last complete frame. */
        assert( frame->pixmap != target->latest );
        assert( frame->pixmap != target->published );
        frame->revision = 0;
        TRACE( "aborted owner assembly generation %s epoch %s pixmap %#lx\n",
               wine_dbgstr_longlong( target->assembly_generation ),
               wine_dbgstr_longlong( target->assembly_epoch ), frame->pixmap );
    }
    target->assembly_generation = 0;
    target->assembly_epoch = 0;
    target->assembly_frame = 0;
}

void invalidate_client_surface_compositor_assembly( struct client_surface_compositor_target *target )
{
    unsigned int i;

    if (!target->assembly_generation) return;
    /* Earlier chunks only proved regions of this private image. Its next
     * catchup may replace them, so replay all members after detaching even
     * when this scene's epoch survives the cancelled transaction. */
    for (i = 0; i < target->scene.count; ++i)
    {
        struct client_surface_compositor_binding *binding = target->scene.members[i];

        if (binding->source_epoch == target->assembly_epoch)
            binding->source_epoch = binding->source_sequence = 0;
        binding->replay_epoch = 0;
    }
    target->replay_member = 0;
    finish_client_surface_compositor_assembly( target, TRUE );
}

void abort_client_surface_output_transform_assembly( struct client_surface_compositor_target *target,
                                                            struct client_surface_output_transform *transform )
{
    if (!transform->generation || target->transform != transform || !target->assembly_generation ||
        target->assembly_generation != transform->generation || target->assembly_epoch != transform->epoch ||
        target->scene.epoch != transform->epoch ||
        target->frames[target->assembly_frame].image != transform->image) return;
    invalidate_client_surface_compositor_assembly( target );
}

struct client_surface_compositor_frame *acquire_client_surface_compositor_assembly_frame(
    struct client_surface_compositor_target *target )
{
    unsigned int i;

    process_client_surface_present_events( target );
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
    {
        unsigned int index = (target->next_frame + i) % ARRAY_SIZE(target->frames);
        struct client_surface_compositor_frame *frame = &target->frames[index];

        /* This nonzero-generation assembly must also preserve the GUI's
         * prepared backing until its publication has been acknowledged. */
        if (!frame->pixmap || frame->serial ||
            frame->pixmap == target->latest || frame->pixmap == target->published ||
            frame->pixmap == target->backing ||
            (target->mailbox_pending && index == target->mailbox_frame) ||
            !client_surface_compositor_frame_writable( frame ))
            continue;
        target->next_frame = (index + 1) % ARRAY_SIZE(target->frames);
        retain_client_surface_frame_image( frame, NULL );
        return frame;
    }
    return alloc_client_surface_compositor_mailbox( target );
}

struct client_surface_compositor_target *find_client_surface_compositor_target( HWND toplevel )
{
    struct rb_entry *entry;

    if (!(entry = rb_get( &client_surface_compositor_target_registry, toplevel ))) return NULL;
    return CONTAINING_RECORD( entry, struct client_surface_compositor_target, registry_entry );
}

BOOL client_surface_present_on_compositor( struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_target *target =
        find_client_surface_compositor_target( job->toplevel );
    struct client_surface_compositor_frame *frame;

    if (!target || target->window != job->u.present.destination ||
        !(frame = acquire_client_surface_compositor_frame( target, job->u.present.source )))
        return FALSE;
    frame->width = job->u.present.width;
    frame->height = job->u.present.height;
    /* This owner scene snapshot includes GDI pixels written outside the
     * compositor connection.  Record a complete checkpoint for later partial
     * handoffs into other pool entries. */
    note_client_surface_compositor_snapshot( target, job->u.present.source );
    if (!submit_client_surface_present( target, frame, 0, 0, NULL, NULL, NULL, NULL ))
        return FALSE;
    frame->waiter = job;
    job->u.present.started = TRUE;
    job->u.present.start = NtGetTickCount();
    return TRUE;
}

void drain_client_surface_compositor_target(
    struct client_surface_compositor_target *target )
{
    unsigned int i;

    /* The scheduler reaches this boundary only after Complete and Idle.
     * A timeout must never manufacture permission to reuse a pixmap. */
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i) assert( !target->frames[i].serial );
    if (target->mailbox_pending && target->mailbox_publish_generation)
        publish_client_surface_handoff_generation( target->toplevel,
            target->mailbox_publish_generation, target->mailbox_publish_epoch, FALSE );
    target->mailbox_pending = FALSE;
    target->mailbox_publish_generation = 0;
    target->mailbox_publish_epoch = 0;
}

static void free_client_surface_compositor_present_input( struct client_surface_compositor_target *target )
{
    hide_client_surface_present_window( target );
    target->present_window = 0;
}

static BOOL create_client_surface_present_window( struct client_surface_compositor_target *target )
{
    NTSTATUS status = x11drv_native_window_prepare_content( target->window_owner, target->window_width,
                                                            target->window_height, target->depth,
                                                            &target->content_epoch, wake_client_surface_compositor );

    if (status && status != STATUS_NOT_SUPPORTED) return FALSE;
    if (status == STATUS_NOT_SUPPORTED) target->content_epoch = 0;
    x11drv_native_window_select_expose( target->window_owner, wake_client_surface_compositor );
    target->content_redirected = !status;
    target->present_window = target->window;
    TRACE_(csperf)( "ticks=%llu event=content_target window=%lx content=%lx epoch=%llu\n",
                   client_surface_perf_time(), target->window,
                   x11drv_native_window_content( target->window_owner ),
                   (unsigned long long)target->content_epoch );
    return TRUE;
}

BOOL update_client_surface_compositor_target( struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_target *target;
    struct client_surface_output_allocation *allocation = NULL;
    BOOL same_pool, checkpoint, created = FALSE;
    BOOL window_changed, extent_changed, format_changed, backing_changed, image_size_changed;

    if (!(target = find_client_surface_compositor_target( job->toplevel )))
    {
        if (!(target = alloc_client_surface_compositor_target( job->toplevel ))) return FALSE;
        created = TRUE;
    }
    same_pool = target->frames[0].pixmap && target->frames[1].pixmap &&
                ((target->frames[0].pixmap == job->u.pool.pixmaps[0] &&
                 target->frames[1].pixmap == job->u.pool.pixmaps[1]) ||
                 (target->frames[0].pixmap == job->u.pool.pixmaps[1] &&
                 target->frames[1].pixmap == job->u.pool.pixmaps[0]));
    if (!same_pool)
    {
        /* Only a checked checkpoint pair can replace the old pool. A failed
         * checkpoint or target allocation leaves it available for cleanup;
         * composition owns any third image separately after installation. */
        for (allocation = client_surface_output_allocations; allocation; allocation = allocation->next)
            if ((allocation->pixmaps[0] == job->u.pool.pixmaps[0] && allocation->pixmaps[1] == job->u.pool.pixmaps[1]) ||
                (allocation->pixmaps[0] == job->u.pool.pixmaps[1] && allocation->pixmaps[1] == job->u.pool.pixmaps[0])) break;
        if (!allocation) goto failed;
    }
    if (created) register_client_surface_compositor_target( target );
    window_changed = target->window != job->u.pool.destination;
    extent_changed = target->window_width != job->u.pool.window_width ||
                     target->window_height != job->u.pool.window_height;
    format_changed = target->depth != job->u.pool.depth || target->visual != job->u.pool.visual;
    backing_changed = target->backing != job->u.pool.pixmaps[0];
    image_size_changed = target->width != job->u.pool.width || target->height != job->u.pool.height;
    if (window_changed || extent_changed || !same_pool)
        quiesce_client_surface_compositor_target( target );
    checkpoint = !same_pool || backing_changed;
    if (window_changed || extent_changed || format_changed)
    {
        target->scene.valid = FALSE;
        if (window_changed) SetRectEmpty( &target->restore_rect );
    }
    if (target->assembly_generation &&
        (window_changed || backing_changed || extent_changed || format_changed))
        finish_client_surface_compositor_assembly( target, TRUE );
    if ((target->window && window_changed) ||
        (target->frames[0].pixmap && !same_pool))
        drain_client_surface_compositor_target( target );
    /* Selection belongs to the native window, not its replaceable images.
     * Keep it across pool replacement and an acknowledged DIRECT plan. */
    if (target->window && window_changed)
        free_client_surface_compositor_present_input( target );
    if (target->mailbox && target->mailbox->pending &&
        (window_changed || image_size_changed || target->depth != job->u.pool.depth))
        free_client_surface_compositor_mailbox( target );
    retry_client_surface_compositor_mailbox( target );
    if (target->window_owner != job->u.pool.window_owner)
    {
        struct x11drv_native_window *previous = target->window_owner;
        target->window_owner = x11drv_native_window_acquire( job->u.pool.window_owner );
        x11drv_native_window_release( previous );
    }
    target->window = job->u.pool.destination;
    if (!same_pool)
    {
        unsigned int i;

        free_client_surface_compositor_mailbox( target );
        for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        {
            if (i < 2) set_client_surface_compositor_pixmap( &target->frames[i], 0, NULL );
        }
        memset( target->frames, 0, sizeof(target->frames) );
        for (i = 0; i < 2; ++i)
            set_client_surface_compositor_pixmap( &target->frames[i], job->u.pool.pixmaps[i],
                allocation->images[allocation->pixmaps[0] != job->u.pool.pixmaps[i]] );
        target->published = job->u.pool.pixmaps[0];
        target->published_width = job->u.pool.valid_width;
        target->published_height = job->u.pool.valid_height;
        target->next_frame = 0;
        target->mailbox_pending = FALSE;
    }
    target->backing = job->u.pool.pixmaps[0];
    target->shrink_start = job->u.pool.shrink_start;
    target->width = job->u.pool.width;
    target->height = job->u.pool.height;
    target->window_width = job->u.pool.window_width;
    target->window_height = job->u.pool.window_height;
    target->depth = job->u.pool.depth;
    target->visual = job->u.pool.visual;
    target->quiescing = target->native_updates || target->deferred_update;
    if (!target->present_window)
    {
        if (!create_client_surface_present_window( target )) goto failed;
    }
    TRACE( "updated compositor target hwnd %p window %#lx size %ux%u depth %u visual %#lx\n",
           target->toplevel, target->window, target->window_width, target->window_height,
           target->depth, target->visual );
    /* Ensuring capacity or refreshing topology does not write an image.
     * Only a replacement pool or an actual GUI snapshot rotation introduces
     * a new checkpoint. Otherwise this would replace the latest completed
     * output with an older spare and corrupt the next incremental copy. */
    if (checkpoint) note_client_surface_compositor_snapshot( target, target->backing );
    return TRUE;

failed:
    if (created) free_client_surface_compositor_target( target );
    else target->quiescing = target->native_updates || target->deferred_update;
    return FALSE;
}

BOOL client_surface_output_checkpoint_scene_current( const struct client_surface_scene *expected )
{
    struct client_surface_scene current;

    return client_surface_capture_scene_state( expected->toplevel, &current ) &&
           current.toplevel == expected->toplevel && current.epoch == expected->epoch &&
           current.paint_serial == expected->paint_serial &&
           current.generation == expected->generation && current.mode == expected->mode;
}

static struct client_surface_compositor_frame *client_surface_output_checkpoint_frame(
    struct client_surface_compositor_target *target, Pixmap source, BOOL *busy )
{
    unsigned int i;

    *busy = FALSE;
    if (!target || target->backing != source) return NULL;
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
    {
        struct client_surface_compositor_frame *frame = &target->frames[i];

        if (frame->pixmap != source || !frame->image) continue;
        /* A partial assembly is not a completed checkpoint, even before its
         * next native request is queued. Admission never waits for it. */
        if (client_surface_cache_write_pending( frame->image ) ||
            (target->assembly_generation && target->assembly_frame == i) ||
            client_surface_frame_copy_pending( frame ))
        {
            *busy = TRUE;
            return NULL;
        }
        if (!frame->revision) return NULL;
        return frame;
    }
    return NULL;
}

void release_client_surface_seed( struct client_surface_output_allocation *allocation )
{
    struct client_surface_compositor_target *target =
        find_client_surface_compositor_target( allocation->release.toplevel );

    if (target && target->seed_serial == allocation->serial)
        target->seed_serial = 0;
}

static void client_surface_output_seed_complete( void *context, BOOL success )
{
    struct client_surface_output_allocation *allocation = context;
    BOOL abandoned;

    pthread_mutex_lock( &client_surface_compositor_mutex );
    abandoned = allocation->abandoned;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    if (success && !abandoned)
    {
        /* The content seed already includes committed SOURCEs and subsequent
         * GDI writes. An old OUTPUT intersection must not overwrite it. */
        client_surface_cache_copy_output( allocation->images[1], allocation->images[0],
            allocation->window_width, allocation->window_height,
            client_surface_output_allocation_complete, allocation );
        return;
    }
    client_surface_output_allocation_complete( allocation, success );
}

static void queue_client_surface_seed( struct client_surface_output_allocation *allocation,
                                       struct client_surface_compositor_job *job,
                                       struct client_surface_compositor_target *target )
{
    allocation->scene = job->u.pool.scene;
    allocation->window = job->u.pool.destination;
    allocation->window_width = job->u.pool.window_width;
    allocation->window_height = job->u.pool.window_height;
    allocation->source = job->u.pool.source;
    allocation->copy_width = job->u.pool.preserve_width;
    allocation->copy_height = job->u.pool.preserve_height;
    allocation->phase = CLIENT_SURFACE_OUTPUT_PAIR_CHECKPOINT;
    allocation->seed_target = !!target;
    if (target)
    {
        allocation->source_revision = target->revision;
        allocation->published = target->published;
        target->seed_serial = allocation->serial;
    }
    pthread_mutex_lock( &client_surface_compositor_mutex );
    allocation->pending = 1;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    TRACE_(csperf)( "ticks=%llu event=%s request=%p serial=%llu hwnd=%p window=%lx source=%lx first=%lx second=%lx width=%u height=%u preserve_width=%u preserve_height=%u\n",
                   client_surface_perf_time(), "output_pair_seed_admit",
                   allocation, (unsigned long long)allocation->serial, job->toplevel,
                   allocation->window, allocation->source, allocation->pixmaps[0], allocation->pixmaps[1],
                   allocation->window_width, allocation->window_height, allocation->copy_width, allocation->copy_height );
    list_add_tail( &client_surface_seed_requests, &allocation->seed_entry );
    ++client_surface_seed_request_count;
}

BOOL copy_client_surface_compositor_pool( struct client_surface_compositor_job *job )
{
    struct client_surface_output_allocation *allocation = job->u.pool.allocation;
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( job->toplevel );

    assert( allocation && allocation->phase == CLIENT_SURFACE_OUTPUT_PAIR_CREATE &&
            !allocation->pending && !allocation->failed );
    if (!allocation->window_owner || !job->u.pool.window_width || !job->u.pool.window_height ||
        job->u.pool.window_width > allocation->width || job->u.pool.window_height > allocation->height ||
        !client_surface_output_checkpoint_scene_current( &job->u.pool.scene ))
        goto stale;
    queue_client_surface_seed( allocation, job, target );
    return TRUE;
stale:
    job->u.pool.stale = TRUE;
    return FALSE;
}

static BOOL client_surface_seed_current( struct client_surface_output_allocation *allocation,
                                         struct client_surface_compositor_target *target )
{
    return (allocation->seed_target ? target && target->seed_serial == allocation->serial &&
            target->window == allocation->window && target->revision == allocation->source_revision &&
            target->published == allocation->published : !target) &&
           client_surface_output_checkpoint_scene_current( &allocation->scene );
}

static BOOL process_client_surface_seed_requests(void)
{
    struct client_surface_output_allocation *allocation;
    unsigned int budget = min( client_surface_seed_request_count, 64 );
    BOOL progressed = FALSE;

    while (budget-- && !list_empty( &client_surface_seed_requests ))
    {
        struct client_surface_compositor_target *target;
        BOOL abandoned;
        NTSTATUS status;
        Window window;

        allocation = LIST_ENTRY( list_head( &client_surface_seed_requests ),
                                 struct client_surface_output_allocation, seed_entry );
        list_remove( &allocation->seed_entry );
        --client_surface_seed_request_count;
        pthread_mutex_lock( &client_surface_compositor_mutex );
        abandoned = allocation->abandoned;
        pthread_mutex_unlock( &client_surface_compositor_mutex );
        target = find_client_surface_compositor_target( allocation->release.toplevel );
        if (abandoned || !client_surface_seed_current( allocation, target )) goto failed;
        status = x11drv_native_window_prepare_content( allocation->window_owner,
            allocation->window_width, allocation->window_height, allocation->depth, &allocation->content_epoch,
            wake_client_surface_compositor );
        if (status == STATUS_PENDING)
        {
            list_add_tail( &client_surface_seed_requests, &allocation->seed_entry );
            ++client_surface_seed_request_count;
            continue;
        }
        if (status && status != STATUS_NOT_SUPPORTED) goto failed;
        allocation->content_owned = !status;
        /* Without XComposite, use a checked read of the canonical content
         * Window. Its coverage receipt must succeed before adoption. */
        if (status == STATUS_NOT_SUPPORTED) allocation->content_epoch = 0;
        /* Seed capture owns a named backing through the checked copy.
         * Scene changes reject adoption without cancelling its native read. */
        window = x11drv_native_window_content_read_init( &allocation->seed_read, allocation->window_owner,
                                                        allocation->content_epoch );
        TRACE_(csperf)( "ticks=%llu event=%s request=%p serial=%llu hwnd=%p window=%lx content=%lx source=%lx source_image=%p first=%lx second=%lx width=%u height=%u preserve_width=%u preserve_height=%u\n",
                       client_surface_perf_time(), "output_pair_seed_submit",
                       allocation, (unsigned long long)allocation->serial,
                       allocation->release.toplevel, allocation->window, window, allocation->source, allocation->source_image,
                       allocation->pixmaps[0], allocation->pixmaps[1], allocation->window_width, allocation->window_height,
                       allocation->copy_width, allocation->copy_height );
        client_surface_cache_seed_window( allocation->images[0], &allocation->seed_read,
            allocation->window_width, allocation->window_height,
            client_surface_output_seed_complete, allocation );
        progressed = TRUE;
        continue;
failed:
        client_surface_output_allocation_complete( allocation, FALSE );
        progressed = TRUE;
    }
    return progressed;
}

static BOOL install_client_surface_output_checkpoint( struct client_surface_compositor_job *job )
{
    struct client_surface_output_allocation *allocation = job->u.pool.allocation;
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( job->toplevel );
    BOOL current;

    assert( allocation->phase == CLIENT_SURFACE_OUTPUT_PAIR_CHECKPOINT && !allocation->pending && !allocation->failed );
    /* Only the second copy completes a successful, un-abandoned seed. */
    current = !allocation->abandoned && client_surface_seed_current( allocation, target ) &&
              job->u.pool.source == allocation->source && job->u.pool.destination == allocation->window &&
              job->u.pool.width == allocation->width && job->u.pool.height == allocation->height &&
              job->u.pool.depth == allocation->depth &&
              job->u.pool.window_width == allocation->window_width &&
              job->u.pool.window_height == allocation->window_height &&
              client_surface_output_checkpoint_scene_current( &allocation->scene );
    TRACE_(csperf)( "ticks=%llu event=output_pair_copy_install request=%p serial=%llu hwnd=%p source=%lx current=%u\n",
                   client_surface_perf_time(), allocation, (unsigned long long)allocation->serial,
                   job->toplevel, allocation->source, current );
    if (!current)
    {
        job->u.pool.stale = TRUE;
        return FALSE;
    }
    release_client_surface_seed( allocation );
    memcpy( job->u.pool.pixmaps, allocation->pixmaps, sizeof(job->u.pool.pixmaps) );
    register_client_surface_output_allocation( allocation );
    return TRUE;
}

BOOL replace_client_surface_compositor_pool( struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( job->toplevel );
    struct client_surface_output_allocation *allocation = job->u.pool.allocation;
    BOOL success;

    /* Completed empty storage becomes an output pool only after checked
     * checkpoint copies and installation. The GUI keeps the old pair until
     * this job succeeds; failure leaves its pending publication intact. */
    if (!install_client_surface_output_checkpoint( job ))
    {
        free_client_surface_pending_allocation( job->u.pool.allocation );
        job->u.pool.allocation = NULL;
        goto failed;
    }
    job->u.pool.allocation = NULL; /* Registered pool owns the completed storage. */
    success = update_client_surface_compositor_target( job );
    release_client_surface_output_checkpoint( allocation );
    if (success) return TRUE;
    release_client_surface_output_allocation( job->u.pool.pixmaps );
    job->u.pool.pixmaps[0] = job->u.pool.pixmaps[1] = 0;
failed:
    if ((target = find_client_surface_compositor_target( job->toplevel )))
        target->quiescing = target->native_updates || target->deferred_update;
    return FALSE;
}

BOOL remove_client_surface_compositor_target( HWND toplevel )
{
    struct client_surface_compositor_target *target;
    unsigned int i;

    target = find_client_surface_compositor_target( toplevel );
    TRACE_(csperf)( "ticks=%llu event=target_remove hwnd=%p target=%p\n",
                   client_surface_perf_time(), toplevel, target );
    if (!target) return TRUE;
    drain_client_surface_compositor_target( target );
    free_client_surface_compositor_present_input( target );
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
    {
        if (i < 2) set_client_surface_compositor_pixmap( &target->frames[i], 0, NULL );
    }
    free_client_surface_compositor_mailbox( target );
    rb_remove( &client_surface_compositor_target_registry, &target->registry_entry );
    *target->prev = target->next;
    if (target->next) target->next->prev = target->prev;
    if (client_surface_compositor_next_target == target)
        client_surface_compositor_next_target = target->next ? target->next : client_surface_compositor_targets;
    --client_surface_compositor_target_count;
    ++client_surface_compositor_target_generation;
    free_client_surface_scene_plan( target );
    free_client_surface_compositor_target( target );
    return TRUE;
}

static BOOL get_client_surface_direct_scene( HWND toplevel, UINT64 epoch, struct client_surface_scene *scene )
{
    return client_surface_get_toplevel_scene( toplevel, scene ) && scene->direct_candidate &&
           scene->epoch == epoch && (!scene->generation || scene->generation == epoch);
}

BOOL client_surface_direct_plan_current( const struct client_surface_compositor_job *job,
                                                const struct client_surface_compositor_target *target )
{
    struct client_surface_scene current;

    return job->u.direct_plan.source && job->u.direct_plan.destination && (!target || target->window == job->u.direct_plan.destination) &&
           get_client_surface_direct_scene( job->toplevel, job->u.direct_plan.scene_epoch, &current );
}

BOOL install_client_surface_direct_plan( struct client_surface_compositor_job *job,
                                                unsigned int *budget, BOOL *done )
{
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( job->toplevel );
    struct client_surface_scene current;
    UINT64 scene_id = job->u.direct_plan.scene_epoch;
    BOOL accepted = FALSE, allocated = FALSE;

    if (job->scan.phase)
    {
        scene_id = job->scan.generation;
        if (!target || !get_client_surface_direct_scene( job->toplevel, scene_id, &current )) goto done;
        accepted = TRUE;
        if (job->scan.phase == 2) goto done;
        goto sweep;
    }
    /* The shared candidate is only an early rejection hint. The prepare and
     * select requests authenticate the sole selected identity, owner process
     * and exact scene before changing the plan. No geometry or clip from a
     * roster snapshot is consumed by native attachment. */
    if (!job->u.direct_plan.source || !job->u.direct_plan.destination ||
        !get_client_surface_direct_scene( job->toplevel, scene_id, &current ))
        goto done;
    /* A managed top-level may still be waiting for the WM to map it. A
     * DIRECT image presented into that unmapped hierarchy can be discarded
     * by the later map. Keep the first frame in the owner cache instead. */
    TRACE( "DIRECT native admission hwnd %p window %#lx queried %u map state %d error %d\n",
           job->toplevel, job->u.direct_plan.destination, job->u.direct_plan.query.success,
           job->u.direct_plan.query.map_state, job->u.direct_plan.query.error );
    if (!ReadAcquire( &job->u.direct_plan.query.complete ) || !job->u.direct_plan.query.success ||
        job->u.direct_plan.query.map_state != IsViewable) goto done;
    if (!current.generation)
    {
        UINT64 next_scene = 0;

        /* A retained owner plan supplies the native target; only the server's
         * successful ACK checkpoint can authorize skipping geometry prepare.
         * The submitting producer keeps this exact drawable alive throughout. */
        if (!target || !target->scene.valid || target->scene.strategy != OWNER_COMPOSITE ||
            target->scene.epoch != scene_id || target->window != job->u.direct_plan.destination) goto done;
        SERVER_START_REQ( prepare_client_surface_direct_plan )
        {
            req->handle = wine_server_user_handle( job->toplevel );
            req->scene_id = scene_id;
            req->surface = job->u.direct_plan.identity;
            req->previous_scene = 0;
            if (!wine_server_call( req )) next_scene = reply->scene_id;
        }
        SERVER_END_REQ;
        if (!next_scene) goto done;
        TRACE( "owner DIRECT strategy scene hwnd %p old %s new %s identity %s\n",
               job->toplevel, wine_dbgstr_longlong( scene_id ), wine_dbgstr_longlong( next_scene ),
               wine_dbgstr_longlong( job->u.direct_plan.identity ) );
        /* Read the new immutable scene rather than retagging the old one.
         * Failed admission leaves the channels and old plan intact; the
         * restart's ordinary owner wake can compose the new scene. */
        scene_id = next_scene;
        if (!get_client_surface_direct_scene( job->toplevel, scene_id, &current ) ||
            current.generation != scene_id) goto done;
    }
    if (!target)
    {
        if (!(target = alloc_client_surface_compositor_target( job->toplevel ))) goto done;
        target->window = job->u.direct_plan.destination;
        target->window_owner = x11drv_native_window_acquire( job->u.direct_plan.window_owner );
        allocated = TRUE;
    }
    if (target->window != job->u.direct_plan.destination) goto done;
    SERVER_START_REQ( select_client_surface_direct_plan )
    {
        req->handle = wine_server_user_handle( job->toplevel );
        req->scene_id = scene_id;
        req->surface = job->u.direct_plan.identity;
        if (!wine_server_call( req )) accepted = reply->accepted;
    }
    SERVER_END_REQ;
    if (!accepted) goto done;
    if (allocated) register_client_surface_compositor_target( target );
    /* Only authenticated admission may cancel the previous assembly. While
     * native reads and Present requests drained, the scheduler paused new
     * work without discarding a newer scene's assembly or mailbox. Keep its
     * output allocation until the host-success publication ACK. */
    quiesce_client_surface_compositor_target( target );
    target->scene.valid = FALSE;
    job->scan.phase = 1;
    job->scan.generation = scene_id;
sweep:
    if (!sweep_client_surface_compositor_handoffs( job, 0, budget ))
    {
        *done = FALSE;
        return FALSE;
    }
    update_client_surface_compositor_scene( target, scene_id );
    free_client_surface_scene_plan( target );
    target->scene = (struct client_surface_scene_plan){
        .strategy = DIRECT_ATTACH, .direct_identity = job->u.direct_plan.identity,
        .direct_drawable = job->u.direct_plan.source, .epoch = scene_id, .valid = TRUE,
        .direct_owner = x11drv_native_window_acquire( job->u.direct_plan.source_owner ),
    };
    hide_client_surface_present_window( target );
    job->scan.phase = 2;
    if (x11drv_native_window_content_status( target->window_owner ) == STATUS_PENDING) *done = FALSE;
    update_client_surface_notification_plan( target );
    SetRectEmpty( &target->restore_rect );
    TRACE( "owner DIRECT_ATTACH hwnd %p scene %s identity %s drawable %#lx\n",
           target->toplevel, wine_dbgstr_longlong( scene_id ), wine_dbgstr_longlong( job->u.direct_plan.identity ), job->u.direct_plan.source );
done:
    if (allocated && !accepted) free_client_surface_compositor_target( target );
    return accepted;
}

BOOL renew_client_surface_direct_plan( const struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( job->toplevel );
    struct client_surface_scene current;
    const struct client_surface_window_query *query = job->u.direct_renew.query;
    RECT rect = {job->u.direct_renew.source_x, job->u.direct_renew.source_y,
                 job->u.direct_renew.source_x + job->u.direct_renew.width,
                 job->u.direct_renew.source_y + job->u.direct_renew.height};
    unsigned int i;
    UINT64 scene_id = 0;

    /* Only a previously admitted attachment with its output pool already
     * retired can renew without preserving a composition checkpoint. */
    if (!target || !target->scene.valid || target->scene.strategy != DIRECT_ATTACH ||
        !target->scene.direct_drawable || target->window != job->u.direct_renew.destination ||
        target->native_updates || target->deferred_update || target->seed_serial) return FALSE;
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        if (target->frames[i].pixmap) return FALSE;
    client_surface_get_toplevel_scene( job->toplevel, &current );
    if (current.valid || !current.direct_candidate || current.generation ||
        current.epoch != job->u.direct_renew.scene_epoch || (current.epoch & 1)) return FALSE;

    /* The native worker checked both retained Windows. Accept only the same
     * attachment and requested geometry, never a newer plan at a reused XID. */
    if (!query || !ReadAcquire( &query->complete ) || !query->success || !query->direct_checked ||
        query->window != target->window || query->child_owner != target->scene.direct_owner ||
        query->child != target->scene.direct_drawable || query->direct_epoch != target->scene.epoch ||
        query->direct_identity != target->scene.direct_identity ||
        query->direct_width != job->u.direct_renew.window_width ||
        query->direct_height != job->u.direct_renew.window_height || !EqualRect( &query->direct_rect, &rect )) return FALSE;

    SERVER_START_REQ( prepare_client_surface_direct_plan )
    {
        req->handle = wine_server_user_handle( job->toplevel );
        req->scene_id = job->u.direct_renew.scene_epoch;
        req->surface = target->scene.direct_identity;
        req->previous_scene = target->scene.epoch;
        if (!wine_server_call( req )) scene_id = reply->scene_id;
    }
    SERVER_END_REQ;
    if (!scene_id) return FALSE;
    target->scene.epoch = scene_id;
    update_client_surface_notification_plan( target );
    target->window_width = job->u.direct_renew.window_width;
    target->window_height = job->u.direct_renew.window_height;
    SetRectEmpty( &target->restore_rect );
    TRACE( "owner DIRECT_ATTACH hwnd %p scene %s identity %s drawable %#lx renewed=1 size=%ux%u\n",
           target->toplevel, wine_dbgstr_longlong( scene_id ),
           wine_dbgstr_longlong( target->scene.direct_identity ), target->scene.direct_drawable,
           target->window_width, target->window_height );
    return TRUE;
}

BOOL complete_client_surface_direct_plan( const struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( job->toplevel );
    BOOL accepted = FALSE;

    if (!target || target->notifications != job->notifications || !target->scene.valid || target->scene.strategy != DIRECT_ATTACH ||
        target->scene.epoch != job->u.direct_complete.scene_epoch || target->scene.direct_identity != job->u.direct_complete.identity ||
        target->scene.direct_drawable != job->u.direct_complete.source) return FALSE;
    /* No native pointer is retained or dereferenced by this queued proof.
     * The producer validated its native epoch before enqueue; destruction or
     * a new scene invalidates server admission for that unique surface ID. */
    SERVER_START_REQ( complete_client_surface_direct_plan )
    {
        req->handle = wine_server_user_handle( job->toplevel );
        req->scene_id = job->u.direct_complete.scene_epoch;
        req->surface = job->u.direct_complete.identity;
        if (!wine_server_call( req )) accepted = reply->accepted;
    }
    SERVER_END_REQ;
    TRACE( "owner DIRECT_ATTACH host complete hwnd %p scene %s identity %s target %s accepted %u\n",
           job->toplevel, wine_dbgstr_longlong( job->u.direct_complete.scene_epoch ), wine_dbgstr_longlong( job->u.direct_complete.identity ),
           wine_dbgstr_longlong( job->u.direct_complete.native_epoch ), accepted );
    return accepted && publish_client_surface_handoff_generation( job->toplevel,
                                                                 job->u.direct_complete.scene_epoch, job->u.direct_complete.scene_epoch, TRUE );
}

static BOOL client_surface_compositor_restore_ready( const struct client_surface_compositor_target *target )
{
    struct client_surface_scene scene;
    unsigned int i;

    /* Expose is an obligation on this Window, even when its owned delivery
     * races a scene transaction. Keep it until the new plan can restore it. */
    if (target->quiescing || !target->scene.valid || target->scene.strategy != OWNER_COMPOSITE ||
        !client_surface_get_toplevel_scene( target->toplevel, &scene ) ||
        !target->published || target->published_width < target->window_width ||
        target->published_height < target->window_height) return FALSE;
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        if (target->frames[i].serial &&
            (target->frames[i].request_pending || !target->frames[i].complete ||
             (target->frames[i].pixmap == target->published && !target->frames[i].idle))) return FALSE;
    return TRUE;
}

static BOOL restore_client_surface_compositor_pixels( struct client_surface_compositor_target *target )
{
    RECT rect = target->restore_rect;

    if (IsRectEmpty( &rect )) return TRUE;
    TRACE( "restoring target %p window %#lx from published pixmap %#lx rect %s\n",
           target->toplevel, target->window, target->published, wine_dbgstr_rect( &rect ) );
    {
        unsigned int i;

        /* Reuse the published frame's admitted request and lifetime. No
         * actor-side XSync, and later publications join the same Window FIFO. */
        for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
            if (target->frames[i].pixmap == target->published)
            {
                if (!submit_client_surface_present( target, &target->frames[i], 0, 0, &rect, NULL, NULL, NULL )) return FALSE;
                SetRectEmpty( &target->restore_rect );
                return TRUE;
            }
        return FALSE;
    }
}

static BOOL process_client_surface_compositor_restore( struct client_surface_compositor_target *target )
{
    if (IsRectEmpty( &target->restore_rect ) || !client_surface_compositor_restore_ready( target )) return FALSE;
    if (!restore_client_surface_compositor_pixels( target ))
    {
        NtUserPostMessage( target->toplevel, WM_WINE_UPDATEWINDOWSTATE,
                           WINE_UPDATE_CLIENT_SURFACE_HANDOFFS, 0 );
        return FALSE;
    }
    return TRUE;
}

static void update_client_surface_compositor_timeout( struct client_surface_compositor_scan *scan,
                                                      DWORD now, unsigned int remaining )
{
    DWORD elapsed = now - scan->timeout_start;

    if (scan->timeout >= 0)
        scan->timeout = elapsed >= scan->timeout ? 0 : scan->timeout - elapsed;
    scan->timeout_start = now;
    if (scan->timeout < 0 || scan->timeout > remaining) scan->timeout = remaining;
}

BOOL notify_client_surface_output_allocations( struct client_surface_compositor_scan *scan )
{
    struct client_surface_output_allocation *allocation;
    unsigned int budget;
    BOOL posted, pending, progressed = FALSE;

    pthread_mutex_lock( &client_surface_compositor_mutex );
    budget = min( client_surface_output_notification_count, 64 );
    while (budget-- && !list_empty( &client_surface_output_notifications ))
    {
        allocation = LIST_ENTRY( list_head( &client_surface_output_notifications ),
                                 struct client_surface_output_allocation, notification_entry );
        /* Queue the scalar server message and publish that fact atomically
         * with respect to its GUI receiver and cancellation. No native call
         * or window-data lookup runs while this metadata lock is held. */
        posted = TRUE;
        if (client_surface_output_waits_scene( allocation ))
        {
            struct client_surface_scene scene;

            posted = client_surface_capture_scene_state( allocation->release.toplevel, &scene );
        }
        if (allocation->phase == CLIENT_SURFACE_OUTPUT_WINDOW_COPY_WAIT)
        {
            struct client_surface_compositor_target *target =
                find_client_surface_compositor_target( allocation->release.toplevel );

            /* A gone/replaced source also wakes the GUI to discard the old
             * request. A live writer does not generate periodic GUI retries. */
            if (target && target->window == allocation->window && target->backing == allocation->source)
            {
                BOOL busy;

                client_surface_output_checkpoint_frame( target, allocation->source, &busy );
                posted = !busy;
            }
        }
        if (posted)
            posted = NtUserPostMessage( allocation->release.toplevel, WM_X11DRV_CLIENT_SURFACE_POOL,
                                        (UINT)allocation->serial, (UINT)(allocation->serial >> 32) );
        list_remove( &allocation->notification_entry );
        if (posted)
        {
            list_init( &allocation->notification_entry );
            --client_surface_output_notification_count;
        }
        else list_add_tail( &client_surface_output_notifications, &allocation->notification_entry );
        progressed |= posted;
    }
    pending = !!client_surface_output_notification_count;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    if (pending) update_client_surface_compositor_timeout( scan, NtGetTickCount(), 100 );
    return progressed;
}

BOOL process_client_surface_compositor_targets( struct client_surface_compositor_scan *scan )
{
    struct client_surface_compositor_target *target;
    unsigned int inspected = 0, budget = CLIENT_SURFACE_COPY_BATCH_SIZE;
    BOOL progressed = FALSE;

    /* Insertion/removal changes the set to recheck, but never resets fair
     * traversal to the list head. Removal repairs the retained cursor before
     * freeing a target. A complete stable round is required before poll(). */
    if (scan->generation != client_surface_compositor_target_generation)
    {
        init_client_surface_output_scan( scan );
        scan->timeout = -1;
    }
    while (scan->remaining && inspected < 64 && budget)
    {
        struct client_surface_compositor_job *job;
        DWORD now, elapsed;

        target = client_surface_compositor_next_target;
        assert( target );
        client_surface_compositor_next_target = target->next ? target->next : client_surface_compositor_targets;
        --scan->remaining;
        ++inspected;
        if (target->deferred_update && target->update_phase == CLIENT_SURFACE_UPDATE_UNPOSTED)
        {
            quiesce_client_surface_compositor_target( target );
            if (client_surface_compositor_update_ready( target, NULL ) != STATUS_PENDING)
            {
                /* Keep this exact target quiescent until the GUI consumes
                 * the token; a delayed notification cannot resume another. */
                if (NtUserPostMessage( target->toplevel, WM_X11DRV_CLIENT_SURFACE_UPDATE,
                                       (UINT)target->deferred_update, (UINT)(target->deferred_update >> 32) ))
                {
                    target->update_phase = CLIENT_SURFACE_UPDATE_POSTED;
                    progressed = TRUE;
                }
                else
                    WARN( "failed to notify deferred native update for %p\n", target->toplevel );
            }
        }
        process_client_surface_present_events( target );
        progressed |= process_client_surface_native_present( target );
        progressed |= process_client_surface_compositor_restore( target );
        progressed |= replay_client_surface_scene_sources( target, &budget );
        flush_client_surface_compositor_mailbox( target );
        now = NtGetTickCount();
        if (target->shrink_start && !target->native_updates && !target->assembly_generation)
        {
            elapsed = now - target->shrink_start;
            if (elapsed >= 2000)
            {
                target->shrink_start = 0;
                NtUserPostMessage( target->toplevel, WM_WINE_UPDATEWINDOWSTATE,
                                   WINE_UPDATE_CLIENT_SURFACE_HANDOFFS, 0 );
                progressed = TRUE;
            }
            else update_client_surface_compositor_timeout( scan, now, 2000 - elapsed );
        }
        /* An in-flight Present owns its target until its FIFO head retires.
         * Collect its deadline here instead of traversing all parked queues. */
        job = target->notifications->queue->head;
        if (job && job->op == CLIENT_SURFACE_COMPOSITOR_PRESENT && job->u.present.started)
        {
            elapsed = now - job->u.present.start;
            if (job->u.present.done || elapsed >= 5000)
            {
                ready_client_surface_compositor_queue( target->notifications->queue );
                progressed = TRUE;
            }
            else update_client_surface_compositor_timeout( scan, now, 5000 - elapsed );
        }
    }
    TRACE_(csperf)( "ticks=%llu event=compositor_target_scan targets=%u inspected=%u replayed=%u remaining=%u progressed=%u generation=%s\n",
                   client_surface_perf_time(), client_surface_compositor_target_count, inspected,
                   CLIENT_SURFACE_COPY_BATCH_SIZE - budget, scan->remaining, progressed,
                   wine_dbgstr_longlong( scan->generation ) );
    return progressed;
}

NTSTATUS client_surface_output_status( const struct client_surface_output_allocation *allocation )
{
    NTSTATUS status;

    pthread_mutex_lock( &client_surface_compositor_mutex );
    status = allocation->pending || !list_empty( &allocation->notification_entry ) ? STATUS_PENDING :
             allocation->failed ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    return status;
}

void defer_client_surface_output_scene( struct client_surface_output_allocation *allocation )
{
    allocation->phase = CLIENT_SURFACE_OUTPUT_PAIR_SCENE_WAIT;
    pthread_mutex_lock( &client_surface_compositor_mutex );
    list_add_tail( &client_surface_output_notifications, &allocation->notification_entry );
    ++client_surface_output_notification_count;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    wake_client_surface_compositor();
}

void cancel_client_surface_output_request( struct client_surface_output_allocation *allocation )
{
    BOOL pending;

    if (!allocation) return;
    pthread_mutex_lock( &client_surface_compositor_mutex );
    allocation->abandoned = TRUE;
    pending = !!allocation->pending;
    if (!list_empty( &allocation->notification_entry ))
    {
        list_remove( &allocation->notification_entry );
        list_init( &allocation->notification_entry );
        --client_surface_output_notification_count;
    }
    TRACE_(csperf)( "ticks=%llu event=%s request=%p serial=%llu pending=%u checkpoint=%u waiting=%u\n",
                   client_surface_perf_time(), client_surface_output_is_window( allocation ) ? "geometry_query_cancel" : "output_pair_create_cancel",
                   allocation, (unsigned long long)allocation->serial, allocation->pending,
                   allocation->phase == CLIENT_SURFACE_OUTPUT_PAIR_CHECKPOINT,
                   allocation->phase == CLIENT_SURFACE_OUTPUT_WINDOW_COPY_WAIT );
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    if (!pending) free_client_surface_pending_allocation( allocation );
}

/* The existing geometry observation owns this continuation and its release
 * capacity. Admission pins the exact completed input, without waiting for an
 * older native publication or retaining any GUI window-data pointer. */
BOOL admit_client_surface_publication( struct client_surface_compositor_job *job )
{
    struct client_surface_output_allocation *allocation = job->u.present.allocation;
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( job->toplevel );
    struct client_surface_compositor_frame *frame;
    BOOL busy;

    if (!target || target->window_owner != allocation->window_owner || target->window != allocation->window ||
        !client_surface_output_checkpoint_scene_current( &allocation->scene )) return FALSE;
    assert( (allocation->phase == CLIENT_SURFACE_OUTPUT_WINDOW_QUERY ||
             allocation->phase == CLIENT_SURFACE_OUTPUT_WINDOW_COPY_WAIT) && !allocation->pending &&
            !allocation->source_image && list_empty( &allocation->notification_entry ) );
    allocation->source = job->u.present.source;
    frame = client_surface_output_checkpoint_frame( target, allocation->source, &busy );
    if (busy)
    {
        /* A completed revision can already be undergoing the next assembly.
         * Reuse the checkpoint wake instead of pinning that mutable input or
         * making the GUI wait behind its native writer. */
        pthread_mutex_lock( &client_surface_compositor_mutex );
        allocation->phase = CLIENT_SURFACE_OUTPUT_WINDOW_COPY_WAIT;
        list_add_tail( &client_surface_output_notifications, &allocation->notification_entry );
        ++client_surface_output_notification_count;
        pthread_mutex_unlock( &client_surface_compositor_mutex );
        return TRUE;
    }
    if (!frame) return FALSE;
    allocation->source_image = client_surface_cache_acquire( client_surface_compositor_frame_image( frame ) );
    allocation->release.op = CLIENT_SURFACE_COMPOSITOR_PRESENT;
    allocation->release.u.present = job->u.present;
    allocation->release.async = TRUE;
    pthread_mutex_lock( &client_surface_compositor_mutex );
    allocation->phase = CLIENT_SURFACE_OUTPUT_WINDOW_PRESENT;
    allocation->pending = 1;
    enqueue_client_surface_compositor_job( &allocation->release );
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    TRACE_(csperf)( "ticks=%llu event=output_publication_admit hwnd=%p serial=%llu source=%lx\n",
                   client_surface_perf_time(), job->toplevel,
                   (unsigned long long)allocation->serial, allocation->source );
    return TRUE;
}

/* Return the independently owned pair through its admitted release node. */
void retire_client_surface_output( Pixmap first, Pixmap second )
{
    struct client_surface_output_allocation *allocation;

    if (!first && !second) return;
    pthread_mutex_lock( &client_surface_compositor_mutex );
    for (allocation = client_surface_output_allocations; allocation; allocation = allocation->next)
        if ((allocation->pixmaps[0] == first && allocation->pixmaps[1] == second) ||
            (allocation->pixmaps[0] == second && allocation->pixmaps[1] == first)) break;
    assert( allocation && !allocation->release.async );
    allocation->release.op = CLIENT_SURFACE_COMPOSITOR_FREE_POOL;
    allocation->release.u.retired_pixmaps[0] = first;
    allocation->release.u.retired_pixmaps[1] = second;
    allocation->release.async = TRUE;
    enqueue_client_surface_compositor_job( &allocation->release );
    pthread_mutex_unlock( &client_surface_compositor_mutex );
}

void init_client_surface_output_scan( struct client_surface_compositor_scan *scan )
{
    scan->generation = client_surface_compositor_target_generation;
    scan->target_count = client_surface_compositor_target_count;
    scan->remaining = scan->target_count;
}

unsigned int get_client_surface_output_waiters( const struct client_surface_compositor_scan *scan,
                                               struct pollfd *waiters, unsigned int capacity )
{
    struct client_surface_compositor_target *target;
    unsigned int count = 0;

    assert( !scan->remaining && scan->generation == client_surface_compositor_target_generation );
    for (target = client_surface_compositor_targets; target; target = target->next)
        if (target->present_window)
        {
            int fd = x11drv_native_window_expose_fd( target->window_owner );

            assert( count < capacity );
            if (fd != -1) waiters[count++] = (struct pollfd){fd, POLLIN, 0};
        }
    return count;
}

BOOL process_client_surface_output_seeds( struct client_surface_compositor_scan *scan )
{
    BOOL progressed = process_client_surface_seed_requests();

    if (!list_empty( &client_surface_seed_requests ))
        update_client_surface_compositor_timeout( scan, NtGetTickCount(), 100 );
    return progressed;
}
