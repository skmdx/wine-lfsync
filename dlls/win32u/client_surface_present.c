/*
 * Client surface presentation transactions
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
#include <unistd.h>
#include <sys/socket.h>

#ifdef __linux__
#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "ntstatus.h"
#include "client_surface.h"
#include "ntuser_private.h"
#include "wine/server.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(win);
WINE_DECLARE_DEBUG_CHANNEL(csperf);

static unsigned long long client_surface_perf_time(void)
{
    LARGE_INTEGER counter;

    NtQueryPerformanceCounter( &counter, NULL );
    return counter.QuadPart;
}

static BOOL get_cached_client_surface_region( struct client_surface *surface, HWND hwnd,
                                              const RECT *monitor_rect,
                                              const struct client_surface_frame *present,
                                              HRGN *region );

static BOOL client_surface_backend_present( struct client_surface *surface,
                                            const struct client_surface_scene *scene,
                                            HDC hdc, HRGN surface_region,
                                            BOOL flush, BOOL defer_visible )
{
    return !surface->backend->present ||
           surface->backend->present( surface, scene, hdc, surface_region, flush, defer_visible );
}

static BOOL client_surface_backend_prepare_completion( struct client_surface *surface )
{
    return surface->backend->completion && surface->backend->completion->prepare( surface );
}

static BOOL client_surface_backend_wait_completion( struct client_surface *surface, DWORD timeout )
{
    assert( surface->backend->completion );
    return surface->backend->completion->wait( surface, timeout );
}

static void client_surface_backend_abandon_completion( struct client_surface *surface )
{
    if (surface->backend->completion && surface->backend->completion->abandon)
        surface->backend->completion->abandon( surface );
}

static void client_surface_handoff_wake_ready( struct client_surface *surface )
{
    struct client_surface_handoff_shared *shared = surface->handoff_shared;
    UINT64 value = 1;
    int ret;

    if (!__atomic_exchange_n( &shared->ready_parked, 0, __ATOMIC_ACQ_REL )) return;
    __atomic_add_fetch( &shared->ready_sequence, 1, __ATOMIC_RELEASE );
    do
#ifdef __linux__
        ret = write( surface->handoff_ready_fd, &value, sizeof(value) );
#else
        ret = send( surface->handoff_ready_fd, &value, sizeof(value), 0 );
#endif
    while (ret < 0 && errno == EINTR);
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

void client_surface_release_handoff( struct client_surface *surface )
{
    struct client_surface_handoff_slot *slot;
    DWORD start = NtGetTickCount();
    unsigned int i;

    if (!surface->handoff_view) return;
    /* Exact WSI completions may finish on a worker after a target update has
     * detached this mapping.  Keep the view and producer endpoint alive until
     * those frames have either published or abandoned their SUBMITTED token. */
    if (InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ))
    {
        surface->handoff_release_pending = TRUE;
        return;
    }
    for (i = 0; i < CLIENT_SURFACE_SOURCE_FRAME_COUNT; ++i)
    {
        slot = surface->handoff_slot + i;
        for (;;)
        {
            UINT64 control = __atomic_load_n( &slot->control, __ATOMIC_ACQUIRE );
            enum client_surface_handoff_state state = client_surface_handoff_state( control );
            LONG sequence;

            if (state == CLIENT_SURFACE_HANDOFF_FREE || state == CLIENT_SURFACE_HANDOFF_RELEASED ||
                state == CLIENT_SURFACE_HANDOFF_LOST)
                break;
            if (NtGetTickCount() - start >= CLIENT_SURFACE_PRESENT_TIMEOUT) break;
            __atomic_store_n( &surface->handoff_shared->release_parked, 1, __ATOMIC_RELEASE );
            sequence = __atomic_load_n( &surface->handoff_shared->release_sequence, __ATOMIC_ACQUIRE );
            control = __atomic_load_n( &slot->control, __ATOMIC_ACQUIRE );
            state = client_surface_handoff_state( control );
            if (state == CLIENT_SURFACE_HANDOFF_FREE || state == CLIENT_SURFACE_HANDOFF_RELEASED ||
                state == CLIENT_SURFACE_HANDOFF_LOST)
                continue;
            client_surface_handoff_wait_sequence( &surface->handoff_shared->release_sequence,
                                                  sequence, 10 );
        }
    }
    SERVER_START_REQ( release_client_surface_handoff )
    {
        req->handle = 0;
        req->producer = 0;
        req->surface = surface->identity;
        req->cookie = surface->handoff_cookie;
        req->owner = 0;
        wine_server_call( req );
    }
    SERVER_END_REQ;
    NtUnmapViewOfSection( NtCurrentProcess(), surface->handoff_view );
    if (surface->handoff_ready_fd >= 0) close( surface->handoff_ready_fd );
    surface->handoff_ready_fd = -1;
    surface->handoff_view = NULL;
    surface->handoff_view_size = 0;
    surface->handoff_shared = NULL;
    surface->handoff_slot = NULL;
    surface->handoff_mapping_id = 0;
    surface->handoff_cookie = 0;
    surface->handoff_release_pending = FALSE;
}

static BOOL map_client_surface_handoff( struct client_surface *surface )
{
    struct client_surface_handoff_shared *shared;
    struct client_surface_handoff_slot *slot;
    HANDLE mapping = NULL, event = NULL;
    SIZE_T offset = 0, size = 0;
    UINT64 mapping_id = 0, cookie = 0;
    void *view = NULL;
    NTSTATUS status;
    unsigned int i;

    if (surface->handoff_release_pending) return FALSE;
    if (surface->handoff_view)
    {
        for (i = 0; i < CLIENT_SURFACE_SOURCE_FRAME_COUNT; ++i)
            if (client_surface_handoff_state( __atomic_load_n( &surface->handoff_slot[i].control,
                                                              __ATOMIC_ACQUIRE ) ) == CLIENT_SURFACE_HANDOFF_LOST)
                break;
        if (i == CLIENT_SURFACE_SOURCE_FRAME_COUNT) return TRUE;
        /* A lost owner binding has already dropped its consumer endpoint.
         * Retire our view before testing endpoints, or that test prevents us
         * from ever reaching acquire() and releasing the obsolete mapping. */
        client_surface_release_handoff( surface );
        if (surface->handoff_view) return FALSE;
    }
    SERVER_START_REQ( get_client_surface_handoff )
    {
        req->handle = wine_server_user_handle( surface->hwnd );
        req->producer = 0;
        req->surface = surface->identity;
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
               wine_dbgstr_longlong( surface->identity ), (unsigned long)status );
        return FALSE;
    }
    status = NtMapViewOfSection( mapping, NtCurrentProcess(), &view, 0, 0, NULL,
                                 &size, ViewShare, 0, PAGE_READWRITE );
    NtClose( mapping );
    if (status) goto release;
    shared = view;
    if (size < sizeof(*shared) || offset > size - CLIENT_SURFACE_SOURCE_FRAME_COUNT * sizeof(*slot) ||
        __atomic_load_n( &shared->magic, __ATOMIC_ACQUIRE ) != CLIENT_SURFACE_HANDOFF_MAGIC ||
        shared->version != CLIENT_SURFACE_HANDOFF_VERSION ||
        shared->slot_count != CLIENT_SURFACE_HANDOFF_SLOTS || shared->mapping_id != mapping_id)
        goto failed;
    slot = (struct client_surface_handoff_slot *)((char *)view + offset);
    for (unsigned int i = 0; i < CLIENT_SURFACE_SOURCE_FRAME_COUNT; ++i)
        if (slot[i].cookie != cookie || slot[i].identity != surface->identity) goto failed;
    SERVER_START_REQ( get_client_surface_handoff_event )
    {
        req->handle = wine_server_user_handle( surface->hwnd );
        req->producer = 0;
        req->surface = surface->identity;
        req->cookie = cookie;
        req->owner = 0;
        status = wine_server_call( req );
        if (!status) event = wine_server_ptr_handle( reply->event );
    }
    SERVER_END_REQ;
    if (status) goto failed;
    status = wine_server_handle_to_fd( event, FILE_WRITE_DATA, &surface->handoff_ready_fd, NULL );
    NtClose( event );
    if (status) goto failed;
    surface->handoff_view = view;
    surface->handoff_view_size = size;
    surface->handoff_shared = shared;
    surface->handoff_slot = slot;
    surface->next_handoff = 0;
    surface->handoff_mapping_id = mapping_id;
    surface->handoff_cookie = cookie;
    TRACE( "mapped handoff hwnd %p identity %s pool %s cookie %s\n", surface->hwnd,
           wine_dbgstr_longlong( surface->identity ), wine_dbgstr_longlong( mapping_id ),
           wine_dbgstr_longlong( cookie ) );
    return TRUE;

failed:
    NtUnmapViewOfSection( NtCurrentProcess(), view );
release:
    SERVER_START_REQ( release_client_surface_handoff )
    {
        req->handle = 0;
        req->producer = 0;
        req->surface = surface->identity;
        req->cookie = cookie;
        req->owner = 0;
        wine_server_call( req );
    }
    SERVER_END_REQ;
    return FALSE;
}

static BOOL acquire_client_surface_handoff( struct client_surface *surface, UINT64 *token,
                                            unsigned int *index_ret )
{
    DWORD start = NtGetTickCount();

    for (;;)
    {
        unsigned int pass, i;
        LONG sequence;
        BOOL available = FALSE;

        /* Prefer returned storage. Only when both images are occupied may an
         * unsubmitted private generation be superseded; READY and READING
         * images are immutable until the owner returns that particular slot. */
        for (pass = 0; pass < 2; ++pass)
            for (i = 0; i < CLIENT_SURFACE_SOURCE_FRAME_COUNT; ++i)
            {
                unsigned int index = (surface->next_handoff + i) % CLIENT_SURFACE_SOURCE_FRAME_COUNT;
                struct client_surface_handoff_slot *slot = surface->handoff_slot + index;
                UINT64 control = __atomic_load_n( &slot->control, __ATOMIC_ACQUIRE );
                UINT64 generation = client_surface_handoff_generation( control ), submitted;
                enum client_surface_handoff_state state = client_surface_handoff_state( control );

                if (state == CLIENT_SURFACE_HANDOFF_LOST) return FALSE;
                if (state != CLIENT_SURFACE_HANDOFF_FREE && state != CLIENT_SURFACE_HANDOFF_RELEASED &&
                    (state != CLIENT_SURFACE_HANDOFF_SUBMITTED || !pass)) continue;
                if (state != CLIENT_SURFACE_HANDOFF_FREE)
                    generation = client_surface_handoff_next_generation( generation );
                submitted = client_surface_handoff_control( generation, CLIENT_SURFACE_HANDOFF_SUBMITTED );
                if (!__atomic_compare_exchange_n( &slot->control, &control, submitted, 0,
                                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE )) continue;
                surface->next_handoff = (index + 1) % CLIENT_SURFACE_SOURCE_FRAME_COUNT;
                *index_ret = index;
                *token = submitted;
                return TRUE;
            }
        if (NtGetTickCount() - start >= CLIENT_SURFACE_PRESENT_TIMEOUT) return FALSE;
        __atomic_store_n( &surface->handoff_shared->release_parked, 1, __ATOMIC_RELEASE );
        sequence = __atomic_load_n( &surface->handoff_shared->release_sequence, __ATOMIC_ACQUIRE );
        for (i = 0; i < CLIENT_SURFACE_SOURCE_FRAME_COUNT; ++i)
        {
            enum client_surface_handoff_state state = client_surface_handoff_state(
                __atomic_load_n( &surface->handoff_slot[i].control, __ATOMIC_ACQUIRE ) );

            available |= state != CLIENT_SURFACE_HANDOFF_READY && state != CLIENT_SURFACE_HANDOFF_READING;
        }
        if (!available)
            client_surface_handoff_wait_sequence( &surface->handoff_shared->release_sequence, sequence, 10 );
    }
}

static BOOL prepare_client_surface_handoff_locked( struct client_surface *surface,
                                                   struct client_surface_frame *present, BOOL independent )
{
    struct client_surface_handoff_slot *slot;
    HRGN surface_region = 0;
    UINT64 token;

    if ((!independent && (present->target != CLIENT_SURFACE_FRAME_TARGET_OFFSCREEN ||
        (present->mode != CLIENT_SURFACE_PRESENTATION_COMPOSITED &&
         (present->mode != CLIENT_SURFACE_PRESENTATION_STAGED || !present->scene.generation)) ||
        !present->scene.valid)) ||
        !client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_GENERATION_HANDOFF ) ||
        !surface->backend->handoff_prepare)
    {
        TRACE( "handoff unavailable identity %s target %u mode %u generation %s valid %u cap %u prepare %p\n",
               wine_dbgstr_longlong( surface->identity ), present->target, present->mode,
               wine_dbgstr_longlong( present->scene.generation ), present->scene.valid,
               client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_GENERATION_HANDOFF ),
               surface->backend->handoff_prepare );
        return FALSE;
    }
    if (!client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_OWNER_SCENE_PLAN ) &&
        !get_cached_client_surface_region( surface, surface->hwnd,
                                           &surface->target.monitor_rect,
                                           present, &surface_region ))
        return FALSE;
    if (!map_client_surface_handoff( surface )) return FALSE;
    if (!independent && (__atomic_load_n( &surface->handoff_slot->endpoints, __ATOMIC_ACQUIRE ) &
         CLIENT_SURFACE_HANDOFF_ENDPOINT_CONSUMER) == 0)
    {
        TRACE( "handoff identity %s has no compositor endpoint\n",
               wine_dbgstr_longlong( surface->identity ) );
        return FALSE;
    }
    if (!acquire_client_surface_handoff( surface, &token, &present->handoff_index ))
    {
        client_surface_release_handoff( surface );
        return FALSE;
    }
    slot = surface->handoff_slot + present->handoff_index;
    slot->scene_epoch = present->scene.epoch;
    slot->scene_generation = present->scene.generation;
    slot->target_seq = present->target_seq;
    /* Process and window identities belong to the server-allocated binding,
     * not the submitting thread (cached replay may run on a Unix worker). */
    if (!surface->backend->handoff_prepare( surface, slot, surface_region ))
    {
        UINT64 expected = token;
        UINT64 free = client_surface_handoff_control(
            client_surface_handoff_generation( token ), CLIENT_SURFACE_HANDOFF_FREE );

        __atomic_compare_exchange_n( &slot->control, &expected, free, 0,
                                     __ATOMIC_RELEASE, __ATOMIC_RELAXED );
        return FALSE;
    }
    if (independent) slot->flags |= CLIENT_SURFACE_HANDOFF_INDEPENDENT;
    present->handoff_control = token;
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
    UINT64 expected, free;

    if (!present->handoff_control || !surface->handoff_slot) return;
    expected = present->handoff_control;
    free = client_surface_handoff_control(
        client_surface_handoff_generation( expected ), CLIENT_SURFACE_HANDOFF_FREE );
    __atomic_compare_exchange_n( &surface->handoff_slot[present->handoff_index].control, &expected, free, 0,
                                 __ATOMIC_RELEASE, __ATOMIC_RELAXED );
    present->handoff_control = 0;
}

BOOL client_surface_publish_handoff_locked( struct client_surface *surface,
                                            struct client_surface_frame *present, BOOL source_frozen )
{
    struct client_surface_handoff_slot *slot = surface->handoff_slot ?
        surface->handoff_slot + present->handoff_index : NULL;
    struct client_surface_scene current = {0};
    UINT64 expected = present->handoff_control;
    UINT64 ready;
    unsigned long long ready_time;
    ptrdiff_t index;
    BOOL valid;

    if (!expected || !slot) return FALSE;
    pthread_mutex_lock( &surface->present_lock );
    valid = slot->source && surface->hwnd && surface->target.valid &&
            __atomic_load_n( &slot->control, __ATOMIC_ACQUIRE ) == expected &&
            present->serial >= surface->composed_serial &&
            present->target_seq == surface->target.seq;
    if (slot->flags & CLIENT_SURFACE_HANDOFF_INDEPENDENT)
        valid = valid && source_frozen && (slot->flags & CLIENT_SURFACE_HANDOFF_COPY_SOURCE) &&
                client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_OWNER_SCENE_PLAN );
    else
        valid = valid && present->scene.valid &&
            slot->scene_epoch == present->scene.epoch &&
            slot->scene_generation == present->scene.generation &&
            client_surface_get_scene( surface, &current ) && current.valid &&
            current.toplevel == present->scene.toplevel &&
            current.epoch == present->scene.epoch && current.mode == present->scene.mode &&
            current.authoritative &&
            (current.generation == present->scene.generation ||
             (present->scene.generation && !current.generation));
    if (valid && !source_frozen && surface->backend->handoff_complete)
        valid = surface->backend->handoff_complete( surface, slot );
    if (valid)
    {
        /* A producer can submit the next frame while the compositor is still
         * exposing the preceding topology generation.  Once that publication
         * completes, the topology epoch and target remain valid but the shared
         * transaction generation becomes zero.  Retarget this already-complete
         * source to the steady owner path instead of falling back to the legacy
         * per-frame transaction and starting the same scene again. */
        if (!(slot->flags & CLIENT_SURFACE_HANDOFF_INDEPENDENT))
            slot->scene_generation = current.generation;
        slot->source_sequence = present->serial ? present->serial : surface->composed_serial;
        slot->damage_base_sequence = 0;
        if (present->damage_base_sequence && !IsRectEmpty( &present->damage ) &&
            present->damage.left >= 0 && present->damage.top >= 0 &&
            present->damage.right <= slot->width && present->damage.bottom <= slot->height)
        {
            slot->flags &= ~CLIENT_SURFACE_HANDOFF_FULL_DAMAGE;
            slot->damage = present->damage;
            slot->damage_base_sequence = present->damage_base_sequence;
        }
        ready = client_surface_handoff_control( client_surface_handoff_generation( expected ),
                                                CLIENT_SURFACE_HANDOFF_READY );
        ready_time = TRACE_ON(csperf) ? client_surface_perf_time() : 0;
        valid = __atomic_compare_exchange_n( &slot->control, &expected, ready, 0,
                                             __ATOMIC_RELEASE, __ATOMIC_ACQUIRE );
        if (valid)
        {
            TRACE_(csperf)( "ticks=%llu event=ready identity=%s cookie=%s token=%s sequence=%s\n",
                           ready_time, wine_dbgstr_longlong( surface->identity ),
                           wine_dbgstr_longlong( surface->handoff_cookie ),
                           wine_dbgstr_longlong( client_surface_handoff_generation( ready ) ),
                           wine_dbgstr_longlong( slot->source_sequence ) );
            surface->composed_serial = present->serial;
            InterlockedExchange( &surface->content_valid, TRUE );
        }
        else if (client_surface_handoff_generation( expected ) !=
                     client_surface_handoff_generation( present->handoff_control ) &&
                 client_surface_handoff_state( expected ) != CLIENT_SURFACE_HANDOFF_LOST)
        {
            /* A newer native submission owns this producer-private slot. Its
             * completion is the only one allowed to publish the shared source. */
            present->result = CLIENT_SURFACE_FRAME_SUPERSEDED;
        }
    }
    pthread_mutex_unlock( &surface->present_lock );
    if (!valid)
    {
        if (present->result == CLIENT_SURFACE_FRAME_SUPERSEDED)
            TRACE( "superseded completed handoff identity %s token %s control %s\n",
                   wine_dbgstr_longlong( surface->identity ),
                   wine_dbgstr_longlong( present->handoff_control ),
                   wine_dbgstr_longlong( expected ) );
        else
            TRACE( "rejected handoff identity %s token %s control %s hwnd %p target %s/%s "
                   "scene %u/%s/%s/%u current %u/%s/%s/%u\n",
                   wine_dbgstr_longlong( surface->identity ),
                   wine_dbgstr_longlong( present->handoff_control ),
                   wine_dbgstr_longlong( __atomic_load_n( &slot->control, __ATOMIC_ACQUIRE ) ),
                   surface->hwnd, wine_dbgstr_longlong( present->target_seq ),
                   wine_dbgstr_longlong( surface->target.seq ),
                   present->scene.valid, wine_dbgstr_longlong( present->scene.generation ),
                   wine_dbgstr_longlong( present->scene.epoch ), present->scene.mode,
                   current.valid, wine_dbgstr_longlong( current.generation ),
                   wine_dbgstr_longlong( current.epoch ), current.mode );
        return FALSE;
    }
    index = slot - surface->handoff_shared->slots;
    __atomic_fetch_or( &surface->handoff_shared->ready_bitmap[index / 64],
                       (UINT64)1 << (index % 64), __ATOMIC_RELEASE );
    client_surface_handoff_wake_ready( surface );
    TRACE( "published handoff identity %s generation %s slot %td\n",
           wine_dbgstr_longlong( surface->identity ),
           wine_dbgstr_longlong( client_surface_handoff_generation( ready ) ), index );
    present->handoff_control = 0;
    return TRUE;
}

static BOOL publish_client_surface_independent_source_locked( struct client_surface *surface,
                                                               struct client_surface_frame *present )
{
    BOOL prepared = FALSE;

    /* Source completion is independent of the GUI thread preparing a scene.
     * Freeze into shared storage now: the application may never repaint or
     * dispatch another message. The owner chooses placement when its current
     * scene is ready, and only after validating the binding and exact extent. */
    pthread_mutex_lock( &surface->present_lock );
    if (client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_OWNER_SCENE_PLAN ) &&
        surface->hwnd && surface->target.valid && surface->target.offscreen &&
        (InterlockedCompareExchange( &surface->active, 0, 0 ) ||
         InterlockedCompareExchange( &surface->server_cached, 0, 0 )) &&
        NtUserIsWindowVisible( surface->hwnd ) &&
        present->serial == surface->composed_serial && surface->content_valid &&
        present->source_size.cx == surface->target.virtual_rect.right - surface->target.virtual_rect.left &&
        present->source_size.cy == surface->target.virtual_rect.bottom - surface->target.virtual_rect.top)
    {
        client_surface_abandon_handoff_locked( surface, present );
        present->target_seq = surface->target.seq;
        prepared = prepare_client_surface_handoff_locked( surface, present, TRUE );
        if (prepared && surface->backend->handoff_complete)
            prepared = surface->backend->handoff_complete( surface,
                &surface->handoff_slot[present->handoff_index] );
    }
    pthread_mutex_unlock( &surface->present_lock );
    if (prepared && client_surface_publish_handoff_locked( surface, present, TRUE )) return TRUE;
    client_surface_abandon_handoff_locked( surface, present );
    return FALSE;
}

static BOOL begin_client_surface_composition( HWND hwnd, const struct client_surface *surface,
                                              const struct client_surface_frame *present,
                                              BOOL *valid )
{
    BOOL compose = FALSE;

    *valid = FALSE;
    SERVER_START_REQ( set_client_surface_state )
    {
        req->handle = wine_server_user_handle( hwnd );
        req->surface = surface->identity;
        req->flags = CLIENT_SURFACE_STATE_PRESENT_BEGIN;
        req->generation = present->scene.generation;
        req->scene_generation = present->scene.epoch;
        if (!wine_server_call( req ))
        {
            compose = reply->compose;
            *valid = TRUE;
        }
    }
    SERVER_END_REQ;
    return compose;
}

struct client_surface_clip_snapshot
{
    struct client_surface_clip_window *windows;
    UINT count;
    UINT size;
};

static BOOL get_client_surface_clip_snapshot( HWND hwnd, const struct ratio *raw_dpi,
                                              const struct client_surface_frame *present,
                                              struct client_surface_clip_snapshot *snapshot )
{
    NTSTATUS status;

    snapshot->size = 8;
    for (;;)
    {
        struct client_surface_clip_window *windows =
            realloc( snapshot->windows, snapshot->size * sizeof(*windows) );
        HWND toplevel = 0;
        UINT64 scene_generation = 0;
        UINT count = 0;
        data_size_t reply_size = 0;

        if (!windows) return FALSE;
        snapshot->windows = windows;

        SERVER_START_REQ( get_client_surface_clip_windows )
        {
            req->handle = wine_server_user_handle( hwnd );
            req->dpi = *raw_dpi;
            wine_server_set_reply( req, snapshot->windows,
                                   snapshot->size * sizeof(*snapshot->windows) );
            status = wine_server_call( req );
            if (!status)
            {
                count = reply->count;
                toplevel = wine_server_ptr_handle( reply->toplevel );
                scene_generation = reply->scene_generation;
                reply_size = wine_server_reply_size( reply );
            }
        }
        SERVER_END_REQ;
        if (status) return FALSE;
        if (count > snapshot->size)
        {
            snapshot->size = count;
            continue;
        }

        snapshot->count = count;
        return reply_size == count * sizeof(*snapshot->windows) &&
               present->scene.valid && toplevel == present->scene.toplevel &&
               !(scene_generation & 1) && scene_generation == present->scene.epoch;
    }
}

static void release_client_surface_clip_snapshot( struct client_surface_clip_snapshot *snapshot )
{
    free( snapshot->windows );
}

static BOOL get_client_surface_region( const RECT *monitor_rect,
                                       const struct client_surface_clip_snapshot *snapshot,
                                       HRGN *region )
{
    RGNDATA *data;
    HRGN clips;
    SIZE_T size;
    UINT i;

    if (!snapshot->count) return TRUE;
    if (snapshot->count > (MAXDWORD - FIELD_OFFSET( RGNDATA, Buffer )) / sizeof(RECT)) return FALSE;
    size = FIELD_OFFSET( RGNDATA, Buffer ) + snapshot->count * sizeof(RECT);
    if (!(data = malloc( size ))) return FALSE;

    data->rdh.dwSize = sizeof(data->rdh);
    data->rdh.iType = RDH_RECTANGLES;
    data->rdh.nCount = snapshot->count;
    data->rdh.nRgnSize = snapshot->count * sizeof(RECT);
    SetRectEmpty( &data->rdh.rcBound );
    for (i = 0; i < snapshot->count; ++i)
        ((RECT *)data->Buffer)[i] = wine_server_get_rect( snapshot->windows[i].rect );

    clips = NtGdiExtCreateRegion( NULL, size, data );
    free( data );
    if (!clips) return FALSE;
    if (!(*region = NtGdiCreateRectRgn( monitor_rect->left, monitor_rect->top,
                                       monitor_rect->right, monitor_rect->bottom )) ||
        NtGdiCombineRgn( *region, *region, clips, RGN_DIFF ) == ERROR)
    {
        if (*region) NtGdiDeleteObjectApp( *region );
        *region = 0;
        NtGdiDeleteObjectApp( clips );
        return FALSE;
    }
    /* The server snapshot is relative to the owner window, while backend
     * presentation regions use the client surface as their origin. */
    NtGdiOffsetRgn( *region, -monitor_rect->left, -monitor_rect->top );
    NtGdiDeleteObjectApp( clips );
    return TRUE;
}

/* Construct an owner-side plan member from authoritative window geometry and
 * occlusion. The caller validates the epoch again after collecting the roster;
 * no producer slot contributes placement or clipping to this snapshot. */
BOOL client_surface_get_scene_member( HWND toplevel, HWND hwnd, UINT64 epoch,
                                      struct client_surface_target *target, HRGN *region )
{
    struct client_surface_clip_snapshot snapshot = {0};
    struct client_surface_frame present = {0};
    struct ratio dpi;
    BOOL ret;

    *region = 0;
    memset( target, 0, sizeof(*target) );
    if ((epoch & 1) || !get_client_surface_rects( toplevel, hwnd, target )) return FALSE;
    present.scene.toplevel = toplevel;
    present.scene.epoch = epoch;
    present.scene.valid = TRUE;
    dpi = (struct ratio){target->dpi_num, target->dpi_den};
    ret = get_client_surface_clip_snapshot( hwnd, &dpi, &present, &snapshot );
    if (ret) ret = get_client_surface_region( &target->monitor_rect, &snapshot, region );
    release_client_surface_clip_snapshot( &snapshot );
    return ret;
}

/* Cross-process clipping changes only with the server-owned scene sequence.
 * Keep the derived region on the surface so steady-state presents avoid a
 * server round trip, heap allocation, and O(occluders) region reconstruction. */
static BOOL get_cached_client_surface_region( struct client_surface *surface, HWND hwnd,
                                              const RECT *monitor_rect,
                                              const struct client_surface_frame *present,
                                              HRGN *region )
{
    struct client_surface_clip_snapshot snapshot = {0};
    struct ratio raw_dpi = {surface->target.dpi_num, surface->target.dpi_den};
    HRGN new_region = 0;
    BOOL valid;

    if (!raw_dpi.num || !raw_dpi.den || !present->scene.valid) return FALSE;

    if (surface->clip_region_valid &&
        surface->clip_scene_epoch == present->scene.epoch &&
        surface->clip_target_seq == present->target_seq)
    {
        *region = surface->clip_region;
        return TRUE;
    }

    valid = get_client_surface_clip_snapshot( hwnd, &raw_dpi, present, &snapshot );
    if (valid) valid = get_client_surface_region( monitor_rect, &snapshot, &new_region );
    release_client_surface_clip_snapshot( &snapshot );
    if (!valid)
    {
        if (new_region) NtGdiDeleteObjectApp( new_region );
        return FALSE;
    }

    if (surface->clip_region) NtGdiDeleteObjectApp( surface->clip_region );
    surface->clip_scene_epoch = present->scene.epoch;
    surface->clip_target_seq = present->target_seq;
    surface->clip_region = new_region;
    surface->clip_region_valid = TRUE;
    *region = new_region;
    return TRUE;
}

static BOOL client_surface_validate_size_locked( struct client_surface *surface,
                                                 const SIZE *expected_size )
{
    if (expected_size &&
        (surface->target.virtual_rect.right - surface->target.virtual_rect.left != expected_size->cx ||
         surface->target.virtual_rect.bottom - surface->target.virtual_rect.top != expected_size->cy))
    {
        WARN( "not composing %s size %dx%d for expected frame %dx%d\n",
              debugstr_client_surface( surface ),
              surface->target.virtual_rect.right - surface->target.virtual_rect.left,
              surface->target.virtual_rect.bottom - surface->target.virtual_rect.top,
              (int)expected_size->cx, (int)expected_size->cy );
        return FALSE;
    }
    return TRUE;
}

static BOOL claim_client_surface_retry( struct client_surface *surface, UINT64 generation )
{
    LONG64 current;

    if (!generation) return FALSE;
    for (;;)
    {
        current = ReadAcquire64( &surface->scene_retry_generation );
        if ((UINT64)current >= generation) return FALSE;
        if (InterlockedCompareExchange64( &surface->scene_retry_generation,
                                          (LONG64)generation, current ) == current)
            return TRUE;
    }
}

BOOL client_surface_end_present_internal( struct client_surface *surface,
                                          const SIZE *expected_size, BOOL new_content,
                                          struct client_surface_frame *present )
{
    HWND hwnd = 0, toplevel = 0;
    RECT monitor_rect = {0};
    HRGN surface_region = 0;
    BOOL commit = FALSE, compose = FALSE, composed = FALSE, copied = FALSE, offscreen = FALSE;
    BOOL region_valid = TRUE, sync = !!present->scene.generation, wake = FALSE;
    BOOL authorized = present->scene.authoritative;
    BOOL begin_valid = TRUE, composition_retry = FALSE;
    BOOL scene_retry = FALSE, source_valid = FALSE;
    HDC hdc = 0;

    assert( present );
    /* The caller owns a surface reference and completion_lock.  present_lock
     * serializes detach, membership transitions and native target changes, so
     * the process-wide registry lock is neither needed for lifetime nor for
     * target validation on the per-frame path. */
    pthread_mutex_lock( &surface->present_lock );
    if (present->target == CLIENT_SURFACE_FRAME_TARGET_INVALID ||
        present->target_seq != surface->target.seq ||
        present->scene.toplevel != surface->target.toplevel)
    {
        TRACE( "discarding %s presentation across target state change\n",
               debugstr_client_surface( surface ) );
    }
    else if (new_content && present->serial <= surface->composed_serial)
    {
        present->result = CLIENT_SURFACE_FRAME_SUPERSEDED;
        TRACE( "discarding superseded presentation %s serial %s, composed %s\n",
               debugstr_client_surface( surface ), wine_dbgstr_longlong( present->serial ),
               wine_dbgstr_longlong( surface->composed_serial ) );
    }
    else if ((hwnd = surface->hwnd) &&
             surface->target.valid &&
             (InterlockedCompareExchange( &surface->active, 0, 0 ) ||
              InterlockedCompareExchange( &surface->server_cached, 0, 0 )))
    {
        if (sync) TRACE( "client surface %p starts composition epoch commit\n", hwnd );
        if (new_content || InterlockedCompareExchange( &surface->content_valid, 0, 0 ))
        {
            compose = client_surface_validate_size_locked( surface, expected_size );
            monitor_rect = surface->target.monitor_rect;
            offscreen = surface->target.offscreen;
        }
        else
            TRACE( "not recomposing incomplete cached content for %s\n",
                   debugstr_client_surface( surface ) );
    }
    source_valid = compose && new_content;
    if (compose && offscreen && !present->scene.valid) compose = FALSE;
    if (compose && offscreen &&
        client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_OWNER_COMPOSITOR ))
    {
        /* This backend never submits work to an owner-owned native target.
         * Hidden frames remain reusable source content; a visible handoff
         * failure is rejected and retried through the scene slow path. */
        if (!NtUserIsWindowVisible( hwnd ))
            composed = client_surface_scene_current( &present->scene );
        else
            scene_retry = TRUE;
        compose = FALSE;
    }
    if (compose && sync)
    {
        authorized = begin_client_surface_composition( hwnd, surface, present, &begin_valid );
        if (!authorized)
        {
            /* Only the authoritative producer for an HWND may touch its
             * composition destination.  A still-current denial is therefore a
             * successful no-op; a stale denial is retried in the new scene. */
            composed = begin_valid && client_surface_scene_current( &present->scene );
            scene_retry = !composed;
            compose = FALSE;
        }
    }
    else if (compose && !authorized)
    {
        /* Steady-state presents also obey the server's active-over-cache
         * producer choice.  The shared identity is covered by the top-level
         * scene seqlock, so this adds no per-frame server round trip. */
        composed = begin_valid && client_surface_scene_current( &present->scene );
        scene_retry = !composed;
        compose = FALSE;
    }

    /* Fetch cross-process clipping in one server scene snapshot.  Monitor-DPI
     * conversion and DCE refresh remain outside surfaces_lock. */
    if (compose && offscreen)
    {
        region_valid = get_cached_client_surface_region( surface, hwnd, &monitor_rect,
                                                         present, &surface_region );
        if (!region_valid)
        {
            WARN( "failed to derive client surface clip state\n" );
            if (!client_surface_scene_current( &present->scene )) scene_retry = TRUE;
            compose = FALSE;
        }
        else
        {
            /* Local DCE invalidation already follows every scene mutation;
             * foreign HWNDs are refreshed unconditionally by NtUserGetDCEx.
             * Forcing another server fetch here made every local frame pay an
             * avoidable round trip despite a matching scene token. */
            /* Keep composition DCEs distinct from ordinary application DCs. */
            DWORD flags = DCX_CACHE | DCX_USESTYLE | WINE_DCX_CLIENT_SURFACE;

            /* A region-only backend leaves the borrowed DC unchanged. Avoid
             * resetting all GDI state (fonts, pens, mapping, driver objects)
             * on each frame just to query SYSRGN. Other backends retain the
             * normal reset contract. The private composition cache key still
             * separates this DCE from application drawing DCs. */
            if (client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_READ_ONLY_DC ))
                flags |= DCX_NORESETATTRS;
            hdc = NtUserGetDCEx( hwnd, 0, flags );
            if (!hdc)
            {
                WARN( "failed to acquire composition DC for %s\n", debugstr_client_surface( surface ) );
                compose = FALSE;
            }
        }
    }

    if (compose && offscreen && !client_surface_scene_current( &present->scene ))
    {
        TRACE( "discarding %s composition across scene change\n",
               debugstr_client_surface( surface ) );
        compose = FALSE;
        scene_retry = TRUE;
    }

    /* Driver composition can include an X round trip.  Serialize only this
     * surface while it runs, allowing independent surfaces to keep moving. */
    if (compose)
    {
        copied = client_surface_backend_present( surface, &present->scene, hdc, surface_region,
                                                 sync, sync );
        composed = copied;
    }
    if (copied && offscreen && !client_surface_scene_current( &present->scene ))
    {
        TRACE( "not committing %s composition invalidated while copying\n",
               debugstr_client_surface( surface ) );
        composed = FALSE;
        scene_retry = TRUE;
    }
    if (hdc) NtUserReleaseDC( hwnd, hdc );
    if (source_valid)
    {
        surface->composed_serial = present->serial;
        InterlockedExchange( &surface->content_valid, TRUE );
    }
    if (composed && sync &&
        (InterlockedCompareExchange( &surface->active, 0, 0 ) ||
         InterlockedCompareExchange( &surface->server_cached, 0, 0 )))
        commit = TRUE;
    composition_retry = sync && authorized && !composed && !scene_retry;
    pthread_mutex_unlock( &surface->present_lock );

    /* wineserver can block behind unrelated requests.  Do not serialize all
     * process-local surfaces while acknowledging one composition epoch. */
    if (commit)
        toplevel = client_surface_set_server_state( hwnd, surface, CLIENT_SURFACE_STATE_PRESENT_COMMIT,
                                                    present->scene.generation,
                                                    present->scene.epoch, &wake );
    if (wake && toplevel) NtUserPostMessage( toplevel, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );

    if ((scene_retry || composition_retry) && present->scene.toplevel &&
        claim_client_surface_retry( surface, present->scene.generation ))
        client_surface_geometry_ready( present->scene.toplevel );
    return composed;
}

void client_surface_lock_present( struct client_surface *surface )
{
    pthread_mutex_lock( &surface->completion_lock );
}

void client_surface_unlock_present( struct client_surface *surface )
{
    pthread_mutex_unlock( &surface->completion_lock );
    client_surface_apply_pending_update( surface );
    if (InterlockedCompareExchange( &surface->external_completion_count, 0, 0 )) return;
    client_surface_resume_recompose( surface );
}

void client_surface_wait_present_locked( struct client_surface *surface, BOOL external_completion )
{
    if (surface->backend->handoff_serialize && surface->backend->handoff_serialize( surface ))
        external_completion = FALSE;
    /* Exact IDs may overlap each other, but a shared driver monitor has no
     * per-frame identity.  Drain exact work before arming that monitor, and
     * do not submit any new frame until an armed monitor has been consumed. */
    if (!external_completion) surface->driver_completion_waiters++;
    /* Every wake releases and reacquires completion_lock.  Recheck native
     * submission and target-writer intent together with completion mode: a
     * producer which drained the old token may already have begun its next
     * native call before another producer reacquires this mutex. */
    while (InterlockedCompareExchange( &surface->target_update_waiters, 0, 0 ) ||
           surface->native_present_count ||
           (external_completion ? surface->driver_completion_count || surface->driver_completion_waiters ||
                                  (surface->backend->handoff_serialize &&
                                   surface->backend->handoff_serialize( surface ) &&
                                   InterlockedCompareExchange( &surface->external_completion_count, 0, 0 )) :
                                  InterlockedCompareExchange( &surface->external_completion_count, 0, 0 )))
        pthread_cond_wait( &surface->completion_cond, &surface->completion_lock );
    if (!external_completion && !--surface->driver_completion_waiters)
        pthread_cond_broadcast( &surface->completion_cond );
}

void client_surface_prepare_present_locked( struct client_surface *surface,
                                            struct client_surface_frame *present,
                                            BOOL external_completion )
{
    struct client_surface_target target;
    unsigned int retry;

    /* The caller has established submission readiness while acquiring this
     * mutex.  Do not release it here: a multi-surface caller may since have
     * acquired later mutexes in the global surface order. */
    assert( !surface->native_present_count );
    if (external_completion)
    {
        assert( !surface->driver_completion_count );
    }
    else
    {
        assert( !InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ) );
    }

    memset( present, 0, sizeof(*present) );

    /* A server scene sequence is also the invalidation token for native
     * geometry.  Reapplying an unchanged scene on every GL/Vulkan frame made
     * resize queries and X11 target setup part of the steady-state hot path. */
    pthread_mutex_lock( &surface->present_lock );
    client_surface_get_scene( surface, &present->scene );
    if (surface->hwnd && InterlockedCompareExchange( &surface->active, 0, 0 ) &&
        !present->scene.authoritative)
    {
        BOOL wake = FALSE;
        HWND toplevel = client_surface_set_server_state( surface->hwnd, surface,
                                                         CLIENT_SURFACE_STATE_CLAIM,
                                                         0, 0, &wake );

        if (wake && toplevel)
            NtUserPostMessage( toplevel, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
        client_surface_get_scene( surface, &present->scene );
    }
    for (retry = 0; retry < 2 && surface->hwnd &&
         (!present->scene.valid || !surface->target.valid ||
          surface->target.toplevel != present->scene.toplevel ||
          surface->target_scene_epoch != present->scene.epoch ||
          surface->target_scene_mode != present->scene.mode); ++retry)
    {
        if (present->scene.valid)
            client_surface_update_present_scene_locked( surface, &present->scene );
        else
            client_surface_update_present_locked( surface );
        client_surface_get_scene( surface, &present->scene );
    }
    client_surface_get_target( surface, &target );
    /* Only client_surface_update_present_locked() may mark a server scene as
     * applied: it does so after validating the exact scene around the native
     * update.  A resize can advance the seqlock between the caller's sample
     * and that update.  Do not stamp the newer epoch onto the retained old
     * target or submit an offscreen completion against a DIRECT scene. */
    if (!present->scene.valid || target.toplevel != present->scene.toplevel ||
        surface->target_scene_epoch != present->scene.epoch ||
        surface->target_scene_mode != present->scene.mode)
        target.valid = FALSE;
    present->target_seq = target.seq;
    present->mode = target.mode;
    present->target = !target.valid ? CLIENT_SURFACE_FRAME_TARGET_INVALID :
                      target.offscreen ? CLIENT_SURFACE_FRAME_TARGET_OFFSCREEN :
                      CLIENT_SURFACE_FRAME_TARGET_ONSCREEN;
    if (present->target == CLIENT_SURFACE_FRAME_TARGET_OFFSCREEN)
        client_surface_prepare_handoff_locked( surface, present );
    /* Native completion also belongs to frames submitted while the owner is
     * preparing its scene. The native drawable is usable independently of
     * that publication token, and its first completed image must be frozen. */
    if (surface->target.valid && target.offscreen)
    {
        if (external_completion)
        {
            present->completion.kind = CLIENT_SURFACE_COMPLETION_EXACT;
            present->completion.external_result = TRUE;
        }
        else if (client_surface_backend_prepare_completion( surface ))
            present->completion.kind = CLIENT_SURFACE_COMPLETION_SHARED;
    }
    pthread_mutex_unlock( &surface->present_lock );
}

void client_surface_prepare_present( struct client_surface *surface,
                                     struct client_surface_frame *present,
                                     BOOL external_completion )
{
    unsigned long long start = TRACE_ON(csperf) ? client_surface_perf_time() : 0;

    client_surface_prepare_scene( surface );
    client_surface_lock_present( surface );
    client_surface_wait_present_locked( surface, external_completion );
    client_surface_prepare_present_locked( surface, present, external_completion );
    TRACE_(csperf)( "ticks=%llu event=prepare identity=%s begin=%llu\n",
                   client_surface_perf_time(), wine_dbgstr_longlong( surface->identity ), start );
}

static void client_surface_begin_present_locked( struct client_surface *surface )
{
    surface->native_present_count++;
}

void client_surface_begin_present( struct client_surface *surface )
{
    client_surface_begin_present_locked( surface );
    client_surface_unlock_present( surface );
}

static void client_surface_register_completion_locked( struct client_surface *surface,
                                                       struct client_surface_frame *present )
{
    if (present->completion.kind == CLIENT_SURFACE_COMPLETION_NONE) return;
    InterlockedIncrement( &surface->external_completion_count );
    if (present->completion.kind == CLIENT_SURFACE_COMPLETION_SHARED)
        surface->driver_completion_count++;
}

void client_surface_submit_present_locked( struct client_surface *surface,
                                           struct client_surface_frame *present )
{
    /* Submission serials, unlike preparation serials, preserve the native
     * order observed by concurrent producer queues. */
    if (!present->serial)
    {
        present->serial = InterlockedIncrement64( &surface->present_serial );
        present->submission_time = NtGetTickCount();
        /* Register completion ownership before releasing submission
         * serialization.  Cached replay can then never pass a native request
         * which escaped without yet reaching the completion queue. */
        client_surface_register_completion_locked( surface, present );
    }
}

void client_surface_submit_present( struct client_surface *surface,
                                    struct client_surface_frame *present )
{
    client_surface_lock_present( surface );
    client_surface_submit_present_locked( surface, present );
    assert( surface->native_present_count > 0 );
    if (!--surface->native_present_count)
        pthread_cond_broadcast( &surface->completion_cond );
    client_surface_unlock_present( surface );
}

static BOOL client_surface_complete_direct_present_locked(
    struct client_surface *surface, struct client_surface_frame *present )
{
    BOOL completed = FALSE;

    /* A DIRECT swap has already made the producer-owned native child visible.
     * It has no compositor destination, completion token or scene generation
     * to publish.  Retain only the completed source metadata needed by a
     * later DIRECT -> STAGED/COMPOSITED transition. */
    pthread_mutex_lock( &surface->present_lock );
    if (present->target_seq != surface->target.seq ||
        present->scene.toplevel != surface->target.toplevel ||
        !surface->hwnd || !surface->target.valid || surface->target.offscreen ||
        (!InterlockedCompareExchange( &surface->active, 0, 0 ) &&
         !InterlockedCompareExchange( &surface->server_cached, 0, 0 )))
    {
        TRACE( "discarding direct %s presentation across target state change\n",
               debugstr_client_surface( surface ) );
    }
    else if (present->serial <= surface->composed_serial)
    {
        present->result = CLIENT_SURFACE_FRAME_SUPERSEDED;
        TRACE( "discarding superseded direct presentation %s serial %s, composed %s\n",
               debugstr_client_surface( surface ), wine_dbgstr_longlong( present->serial ),
               wine_dbgstr_longlong( surface->composed_serial ) );
    }
    else
    {
        surface->composed_serial = present->serial;
        InterlockedExchange( &surface->content_valid, TRUE );
        completed = TRUE;
    }
    pthread_mutex_unlock( &surface->present_lock );
    return completed;
}

BOOL client_surface_complete_present_locked( struct client_surface *surface,
                                             struct client_surface_frame *present,
                                             BOOL submitted, BOOL external_completed,
                                             const SIZE *expected_size, DWORD timeout )
{
    BOOL handed_off = FALSE, source_valid = FALSE, source_frozen = FALSE;
    BOOL completed = submitted && present->target != CLIENT_SURFACE_FRAME_TARGET_INVALID;

    TRACE( "completing source %s serial %s snapshot %dx%d submitted %u external %u target %u\n",
           debugstr_client_surface( surface ), wine_dbgstr_longlong( present->serial ),
           (int)present->source_size.cx, (int)present->source_size.cy, submitted, external_completed, present->target );

    if (!submitted && present->completion.kind == CLIENT_SURFACE_COMPLETION_EXACT)
        present->result = CLIENT_SURFACE_FRAME_COMPLETION_FAILED;

    if (!submitted && present->completion.kind == CLIENT_SURFACE_COMPLETION_SHARED)
    {
        /* A failed WSI call does not prove that no native request escaped.
         * Retire the armed boundary so a delayed request cannot satisfy the
         * next transaction's completion wait. */
        client_surface_backend_abandon_completion( surface );
        present->result = CLIENT_SURFACE_FRAME_COMPLETION_FAILED;
    }
    if (completed && present->target == CLIENT_SURFACE_FRAME_TARGET_OFFSCREEN)
    {
        /* kind identifies the host completion source, while external_result
         * identifies who consumed it.  A queued shared monitor has already
         * consumed its one-shot backend event and its supplied result must be
         * used instead of waiting on that event a second time. */
        if (client_surface_completion_result_is_external( &present->completion ))
            completed = external_completed;
        else if (present->completion.kind == CLIENT_SURFACE_COMPLETION_SHARED)
            completed = client_surface_backend_wait_completion( surface, timeout );
        else
            completed = FALSE;
        if (!completed) present->result = CLIENT_SURFACE_FRAME_COMPLETION_FAILED;
    }
    if (completed && present->completion.resolve)
    {
        /* Completion waits run without submission serialization. A newer
         * frame can replace their handoff while they sleep; it alone owns
         * the mutable native source when the wait returns. */
        pthread_mutex_lock( &surface->present_lock );
        if (!surface->target.valid || present->target_seq != surface->target.seq ||
            present->serial <= surface->composed_serial ||
            (present->handoff_control && (!surface->handoff_slot ||
             __atomic_load_n( &surface->handoff_slot[present->handoff_index].control, __ATOMIC_ACQUIRE ) !=
             present->handoff_control)))
        {
            present->result = CLIENT_SURFACE_FRAME_SUPERSEDED;
        }
        else if (!present->completion.resolve( present->completion.context, surface, present ))
        {
            present->result = CLIENT_SURFACE_FRAME_COMPLETION_FAILED;
            completed = FALSE;
        }
        pthread_mutex_unlock( &surface->present_lock );
        if (present->result == CLIENT_SURFACE_FRAME_SUPERSEDED)
            client_surface_abandon_handoff_locked( surface, present );
    }
    if (submitted && external_completed && !present->completion.resolve &&
        present->completion.kind == CLIENT_SURFACE_COMPLETION_EXACT &&
        present->target == CLIENT_SURFACE_FRAME_TARGET_INVALID && !present->source_size.cx &&
        client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_OWNER_SCENE_PLAN ))
    {
        /* An exact native completion proves the front buffer, even when no
         * owner scene existed at submission. Unlike an already independent
         * image, this mutable drawable must still match its native target. */
        pthread_mutex_lock( &surface->present_lock );
        if (surface->hwnd && surface->target.valid && surface->target.offscreen &&
            present->target_seq == surface->target.seq && present->serial > surface->composed_serial &&
            (surface->active || surface->server_cached) && surface->backend->handoff_complete &&
            prepare_client_surface_handoff_locked( surface, present, TRUE ))
        {
            struct client_surface_handoff_slot *slot = &surface->handoff_slot[present->handoff_index];

            if ((source_frozen = surface->backend->handoff_complete( surface, slot )))
                present->source_size = (SIZE){slot->width, slot->height};
        }
        pthread_mutex_unlock( &surface->present_lock );
    }
    if (submitted && external_completed && present->source_size.cx && present->source_size.cy)
    {
        /* An independently frozen source can outlive the scene sampled at
         * submission. Retain it before checking publication authorization;
         * a preparing owner must not consume the application's only repaint. */
        pthread_mutex_lock( &surface->present_lock );
        source_valid = surface->hwnd && surface->target.valid &&
                       present->serial > surface->composed_serial &&
                       (InterlockedCompareExchange( &surface->active, 0, 0 ) ||
                        InterlockedCompareExchange( &surface->server_cached, 0, 0 )) &&
                       client_surface_validate_size_locked( surface, &present->source_size );
        if (source_valid && present->handoff_control)
            source_valid = surface->handoff_slot &&
                __atomic_load_n( &surface->handoff_slot[present->handoff_index].control, __ATOMIC_ACQUIRE ) ==
                    present->handoff_control &&
                (source_frozen || !surface->backend->handoff_complete || surface->backend->handoff_complete(
                    surface, surface->handoff_slot + present->handoff_index ));
        if (source_valid)
        {
            surface->composed_serial = present->serial;
            InterlockedExchange( &surface->content_valid, TRUE );
        }
        TRACE( "retained source %s valid %u target %u scene %u/%s/%s content %ld\n",
               debugstr_client_surface( surface ), source_valid, surface->target.valid,
               present->scene.valid, wine_dbgstr_longlong( present->scene.generation ),
               wine_dbgstr_longlong( present->scene.epoch ), (long)surface->content_valid );
        pthread_mutex_unlock( &surface->present_lock );
    }
    if ((completed || source_valid) && InterlockedCompareExchange( &surface->active, 0, 0 ) &&
        !present->scene.authoritative)
    {
        BOOL wake = FALSE;
        HWND hwnd;
        HWND toplevel;

        /* Registration only advertises lifetime. A surface becomes the
         * producer after a host presentation or independent source snapshot
         * has completed, so an
         * unused VkSurfaceKHR or drawable cannot take publication ownership
         * merely by being created later.  Serialize the identity read and
         * server transition with unregister/reuse; otherwise the latter can
         * renew the token between the active test and this request. */
        pthread_mutex_lock( &surface->present_lock );
        hwnd = surface->hwnd;
        if (!InterlockedCompareExchange( &surface->active, 0, 0 )) hwnd = NULL;
        if (hwnd)
        {
            toplevel = client_surface_set_server_state( hwnd, surface,
                                                        CLIENT_SURFACE_STATE_CLAIM,
                                                        0, 0, &wake );
            if (wake && toplevel)
                NtUserPostMessage( toplevel, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
            client_surface_get_scene( surface, &present->scene );
        }
        pthread_mutex_unlock( &surface->present_lock );
    }
    if ((completed || source_valid) && present->handoff_control &&
        present->result != CLIENT_SURFACE_FRAME_SUPERSEDED)
    {
        handed_off = client_surface_publish_handoff_locked( surface, present, source_valid );
        if (!handed_off && present->result != CLIENT_SURFACE_FRAME_SUPERSEDED)
            client_surface_abandon_handoff_locked( surface, present );
    }
    if (source_valid && !handed_off)
    {
        handed_off = publish_client_surface_independent_source_locked( surface, present );
    }
    if (handed_off) completed = TRUE;
    if (completed && !handed_off && !source_valid && present->result != CLIENT_SURFACE_FRAME_SUPERSEDED)
    {
        if (present->target == CLIENT_SURFACE_FRAME_TARGET_ONSCREEN &&
            present->mode == CLIENT_SURFACE_PRESENTATION_DIRECT &&
            !present->scene.generation &&
            present->completion.kind == CLIENT_SURFACE_COMPLETION_NONE)
            completed = client_surface_complete_direct_present_locked( surface, present );
        else
            completed = client_surface_end_present_internal( surface, expected_size, TRUE, present );
    }
    if (!completed) client_surface_abandon_handoff_locked( surface, present );
    /* A composition failure may still have accepted a completed source; its
     * serial then protects it from invalidation.  Otherwise retire both the
     * failed frame and any older cached contents before releasing its token. */
    if (!completed) client_surface_invalidate_source_locked( surface, present );
    if (source_valid && !handed_off && present->scene.toplevel)
    {
        InterlockedIncrement64( &surface->recompose_seq );
        client_surface_geometry_ready( present->scene.toplevel );
    }
    if (present->completion.kind != CLIENT_SURFACE_COMPLETION_NONE)
    {
        BOOL release_handoff = FALSE, wake = FALSE;

        assert( InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ) > 0 );
        if (present->completion.kind == CLIENT_SURFACE_COMPLETION_SHARED)
        {
            assert( surface->driver_completion_count > 0 );
            if (!--surface->driver_completion_count) wake = TRUE;
        }
        if (!InterlockedDecrement( &surface->external_completion_count ))
        {
            release_handoff = surface->handoff_release_pending;
            wake = TRUE;
        }
        memset( &present->completion, 0, sizeof(present->completion) );
        if (release_handoff) client_surface_release_handoff( surface );
        if (wake) pthread_cond_broadcast( &surface->completion_cond );
    }
    return completed;
}

static BOOL wait_deferred_driver_completion( void *context, DWORD timeout )
{
    struct client_surface *surface = context;

    return client_surface_backend_wait_completion( surface, timeout );
}

static void release_deferred_driver_completion( void *context )
{
    client_surface_release( context );
}

BOOL client_surface_complete_present( struct client_surface *surface,
                                      struct client_surface_frame *present,
                                      BOOL submitted, BOOL external_completed,
                                      const SIZE *expected_size, DWORD timeout )
{
    BOOL ret;

    /* An armed driver monitor has exclusive ownership through
     * completion_lock.  Transfer that ownership to the same bounded queue as
     * explicit GLX/EGL/Vulkan completion IDs instead of blocking the caller. */
    if (submitted && present->completion.kind == CLIENT_SURFACE_COMPLETION_SHARED &&
        present->target == CLIENT_SURFACE_FRAME_TARGET_OFFSCREEN && timeout)
    {
        client_surface_add_ref( surface );
        client_surface_set_present_completion( present, wait_deferred_driver_completion,
                                               release_deferred_driver_completion, surface );
        client_surface_defer_present( surface, present, expected_size );
        return TRUE;
    }

    client_surface_lock_present( surface );
    ret = client_surface_complete_present_locked( surface, present, submitted,
                                                  external_completed, expected_size, timeout );
    client_surface_unlock_present( surface );
    return ret;
}

void client_surface_present( struct client_surface *surface )
{
    struct client_surface_frame present;

    /* Compatibility path for drivers whose presentation callback already
     * supplies a host completion boundary.  It still participates in target
     * token validation and per-surface submission serialization. */
    client_surface_prepare_present( surface, &present, TRUE );
    client_surface_begin_present( surface );
    client_surface_submit_present( surface, &present );
    client_surface_complete_present( surface, &present, TRUE, TRUE, NULL, 0 );
}
