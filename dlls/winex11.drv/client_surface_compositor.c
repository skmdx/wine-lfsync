/*
 * Compositor request admission, scheduling and notifications
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
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>

#ifdef __linux__
#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/eventfd.h>
#endif

#include "ntstatus.h"
#include "x11drv.h"
#include "client_surface_cache.h"
#include "client_surface_compositor_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);
WINE_DECLARE_DEBUG_CHANNEL(csperf);

pthread_mutex_t client_surface_compositor_mutex = PTHREAD_MUTEX_INITIALIZER;
static BOOL client_surface_compositor_started;
static int client_surface_compositor_notify[2] = {-1, -1};

#define CLIENT_SURFACE_COMPOSITOR_MAX_QUEUES 2048
static const struct { unsigned int count; SIZE_T bytes; } client_surface_compositor_limits[] =
{
    [CLIENT_SURFACE_COMPOSITOR_REQUEST_CAPACITY] = {1024, 256 * 1024},
    [CLIENT_SURFACE_COMPOSITOR_QUEUE_CAPACITY] = {CLIENT_SURFACE_COMPOSITOR_MAX_QUEUES, 1024 * 1024},
    [CLIENT_SURFACE_COMPOSITOR_RELEASE_CAPACITY] = {4096, 1024 * 1024},
};
#define CLIENT_SURFACE_COMPOSITOR_REQUESTS_PER_TARGET 64
static struct { unsigned int count; SIZE_T bytes; } client_surface_compositor_capacity[CLIENT_SURFACE_COMPOSITOR_CAPACITY_COUNT];

static struct client_surface_compositor_job *client_surface_compositor_head;
static struct client_surface_compositor_job **client_surface_compositor_tail =
    &client_surface_compositor_head;
static int compare_client_surface_compositor_queue( const void *key, const struct rb_entry *entry )
{
    const struct client_surface_compositor_queue *queue =
        CONTAINING_RECORD( entry, const struct client_surface_compositor_queue, registry_entry );
    ULONG_PTR a = (ULONG_PTR)key, b = (ULONG_PTR)queue->toplevel;

    return (a > b) - (a < b);
}

static struct rb_tree client_surface_compositor_queues = {compare_client_surface_compositor_queue};
static struct list client_surface_compositor_ready = LIST_INIT( client_surface_compositor_ready );
static struct list client_surface_compositor_parked = LIST_INIT( client_surface_compositor_parked );
static UINT64 client_surface_compositor_wake_serial;
static UINT64 client_surface_compositor_sequence;
static LONGLONG client_surface_compositor_domain;
static UINT64 client_surface_native_update_serial;

static struct client_surface_owner_notifications *client_surface_owner_notifications;

void wake_client_surface_compositor_queues(void)
{
    ++client_surface_compositor_wake_serial;
    /* Native completion or a timer can make parked heads runnable. Move the
     * list in constant time; dispatch still inspects at most 64 queue heads. */
    list_move_tail( &client_surface_compositor_ready, &client_surface_compositor_parked );
}

/* Capacity is reserved before allocation and returned after the real free.
 * Reserved release nodes compete only when their resources are constructed,
 * never when those resources need to retire. Caller holds compositor_mutex. */
static BOOL reserve_client_surface_compositor_capacity( enum client_surface_compositor_capacity_kind kind,
                                                        unsigned int count, SIZE_T bytes,
                                                        struct client_surface_compositor_queue *queue )
{
    BOOL accepted = count <= client_surface_compositor_limits[kind].count - client_surface_compositor_capacity[kind].count &&
                    bytes <= client_surface_compositor_limits[kind].bytes - client_surface_compositor_capacity[kind].bytes;

    if (queue) accepted = accepted && queue->requests < CLIENT_SURFACE_COMPOSITOR_REQUESTS_PER_TARGET;
    if (accepted)
    {
        client_surface_compositor_capacity[kind].count += count;
        client_surface_compositor_capacity[kind].bytes += bytes;
        if (queue) ++queue->requests;
    }
    TRACE_(csperf)( "ticks=%llu event=compositor_admission kind=%u count=%u bytes=%zu accepted=%u "
                   "used=%u used_bytes=%zu hwnd=%p target_requests=%u\n", client_surface_perf_time(), kind,
                   count, (size_t)bytes, accepted, client_surface_compositor_capacity[kind].count,
                   (size_t)client_surface_compositor_capacity[kind].bytes, queue ? queue->toplevel : NULL,
                   queue ? queue->requests : 0 );
    return accepted;
}

void release_client_surface_compositor_capacity( enum client_surface_compositor_capacity_kind kind,
                                                        unsigned int count, SIZE_T bytes,
                                                        struct client_surface_compositor_queue *queue )
{
    pthread_mutex_lock( &client_surface_compositor_mutex );
    assert( client_surface_compositor_capacity[kind].count >= count && client_surface_compositor_capacity[kind].bytes >= bytes );
    client_surface_compositor_capacity[kind].count -= count;
    client_surface_compositor_capacity[kind].bytes -= bytes;
    if (queue)
    {
        assert( queue->requests );
        --queue->requests;
    }
    TRACE_(csperf)( "ticks=%llu event=compositor_capacity_return kind=%u count=%u bytes=%zu used=%u used_bytes=%zu "
                   "hwnd=%p target_requests=%u\n", client_surface_perf_time(), kind, count, (size_t)bytes,
                   client_surface_compositor_capacity[kind].count, (size_t)client_surface_compositor_capacity[kind].bytes,
                   queue ? queue->toplevel : NULL, queue ? queue->requests : 0 );
    pthread_mutex_unlock( &client_surface_compositor_mutex );
}

BOOL x11drv_reserve_release_capacity( unsigned int count, SIZE_T bytes )
{
    BOOL ret;

    pthread_mutex_lock( &client_surface_compositor_mutex );
    ret = reserve_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_RELEASE_CAPACITY, count, bytes, NULL );
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    return ret;
}

void x11drv_return_release_capacity( unsigned int count, SIZE_T bytes )
{
    release_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_RELEASE_CAPACITY, count, bytes, NULL );
}

void free_client_surface_compositor_release( struct client_surface_memory_scope *memory, void *data,
                                                    unsigned int count, SIZE_T bytes )
{
    client_surface_free_owned_metadata( memory, data, bytes );
    x11drv_return_release_capacity( count, bytes );
}

/* Routing storage is admitted before the actor starts. Its account uses the
 * same stable native domain as the actor's images, never the GUI thread's
 * execution domain. Each allocation retains it through actual retirement. */
void *alloc_client_surface_compositor_metadata( HWND toplevel, SIZE_T size,
                                                        struct client_surface_memory_scope *memory )
{
    UINT64 domain;
    void *data;

    pthread_mutex_lock( &client_surface_compositor_mutex );
    if (!client_surface_compositor_domain)
        client_surface_compositor_domain = client_surface_allocate_completion_domains( 1 );
    domain = client_surface_compositor_domain;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    if (!domain || !client_surface_memory_scope_init( memory, toplevel, domain )) return NULL;
    if (!(data = client_surface_alloc_scoped_metadata( memory, 1, size )))
        client_surface_memory_scope_destroy( memory );
    return data;
}

static void free_client_surface_compositor_queue( struct client_surface_compositor_queue *queue )
{
    pthread_cond_destroy( &queue->barrier_available );
    client_surface_free_owned_metadata( &queue->memory, queue, sizeof(*queue) );
    release_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_QUEUE_CAPACITY, 1, sizeof(*queue), NULL );
}

struct client_surface_compositor_queue *get_client_surface_compositor_queue( HWND toplevel )
{
    struct client_surface_compositor_queue *queue, *created = NULL;
    struct client_surface_memory_scope memory = {0};
    struct rb_entry *entry;

    assert( toplevel );
    pthread_mutex_lock( &client_surface_compositor_mutex );
    for (;;)
    {
        if ((entry = rb_get( &client_surface_compositor_queues, toplevel )))
        {
            queue = CONTAINING_RECORD( entry, struct client_surface_compositor_queue, registry_entry );
            ++queue->refs;
            break;
        }
        if (created)
        {
            queue = created;
            created = NULL;
            rb_put( &client_surface_compositor_queues, toplevel, &queue->registry_entry );
            break;
        }
        if (!reserve_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_QUEUE_CAPACITY, 1, sizeof(*created), NULL ))
        {
            pthread_mutex_unlock( &client_surface_compositor_mutex );
            return NULL;
        }
        pthread_mutex_unlock( &client_surface_compositor_mutex );
        if (!(created = alloc_client_surface_compositor_metadata( toplevel, sizeof(*created), &memory )))
        {
            release_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_QUEUE_CAPACITY, 1, sizeof(*created), NULL );
            return NULL;
        }
        created->memory = memory;
        if (pthread_cond_init( &created->barrier_available, NULL )) goto failed;
        created->toplevel = toplevel;
        created->refs = 1;
        created->tail = &created->head;
        created->control_tail = &created->control_head;
        list_init( &created->entry );
        pthread_mutex_lock( &client_surface_compositor_mutex );
    }
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    if (created) free_client_surface_compositor_queue( created );
    return queue;

failed:
    client_surface_free_owned_metadata( &created->memory, created, sizeof(*created) );
    release_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_QUEUE_CAPACITY, 1, sizeof(*created), NULL );
    return NULL;
}

void release_client_surface_compositor_queue( struct client_surface_compositor_queue *queue )
{
    BOOL unused;

    pthread_mutex_lock( &client_surface_compositor_mutex );
    assert( queue->refs );
    if ((unused = !--queue->refs))
    {
        assert( !queue->head && !queue->control_head && !queue->incoming && !queue->requests && !queue->barrier_in_use &&
                !queue->target && !queue->remove_queued && list_empty( &queue->entry ) );
        rb_remove( &client_surface_compositor_queues, &queue->registry_entry );
    }
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    if (unused) free_client_surface_compositor_queue( queue );
}

static void client_surface_handoff_futex_wake( LONG *address )
{
#ifdef __linux__
    syscall( SYS_futex, address, FUTEX_WAKE, INT_MAX, NULL, NULL, 0 );
#else
    (void)address;
#endif
}

void wake_client_surface_compositor(void)
{
    UINT64 value = 1;
    int ret;

    do
#ifdef __linux__
        ret = write( client_surface_compositor_notify[1], &value, sizeof(value) );
#else
        ret = send( client_surface_compositor_notify[1], &value, sizeof(value), 0 );
#endif
    while (ret < 0 && errno == EINTR);
    TRACE_(csperf)( "ticks=%llu event=actor_signal fd=%d result=%d\n",
                   client_surface_perf_time(), client_surface_compositor_notify[1], ret );
}

void client_surface_handoff_wake_release( struct client_surface_handoff_shared *shared )
{
    if (!__atomic_exchange_n( &shared->release_parked, 0, __ATOMIC_ACQ_REL )) return;
    __atomic_add_fetch( &shared->release_sequence, 1, __ATOMIC_RELEASE );
    client_surface_handoff_futex_wake( &shared->release_sequence );
    TRACE_(csperf)( "ticks=%llu event=release_signal mapping=%s\n",
                   client_surface_perf_time(), wine_dbgstr_longlong( shared->mapping_id ) );
}

void drain_client_surface_notification( int fd )
{
    UINT64 value;
    int ret;

    do ret = read( fd, &value, sizeof(value) ); while (ret > 0 || (ret < 0 && errno == EINTR));
}

static void arm_client_surface_compositor_work(void)
{
    drain_client_surface_notification( client_surface_compositor_notify[0] );
    /* A native callback can complete a parked head while the actor is busy.
     * Draining its wake must be followed by rechecking that head, even when
     * no poll was needed to resume the actor. Dispatch remains sliced. */
    wake_client_surface_compositor_queues();
    arm_client_surface_sources();
}

static void wait_client_surface_compositor_work( const struct client_surface_compositor_scan *scan )
{
    struct pollfd waiters[CLIENT_SURFACE_HANDOFF_MAX_POOLS_PER_CONSUMER + CLIENT_SURFACE_COMPOSITOR_MAX_QUEUES + 1];
    unsigned int count = 0;
    DWORD elapsed;
    int ret, timeout = scan->timeout;

    /* A quiet traversal consumed owned Expose rectangles and scheduled any
     * native readers. Their completion wakes us even for Xlib-buffered input;
     * the actor only polls file descriptors, never Xlib state. */
    pthread_mutex_lock( &client_surface_compositor_mutex );
    if (client_surface_compositor_head)
    {
        pthread_mutex_unlock( &client_surface_compositor_mutex );
        return;
    }
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    waiters[count++] = (struct pollfd){client_surface_compositor_notify[0], POLLIN, 0};
    count += get_client_surface_output_waiters( scan, waiters + count, ARRAY_SIZE(waiters) - count );
    count += get_client_surface_source_waiters( scan, waiters + count, ARRAY_SIZE(waiters) - count );
    /* The earliest deadline was collected during the traversal. Time spent
     * in later slices must not extend it, including across tick wraparound. */
    do
    {
        if ((timeout = scan->timeout) >= 0)
        {
            elapsed = NtGetTickCount() - scan->timeout_start;
            timeout = elapsed >= timeout ? 0 : timeout - elapsed;
        }
        TRACE_(csperf)( "ticks=%llu event=compositor_wait targets=%u remaining=%u timeout=%d generation=%s pools=%u handoff_remaining=%u pool_generation=%s\n",
                       client_surface_perf_time(), scan->target_count, scan->remaining, timeout,
                       wine_dbgstr_longlong( scan->generation ), scan->pool_count,
                       scan->handoff_remaining, wine_dbgstr_longlong( scan->pool_generation ) );
        ret = poll( waiters, count, timeout );
    } while (ret < 0 && errno == EINTR);
    wake_client_surface_compositor_queues();
    if (ret < 0) WARN( "client-surface compositor poll failed, error %d\n", errno );
}

void quiesce_client_surface_compositor_target( struct client_surface_compositor_target *target )
{
    target->quiescing = TRUE;
    detach_client_surface_output_transform( target );
    finish_client_surface_compositor_assembly( target, TRUE );
    if (target->mailbox_pending && target->mailbox_publish_generation)
        publish_client_surface_handoff_generation( target->toplevel,
            target->mailbox_publish_generation, target->mailbox_publish_epoch, FALSE );
    target->mailbox_pending = FALSE;
    target->mailbox_publish_generation = target->mailbox_publish_epoch = 0;
}

NTSTATUS client_surface_compositor_update_ready( struct client_surface_compositor_target *target,
                                                        const struct client_surface_compositor_job *until )
{
    const struct client_surface_compositor_queue *queue = target->notifications->queue;
    unsigned int i;

    if (target->native_updates || x11drv_native_window_content_status( target->window_owner ) == STATUS_PENDING) return STATUS_PENDING;
    /* A state notification can also follow a topology change. Do not wait
     * for unissued output from the server's invalidated scene before
     * allowing the GUI to adopt its replacement. Executing requests retain
     * their native completion boundary. */
    if (count_client_surface_compositor_frames( target ) &&
        !client_surface_scene_snapshot_current( target->toplevel, target->scene.epoch ))
    {
        client_surface_cancel_native_presents( &target->native_presents );
        process_client_surface_native_present( target );
    }
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        if (target->frames[i].serial) return STATUS_PENDING;
    if (!until)
    {
        BOOL incoming;

        pthread_mutex_lock( &client_surface_compositor_mutex );
        incoming = !!queue->incoming;
        pthread_mutex_unlock( &client_surface_compositor_mutex );
        if (incoming) return STATUS_PENDING;
    }
    if ((queue->head && (!until || queue->head->sequence < until->sequence)) ||
        (queue->control_head && (!until || queue->control_head->sequence < until->sequence))) return STATUS_PENDING;
    return target->preserve_content ? x11drv_native_window_preserve_content( target->window_owner ) : STATUS_SUCCESS;
}

static BOOL execute_client_surface_compositor_job( struct client_surface_compositor_job *job )
{
    if (job->op == CLIENT_SURFACE_COMPOSITOR_ADMIT_PRESENT)
        return admit_client_surface_publication( job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_QUERY_WINDOW)
        return create_client_surface_window_query( job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_CREATE_POOL)
        return create_client_surface_output_allocation( job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_BEGIN_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_TRY_BEGIN_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_END_UPDATE)
    {
        struct client_surface_compositor_target *target =
            find_client_surface_compositor_target( job->toplevel );

        if (!target || (job->notifications && target->notifications != job->notifications)) return FALSE;
        if (job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE ||
            job->op == CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE)
        {
            if (target->deferred_update != job->u.update.mark ||
                target->update_phase == CLIENT_SURFACE_UPDATE_UNPOSTED) return FALSE;
            if (job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE)
            {
                if (target->update_phase == CLIENT_SURFACE_UPDATE_CONSUMED) return FALSE;
                target->update_phase = CLIENT_SURFACE_UPDATE_CONSUMED;
                job->u.update.types = target->deferred_update_types | X11DRV_CLIENT_SURFACE_UPDATE_STATE;
                target->deferred_update_types = 0;
                job->u.update.notifications = target->notifications;
                pthread_mutex_lock( &client_surface_compositor_mutex );
                assert( !target->notifications->finish_serial && !target->notifications->finish_queued );
                ++target->notifications->refs;
                target->notifications->finish_serial = job->u.update.mark;
                target->notifications->finish.u.update.mark = job->u.update.mark;
                pthread_mutex_unlock( &client_surface_compositor_mutex );
            }
            else
            {
                if (!target->deferred_update_types) target->deferred_update = 0;
                target->update_phase = CLIENT_SURFACE_UPDATE_UNPOSTED;
                target->quiescing = target->native_updates || target->deferred_update;
            }
            return TRUE;
        }
        if (job->op == CLIENT_SURFACE_COMPOSITOR_TRY_BEGIN_UPDATE)
        {
            NTSTATUS status;

            quiesce_client_surface_compositor_target( target );
            target->preserve_content |= job->u.update.preserve_content;
            status = client_surface_compositor_update_ready( target, job );
            if (status == STATUS_PENDING)
            {
                if (!target->deferred_update)
                {
                    if (!(++client_surface_native_update_serial)) ++client_surface_native_update_serial;
                    target->deferred_update = client_surface_native_update_serial;
                }
                target->deferred_update_types |= job->u.update.types;
                job->u.update.status = STATUS_PENDING;
                TRACE( "deferring native state update %s for %p\n",
                       wine_dbgstr_longlong( target->deferred_update ), target->toplevel );
                return FALSE;
            }
            if (target->preserve_content)
            {
                job->u.update.status = status;
                target->preserve_content = FALSE;
                if (job->u.update.status)
                {
                    target->deferred_update_types &= ~job->u.update.types;
                    target->quiescing = target->native_updates || target->deferred_update;
                    return FALSE;
                }
            }
        }
        if (job->op != CLIENT_SURFACE_COMPOSITOR_END_UPDATE)
        {
            target->deferred_update_types &= ~job->u.update.types;
            ++target->native_updates;
            job->u.update.notifications = target->notifications;
            pthread_mutex_lock( &client_surface_compositor_mutex );
            ++target->notifications->refs;
            pthread_mutex_unlock( &client_surface_compositor_mutex );
            target->quiescing = TRUE;
            if (job->u.update.invalidate_scene) target->scene.valid = FALSE;
        }
        else
        {
            assert( job->u.update.count && target->native_updates >= job->u.update.count );
            target->native_updates -= job->u.update.count;
            target->quiescing = target->native_updates || target->deferred_update;
        }
        return TRUE;
    }
    if (job->op == CLIENT_SURFACE_COMPOSITOR_REGISTER_HANDOFF)
        return register_client_surface_compositor_handoff( job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_UPDATE_TARGET)
        return update_client_surface_compositor_target( job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_DROP_SEED)
    {
        struct client_surface_output_allocation *allocation =
            CONTAINING_RECORD( job, struct client_surface_output_allocation, release );

        release_client_surface_seed( allocation );
        free_client_surface_pending_allocation( allocation );
        return TRUE;
    }
    if (job->op == CLIENT_SURFACE_COMPOSITOR_COPY_POOL)
        return copy_client_surface_compositor_pool( job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_REPLACE_POOL)
        return replace_client_surface_compositor_pool( job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_REMOVE_TARGET)
        return remove_client_surface_compositor_target( job->toplevel );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_RETIRE_POOL)
        return retire_client_surface_compositor_pool( job->toplevel );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_RENEW_DIRECT)
        return renew_client_surface_direct_plan( job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_DIRECT_COMPLETE)
        return complete_client_surface_direct_plan( job );
    switch (job->op)
    {
    case CLIENT_SURFACE_COMPOSITOR_FREE_POOL:
        return release_client_surface_output_allocation( job->u.retired_pixmaps );
    case CLIENT_SURFACE_COMPOSITOR_PRESENT:
        return client_surface_present_on_compositor( job );
    default:
        return FALSE;
    }
}

static struct client_surface_compositor_target *client_surface_compositor_job_target(
    const struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_target *target = job->queue->target;
    unsigned int i;

    if (target && job->notifications && target->notifications != job->notifications) return NULL;
    /* A detached old pool shares routing order with its HWND, but it need
     * not wait for native reads of a replacement pool on the new target. */
    if (target && job->op == CLIENT_SURFACE_COMPOSITOR_FREE_POOL)
    {
        for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
            if (target->frames[i].pixmap && (target->frames[i].pixmap == job->u.retired_pixmaps[0] ||
                                           target->frames[i].pixmap == job->u.retired_pixmaps[1])) return target;
        return NULL;
    }
    return target;
}

static BOOL client_surface_compositor_job_ready( struct client_surface_compositor_job *job,
                                                struct client_surface_compositor_target *target,
                                                BOOL *rejected )
{
    unsigned int i;
    BOOL drain = FALSE;

    *rejected = FALSE;
    /* Authenticated DIRECT admission has already invalidated the old plan.
     * Its continuation only retires bindings and rechecks the selected scene. */
    if (job->op == CLIENT_SURFACE_COMPOSITOR_DIRECT_PLAN && job->scan.phase)
        return job->scan.phase != 2 || !target ||
               x11drv_native_window_content_status( target->window_owner ) != STATUS_PENDING;
    if (job->op == CLIENT_SURFACE_COMPOSITOR_DIRECT_PLAN)
    {
        struct client_surface_window_query *query = &job->u.direct_plan.query;

        /* Never reject/free the request while its native callback owns it.
         * Scene admission is checked again after this receipt completes. */
        if (!query->started)
            start_client_surface_window_query( query, job->u.direct_plan.window_owner, NULL );
        if (!ReadAcquire( &query->complete )) return FALSE;
    }
    if (job->op == CLIENT_SURFACE_COMPOSITOR_QUERY_WINDOW ||
        job->op == CLIENT_SURFACE_COMPOSITOR_ADMIT_PRESENT) return TRUE;
    if (job->op == CLIENT_SURFACE_COMPOSITOR_PRESENT)
    {
        struct client_surface_output_allocation *allocation = job->u.present.allocation;
        struct client_surface_compositor_frame *frame;
        BOOL abandoned;

        pthread_mutex_lock( &client_surface_compositor_mutex );
        abandoned = allocation->abandoned;
        pthread_mutex_unlock( &client_surface_compositor_mutex );
        if (abandoned || !target || target->window_owner != allocation->window_owner ||
            !(frame = get_client_surface_compositor_pixmap( target, allocation->source )) ||
            frame->image != allocation->source_image ||
            !client_surface_output_checkpoint_scene_current( &allocation->scene ))
        {
            *rejected = TRUE;
            return TRUE;
        }
    }
    if (job->op == CLIENT_SURFACE_COMPOSITOR_CREATE_POOL || job->op == CLIENT_SURFACE_COMPOSITOR_COPY_POOL ||
        job->op == CLIENT_SURFACE_COMPOSITOR_DROP_SEED) return TRUE;
    if (!target) return TRUE;
    if (job->op == CLIENT_SURFACE_COMPOSITOR_DIRECT_PLAN ||
        job->op == CLIENT_SURFACE_COMPOSITOR_RETIRE_POOL)
    {
        BOOL current = job->op == CLIENT_SURFACE_COMPOSITOR_DIRECT_PLAN ?
            client_surface_direct_plan_current( job, target ) :
            client_surface_compositor_pool_retirable( target );

        if (!current)
        {
            target->quiescing = target->native_updates || target->deferred_update;
            *rejected = TRUE;
            return TRUE;
        }
        /* Admission may fail after native completion or a server mutation.
         * Pause new work, but preserve assembly/mailbox ownership until the
         * exact scene RPC accepts. Other native barriers remain independent. */
        target->quiescing = TRUE;
        detach_client_surface_output_transform( target );
        for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
            if (target->frames[i].serial) return FALSE;
        return TRUE;
    }
    /* These probes never mutate native geometry or consume an output. They
     * must answer while that target's older Present work is still pending. */
    if (job->op == CLIENT_SURFACE_COMPOSITOR_TRY_BEGIN_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_END_UPDATE) return TRUE;
    /* The copy uses immutable scene/binding references. Inspecting those
     * references, or resolving source availability from completed owner
     * caches, neither mutates nor releases its output. A queued resolve can
     * arrive after replay resolved the inventory and started an assembly;
     * making the GUI wait for that assembly would block its next scene
     * change behind native completion. The resolver keeps the exact scene
     * and receipt checks, including its already-resolved no-op. */
    if (job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_SCENE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_CACHE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_RESOLVE_SOURCES) return TRUE;
    /* Expose restoration records its own deferred work if necessary. */
    /* Unlike checked XCB copies, transforms own every native input and their
     * completion record. Mutation/removal detaches adoption, not native work. */
    detach_client_surface_output_transform( target );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_PRESENT)
    {
        Pixmap source = job->u.present.source;
        struct client_surface_compositor_frame *frame = get_client_surface_compositor_pixmap( target, source );

        if (frame && (!frame->revision || client_surface_cache_write_pending( frame->image )))
        {
            /* PREPARE/snapshot and its nonzero generation protect GUI
             * publication. Expose fallback can still encounter a cancelled
             * old backing: refuse that read and let its normal repaint run. */
            TRACE_(csperf)( "ticks=%llu event=output_read_reject hwnd=%p op=%u source=%lx revision=%llu\n",
                           client_surface_perf_time(), target->toplevel, job->op, source,
                           (unsigned long long)frame->revision );
            *rejected = TRUE;
            return TRUE;
        }
    }
    /* Preserve the scene, binding and frame referenced by the request. Only
     * this target waits; jobs for independent targets remain eligible. */
    if (job->op == CLIENT_SURFACE_COMPOSITOR_REMOVE_TARGET ||
        job->op == CLIENT_SURFACE_COMPOSITOR_BEGIN_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_REPLACE_POOL) drain = TRUE;
    if (job->op == CLIENT_SURFACE_COMPOSITOR_UPDATE_TARGET)
        drain = target->window != job->u.pool.destination ||
                target->window_width != job->u.pool.window_width || target->window_height != job->u.pool.window_height ||
                !((target->frames[0].pixmap == job->u.pool.pixmaps[0] && target->frames[1].pixmap == job->u.pool.pixmaps[1]) ||
                  (target->frames[1].pixmap == job->u.pool.pixmaps[0] && target->frames[0].pixmap == job->u.pool.pixmaps[1]));
    if (drain)
    {
        /* Stop producing work for this target while its previous native
         * scene drains. Other targets remain eligible in the same loop. */
        if (job->op == CLIENT_SURFACE_COMPOSITOR_UPDATE_TARGET ||
            job->op == CLIENT_SURFACE_COMPOSITOR_REPLACE_POOL)
            /* Allocation may still fail. Preserve the pending publication
             * until the replacement pool has been prepared successfully. */
            target->quiescing = TRUE;
        else
            quiesce_client_surface_compositor_target( target );
    }
    /* An invalidating update discards the old scene, including output that
     * has not started native execution. State-only refreshes keep it. */
    if (job->op == CLIENT_SURFACE_COMPOSITOR_REMOVE_TARGET ||
        (job->op == CLIENT_SURFACE_COMPOSITOR_BEGIN_UPDATE && job->u.update.invalidate_scene))
    {
        client_surface_cancel_native_presents( &target->native_presents );
        process_client_surface_native_present( target );
    }
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
    {
        struct client_surface_compositor_frame *frame = &target->frames[i];

        if (!frame->serial) continue;
        if (drain ||
            (job->op == CLIENT_SURFACE_COMPOSITOR_PRESENT && frame->pixmap == job->u.present.source) ||
            (job->op == CLIENT_SURFACE_COMPOSITOR_FREE_POOL &&
             (frame->pixmap == job->u.retired_pixmaps[0] || frame->pixmap == job->u.retired_pixmaps[1]))) return FALSE;
    }
    return TRUE;
}

static void release_client_surface_compositor_job_resources( struct client_surface_compositor_job *job )
{
    client_surface_free_owned_array( job->scan.members );
    client_surface_free_owned_array( job->scan.receipts );
    job->scan.members = NULL;
    job->scan.receipts = NULL;
    if (job->op == CLIENT_SURFACE_COMPOSITOR_REPAIR_OWNER || job->op == CLIENT_SURFACE_COMPOSITOR_RESOLVE_SOURCES)
        reset_client_surface_owner_repair( &job->u.repair );
    /* Registration may fail validation, be cancelled before execution, or
     * reuse an existing pool. Only a newly created pool adopts the fd. The
     * section handle remains borrowed from the synchronous caller. */
    if (job->op == CLIENT_SURFACE_COMPOSITOR_REGISTER_HANDOFF && job->u.registration.ready_fd >= 0)
    {
        close( job->u.registration.ready_fd );
        job->u.registration.ready_fd = -1;
    }
}

static void queue_client_surface_end_locked( struct client_surface_owner_notifications *notifications,
                                             unsigned int count )
{
    BOOL idle = !notifications->pending_ends && !notifications->end.u.update.count;

    assert( count && notifications->refs >= count && count <= ~0u - notifications->pending_ends );
    notifications->pending_ends += count;
    if (idle) enqueue_client_surface_compositor_job( &notifications->end );
}

static void prepare_client_surface_notification( struct client_surface_compositor_job *job )
{
    struct client_surface_owner_notifications *notifications = job->notifications;

    if (!notifications) return;
    pthread_mutex_lock( &client_surface_compositor_mutex );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_END_UPDATE)
    {
        job->u.update.count = notifications->pending_ends;
        notifications->pending_ends = 0;
        assert( job->u.update.count );
    }
    else if (job->op == CLIENT_SURFACE_COMPOSITOR_DIRECT_COMPLETE)
    {
        assert( notifications->direct_pending );
        job->u.direct_complete = notifications->direct_proof;
        notifications->direct_pending = FALSE;
    }
    pthread_mutex_unlock( &client_surface_compositor_mutex );
}

/* The dispatcher has unlinked the node. Producers only change the pending
 * fields while it executes, so a concurrent notification can now requeue it
 * without overwriting the live job or losing its retained reference. */
static void finish_client_surface_notification( struct client_surface_compositor_job *job )
{
    struct client_surface_owner_notifications *notifications = job->notifications;
    BOOL unused;

    pthread_mutex_lock( &client_surface_compositor_mutex );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_END_UPDATE)
    {
        assert( notifications->refs >= job->u.update.count );
        notifications->refs -= job->u.update.count;
        job->u.update.count = 0;
        if (notifications->pending_ends) enqueue_client_surface_compositor_job( job );
    }
    else if (job->op == CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE)
    {
        notifications->finish_queued = FALSE;
        notifications->finish_serial = 0;
        --notifications->refs;
    }
    else
    {
        assert( job->op == CLIENT_SURFACE_COMPOSITOR_DIRECT_COMPLETE );
        if (notifications->direct_pending) enqueue_client_surface_compositor_job( job );
        else
        {
            notifications->direct_queued = FALSE;
            --notifications->refs;
        }
    }
    unused = !notifications->refs;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    if (unused)
    {
        release_client_surface_compositor_queue( notifications->queue );
        free_client_surface_compositor_release( &notifications->memory, notifications, 4, sizeof(*notifications) );
    }
}

static BOOL client_surface_compositor_control_job( const struct client_surface_compositor_job *job )
{
    return job->op == CLIENT_SURFACE_COMPOSITOR_CREATE_POOL ||
           job->op == CLIENT_SURFACE_COMPOSITOR_QUERY_WINDOW ||
           job->op == CLIENT_SURFACE_COMPOSITOR_ADMIT_PRESENT ||
           job->op == CLIENT_SURFACE_COMPOSITOR_DROP_SEED ||
           job->op == CLIENT_SURFACE_COMPOSITOR_COPY_POOL ||
           job->op == CLIENT_SURFACE_COMPOSITOR_TRY_BEGIN_UPDATE ||
           job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE ||
           job->op == CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE ||
           job->op == CLIENT_SURFACE_COMPOSITOR_END_UPDATE ||
           job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_CACHE;
}

void ready_client_surface_compositor_queue( struct client_surface_compositor_queue *queue )
{
    list_remove( &queue->entry );
    list_add_tail( &client_surface_compositor_ready, &queue->entry );
}

/* Inspect only a FIFO head. A started Present keeps this exact job until its
 * completion or caller deadline, independently of the output's native lease. */
static BOOL step_client_surface_compositor_job( struct client_surface_compositor_job *job,
                                               BOOL *progressed, BOOL *runnable, unsigned int *budget )
{
    struct client_surface_compositor_target *target = client_surface_compositor_job_target( job );
    BOOL rejected, done = TRUE;

    *runnable = FALSE;
    if (job->op == CLIENT_SURFACE_COMPOSITOR_PRESENT && job->u.present.started)
    {
        if (!job->u.present.done && NtGetTickCount() - job->u.present.start >= 5000)
        {
            unsigned int i;

            if (target)
                for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
                    if (target->frames[i].waiter == job) target->frames[i].waiter = NULL;
            job->u.present.done = TRUE;
            job->result = FALSE;
        }
        return job->u.present.done;
    }
    rejected = job->sequence <= job->queue->retired_through && !job->notifications &&
               job->op != CLIENT_SURFACE_COMPOSITOR_REMOVE_TARGET &&
               job->op != CLIENT_SURFACE_COMPOSITOR_FREE_POOL &&
               job->op != CLIENT_SURFACE_COMPOSITOR_DROP_SEED;
    if (rejected)
    {
        /* A query started before the removal marker still owns its request
         * until native completion. Reject adoption, never that ownership. */
        if (job->op == CLIENT_SURFACE_COMPOSITOR_DIRECT_PLAN && job->u.direct_plan.query.started &&
            !ReadAcquire( &job->u.direct_plan.query.complete )) return FALSE;
        TRACE_(csperf)( "ticks=%llu event=retired_job_reject hwnd=%p op=%u sequence=%llu through=%llu\n",
                       client_surface_perf_time(), job->toplevel, job->op,
                       (unsigned long long)job->sequence, (unsigned long long)job->queue->retired_through );
    }
    else if (!client_surface_compositor_job_ready( job, target, &rejected )) return FALSE;
    prepare_client_surface_notification( job );
    job->result = FALSE;
    if (!rejected)
    {
        switch (job->op)
        {
        case CLIENT_SURFACE_COMPOSITOR_REUSE_HANDOFFS:
            done = reuse_client_surface_compositor_handoffs( job, budget );
            break;
        case CLIENT_SURFACE_COMPOSITOR_CHECK_SCENE:
            done = check_client_surface_compositor_scene( job, budget );
            break;
        case CLIENT_SURFACE_COMPOSITOR_REPAIR_OWNER:
        case CLIENT_SURFACE_COMPOSITOR_RESOLVE_SOURCES:
            done = repair_client_surface_compositor_owner( job->toplevel,
                job->op == CLIENT_SURFACE_COMPOSITOR_RESOLVE_SOURCES, &job->u.repair );
            job->result = job->u.repair.result;
            break;
        case CLIENT_SURFACE_COMPOSITOR_CHECK_CACHE:
        {
            struct client_surface_compositor_binding *binding = next_client_surface_job_binding( job );

            while (binding && *budget)
            {
                --*budget;
                advance_client_surface_job_binding( job, binding );
                if ((job->result = !!binding->latest_image.pixmap)) break;
                binding = next_client_surface_compositor_binding( binding );
            }
            done = job->result || !binding;
            if (done) TRACE( "owner cache probe hwnd %p cached %u\n", job->toplevel, job->result );
            break;
        }
        case CLIENT_SURFACE_COMPOSITOR_SWEEP_HANDOFFS:
            if (!job->scan.phase)
            {
                if (!(done = sweep_client_surface_compositor_handoffs( job, job->u.scene_install.mark, budget ))) break;
                job->scan.phase = 1;
            }
            if (target) done = install_client_surface_scene_plan( target, job, budget );
            else job->result = TRUE;
            break;
        case CLIENT_SURFACE_COMPOSITOR_REMOVE_TARGET:
            if ((done = sweep_client_surface_compositor_handoffs( job, 0, budget )))
                job->result = remove_client_surface_compositor_target( job->toplevel );
            break;
        case CLIENT_SURFACE_COMPOSITOR_DIRECT_PLAN:
            job->result = install_client_surface_direct_plan( job, budget, &done );
            break;
        default:
            job->result = execute_client_surface_compositor_job( job );
            break;
        }
    }
    *progressed = TRUE;
    if (!done)
    {
        TRACE_(csperf)( "ticks=%llu event=compositor_job_yield hwnd=%p op=%u phase=%u index=%u\n",
                       client_surface_perf_time(), job->toplevel, job->op, job->scan.phase, job->scan.index );
        *runnable = TRUE;
        return FALSE;
    }
    if (job->op == CLIENT_SURFACE_COMPOSITOR_DIRECT_PLAN ||
        job->op == CLIENT_SURFACE_COMPOSITOR_RETIRE_POOL)
    {
        target = client_surface_compositor_job_target( job );
        if (target) target->quiescing = target->native_updates || target->deferred_update;
    }
    return job->op != CLIENT_SURFACE_COMPOSITOR_PRESENT || !job->u.present.started || job->u.present.done;
}

static BOOL process_client_surface_compositor_jobs(void)
{
    struct client_surface_compositor_queue *queue;
    struct client_surface_compositor_job *incoming, **tail = &incoming, *job;
    struct list yielded = LIST_INIT( yielded );
    unsigned int admitted = 0, scanned = 0, completed = 0, budget = 64, member_budget = 64;
    BOOL progressed = FALSE, more;

    /* Bound ingestion as well as execution. The next iteration resumes from
     * the same incoming head, without traversing any parked queue's jobs. */
    pthread_mutex_lock( &client_surface_compositor_mutex );
    while (admitted < 64 && (job = client_surface_compositor_head))
    {
        client_surface_compositor_head = job->next;
        *tail = job;
        tail = &job->next;
        --job->queue->incoming;
        ++admitted;
    }
    if (!client_surface_compositor_head) client_surface_compositor_tail = &client_surface_compositor_head;
    more = !!client_surface_compositor_head;
    *tail = NULL;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    while ((job = incoming))
    {
        incoming = job->next;
        job->next = NULL;
        queue = job->queue;
        if (client_surface_compositor_control_job( job ))
        {
            *queue->control_tail = job;
            queue->control_tail = &job->next;
        }
        else
        {
            *queue->tail = job;
            queue->tail = &job->next;
        }
        ready_client_surface_compositor_queue( queue );
    }

    while (budget && member_budget && !list_empty( &client_surface_compositor_ready ))
    {
        BOOL done, control, runnable, control_runnable;

        queue = LIST_ENTRY( list_head( &client_surface_compositor_ready ), struct client_surface_compositor_queue, entry );
        list_remove( &queue->entry );
        list_init( &queue->entry );
        control = queue->control_head && (!queue->head || queue->control_head->sequence < queue->head->sequence);
        job = control ? queue->control_head : queue->head;
        assert( job );
        --budget;
        ++scanned;
        done = step_client_surface_compositor_job( job, &progressed, &runnable, &member_budget );
        /* Preserve the old bypass contract: only these control operations
         * may pass a blocked normal head. Their own FIFO remains ordered. */
        if (!done && !control && queue->control_head)
        {
            if (!budget || !member_budget)
            {
                ready_client_surface_compositor_queue( queue );
                break;
            }
            control = TRUE;
            job = queue->control_head;
            --budget;
            ++scanned;
            done = step_client_surface_compositor_job( job, &progressed, &control_runnable, &member_budget );
            runnable |= control_runnable;
        }
        if (!done)
        {
            /* This head exhausted its member/repair budget. Inspect other
             * queues now, and resume it only in the next actor interval. */
            if (runnable) list_add_tail( &yielded, &queue->entry );
            else list_add_tail( &client_surface_compositor_parked, &queue->entry );
            continue;
        }
        if (control)
        {
            if (!(queue->control_head = job->next)) queue->control_tail = &queue->control_head;
        }
        else if (!(queue->head = job->next)) queue->tail = &queue->head;
        if (queue->head || queue->control_head)
        {
            if (runnable) list_add_tail( &yielded, &queue->entry );
            else ready_client_surface_compositor_queue( queue );
        }
        ++completed;
        progressed = TRUE;
        release_client_surface_compositor_job_resources( job );
        if (job->async)
        {
            if (job->notifications) finish_client_surface_notification( job );
            else if (job->op == CLIENT_SURFACE_COMPOSITOR_REMOVE_TARGET)
            {
                pthread_mutex_lock( &client_surface_compositor_mutex );
                queue->retired_through = queue->remove_through;
                queue->remove_queued = FALSE;
                pthread_mutex_unlock( &client_surface_compositor_mutex );
            }
            else if (job->op == CLIENT_SURFACE_COMPOSITOR_PRESENT)
            {
                struct client_surface_output_allocation *allocation = job->u.present.allocation;
                BOOL success = job->result;

                /* The job is unlinked and its frame waiter has detached.
                 * Cancellation may free the continuation during this call. */
                job->async = FALSE;
                client_surface_output_allocation_complete( allocation, success );
            }
            else
            {
                struct client_surface_output_allocation *allocation =
                    CONTAINING_RECORD( job, struct client_surface_output_allocation, release );

                assert( job->op == CLIENT_SURFACE_COMPOSITOR_FREE_POOL || job->op == CLIENT_SURFACE_COMPOSITOR_DROP_SEED );
                release_client_surface_compositor_queue( allocation->release.queue );
                free_client_surface_compositor_release( &allocation->memory, allocation, 1, sizeof(*allocation) );
            }
        }
        else
        {
            struct client_surface_compositor_request *request =
                CONTAINING_RECORD( job, struct client_surface_compositor_request, job );

            pthread_mutex_lock( &client_surface_compositor_mutex );
            job->complete = TRUE;
            pthread_cond_signal( &request->completed );
            pthread_mutex_unlock( &client_surface_compositor_mutex );
        }
        /* The enqueue reference also covers dispatcher bookkeeping after a
         * synchronous caller returns or the final notification frees itself. */
        release_client_surface_compositor_queue( queue );
    }
    list_move_tail( &client_surface_compositor_ready, &yielded );
    TRACE_(csperf)( "ticks=%llu event=compositor_queue_scan ingested=%u inspected=%u completed=%u more=%u runnable=%u members=%u\n",
                   client_surface_perf_time(), admitted, scanned, completed, more,
                   !list_empty( &client_surface_compositor_ready ), 64 - member_budget );
    return progressed || more || !list_empty( &client_surface_compositor_ready );
}

static void client_surface_compositor_thread( void *context )
{
    BOOL armed = FALSE;

    (void)context;
    for (;;)
    {
        struct client_surface_compositor_scan scan =
        {
            .wake_serial = client_surface_compositor_wake_serial,
            .timeout = -1,
        };

        init_client_surface_output_scan( &scan );
        init_client_surface_source_scan( &scan );
        TRACE_(csperf)( "ticks=%llu event=compositor_scan_begin targets=%u armed=%u generation=%s pools=%u pool_generation=%s\n",
                       client_surface_perf_time(), scan.target_count, armed,
                       wine_dbgstr_longlong( scan.generation ), scan.pool_count,
                       wine_dbgstr_longlong( scan.pool_generation ) );
        do
        {
            BOOL progressed;

            progressed = begin_client_surface_composition();
            if (progressed) wake_client_surface_compositor_queues();
            progressed |= process_client_surface_compositor_jobs();
            progressed |= process_client_surface_output_seeds( &scan );
            progressed |= process_client_surface_handoffs( &scan );
            progressed |= process_client_surface_compositor_targets( &scan );
            progressed |= notify_client_surface_output_allocations( &scan );
            progressed |= finish_client_surface_composition();
            scan.progressed |= progressed;
        } while (scan.remaining || scan.handoff_remaining);
        /* Helpers acquiring output credit can consume events too. Count
         * those wakes even when their caller could not submit any work. */
        if (scan.progressed || scan.wake_serial != client_surface_compositor_wake_serial)
        {
            armed = FALSE;
            continue;
        }
        if (!armed)
        {
            arm_client_surface_compositor_work();
            armed = TRUE;
            continue;
        }
        wait_client_surface_compositor_work( &scan );
        armed = FALSE;
    }
}

static BOOL init_client_surface_compositor_notification(void)
{
    int fds[2];

    if (client_surface_compositor_notify[0] >= 0) return TRUE;
#ifdef __linux__
    if ((fds[0] = eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK )) < 0) return FALSE;
    if ((fds[1] = fcntl( fds[0], F_DUPFD_CLOEXEC, 0 )) < 0)
    {
        close( fds[0] );
        return FALSE;
    }
#else
    if (socketpair( AF_UNIX, SOCK_DGRAM, 0, fds )) return FALSE;
    if (fcntl( fds[0], F_SETFD, FD_CLOEXEC ) < 0 || fcntl( fds[1], F_SETFD, FD_CLOEXEC ) < 0 ||
        fcntl( fds[0], F_SETFL, O_NONBLOCK ) < 0 || fcntl( fds[1], F_SETFL, O_NONBLOCK ) < 0)
    {
        close( fds[0] );
        close( fds[1] );
        return FALSE;
    }
#endif
    client_surface_compositor_notify[0] = fds[0];
    client_surface_compositor_notify[1] = fds[1];
    return TRUE;
}

/* An admitted resource's release is infallible. This only links its existing
 * node under the metadata mutex; no native operation runs while it is held. */
void enqueue_client_surface_compositor_job( struct client_surface_compositor_job *job )
{
    assert( client_surface_compositor_started );
    assert( job->queue );
    ++job->queue->refs;
    ++job->queue->incoming;
    job->sequence = ++client_surface_compositor_sequence;
    job->next = NULL;
    job->complete = FALSE;
    if (job->op == CLIENT_SURFACE_COMPOSITOR_PRESENT)
        job->u.present.started = job->u.present.done = FALSE;
    if (job->op == CLIENT_SURFACE_COMPOSITOR_TRY_BEGIN_UPDATE)
        job->u.update.status = STATUS_SUCCESS;
    *client_surface_compositor_tail = job;
    client_surface_compositor_tail = &job->next;
    wake_client_surface_compositor();
}

/* The caller holds client_surface_compositor_mutex. Ownership of asynchronous
 * jobs passes to the actor only when they have been queued successfully. */
static BOOL queue_client_surface_compositor_job( struct client_surface_compositor_job *job )
{
    HANDLE thread;
    NTSTATUS status;

    job->complete = FALSE;
    if (!client_surface_compositor_started)
    {
        if (!init_client_surface_compositor_notification()) return FALSE;
        status = PsCreateSystemThread( &thread, THREAD_ALL_ACCESS, NULL, 0, NULL,
                                       client_surface_compositor_thread, NULL );
        if (status)
        {
            WARN( "failed to create client-surface compositor, status %#lx\n",
                  (unsigned long)status );
            return FALSE;
        }
        NtClose( thread );
        client_surface_compositor_started = TRUE;
    }
    enqueue_client_surface_compositor_job( job );
    return TRUE;
}

BOOL submit_client_surface_compositor_request( struct client_surface_compositor_request *request )
{
    struct client_surface_compositor_job *job = &request->job;
    struct client_surface_compositor_queue *queue;
    BOOL ret = FALSE;
    BOOL barrier = job->op == CLIENT_SURFACE_COMPOSITOR_BEGIN_UPDATE ||
                   job->op == CLIENT_SURFACE_COMPOSITOR_TRY_BEGIN_UPDATE ||
                   job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE;

    if (!(queue = get_client_surface_compositor_queue( job->toplevel ))) goto failed;
    pthread_mutex_lock( &client_surface_compositor_mutex );
    if (barrier)
    {
        /* These callers already synchronously protect native geometry or
         * drain a target. They cannot interpret capacity refusal as "there
         * was no target to protect". Their admitted queue owns one slot;
         * concurrent barriers serialize here instead of allocating jobs.
         * No release notification waits for this slot or uses its storage. */
        while (queue->barrier_in_use)
            pthread_cond_wait( &queue->barrier_available, &client_surface_compositor_mutex );
        queue->barrier_in_use = TRUE;
        pthread_mutex_unlock( &client_surface_compositor_mutex );
    }
    else
    {
        BOOL accepted = reserve_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_REQUEST_CAPACITY,
                                                                     1, sizeof(*request), queue );

        pthread_mutex_unlock( &client_surface_compositor_mutex );
        if (!accepted) goto release_queue;
        /* Stack storage has the same admission lifetime as a heap request.
         * The retained queue owns the accounting scope through its return. */
        if (!client_surface_reserve_scoped_metadata( &queue->memory, sizeof(*request) ))
        {
            release_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_REQUEST_CAPACITY,
                                                        1, sizeof(*request), queue );
            goto release_queue;
        }
    }
    request->completed = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
    job->queue = queue;
    job->async = FALSE;
    memset( &job->scan, 0, sizeof(job->scan) );
    pthread_mutex_lock( &client_surface_compositor_mutex );
    if (queue_client_surface_compositor_job( job ))
    {
        /* The caller retains this request and every borrowed payload until
         * the actor unlinks it and returns all of its resource ownership. */
        while (!job->complete)
            pthread_cond_wait( &request->completed, &client_surface_compositor_mutex );
        ret = job->result;
    }
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    if (!job->complete) release_client_surface_compositor_job_resources( job );
    pthread_cond_destroy( &request->completed );
    if (barrier)
    {
        pthread_mutex_lock( &client_surface_compositor_mutex );
        assert( queue->barrier_in_use );
        queue->barrier_in_use = FALSE;
        pthread_cond_signal( &queue->barrier_available );
        pthread_mutex_unlock( &client_surface_compositor_mutex );
    }
    else
    {
        client_surface_release_scoped_metadata( &queue->memory, sizeof(*request) );
        release_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_REQUEST_CAPACITY,
                                                    1, sizeof(*request), queue );
    }
    release_client_surface_compositor_queue( queue );
    return ret;

release_queue:
    release_client_surface_compositor_queue( queue );
failed:
    release_client_surface_compositor_job_resources( job );
    return FALSE;
}

/* Every notification already owns storage: an output allocation or a target
 * notification object. This boundary never allocates or waits for dispatch. */
void post_client_surface_compositor_job( struct client_surface_compositor_job *job )
{
    struct client_surface_owner_notifications *notifications = job->notifications;

    assert( job->op == CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE ||
            job->op == CLIENT_SURFACE_COMPOSITOR_END_UPDATE ||
            job->op == CLIENT_SURFACE_COMPOSITOR_DIRECT_COMPLETE );
    pthread_mutex_lock( &client_surface_compositor_mutex );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_END_UPDATE)
    {
        assert( notifications );
        queue_client_surface_end_locked( notifications, 1 );
    }
    else if (job->op == CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE)
    {
        if (notifications && notifications->finish_serial == job->u.update.mark && !notifications->finish_queued)
        {
            notifications->finish_queued = TRUE;
            enqueue_client_surface_compositor_job( &notifications->finish );
        }
    }
    else
    {
        for (notifications = client_surface_owner_notifications; notifications; notifications = notifications->next)
            if (notifications->toplevel == job->toplevel) break;
        if (notifications && notifications->direct_plan.identity == job->u.direct_complete.identity &&
            notifications->direct_plan.scene_epoch == job->u.direct_complete.scene_epoch &&
            notifications->direct_plan.source == job->u.direct_complete.source)
        {
            notifications->direct_proof = job->u.direct_complete;
            notifications->direct_pending = TRUE;
            if (!notifications->direct_queued)
            {
                notifications->direct_queued = TRUE;
                ++notifications->refs;
                enqueue_client_surface_compositor_job( &notifications->direct );
            }
        }
    }
    pthread_mutex_unlock( &client_surface_compositor_mutex );
}

void remove_client_surface_backing_target( HWND toplevel )
{
    struct client_surface_compositor_queue *queue;
    struct rb_entry *entry;

    /* Every target, binding and admitted request retains its queue. No
     * registry entry means there is no actor ownership to retire. */
    pthread_mutex_lock( &client_surface_compositor_mutex );
    if (client_surface_compositor_started && (entry = rb_get( &client_surface_compositor_queues, toplevel )))
    {
        queue = CONTAINING_RECORD( entry, struct client_surface_compositor_queue, registry_entry );
        if (!queue->remove_queued)
        {
            queue->remove = (struct client_surface_compositor_job){
                .op = CLIENT_SURFACE_COMPOSITOR_REMOVE_TARGET, .toplevel = toplevel,
                .queue = queue, .async = TRUE};
            queue->remove_queued = TRUE;
            enqueue_client_surface_compositor_job( &queue->remove );
        }
        else ++client_surface_compositor_sequence;
        queue->remove_through = client_surface_compositor_sequence;
        TRACE_(csperf)( "ticks=%llu event=target_remove_admit hwnd=%p sequence=%llu through=%llu\n",
                       client_surface_perf_time(), toplevel, (unsigned long long)queue->remove.sequence,
                       (unsigned long long)queue->remove_through );
    }
    pthread_mutex_unlock( &client_surface_compositor_mutex );
}

/* Each END already owns a notifications reference from BEGIN. Freeze a
 * count before submitting its stream marker; later ENDs form the next batch.
 * Thus coalescing never assigns an earlier receipt to a newer GUI update. */
static void execute_client_surface_native_end( struct client_surface_native_work *work )
{
    struct client_surface_owner_notifications *notifications =
        CONTAINING_RECORD( work, struct client_surface_owner_notifications, native_end );

    if (!notifications->native_batch)
    {
        pthread_mutex_lock( &client_surface_compositor_mutex );
        notifications->native_batch = notifications->native_ends;
        notifications->native_ends = 0;
        assert( notifications->native_batch );
        pthread_mutex_unlock( &client_surface_compositor_mutex );
    }
    x11drv_native_window_read_ready( &notifications->end_read );
}

static void finish_client_surface_native_end( struct client_surface_native_work *work )
{
    struct client_surface_owner_notifications *notifications =
        CONTAINING_RECORD( work, struct client_surface_owner_notifications, native_end );
    struct x11drv_native_window *window = NULL;
    BOOL again;

    if (!notifications->end_read.geometry.complete)
    {
        client_surface_submit_native_work( work );
        return;
    }
    pthread_mutex_lock( &client_surface_compositor_mutex );
    queue_client_surface_end_locked( notifications, notifications->native_batch );
    TRACE_(csperf)( "ticks=%llu event=native_end_receipt notifications=%p count=%u serial=%lu\n",
                   client_surface_perf_time(), notifications, notifications->native_batch,
                   notifications->end_read.geometry.serial );
    notifications->native_batch = 0;
    again = !!notifications->native_ends;
    if (again) window = x11drv_native_window_acquire( notifications->end_read.window );
    x11drv_native_window_read_finish( &notifications->end_read );
    notifications->end_read = (struct x11drv_native_window_read){0};
    if (again)
    {
        x11drv_native_window_read_init( &notifications->end_read, window );
        x11drv_native_window_release( window );
    }
    else
    {
        notifications->end_display = NULL;
    }
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    /* With no next batch the actor may immediately free this object. */
    if (again) client_surface_submit_native_work( work );
    wake_client_surface_compositor();
}

void end_client_surface_native_update( struct client_surface_owner_notifications *notifications,
                                       Display *display, struct x11drv_native_window *window )
{
    BOOL start;

    if (!window)
    {
        struct client_surface_compositor_job job =
        {
            .op = CLIENT_SURFACE_COMPOSITOR_END_UPDATE,
            .notifications = notifications,
            .toplevel = notifications->toplevel,
        };
        post_client_surface_compositor_job( &job );
        return;
    }
    pthread_mutex_lock( &client_surface_compositor_mutex );
    assert( notifications->refs && notifications->native_ends < ~0u );
    start = !notifications->native_ends && !notifications->native_batch;
    ++notifications->native_ends;
    if (start)
    {
        notifications->end_display = display;
        x11drv_native_window_read_init( &notifications->end_read, window );
        notifications->native_end.execute = execute_client_surface_native_end;
        notifications->native_end.finished = finish_client_surface_native_end;
    }
    else assert( notifications->end_display == display );
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    if (start) client_surface_submit_native_work( &notifications->native_end );
}

const struct client_surface_memory_scope *client_surface_compositor_queue_memory(
    const struct client_surface_compositor_queue *queue )
{
    return &queue->memory;
}

/* Admission owns all three asynchronous notifications and their native END
 * receipt. The target releases its reference; queued receipts keep theirs. */
BOOL init_client_surface_target_notifications( struct client_surface_compositor_target *target )
{
    struct client_surface_memory_scope memory;
    struct client_surface_owner_notifications *notifications;

    if (!x11drv_reserve_release_capacity( 4, sizeof(*notifications) )) return FALSE;
    memset( &memory, 0, sizeof(memory) );
    client_surface_memory_scope_copy( &memory, &target->memory, TRUE );
    if (!(notifications = client_surface_alloc_scoped_metadata( &memory, 1, sizeof(*notifications) )))
    {
        client_surface_memory_scope_destroy( &memory );
        release_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_RELEASE_CAPACITY, 4, sizeof(*notifications), NULL );
        return FALSE;
    }
    notifications->memory = memory;
    /* The creating job already owns this queue, so this lookup cannot need
     * another allocation or depend on a second admission decision. */
    notifications->queue = get_client_surface_compositor_queue( target->toplevel );
    assert( notifications->queue );
    notifications->toplevel = target->toplevel;
    notifications->refs = 1;
    notifications->end = (struct client_surface_compositor_job){
        .op = CLIENT_SURFACE_COMPOSITOR_END_UPDATE, .toplevel = target->toplevel,
        .notifications = notifications, .queue = notifications->queue, .async = TRUE};
    notifications->finish = (struct client_surface_compositor_job){
        .op = CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE, .toplevel = target->toplevel,
        .notifications = notifications, .queue = notifications->queue, .async = TRUE};
    notifications->direct = (struct client_surface_compositor_job){
        .op = CLIENT_SURFACE_COMPOSITOR_DIRECT_COMPLETE, .toplevel = target->toplevel,
        .notifications = notifications, .queue = notifications->queue, .async = TRUE};
    target->notifications = notifications;
    notifications->queue->target = target;
    pthread_mutex_lock( &client_surface_compositor_mutex );
    notifications->next = client_surface_owner_notifications;
    client_surface_owner_notifications = notifications;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    return TRUE;
}

void destroy_client_surface_target_notifications( struct client_surface_compositor_target *target )
{
    struct client_surface_owner_notifications **cursor, *notifications = target->notifications;
    BOOL unused;

    assert( notifications->queue->target == target );
    notifications->queue->target = NULL;
    pthread_mutex_lock( &client_surface_compositor_mutex );
    for (cursor = &client_surface_owner_notifications; *cursor != notifications; cursor = &(*cursor)->next)
        assert( *cursor );
    *cursor = notifications->next;
    unused = !--notifications->refs;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    if (unused)
    {
        release_client_surface_compositor_queue( notifications->queue );
        free_client_surface_compositor_release( &notifications->memory, notifications, 4, sizeof(*notifications) );
    }
}
