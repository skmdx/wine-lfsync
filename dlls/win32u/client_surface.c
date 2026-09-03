/*
 * Client-rendered window surface management
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
#include "ntgdi_private.h"
#include "ntuser_private.h"
#include "wine/server.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(win);

static pthread_mutex_t surfaces_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t surface_index_lock = PTHREAD_MUTEX_INITIALIZER;
static struct list client_surfaces = LIST_INIT( client_surfaces ); /* non-owning used client surfaces */
static struct list unused_surfaces = LIST_INIT( unused_surfaces ); /* owning unused client surfaces */
static unsigned int unused_surface_count;
static UINT64 unused_surface_bytes;

#define CLIENT_SURFACE_INDEX_BUCKETS 256
static struct client_surface *client_surface_identity_index[CLIENT_SURFACE_INDEX_BUCKETS];
static struct client_surface *client_surface_toplevel_index[CLIENT_SURFACE_INDEX_BUCKETS];
static UINT_PTR client_surface_identity;

static void client_surface_backend_destroy( struct client_surface *surface )
{
    if (surface->backend->destroy) surface->backend->destroy( surface );
}

static void client_surface_backend_detach( struct client_surface *surface )
{
    if (surface->backend->detach) surface->backend->detach( surface );
}

static BOOL client_surface_backend_update( struct client_surface *surface )
{
    return !surface->backend->update || surface->backend->update( surface );
}

static BOOL client_surface_backend_present( struct client_surface *surface, HDC hdc,
                                            HRGN surface_region, BOOL flush )
{
    return !surface->backend->present ||
           surface->backend->present( surface, hdc, surface_region, flush );
}

static unsigned int client_surface_backend_state_flags( const struct client_surface *surface )
{
    unsigned int flags = 0;

    if (client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_SCENE_PUBLICATION ))
        flags |= CLIENT_SURFACE_STATE_SCENE_PUBLICATION;
    if (client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_NATIVE_WRITE_LEASE ))
        flags |= CLIENT_SURFACE_STATE_NATIVE_WRITE_LEASE;
    return flags;
}

static void remove_client_surface_chain( struct client_surface **head,
                                         struct client_surface *surface, BOOL identity );

static unsigned int client_surface_index_hash( UINT_PTR value )
{
    value ^= value >> 17;
    value ^= value >> 9;
    return value & (CLIENT_SURFACE_INDEX_BUCKETS - 1);
}

static void assign_client_surface_identity_locked( struct client_surface *surface )
{
    struct client_surface *cursor;
    unsigned int identity_bucket;

    /* The server returns this token through a queued message.  A pointer
     * identity lets a delayed message address an unrelated allocation after
     * malloc reuses the old surface, so allocate an opaque live-unique token
     * instead.  The index lookup below also makes wraparound safe while any
     * colliding token is still live. */
    do
    {
        if (!(++client_surface_identity)) ++client_surface_identity;
        identity_bucket = client_surface_index_hash( client_surface_identity );
        for (cursor = client_surface_identity_index[identity_bucket]; cursor;
             cursor = cursor->identity_next)
            if (cursor->identity == client_surface_identity) break;
    } while (cursor);
    surface->identity = client_surface_identity;
    surface->identity_next = client_surface_identity_index[identity_bucket];
    client_surface_identity_index[identity_bucket] = surface;
}

static void insert_client_surface_index( struct client_surface *surface )
{
    pthread_mutex_lock( &surface_index_lock );
    assign_client_surface_identity_locked( surface );
    surface->indexed_toplevel = surface->ready_toplevel;
    if (surface->ready_toplevel)
    {
        unsigned int bucket = client_surface_index_hash( (UINT_PTR)surface->ready_toplevel );
        surface->toplevel_next = client_surface_toplevel_index[bucket];
        client_surface_toplevel_index[bucket] = surface;
    }
    pthread_mutex_unlock( &surface_index_lock );
}

/* A non-cached unregister ends the server lifetime of the token even when the
 * client object itself is reused.  Move it to a fresh identity before another
 * registration so a delayed queue entry can only miss, never target the new
 * lifecycle.  present_lock excludes registration while the index changes. */
static void renew_client_surface_identity( struct client_surface *surface )
{
    unsigned int bucket;

    pthread_mutex_lock( &surface_index_lock );
    bucket = client_surface_index_hash( surface->identity );
    remove_client_surface_chain( &client_surface_identity_index[bucket], surface, TRUE );
    surface->identity_next = NULL;
    assign_client_surface_identity_locked( surface );
    pthread_mutex_unlock( &surface_index_lock );
}

static void remove_client_surface_chain( struct client_surface **head,
                                         struct client_surface *surface,
                                         BOOL identity )
{
    struct client_surface **cursor;

    for (cursor = head; *cursor; cursor = identity ? &(*cursor)->identity_next
                                                   : &(*cursor)->toplevel_next)
    {
        if (*cursor != surface) continue;
        *cursor = identity ? surface->identity_next : surface->toplevel_next;
        return;
    }
    assert( 0 );
}

static void remove_client_surface_index_locked( struct client_surface *surface )
{
    unsigned int identity_bucket = client_surface_index_hash( surface->identity );

    remove_client_surface_chain( &client_surface_identity_index[identity_bucket], surface, TRUE );
    if (surface->indexed_toplevel)
    {
        unsigned int bucket = client_surface_index_hash( (UINT_PTR)surface->indexed_toplevel );
        remove_client_surface_chain( &client_surface_toplevel_index[bucket], surface, FALSE );
    }
    surface->identity_next = surface->toplevel_next = NULL;
    surface->indexed_toplevel = NULL;
}

/* present_lock protects the native target while this atomically moves the
 * lock-free geometry snapshot between top-level index buckets. */
static void publish_client_surface_geometry( struct client_surface *surface, HWND toplevel,
                                             const RECT *virtual_rect, const RECT *monitor_rect )
{
    unsigned int bucket;

    pthread_mutex_lock( &surface_index_lock );
    if (surface->indexed_toplevel)
    {
        bucket = client_surface_index_hash( (UINT_PTR)surface->indexed_toplevel );
        remove_client_surface_chain( &client_surface_toplevel_index[bucket], surface, FALSE );
    }

    InterlockedIncrement64( &surface->geometry_seq );
    surface->ready_toplevel = toplevel;
    if (virtual_rect) surface->ready_virtual_rect = *virtual_rect;
    if (monitor_rect) surface->ready_monitor_rect = *monitor_rect;
    InterlockedIncrement64( &surface->geometry_seq );

    surface->indexed_toplevel = toplevel;
    surface->toplevel_next = NULL;
    if (toplevel)
    {
        bucket = client_surface_index_hash( (UINT_PTR)toplevel );
        surface->toplevel_next = client_surface_toplevel_index[bucket];
        client_surface_toplevel_index[bucket] = surface;
    }
    pthread_mutex_unlock( &surface_index_lock );
}

#define MAX_UNUSED_CLIENT_SURFACES 64
#define MAX_UNUSED_CLIENT_SURFACE_BYTES (256 * 1024 * 1024)

static UINT64 get_client_surface_cache_cost( const struct client_surface *surface )
{
    const RECT *rect = surface->raw ? &surface->ready_monitor_rect : &surface->ready_virtual_rect;
    LONGLONG signed_width = (LONGLONG)rect->right - rect->left;
    LONGLONG signed_height = (LONGLONG)rect->bottom - rect->top;
    UINT64 width = max( (LONGLONG)0, signed_width );
    UINT64 height = max( (LONGLONG)0, signed_height );

    /* The cached native window retains at least one 32-bpp image.  Cap the
     * estimate above the total budget; exact accounting beyond that point is
     * unnecessary because this entry must be evicted. */
    if (width && height > (MAX_UNUSED_CLIENT_SURFACE_BYTES + 1) / 4 / width)
        return MAX_UNUSED_CLIENT_SURFACE_BYTES + 1;
    return min( width * height * 4, (UINT64)MAX_UNUSED_CLIENT_SURFACE_BYTES + 1 );
}

static void add_unused_client_surface_locked( struct client_surface *surface )
{
    surface->cache_cost = get_client_surface_cache_cost( surface );
    unused_surface_bytes += surface->cache_cost;
    unused_surface_count++;
}

static void remove_unused_client_surface_locked( struct client_surface *surface )
{
    assert( unused_surface_count );
    assert( unused_surface_bytes >= surface->cache_cost );
    unused_surface_count--;
    unused_surface_bytes -= surface->cache_cost;
    surface->cache_cost = 0;
}

static HWND set_client_surface_server_state( HWND hwnd, const struct client_surface *surface,
                                             UINT flags, UINT64 generation,
                                             UINT64 scene_generation,
                                             BOOL *wake )
{
    HWND toplevel = 0;

    if (wake) *wake = FALSE;
    SERVER_START_REQ( set_client_surface_state )
    {
        req->handle = wine_server_user_handle( hwnd );
        req->surface = surface ? surface->identity : 0;
        req->flags = flags;
        req->generation = generation;
        req->scene_generation = scene_generation;
        if (!wine_server_call( req ))
        {
            toplevel = wine_server_ptr_handle( reply->toplevel );
            if (wake) *wake = reply->wake;
        }
    }
    SERVER_END_REQ;
    return toplevel;
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

static void client_surface_uncache_present_locked( struct client_surface *surface )
{
    HWND toplevel;
    BOOL wake;

    if (InterlockedCompareExchange( &surface->server_cached, FALSE, TRUE ) != TRUE) return;
    toplevel = set_client_surface_server_state( surface->hwnd, surface,
                                                CLIENT_SURFACE_STATE_UNCACHE, 0, 0, &wake );
    if (wake && toplevel) NtUserPostMessage( toplevel, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
}

static void client_surface_wait_driver_completion_locked( struct client_surface *surface )
{
    while (surface->driver_completion_count)
        pthread_cond_wait( &surface->completion_cond, &surface->completion_lock );
}

static void client_surface_wait_all_completions_locked( struct client_surface *surface )
{
    while (InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ))
        pthread_cond_wait( &surface->completion_cond, &surface->completion_lock );
}

static void client_surface_detach_locked( struct client_surface *surface )
{
    HWND toplevel;
    BOOL wake;
    UINT flags = 0;

    client_surface_lock_present( surface );
    client_surface_wait_driver_completion_locked( surface );
    pthread_mutex_lock( &surface->present_lock );
    if (!surface->hwnd)
    {
        pthread_mutex_unlock( &surface->present_lock );
        client_surface_unlock_present( surface );
        return;
    }
    InterlockedIncrement( &surface->lifecycle_seq );

    if (surface->active)
    {
        flags |= CLIENT_SURFACE_STATE_UNREGISTER;
        InterlockedExchange( &surface->active, FALSE );
        InterlockedExchange( &surface->producer_claimed, FALSE );
    }
    if (surface->server_cached)
    {
        flags |= CLIENT_SURFACE_STATE_UNCACHE;
        InterlockedExchange( &surface->server_cached, FALSE );
    }
    if (flags)
    {
        toplevel = set_client_surface_server_state( surface->hwnd, surface, flags, 0, 0, &wake );
        if (wake && toplevel) NtUserPostMessage( toplevel, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
    }
    list_remove( &surface->entry );
    client_surface_backend_detach( surface );
    surface->toplevel = NULL;
    InterlockedIncrement64( &surface->target_epoch );
    publish_client_surface_geometry( surface, NULL, NULL, NULL );
    InterlockedExchangePointer( (void **)&surface->hwnd, NULL );
    InterlockedIncrement( &surface->lifecycle_seq );
    pthread_mutex_unlock( &surface->present_lock );
    client_surface_unlock_present( surface );
}

static void client_surface_release_locked( struct client_surface *surface )
{
    ULONG ref;

    /* Index lookups acquire a reference while holding surface_index_lock.
     * Remove the object under the same lock when consuming its final
     * reference, otherwise a lookup can resurrect ref from zero between the
     * decrement and index removal and race the destructor. */
    pthread_mutex_lock( &surface_index_lock );
    ref = InterlockedDecrement( &surface->ref );
    if (!ref) remove_client_surface_index_locked( surface );
    pthread_mutex_unlock( &surface_index_lock );
    TRACE( "%s decreasing refcount to %u\n", debugstr_client_surface( surface ), ref );

    if (!ref)
    {
        client_surface_detach_locked( surface );
        if (surface->clip_region) NtGdiDeleteObjectApp( surface->clip_region );
        assert( list_empty( &surface->completion_queue ) );
        assert( !surface->completion_worker_active );
        assert( !surface->external_completion_count );
        assert( !surface->driver_completion_count );
        assert( !surface->driver_completion_waiters );
        client_surface_backend_destroy( surface );
        pthread_mutex_destroy( &surface->completion_queue_lock );
        pthread_cond_destroy( &surface->completion_cond );
        pthread_mutex_destroy( &surface->completion_lock );
        pthread_mutex_destroy( &surface->present_lock );
        free( surface );
    }
}

static void trim_unused_client_surfaces_locked(void)
{
    while (unused_surface_count > MAX_UNUSED_CLIENT_SURFACES ||
           unused_surface_bytes > MAX_UNUSED_CLIENT_SURFACE_BYTES)
    {
        struct client_surface *surface = LIST_ENTRY( list_tail( &unused_surfaces ),
                                                     struct client_surface, entry );

        list_remove( &surface->entry );
        list_init( &surface->entry );
        remove_unused_client_surface_locked( surface );

        /* Retire server membership immediately even if a queued recomposition
         * still owns a temporary reference.  The final release then destroys
         * the native drawable without allowing the cache to grow unbounded. */
        pthread_mutex_lock( &surface->present_lock );
        client_surface_uncache_present_locked( surface );
        pthread_mutex_unlock( &surface->present_lock );
        client_surface_release_locked( surface );
    }
}

void detach_client_surfaces( HWND hwnd )
{
    struct client_surface *surface, *next;

    pthread_mutex_lock( &surfaces_lock );

    LIST_FOR_EACH_ENTRY_SAFE( surface, next, &client_surfaces, struct client_surface, entry )
        if (surface->hwnd == hwnd) client_surface_detach_locked( surface );
    LIST_FOR_EACH_ENTRY_SAFE( surface, next, &unused_surfaces, struct client_surface, entry )
    {
        if (surface->hwnd != hwnd) continue;
        remove_unused_client_surface_locked( surface );
        client_surface_detach_locked( surface );
        client_surface_release_locked( surface );
    }

    pthread_mutex_unlock( &surfaces_lock );
}

void detach_client_surface_identity( UINT_PTR identity )
{
    struct client_surface *surface, *next;

    pthread_mutex_lock( &surfaces_lock );

    LIST_FOR_EACH_ENTRY_SAFE( surface, next, &client_surfaces, struct client_surface, entry )
    {
        if (surface->identity != identity) continue;
        client_surface_detach_locked( surface );
        goto done;
    }
    LIST_FOR_EACH_ENTRY_SAFE( surface, next, &unused_surfaces, struct client_surface, entry )
    {
        if (surface->identity != identity) continue;
        remove_unused_client_surface_locked( surface );
        client_surface_detach_locked( surface );
        client_surface_release_locked( surface ); /* unused-list ownership */
        break;
    }

done:
    pthread_mutex_unlock( &surfaces_lock );
}

static BOOL get_client_surface_rects( HWND toplevel, HWND hwnd, RECT *virtual_rect,
                                      RECT *monitor_rect )
{
    struct ratio dpi = get_dpi_for_window( hwnd ), raw_dpi;
    struct window_rects rects, monitor_rects;
    RECT rect = {0};

    if (!toplevel) toplevel = NtUserGetAncestor( hwnd, GA_ROOT );
    if (!toplevel || !dpi.num || !get_window_rects( toplevel, COORDS_PARENT, &rects, dpi ))
        return FALSE;
    monitor_rects = map_window_rects_virt_to_raw( rects, dpi );

    if (get_present_rect( hwnd, &rect, dpi )) OffsetRect( &rect, -rects.client.left, -rects.client.top );
    else
    {
        if (!get_client_rect( hwnd, &rect, dpi )) return FALSE;
        map_window_points( hwnd, toplevel, (POINT *)&rect, 2, dpi );
    }

    get_win_monitor_dpi( hwnd, &raw_dpi );
    if (!raw_dpi.num) return FALSE;
    *monitor_rect = map_dpi_rect( rect, dpi, raw_dpi );

    /* use toplevel visible rect relative position, so drivers can then assume it */
    OffsetRect( monitor_rect, monitor_rects.client.left - monitor_rects.visible.left,
                monitor_rects.client.top - monitor_rects.visible.top );
    OffsetRect( &rect, rects.client.left - rects.visible.left,
                rects.client.top - rects.visible.top );

    *virtual_rect = rect;
    return TRUE;
}

struct client_surface_geometry
{
    HWND toplevel;
    RECT virtual_rect;
    RECT monitor_rect;
};

static void get_client_surface_geometry( const struct client_surface *surface,
                                         struct client_surface_geometry *geometry )
{
    LONG64 seq;

    do
    {
        while ((seq = ReadNoFence64( &surface->geometry_seq )) & 1) YieldProcessor();
        __SHARED_READ_FENCE;
        geometry->toplevel = surface->ready_toplevel;
        geometry->virtual_rect = surface->ready_virtual_rect;
        geometry->monitor_rect = surface->ready_monitor_rect;
        __SHARED_READ_FENCE;
    } while (seq != ReadNoFence64( &surface->geometry_seq ));
}

static BOOL client_surface_update_present_locked( struct client_surface *surface )
{
    RECT virtual_rect = surface->virtual_rect, monitor_rect = surface->monitor_rect;
    HWND toplevel = surface->toplevel;
    RECT old_source_rect = surface->raw ? monitor_rect : virtual_rect;
    RECT new_virtual_rect, new_monitor_rect;
    HWND new_toplevel;
    RECT new_source_rect;
    BOOL changed, offscreen, publish_changed, ready;

    new_toplevel = NtUserGetAncestor( surface->hwnd, GA_ROOT );
    if (!new_toplevel || !get_client_surface_rects( new_toplevel, surface->hwnd,
                                                    &new_virtual_rect, &new_monitor_rect ))
    {
        if (InterlockedCompareExchange( &surface->target_ready, FALSE, TRUE ))
            InterlockedIncrement64( &surface->target_epoch );
        InterlockedExchange( &surface->content_valid, FALSE );
        return FALSE;
    }
    new_source_rect = surface->raw ? new_monitor_rect : new_virtual_rect;
    changed = new_toplevel != toplevel || !EqualRect( &new_virtual_rect, &virtual_rect ) ||
              !EqualRect( &new_monitor_rect, &monitor_rect );
    publish_changed = new_toplevel != surface->ready_toplevel ||
                      !EqualRect( &new_virtual_rect, &surface->ready_virtual_rect ) ||
                      !EqualRect( &new_monitor_rect, &surface->ready_monitor_rect );
    offscreen = InterlockedCompareExchange( &surface->offscreen, 0, 0 );

    surface->toplevel = new_toplevel;
    surface->virtual_rect = new_virtual_rect;
    surface->monitor_rect = new_monitor_rect;

    /* A larger drawable contains pixels for which no completed application
     * frame exists yet.  Do not treat its old intersection as a complete
     * frame merely because it can be copied by the host driver. */
    if (new_source_rect.right - new_source_rect.left > old_source_rect.right - old_source_rect.left ||
        new_source_rect.bottom - new_source_rect.top > old_source_rect.bottom - old_source_rect.top)
    {
        InterlockedExchange( &surface->content_valid, FALSE );
        /* An unused cached drawable has no renderer left to fill the newly
         * exposed extent.  Keeping it registered would leave every staged
         * generation waiting for a frame which cannot arrive. */
        if (!InterlockedCompareExchange( &surface->active, 0, 0 ))
            client_surface_uncache_present_locked( surface );
    }

    TRACE( "updating %s, toplevel %p, virtual_rect %s, monitor_rect %s\n", debugstr_client_surface( surface ), surface->toplevel,
           wine_dbgstr_rect( &surface->virtual_rect ), wine_dbgstr_rect( &surface->monitor_rect ) );
    ready = client_surface_backend_update( surface );

    if (publish_changed || offscreen != InterlockedCompareExchange( &surface->offscreen, 0, 0 ) ||
        ready != InterlockedCompareExchange( &surface->target_ready, 0, 0 ))
        InterlockedIncrement64( &surface->target_epoch );
    InterlockedExchange( &surface->target_ready, ready );

    if (!ready)
    {
        InterlockedExchange( &surface->content_valid, FALSE );
        return FALSE;
    }

    /* Publish geometry only after the driver has resized/reparented its native
     * drawable.  Lock-free readers must never observe a new extent paired with
     * the previous X11/EGL or Wayland surface state. */
    if (publish_changed)
    {
        publish_client_surface_geometry( surface, new_toplevel, &new_virtual_rect,
                                         &new_monitor_rect );
        if (changed) InterlockedExchange( &surface->updated, TRUE );
    }
    return TRUE;
}

/* completion_lock is held.  A completed driver wait calls this before
 * retiring its own token, so only external target mutators wait below. */
static BOOL client_surface_update_now_locked( struct client_surface *surface )
{
    BOOL ret = FALSE;

    pthread_mutex_lock( &surface->present_lock );
    if (InterlockedCompareExchangePointer( (void **)&surface->hwnd, NULL, NULL ))
        ret = client_surface_update_present_locked( surface );
    pthread_mutex_unlock( &surface->present_lock );
    return ret;
}

static BOOL client_surface_update_now( struct client_surface *surface )
{
    BOOL ret;

    client_surface_lock_present( surface );
    client_surface_wait_driver_completion_locked( surface );
    ret = client_surface_update_now_locked( surface );
    client_surface_unlock_present( surface );
    return ret;
}

static BOOL client_surface_recompose( struct client_surface *surface );
static void drain_client_surface_recompose( struct client_surface *surface );

static BOOL add_exposed_client_surface_region( HRGN *exposed_region, const RECT *old_rect,
                                               const RECT *new_rect, BOOL visible )
{
    HRGN old_region, new_region = 0;

    if (IsRectEmpty( old_rect )) return TRUE;
    if (!(old_region = NtGdiCreateRectRgn( old_rect->left, old_rect->top,
                                           old_rect->right, old_rect->bottom )))
        return FALSE;

    if (visible && !IsRectEmpty( new_rect ))
    {
        if (!(new_region = NtGdiCreateRectRgn( new_rect->left, new_rect->top,
                                               new_rect->right, new_rect->bottom )))
        {
            NtGdiDeleteObjectApp( old_region );
            return FALSE;
        }
        NtGdiCombineRgn( old_region, old_region, new_region, RGN_DIFF );
        NtGdiDeleteObjectApp( new_region );
    }

    if (!*exposed_region)
        *exposed_region = old_region;
    else
    {
        NtGdiCombineRgn( *exposed_region, *exposed_region, old_region, RGN_OR );
        NtGdiDeleteObjectApp( old_region );
    }
    return TRUE;
}

static BOOL queue_client_surface_recompose( struct client_surface *surface,
                                            struct client_surface ***surfaces,
                                            UINT *count, UINT *size )
{
    struct client_surface **new_surfaces;

    InterlockedIncrement64( &surface->recompose_requested );
    if (InterlockedCompareExchange( &surface->recompose_scheduled, TRUE, FALSE )) return TRUE;

    if (*count == *size)
    {
        UINT new_size = *size ? *size * 2 : 4;

        if (!(new_surfaces = realloc( *surfaces, new_size * sizeof(**surfaces) )))
        {
            InterlockedExchange( &surface->recompose_scheduled, FALSE );
            return FALSE;
        }
        *surfaces = new_surfaces;
        *size = new_size;
    }

    client_surface_add_ref( surface );
    (*surfaces)[(*count)++] = surface;
    return TRUE;
}

static BOOL queue_client_surface_update( struct client_surface *surface,
                                         struct client_surface ***surfaces,
                                         UINT *count, UINT *size )
{
    struct client_surface **new_surfaces;

    if (*count == *size)
    {
        UINT new_size = *size ? *size * 2 : 8;

        if (!(new_surfaces = realloc( *surfaces, new_size * sizeof(**surfaces) ))) return FALSE;
        *surfaces = new_surfaces;
        *size = new_size;
    }

    client_surface_add_ref( surface );
    (*surfaces)[(*count)++] = surface;
    return TRUE;
}

static BOOL collect_indexed_client_surfaces( HWND toplevel, struct client_surface ***surfaces,
                                             UINT *count, UINT *size )
{
    unsigned int bucket = client_surface_index_hash( (UINT_PTR)toplevel );
    struct client_surface *surface;
    BOOL ret = TRUE;

    pthread_mutex_lock( &surface_index_lock );
    for (surface = client_surface_toplevel_index[bucket]; surface; surface = surface->toplevel_next)
    {
        if (surface->indexed_toplevel != toplevel ||
            !InterlockedCompareExchange( &surface->active, 0, 0 ))
            continue;
        if (!queue_client_surface_update( surface, surfaces, count, size ))
        {
            ret = FALSE;
            break;
        }
    }
    pthread_mutex_unlock( &surface_index_lock );
    return ret;
}

static struct client_surface *find_client_surface_identity( UINT_PTR identity )
{
    unsigned int bucket = client_surface_index_hash( identity );
    struct client_surface *surface;

    pthread_mutex_lock( &surface_index_lock );
    for (surface = client_surface_identity_index[bucket]; surface; surface = surface->identity_next)
    {
        if (surface->identity != identity) continue;
        if (!InterlockedCompareExchange( &surface->active, 0, 0 ) &&
            !InterlockedCompareExchange( &surface->server_cached, 0, 0 ))
            surface = NULL;
        else
            client_surface_add_ref( surface );
        break;
    }
    pthread_mutex_unlock( &surface_index_lock );
    return surface;
}

void update_client_surfaces( HWND hwnd )
{
    struct client_surface *surface, *next;
    struct client_surface **update_surfaces = NULL;
    struct client_surface **recompose_surfaces = NULL;
    HRGN exposed_region = 0;
    UINT count = 0, update_count = 0, update_size = 0;
    UINT recompose_count = 0, recompose_size = 0, i;

    if (!collect_indexed_client_surfaces( hwnd, &update_surfaces, &update_count, &update_size ))
    {
        WARN( "failed to allocate client surface update list\n" );
    }

    for (i = 0; i < update_count; ++i)
    {
        RECT monitor_rect, new_monitor_rect;
        HWND surface_hwnd, toplevel, new_toplevel;
        BOOL visible;

        surface = update_surfaces[i];
        client_surface_lock_present( surface );
        client_surface_wait_driver_completion_locked( surface );
        pthread_mutex_lock( &surface->present_lock );
        surface_hwnd = InterlockedCompareExchangePointer( (void **)&surface->hwnd, NULL, NULL );
        if (!surface_hwnd || NtUserGetAncestor( surface_hwnd, GA_ROOT ) != hwnd)
        {
            pthread_mutex_unlock( &surface->present_lock );
            client_surface_unlock_present( surface );
            continue;
        }
        monitor_rect = surface->monitor_rect;
        toplevel = surface->toplevel;
        client_surface_update_present_locked( surface );
        new_monitor_rect = surface->monitor_rect;
        new_toplevel = surface->toplevel;
        visible = NtUserIsWindowVisible( surface_hwnd );
        pthread_mutex_unlock( &surface->present_lock );
        client_surface_unlock_present( surface );

        if (new_toplevel == toplevel && !EqualRect( &new_monitor_rect, &monitor_rect ) &&
            !add_exposed_client_surface_region( &exposed_region, &monitor_rect,
                                                &new_monitor_rect, visible ))
            WARN( "failed to allocate exposed client surface region\n" );
    }

    if (exposed_region)
    {
        for (i = 0; i < update_count; ++i)
        {
            struct client_surface_geometry geometry;
            HWND surface_hwnd;

            surface = update_surfaces[i];
            get_client_surface_geometry( surface, &geometry );
            surface_hwnd = InterlockedCompareExchangePointer( (void **)&surface->hwnd, NULL, NULL );
            if (!surface_hwnd || geometry.toplevel != hwnd || !NtUserIsWindowVisible( surface_hwnd ) ||
                !NtGdiRectInRegion( exposed_region, &geometry.monitor_rect ))
                continue;
            if (!queue_client_surface_recompose( surface, &recompose_surfaces,
                                                 &recompose_count, &recompose_size ))
            {
                WARN( "failed to allocate exposed client surface list\n" );
                break;
            }
        }
    }

    pthread_mutex_lock( &surfaces_lock );
    /* discard extra unused surfaces when updating window */
    LIST_FOR_EACH_ENTRY_SAFE( surface, next, &unused_surfaces, struct client_surface, entry )
    {
        if (surface->hwnd != hwnd || !count++) continue;
        /* Drop the list's owning reference immediately.  A clip snapshot may
         * still hold the object alive, but it must no longer be reusable. */
        list_remove( &surface->entry );
        list_init( &surface->entry );
        remove_unused_client_surface_locked( surface );
        client_surface_release_locked( surface );
    }
    for (i = 0; i < update_count; ++i) client_surface_release_locked( update_surfaces[i] );

    pthread_mutex_unlock( &surfaces_lock );
    if (exposed_region) NtGdiDeleteObjectApp( exposed_region );
    free( update_surfaces );

    /* Publish cached content through the normal generation protocol.  A raw
     * driver copy here can repair the pixels while leaving a staged menu
     * publication out of sync with the server. */
    for (i = 0; i < recompose_count; ++i)
    {
        TRACE( "recomposing newly exposed %s from cached content\n",
               debugstr_client_surface( recompose_surfaces[i] ) );
        drain_client_surface_recompose( recompose_surfaces[i] );
        client_surface_release( recompose_surfaces[i] );
    }
    free( recompose_surfaces );
}

void *client_surface_create( UINT size, const struct client_surface_backend *backend, HWND hwnd, int format, BOOL raw )
{
    HWND toplevel = NtUserGetAncestor( hwnd, GA_ROOT );
    struct client_surface *surface;

    if (!backend || size < sizeof(*surface) ||
        !!backend->prepare_completion != !!backend->wait_completion)
        return NULL;
    if (!(surface = calloc( 1, size ))) return NULL;
    if (pthread_mutex_init( &surface->present_lock, NULL ))
    {
        free( surface );
        return NULL;
    }
    if (pthread_mutex_init( &surface->completion_lock, NULL ))
    {
        pthread_mutex_destroy( &surface->present_lock );
        free( surface );
        return NULL;
    }
    if (pthread_cond_init( &surface->completion_cond, NULL ))
    {
        pthread_mutex_destroy( &surface->completion_lock );
        pthread_mutex_destroy( &surface->present_lock );
        free( surface );
        return NULL;
    }
    if (pthread_mutex_init( &surface->completion_queue_lock, NULL ))
    {
        pthread_cond_destroy( &surface->completion_cond );
        pthread_mutex_destroy( &surface->completion_lock );
        pthread_mutex_destroy( &surface->present_lock );
        free( surface );
        return NULL;
    }
    surface->backend = backend;
    surface->ref = 1;
    surface->hwnd = hwnd;
    surface->format = format;
    surface->raw = raw;
    surface->cacheable = TRUE;
    surface->toplevel = toplevel;
    if (!get_client_surface_rects( toplevel, hwnd, &surface->virtual_rect, &surface->monitor_rect ))
        surface->virtual_rect = surface->monitor_rect = (RECT){0};
    surface->ready_toplevel = surface->toplevel;
    surface->ready_virtual_rect = surface->virtual_rect;
    surface->ready_monitor_rect = surface->monitor_rect;
    list_init( &surface->entry );
    list_init( &surface->completion_queue );
    insert_client_surface_index( surface );

    TRACE( "created %s, format %d, raw %u, toplevel %p, virtual_rect %s, monitor_rect %s\n", debugstr_client_surface( surface ),
           format, raw, toplevel, wine_dbgstr_rect( &surface->virtual_rect ), wine_dbgstr_rect( &surface->monitor_rect ) );
    return surface;
}

void client_surface_add_ref( struct client_surface *surface )
{
    ULONG ref = InterlockedIncrement( &surface->ref );
    TRACE( "%s increasing refcount to %u\n", debugstr_client_surface( surface ), ref );
}

void client_surface_release( struct client_surface *surface )
{
    pthread_mutex_lock( &surfaces_lock );
    client_surface_release_locked( surface );
    pthread_mutex_unlock( &surfaces_lock );
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

static BOOL get_client_surface_publication( struct client_surface *surface, UINT64 *generation,
                                            UINT64 *scene_generation, HWND *scene_toplevel,
                                            BOOL *authoritative )
{
    struct object_lock top_lock = OBJECT_LOCK_INIT, producer_lock = OBJECT_LOCK_INIT;
    const window_shm_t *top_shm = NULL, *producer_shm = NULL;
    process_id_t producer_process = 0;
    client_ptr_t producer_id = 0;
    HWND hwnd, toplevel;
    NTSTATUS status;
    UINT64 current_generation = 0, current_scene_generation = 0;
    BOOL preparing = FALSE;

    if (generation) *generation = 0;
    if (scene_generation) *scene_generation = 0;
    if (scene_toplevel) *scene_toplevel = 0;
    if (authoritative) *authoritative = FALSE;

    hwnd = InterlockedCompareExchangePointer( (void **)&surface->hwnd, NULL, NULL );
    if (!hwnd || (!InterlockedCompareExchange( &surface->active, 0, 0 ) &&
                  !InterlockedCompareExchange( &surface->server_cached, 0, 0 )))
        return FALSE;

    if (!(toplevel = NtUserGetAncestor( hwnd, GA_ROOT ))) return FALSE;
    while ((status = get_shared_window( toplevel, &top_lock, &top_shm )) == STATUS_PENDING)
    {
        current_generation = (top_shm->client_surface_flags & WINDOW_SHM_CLIENT_SURFACE_COMPOSING) ?
                             top_shm->client_surface_generation : 0;
        current_scene_generation = top_shm->client_surface_scene_generation;
        preparing = !!(top_shm->client_surface_flags & WINDOW_SHM_CLIENT_SURFACE_PREPARING);
        if (hwnd == toplevel)
        {
            producer_process = top_shm->client_surface_process;
            producer_id = top_shm->client_surface_id;
        }
    }
    if (status || (current_scene_generation & 1)) return FALSE;

    while (hwnd != toplevel &&
           (status = get_shared_window( hwnd, &producer_lock, &producer_shm )) == STATUS_PENDING)
    {
        producer_process = producer_shm->client_surface_process;
        producer_id = producer_shm->client_surface_id;
    }
    if (status) return FALSE;

    if (generation) *generation = current_generation;
    if (scene_generation) *scene_generation = current_scene_generation;
    if (scene_toplevel) *scene_toplevel = toplevel;
    if (authoritative)
        *authoritative = producer_process == HandleToULong( NtCurrentTeb()->ClientId.UniqueProcess ) &&
                         producer_id == surface->identity;
    return !preparing;
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

static BOOL client_surface_end_present_internal( struct client_surface *surface, UINT64 generation,
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

    /* The caller owns a surface reference and completion_lock.  present_lock
     * serializes detach, membership transitions and native target changes, so
     * the process-wide registry lock is neither needed for lifetime nor for
     * target validation on the per-frame path. */
    pthread_mutex_lock( &surface->present_lock );
    if (present && (!present->target_ready ||
        present->target_epoch != ReadAcquire64( &surface->target_epoch ) ||
        present->lifecycle_seq != ReadAcquire( &surface->lifecycle_seq ) ||
        present->scene_toplevel != surface->ready_toplevel))
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
    if (compose && present && !present->scene_valid) compose = FALSE;
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
        copied = client_surface_backend_present( surface, hdc, surface_region, sync || leased );
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
        toplevel = set_client_surface_server_state( hwnd, surface, server_flags,
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
    get_client_surface_geometry( surface, &geometry );
    if (geometry.toplevel) client_surface_geometry_ready( geometry.toplevel );
}

void client_surface_prepare_present_locked( struct client_surface *surface,
                                            struct client_surface_present *present,
                                            BOOL external_completion )
{
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
    present->scene_valid = get_client_surface_publication( surface, &present->generation,
                                                           &present->scene_generation,
                                                           &present->scene_toplevel,
                                                           &present->authoritative );
    if (surface->hwnd &&
        (!present->scene_valid || !surface->target_ready ||
         surface->target_scene_toplevel != present->scene_toplevel ||
         surface->target_scene_generation != present->scene_generation))
    {
        client_surface_update_present_locked( surface );
        present->scene_valid = get_client_surface_publication( surface, &present->generation,
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
    if (present->target_ready && present->offscreen && !present->external_completion &&
        surface->backend->prepare_completion)
        present->driver_completion = surface->backend->prepare_completion( surface );
    pthread_mutex_unlock( &surface->present_lock );
}

void client_surface_prepare_present( struct client_surface *surface,
                                     struct client_surface_present *present,
                                     BOOL external_completion )
{
    client_surface_lock_present( surface );
    client_surface_prepare_present_locked( surface, present, external_completion );
}

static void register_external_completion_locked( struct client_surface *surface,
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
        register_external_completion_locked( surface, present );
}

void client_surface_submit_present( struct client_surface *surface,
                                    struct client_surface_present *present )
{
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
        if (surface->backend->abandon_completion)
            surface->backend->abandon_completion( surface );
        present->completion_failed = TRUE;
    }
    if (completed && present->offscreen)
    {
        if (present->external_completion)
            completed = external_completed;
        else if (present->driver_completion && surface->backend->wait_completion)
            completed = surface->backend->wait_completion( surface, timeout );
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
            toplevel = set_client_surface_server_state( hwnd, surface,
                                                        CLIENT_SURFACE_STATE_CLAIM,
                                                        0, 0, &wake );
            if (wake && toplevel)
                NtUserPostMessage( toplevel, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
            present->scene_valid = get_client_surface_publication( surface,
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

    return surface->backend->wait_completion( surface, timeout );
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
    if (submitted && present->driver_completion && present->offscreen &&
        surface->backend->wait_completion && timeout)
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

struct client_surface_completion_job
{
    struct list entry;
    struct client_surface_present present;
    client_surface_completion_wait_func wait;
    client_surface_completion_release_func release;
    void *context;
    SIZE expected_size;
    DWORD submission_time;
    BOOL has_expected_size;
    BOOL submitted;
};

#define CLIENT_SURFACE_MAX_DEFERRED_PRESENTS 64
#define CLIENT_SURFACE_MAX_PROCESS_DEFERRED_PRESENTS 1024
static LONG client_surface_deferred_present_count;

void client_surface_begin_inline_completion( struct client_surface *surface,
                                             struct client_surface_present *present )
{
    /* Inline fallback still owns a causal completion boundary.  Publish that
     * ownership under completion_lock so cached replay and a shared driver
     * monitor cannot pass it merely because no FIFO node was allocated. */
    assert( present->external_completion );
    if (!present->completion_locked)
    {
        client_surface_lock_present( surface );
        present->completion_locked = TRUE;
    }
    register_external_completion_locked( surface, present );
    present->completion_locked = FALSE;
    client_surface_unlock_present( surface );
}

static void CALLBACK client_surface_completion_worker( void *context )
{
    struct client_surface *surface = context;
    struct client_surface_completion_job *job;

    for (;;)
    {
        DWORD elapsed, remaining;
        BOOL completed;

        pthread_mutex_lock( &surface->completion_queue_lock );
        if (list_empty( &surface->completion_queue ))
        {
            surface->completion_worker_active = FALSE;
            pthread_mutex_unlock( &surface->completion_queue_lock );
            client_surface_release( surface );
            return;
        }
        job = LIST_ENTRY( list_head( &surface->completion_queue ),
                          struct client_surface_completion_job, entry );
        list_remove( &job->entry );
        pthread_mutex_unlock( &surface->completion_queue_lock );

        elapsed = NtGetTickCount() - job->submission_time;
        remaining = elapsed < CLIENT_SURFACE_PRESENT_TIMEOUT ?
                    CLIENT_SURFACE_PRESENT_TIMEOUT - elapsed : 0;
        completed = job->submitted && job->wait( job->context, remaining );
        if (!client_surface_complete_present( surface, &job->present, job->submitted,
                                              completed,
                                              job->has_expected_size ? &job->expected_size : NULL,
                                              0 ) && job->submitted &&
            !job->present.superseded && !job->present.completion_failed)
            WARN( "deferred client-surface composition did not complete for %s\n",
                  debugstr_client_surface( surface ) );
        job->release( job->context );
        InterlockedDecrement( &client_surface_deferred_present_count );
        free( job );
    }
}

/* NtCreateThreadEx enters its start routine with the Windows ABI, while this
 * Unix library normally uses the host ABI.  Keep the actual worker in the
 * host ABI and use a narrow bridge on architectures where the argument
 * registers differ. */
#if defined(__x86_64__)
static void __attribute__((ms_abi)) client_surface_completion_thread( void *context )
#elif defined(__i386__)
static void __attribute__((stdcall)) client_surface_completion_thread( void *context )
#else
static void client_surface_completion_thread( void *context )
#endif
{
    client_surface_completion_worker( context );
}

void client_surface_defer_present( struct client_surface *surface,
                                   struct client_surface_present *present,
                                   BOOL submitted, const SIZE *expected_size,
                                   client_surface_completion_wait_func wait,
                                   client_surface_completion_release_func release,
                                   void *context )
{
    struct client_surface_completion_job *job;
    BOOL completed, start_worker = FALSE;
    DWORD elapsed, remaining;

    assert( present->external_completion );
    assert( wait && release );

    if (!(job = malloc( sizeof(*job) )))
    {
        client_surface_begin_inline_completion( surface, present );
        elapsed = NtGetTickCount() - present->submission_time;
        remaining = elapsed < CLIENT_SURFACE_PRESENT_TIMEOUT ?
                    CLIENT_SURFACE_PRESENT_TIMEOUT - elapsed : 0;
        completed = submitted && wait( context, remaining );
        client_surface_complete_present( surface, present, submitted, completed,
                                         expected_size, 0 );
        release( context );
        return;
    }
    if (InterlockedIncrement( &client_surface_deferred_present_count ) >
        CLIENT_SURFACE_MAX_PROCESS_DEFERRED_PRESENTS)
    {
        InterlockedDecrement( &client_surface_deferred_present_count );
        free( job );
        client_surface_begin_inline_completion( surface, present );
        elapsed = NtGetTickCount() - present->submission_time;
        remaining = elapsed < CLIENT_SURFACE_PRESENT_TIMEOUT ?
                    CLIENT_SURFACE_PRESENT_TIMEOUT - elapsed : 0;
        completed = submitted && wait( context, remaining );
        client_surface_complete_present( surface, present, submitted, completed,
                                         expected_size, 0 );
        release( context );
        return;
    }
    job->wait = wait;
    job->release = release;
    job->context = context;
    job->submission_time = present->submission_time;
    job->submitted = submitted;
    job->has_expected_size = !!expected_size;
    if (expected_size) job->expected_size = *expected_size;

    /* Register queue ownership while excluding cached replay.  A native
     * driver monitor may transfer an already-held completion lock; explicit
     * completion IDs acquire it only for this ownership hand-off. */
    if (!present->completion_locked)
    {
        client_surface_lock_present( surface );
        present->completion_locked = TRUE;
    }
    register_external_completion_locked( surface, present );
    job->present = *present;
    pthread_mutex_lock( &surface->completion_queue_lock );
    if (InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ) >=
        CLIENT_SURFACE_MAX_DEFERRED_PRESENTS)
    {
        pthread_mutex_unlock( &surface->completion_queue_lock );
        client_surface_begin_inline_completion( surface, present );
        elapsed = NtGetTickCount() - present->submission_time;
        remaining = elapsed < CLIENT_SURFACE_PRESENT_TIMEOUT ?
                    CLIENT_SURFACE_PRESENT_TIMEOUT - elapsed : 0;
        completed = submitted && wait( context, remaining );
        client_surface_complete_present( surface, present, submitted, completed,
                                         expected_size, 0 );
        release( context );
        InterlockedDecrement( &client_surface_deferred_present_count );
        free( job );
        return;
    }
    job->present.completion_locked = FALSE;
    list_add_tail( &surface->completion_queue, &job->entry );
    if (!surface->completion_worker_active)
    {
        surface->completion_worker_active = TRUE;
        client_surface_add_ref( surface );
        start_worker = TRUE;
    }
    pthread_mutex_unlock( &surface->completion_queue_lock );
    present->completion_locked = FALSE;
    client_surface_unlock_present( surface );

    if (start_worker)
    {
        HANDLE thread;
        NTSTATUS status;

        status = NtCreateThreadEx( &thread, THREAD_ALL_ACCESS, NULL, NtCurrentProcess(),
                                   (PRTL_THREAD_START_ROUTINE)client_surface_completion_thread,
                                   surface, 0, 0, 0, 0, NULL );
        if (status)
        {
            /* Thread allocation failure is rare.  Drain this FIFO inline so
             * every external completion token still has exactly one owner. */
            WARN( "Failed to create client-surface completion worker, status %#lx\n",
                  (unsigned long)status );
            client_surface_completion_worker( surface );
        }
        else NtClose( thread );
    }
    memset( present, 0, sizeof(*present) );
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

static BOOL client_surface_recompose( struct client_surface *surface )
{
    struct client_surface_present present;

    /* Cached replay reads the same native drawable that a deferred host
     * presentation updates.  Do not let an older cached frame commit the
     * composition epoch ahead of the queued producer. */
    InterlockedExchange( &surface->recompose_deferred, TRUE );
    if (pthread_mutex_trylock( &surface->completion_lock )) return FALSE;
    if (InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ))
    {
        /* Keep deferred and scheduled ownership until the completion worker
         * reaches the last queued causal boundary. */
        pthread_mutex_unlock( &surface->completion_lock );
        return FALSE;
    }
    InterlockedExchange( &surface->recompose_deferred, FALSE );
    client_surface_prepare_present_locked( surface, &present, TRUE );
    client_surface_end_present_internal( surface, present.generation, NULL, FALSE, &present );
    client_surface_unlock_present( surface );
    return TRUE;
}

static void drain_client_surface_recompose( struct client_surface *surface )
{
    LONG64 requested;

    for (;;)
    {
        requested = ReadAcquire64( &surface->recompose_requested );
        if (!client_surface_recompose( surface )) return;
        if (requested != ReadAcquire64( &surface->recompose_requested )) continue;

        InterlockedExchange( &surface->recompose_scheduled, FALSE );
        if (requested == ReadAcquire64( &surface->recompose_requested )) break;

        /* A producer which observes the cleared flag owns the next queue
         * entry.  Otherwise retain this entry's reference and consume the
         * latest request without replaying every intermediate generation. */
        if (InterlockedCompareExchange( &surface->recompose_scheduled, TRUE, FALSE )) break;
    }
}

void recompose_client_surface( HWND hwnd, UINT_PTR identity )
{
    struct client_surface *selected;
    struct client_surface_geometry geometry;
    HWND surface_hwnd;
    HWND toplevel = NtUserGetAncestor( hwnd, GA_ROOT );

    if (!(selected = find_client_surface_identity( identity ))) return;
    /* A cross-process geometry notification can be the first observation of
     * a reparent.  Refresh before validating the indexed top-level, otherwise
     * the old bucket would make the exact notification reject itself. */
    client_surface_update_now( selected );
    get_client_surface_geometry( selected, &geometry );
    surface_hwnd = InterlockedCompareExchangePointer( (void **)&selected->hwnd, NULL, NULL );
    if (!surface_hwnd || geometry.toplevel != toplevel || !NtUserIsWindowVisible( surface_hwnd ) ||
        (!InterlockedCompareExchange( &selected->active, 0, 0 ) &&
         (!InterlockedCompareExchange( &selected->server_cached, 0, 0 ) ||
          !InterlockedCompareExchange( &selected->content_valid, 0, 0 ))))
    {
        /* The queued notification is generation-neutral, but its HWND names
         * the top-level which owned the surface when it was posted.  Queue
         * removal has released notification_pending; immediately route the
         * current generation after a cross-top-level reparent instead of
         * leaving the new hierarchy to its publication timeout. */
        if (surface_hwnd && geometry.toplevel && geometry.toplevel != toplevel)
            client_surface_geometry_ready( geometry.toplevel );
        client_surface_release( selected );
        return;
    }

    InterlockedIncrement64( &selected->recompose_requested );
    if (InterlockedCompareExchange( &selected->recompose_scheduled, TRUE, FALSE ))
    {
        client_surface_release( selected );
        return;
    }

    TRACE( "recomposing geometry-ready %s from cached content\n",
           debugstr_client_surface( selected ) );
    drain_client_surface_recompose( selected );
    client_surface_release( selected );
}

void client_surface_geometry_ready( HWND hwnd )
{
    HWND toplevel;
    BOOL wake;

    toplevel = set_client_surface_server_state( hwnd, NULL,
                                                CLIENT_SURFACE_STATE_GEOMETRY_READY, 0, 0, &wake );
    if (wake && toplevel) NtUserPostMessage( toplevel, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
}

void client_surface_set_staged( HWND hwnd )
{
    HWND toplevel;
    BOOL wake;

    TRACE( "client surface composition for %p is staged\n", hwnd );
    toplevel = set_client_surface_server_state( hwnd, NULL, CLIENT_SURFACE_STATE_STAGED, 0, 0, &wake );
    if (wake && toplevel) NtUserPostMessage( toplevel, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
}

void client_surface_bypass_staging( HWND hwnd )
{
    set_client_surface_server_state( hwnd, NULL, CLIENT_SURFACE_STATE_BYPASS, 0, 0, NULL );
}

BOOL client_surface_begin_native_barrier( HWND hwnd, UINT_PTR token, UINT *writers )
{
    BOOL ret = FALSE;

    *writers = 0;
    SERVER_START_REQ( set_client_surface_state )
    {
        req->handle = wine_server_user_handle( hwnd );
        req->surface = token;
        req->flags = CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN;
        req->generation = 0;
        req->scene_generation = 0;
        if (!wine_server_call( req ))
        {
            *writers = reply->pending;
            ret = TRUE;
        }
    }
    SERVER_END_REQ;
    return ret;
}

BOOL client_surface_end_native_barrier( HWND hwnd, UINT_PTR token )
{
    BOOL ret = FALSE;

    SERVER_START_REQ( set_client_surface_state )
    {
        req->handle = wine_server_user_handle( hwnd );
        req->surface = token;
        req->flags = CLIENT_SURFACE_STATE_NATIVE_BARRIER_END;
        req->generation = 0;
        req->scene_generation = 0;
        ret = !wine_server_call( req );
    }
    SERVER_END_REQ;
    return ret;
}

BOOL client_surface_begin_publish( HWND hwnd, UINT64 *generation, UINT64 *scene_generation )
{
    BOOL publish = FALSE;

    *generation = 0;
    *scene_generation = 0;
    SERVER_START_REQ( set_client_surface_state )
    {
        req->handle = wine_server_user_handle( hwnd );
        req->surface = 0;
        req->flags = CLIENT_SURFACE_STATE_PUBLISH_BEGIN;
        req->generation = 0;
        req->scene_generation = 0;
        if (!wine_server_call( req ) && reply->publish)
        {
            *generation = reply->generation;
            *scene_generation = reply->scene_generation;
            publish = TRUE;
        }
    }
    SERVER_END_REQ;
    return publish;
}

void client_surface_end_publish( HWND hwnd, UINT64 generation, UINT64 scene_generation )
{
    set_client_surface_server_state( hwnd, NULL, CLIENT_SURFACE_STATE_PUBLISH_COMMIT,
                                     generation, scene_generation, NULL );
}

BOOL client_surface_begin_prepare( HWND hwnd, UINT64 *scene_generation )
{
    BOOL prepare = FALSE;

    *scene_generation = 0;
    SERVER_START_REQ( set_client_surface_state )
    {
        req->handle = wine_server_user_handle( hwnd );
        req->surface = 0;
        req->flags = CLIENT_SURFACE_STATE_PREPARE_BEGIN;
        req->generation = 0;
        req->scene_generation = 0;
        if (!wine_server_call( req ) && reply->publish)
        {
            *scene_generation = reply->scene_generation;
            prepare = TRUE;
        }
    }
    SERVER_END_REQ;
    return prepare;
}

void client_surface_end_prepare( HWND hwnd, UINT64 scene_generation )
{
    set_client_surface_server_state( hwnd, NULL, CLIENT_SURFACE_STATE_PREPARE_COMMIT,
                                     0, scene_generation, NULL );
}

BOOL client_surface_update( struct client_surface *surface )
{
    UINT64 scene_generation;
    HWND scene_toplevel;
    BOOL scene_valid, ret = FALSE;

    client_surface_lock_present( surface );
    client_surface_wait_driver_completion_locked( surface );
    pthread_mutex_lock( &surface->present_lock );
    scene_valid = get_client_surface_publication( surface, NULL, &scene_generation,
                                                  &scene_toplevel, NULL );
    if (scene_valid && surface->target_ready &&
        surface->target_scene_toplevel == scene_toplevel &&
        surface->target_scene_generation == scene_generation)
        ret = TRUE;
    else if (surface->hwnd)
    {
        ret = client_surface_update_present_locked( surface );
        scene_valid = get_client_surface_publication( surface, NULL, &scene_generation,
                                                      &scene_toplevel, NULL );
        if (ret && scene_valid)
        {
            surface->target_scene_toplevel = scene_toplevel;
            surface->target_scene_generation = scene_generation;
        }
    }
    pthread_mutex_unlock( &surface->present_lock );
    client_surface_unlock_present( surface );
    return ret;
}

BOOL client_surface_get_size( struct client_surface *surface, SIZE *virtual_size, SIZE *monitor_size )
{
    struct client_surface_geometry geometry;
    BOOL updated;

    updated = InterlockedExchange( &surface->updated, FALSE );
    get_client_surface_geometry( surface, &geometry );

    virtual_size->cx = max( 1, geometry.virtual_rect.right - geometry.virtual_rect.left );
    virtual_size->cy = max( 1, geometry.virtual_rect.bottom - geometry.virtual_rect.top );
    monitor_size->cx = max( 1, geometry.monitor_rect.right - geometry.monitor_rect.left );
    monitor_size->cy = max( 1, geometry.monitor_rect.bottom - geometry.monitor_rect.top );

    return updated;
}

void use_window_client_surface( struct client_surface *surface, BOOL use )
{
    HWND hwnd = 0, toplevel = 0;
    BOOL cache = FALSE, invalid = FALSE, renew_identity = FALSE, wake = FALSE;
    UINT flags;

    TRACE( "surface %s, use %u\n", debugstr_client_surface( surface ), use );
    if (use) client_surface_update_now( surface );

    pthread_mutex_lock( &surfaces_lock );
    pthread_mutex_lock( &surface->present_lock );

    if (!surface->hwnd)
        WARN( "surface %s has been detached already, ignoring.\n", debugstr_client_surface( surface ) );
    else if (use)
    {
        /* surface wasn't used, it shouldn't be in any list */
        list_add_tail( &client_surfaces, &surface->entry );
        InterlockedExchange( &surface->active, TRUE );
        flags = CLIENT_SURFACE_STATE_REGISTER | client_surface_backend_state_flags( surface );
        if (surface->server_cached)
        {
            flags |= CLIENT_SURFACE_STATE_UNCACHE;
            InterlockedExchange( &surface->server_cached, FALSE );
        }
        hwnd = surface->hwnd;
    }
    else
    {
        list_remove( &surface->entry ); /* remove it from client_surfaces, if it was used */
        if (InterlockedCompareExchange( &surface->cacheable, 0, 0 ))
        {
            list_add_head( &unused_surfaces, &surface->entry );
            add_unused_client_surface_locked( surface );
            client_surface_add_ref( surface );
            cache = TRUE;
        }
        else list_init( &surface->entry );
        flags = CLIENT_SURFACE_STATE_UNREGISTER;
        if (cache && InterlockedCompareExchange( &surface->content_valid, 0, 0 ))
        {
            flags |= CLIENT_SURFACE_STATE_CACHE | client_surface_backend_state_flags( surface );
            InterlockedExchange( &surface->server_cached, TRUE );
        }
        else if (InterlockedCompareExchange( &surface->server_cached, 0, 0 ))
        {
            /* Reusing a cached surface invalidates its old drawable before a
             * replacement is created.  If creation fails and the surface is
             * returned unused, it must no longer participate in staged
             * generations because there is no complete frame to recompose. */
            flags |= CLIENT_SURFACE_STATE_UNCACHE;
            InterlockedExchange( &surface->server_cached, FALSE );
        }
        if (!(flags & CLIENT_SURFACE_STATE_CACHE))
        {
            InterlockedExchange( &surface->producer_claimed, FALSE );
            renew_identity = TRUE;
        }
        /* Publish the cached ownership before retiring the active ownership.
         * Lock-free begin_present() must not observe a gap between the two;
         * end_present() waits on present_lock until the server transition has
         * completed before it can acknowledge the sampled generation. */
        InterlockedExchange( &surface->active, FALSE );
        hwnd = surface->hwnd;
    }

    pthread_mutex_unlock( &surfaces_lock );

    if (hwnd)
    {
        toplevel = set_client_surface_server_state( hwnd, surface, flags, 0, 0, &wake );
        if (wake && toplevel) NtUserPostMessage( toplevel, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
        /* A renderer process does not receive the owning process's local
         * destroy callback for a foreign HWND.  If the server no longer has
         * that window, do not leave its owning unused-list reference cached
         * forever.  Check validity separately because a zero reply can also
         * be caused by an allocation failure while the HWND is still alive. */
        if (!toplevel && !NtUserIsWindow( hwnd )) invalid = TRUE;
    }
    /* Keep the old token when the server request failed.  Re-registering the
     * same identity can then repair client/server membership instead of
     * leaking an unreachable active ref until process teardown. */
    if (renew_identity && toplevel) renew_client_surface_identity( surface );
    pthread_mutex_unlock( &surface->present_lock );

    if (invalid) detach_client_surfaces( hwnd );
    else if (!use && cache)
    {
        pthread_mutex_lock( &surfaces_lock );
        trim_unused_client_surfaces_locked();
        pthread_mutex_unlock( &surfaces_lock );
    }
}

struct client_surface *get_unused_client_surface( HWND hwnd, int format, BOOL raw )
{
    struct client_surface *surface = NULL, *candidate;

    pthread_mutex_lock( &surfaces_lock );

    LIST_FOR_EACH_ENTRY( candidate, &unused_surfaces, struct client_surface, entry )
    {
        if (candidate->hwnd != hwnd || candidate->format != format || candidate->raw != raw) continue;
        surface = candidate;
        client_surface_lock_present( surface );
        client_surface_wait_all_completions_locked( surface );
        pthread_mutex_lock( &surface->present_lock );
        list_remove( &surface->entry ); /* take over its reference */
        list_init( &surface->entry );
        remove_unused_client_surface_locked( surface );
        /* A queued cached recomposition may still hold a reference after the
         * list entry is removed.  Invalidate its old frame while serialized
         * with presentation so it cannot copy from the replacement drawable. */
        InterlockedExchange( &surface->content_valid, FALSE );
        break;
    }

    pthread_mutex_unlock( &surfaces_lock );

    if (surface)
    {
        client_surface_uncache_present_locked( surface );
        if (InterlockedCompareExchangePointer( (void **)&surface->hwnd, NULL, NULL ))
            client_surface_update_present_locked( surface ); /* refresh before creating GL/VK drawable */
        pthread_mutex_unlock( &surface->present_lock );
        client_surface_unlock_present( surface );
        TRACE( "Reusing surface %s\n", debugstr_client_surface( surface ) );
    }
    return surface ? surface : user_driver->pCreateClientSurface( hwnd, format, raw );
}

BOOL is_client_surface_window( struct client_surface *surface, HWND hwnd )
{
    HWND surface_hwnd;

    if (!surface) return FALSE;
    surface_hwnd = InterlockedCompareExchangePointer( (void **)&surface->hwnd, NULL, NULL );
    return hwnd ? surface_hwnd == hwnd : !!surface_hwnd;
}
