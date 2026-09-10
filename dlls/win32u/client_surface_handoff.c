/*
 * Client surface producer handoff and immutable source reservations
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

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#ifdef __linux__
#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#endif

#include "ntstatus.h"
#include "client_surface.h"
#include "ntuser_private.h"
#include "wine/server.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(win);
WINE_DECLARE_DEBUG_CHANNEL(csperf);

/* Only this module changes the binding, source reservations and release waiters.
 * Submission/completion serialization pins the binding for borrowed frame data;
 * present_lock protects detachment against native target changes. */
struct client_surface_handoff
{
    void *view;
    SIZE_T view_size;
    struct client_surface_handoff_shared *shared;
    struct client_surface_handoff_channel *channel;
    struct client_surface_source sources[CLIENT_SURFACE_SOURCE_FRAME_COUNT];
    unsigned int next;
    UINT64 serial;
    UINT64 mapping_id;
    UINT64 cookie;
    BOOL release_pending;
    unsigned int waiters;
    int ready_fd;
};

struct client_surface *client_surface_alloc( UINT size )
{
    struct client_surface *surface;
    SIZE_T offset = (SIZE_T)size + __alignof__(struct client_surface_handoff) - 1;

    offset &= ~((SIZE_T)__alignof__(struct client_surface_handoff) - 1);
    if (offset < size || offset > ~(SIZE_T)0 - sizeof(*surface->handoff)) return NULL;
    /* Keep driver storage and private state in the same allocation. DIRECT
     * surfaces gain no extra allocator call or separately owned lifetime. */
    if (!(surface = calloc( 1, offset + sizeof(*surface->handoff) ))) return NULL;
    surface->handoff = (void *)((char *)surface + offset);
    surface->handoff->ready_fd = -1;
    return surface;
}

void client_surface_handoff_destroy( struct client_surface *surface )
{
    assert( !surface->handoff->waiters );
}

static unsigned long long client_surface_perf_time(void)
{
    LARGE_INTEGER counter;

    NtQueryPerformanceCounter( &counter, NULL );
    return counter.QuadPart;
}

static void client_surface_handoff_wake_ready( struct client_surface *surface )
{
    struct client_surface_handoff_shared *shared = surface->handoff->shared;
    UINT64 value = 1;
    int ret;

    if (!__atomic_exchange_n( &shared->ready_parked, 0, __ATOMIC_ACQ_REL )) return;
    __atomic_add_fetch( &shared->ready_sequence, 1, __ATOMIC_RELEASE );
    do
#ifdef __linux__
        ret = write( surface->handoff->ready_fd, &value, sizeof(value) );
#else
        ret = send( surface->handoff->ready_fd, &value, sizeof(value), 0 );
#endif
    while (ret < 0 && errno == EINTR);
    TRACE_(csperf)( "ticks=%llu event=ready_signal identity=%s cookie=%s fd=%d result=%d\n",
                   client_surface_perf_time(), wine_dbgstr_longlong( client_surface_get_identity( surface ) ),
                   wine_dbgstr_longlong( surface->handoff->cookie ), surface->handoff->ready_fd, ret );
}

static void client_surface_handoff_wait_sequence( LONG *address, LONG sequence, DWORD timeout )
{
#ifdef __linux__
    struct timespec timespec = {timeout / 1000, (timeout % 1000) * 1000000};

    syscall( SYS_futex, address, FUTEX_WAIT, sequence, &timespec, NULL, 0 );
#else
    LARGE_INTEGER delay = {.QuadPart = -(LONGLONG)min( timeout, 1 ) * 10000};

    NtDelayExecution( FALSE, &delay );
#endif
}

static void client_surface_handoff_wake_release( struct client_surface_handoff_shared *shared )
{
    if (!__atomic_exchange_n( &shared->release_parked, 0, __ATOMIC_ACQ_REL )) return;
    __atomic_add_fetch( &shared->release_sequence, 1, __ATOMIC_RELEASE );
#ifdef __linux__
    syscall( SYS_futex, &shared->release_sequence, FUTEX_WAKE, INT_MAX, NULL, NULL, 0 );
#endif
}

/* The caller holds present_lock through both backend retirement and detachment. */
void client_surface_release_handoff( struct client_surface *surface )
{
    struct client_surface_handoff_channel *channel;
    DWORD start = NtGetTickCount();
    unsigned int index;

    if (!surface->handoff->view) return;
    /* Exact WSI completions may finish on a worker after a target update has
     * detached this mapping.  Keep the view and producer endpoint alive until
     * those frames have either published or abandoned their private reservation.
     * A source-capacity waiter also retains the view while its lock is dropped. */
    if (surface->handoff->waiters || InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ))
    {
        if (surface->handoff->waiters)
            TRACE( "retaining handoff mapping identity %s cookie %s for %u source waiters\n",
                   wine_dbgstr_longlong( client_surface_get_identity( surface ) ), wine_dbgstr_longlong( surface->handoff->cookie ),
                   surface->handoff->waiters );
        surface->handoff->release_pending = TRUE;
        return;
    }
    if (surface->backend->handoff_retire)
    {
        struct client_surface_handoff_lease lease =
        {
            .view = surface->handoff->view, .channel = surface->handoff->channel,
            .cookie = surface->handoff->cookie, .ready_fd = surface->handoff->ready_fd,
        };

        surface->backend->handoff_retire( surface, &lease );
        goto detached;
    }
    channel = surface->handoff->channel;
    __atomic_store_n( &channel->closed, 1, __ATOMIC_RELEASE );
    index = channel - surface->handoff->shared->channels;
    __atomic_fetch_or( &surface->handoff->shared->ready_bitmap[index / 64],
                       (UINT64)1 << (index % 64), __ATOMIC_RELEASE );
    client_surface_handoff_wake_ready( surface );
    while ((__atomic_load_n( &channel->endpoints, __ATOMIC_ACQUIRE ) &
            CLIENT_SURFACE_HANDOFF_ENDPOINT_CONSUMER) &&
           __atomic_load_n( &channel->consumer_sequence, __ATOMIC_ACQUIRE ) !=
           __atomic_load_n( &channel->producer_sequence, __ATOMIC_ACQUIRE ))
    {
        LONG sequence;

        if (NtGetTickCount() - start >= CLIENT_SURFACE_PRESENT_TIMEOUT)
        {
            surface->handoff->release_pending = TRUE;
            return;
        }
        __atomic_store_n( &surface->handoff->shared->release_parked, 1, __ATOMIC_RELEASE );
        sequence = __atomic_load_n( &surface->handoff->shared->release_sequence, __ATOMIC_ACQUIRE );
        client_surface_handoff_wait_sequence( &surface->handoff->shared->release_sequence, sequence, 10 );
    }
    SERVER_START_REQ( release_client_surface_handoff )
    {
        req->handle = 0;
        req->producer = 0;
        req->surface = client_surface_get_identity( surface );
        req->cookie = surface->handoff->cookie;
        req->owner = 0;
        wine_server_call( req );
    }
    SERVER_END_REQ;
    NtUnmapViewOfSection( NtCurrentProcess(), surface->handoff->view );
    if (surface->handoff->ready_fd >= 0) close( surface->handoff->ready_fd );
detached:
    surface->handoff->ready_fd = -1;
    surface->handoff->view = NULL;
    surface->handoff->view_size = 0;
    surface->handoff->shared = NULL;
    surface->handoff->channel = NULL;
    surface->handoff->mapping_id = 0;
    surface->handoff->cookie = 0;
    surface->handoff->release_pending = FALSE;
}

static BOOL map_client_surface_handoff( struct client_surface *surface )
{
    struct client_surface_handoff_shared *shared;
    struct client_surface_handoff_channel *channel;
    HANDLE mapping = NULL, event = NULL;
    SIZE_T offset = 0, size = 0;
    UINT64 mapping_id = 0, cookie = 0;
    void *view = NULL;
    NTSTATUS status;

    if (surface->handoff->release_pending) return FALSE;
    if (surface->handoff->view)
    {
        if (!__atomic_load_n( &surface->handoff->channel->closed, __ATOMIC_ACQUIRE )) return TRUE;
        /* Retire a closed binding even when its consumer is still finishing
         * a cache copy. The retirement object retains that mapping and its
         * source images until the actual read completes. */
        client_surface_release_handoff( surface );
        if (surface->handoff->view) return FALSE;
    }
    SERVER_START_REQ( get_client_surface_handoff )
    {
        req->handle = wine_server_user_handle( surface->hwnd );
        req->producer = 0;
        req->surface = client_surface_get_identity( surface );
        req->owner = 0;
        status = wine_server_call( req );
        if (!status)
        {
            mapping = wine_server_ptr_handle( reply->mapping );
            size = reply->size;
            offset = reply->offset;
            mapping_id = reply->mapping_id;
            cookie = reply->cookie;
        }
    }
    SERVER_END_REQ;
    if (status)
    {
        TRACE( "failed to map handoff identity %s, status %#lx\n",
               wine_dbgstr_longlong( client_surface_get_identity( surface ) ), (unsigned long)status );
        return FALSE;
    }
    status = NtMapViewOfSection( mapping, NtCurrentProcess(), &view, 0, 0, NULL,
                                 &size, ViewShare, 0, PAGE_READWRITE );
    NtClose( mapping );
    if (status) goto release;
    shared = view;
    if (size < sizeof(*shared) || offset < offsetof(struct client_surface_handoff_shared, channels) ||
        offset > sizeof(*shared) - sizeof(*channel) ||
        (offset - offsetof(struct client_surface_handoff_shared, channels)) % sizeof(*channel) ||
        __atomic_load_n( &shared->magic, __ATOMIC_ACQUIRE ) != CLIENT_SURFACE_HANDOFF_MAGIC ||
        shared->version != CLIENT_SURFACE_HANDOFF_VERSION ||
        shared->channel_count != CLIENT_SURFACE_HANDOFF_CHANNELS || shared->mapping_id != mapping_id)
        goto failed;
    channel = (struct client_surface_handoff_channel *)((char *)view + offset);
    if (channel->cookie != cookie || channel->identity != client_surface_get_identity( surface ) ||
        __atomic_load_n( &channel->closed, __ATOMIC_ACQUIRE )) goto failed;
    SERVER_START_REQ( get_client_surface_handoff_event )
    {
        req->handle = wine_server_user_handle( surface->hwnd );
        req->producer = 0;
        req->surface = client_surface_get_identity( surface );
        req->cookie = cookie;
        req->owner = 0;
        status = wine_server_call( req );
        if (!status) event = wine_server_ptr_handle( reply->event );
    }
    SERVER_END_REQ;
    if (status) goto failed;
    status = wine_server_handle_to_fd( event, FILE_WRITE_DATA, &surface->handoff->ready_fd, NULL );
    NtClose( event );
    if (status) goto failed;
    surface->handoff->view = view;
    surface->handoff->view_size = size;
    surface->handoff->shared = shared;
    surface->handoff->channel = channel;
    memset( surface->handoff->sources, 0, sizeof(surface->handoff->sources) );
    surface->handoff->next = 0;
    surface->handoff->mapping_id = mapping_id;
    surface->handoff->cookie = cookie;
    TRACE( "mapped handoff hwnd %p identity %s pool %s cookie %s\n", surface->hwnd,
           wine_dbgstr_longlong( client_surface_get_identity( surface ) ), wine_dbgstr_longlong( mapping_id ),
           wine_dbgstr_longlong( cookie ) );
    return TRUE;

failed:
    NtUnmapViewOfSection( NtCurrentProcess(), view );
release:
    SERVER_START_REQ( release_client_surface_handoff )
    {
        req->handle = 0;
        req->producer = 0;
        req->surface = client_surface_get_identity( surface );
        req->cookie = cookie;
        req->owner = 0;
        wine_server_call( req );
    }
    SERVER_END_REQ;
    return FALSE;
}

static BOOL acquire_client_surface_handoff( struct client_surface *surface,
                                            const struct client_surface_scene *scene, UINT64 *token,
                                            unsigned int *index_ret )
{
    DWORD start = NtGetTickCount();

    for (;;)
    {
        unsigned int pass, i;
        LONG sequence;
        BOOL available = FALSE;

        if (__atomic_load_n( &surface->handoff->channel->closed, __ATOMIC_ACQUIRE )) return FALSE;
        /* Native reservations are producer-private. Published storage remains
         * immutable until the consumer has finished its checked cache copy. */
        for (pass = 0; pass < 2; ++pass)
            for (i = 0; i < CLIENT_SURFACE_SOURCE_FRAME_COUNT; ++i)
            {
                unsigned int index = (surface->handoff->next + i) % CLIENT_SURFACE_SOURCE_FRAME_COUNT;
                struct client_surface_source *source = surface->handoff->sources + index;

                if (source->published)
                {
                    if (!client_surface_handoff_consumed( surface->handoff->channel, source->publication )) continue;
                    source->published = FALSE;
                }
                if (!pass && __atomic_load_n( &source->reservation, __ATOMIC_ACQUIRE )) continue;
                if (!++surface->handoff->serial) ++surface->handoff->serial;
                __atomic_store_n( &source->reservation, surface->handoff->serial, __ATOMIC_RELEASE );
                surface->handoff->next = (index + 1) % CLIENT_SURFACE_SOURCE_FRAME_COUNT;
                *index_ret = index;
                *token = surface->handoff->serial;
                return TRUE;
            }
        /* A target writer may need this mutex to move the producer to the
         * owner which can consume its images. Do not keep waiting on the old
         * binding after that writer or a new scene has invalidated this wait.
         * Retirement retains immutable images until their actual readers
         * finish; cancellation does not acknowledge an unfinished cache copy.
         * Independent snapshots need no published scene, but must yield to
         * a pending target change for the same reason. */
        if (InterlockedCompareExchange( &surface->target_update_waiters, 0, 0 ) ||
            InterlockedCompareExchange( &surface->target_update_pending, 0, 0 ) ||
            (scene && !client_surface_scene_current( scene )))
        {
            TRACE( "cancelling stale source wait identity %s cookie %s\n",
                   wine_dbgstr_longlong( client_surface_get_identity( surface ) ), wine_dbgstr_longlong( surface->handoff->cookie ) );
            return FALSE;
        }
        if (NtGetTickCount() - start >= CLIENT_SURFACE_PRESENT_TIMEOUT) return FALSE;
        __atomic_store_n( &surface->handoff->shared->release_parked, 1, __ATOMIC_RELEASE );
        sequence = __atomic_load_n( &surface->handoff->shared->release_sequence, __ATOMIC_ACQUIRE );
        for (i = 0; i < CLIENT_SURFACE_SOURCE_FRAME_COUNT; ++i)
        {
            const struct client_surface_source *source = surface->handoff->sources + i;

            available |= !source->published ||
                         client_surface_handoff_consumed( surface->handoff->channel, source->publication );
        }
        if (!available)
            client_surface_handoff_wait_sequence( &surface->handoff->shared->release_sequence, sequence, 10 );
    }
}

static BOOL prepare_client_surface_handoff_locked( struct client_surface *surface,
                                                   struct client_surface_frame *present, BOOL independent )
{
    struct client_surface_source *source;
    UINT64 token;

    if ((!independent && (present->target != CLIENT_SURFACE_FRAME_TARGET_OFFSCREEN ||
        (present->mode != CLIENT_SURFACE_PRESENTATION_COMPOSITED &&
         (present->mode != CLIENT_SURFACE_PRESENTATION_STAGED || !present->scene.generation)) ||
        !present->scene.valid)) ||
        !client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_GENERATION_HANDOFF ) ||
        !surface->backend->handoff_prepare)
    {
        TRACE( "handoff unavailable identity %s target %u mode %u generation %s valid %u cap %u prepare %p\n",
               wine_dbgstr_longlong( client_surface_get_identity( surface ) ), present->target, present->mode,
               wine_dbgstr_longlong( present->scene.generation ), present->scene.valid,
               client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_GENERATION_HANDOFF ),
               surface->backend->handoff_prepare );
        return FALSE;
    }
    if (!map_client_surface_handoff( surface )) return FALSE;
    if (!independent && (__atomic_load_n( &surface->handoff->channel->endpoints, __ATOMIC_ACQUIRE ) &
         CLIENT_SURFACE_HANDOFF_ENDPOINT_CONSUMER) == 0)
    {
        TRACE( "handoff identity %s has no compositor endpoint\n",
               wine_dbgstr_longlong( client_surface_get_identity( surface ) ) );
        return FALSE;
    }
    if (!acquire_client_surface_handoff( surface, independent ? NULL : &present->scene,
                                        &token, &present->handoff_index ))
    {
        client_surface_release_handoff( surface );
        return FALSE;
    }
    source = surface->handoff->sources + present->handoff_index;
    source->target_epoch = present->target_epoch;
    source->source = source->source_visual = 0;
    source->width = source->height = source->flags = 0;
    if (!surface->backend->handoff_prepare( surface, source, present->handoff_index ))
    {
        UINT64 expected = token;
        __atomic_compare_exchange_n( &source->reservation, &expected, 0, 0,
                                     __ATOMIC_RELEASE, __ATOMIC_RELAXED );
        return FALSE;
    }
    present->handoff_control = token;
    present->handoff_source = source;
    present->handoff_channel = surface->handoff->channel;
    return TRUE;
}

BOOL client_surface_prepare_handoff_locked( struct client_surface *surface,
                                            struct client_surface_frame *present )
{
    return prepare_client_surface_handoff_locked( surface, present, FALSE );
}

void client_surface_abandon_handoff_locked( struct client_surface *surface,
                                            struct client_surface_frame *present )
{
    UINT64 expected = present->handoff_control;

    if (!expected || !surface->handoff->channel) return;
    __atomic_compare_exchange_n( &surface->handoff->sources[present->handoff_index].reservation, &expected, 0, 0,
                                 __ATOMIC_RELEASE, __ATOMIC_RELAXED );
    present->handoff_control = 0;
}

static BOOL source_capture_current( struct client_surface *surface, struct client_surface_frame *present )
{
    struct client_surface_source *source = surface->handoff->sources + present->handoff_index;

    return surface->handoff->channel &&
           !__atomic_load_n( &surface->handoff->channel->closed, __ATOMIC_ACQUIRE ) &&
           present->handoff_control && present->result == CLIENT_SURFACE_FRAME_PENDING &&
           surface->hwnd && surface->target.valid && present->target_epoch == surface->target.epoch &&
           (surface->active || surface->server_cached) &&
           __atomic_load_n( &source->reservation, __ATOMIC_ACQUIRE ) == present->handoff_control &&
           (present->serial > surface->composed_serial ||
            (present->serial == surface->composed_serial && surface->content_valid));
}

BOOL client_surface_freeze_frame_locked( struct client_surface *surface,
                                         struct client_surface_frame *present,
                                         struct client_surface_completed_frame *frame )
{
    struct client_surface_source *source = surface->handoff->sources + present->handoff_index;
    struct client_surface_capture capture = {0};
    BOOL valid, pinned = FALSE;

    memset( frame, 0, sizeof(*frame) );
    pthread_mutex_lock( &surface->present_lock );
    valid = source_capture_current( surface, present );
    if (valid && present->capture.size.cx)
        valid = source->width == present->capture.size.cx && source->height == present->capture.size.cy;
    if (valid && surface->backend->handoff_capture)
        valid = surface->backend->handoff_capture( surface, present, &capture );
    if (valid && capture.read)
    {
        /* Cached replay has no host completion token of its own. Retain the
         * mapping and native-source submission order for every private read,
         * while allowing target writers to invalidate its eventual publication. */
        InterlockedIncrement( &surface->external_completion_count );
        pinned = TRUE;
        pthread_mutex_unlock( &surface->present_lock );
        pthread_mutex_unlock( &surface->completion_lock );
        valid = capture.read( capture.context );
        pthread_mutex_lock( &surface->completion_lock );
        pthread_mutex_lock( &surface->present_lock );
        if (!source_capture_current( surface, present ))
        {
            present->result = CLIENT_SURFACE_FRAME_SUPERSEDED;
            valid = FALSE;
        }
        if (valid && capture.apply) valid = capture.apply( capture.context, surface, present );
    }
    if (valid && surface->backend->handoff_complete)
        valid = surface->backend->handoff_complete( surface, source, present->handoff_index );
    if (valid)
        valid = source->source && source->width && source->height && (source->flags & CLIENT_SURFACE_HANDOFF_COPY_SOURCE);
    if (valid)
    {
        frame->surface_id = client_surface_get_identity( surface );
        frame->frame_id = present->serial;
        frame->target_epoch = present->target_epoch;
        frame->image = source->source;
        frame->visual = source->source_visual;
        frame->flags = source->flags;
        frame->size = (SIZE){source->width, source->height};
        SetRect( &frame->damage, 0, 0, source->width, source->height );
        if (present->damage_base_sequence && !IsRectEmpty( &present->damage ) &&
            present->damage.left >= 0 && present->damage.top >= 0 &&
            present->damage.right <= source->width && present->damage.bottom <= source->height)
        {
            frame->damage = present->damage;
            frame->damage_base_frame = present->damage_base_sequence;
        }
        surface->composed_serial = present->serial;
        InterlockedExchange( &surface->content_valid, TRUE );
    }
    /* Source captures release references only. Their admitted backend
     * retirement owns any native destruction after the last reference. */
    if (capture.release) capture.release( capture.context );
    if (pinned)
    {
        if (!InterlockedDecrement( &surface->external_completion_count ))
            pthread_cond_broadcast( &surface->completion_cond );
        client_surface_handoff_completed( surface );
    }
    pthread_mutex_unlock( &surface->present_lock );
    return valid;
}

static BOOL client_surface_handoff_is_visible( const struct client_surface *surface )
{
    BOOL visible = FALSE;

    /* Completion holds the surface locks. Query server-owned visibility only
     * before the first consumer binds, without taking the process USER lock
     * through IsWindowVisible/ancestor traversal. The normal channel path
     * remains free of this request. */
    SERVER_START_REQ( get_client_surface_handoff_visibility )
    {
        req->handle = wine_server_user_handle( surface->hwnd );
        req->surface = client_surface_get_identity( surface );
        req->cookie = surface->handoff->cookie;
        if (!wine_server_call( req )) visible = reply->visible;
    }
    SERVER_END_REQ;
    return visible;
}

BOOL client_surface_publish_handoff_locked( struct client_surface *surface,
                                            struct client_surface_frame *present,
                                            const struct client_surface_completed_frame *frame )
{
    struct client_surface_handoff_channel *channel = surface->handoff->channel;
    struct client_surface_source *source = surface->handoff->sources + present->handoff_index;
    struct client_surface_handoff_slot *slot;
    UINT64 produced, consumed;
    unsigned long long ready_time;
    ptrdiff_t index;
    BOOL valid;

    if (!present->handoff_control || !channel || !frame->image) return FALSE;
    pthread_mutex_lock( &surface->present_lock );
    produced = __atomic_load_n( &channel->producer_sequence, __ATOMIC_RELAXED );
    consumed = __atomic_load_n( &channel->consumer_sequence, __ATOMIC_ACQUIRE );
    valid = !__atomic_load_n( &channel->closed, __ATOMIC_ACQUIRE ) &&
            produced - consumed < CLIENT_SURFACE_HANDOFF_RING_SIZE &&
            frame->surface_id == client_surface_get_identity( surface ) && frame->frame_id == present->serial &&
            frame->target_epoch == present->target_epoch && frame->image == source->source &&
            frame->visual == source->source_visual && frame->size.cx == source->width &&
            frame->size.cy == source->height && frame->flags == source->flags &&
            (frame->flags & CLIENT_SURFACE_HANDOFF_COPY_SOURCE) && surface->hwnd && surface->target.valid &&
            __atomic_load_n( &source->reservation, __ATOMIC_ACQUIRE ) == present->handoff_control &&
            present->serial >= surface->composed_serial && present->target_epoch == surface->target.epoch &&
            client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_OWNER_SCENE_PLAN );
    /* A hidden producer without a consumer keeps its latest completed source
     * private instead of filling the ring with images no owner can consume.
     * A visible producer may finish before the asynchronous owner binds:
     * queue its exact READY image now, without requiring another application
     * present or a producer message pump. Registration scans the retained
     * ready bitmap even if this publication preceded the owner's event fd.
     * Visibility never returns storage: a concurrent hide leaves every READY
     * image immutable until checked consumption or channel retirement. */
    if (valid && !(__atomic_load_n( &channel->endpoints, __ATOMIC_ACQUIRE ) &
                   CLIENT_SURFACE_HANDOFF_ENDPOINT_CONSUMER) &&
        !client_surface_handoff_is_visible( surface ))
    {
        TRACE( "retaining source identity %s frame %s without compositor endpoint\n",
               wine_dbgstr_longlong( frame->surface_id ), wine_dbgstr_longlong( frame->frame_id ) );
        valid = FALSE;
    }
    if (valid) valid = !__atomic_load_n( &channel->closed, __ATOMIC_ACQUIRE );
    if (valid)
    {
        /* completion_lock is the single publication domain, including callers
         * whose native queue completions arrived out of submission order. Only
         * completed images advance this sequence; abandoned reservations do not. */
        slot = &channel->slots[produced & (CLIENT_SURFACE_HANDOFF_RING_SIZE - 1)];
        *slot = (struct client_surface_handoff_slot){
            .cookie = channel->cookie, .identity = frame->surface_id,
            .producer_process = channel->producer_process, .window = channel->window,
            .toplevel = channel->toplevel, .source = frame->image, .source_visual = frame->visual,
            .target_epoch = frame->target_epoch, .width = frame->size.cx, .height = frame->size.cy,
            .flags = frame->flags, .source_sequence = frame->frame_id,
            .damage = frame->damage, .damage_base_sequence = frame->damage_base_frame,
        };
        if (frame->damage_base_frame) slot->flags &= ~CLIENT_SURFACE_HANDOFF_FULL_DAMAGE;
        else slot->flags |= CLIENT_SURFACE_HANDOFF_FULL_DAMAGE;
        source->publication = produced + 1;
        source->published = TRUE;
        __atomic_store_n( &source->reservation, 0, __ATOMIC_RELEASE );
        ready_time = TRACE_ON(csperf) ? client_surface_perf_time() : 0;
        __atomic_store_n( &channel->producer_sequence, produced + 1, __ATOMIC_RELEASE );
        TRACE_(csperf)( "ticks=%llu event=ready identity=%s cookie=%s token=%s sequence=%s target_epoch=%s "
                       "image=%s width=%u height=%u flags=%x damage=%s damage_base=%s\n",
                       ready_time, wine_dbgstr_longlong( client_surface_get_identity( surface ) ),
                       wine_dbgstr_longlong( surface->handoff->cookie ),
                       wine_dbgstr_longlong( produced + 1 ), wine_dbgstr_longlong( frame->frame_id ),
                       wine_dbgstr_longlong( frame->target_epoch ), wine_dbgstr_longlong( frame->image ),
                       frame->size.cx, frame->size.cy, slot->flags, wine_dbgstr_rect( &slot->damage ),
                       wine_dbgstr_longlong( slot->damage_base_sequence ) );
        surface->composed_serial = present->serial;
        InterlockedExchange( &surface->content_valid, TRUE );
    }
    pthread_mutex_unlock( &surface->present_lock );
    if (!valid) return FALSE;
    index = channel - surface->handoff->shared->channels;
    __atomic_fetch_or( &surface->handoff->shared->ready_bitmap[index / 64],
                       (UINT64)1 << (index % 64), __ATOMIC_RELEASE );
    client_surface_handoff_wake_ready( surface );
    TRACE( "published handoff identity %s sequence %s channel %td\n",
           wine_dbgstr_longlong( client_surface_get_identity( surface ) ), wine_dbgstr_longlong( produced + 1 ), index );
    present->handoff_control = 0;
    return TRUE;
}

BOOL client_surface_handoff_write_available( const struct client_surface *surface )
{
    unsigned int i;

    if (!surface->handoff->channel ||
        __atomic_load_n( &surface->handoff->channel->closed, __ATOMIC_ACQUIRE )) return TRUE;
    for (i = 0; i < CLIENT_SURFACE_SOURCE_FRAME_COUNT; ++i)
    {
        const struct client_surface_source *source = surface->handoff->sources + i;

        if ((!source->published || client_surface_handoff_consumed( surface->handoff->channel, source->publication )) &&
            !__atomic_load_n( &source->reservation, __ATOMIC_ACQUIRE )) return TRUE;
    }
    return FALSE;
}

BOOL client_surface_prepare_source_locked( struct client_surface *surface,
                                            struct client_surface_frame *present )
{
    return prepare_client_surface_handoff_locked( surface, present, TRUE );
}

/* The caller holds completion_lock. Recheck submission/target intent after
 * this operation returns, because waiting temporarily drops that lock. */
void client_surface_handoff_wait( struct client_surface *surface )
{
    struct client_surface_handoff_shared *shared;
    BOOL wait;
    LONG sequence;

    /* Independent GPU writes only need one returned image, not a drain
     * of every completion. Wait on the owner's release notification with
     * the completion mutex dropped so those writes can be published.
     * Retain the view even if the final callback or a target update asks
     * to detach it while this thread sleeps on its shared sequence. */
    pthread_mutex_lock( &surface->present_lock );
    shared = surface->handoff->shared;
    ++surface->handoff->waiters;
    __atomic_store_n( &shared->release_parked, 1, __ATOMIC_RELEASE );
    sequence = __atomic_load_n( &shared->release_sequence, __ATOMIC_ACQUIRE );
    wait = !client_surface_handoff_write_available( surface );
    pthread_mutex_unlock( &surface->present_lock );
    if (wait)
    {
        pthread_mutex_unlock( &surface->completion_lock );
        client_surface_handoff_wait_sequence( &shared->release_sequence, sequence, 10 );
        pthread_mutex_lock( &surface->completion_lock );
    }
    pthread_mutex_lock( &surface->present_lock );
    if (!--surface->handoff->waiters && surface->handoff->release_pending &&
        !InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ))
        client_surface_release_handoff( surface );
    pthread_mutex_unlock( &surface->present_lock );
}

void client_surface_handoff_retire_closed( struct client_surface *surface )
{
    pthread_mutex_lock( &surface->present_lock );
    if (surface->handoff->channel &&
        __atomic_load_n( &surface->handoff->channel->closed, __ATOMIC_ACQUIRE ) &&
        !InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ))
        client_surface_release_handoff( surface );
    pthread_mutex_unlock( &surface->present_lock );
}

/* The caller removed a completion token while holding present_lock. */
void client_surface_handoff_completed( struct client_surface *surface )
{
    if (surface->handoff->release_pending &&
        !InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ))
        client_surface_release_handoff( surface );
    /* An abandoned image or the last completion can also satisfy the
     * source waiter, without an owner copy producing a release wake. */
    if (surface->handoff->waiters && (client_surface_handoff_write_available( surface ) ||
        !InterlockedCompareExchange( &surface->external_completion_count, 0, 0 )))
        client_surface_handoff_wake_release( surface->handoff->shared );
}

BOOL client_surface_handoff_valid( const struct client_surface *surface,
                                   const struct client_surface_frame *present )
{
    return !present->handoff_control ||
           (surface->handoff->channel &&
            !__atomic_load_n( &surface->handoff->channel->closed, __ATOMIC_ACQUIRE ) &&
            __atomic_load_n( &surface->handoff->sources[present->handoff_index].reservation,
                             __ATOMIC_ACQUIRE ) == present->handoff_control);
}

BOOL client_surface_handoff_has_source( const struct client_surface *surface )
{
    const struct client_surface_handoff_channel *channel = surface->handoff->channel;
    UINT64 produced;

    if (!client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_OWNER_SCENE_PLAN ) ||
        !channel || __atomic_load_n( &channel->closed, __ATOMIC_ACQUIRE ) ||
        !surface->content_valid || !surface->composed_serial) return FALSE;
    /* An open channel retains the owner's source cache independently of scene
     * placement. A closed/replaced binding must receive the completed image. */
    produced = __atomic_load_n( &channel->producer_sequence, __ATOMIC_RELAXED );
    return channel->slots[(produced - 1) & (CLIENT_SURFACE_HANDOFF_RING_SIZE - 1)].source_sequence ==
           surface->composed_serial;
}
