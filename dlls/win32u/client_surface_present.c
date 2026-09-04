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

#include "ntstatus.h"
#include "client_surface.h"
#include "ntuser_private.h"
#include "wine/server.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(win);

static BOOL client_surface_backend_present( struct client_surface *surface, HDC hdc,
                                            HRGN surface_region, BOOL flush,
                                            BOOL defer_visible )
{
    return !surface->backend->present ||
           surface->backend->present( surface, hdc, surface_region, flush, defer_visible );
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

static BOOL begin_client_surface_composition( HWND hwnd, const struct client_surface *surface,
                                              const struct client_surface_present *present,
                                              BOOL lease, BOOL *valid )
{
    BOOL compose = FALSE;

    *valid = FALSE;
    SERVER_START_REQ( set_client_surface_state )
    {
        req->handle = wine_server_user_handle( hwnd );
        req->surface = surface->identity;
        req->flags = CLIENT_SURFACE_STATE_PRESENT_BEGIN |
                     (lease ? CLIENT_SURFACE_STATE_PRESENT_WRITE_LEASE : 0);
        req->generation = present->generation;
        req->scene_generation = present->scene_generation;
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
                                              const struct client_surface_present *present,
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
               present->scene_valid && toplevel == present->scene_toplevel &&
               !(scene_generation & 1) && scene_generation == present->scene_generation;
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
    NtGdiDeleteObjectApp( clips );
    return TRUE;
}

/* Cross-process clipping changes only with the server-owned scene sequence.
 * Keep the derived region on the surface so steady-state presents avoid a
 * server round trip, heap allocation, and O(occluders) region reconstruction. */
static BOOL get_cached_client_surface_region( struct client_surface *surface, HWND hwnd,
                                              const RECT *monitor_rect,
                                              const struct client_surface_present *present,
                                              HRGN *region )
{
    struct client_surface_clip_snapshot snapshot = {0};
    struct ratio raw_dpi;
    HRGN new_region = 0;
    BOOL valid;

    get_win_monitor_dpi( hwnd, &raw_dpi );
    if (!raw_dpi.num || !raw_dpi.den || !present->scene_valid) return FALSE;

    if (surface->clip_region_valid &&
        surface->clip_scene_generation == present->scene_generation &&
        surface->clip_target_epoch == present->target_epoch &&
        surface->clip_dpi_num == raw_dpi.num && surface->clip_dpi_den == raw_dpi.den &&
        EqualRect( &surface->clip_monitor_rect, monitor_rect ))
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
    surface->clip_scene_generation = present->scene_generation;
    surface->clip_target_epoch = present->target_epoch;
    surface->clip_monitor_rect = *monitor_rect;
    surface->clip_dpi_num = raw_dpi.num;
    surface->clip_dpi_den = raw_dpi.den;
    surface->clip_region = new_region;
    surface->clip_region_valid = TRUE;
    *region = new_region;
    return TRUE;
}

static BOOL client_surface_validate_size_locked( struct client_surface *surface,
                                                 const SIZE *expected_size )
{
    if (expected_size &&
        (surface->virtual_rect.right - surface->virtual_rect.left != expected_size->cx ||
         surface->virtual_rect.bottom - surface->virtual_rect.top != expected_size->cy))
    {
        WARN( "not composing %s size %dx%d for expected frame %dx%d\n",
              debugstr_client_surface( surface ),
              surface->virtual_rect.right - surface->virtual_rect.left,
              surface->virtual_rect.bottom - surface->virtual_rect.top,
              (int)expected_size->cx, (int)expected_size->cy );
        return FALSE;
    }
    return TRUE;
}

static BOOL client_surface_publication_matches( const struct client_surface_present *present )
{
    struct object_lock lock = OBJECT_LOCK_INIT;
    const window_shm_t *window_shm = NULL;
    UINT64 generation = 0, scene_generation = 0;
    BOOL preparing = FALSE;
    NTSTATUS status;

    if (!present->scene_valid || !present->scene_toplevel) return FALSE;
    while ((status = get_shared_window( present->scene_toplevel, &lock, &window_shm )) == STATUS_PENDING)
    {
        generation = (window_shm->client_surface_flags & WINDOW_SHM_CLIENT_SURFACE_COMPOSING) ?
                     window_shm->client_surface_generation : 0;
        scene_generation = window_shm->client_surface_scene_generation;
        preparing = !!(window_shm->client_surface_flags & WINDOW_SHM_CLIENT_SURFACE_PREPARING);
    }
    return !status && !preparing && generation == present->generation && !(scene_generation & 1) &&
           scene_generation == present->scene_generation;
}

static BOOL claim_client_surface_retry( struct client_surface *surface, UINT64 generation )
{
    LONG64 current;

    if (!generation) return FALSE;
    for (;;)
    {
        current = ReadAcquire64( &surface->recompose_retry_generation );
        if ((UINT64)current >= generation) return FALSE;
        if (InterlockedCompareExchange64( &surface->recompose_retry_generation,
                                          (LONG64)generation, current ) == current)
            return TRUE;
    }
}

BOOL client_surface_end_present_internal( struct client_surface *surface, UINT64 generation,
                                          const SIZE *expected_size, BOOL new_content,
                                          struct client_surface_present *present )
{
    HWND hwnd = 0, toplevel = 0;
    RECT monitor_rect = {0};
    HRGN surface_region = 0;
    BOOL commit = FALSE, compose = FALSE, composed = FALSE, copied = FALSE, offscreen = FALSE;
    BOOL region_valid = TRUE, sync = !!generation, wake = FALSE;
    BOOL authorized = present && present->authoritative;
    BOOL begin_valid = TRUE, composition_retry = FALSE, guarded = FALSE, leased = FALSE;
    BOOL scene_retry = FALSE, source_valid = FALSE;
    UINT server_flags = 0;
    HDC hdc = 0;

    assert( present );
    /* The caller owns a surface reference and completion_lock.  present_lock
     * serializes detach, membership transitions and native target changes, so
     * the process-wide registry lock is neither needed for lifetime nor for
     * target validation on the per-frame path. */
    pthread_mutex_lock( &surface->present_lock );
    if (!present->target_ready ||
        present->target_epoch != ReadAcquire64( &surface->target_epoch ) ||
        present->lifecycle_seq != ReadAcquire( &surface->lifecycle_seq ) ||
        present->scene_toplevel != surface->ready_toplevel)
    {
        TRACE( "discarding %s presentation across target state change\n",
               debugstr_client_surface( surface ) );
    }
    else if (new_content && present->serial <= surface->composed_serial)
    {
        present->superseded = TRUE;
        TRACE( "discarding superseded presentation %s serial %s, composed %s\n",
               debugstr_client_surface( surface ), wine_dbgstr_longlong( present->serial ),
               wine_dbgstr_longlong( surface->composed_serial ) );
    }
    else if ((hwnd = surface->hwnd) &&
             InterlockedCompareExchange( &surface->target_ready, 0, 0 ) &&
             (InterlockedCompareExchange( &surface->active, 0, 0 ) ||
              InterlockedCompareExchange( &surface->server_cached, 0, 0 )))
    {
        if (sync) TRACE( "client surface %p starts composition epoch commit\n", hwnd );
        if (new_content || InterlockedCompareExchange( &surface->content_valid, 0, 0 ))
        {
            compose = client_surface_validate_size_locked( surface, expected_size );
            monitor_rect = surface->monitor_rect;
            offscreen = InterlockedCompareExchange( &surface->offscreen, 0, 0 );
        }
        else
            TRACE( "not recomposing incomplete cached content for %s\n",
                   debugstr_client_surface( surface ) );
    }
    source_valid = compose && new_content;
    if (compose && offscreen && present && !present->scene_valid) compose = FALSE;
    guarded = compose && offscreen &&
              client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_NATIVE_WRITE_LEASE );
    if (compose && (sync || guarded))
    {
        authorized = begin_client_surface_composition( hwnd, surface, present,
                                                       guarded, &begin_valid );
        leased = guarded && authorized;
        if (!authorized)
        {
            /* Only the authoritative producer for an HWND may touch its
             * composition destination.  A still-current denial is therefore a
             * successful no-op; a stale denial is retried in the new scene. */
            composed = begin_valid && client_surface_publication_matches( present );
            scene_retry = !composed;
            compose = FALSE;
        }
    }
    else if (compose && !authorized)
    {
        /* Steady-state presents also obey the server's active-over-cache
         * producer choice.  The shared identity is covered by the top-level
         * scene seqlock, so this adds no per-frame server round trip. */
        composed = begin_valid && client_surface_publication_matches( present );
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
            if (!client_surface_publication_matches( present )) scene_retry = TRUE;
            compose = FALSE;
        }
        else
        {
            /* Local DCE invalidation already follows every scene mutation;
             * foreign HWNDs are refreshed unconditionally by NtUserGetDCEx.
             * Forcing another server fetch here made every local frame pay an
             * avoidable round trip despite a matching scene token. */
            hdc = NtUserGetDCEx( hwnd, 0, DCX_CACHE | DCX_USESTYLE );
            if (!hdc)
            {
                WARN( "failed to acquire composition DC for %s\n", debugstr_client_surface( surface ) );
                compose = FALSE;
            }
        }
    }

    if (compose && offscreen && !client_surface_publication_matches( present ))
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
        surface->composition_scene_generation = present->scene_generation;
        surface->composition_toplevel = present->scene_toplevel;
        /* A native-target writer lease protects execution on the host server,
         * not merely submission from this process.  Complete the backend
         * copy before returning the lease so the owner cannot publish or
         * replace the shared target ahead of work queued on this connection. */
        copied = client_surface_backend_present( surface, hdc, surface_region,
                                                 sync || leased, sync );
        composed = copied;
    }
    if (copied && offscreen && !client_surface_publication_matches( present ))
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
    if (leased) server_flags |= CLIENT_SURFACE_STATE_PRESENT_END;
    if (commit) server_flags |= CLIENT_SURFACE_STATE_PRESENT_COMMIT;
    if (server_flags)
        toplevel = client_surface_set_server_state( hwnd, surface, server_flags,
                                                    generation, present->scene_generation, &wake );
    if (wake && toplevel) NtUserPostMessage( toplevel, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );

    /* Release a native-target writer before requesting repair.  The server
     * can then linearize the fresh owner snapshot immediately after the last
     * stale writer instead of creating a second deferred restart. */
    if ((scene_retry || composition_retry) && present->scene_toplevel &&
        claim_client_surface_retry( surface, generation ))
        client_surface_geometry_ready( present->scene_toplevel );
    return composed;
}

void client_surface_lock_present( struct client_surface *surface )
{
    pthread_mutex_lock( &surface->completion_lock );
}

void client_surface_unlock_present( struct client_surface *surface )
{
    struct client_surface_geometry geometry;

    pthread_mutex_unlock( &surface->completion_lock );
    if (InterlockedCompareExchange( &surface->external_completion_count, 0, 0 )) return;
    if (!InterlockedExchange( &surface->recompose_deferred, FALSE )) return;

    /* A replay consumer which could not acquire completion_lock transferred
     * scheduling ownership to this producer.  Drop the old scheduled state
     * only after the producer releases its causal boundary, then request one
     * fresh coalesced replay for the current top-level. */
    InterlockedExchange( &surface->recompose_scheduled, FALSE );
    client_surface_get_geometry( surface, &geometry );
    if (geometry.toplevel) client_surface_geometry_ready( geometry.toplevel );
}

void client_surface_prepare_present_locked( struct client_surface *surface,
                                            struct client_surface_present *present,
                                            BOOL external_completion )
{
    while (InterlockedCompareExchange( &surface->target_update_waiters, 0, 0 ) ||
           surface->native_present_count)
        pthread_cond_wait( &surface->completion_cond, &surface->completion_lock );

    /* Exact IDs may overlap each other, but a shared driver monitor has no
     * per-frame identity.  Drain exact work before arming that monitor, and
     * do not submit any new frame until an armed monitor has been consumed. */
    if (!external_completion)
    {
        surface->driver_completion_waiters++;
        while (InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ))
            pthread_cond_wait( &surface->completion_cond, &surface->completion_lock );
        if (!--surface->driver_completion_waiters)
            pthread_cond_broadcast( &surface->completion_cond );
    }
    else
    {
        while (surface->driver_completion_count || surface->driver_completion_waiters)
            pthread_cond_wait( &surface->completion_cond, &surface->completion_lock );
    }

    memset( present, 0, sizeof(*present) );
    present->completion_locked = TRUE;

    /* A server scene sequence is also the invalidation token for native
     * geometry.  Reapplying an unchanged scene on every GL/Vulkan frame made
     * resize queries and X11 target setup part of the steady-state hot path. */
    pthread_mutex_lock( &surface->present_lock );
    present->scene_valid = client_surface_get_publication( surface, &present->generation,
                                                           &present->scene_generation,
                                                           &present->scene_toplevel,
                                                           &present->authoritative );
    if (surface->hwnd &&
        (!present->scene_valid || !surface->target_ready ||
         surface->target_scene_toplevel != present->scene_toplevel ||
         surface->target_scene_generation != present->scene_generation))
    {
        client_surface_update_present_locked( surface );
        present->scene_valid = client_surface_get_publication( surface, &present->generation,
                                                               &present->scene_generation,
                                                               &present->scene_toplevel,
                                                               &present->authoritative );
    }
    if (present->scene_valid && surface->target_ready)
    {
        surface->target_scene_toplevel = present->scene_toplevel;
        surface->target_scene_generation = present->scene_generation;
    }
    present->target_epoch = ReadAcquire64( &surface->target_epoch );
    present->lifecycle_seq = ReadAcquire( &surface->lifecycle_seq );
    present->target_ready = InterlockedCompareExchange( &surface->target_ready, 0, 0 );
    present->offscreen = InterlockedCompareExchange( &surface->offscreen, 0, 0 );
    present->external_completion = present->target_ready && present->offscreen && external_completion;
    if (present->target_ready && present->offscreen && !present->external_completion)
        present->driver_completion = client_surface_backend_prepare_completion( surface );
    pthread_mutex_unlock( &surface->present_lock );
}

void client_surface_prepare_present( struct client_surface *surface,
                                     struct client_surface_present *present,
                                     BOOL external_completion )
{
    client_surface_lock_present( surface );
    client_surface_prepare_present_locked( surface, present, external_completion );
}

static void client_surface_begin_present_locked( struct client_surface *surface,
                                                 struct client_surface_present *present )
{
    assert( present->completion_locked );
    assert( !present->native_present_registered );
    surface->native_present_count++;
    present->native_present_registered = TRUE;
}

void client_surface_begin_present( struct client_surface *surface,
                                   struct client_surface_present *present )
{
    client_surface_begin_present_locked( surface, present );
    present->completion_locked = FALSE;
    client_surface_unlock_present( surface );
}

void client_surface_register_external_completion_locked( struct client_surface *surface,
                                                          struct client_surface_present *present )
{
    if (present->external_completion_registered) return;
    InterlockedIncrement( &surface->external_completion_count );
    if (present->driver_completion) surface->driver_completion_count++;
    present->external_completion_registered = TRUE;
}

void client_surface_submit_present_locked( struct client_surface *surface,
                                           struct client_surface_present *present )
{
    assert( present->completion_locked );
    /* Submission serials, unlike preparation serials, preserve the native
     * order observed by concurrent producer queues. */
    if (!present->serial)
    {
        present->serial = InterlockedIncrement64( &surface->present_serial );
        present->submission_time = NtGetTickCount();
    }
    /* Register exact completion ownership before releasing submission
     * serialization.  Otherwise cached replay can enter after the native
     * request escaped but before its deferred job increments the count. */
    if (present->external_completion)
        client_surface_register_external_completion_locked( surface, present );
    if (present->native_present_registered)
    {
        assert( surface->native_present_count > 0 );
        present->native_present_registered = FALSE;
        if (!--surface->native_present_count)
            pthread_cond_broadcast( &surface->completion_cond );
    }
}

void client_surface_submit_present( struct client_surface *surface,
                                    struct client_surface_present *present )
{
    if (!present->completion_locked)
    {
        client_surface_lock_present( surface );
        present->completion_locked = TRUE;
    }
    client_surface_submit_present_locked( surface, present );
    if (present->completion_locked && present->external_completion)
    {
        present->completion_locked = FALSE;
        client_surface_unlock_present( surface );
    }
}

BOOL client_surface_complete_present_locked( struct client_surface *surface,
                                             struct client_surface_present *present,
                                             BOOL submitted, BOOL external_completed,
                                             const SIZE *expected_size, DWORD timeout )
{
    BOOL completed = submitted && present->target_ready;

    if (!submitted && present->external_completion)
        present->completion_failed = TRUE;

    if (!submitted && present->driver_completion)
    {
        /* A failed WSI call does not prove that no native request escaped.
         * Retire the armed boundary so a delayed request cannot satisfy the
         * next transaction's completion wait. */
        client_surface_backend_abandon_completion( surface );
        present->completion_failed = TRUE;
    }
    if (completed && present->offscreen)
    {
        if (present->external_completion)
            completed = external_completed;
        else if (present->driver_completion)
            completed = client_surface_backend_wait_completion( surface, timeout );
        else
            completed = FALSE;
        present->completion_failed = !completed;
    }
    if (completed && InterlockedCompareExchange( &surface->active, 0, 0 ) &&
        (!present->authoritative ||
         !InterlockedCompareExchange( &surface->producer_claimed, 0, 0 )))
    {
        BOOL wake = FALSE;
        HWND hwnd;
        HWND toplevel;

        /* Registration only advertises lifetime.  A surface becomes the
         * producer after a real host presentation has completed, so an
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
            present->scene_valid = client_surface_get_publication( surface,
                                                    &present->generation,
                                                    &present->scene_generation,
                                                    &present->scene_toplevel,
                                                    &present->authoritative );
            InterlockedExchange( &surface->producer_claimed,
                                 present->authoritative );
        }
        pthread_mutex_unlock( &surface->present_lock );
    }
    if (completed)
        completed = client_surface_end_present_internal( surface, present->generation,
                                                         expected_size, TRUE, present );
    if (present->external_completion_registered)
    {
        BOOL wake = FALSE;

        assert( InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ) > 0 );
        if (present->driver_completion)
        {
            assert( surface->driver_completion_count > 0 );
            if (!--surface->driver_completion_count) wake = TRUE;
        }
        if (!InterlockedDecrement( &surface->external_completion_count )) wake = TRUE;
        present->external_completion_registered = FALSE;
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
                                      struct client_surface_present *present,
                                      BOOL submitted, BOOL external_completed,
                                      const SIZE *expected_size, DWORD timeout )
{
    BOOL ret;

    /* An armed driver monitor has exclusive ownership through
     * completion_lock.  Transfer that ownership to the same bounded queue as
     * explicit GLX/EGL/Vulkan completion IDs instead of blocking the caller. */
    if (submitted && present->driver_completion && present->offscreen && timeout)
    {
        present->external_completion = TRUE;
        client_surface_add_ref( surface );
        client_surface_defer_present( surface, present, TRUE, expected_size,
                                      wait_deferred_driver_completion,
                                      release_deferred_driver_completion, surface );
        return TRUE;
    }

    if (!present->completion_locked)
    {
        client_surface_lock_present( surface );
        present->completion_locked = TRUE;
    }
    ret = client_surface_complete_present_locked( surface, present, submitted,
                                                  external_completed, expected_size, timeout );
    client_surface_unlock_present( surface );
    present->completion_locked = FALSE;
    return ret;
}

void client_surface_present( struct client_surface *surface )
{
    struct client_surface_present present;

    /* Compatibility path for drivers whose presentation callback already
     * supplies a host completion boundary.  It still participates in target
     * token validation and per-surface submission serialization. */
    client_surface_prepare_present( surface, &present, TRUE );
    client_surface_submit_present( surface, &present );
    client_surface_complete_present( surface, &present, TRUE, TRUE, NULL, 0 );
}
