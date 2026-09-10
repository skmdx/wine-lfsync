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

static void client_surface_backend_abandon_completion( struct client_surface *surface )
{
    if (surface->backend->completion && surface->backend->completion->abandon)
        surface->backend->completion->abandon( surface );
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
        req->surface = client_surface_get_identity( surface );
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
                                              const RECT *monitor_rect,
                                              const struct client_surface_frame *present,
                                              struct client_surface_clip_snapshot *snapshot )
{
    struct rectangle bounds = wine_server_rectangle( *monitor_rect );
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
            wine_server_add_data( req, &bounds, sizeof(bounds) );
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

static HRGN create_client_surface_visible_region( const RECT *rects, UINT count,
                                                  struct ratio from, struct ratio to )
{
    RGNDATA *data;
    HRGN region, mapped;
    SIZE_T size;

    if (count > (MAXDWORD - FIELD_OFFSET( RGNDATA, Buffer )) / sizeof(*rects)) return 0;
    size = FIELD_OFFSET( RGNDATA, Buffer ) + (SIZE_T)count * sizeof(*rects);
    if (!(data = malloc( size ))) return 0;
    memset( &data->rdh, 0, sizeof(data->rdh) );
    data->rdh.dwSize = sizeof(data->rdh);
    data->rdh.iType = RDH_RECTANGLES;
    data->rdh.nCount = count;
    data->rdh.nRgnSize = count * sizeof(*rects);
    if (count) memcpy( data->Buffer, rects, count * sizeof(*rects) );
    region = NtGdiExtCreateRegion( NULL, size, data );
    free( data );
    if (!region) return 0;
    mapped = map_dpi_region( region, from, to );
    NtGdiDeleteObjectApp( region );
    return mapped;
}

void client_surface_free_scene_snapshot( UINT count, struct client_surface_scene_member *members )
{
    UINT i;

    for (i = 0; i < count; ++i)
        if (members[i].region) NtGdiDeleteObjectApp( members[i].region );
    free( members );
}

BOOL client_surface_get_scene_snapshot( HWND toplevel, UINT64 *scene_id, UINT *count,
                                        struct client_surface_scene_member **members )
{
    struct client_surface_scene_member *result = NULL;
    struct client_surface_scene_layer *layers = NULL;
    struct window_rects *monitor_rects = NULL;
    struct ratio *dpis = NULL;
    unsigned char *data = NULL, *next, *cursor;
    UINT size = 4096, total = 0, i;
    data_size_t reply_size = 0, remaining;
    UINT64 id = *scene_id;
    NTSTATUS status;
    BOOL ret = FALSE;

    *count = 0;
    *members = NULL;
    if (id & 1) return FALSE;
    for (;;)
    {
        UINT required;
        UINT64 current;

        if (!(next = realloc( data, size ))) goto done;
        data = next;
        SERVER_START_REQ( get_client_surface_scene_snapshot )
        {
            req->handle = wine_server_user_handle( toplevel );
            req->scene_id = id;
            wine_server_set_reply( req, data, size );
            status = wine_server_call( req );
            required = reply->total_size;
            current = reply->scene_id;
            total = reply->count;
            reply_size = wine_server_reply_size( reply );
        }
        SERVER_END_REQ;
        if (status == STATUS_BUFFER_OVERFLOW && required > size && !(current & 1))
        {
            /* Do not silently join the sizing reply to a different scene. */
            id = current;
            size = required;
            continue;
        }
        if (status || (current & 1) || (id && id != current) || required != reply_size ||
            total > reply_size / sizeof(*layers)) goto done;
        id = current;
        break;
    }
    if (total && (!(result = calloc( total, sizeof(*result) )) ||
                  !(layers = calloc( total, sizeof(*layers) )) ||
                  !(monitor_rects = calloc( total, sizeof(*monitor_rects) )) ||
                  !(dpis = calloc( total, sizeof(*dpis) )))) goto done;
    cursor = data;
    remaining = reply_size;
    for (i = 0; i < total; ++i)
    {
        struct client_surface_scene_layer *layer = &layers[i];
        struct client_surface_target *target = &result[i].target;
        UINT bytes;

        if (remaining < sizeof(*layer)) goto done;
        memcpy( layer, cursor, sizeof(*layer) );
        cursor += sizeof(*layer);
        remaining -= sizeof(*layer);
        if (!layer->producer.handle || !layer->producer.surface || !layer->producer.process ||
            !layer->window_dpi.num || !layer->window_dpi.den || !layer->raw_dpi.num || !layer->raw_dpi.den ||
            (layer->flags & ~(CLIENT_SURFACE_SCENE_PRESENT_RECT | CLIENT_SURFACE_SCENE_DIRECT_CANDIDATE)) ||
            layer->reserved ||
            layer->visible_count > remaining / sizeof(RECT)) goto done;
        bytes = layer->visible_count * sizeof(RECT);
        cursor += bytes;
        remaining -= bytes;
        if (layer->clip_count > remaining / sizeof(struct client_surface_clip_window)) goto done;
        bytes = layer->clip_count * sizeof(struct client_surface_clip_window);
        cursor += bytes;
        remaining -= bytes;
        if (!layer->producer.visible && (layer->visible_count || layer->clip_count)) goto done;
        result[i].hwnd = wine_server_ptr_handle( layer->producer.handle );
        result[i].process = layer->producer.process;
        result[i].identity = layer->producer.surface;
        result[i].cookie = layer->producer.cookie;
        result[i].direct_candidate = !!(layer->flags & CLIENT_SURFACE_SCENE_DIRECT_CANDIDATE);
        result[i].visible = !!layer->producer.visible;
        result[i].producer_mapped = !!layer->producer.producer_mapped;
        dpis[i] = layer->window_dpi;
        monitor_rects[i].window = wine_server_get_rect( layer->top_window );
        monitor_rects[i].client = wine_server_get_rect( layer->top_client );
        monitor_rects[i].visible = wine_server_get_rect( layer->top_visible );
        target->toplevel = toplevel;
        target->virtual_rect = wine_server_get_rect( layer->source );
        target->monitor_rect = map_dpi_rect( target->virtual_rect, layer->window_dpi, layer->raw_dpi );
        OffsetRect( &target->virtual_rect, layer->top_client.left - layer->top_visible.left,
                     layer->top_client.top - layer->top_visible.top );
        target->dpi_num = layer->raw_dpi.num;
        target->dpi_den = layer->raw_dpi.den;
    }
    if (remaining || (total && !map_window_rects_virt_to_raw_batch( total, monitor_rects, dpis ))) goto done;
    cursor = data;
    for (i = 0; i < total; ++i)
    {
        const struct client_surface_scene_layer *layer = &layers[i];
        struct client_surface_clip_snapshot snapshot = {0};
        struct client_surface_target *target = &result[i].target;
        const RECT *visible_rects;
        HRGN visible;
        BOOL combined;

        OffsetRect( &target->monitor_rect, monitor_rects[i].client.left - monitor_rects[i].visible.left,
                     monitor_rects[i].client.top - monitor_rects[i].visible.top );
        cursor += sizeof(*layer);
        visible_rects = (const RECT *)cursor;
        cursor += layer->visible_count * sizeof(RECT);
        snapshot.windows = (struct client_surface_clip_window *)cursor;
        snapshot.count = layer->clip_count;
        cursor += layer->clip_count * sizeof(*snapshot.windows);
        if (!result[i].visible) continue;
        if (!get_client_surface_region( &target->monitor_rect, &snapshot, &result[i].region )) goto done;
        if (!result[i].region && !(result[i].region = NtGdiCreateRectRgn( 0, 0,
            target->monitor_rect.right - target->monitor_rect.left,
            target->monitor_rect.bottom - target->monitor_rect.top ))) goto done;
        if (!(layer->flags & CLIENT_SURFACE_SCENE_PRESENT_RECT))
        {
            visible = create_client_surface_visible_region( visible_rects, layer->visible_count,
                                                             layer->window_dpi, layer->raw_dpi );
            if (!visible) goto done;
            combined = NtGdiCombineRgn( result[i].region, result[i].region, visible, RGN_AND ) != ERROR;
            NtGdiDeleteObjectApp( visible );
            if (!combined) goto done;
        }
    }
    *scene_id = id;
    *members = result;
    *count = total;
    ret = TRUE;
done:
    if (!ret && result) client_surface_free_scene_snapshot( total, result );
    free( monitor_rects );
    free( dpis );
    free( layers );
    free( data );
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
        surface->clip_target_seq == surface->target.seq)
    {
        *region = surface->clip_region;
        return TRUE;
    }

    valid = get_client_surface_clip_snapshot( hwnd, &raw_dpi, monitor_rect, present, &snapshot );
    if (valid) valid = get_client_surface_region( monitor_rect, &snapshot, &new_region );
    release_client_surface_clip_snapshot( &snapshot );
    if (!valid)
    {
        if (new_region) NtGdiDeleteObjectApp( new_region );
        return FALSE;
    }

    if (surface->clip_region) NtGdiDeleteObjectApp( surface->clip_region );
    surface->clip_scene_epoch = present->scene.epoch;
    surface->clip_target_seq = surface->target.seq;
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
    BOOL scene_retry = FALSE, source_valid = FALSE, direct = FALSE;
    HDC hdc = 0;

    assert( present );
    /* The caller owns a surface reference and completion_lock.  present_lock
     * serializes detach, membership transitions and native target changes, so
     * the process-wide registry lock is neither needed for lifetime nor for
     * target validation on the per-frame path. */
    pthread_mutex_lock( &surface->present_lock );
    if (present->target == CLIENT_SURFACE_FRAME_TARGET_INVALID ||
        present->target_epoch != surface->target.epoch ||
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
    if (compose && !offscreen && present->mode == CLIENT_SURFACE_PRESENTATION_DIRECT)
    {
        /* The native WSI call already presented on the owner's attached
         * target. Keep the same source/epoch checks without a DC, copy, fence
         * or per-frame server transaction on the generation-zero path. */
        direct = composed = TRUE;
        compose = FALSE;
    }
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
    if (composed && sync && !direct &&
        (InterlockedCompareExchange( &surface->active, 0, 0 ) ||
         InterlockedCompareExchange( &surface->server_cached, 0, 0 )))
        commit = TRUE;
    composition_retry = sync && authorized && !composed && !scene_retry;
    pthread_mutex_unlock( &surface->present_lock );

    if (direct && new_content && sync && present->scene.mode == CLIENT_SURFACE_PRESENTATION_DIRECT &&
        present->scene.authoritative && client_surface_scene_current( &present->scene ) &&
        surface->backend->complete_direct)
        surface->backend->complete_direct( surface, present );

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
    for (;;)
    {
        while (InterlockedCompareExchange( &surface->target_update_waiters, 0, 0 ) ||
               surface->native_present_count ||
               (external_completion ? surface->driver_completion_count || surface->driver_completion_waiters ||
                                      (surface->backend->handoff_serialize &&
                                       surface->backend->handoff_serialize( surface ) &&
                                       InterlockedCompareExchange( &surface->external_completion_count, 0, 0 )) :
                                      InterlockedCompareExchange( &surface->external_completion_count, 0, 0 )))
            pthread_cond_wait( &surface->completion_cond, &surface->completion_lock );
        if (!external_completion || !InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ) ||
            client_surface_handoff_write_available( surface )) break;

        client_surface_handoff_wait( surface );
    }
    if (!external_completion && !--surface->driver_completion_waiters)
        pthread_cond_broadcast( &surface->completion_cond );
    /* Retire a discarded owner cache before the next native submission adds
     * a completion reference. Otherwise that new frame can pin the closed
     * channel while its capture needs to map the replacement channel. */
    client_surface_handoff_retire_closed( surface );
}

static void prepare_client_surface_present_locked( struct client_surface *surface,
                                                    struct client_surface_frame *present,
                                                    BOOL external_completion, BOOL allow_direct_transition )
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
          surface->target_scene_mode != present->scene.mode ||
          (allow_direct_transition && present->scene.authoritative &&
           present->scene.direct_candidate &&
           present->scene.mode != CLIENT_SURFACE_PRESENTATION_DIRECT)); ++retry)
    {
        /* Geometry may already match an unselected candidate scene. The
         * actual producer must still ask the owner to select its strategy
         * before the native present; geometry application is not admission. */
        if (present->scene.valid)
            client_surface_update_present_scene_locked( surface, &present->scene, allow_direct_transition );
        else
            client_surface_update_present_scene_locked( surface, NULL, allow_direct_transition );
        client_surface_get_scene( surface, &present->scene );
    }
    /* PREPARING can consume one geometry resample before the owner admits
     * DIRECT in the next. Admission and native attachment are separate
     * steps: finish applying that exact selected scene before choosing the
     * completion path. Otherwise its first image is captured offscreen while
     * the owner waits for a native DIRECT completion that cannot arrive.
     * A concurrent scene change fails the existing exact-snapshot check. */
    if (allow_direct_transition && present->scene.valid &&
        present->scene.mode == CLIENT_SURFACE_PRESENTATION_DIRECT &&
        (surface->target_scene_epoch != present->scene.epoch ||
         surface->target_scene_mode != present->scene.mode))
    {
        client_surface_update_present_scene_locked( surface, &present->scene, TRUE );
        client_surface_get_scene( surface, &present->scene );
    }
    client_surface_get_target( surface, &target );
    /* Only client_surface_update_present_locked() may mark a server scene as
     * applied: it does so after validating the exact scene around the native
     * update.  A resize can advance the seqlock between the caller's sample
     * and that update.  Do not stamp the newer epoch onto the retained old
     * target or submit an offscreen completion against a DIRECT scene. */
    if ((!present->scene.valid || target.toplevel != present->scene.toplevel ||
         surface->target_scene_epoch != present->scene.epoch ||
         surface->target_scene_mode != present->scene.mode) &&
        (target.offscreen || target.mode != CLIENT_SURFACE_PRESENTATION_DIRECT))
        target.valid = FALSE;
    /* An established attachment remains a valid native WSI target while the
     * owner prepares its next scene. Keep the native result and completed
     * image metadata; only a matching admitted plan can acknowledge that new
     * publication. Size, native epoch and lifetime checks still apply. */
    present->target_epoch = target.epoch;
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

void client_surface_prepare_present_locked( struct client_surface *surface,
                                            struct client_surface_frame *present,
                                            BOOL external_completion )
{
    prepare_client_surface_present_locked( surface, present, external_completion, TRUE );
}

void client_surface_prepare_recompose_locked( struct client_surface *surface,
                                              struct client_surface_frame *present )
{
    /* A cached image can be replayed by a completion system thread. It has
     * neither a producer TEB nor a new native Present to replace the image
     * that attaching a DIRECT drawable may discard. */
    prepare_client_surface_present_locked( surface, present, TRUE, FALSE );
}

BOOL client_surface_needs_completion_reservation( struct client_surface *surface )
{
    struct client_surface_scene scene;
    struct client_surface_target target;

    client_surface_get_target( surface, &target );
    if (!target.offscreen) return FALSE;
    /* Preparation selects and attaches an eligible DIRECT scene. Its old
     * offscreen target must not require capacity before that selection. If
     * the owner keeps composition, the prepared frame requests admission. */
    return !client_surface_get_scene( surface, &scene ) ||
           !scene.authoritative || !scene.direct_candidate;
}

/* The caller holds completion_lock and has not entered native submission.
 * Admission can require dropping that lock and preparing a newer scene. This
 * cancels only our unsubmitted tokens, not an uncertain native operation. */
void client_surface_cancel_prepare_locked( struct client_surface *surface,
                                           struct client_surface_frame *present )
{
    assert( !present->serial );
    pthread_mutex_lock( &surface->present_lock );
    client_surface_abandon_handoff_locked( surface, present );
    if (present->completion.kind == CLIENT_SURFACE_COMPLETION_SHARED &&
        surface->backend->completion && surface->backend->completion->cancel)
        surface->backend->completion->cancel( surface );
    pthread_mutex_unlock( &surface->present_lock );
}

BOOL client_surface_prepare_present( struct client_surface *surface,
                                     struct client_surface_frame *present,
                                     BOOL external_completion, BOOL asynchronous )
{
    struct client_surface_completion_job *job = NULL;
    unsigned long long start = TRACE_ON(csperf) ? client_surface_perf_time() : 0;
    unsigned long long scene, locked, ready;
    LONG pending_before, pending_after;

    client_surface_prepare_scene( surface );
    scene = start ? client_surface_perf_time() : 0;
    if (asynchronous && client_surface_needs_completion_reservation( surface ) &&
        !(job = client_surface_reserve_completion( surface ))) goto failed;
prepare:
    client_surface_lock_present( surface );
    locked = start ? client_surface_perf_time() : 0;
    pending_before = start ? InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ) : 0;
    client_surface_wait_present_locked( surface, external_completion );
    ready = start ? client_surface_perf_time() : 0;
    pending_after = start ? InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ) : 0;
    client_surface_prepare_present_locked( surface, present, external_completion );
    if (asynchronous && present->completion.kind != CLIENT_SURFACE_COMPLETION_NONE && !job)
    {
        /* Preparation may attach an offscreen target after the initial
         * inspection. Cancel its unsubmitted token and acquire admission
         * outside every surface lock before preparing that target again. */
        client_surface_cancel_prepare_locked( surface, present );
        client_surface_unlock_present( surface );
        if (!(job = client_surface_reserve_completion( surface ))) goto failed;
        goto prepare;
    }
    present->completion_job = job;
    TRACE_(csperf)( "ticks=%llu event=prepare identity=%s begin=%llu scene=%llu locked=%llu ready=%llu "
                   "pending_before=%d pending_after=%d\n", client_surface_perf_time(),
                   wine_dbgstr_longlong( client_surface_get_identity( surface ) ), start, scene, locked, ready,
                   pending_before, pending_after );
    return TRUE;
failed:
    RtlSetLastWin32Error( ERROR_NOT_ENOUGH_MEMORY );
    return FALSE;
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

static BOOL client_surface_capture_frame( struct client_surface *surface, struct client_surface_frame *present,
                                          const SIZE *expected_size, struct client_surface_completed_frame *frame )
{
    BOOL captured = FALSE;

    /* Completion waits run without submission serialization. A newer frame
     * can replace their handoff while they sleep; it alone owns the mutable
     * native source when the wait returns. Capture never consumes the fence. */
    pthread_mutex_lock( &surface->present_lock );
    if (!surface->hwnd || !surface->target.valid || present->target_epoch != surface->target.epoch ||
        present->serial <= surface->composed_serial ||
        !client_surface_handoff_valid( surface, present ))
        present->result = CLIENT_SURFACE_FRAME_SUPERSEDED;
    else if ((surface->active || surface->server_cached) &&
             (!present->capture.capture || present->capture.capture( present->capture.context, surface, present )) &&
             client_surface_validate_size_locked( surface, present->capture.size.cx ?
                                                  &present->capture.size : expected_size ))
    {
        if (!present->handoff_control && surface->target.offscreen &&
            client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_OWNER_SCENE_PLAN ))
            client_surface_prepare_source_locked( surface, present );
        captured = !!present->handoff_control;
    }
    pthread_mutex_unlock( &surface->present_lock );
    if (captured) captured = client_surface_freeze_frame_locked( surface, present, frame );
    if (!captured && present->result == CLIENT_SURFACE_FRAME_PENDING)
        present->result = CLIENT_SURFACE_FRAME_COMPLETION_FAILED;
    return captured;
}

static BOOL client_surface_finish_host_completion( struct client_surface *surface,
                                                    struct client_surface_frame *present,
                                                    BOOL submitted, BOOL external_completed, DWORD timeout )
{
    BOOL completed = submitted;

    if (completed && present->completion.kind != CLIENT_SURFACE_COMPLETION_NONE)
    {
        /* kind identifies the host completion source, while external_result
         * identifies who consumed it.  A queued shared monitor has already
         * consumed its one-shot backend event and its supplied result must be
         * used instead of waiting on that event a second time. */
        if (client_surface_completion_result_is_external( &present->completion ))
            completed = external_completed;
        else if (present->completion.kind == CLIENT_SURFACE_COMPLETION_SHARED)
            completed = client_surface_wait_present_completion( surface, present, timeout ).status ==
                        CLIENT_SURFACE_COMPLETION_SIGNALED;
        else
            completed = FALSE;
    }
    else if (completed)
        completed = present->target == CLIENT_SURFACE_FRAME_TARGET_ONSCREEN;
    if (!completed && present->completion.kind == CLIENT_SURFACE_COMPLETION_SHARED)
    {
        /* A failed submission, terminal poll failure or expired frame can
         * still produce delayed native damage. Retire its one-shot monitor
         * before releasing driver_completion_count. Short PENDING polls do
         * not enter this path and must never rearm or abandon the monitor. */
        client_surface_backend_abandon_completion( surface );
    }
    if (!completed && present->completion.kind != CLIENT_SURFACE_COMPLETION_NONE)
        present->result = CLIENT_SURFACE_FRAME_COMPLETION_FAILED;
    return completed;
}

BOOL client_surface_complete_present_locked( struct client_surface *surface,
                                             struct client_surface_frame *present,
                                             BOOL submitted, BOOL external_completed,
                                             const SIZE *expected_size, DWORD timeout )
{
    struct client_surface_completed_frame frame = {0};
    BOOL handed_off = FALSE, source_valid = FALSE;
    BOOL completed;

    TRACE( "completing source %s serial %s snapshot %dx%d submitted %u external %u target %u\n",
           debugstr_client_surface( surface ), wine_dbgstr_longlong( present->serial ),
           (int)present->capture.size.cx, (int)present->capture.size.cy, submitted, external_completed, present->target );

    completed = client_surface_finish_host_completion( surface, present, submitted, external_completed, timeout );
    if (present->result != CLIENT_SURFACE_FRAME_PENDING) completed = FALSE;
    if (completed && present->completion.kind != CLIENT_SURFACE_COMPLETION_NONE &&
        client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_GENERATION_HANDOFF ))
        completed = source_valid = client_surface_capture_frame( surface, present, expected_size, &frame );
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
    if (source_valid && present->handoff_control &&
        present->result != CLIENT_SURFACE_FRAME_SUPERSEDED)
    {
        handed_off = client_surface_publish_handoff_locked( surface, present, &frame );
        if (!handed_off && present->result != CLIENT_SURFACE_FRAME_SUPERSEDED)
            client_surface_abandon_handoff_locked( surface, present );
    }
    if (handed_off) completed = TRUE;
    if (completed && !handed_off && !source_valid && present->result != CLIENT_SURFACE_FRAME_SUPERSEDED)
    {
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
        BOOL wake = FALSE;

        /* Target updates can retire the mapping as soon as this last token
         * disappears. Serialize that transition and detachment with them. */
        pthread_mutex_lock( &surface->present_lock );
        assert( InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ) > 0 );
        if (present->completion.kind == CLIENT_SURFACE_COMPLETION_SHARED)
        {
            assert( surface->driver_completion_count > 0 );
            if (!--surface->driver_completion_count) wake = TRUE;
        }
        if (!InterlockedDecrement( &surface->external_completion_count )) wake = TRUE;
        memset( &present->completion, 0, sizeof(present->completion) );
        memset( &present->capture, 0, sizeof(present->capture) );
        client_surface_handoff_completed( surface );
        pthread_mutex_unlock( &surface->present_lock );
        if (wake) pthread_cond_broadcast( &surface->completion_cond );
    }
    return completed;
}

static struct client_surface_completion_result wait_deferred_driver_completion( void *context, DWORD timeout )
{
    struct client_surface *surface = context;

    return surface->backend->completion->wait( surface, timeout );
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
    struct client_surface_completion_job *job;
    BOOL ret;

    /* An armed driver monitor has exclusive ownership through
     * completion_lock.  Transfer that ownership to the same bounded queue as
     * explicit GLX/EGL/Vulkan completion IDs instead of blocking the caller. */
    if (submitted && present->completion.kind == CLIENT_SURFACE_COMPLETION_SHARED &&
        !client_surface_completion_result_is_external( &present->completion ) && timeout)
    {
        client_surface_add_ref( surface );
        client_surface_set_present_completion( present, wait_deferred_driver_completion,
                                               release_deferred_driver_completion, surface );
        client_surface_defer_present( surface, present, expected_size );
        return TRUE;
    }

    job = present->completion_job;
    present->completion_job = NULL;
    client_surface_lock_present( surface );
    ret = client_surface_complete_present_locked( surface, present, submitted,
                                                  external_completed, expected_size, timeout );
    client_surface_unlock_present( surface );
    client_surface_cancel_completion( job );
    return ret;
}

void client_surface_present( struct client_surface *surface )
{
    struct client_surface_frame present;

    /* Compatibility path for drivers whose presentation callback already
     * supplies a host completion boundary.  It still participates in target
     * token validation and per-surface submission serialization. */
    if (!client_surface_prepare_present( surface, &present, TRUE, FALSE )) return;
    client_surface_begin_present( surface );
    client_surface_submit_present( surface, &present );
    client_surface_complete_present( surface, &present, TRUE, TRUE, NULL, 0 );
}
