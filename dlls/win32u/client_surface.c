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
#include "client_surface.h"
#include "ntuser_private.h"
#include "wine/server.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(win);
WINE_DECLARE_DEBUG_CHANNEL(csperf);

static const struct client_surface_backend default_client_surface_backend;

static pthread_mutex_t surfaces_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t surface_index_lock = PTHREAD_MUTEX_INITIALIZER;
static struct list client_surfaces = LIST_INIT( client_surfaces ); /* non-owning used client surfaces */
static struct list unused_surfaces = LIST_INIT( unused_surfaces ); /* owning unused client surfaces */
static unsigned int unused_surface_count;
static UINT64 unused_surface_bytes;

/* Explicitly allocated client-surface images share a process budget. Native
 * WSI allocations are owned by the host driver and are not estimated here.
 * Retired storage stays charged until its real completion permits release. */
#define CLIENT_SURFACE_IMAGE_MEMORY_LIMIT ((UINT64)1024 * 1024 * 1024)
static pthread_mutex_t image_memory_lock = PTHREAD_MUTEX_INITIALIZER;
static UINT64 image_memory[CLIENT_SURFACE_MEMORY_CLASS_COUNT], image_memory_total;

static void trace_client_surface_memory( const char *event, enum client_surface_memory_class type,
                                         UINT64 bytes, BOOL accepted )
{
    LARGE_INTEGER counter;

    if (!TRACE_ON(csperf)) return;
    NtQueryPerformanceCounter( &counter, NULL );
    TRACE_(csperf)( "ticks=%llu event=%s class=%u bytes=%llu accepted=%u source=%llu staging=%llu output=%llu total=%llu\n",
                   (unsigned long long)counter.QuadPart, event, type, (unsigned long long)bytes, accepted,
                   (unsigned long long)image_memory[CLIENT_SURFACE_MEMORY_SOURCE],
                   (unsigned long long)image_memory[CLIENT_SURFACE_MEMORY_STAGING],
                   (unsigned long long)image_memory[CLIENT_SURFACE_MEMORY_OUTPUT],
                   (unsigned long long)image_memory_total );
}

BOOL client_surface_reserve_memory( enum client_surface_memory_class type, UINT64 bytes )
{
    BOOL ret;

    assert( type < CLIENT_SURFACE_MEMORY_CLASS_COUNT );
    pthread_mutex_lock( &image_memory_lock );
    ret = bytes <= CLIENT_SURFACE_IMAGE_MEMORY_LIMIT - image_memory_total;
    if (ret)
    {
        image_memory[type] += bytes;
        image_memory_total += bytes;
    }
    TRACE( "image reservation class %u bytes %s accepted %u source %s staging %s output %s\n",
           type, wine_dbgstr_longlong( bytes ), ret,
           wine_dbgstr_longlong( image_memory[CLIENT_SURFACE_MEMORY_SOURCE] ),
           wine_dbgstr_longlong( image_memory[CLIENT_SURFACE_MEMORY_STAGING] ),
           wine_dbgstr_longlong( image_memory[CLIENT_SURFACE_MEMORY_OUTPUT] ) );
    trace_client_surface_memory( "image_reserve", type, bytes, ret );
    pthread_mutex_unlock( &image_memory_lock );
    return ret;
}

void client_surface_release_memory( enum client_surface_memory_class type, UINT64 bytes )
{
    assert( type < CLIENT_SURFACE_MEMORY_CLASS_COUNT );
    pthread_mutex_lock( &image_memory_lock );
    assert( image_memory[type] >= bytes && image_memory_total >= bytes );
    image_memory[type] -= bytes;
    image_memory_total -= bytes;
    trace_client_surface_memory( "image_release", type, bytes, TRUE );
    pthread_mutex_unlock( &image_memory_lock );
}

#define CLIENT_SURFACE_INDEX_BUCKETS 256
static struct client_surface *client_surface_identity_index[CLIENT_SURFACE_INDEX_BUCKETS];
static struct client_surface *client_surface_toplevel_index[CLIENT_SURFACE_INDEX_BUCKETS];
static LONG client_surface_process_id;

static void client_surface_backend_destroy( struct client_surface *surface )
{
    if (surface->backend->destroy) surface->backend->destroy( surface );
}

static void client_surface_backend_detach( struct client_surface *surface )
{
    if (surface->backend->detach) surface->backend->detach( surface );
}

static BOOL client_surface_backend_update( struct client_surface *surface,
                                           struct client_surface_target *target,
                                           enum client_surface_target_update *update )
{
    *update = CLIENT_SURFACE_TARGET_UPDATE_DEFAULT;
    return !surface->backend->update || surface->backend->update( surface, target, update );
}

static unsigned int client_surface_backend_state_flags( struct client_surface *surface )
{
    unsigned int flags = 0;

    if (client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_SCENE_PUBLICATION ))
        flags |= CLIENT_SURFACE_STATE_SCENE_PUBLICATION;
    if (client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_DIRECT_PRESENTATION ) &&
        InterlockedCompareExchange( &surface->direct_ready, 0, 0 ))
        flags |= CLIENT_SURFACE_STATE_DIRECT_PRESENTATION;
    return flags;
}

static void remove_client_surface_chain( struct client_surface **head,
                                         struct client_surface *surface, BOOL identity );

static unsigned int client_surface_index_hash( UINT64 value )
{
    value ^= value >> 32;
    value ^= value >> 17;
    value ^= value >> 9;
    return value & (CLIENT_SURFACE_INDEX_BUCKETS - 1);
}

static UINT64 allocate_client_surface_identity(void)
{
    UINT64 identity = 0;

    SERVER_START_REQ( allocate_client_surface )
    {
        if (!wine_server_call( req )) identity = reply->surface;
    }
    SERVER_END_REQ;
    return identity;
}

static void release_client_surface_id( UINT64 identity )
{
    if (!identity) return;
    SERVER_START_REQ( release_client_surface )
    {
        req->surface = identity;
        wine_server_call( req );
    }
    SERVER_END_REQ;
}

static void insert_client_surface_identity_locked( struct client_surface *surface )
{
    unsigned int bucket = client_surface_index_hash( client_surface_get_identity( surface ) );

    assert( client_surface_get_identity( surface ) );
    surface->identity_next = client_surface_identity_index[bucket];
    client_surface_identity_index[bucket] = surface;
}

/* Reserve before registration can make the server send any notification. */
static BOOL ensure_client_surface_identity( struct client_surface *surface )
{
    UINT64 identity;

    if (client_surface_get_identity( surface )) return TRUE;
    if (!(identity = allocate_client_surface_identity())) return FALSE;
    pthread_mutex_lock( &surface_index_lock );
    __atomic_store_n( &surface->identity, identity, __ATOMIC_RELEASE );
    insert_client_surface_identity_locked( surface );
    pthread_mutex_unlock( &surface_index_lock );
    return TRUE;
}

static void insert_client_surface_index( struct client_surface *surface )
{
    pthread_mutex_lock( &surface_index_lock );
    insert_client_surface_identity_locked( surface );
    surface->indexed_toplevel = surface->target.toplevel;
    if (surface->target.toplevel)
    {
        unsigned int bucket = client_surface_index_hash( (UINT_PTR)surface->target.toplevel );
        surface->toplevel_next = client_surface_toplevel_index[bucket];
        client_surface_toplevel_index[bucket] = surface;
    }
    pthread_mutex_unlock( &surface_index_lock );
}

/* End notification lookup before the object can be reused. A later activation
 * reserves a new server lifetime. present_lock excludes registration here. */
static void reset_client_surface_identity( struct client_surface *surface )
{
    unsigned int bucket;
    UINT64 identity = client_surface_get_identity( surface );

    if (!identity) return;
    pthread_mutex_lock( &surface_index_lock );
    bucket = client_surface_index_hash( identity );
    remove_client_surface_chain( &client_surface_identity_index[bucket], surface, TRUE );
    surface->identity_next = NULL;
    __atomic_store_n( &surface->identity, 0, __ATOMIC_RELEASE );
    pthread_mutex_unlock( &surface_index_lock );
    release_client_surface_id( identity );
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
    unsigned int identity_bucket = client_surface_index_hash( client_surface_get_identity( surface ) );

    if (client_surface_get_identity( surface ))
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
static void publish_client_surface_target( struct client_surface *surface,
                                           const struct client_surface_target *target, BOOL preserve_native )
{
    unsigned int bucket;

    pthread_mutex_lock( &surface_index_lock );
    if (surface->indexed_toplevel)
    {
        bucket = client_surface_index_hash( (UINT_PTR)surface->indexed_toplevel );
        remove_client_surface_chain( &client_surface_toplevel_index[bucket], surface, FALSE );
    }

    InterlockedIncrement64( &surface->target.seq );
    if (!preserve_native) ++surface->target.epoch;
    surface->target.toplevel = target->toplevel;
    surface->target.virtual_rect = target->virtual_rect;
    surface->target.monitor_rect = target->monitor_rect;
    surface->target.dpi_num = target->dpi_num;
    surface->target.dpi_den = target->dpi_den;
    surface->target.mode = target->mode;
    surface->target.offscreen = target->offscreen;
    surface->target.valid = target->valid;
    InterlockedIncrement64( &surface->target.seq );

    surface->indexed_toplevel = target->toplevel;
    surface->toplevel_next = NULL;
    if (target->toplevel)
    {
        bucket = client_surface_index_hash( (UINT_PTR)target->toplevel );
        surface->toplevel_next = client_surface_toplevel_index[bucket];
        client_surface_toplevel_index[bucket] = surface;
    }
    pthread_mutex_unlock( &surface_index_lock );
    TRACE( "event=target identity=%s sequence=%s epoch=%s preserved=%u toplevel=%p "
           "position=%d,%d size=%dx%d mode=%u valid=%u\n",
           wine_dbgstr_longlong( client_surface_get_identity( surface ) ),
           wine_dbgstr_longlong( surface->target.seq ), wine_dbgstr_longlong( surface->target.epoch ),
           preserve_native, surface->target.toplevel, (int)surface->target.virtual_rect.left,
           (int)surface->target.virtual_rect.top,
           (int)(surface->target.virtual_rect.right - surface->target.virtual_rect.left),
           (int)(surface->target.virtual_rect.bottom - surface->target.virtual_rect.top),
           surface->target.mode, surface->target.valid );
}

#define MAX_UNUSED_CLIENT_SURFACES 64
#define MAX_UNUSED_CLIENT_SURFACE_BYTES (256 * 1024 * 1024)

static UINT64 get_client_surface_cache_cost( const struct client_surface *surface )
{
    const RECT *rect = surface->raw ? &surface->target.monitor_rect : &surface->target.virtual_rect;
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

HWND client_surface_set_server_state( HWND hwnd, const struct client_surface *surface,
                                      UINT flags, UINT64 generation,
                                      UINT64 scene_generation, BOOL *wake )
{
    HWND toplevel = 0;

    if (wake) *wake = FALSE;
    SERVER_START_REQ( set_client_surface_state )
    {
        req->handle = wine_server_user_handle( hwnd );
        req->surface = surface ? client_surface_get_identity( surface ) : 0;
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

/* DIRECT is a server scene decision, but the native backend owns constraints
 * which the server cannot observe (DPI transforms and native clipping on X11).
 * Publish changes only on geometry/lifecycle updates; steady-state presents
 * consume the shared scene and perform no server request. */
static void client_surface_update_direct_ready_locked( struct client_surface *surface )
{
    BOOL ready = client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_DIRECT_PRESENTATION ) &&
                 (!surface->backend->direct_ready || surface->backend->direct_ready( surface ));
    HWND toplevel;
    BOOL wake;

    if (InterlockedCompareExchange( &surface->direct_ready, ready, !ready ) == ready) return;
    if (!InterlockedCompareExchange( &surface->active, 0, 0 ) &&
        !InterlockedCompareExchange( &surface->server_cached, 0, 0 ))
        return;

    toplevel = client_surface_set_server_state( surface->hwnd, surface,
                                                CLIENT_SURFACE_STATE_UPDATE_CAPS |
                                                client_surface_backend_state_flags( surface ),
                                                0, 0, &wake );
    if (wake && toplevel) NtUserPostMessage( toplevel, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
}

static void client_surface_uncache_present_locked( struct client_surface *surface )
{
    HWND toplevel;
    BOOL wake;

    if (InterlockedCompareExchange( &surface->server_cached, FALSE, TRUE ) != TRUE) return;
    toplevel = client_surface_set_server_state( surface->hwnd, surface,
                                                CLIENT_SURFACE_STATE_UNCACHE, 0, 0, &wake );
    if (!toplevel) InterlockedExchange( &surface->server_cached, TRUE );
    if (wake && toplevel) NtUserPostMessage( toplevel, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
}

void client_surface_invalidate_source_locked( struct client_surface *surface,
                                              const struct client_surface_frame *present )
{
    pthread_mutex_lock( &surface->present_lock );
    if (present->serial > surface->composed_serial)
    {
        /* A failed or stale presentation may still have changed the native
         * source.  Its previous completed contents are no longer reusable,
         * even if a move or resize has since changed the target sequence.
         * Retire the serial too: an older completion arriving afterwards
         * must not make that unproven source valid again. */
        surface->composed_serial = present->serial;
        InterlockedExchange( &surface->content_valid, FALSE );
        if (!InterlockedCompareExchange( &surface->active, 0, 0 ))
        {
            client_surface_uncache_present_locked( surface );
            /* No renderer or cached image can use this handoff again. Retire
             * it independently of the native drawable's final release, which
             * may still wait for the failed frame's GPU work. Completion
             * ownership protects the remaining submitted handoff tokens. */
            if (!InterlockedCompareExchange( &surface->server_cached, 0, 0 ))
                client_surface_release_handoff( surface );
        }
    }
    pthread_mutex_unlock( &surface->present_lock );
}

static void client_surface_wait_driver_completion_locked( struct client_surface *surface )
{
    /* DIRECT has no completion monitor, but the native WSI call still owns
     * the drawable between begin_present() and submit_present().  Shared
     * monitors also own mutable backend state.  Exact GLX completion cannot
     * be waited here: an offscreen completion may itself require the pending
     * show transition to reach the X server. */
    while (surface->native_present_count || surface->driver_completion_count)
        pthread_cond_wait( &surface->completion_cond, &surface->completion_lock );
}

static void client_surface_lock_target( struct client_surface *surface )
{
    /* Publish target-writer intent before contending for completion_lock.
     * Otherwise a producer woken by the preceding completion can repeatedly
     * win the mutex and submit another blocking swap ahead of the window
     * thread which must resize or detach that drawable. */
    InterlockedIncrement( &surface->target_update_waiters );
    client_surface_lock_present( surface );
    client_surface_wait_driver_completion_locked( surface );
}

static void client_surface_unlock_target( struct client_surface *surface )
{
    assert( InterlockedCompareExchange( &surface->target_update_waiters, 0, 0 ) > 0 );
    if (!InterlockedDecrement( &surface->target_update_waiters ))
        pthread_cond_broadcast( &surface->completion_cond );
    client_surface_unlock_present( surface );
}

void client_surface_prepare_scene( struct client_surface *surface )
{
    struct client_surface_scene scene;
    HWND toplevel = 0;
    UINT64 generation;
    BOOL wake = FALSE;

    if (client_surface_get_scene( surface, &scene ) && scene.authoritative)
    {
        /* The owner can plan a strategy-only transition from an acknowledged
         * scene at the actual native submission boundary. Restarting geometry
         * preparation here races every foreign-thread producer against the
         * owner's asynchronous prepare/ACK and can starve DIRECT forever. */
        return;
    }

    /* A first submission can select a new producer and require an owner
     * snapshot. Do that before acquiring a multi-surface submission's locks:
     * preparing the owner may update every surface belonging to it. */
    client_surface_lock_target( surface );
    pthread_mutex_lock( &surface->present_lock );
    client_surface_get_scene( surface, &scene );
    if (surface->hwnd && InterlockedCompareExchange( &surface->active, 0, 0 ))
    {
        if (!scene.authoritative)
            toplevel = client_surface_set_server_state( surface->hwnd, surface,
                CLIENT_SURFACE_STATE_CLAIM, 0, 0, &wake );
        else if (!scene.valid)
            toplevel = scene.toplevel;
    }
    pthread_mutex_unlock( &surface->present_lock );
    client_surface_unlock_target( surface );

    if (toplevel && is_current_thread_window( toplevel ))
    {
        if (client_surface_begin_prepare( toplevel, &generation ) &&
            prepare_window_client_surfaces( toplevel ))
            client_surface_end_prepare( toplevel, generation );
        update_window_state( toplevel );
    }
    else if (wake && toplevel)
        NtUserPostMessage( toplevel, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
}

/* Owner-side geometry changes must not wait for a native present whose host
 * completion can depend on that owner reaching the window system.  Publish a
 * coalesced request first, then take the target only when it is immediately
 * idle.  The current presenter consumes the request when it drops the lock. */
static BOOL client_surface_trylock_target( struct client_surface *surface )
{
    InterlockedExchange( &surface->target_update_pending, TRUE );
    if (pthread_mutex_trylock( &surface->completion_lock )) return FALSE;
    if (surface->native_present_count || surface->driver_completion_count)
    {
        pthread_mutex_unlock( &surface->completion_lock );
        return FALSE;
    }
    InterlockedExchange( &surface->target_update_pending, FALSE );
    InterlockedIncrement( &surface->target_update_waiters );
    return TRUE;
}

static void client_surface_wait_all_completions_locked( struct client_surface *surface )
{
    while (InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ))
        pthread_cond_wait( &surface->completion_cond, &surface->completion_lock );
}

static void client_surface_detach_locked( struct client_surface *surface )
{
    struct client_surface_target target;
    HWND toplevel;
    BOOL wake;
    UINT flags = 0;

    client_surface_lock_target( surface );
    pthread_mutex_lock( &surface->present_lock );
    if (!surface->hwnd)
    {
        pthread_mutex_unlock( &surface->present_lock );
        client_surface_unlock_target( surface );
        return;
    }
    client_surface_get_target( surface, &target );
    target.valid = FALSE;
    publish_client_surface_target( surface, &target, FALSE );
    client_surface_release_handoff( surface );

    if (surface->active)
    {
        flags |= CLIENT_SURFACE_STATE_UNREGISTER;
        InterlockedExchange( &surface->active, FALSE );
    }
    if (surface->server_cached)
    {
        flags |= CLIENT_SURFACE_STATE_UNCACHE;
        InterlockedExchange( &surface->server_cached, FALSE );
    }
    if (flags)
    {
        toplevel = client_surface_set_server_state( surface->hwnd, surface, flags, 0, 0, &wake );
        if (wake && toplevel) NtUserPostMessage( toplevel, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
    }
    list_remove( &surface->entry );
    client_surface_backend_detach( surface );
    target.toplevel = NULL;
    publish_client_surface_target( surface, &target, FALSE );
    InterlockedExchangePointer( (void **)&surface->hwnd, NULL );
    pthread_mutex_unlock( &surface->present_lock );
    client_surface_unlock_target( surface );
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
        release_client_surface_id( client_surface_get_identity( surface ) );
        if (surface->clip_region) NtGdiDeleteObjectApp( surface->clip_region );
        assert( list_empty( &surface->completion_queue ) );
        assert( list_empty( &surface->completion_ready_entry ) );
        assert( !surface->completion_in_progress );
        assert( !surface->external_completion_count );
        assert( !surface->handoff_waiters );
        assert( !surface->driver_completion_count );
        assert( !surface->driver_completion_waiters );
        assert( !surface->native_present_count );
        assert( !surface->target_update_waiters );
        client_surface_backend_destroy( surface );
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

void detach_client_surface_identity( UINT64 identity )
{
    struct client_surface *surface, *next;

    pthread_mutex_lock( &surfaces_lock );

    LIST_FOR_EACH_ENTRY_SAFE( surface, next, &client_surfaces, struct client_surface, entry )
    {
        if (client_surface_get_identity( surface ) != identity) continue;
        client_surface_detach_locked( surface );
        goto done;
    }
    LIST_FOR_EACH_ENTRY_SAFE( surface, next, &unused_surfaces, struct client_surface, entry )
    {
        if (client_surface_get_identity( surface ) != identity) continue;
        remove_unused_client_surface_locked( surface );
        client_surface_detach_locked( surface );
        client_surface_release_locked( surface ); /* unused-list ownership */
        break;
    }

done:
    pthread_mutex_unlock( &surfaces_lock );
}

BOOL get_client_surface_rects( HWND toplevel, HWND hwnd,
                                      struct client_surface_target *target )
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
    target->monitor_rect = map_dpi_rect( rect, dpi, raw_dpi );

    /* use toplevel visible rect relative position, so drivers can then assume it */
    OffsetRect( &target->monitor_rect, monitor_rects.client.left - monitor_rects.visible.left,
                monitor_rects.client.top - monitor_rects.visible.top );
    OffsetRect( &rect, rects.client.left - rects.visible.left,
                rects.client.top - rects.visible.top );

    target->toplevel = toplevel;
    target->virtual_rect = rect;
    target->dpi_num = raw_dpi.num;
    target->dpi_den = raw_dpi.den;
    return TRUE;
}

void client_surface_get_target( const struct client_surface *surface,
                                struct client_surface_target *target )
{
    LONG64 seq;

    do
    {
        while ((seq = ReadNoFence64( &surface->target.seq )) & 1) YieldProcessor();
        __SHARED_READ_FENCE;
        *target = surface->target;
        __SHARED_READ_FENCE;
    } while (seq != ReadNoFence64( &surface->target.seq ));
    target->seq = seq;
}

void client_surface_get_geometry( const struct client_surface *surface,
                                  struct client_surface_geometry *geometry )
{
    struct client_surface_target target;

    client_surface_get_target( surface, &target );

    geometry->toplevel = target.toplevel;
    geometry->virtual_rect = target.virtual_rect;
    geometry->monitor_rect = target.monitor_rect;
}

static BOOL read_client_surface_scene( HWND toplevel, struct client_surface_scene *scene,
                                       process_id_t *producer_process, UINT64 *producer_id )
{
    struct object_lock lock = OBJECT_LOCK_INIT;
    const window_shm_t *window_shm = NULL;
    BOOL preparing = FALSE;
    NTSTATUS status;

    memset( scene, 0, sizeof(*scene) );
    scene->toplevel = toplevel;
    while ((status = get_shared_window( toplevel, &lock, &window_shm )) == STATUS_PENDING)
    {
        scene->generation = (window_shm->client_surface_flags & WINDOW_SHM_CLIENT_SURFACE_COMPOSING) ?
                            window_shm->client_surface_generation : 0;
        scene->epoch = window_shm->client_surface_scene_generation;
        scene->mode = (window_shm->client_surface_flags & WINDOW_SHM_CLIENT_SURFACE_DIRECT) ?
                      CLIENT_SURFACE_PRESENTATION_DIRECT :
                      (window_shm->client_surface_flags & WINDOW_SHM_CLIENT_SURFACE_STAGED) ?
                      CLIENT_SURFACE_PRESENTATION_STAGED : CLIENT_SURFACE_PRESENTATION_COMPOSITED;
        preparing = !!(window_shm->client_surface_flags & WINDOW_SHM_CLIENT_SURFACE_PREPARING);
        scene->source_pending = !!(window_shm->client_surface_flags & WINDOW_SHM_CLIENT_SURFACE_SOURCE_PENDING);
        scene->direct_candidate = !!(window_shm->client_surface_flags & WINDOW_SHM_CLIENT_SURFACE_DIRECT_CANDIDATE);
        scene->publication_pending = !!(window_shm->client_surface_flags & WINDOW_SHM_CLIENT_SURFACE_PUBLISHING);
        if (producer_process) *producer_process = window_shm->client_surface_process;
        if (producer_id) *producer_id = window_shm->client_surface_id;
    }
    if (status) return FALSE;
    /* A publication which started in an older epoch remains COMPOSING until
     * the owner ACK arrives, but it is not a valid target for newer frames.
     * Treat that interval like the odd seqlock phase.  Otherwise a resize
     * storm can keep feeding the compositor frames which the server must
     * reject, delaying the one Present completion needed to start repair. */
    scene->valid = !preparing && !(scene->epoch & 1) &&
                   (!scene->generation || scene->generation == scene->epoch);
    return TRUE;
}

BOOL client_surface_get_toplevel_scene( HWND toplevel, struct client_surface_scene *scene )
{
    return read_client_surface_scene( toplevel, scene, NULL, NULL ) && scene->valid;
}

BOOL client_surface_scene_snapshot_current( HWND toplevel, UINT64 scene_id )
{
    struct client_surface_scene current;

    /* Layout remains readable while native preparation blocks publication. */
    return !(scene_id & 1) && read_client_surface_scene( toplevel, &current, NULL, NULL ) &&
           current.epoch == scene_id;
}

static BOOL read_client_surface_placement( struct client_surface *surface, struct client_surface_scene *scene )
{
    struct object_lock producer_lock = OBJECT_LOCK_INIT;
    const window_shm_t *producer_shm = NULL;
    struct client_surface_target target;
    process_id_t producer_process = 0;
    UINT64 producer_id = 0;
    HWND hwnd, toplevel;
    NTSTATUS status = STATUS_SUCCESS;

    memset( scene, 0, sizeof(*scene) );

    hwnd = InterlockedCompareExchangePointer( (void **)&surface->hwnd, NULL, NULL );
    if (!hwnd || (!InterlockedCompareExchange( &surface->active, 0, 0 ) &&
                  !InterlockedCompareExchange( &surface->server_cached, 0, 0 )))
        return FALSE;

    /* target writers hold completion_lock across a topology change, so its
     * seqlock snapshot is the authoritative root for this presentation.
     * Calling NtUserGetAncestor() here would invert completion_lock and the
     * process USER lock against concurrent SetWindowPos(). */
    client_surface_get_target( surface, &target );
    if (!(toplevel = target.toplevel)) return FALSE;
    if (!read_client_surface_scene( toplevel, scene, &producer_process, &producer_id )) return FALSE;

    while (hwnd != toplevel &&
           (status = get_shared_window( hwnd, &producer_lock, &producer_shm )) == STATUS_PENDING)
    {
        producer_process = producer_shm->client_surface_process;
        producer_id = producer_shm->client_surface_id;
    }
    if (status)
    {
        scene->valid = FALSE;
        return FALSE;
    }
    if (hwnd != toplevel && scene->valid && !client_surface_scene_current( scene ))
        scene->valid = FALSE;
    scene->authoritative = producer_process == (process_id_t)client_surface_process_id &&
                           producer_id == client_surface_get_identity( surface );
    return TRUE;
}

BOOL client_surface_get_scene( struct client_surface *surface, struct client_surface_scene *scene )
{
    return read_client_surface_placement( surface, scene ) && scene->valid;
}

BOOL client_surface_scene_current( const struct client_surface_scene *scene )
{
    struct client_surface_scene current;

    if (!scene->valid || !scene->toplevel ||
        !read_client_surface_scene( scene->toplevel, &current, NULL, NULL ) || !current.valid)
        return FALSE;
    return current.generation == scene->generation && current.epoch == scene->epoch &&
           current.mode == scene->mode;
}

static BOOL client_surface_update_present_scene_internal_locked(
    struct client_surface *surface, const struct client_surface_scene *requested_scene,
    BOOL allow_direct_transition )
{
    struct client_surface_target current, next, invalid;
    RECT old_source_rect, new_source_rect;
    struct client_surface_scene scene;
    enum client_surface_target_update update;
    BOOL changed, defer_direct, ready, scene_valid, preserve_native, preparing_candidate;

    client_surface_get_target( surface, &current );
    next = current;
    next.toplevel = NtUserGetAncestor( surface->hwnd, GA_ROOT );
    if (!next.toplevel || !get_client_surface_rects( next.toplevel, surface->hwnd, &next ))
    {
        if (current.valid)
        {
            invalid = current;
            invalid.valid = FALSE;
            publish_client_surface_target( surface, &invalid, FALSE );
        }
        InterlockedExchange( &surface->content_valid, FALSE );
        return FALSE;
    }
    client_surface_update_direct_ready_locked( surface );
    if (requested_scene)
    {
        scene = *requested_scene;
        /* direct_ready can change the server scene before the native update.
         * Never substitute a second, unrelated snapshot for the one the
         * frame is about to validate.  The caller can resample and retry. */
        if (!scene.valid || !client_surface_scene_current( &scene ))
        {
            surface->target_scene_epoch = 0;
            surface->target_scene_mode = CLIENT_SURFACE_PRESENTATION_INVALID;
            return FALSE;
        }
        scene_valid = TRUE;
    }
    else
        scene_valid = client_surface_get_scene( surface, &scene );
    if (allow_direct_transition && scene_valid && scene.authoritative && scene.direct_candidate &&
        scene.mode != CLIENT_SURFACE_PRESENTATION_DIRECT && surface->backend->prepare_direct)
    {
        /* The owner selects from the final COMPOSING snapshot. Its admission
         * changes the strategy but not the immutable scene. Resample before
         * touching native geometry; never retag a PREPARING snapshot. */
        if (surface->backend->prepare_direct( surface, &scene )) return FALSE;
    }
    /* PREPARING has an immutable layout but no publication token yet. An
     * actual producer which cannot admit DIRECT must still preserve its new
     * image through the private completion/handoff path. Candidate excludes
     * native barriers; the even exact scene and identity remain mandatory.
     * This applies only native geometry, never a valid scene or generation. */
    preparing_candidate = allow_direct_transition && !scene_valid && scene.authoritative &&
        scene.direct_candidate && !scene.generation && scene.epoch && !(scene.epoch & 1) &&
        scene.toplevel == next.toplevel && client_surface_scene_snapshot_current( next.toplevel, scene.epoch );
    next.mode = !scene_valid && !preparing_candidate ? current.mode : scene.authoritative ? scene.mode :
                CLIENT_SURFACE_PRESENTATION_COMPOSITED;
    /* Reparenting an already presented offscreen drawable may discard its
     * front buffer.  Keep the last STAGED/COMPOSITED image visible until an
     * actual producer present is ready to replace it.  Geometry owners still
     * update an established DIRECT target in place. */
    defer_direct = !allow_direct_transition &&
                   ((next.mode == CLIENT_SURFACE_PRESENTATION_DIRECT &&
                     current.mode != CLIENT_SURFACE_PRESENTATION_DIRECT) ||
                    (scene_valid && scene.authoritative && scene.direct_candidate &&
                     scene.mode != CLIENT_SURFACE_PRESENTATION_DIRECT));
    if (defer_direct) next.mode = current.mode;
    old_source_rect = surface->raw ? current.monitor_rect : current.virtual_rect;
    new_source_rect = surface->raw ? next.monitor_rect : next.virtual_rect;
    changed = next.toplevel != current.toplevel ||
              !EqualRect( &next.virtual_rect, &current.virtual_rect ) ||
              !EqualRect( &next.monitor_rect, &current.monitor_rect ) ||
              next.dpi_num != current.dpi_num || next.dpi_den != current.dpi_den;
    if (next.toplevel != current.toplevel) client_surface_release_handoff( surface );

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

    TRACE( "updating %s, toplevel %p, virtual_rect %s, monitor_rect %s\n",
           debugstr_client_surface( surface ), next.toplevel,
           wine_dbgstr_rect( &next.virtual_rect ), wine_dbgstr_rect( &next.monitor_rect ) );
    ready = client_surface_backend_update( surface, &next, &update );

    if (!ready)
    {
        if (current.valid)
        {
            invalid = current;
            invalid.valid = FALSE;
            publish_client_surface_target( surface, &invalid, FALSE );
        }
        InterlockedExchange( &surface->content_valid, FALSE );
        return FALSE;
    }

    next.valid = TRUE;
    /* A backend may preserve an established offscreen target across position
     * changes. Size, DPI, mode, ownership and validity still delimit native
     * lifetimes even if the new snapshot later returns to the old values. */
    preserve_native = update == CLIENT_SURFACE_TARGET_UPDATE_PRESERVED && current.valid &&
        current.offscreen && next.offscreen && next.mode == current.mode &&
        next.toplevel == current.toplevel && next.dpi_num == current.dpi_num && next.dpi_den == current.dpi_den &&
        next.virtual_rect.right - next.virtual_rect.left == current.virtual_rect.right - current.virtual_rect.left &&
        next.virtual_rect.bottom - next.virtual_rect.top == current.virtual_rect.bottom - current.virtual_rect.top &&
        next.monitor_rect.right - next.monitor_rect.left == current.monitor_rect.right - current.monitor_rect.left &&
        next.monitor_rect.bottom - next.monitor_rect.top == current.monitor_rect.bottom - current.monitor_rect.top;
    /* Publish only after the native mutation. Geometry readers retain their
     * seqlock, while completed frames validate the independent native epoch. */
    if (changed || next.mode != current.mode || next.offscreen != current.offscreen ||
        next.valid != current.valid || update == CLIENT_SURFACE_TARGET_UPDATE_CHANGED)
    {
        publish_client_surface_target( surface, &next, preserve_native );
        if (changed) InterlockedExchange( &surface->updated, TRUE );
    }
    if (!defer_direct && scene_valid && client_surface_scene_current( &scene ))
    {
        surface->target_scene_epoch = scene.epoch;
        surface->target_scene_mode = scene.mode;
    }
    else
    {
        surface->target_scene_epoch = 0;
        surface->target_scene_mode = CLIENT_SURFACE_PRESENTATION_INVALID;
    }
    return TRUE;
}

BOOL client_surface_update_present_scene_locked( struct client_surface *surface,
                                                  const struct client_surface_scene *scene,
                                                  BOOL allow_direct_transition )
{
    return client_surface_update_present_scene_internal_locked( surface, scene, allow_direct_transition );
}

BOOL client_surface_update_present_locked( struct client_surface *surface )
{
    return client_surface_update_present_scene_internal_locked( surface, NULL, FALSE );
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

    client_surface_lock_target( surface );
    ret = client_surface_update_now_locked( surface );
    client_surface_unlock_target( surface );
    return ret;
}

void client_surface_apply_pending_update( struct client_surface *surface )
{
    if (!InterlockedCompareExchange( &surface->target_update_pending, 0, 0 ) ||
        !client_surface_trylock_target( surface ))
        return;

    client_surface_update_now_locked( surface );
    client_surface_unlock_target( surface );
}

static BOOL client_surface_recompose( struct client_surface *surface, LONG64 seq );
static void drain_client_surface_recompose( struct client_surface *surface );

static BOOL request_client_surface_recompose( struct client_surface *surface )
{
    InterlockedIncrement64( &surface->recompose_seq );
    return !InterlockedCompareExchange( &surface->recompose_queued, TRUE, FALSE );
}

static void complete_client_surface_recompose( struct client_surface *surface, LONG64 seq )
{
    LONG64 done;

    for (;;)
    {
        done = ReadAcquire64( &surface->recompose_done );
        if ((UINT64)done >= (UINT64)seq) return;
        if (InterlockedCompareExchange64( &surface->recompose_done, seq, done ) == done) return;
    }
}

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

    if (!request_client_surface_recompose( surface )) return TRUE;

    if (*count == *size)
    {
        UINT new_size = *size ? *size * 2 : 4;

        if (!(new_surfaces = realloc( *surfaces, new_size * sizeof(**surfaces) )))
        {
            InterlockedExchange( &surface->recompose_queued, FALSE );
            client_surface_resume_recompose( surface );
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

static struct client_surface *find_client_surface_identity( UINT64 identity )
{
    unsigned int bucket = client_surface_index_hash( identity );
    struct client_surface *surface;

    pthread_mutex_lock( &surface_index_lock );
    for (surface = client_surface_identity_index[bucket]; surface; surface = surface->identity_next)
    {
        if (client_surface_get_identity( surface ) != identity) continue;
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

static BOOL client_surface_owner_handles_exposure( struct client_surface *surface, HWND hwnd, HWND toplevel )
{
    struct client_surface_target target, current;
    struct client_surface_scene scene;

    if (!client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_OWNER_SCENE_PLAN )) return FALSE;
    client_surface_get_target( surface, &target );
    if (!target.valid || !target.offscreen || target.toplevel != toplevel ||
        (target.mode != CLIENT_SURFACE_PRESENTATION_COMPOSITED &&
         target.mode != CLIENT_SURFACE_PRESENTATION_STAGED)) return FALSE;
    /* This routes incidental exposure to the owner, not proof of a warm
     * image. The owner resolves its actual inventory and requests any cold
     * source explicitly. PREPARING blocks publication, not layout reads. */
    if (!read_client_surface_placement( surface, &scene ) || !scene.authoritative ||
        scene.toplevel != toplevel || scene.mode != target.mode ||
        NtUserGetAncestor( hwnd, GA_ROOT ) != toplevel ||
        !client_surface_scene_snapshot_current( toplevel, scene.epoch )) return FALSE;
    client_surface_get_target( surface, &current );
    return current.seq == target.seq;
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
        WARN( "failed to allocate client surface update list\n" );

    for (i = 0; i < update_count; ++i)
    {
        RECT monitor_rect, new_monitor_rect;
        HWND surface_hwnd, toplevel, new_toplevel;
        BOOL visible;

        surface = update_surfaces[i];
        if (!client_surface_trylock_target( surface )) continue;
        pthread_mutex_lock( &surface->present_lock );
        surface_hwnd = InterlockedCompareExchangePointer( (void **)&surface->hwnd, NULL, NULL );
        if (!surface_hwnd || NtUserGetAncestor( surface_hwnd, GA_ROOT ) != hwnd)
        {
            pthread_mutex_unlock( &surface->present_lock );
            client_surface_unlock_target( surface );
            continue;
        }
        monitor_rect = surface->target.monitor_rect;
        toplevel = surface->target.toplevel;
        client_surface_update_present_locked( surface );
        new_monitor_rect = surface->target.monitor_rect;
        new_toplevel = surface->target.toplevel;
        visible = NtUserIsWindowVisible( surface_hwnd );
        pthread_mutex_unlock( &surface->present_lock );
        client_surface_unlock_target( surface );

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
            client_surface_get_geometry( surface, &geometry );
            surface_hwnd = InterlockedCompareExchangePointer( (void **)&surface->hwnd, NULL, NULL );
            if (!surface_hwnd || geometry.toplevel != hwnd || !NtUserIsWindowVisible( surface_hwnd ) ||
                !NtGdiRectInRegion( exposed_region, &geometry.monitor_rect ))
                continue;
            if (client_surface_owner_handles_exposure( surface, surface_hwnd, hwnd ))
            {
                TRACE( "owner scene handles newly exposed %s\n", debugstr_client_surface( surface ) );
                continue;
            }
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

    if (size < sizeof(*surface)) return NULL;
    if (!backend) backend = &default_client_surface_backend;
    if (backend->completion &&
        (!backend->completion->prepare || !backend->completion->wait)) return NULL;
    if (!(surface = calloc( 1, size ))) return NULL;
    if (pthread_mutex_init( &surface->present_lock, NULL )) goto failed_present_lock;
    if (pthread_mutex_init( &surface->completion_lock, NULL )) goto failed_completion_lock;
    if (pthread_cond_init( &surface->completion_cond, NULL )) goto failed_completion_cond;
    if (!(surface->identity = allocate_client_surface_identity())) goto failed_identity;
    surface->backend = backend;
    surface->handoff_ready_fd = -1;
    surface->ref = 1;
    surface->hwnd = hwnd;
    surface->format = format;
    surface->raw = raw;
    surface->cacheable = TRUE;
    surface->target.toplevel = toplevel;
    if (!get_client_surface_rects( toplevel, hwnd, &surface->target ))
        surface->target.virtual_rect = surface->target.monitor_rect = (RECT){0};
    list_init( &surface->entry );
    list_init( &surface->completion_queue );
    list_init( &surface->completion_ready_entry );
    InterlockedCompareExchange( &client_surface_process_id,
                                HandleToULong( NtCurrentTeb()->ClientId.UniqueProcess ), 0 );
    insert_client_surface_index( surface );

    TRACE( "created %s, format %d, raw %u, toplevel %p, virtual_rect %s, monitor_rect %s\n", debugstr_client_surface( surface ),
           format, raw, toplevel, wine_dbgstr_rect( &surface->target.virtual_rect ),
           wine_dbgstr_rect( &surface->target.monitor_rect ) );
    return surface;

failed_identity:
    pthread_cond_destroy( &surface->completion_cond );
failed_completion_cond:
    pthread_mutex_destroy( &surface->completion_lock );
failed_completion_lock:
    pthread_mutex_destroy( &surface->present_lock );
failed_present_lock:
    free( surface );
    return NULL;
}

void client_surface_add_ref( struct client_surface *surface )
{
    ULONG ref = InterlockedIncrement( &surface->ref );
    TRACE( "%s increasing refcount to %u\n", debugstr_client_surface( surface ), ref );
}

void client_surface_release( struct client_surface *surface )
{
    LONG ref = ReadAcquire( &surface->ref );

    /* Registry operations can wait for this surface's completion FIFO while
     * holding surfaces_lock.  A worker must be able to release one job's
     * reference before consuming the next job, even during that wait.
     * Only the final release needs to serialize list and index removal. */
    while (ref > 1)
    {
        LONG previous = InterlockedCompareExchange( &surface->ref, ref - 1, ref );
        if (previous == ref) return;
        ref = previous;
    }

    pthread_mutex_lock( &surfaces_lock );
    client_surface_release_locked( surface );
    pthread_mutex_unlock( &surfaces_lock );
}

static BOOL client_surface_recompose( struct client_surface *surface, LONG64 seq )
{
    struct client_surface_handoff_channel *channel;
    struct client_surface_frame present;
    struct client_surface_completed_frame frame;
    UINT64 produced;
    BOOL handed_off = FALSE;

    /* Cached replay reads the same native drawable that a deferred host
     * presentation updates.  Do not let an older cached frame commit the
     * composition epoch ahead of the queued producer. */
    if (pthread_mutex_trylock( &surface->completion_lock )) return FALSE;
    if (InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ) ||
        surface->native_present_count ||
        InterlockedCompareExchange( &surface->target_update_waiters, 0, 0 ))
    {
        pthread_mutex_unlock( &surface->completion_lock );
        return FALSE;
    }
    channel = surface->handoff_channel;
    if (client_surface_backend_has_cap( surface, CLIENT_SURFACE_BACKEND_OWNER_SCENE_PLAN ) &&
        channel && !__atomic_load_n( &channel->closed, __ATOMIC_ACQUIRE ) &&
        surface->content_valid && surface->composed_serial)
    {
        /* The owner retains its replay cache for the channel's lifetime.
         * Once a completed frame has been handed off, scene changes need
         * neither another native freeze nor publication. Destroying the
         * cache closes the channel so a replacement receives content again. */
        produced = __atomic_load_n( &channel->producer_sequence, __ATOMIC_RELAXED );
        if (channel->slots[(produced - 1) & (CLIENT_SURFACE_HANDOFF_RING_SIZE - 1)].source_sequence ==
            surface->composed_serial)
        {
            complete_client_surface_recompose( surface, seq );
            pthread_mutex_unlock( &surface->completion_lock );
            return TRUE;
        }
    }
    client_surface_prepare_recompose_locked( surface, &present );
    if (present.handoff_control)
    {
        /* Cached replay has no new native submission or completion token.
         * Publish the already completed source through the same generation
         * slot instead of leaking SUBMITTED and falling back to a producer
         * copy/RPC transaction. */
        present.serial = surface->composed_serial;
        if (client_surface_freeze_frame_locked( surface, &present, &frame ))
            handed_off = client_surface_publish_handoff_locked( surface, &present, &frame );
        if (!handed_off) client_surface_abandon_handoff_locked( surface, &present );
    }
    if (!handed_off)
        client_surface_end_present_internal( surface, NULL, FALSE, &present );
    complete_client_surface_recompose( surface, seq );
    /* drain_client_surface_recompose() owns scheduling while this lock is
     * held.  Unlock directly so a request arriving during the replay is
     * consumed by its loop instead of recursively starting another drain. */
    pthread_mutex_unlock( &surface->completion_lock );
    return TRUE;
}

static void drain_client_surface_recompose( struct client_surface *surface )
{
    LONG64 done, requested;

    InterlockedExchange( &surface->recompose_queued, FALSE );
    for (;;)
    {
        done = ReadAcquire64( &surface->recompose_done );
        requested = ReadAcquire64( &surface->recompose_seq );
        if (done == requested || !client_surface_recompose( surface, requested )) return;
    }
}

void client_surface_resume_recompose( struct client_surface *surface )
{
    if (ReadAcquire64( &surface->recompose_done ) ==
        ReadAcquire64( &surface->recompose_seq )) return;
    if (InterlockedCompareExchange( &surface->recompose_queued, TRUE, FALSE )) return;
    drain_client_surface_recompose( surface );
}

void recompose_client_surface( HWND hwnd, UINT64 identity )
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
    client_surface_get_geometry( selected, &geometry );
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

    if (!request_client_surface_recompose( selected ))
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

    toplevel = client_surface_set_server_state( hwnd, NULL,
                                                CLIENT_SURFACE_STATE_GEOMETRY_READY, 0, 0, &wake );
    if (wake && toplevel) NtUserPostMessage( toplevel, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
}

void client_surface_repair_owner( HWND hwnd )
{
    /* Only the native owner can know whether its immutable images cover the
     * current scene. Legacy and cold caches still need producer source recovery. */
    if (!user_driver->pRepairClientSurfaceOwner( hwnd, FALSE )) client_surface_geometry_ready( hwnd );
}

void client_surface_resolve_sources( HWND hwnd )
{
    struct client_surface_scene scene;

    if (!client_surface_get_toplevel_scene( hwnd, &scene ) || !scene.source_pending) return;
    if (user_driver->pRepairClientSurfaceOwner( hwnd, TRUE )) return;
    /* Unsupported backends and failed cache inspections report an empty
     * inventory for the captured scene. A late owner message cannot reopen a
     * finished repair or supersede a newer source recovery request. */
    SERVER_START_REQ( resolve_client_surface_scene_sources )
    {
        req->handle = wine_server_user_handle( hwnd );
        req->scene_id = scene.epoch;
        wine_server_call( req );
    }
    SERVER_END_REQ;
}

void client_surface_set_staged( HWND hwnd )
{
    HWND toplevel;
    BOOL wake;

    TRACE( "client surface composition for %p is staged\n", hwnd );
    toplevel = client_surface_set_server_state( hwnd, NULL, CLIENT_SURFACE_STATE_STAGED, 0, 0, &wake );
    if (wake && toplevel) NtUserPostMessage( toplevel, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
}

void client_surface_bypass_staging( HWND hwnd )
{
    client_surface_set_server_state( hwnd, NULL, CLIENT_SURFACE_STATE_BYPASS, 0, 0, NULL );
}

void client_surface_fail_scene( HWND hwnd )
{
    client_surface_set_server_state( hwnd, NULL, CLIENT_SURFACE_STATE_FAILED, 0, 0, NULL );
}

BOOL client_surface_begin_native_barrier( HWND hwnd, UINT_PTR token )
{
    BOOL ret = FALSE;

    SERVER_START_REQ( set_client_surface_native_barrier )
    {
        req->handle = wine_server_user_handle( hwnd );
        req->token = token;
        req->begin = TRUE;
        ret = !wine_server_call( req );
    }
    SERVER_END_REQ;
    return ret;
}

BOOL client_surface_end_native_barrier( HWND hwnd, UINT_PTR token )
{
    BOOL ret = FALSE;

    SERVER_START_REQ( set_client_surface_native_barrier )
    {
        req->handle = wine_server_user_handle( hwnd );
        req->token = token;
        req->begin = FALSE;
        ret = !wine_server_call( req );
    }
    SERVER_END_REQ;
    return ret;
}

UINT client_surface_begin_publish( HWND hwnd, UINT64 *generation, UINT64 *scene_generation )
{
    UINT publish = 0;

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
            publish = reply->publish;
        }
    }
    SERVER_END_REQ;
    return publish;
}

BOOL client_surface_end_publish( HWND hwnd, UINT64 generation, UINT64 scene_generation, BOOL success )
{
    BOOL accepted = FALSE;
    NTSTATUS status;

    if (success)
    {
        SERVER_START_REQ( set_client_surface_state )
        {
            req->handle = wine_server_user_handle( hwnd );
            req->flags = CLIENT_SURFACE_STATE_PUBLISH_COMMIT;
            req->generation = generation;
            req->scene_generation = scene_generation;
            status = wine_server_call( req );
            if (!status) accepted = reply->publish;
        }
        SERVER_END_REQ;
    }
    else
    {
        /* A failed native exposure retires only the publication this GUI
         * reserved, never a newer scene reached while it was running. */
        SERVER_START_REQ( publish_client_surface_handoff )
        {
            req->handle = wine_server_user_handle( hwnd );
            req->generation = generation;
            req->scene_generation = scene_generation;
            req->success = FALSE;
            status = wine_server_call( req );
            if (!status) accepted = reply->accepted;
        }
        SERVER_END_REQ;
    }
    TRACE( "published GUI generation %s epoch %s status %#lx accepted %u success %u\n",
           wine_dbgstr_longlong( generation ), wine_dbgstr_longlong( scene_generation ),
           (unsigned long)status, accepted, success );
    return !status && accepted && success;
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
    client_surface_set_server_state( hwnd, NULL, CLIENT_SURFACE_STATE_PREPARE_COMMIT,
                                     0, scene_generation, NULL );
}

BOOL client_surface_update( struct client_surface *surface )
{
    struct client_surface_scene scene;
    BOOL scene_valid, ret = FALSE;

    client_surface_lock_target( surface );
    pthread_mutex_lock( &surface->present_lock );
    scene_valid = client_surface_get_scene( surface, &scene );
    if (scene_valid && surface->target.valid &&
        surface->target.toplevel == scene.toplevel &&
        surface->target_scene_epoch == scene.epoch &&
        surface->target_scene_mode == scene.mode)
        ret = TRUE;
    else if (surface->hwnd)
    {
        /* GL storage and flush paths need current geometry without selecting
         * or attaching a DIRECT target before a real native presentation. */
        ret = client_surface_update_present_scene_internal_locked( surface, NULL, FALSE );
        scene_valid = client_surface_get_scene( surface, &scene );
        ret = ret && scene_valid && surface->target.valid &&
              surface->target.toplevel == scene.toplevel &&
              surface->target_scene_epoch == scene.epoch &&
              surface->target_scene_mode == scene.mode;
    }
    pthread_mutex_unlock( &surface->present_lock );
    client_surface_unlock_target( surface );
    return ret;
}

BOOL client_surface_get_size( struct client_surface *surface, SIZE *virtual_size, SIZE *monitor_size )
{
    struct client_surface_geometry geometry;
    BOOL updated;

    updated = InterlockedExchange( &surface->updated, FALSE );
    client_surface_get_geometry( surface, &geometry );

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
        if (!ensure_client_surface_identity( surface ))
        {
            WARN( "failed to reserve a server lifetime for %s\n", debugstr_client_surface( surface ) );
            pthread_mutex_unlock( &surface->present_lock );
            pthread_mutex_unlock( &surfaces_lock );
            return;
        }
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
        if (!(flags & CLIENT_SURFACE_STATE_CACHE)) renew_identity = TRUE;
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
        toplevel = client_surface_set_server_state( hwnd, surface, flags, 0, 0, &wake );
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
    if (renew_identity && toplevel)
    {
        /* An invalidated frame may have completed before the drawable became
         * unused. End its old handoff here as well as in the completion path. */
        client_surface_release_handoff( surface );
        reset_client_surface_identity( surface );
    }
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
        /* Uncaching an inactive surface ends its server registration.  Its
         * old handoff or queued notification can still keep that identity
         * retired, so detach the old mapping and use a fresh token for the
         * replacement drawable.  Preserve the token if uncaching failed. */
        if (!InterlockedCompareExchange( &surface->server_cached, 0, 0 ))
        {
            client_surface_release_handoff( surface );
            reset_client_surface_identity( surface );
        }
        if (!ensure_client_surface_identity( surface ))
        {
            pthread_mutex_unlock( &surface->present_lock );
            client_surface_unlock_present( surface );
            client_surface_release( surface );
            return NULL;
        }
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
