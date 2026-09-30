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

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static struct list client_surfaces = LIST_INIT( client_surfaces ); /* lifecycle ownership, including cached and claimed */
static struct list unused_surfaces = LIST_INIT( unused_surfaces ); /* cache subset, covered by lifecycle ownership */
static unsigned int unused_surface_count;
static UINT64 unused_surface_bytes;

#define CLIENT_SURFACE_INDEX_BUCKETS 256
static struct client_surface *client_surface_identity_index[CLIENT_SURFACE_INDEX_BUCKETS];
static struct client_surface *client_surface_toplevel_index[CLIENT_SURFACE_INDEX_BUCKETS];
static LONG client_surface_process_id;

struct client_surface_mailbox
{
    struct list entry, pending;
    DWORD tid;
    LONG refs;
    BOOL closed;
};
static struct list mailboxes = LIST_INIT(mailboxes);

BOOL client_surface_init_thread(void)
{
    struct user_thread_info *info = get_user_thread_info();
    struct client_surface_mailbox *mailbox;

    if (info->client_surface_mailbox) return TRUE;
    if (!(mailbox = client_surface_alloc_metadata( 1, sizeof(*mailbox) ))) return FALSE;
    mailbox->refs = 1;
    mailbox->tid = GetCurrentThreadId();
    list_init( &mailbox->pending );
    pthread_mutex_lock( &registry_lock );
    list_add_tail( &mailboxes, &mailbox->entry );
    info->client_surface_mailbox = mailbox;
    pthread_mutex_unlock( &registry_lock );
    return TRUE;
}

static void release_client_surface_mailbox( struct client_surface_mailbox *mailbox )
{
    if (!mailbox || InterlockedDecrement( &mailbox->refs )) return;
    assert( mailbox->closed && list_empty( &mailbox->pending ) );
    client_surface_free_metadata( mailbox, sizeof(*mailbox) );
}

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

static UINT64 allocate_client_surface_identity( struct client_surface *surface, HWND hwnd )
{
    UINT64 identity = 0;

    SERVER_START_REQ( allocate_client_surface )
    {
        req->handle = wine_server_user_handle( hwnd );
        req->window = surface->window_lifetime;
        if (!wine_server_call( req ))
        {
            identity = reply->surface;
            surface->window_lifetime = reply->window;
            surface->owner_thread = reply->owner_thread;
        }
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
    if (!(identity = allocate_client_surface_identity( surface, surface->hwnd ))) return FALSE;
    pthread_mutex_lock( &registry_lock );
    if (surface->lifecycle >= CLIENT_SURFACE_CLOSING)
    {
        pthread_mutex_unlock( &registry_lock );
        release_client_surface_id( identity );
        return FALSE;
    }
    __atomic_store_n( &surface->identity, identity, __ATOMIC_RELEASE );
    if (surface->indexed) insert_client_surface_identity_locked( surface );
    pthread_mutex_unlock( &registry_lock );
    return TRUE;
}

static BOOL client_surface_window_current( const struct client_surface *surface )
{
    struct object_lock lock = OBJECT_LOCK_INIT;
    const window_shm_t *shared;
    NTSTATUS status;

    while ((status = get_shared_window( surface->hwnd, &lock, &shared )) == STATUS_PENDING) {}
    return !status && lock.id == surface->window_lifetime;
}

struct client_surface_target_store
{
    pthread_mutex_t mutex; /* Leaf: only copy payload while held. */
    struct client_surface_target target;
    LONG updating; /* Owned native operation; independent of the published snapshot. */
};

BOOL client_surface_target_is_updating( const struct client_surface *surface )
{
    return ReadAcquire( &surface->target_store->updating );
}

static void begin_client_surface_target_operation( struct client_surface *surface )
{
    assert( !surface->native_present_count && !client_surface_target_is_updating( surface ) );
    ++surface->native_present_count;
    WriteRelease( &surface->target_store->updating, TRUE );
    pthread_mutex_unlock( &surface->present_lock );
    pthread_mutex_unlock( &surface->completion_lock );
}

static void end_client_surface_target_operation( struct client_surface *surface )
{
    pthread_mutex_lock( &surface->completion_lock );
    pthread_mutex_lock( &surface->present_lock );
    assert( surface->native_present_count == 1 && client_surface_target_is_updating( surface ) );
    --surface->native_present_count;
    WriteRelease( &surface->target_store->updating, FALSE );
    pthread_cond_broadcast( &surface->completion_cond );
}

static BOOL insert_client_surface_index( struct client_surface *surface )
{
    struct client_surface_target target;
    WND *win = NULL;

    if (surface->owner_thread)
    {
        win = get_win_ptr( surface->hwnd );
        if (!win || win == WND_OTHER_PROCESS || win == WND_DESKTOP) return FALSE;
        if (win->client_surfaces_closed || !client_surface_window_current( surface ))
        {
            release_win_ptr( win );
            return FALSE;
        }
    }
    else if (!client_surface_window_current( surface )) return FALSE;
    pthread_mutex_lock( &registry_lock );
    assert( !surface->indexed );
    assert( surface->lifecycle == CLIENT_SURFACE_NEW );
    client_surface_add_ref( surface );
    surface->lifecycle = CLIENT_SURFACE_REGISTERED;
    list_add_tail( &client_surfaces, &surface->entry );
    surface->indexed = TRUE;
    insert_client_surface_identity_locked( surface );
    client_surface_get_target( surface, &target );
    surface->indexed_toplevel = target.toplevel;
    if (target.toplevel)
    {
        unsigned int bucket = client_surface_index_hash( (UINT_PTR)target.toplevel );
        surface->toplevel_next = client_surface_toplevel_index[bucket];
        client_surface_toplevel_index[bucket] = surface;
    }
    pthread_mutex_unlock( &registry_lock );
    if (win) release_win_ptr( win );
    return TRUE;
}

/* End notification lookup before the object can be reused. A later activation
 * reserves a new server lifetime. present_lock excludes registration here. */
static void reset_client_surface_identity( struct client_surface *surface )
{
    unsigned int bucket;
    UINT64 identity = client_surface_get_identity( surface );

    if (!identity) return;
    pthread_mutex_lock( &registry_lock );
    if (surface->lifecycle >= CLIENT_SURFACE_CLOSING)
    {
        pthread_mutex_unlock( &registry_lock );
        return;
    }
    bucket = client_surface_index_hash( identity );
    if (surface->indexed)
        remove_client_surface_chain( &client_surface_identity_index[bucket], surface, TRUE );
    surface->identity_next = NULL;
    __atomic_store_n( &surface->identity, 0, __ATOMIC_RELEASE );
    pthread_mutex_unlock( &registry_lock );
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

    if (!surface->indexed) return;
    surface->indexed = FALSE;
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
    struct client_surface_target_store *store = surface->target_store;
    struct client_surface_target next = *target;
    unsigned int bucket;

    pthread_mutex_lock( &registry_lock );
    if (surface->indexed_toplevel)
    {
        bucket = client_surface_index_hash( (UINT_PTR)surface->indexed_toplevel );
        remove_client_surface_chain( &client_surface_toplevel_index[bucket], surface, FALSE );
    }

    pthread_mutex_lock( &store->mutex );
    next.seq = store->target.seq + 2;
    next.epoch = store->target.epoch + !preserve_native;
    store->target = next;
    pthread_mutex_unlock( &store->mutex );

    surface->indexed_toplevel = surface->indexed ? target->toplevel : NULL;
    surface->toplevel_next = NULL;
    if (surface->indexed_toplevel)
    {
        bucket = client_surface_index_hash( (UINT_PTR)target->toplevel );
        surface->toplevel_next = client_surface_toplevel_index[bucket];
        client_surface_toplevel_index[bucket] = surface;
    }
    pthread_mutex_unlock( &registry_lock );
    TRACE( "event=target identity=%s sequence=%s epoch=%s preserved=%u toplevel=%p "
           "position=%d,%d size=%dx%d mode=%u valid=%u surface=%p\n",
           wine_dbgstr_longlong( client_surface_get_identity( surface ) ),
           wine_dbgstr_longlong( next.seq ), wine_dbgstr_longlong( next.epoch ),
           preserve_native, next.toplevel, (int)next.virtual_rect.left,
           (int)next.virtual_rect.top,
           (int)(next.virtual_rect.right - next.virtual_rect.left),
           (int)(next.virtual_rect.bottom - next.virtual_rect.top),
           next.mode, next.valid, surface );
}

#define MAX_UNUSED_CLIENT_SURFACES 64
#define MAX_UNUSED_CLIENT_SURFACE_BYTES (256 * 1024 * 1024)

static UINT64 get_client_surface_cache_cost( const struct client_surface *surface )
{
    struct client_surface_target target;
    const RECT *rect = surface->raw ? &target.monitor_rect : &target.virtual_rect;
    LONGLONG signed_width, signed_height;
    UINT64 width, height;

    client_surface_get_target( surface, &target );
    signed_width = (LONGLONG)rect->right - rect->left;
    signed_height = (LONGLONG)rect->bottom - rect->top;
    width = max( (LONGLONG)0, signed_width );
    height = max( (LONGLONG)0, signed_height );

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
        req->generation = (flags & CLIENT_SURFACE_STATE_NATIVE_CANDIDATE) && surface ?
                          surface->present_serial + 1 : generation;
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
        /* A retained private image survives a failed native write, but the
         * mutable DIRECT attachment no longer supplies publication proof. */
        surface->direct_content_epoch = 0;
        /* A later failed write cannot modify an independently owned completed
         * handoff image. Keep its serial and replay rights until a new image
         * is actually accepted; mutable native sources still invalidate. */
        if (surface->content_valid && surface->completed_image_serial &&
            surface->completed_image_serial == surface->composed_serial)
        {
            pthread_mutex_unlock( &surface->present_lock );
            return;
        }
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
    BOOL wake = FALSE;

    if (client_surface_get_scene( surface, &scene ) && scene.authoritative)
    {
        /* The owner can plan a strategy-only transition from an acknowledged
         * scene at the actual native submission boundary. Restarting geometry
         * preparation here races every foreign-thread producer against the
         * owner's asynchronous prepare/ACK and can starve DIRECT forever. */
        return;
    }

    /* A first submission can prepare the sole native candidate and require
     * an owner snapshot. Do that before acquiring a multi-surface submission's locks:
     * preparing the owner may update every surface belonging to it. */
    client_surface_lock_target( surface );
    pthread_mutex_lock( &surface->present_lock );
    client_surface_get_scene( surface, &scene );
    if (surface->hwnd && InterlockedCompareExchange( &surface->active, 0, 0 ))
    {
        if (!scene.authoritative)
            toplevel = client_surface_set_server_state( surface->hwnd, surface,
                CLIENT_SURFACE_STATE_NATIVE_CANDIDATE, 0, 0, &wake );
        else if (!scene.valid)
            toplevel = scene.toplevel;
    }
    pthread_mutex_unlock( &surface->present_lock );
    client_surface_unlock_target( surface );

    if (toplevel && is_current_thread_window( toplevel ))
    {
        NTSTATUS status = client_surface_begin_prepare( toplevel, &scene );

        if (status == STATUS_SUCCESS)
        {
            status = prepare_window_client_surfaces( toplevel );
            if (status == STATUS_SUCCESS) client_surface_end_prepare( &scene );
        }
        /* Keep the deferred native update on its owned replay path instead
         * of submitting the same owner again here. */
        if (status != STATUS_PENDING) update_window_state( toplevel );
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

static void client_surface_detach_binding( struct client_surface *surface, BOOL gui )
{
    HWND toplevel = 0;
    BOOL wake = FALSE;

    /* Seal the exact token even when a renderer's initial registration has
     * not reached the server yet. No mutable state or native lease is taken
     * from existing submissions here. */
    if (surface->close_identity)
    {
        SERVER_START_REQ( set_client_surface_state )
        {
            req->handle = wine_server_user_handle( surface->hwnd );
            req->surface = surface->close_identity;
            req->flags = CLIENT_SURFACE_STATE_CLOSE | CLIENT_SURFACE_STATE_UNREGISTER | CLIENT_SURFACE_STATE_UNCACHE;
            if (!wine_server_call( req ))
            {
                toplevel = wine_server_ptr_handle( reply->toplevel );
                wake = reply->wake;
            }
        }
        SERVER_END_REQ;
    }
    if (wake && toplevel) NtUserPostMessage( toplevel, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
    if (gui) client_surface_backend_detach( surface );
}

void client_surface_retire_resources( struct client_surface *surface )
{
    assert( !surface->indexed );
    assert( list_empty( &surface->entry ) );
    assert( surface->lifecycle == CLIENT_SURFACE_NEW || surface->lifecycle == CLIENT_SURFACE_DETACHED );
    client_surface_release_handoff( surface );
    release_client_surface_id( client_surface_get_identity( surface ) );
    __atomic_store_n( &surface->identity, 0, __ATOMIC_RELEASE );
    if (surface->clip_region) NtGdiDeleteObjectApp( surface->clip_region );
    assert( !surface->external_completion_count );
    client_surface_handoff_destroy( surface );
    assert( !surface->driver_completion_count );
    assert( !surface->driver_completion_waiters );
    assert( !surface->native_present_count );
    assert( !surface->target_update_waiters );
    client_surface_backend_destroy( surface );
    InterlockedAnd( &surface->ref, ~CLIENT_SURFACE_REF_CLOSED );
}

static void client_surface_destroy( struct client_surface *surface )
{
    assert( !surface->indexed && !surface->identity );
    TRACE( "freeing %s\n", debugstr_client_surface( surface ) );
    client_surface_completion_destroy( surface );
    release_client_surface_mailbox( surface->mailbox );
    pthread_cond_destroy( &surface->completion_cond );
    pthread_mutex_destroy( &surface->completion_lock );
    pthread_mutex_destroy( &surface->present_lock );
    pthread_mutex_destroy( &surface->target_store->mutex );
    free( surface );
}

/* Remove public reachability and transfer registry ownership to the closer.
 * State waits, server requests and native operations run after unlocking. */
static void close_client_surface_locked( struct client_surface *surface )
{
    assert( surface->lifecycle != CLIENT_SURFACE_NEW && surface->lifecycle < CLIENT_SURFACE_CLOSING );
    if (surface->lifecycle == CLIENT_SURFACE_CACHED || surface->lifecycle == CLIENT_SURFACE_CLAIMED)
    {
        list_remove( &surface->cache_entry );
        list_init( &surface->cache_entry );
        remove_unused_client_surface_locked( surface );
    }
    surface->lifecycle = CLIENT_SURFACE_CLOSING;
    surface->close_identity = client_surface_get_identity( surface );
    InterlockedExchange( &surface->closing, TRUE );
    InterlockedExchange( &surface->active, FALSE );
    InterlockedExchange( &surface->server_cached, FALSE );
    remove_client_surface_index_locked( surface );
    list_remove( &surface->entry );
    list_init( &surface->entry );
}

static void detach_owned_client_surface( struct client_surface *surface, BOOL gui )
{
    TRACE( "closing %s, identity %s, gui %u\n", debugstr_client_surface( surface ),
           wine_dbgstr_longlong( surface->close_identity ), gui );
    client_surface_detach_binding( surface, gui );
    pthread_mutex_lock( &registry_lock );
    surface->lifecycle = CLIENT_SURFACE_DETACHED;
    pthread_mutex_unlock( &registry_lock );
    /* The closer and the last renderer race in one atomic word. Only the
     * transition to CLOSED|1 owns scheduling; the other thread touches no
     * surface memory after returning its reference. */
    if (InterlockedOr( &surface->ref, CLIENT_SURFACE_REF_CLOSED ) == 1)
        client_surface_queue_retirement( surface );
}

static void finish_client_surface_close( struct client_surface *surface )
{
    struct client_surface_mailbox *mailbox = surface->mailbox;
    HWND hwnd = surface->hwnd;
    BOOL gui;

    pthread_mutex_lock( &registry_lock );
    gui = mailbox && (!mailbox->closed || mailbox == get_user_thread_info()->client_surface_mailbox);
    if (gui && mailbox != get_user_thread_info()->client_surface_mailbox)
    {
        list_add_tail( &mailbox->pending, &surface->close_entry );
        pthread_mutex_unlock( &registry_lock );
        /* Ownership is in the mailbox before posting. The receiver may
         * already have consumed it, so use only the saved scalar here. */
        NtUserPostMessage( hwnd, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
        return;
    }
    pthread_mutex_unlock( &registry_lock );
    detach_owned_client_surface( surface, gui );
}

void client_surface_drain_mailbox(void)
{
    struct client_surface_mailbox *mailbox = get_user_thread_info()->client_surface_mailbox;
    struct client_surface *surface;

    if (!mailbox) return;
    for (;;)
    {
        pthread_mutex_lock( &registry_lock );
        if (list_empty( &mailbox->pending ))
        {
            pthread_mutex_unlock( &registry_lock );
            return;
        }
        surface = LIST_ENTRY( list_head( &mailbox->pending ), struct client_surface, close_entry );
        list_remove( &surface->close_entry );
        list_init( &surface->close_entry );
        pthread_mutex_unlock( &registry_lock );
        detach_owned_client_surface( surface, TRUE );
    }
}

void client_surface_close_thread(void)
{
    struct user_thread_info *info = get_user_thread_info();
    struct client_surface_mailbox *mailbox = info->client_surface_mailbox;

    if (!mailbox) return;
    pthread_mutex_lock( &registry_lock );
    mailbox->closed = TRUE;
    list_remove( &mailbox->entry );
    pthread_mutex_unlock( &registry_lock );
    client_surface_drain_mailbox();
}

void client_surface_release_thread(void)
{
    struct user_thread_info *info = get_user_thread_info();
    struct client_surface_mailbox *mailbox = info->client_surface_mailbox;

    if (!mailbox) return;
    assert( mailbox->closed && list_empty( &mailbox->pending ) );
    info->client_surface_mailbox = NULL;
    release_client_surface_mailbox( mailbox );
}

static void trim_unused_client_surfaces(void)
{
    struct client_surface *surface;

    for (;;)
    {
        pthread_mutex_lock( &registry_lock );
        if (unused_surface_count <= MAX_UNUSED_CLIENT_SURFACES &&
            unused_surface_bytes <= MAX_UNUSED_CLIENT_SURFACE_BYTES)
        {
            pthread_mutex_unlock( &registry_lock );
            return;
        }
        surface = LIST_ENTRY( list_tail( &unused_surfaces ), struct client_surface, cache_entry );
        close_client_surface_locked( surface );
        pthread_mutex_unlock( &registry_lock );
        finish_client_surface_close( surface );
    }
}

void detach_client_surfaces( HWND hwnd )
{
    struct client_surface *surface, *next;
    struct list closing = LIST_INIT(closing);
    WND *win = get_win_ptr( hwnd );

    if (win && win != WND_OTHER_PROCESS && win != WND_DESKTOP)
    {
        win->client_surfaces_closed = TRUE;
        release_win_ptr( win );
    }

    pthread_mutex_lock( &registry_lock );
    LIST_FOR_EACH_ENTRY_SAFE( surface, next, &client_surfaces, struct client_surface, entry )
    {
        if (surface->hwnd != hwnd) continue;
        close_client_surface_locked( surface );
        list_add_tail( &closing, &surface->entry );
    }
    pthread_mutex_unlock( &registry_lock );
    LIST_FOR_EACH_ENTRY_SAFE( surface, next, &closing, struct client_surface, entry )
    {
        list_remove( &surface->entry );
        list_init( &surface->entry );
        finish_client_surface_close( surface );
    }
    /* Evictions accepted before window destruction are no longer in the
     * registry. Consume their GUI bindings before the driver frees win_data. */
    client_surface_drain_mailbox();
}

void detach_client_surface_identity( UINT64 identity )
{
    struct client_surface *surface, *found = NULL;

    pthread_mutex_lock( &registry_lock );
    LIST_FOR_EACH_ENTRY( surface, &client_surfaces, struct client_surface, entry )
    {
        if (client_surface_get_identity( surface ) != identity) continue;
        close_client_surface_locked( surface );
        found = surface;
        break;
    }
    pthread_mutex_unlock( &registry_lock );
    TRACE( "identity %s, found %u\n", wine_dbgstr_longlong( identity ), !!found );
    if (found) finish_client_surface_close( found );
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
    struct client_surface_target_store *store = surface->target_store;

    pthread_mutex_lock( &store->mutex );
    *target = store->target;
    pthread_mutex_unlock( &store->mutex );
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
        scene->paint_serial = window_shm->client_surface_paint_serial;
        scene->native_candidate = window_shm->client_surface_native_candidate;
        scene->producer_sequence = window_shm->client_surface_producer_sequence;
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

BOOL client_surface_needs_backing( HWND toplevel )
{
    struct object_lock lock = OBJECT_LOCK_INIT;
    const window_shm_t *window_shm = NULL;
    BOOL backing = FALSE;
    NTSTATUS status;

    while ((status = get_shared_window( toplevel, &lock, &window_shm )) == STATUS_PENDING)
        backing = !!(window_shm->client_surface_flags & WINDOW_SHM_CLIENT_SURFACE_BACKING);
    return !status && backing;
}

BOOL client_surface_capture_scene_state( HWND toplevel, struct client_surface_scene *scene )
{
    /* PREPARING blocks producer publication, but its even scene and zero
     * composition generation still identify the owner's native operation. */
    if (read_client_surface_scene( toplevel, scene, NULL, NULL ) && scene->epoch &&
        !(scene->epoch & 1) && (!scene->generation || scene->generation == scene->epoch))
        return TRUE;
    memset( scene, 0, sizeof(*scene) );
    return FALSE;
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
        scene->producer_sequence = producer_shm->client_surface_producer_sequence;
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
    BOOL changed, defer_direct, ready, scene_valid, preserve_native, preparing_candidate, direct_image;

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
    if (allow_direct_transition && scene_valid &&
        scene.native_candidate == client_surface_get_identity( surface ) && scene.direct_candidate &&
        scene.mode != CLIENT_SURFACE_PRESENTATION_DIRECT && surface->backend->prepare_direct)
    {
        /* The owner selects from the final COMPOSING snapshot. Its admission
         * changes the strategy but not the immutable scene. Resample before
         * touching native geometry; never retag a PREPARING snapshot. */
        begin_client_surface_target_operation( surface );
        ready = surface->backend->prepare_direct( surface, &scene );
        end_client_surface_target_operation( surface );
        if (ready || ReadAcquire( &surface->closing )) return FALSE;
    }
    /* PREPARING has an immutable layout but no publication token yet. An
     * actual producer which cannot admit DIRECT must still preserve its new
     * image through the private completion/handoff path. Candidate excludes
     * native barriers; the even exact scene and identity remain mandatory.
     * This applies only native geometry, never a valid scene or generation. */
    preparing_candidate = allow_direct_transition && !scene_valid &&
        scene.native_candidate == client_surface_get_identity( surface ) &&
        scene.direct_candidate && !scene.generation && scene.epoch && !(scene.epoch & 1) &&
        scene.toplevel == next.toplevel && client_surface_scene_snapshot_current( next.toplevel, scene.epoch );
    next.mode = !scene_valid && !preparing_candidate ? current.mode :
                (scene.authoritative || scene.native_candidate == client_surface_get_identity( surface )) ? scene.mode :
                CLIENT_SURFACE_PRESENTATION_COMPOSITED;
    /* Keep an admitted sole native attachment while its resize renewal is
     * pending. Detaching it discards the image needed by that renewal and
     * makes the native parent check fail. The PREPARING scene still cannot
     * authorize publication; the owner must validate the new geometry. */
    if (preparing_candidate && surface->format && current.valid && current.mode == CLIENT_SURFACE_PRESENTATION_DIRECT &&
        current.toplevel == next.toplevel)
        next.mode = current.mode;
    /* Reparenting an already presented offscreen drawable may discard its
     * front buffer.  Keep the last STAGED/COMPOSITED image visible until an
     * actual producer present is ready to replace it.  Geometry owners still
     * update an established DIRECT target in place. */
    /* A completed private copy can survive fallback detachment. An in-flight
     * DIRECT image still needs its native attachment until capture completes. */
    direct_image = current.mode == CLIENT_SURFACE_PRESENTATION_DIRECT && surface->content_valid &&
        surface->completed_image_serial && surface->completed_image_serial == surface->composed_serial;
    defer_direct = !allow_direct_transition &&
                   ((next.mode == CLIENT_SURFACE_PRESENTATION_DIRECT &&
                     current.mode != CLIENT_SURFACE_PRESENTATION_DIRECT) ||
                    (scene_valid && scene.native_candidate == client_surface_get_identity( surface ) && scene.direct_candidate &&
                     scene.mode != CLIENT_SURFACE_PRESENTATION_DIRECT && !direct_image));
    /* A new child can remove the sole-candidate flag while the parent's
     * independent snapshot is still completing. Do not change that frame's
     * native epoch before its captured image has been validated. */
    if (surface->format && current.mode == CLIENT_SURFACE_PRESENTATION_DIRECT &&
        next.mode != CLIENT_SURFACE_PRESENTATION_DIRECT && current.toplevel == next.toplevel &&
        InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ))
        defer_direct = TRUE;
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
    begin_client_surface_target_operation( surface );
    ready = client_surface_backend_update( surface, &next, &update );
    end_client_surface_target_operation( surface );
    if (ReadAcquire( &surface->closing )) return FALSE;
    /* The owned target operation pins native lifetime, but the server scene
     * can change while its callback runs. Do not publish that obsolete next
     * geometry as usable. Invalidate the old epoch and let the caller resample. */
    if (ready && ((scene_valid && !client_surface_scene_current( &scene )) ||
                  (preparing_candidate && !client_surface_scene_snapshot_current( next.toplevel, scene.epoch ))))
        ready = FALSE;

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
    /* Publish only after the native mutation. Geometry readers copy under
     * the leaf mutex; completed frames validate the independent native epoch. */
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
    if (ReadAcquire( &surface->closing )) return FALSE;
    return client_surface_update_present_scene_internal_locked( surface, scene, allow_direct_transition );
}

BOOL client_surface_update_present_locked( struct client_surface *surface )
{
    if (ReadAcquire( &surface->closing )) return FALSE;
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

    pthread_mutex_lock( &registry_lock );
    for (surface = client_surface_toplevel_index[bucket]; surface; surface = surface->toplevel_next)
    {
        if (surface->lifecycle == CLIENT_SURFACE_CLAIMED || surface->indexed_toplevel != toplevel ||
            !InterlockedCompareExchange( &surface->active, 0, 0 ))
            continue;
        if (!queue_client_surface_update( surface, surfaces, count, size ))
        {
            ret = FALSE;
            break;
        }
    }
    pthread_mutex_unlock( &registry_lock );
    return ret;
}

static struct client_surface *find_client_surface_identity( UINT64 identity )
{
    unsigned int bucket = client_surface_index_hash( identity );
    struct client_surface *surface;

    pthread_mutex_lock( &registry_lock );
    for (surface = client_surface_identity_index[bucket]; surface; surface = surface->identity_next)
    {
        if (client_surface_get_identity( surface ) != identity) continue;
        if (surface->lifecycle == CLIENT_SURFACE_CLAIMED ||
            (!InterlockedCompareExchange( &surface->active, 0, 0 ) &&
             !InterlockedCompareExchange( &surface->server_cached, 0, 0 )))
            surface = NULL;
        else
            client_surface_add_ref( surface );
        break;
    }
    pthread_mutex_unlock( &registry_lock );
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
    struct list closing = LIST_INIT(closing);
    struct client_surface **update_surfaces = NULL;
    struct client_surface **recompose_surfaces = NULL;
    HRGN exposed_region = 0;
    UINT count = 0, update_count = 0, update_size = 0;
    UINT recompose_count = 0, recompose_size = 0, i;

    if (!collect_indexed_client_surfaces( hwnd, &update_surfaces, &update_count, &update_size ))
        WARN( "failed to allocate client surface update list\n" );

    for (i = 0; i < update_count; ++i)
    {
        struct client_surface_target target;
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
        client_surface_get_target( surface, &target );
        monitor_rect = target.monitor_rect;
        toplevel = target.toplevel;
        client_surface_update_present_locked( surface );
        client_surface_get_target( surface, &target );
        new_monitor_rect = target.monitor_rect;
        new_toplevel = target.toplevel;
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

    pthread_mutex_lock( &registry_lock );
    /* discard extra unused surfaces when updating window */
    LIST_FOR_EACH_ENTRY_SAFE( surface, next, &unused_surfaces, struct client_surface, cache_entry )
    {
        if (surface->hwnd != hwnd || !count++) continue;
        close_client_surface_locked( surface );
        list_add_tail( &closing, &surface->entry );
    }
    pthread_mutex_unlock( &registry_lock );
    LIST_FOR_EACH_ENTRY_SAFE( surface, next, &closing, struct client_surface, entry )
    {
        list_remove( &surface->entry );
        list_init( &surface->entry );
        finish_client_surface_close( surface );
    }
    for (i = 0; i < update_count; ++i) client_surface_release( update_surfaces[i] );
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
    struct client_surface_mailbox *mailbox;
    struct client_surface_target target = { .toplevel = toplevel };
    SIZE_T offset = ((SIZE_T)size + __alignof__(struct client_surface_target_store) - 1) &
                    ~((SIZE_T)__alignof__(struct client_surface_target_store) - 1);

    if (size < sizeof(*surface)) return NULL;
    if (!backend) backend = &default_client_surface_backend;
    if (!client_surface_backend_valid( backend )) return NULL;
    if (offset > ~0u - sizeof(struct client_surface_target_store)) return NULL;
    if (!(surface = client_surface_alloc( offset + sizeof(struct client_surface_target_store) ))) return NULL;
    surface->target_store = (void *)((char *)surface + offset);
    if (pthread_mutex_init( &surface->target_store->mutex, NULL ))
    {
        free( surface );
        return NULL;
    }
    if (!client_surface_completion_init( surface ))
    {
        pthread_mutex_destroy( &surface->target_store->mutex );
        free( surface );
        return NULL;
    }
    if (pthread_mutex_init( &surface->present_lock, NULL )) goto failed_present_lock;
    if (pthread_mutex_init( &surface->completion_lock, NULL )) goto failed_completion_lock;
    if (pthread_cond_init( &surface->completion_cond, NULL )) goto failed_completion_cond;
    if (!(surface->identity = allocate_client_surface_identity( surface, hwnd ))) goto failed_identity;
    if (surface->owner_thread)
    {
        pthread_mutex_lock( &registry_lock );
        LIST_FOR_EACH_ENTRY( mailbox, &mailboxes, struct client_surface_mailbox, entry )
        {
            if (mailbox->tid != surface->owner_thread) continue;
            InterlockedIncrement( &mailbox->refs );
            surface->mailbox = mailbox;
            break;
        }
        pthread_mutex_unlock( &registry_lock );
        if (!surface->mailbox)
        {
            release_client_surface_id( surface->identity );
            goto failed_identity;
        }
    }
    if (!client_surface_prepare_retirement( surface ))
    {
        release_client_surface_id( surface->identity );
        goto failed_identity;
    }
    surface->backend = backend;
    surface->ref = 1;
    surface->hwnd = hwnd;
    surface->format = format;
    surface->raw = raw;
    surface->cacheable = TRUE;
    if (!get_client_surface_rects( toplevel, hwnd, &target ))
        target.virtual_rect = target.monitor_rect = (RECT){0};
    /* No reader can see the unpublished allocation yet. */
    surface->target_store->target = target;
    list_init( &surface->entry );
    list_init( &surface->cache_entry );
    list_init( &surface->close_entry );
    InterlockedCompareExchange( &client_surface_process_id,
                                HandleToULong( NtCurrentTeb()->ClientId.UniqueProcess ), 0 );

    TRACE( "created %s, identity %s, format %d, raw %u, toplevel %p, virtual_rect %s, monitor_rect %s\n", debugstr_client_surface( surface ),
           wine_dbgstr_longlong( surface->identity ), format, raw, toplevel, wine_dbgstr_rect( &target.virtual_rect ),
           wine_dbgstr_rect( &target.monitor_rect ) );
    return surface;

failed_identity:
    release_client_surface_mailbox( surface->mailbox );
    pthread_cond_destroy( &surface->completion_cond );
failed_completion_cond:
    pthread_mutex_destroy( &surface->completion_lock );
failed_completion_lock:
    pthread_mutex_destroy( &surface->present_lock );
failed_present_lock:
    client_surface_completion_destroy( surface );
    pthread_mutex_destroy( &surface->target_store->mutex );
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
    LONG ref = InterlockedDecrement( &surface->ref );
    if (ref == (CLIENT_SURFACE_REF_CLOSED | 1)) client_surface_queue_retirement( surface );
    else if (!ref) client_surface_destroy( surface );
}

void client_surface_abort( struct client_surface *surface )
{
    assert( surface->lifecycle == CLIENT_SURFACE_NEW && surface->ref == 1 );
    client_surface_retire_resources( surface );
    client_surface_release( surface );
}

static BOOL client_surface_recompose( struct client_surface *surface, LONG64 seq )
{
    struct client_surface_frame present;
    struct client_surface_completed_frame frame;
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
    if (client_surface_handoff_has_source( surface ))
    {
        complete_client_surface_recompose( surface, seq );
        pthread_mutex_unlock( &surface->completion_lock );
        return TRUE;
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

static BOOL client_surface_set_scene_result( const struct client_surface_scene *scene, UINT flags )
{
    BOOL accepted = FALSE, staged = FALSE;

    if (!scene->toplevel || !scene->epoch) return FALSE;
    SERVER_START_REQ( set_client_surface_state )
    {
        req->handle = req->scene_toplevel = wine_server_user_handle( scene->toplevel );
        req->flags = flags;
        req->generation = scene->generation;
        req->scene_generation = scene->epoch;
        req->producer_sequence = scene->paint_serial;
        if (!wine_server_call( req ) && reply->toplevel == wine_server_user_handle( scene->toplevel ))
        {
            accepted = TRUE;
            staged = reply->staged;
            if (reply->wake && flags != CLIENT_SURFACE_STATE_PREPARE_COMMIT)
                NtUserPostMessage( scene->toplevel, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
        }
    }
    SERVER_END_REQ;
    TRACE( "client surface result %#x for %p generation %s scene %s accepted %u staged %u\n", flags,
           scene->toplevel, wine_dbgstr_longlong( scene->generation ),
           wine_dbgstr_longlong( scene->epoch ), accepted, staged );
    return accepted && (flags != CLIENT_SURFACE_STATE_STAGED || staged);
}

BOOL client_surface_set_staged( const struct client_surface_scene *scene )
{
    return client_surface_set_scene_result( scene, CLIENT_SURFACE_STATE_STAGED );
}

void client_surface_bypass_staging( HWND hwnd )
{
    client_surface_set_server_state( hwnd, NULL, CLIENT_SURFACE_STATE_BYPASS, 0, 0, NULL );
}

void client_surface_fail_scene( const struct client_surface_scene *scene )
{
    client_surface_set_scene_result( scene, CLIENT_SURFACE_STATE_FAILED );
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

NTSTATUS client_surface_publish_window( HWND hwnd )
{
    UINT64 generation, scene;
    NTSTATUS status = STATUS_NOT_FOUND;
    UINT publish = 0;
    BOOL again;
    WND *win;

    if (!is_current_thread_window( hwnd ) || !(win = get_win_ptr( hwnd ))) return STATUS_NOT_FOUND;
    if (win == WND_DESKTOP || win == WND_OTHER_PROCESS) return STATUS_NOT_FOUND;
    if (win->publish_busy)
    {
        win->publish_again = TRUE;
        release_win_ptr( win );
        return STATUS_PENDING;
    }
    win->publish_busy = TRUE;
    generation = win->publish_generation;
    scene = win->publish_scene;
    release_win_ptr( win );

    if (generation)
    {
        SERVER_START_REQ( set_client_surface_state )
        {
            req->handle = wine_server_user_handle( hwnd );
            req->flags = CLIENT_SURFACE_STATE_PUBLISH_RESUME;
            req->surface = 0;
            req->generation = generation;
            req->scene_generation = scene;
            if (!wine_server_call( req )) publish = reply->publish;
        }
        SERVER_END_REQ;
        /* Retirement authenticates the old reservation independently. A
         * stale continuation must never fail or commit a newer publication. */
        if (!publish)
        {
            client_surface_end_publish( hwnd, generation, scene, FALSE );
            /* This wake may already belong to the next READY transaction. */
            publish = client_surface_begin_publish( hwnd, &generation, &scene );
        }
    }
    else publish = client_surface_begin_publish( hwnd, &generation, &scene );
    if (publish)
    {
        if (publish == CLIENT_SURFACE_PUBLISH_EXPOSE)
            status = user_driver->pExposeClientSurface( hwnd, scene ) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
        else status = publish_window_state( hwnd );
        if (status != STATUS_PENDING)
            client_surface_end_publish( hwnd, generation, scene, status == STATUS_SUCCESS );
    }
    if (!(win = get_win_ptr( hwnd ))) return STATUS_NOT_FOUND;
    if (win == WND_DESKTOP || win == WND_OTHER_PROCESS) return STATUS_NOT_FOUND;
    win->publish_generation = status == STATUS_PENDING ? generation : 0;
    win->publish_scene = status == STATUS_PENDING ? scene : 0;
    win->publish_busy = FALSE;
    again = win->publish_again;
    win->publish_again = FALSE;
    release_win_ptr( win );
    if (again) NtUserPostMessage( hwnd, WM_WINE_UPDATEWINDOWSTATE, WINE_PUBLISH_CLIENT_SURFACES, 0 );
    return status;
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

NTSTATUS client_surface_begin_prepare( HWND hwnd, struct client_surface_scene *scene )
{
    NTSTATUS status;

    memset( scene, 0, sizeof(*scene) );
    SERVER_START_REQ( set_client_surface_state )
    {
        req->handle = wine_server_user_handle( hwnd );
        req->surface = 0;
        req->flags = CLIENT_SURFACE_STATE_PREPARE_BEGIN;
        req->generation = 0;
        req->scene_generation = 0;
        wine_server_set_reply( req, &scene->paint_serial, sizeof(scene->paint_serial) );
        status = wine_server_call( req );
        if (status == STATUS_SUCCESS && wine_server_reply_size( reply ) == sizeof(scene->paint_serial) && reply->publish &&
            reply->toplevel == wine_server_user_handle( hwnd ) &&
            reply->scene_generation && !(reply->scene_generation & 1))
        {
            /* Keep the exact admission reply, including PREPARING generation
             * zero. A later shared-state read could name another preparation. */
            scene->toplevel = wine_server_ptr_handle( reply->toplevel );
            scene->epoch = reply->scene_generation;
            scene->generation = reply->generation;
        }
        else if (status == STATUS_SUCCESS) status = STATUS_NOT_FOUND;
    }
    SERVER_END_REQ;
    TRACE_(csperf)( "event=client_surface_prepare hwnd=%p status=%#x epoch=%llu generation=%llu paint_serial=%llu\n",
                   hwnd, (unsigned int)status, (unsigned long long)scene->epoch,
                   (unsigned long long)scene->generation, (unsigned long long)scene->paint_serial );
    return status;
}

BOOL client_surface_end_prepare( const struct client_surface_scene *scene )
{
    return client_surface_set_scene_result( scene, CLIENT_SURFACE_STATE_PREPARE_COMMIT );
}

BOOL client_surface_update( struct client_surface *surface )
{
    struct client_surface_target target;
    struct client_surface_scene scene;
    BOOL scene_valid, ret = FALSE;

    client_surface_lock_target( surface );
    pthread_mutex_lock( &surface->present_lock );
    client_surface_get_target( surface, &target );
    scene_valid = client_surface_get_scene( surface, &scene );
    if (scene_valid && target.valid &&
        target.toplevel == scene.toplevel &&
        surface->target_scene_epoch == scene.epoch &&
        surface->target_scene_mode == scene.mode)
        ret = TRUE;
    else if (surface->hwnd)
    {
        /* GL storage and flush paths need current geometry without selecting
         * or attaching a DIRECT target before a real native presentation. */
        ret = client_surface_update_present_scene_internal_locked( surface, NULL, FALSE );
        client_surface_get_target( surface, &target );
        scene_valid = client_surface_get_scene( surface, &scene );
        ret = ret && scene_valid && target.valid &&
              target.toplevel == scene.toplevel &&
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
    BOOL cache = FALSE, close = FALSE, renew_identity = FALSE, wake = FALSE;
    UINT flags = 0;

    TRACE( "surface %s, use %u\n", debugstr_client_surface( surface ), use );
    if (use) client_surface_update_now( surface );
    pthread_mutex_lock( &surface->present_lock );
    if (use && !ensure_client_surface_identity( surface ))
    {
        pthread_mutex_unlock( &surface->present_lock );
        return;
    }
    pthread_mutex_lock( &registry_lock );
    if (surface->lifecycle >= CLIENT_SURFACE_CLOSING || !surface->hwnd)
        WARN( "surface %s has been closed already, ignoring.\n", debugstr_client_surface( surface ) );
    else if (use)
    {
        assert( surface->lifecycle == CLIENT_SURFACE_REGISTERED );
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
        assert( surface->lifecycle == CLIENT_SURFACE_REGISTERED );
        if (InterlockedCompareExchange( &surface->cacheable, 0, 0 ) &&
            (surface->owner_thread || ReadAcquire( &surface->content_valid )))
        {
            surface->lifecycle = CLIENT_SURFACE_CACHED;
            list_add_head( &unused_surfaces, &surface->cache_entry );
            add_unused_client_surface_locked( surface );
            cache = TRUE;
        }
        else
        {
            close_client_surface_locked( surface );
            close = TRUE;
        }
        flags = CLIENT_SURFACE_STATE_UNREGISTER;
        if (cache && InterlockedCompareExchange( &surface->content_valid, 0, 0 ))
        {
            flags |= CLIENT_SURFACE_STATE_CACHE | client_surface_backend_state_flags( surface );
            InterlockedExchange( &surface->server_cached, TRUE );
        }
        else if (InterlockedCompareExchange( &surface->server_cached, 0, 0 ))
        {
            flags |= CLIENT_SURFACE_STATE_UNCACHE;
            InterlockedExchange( &surface->server_cached, FALSE );
        }
        if (!(flags & CLIENT_SURFACE_STATE_CACHE)) renew_identity = TRUE;
        InterlockedExchange( &surface->active, FALSE );
        hwnd = surface->hwnd;
    }
    pthread_mutex_unlock( &registry_lock );

    if (hwnd)
    {
        toplevel = client_surface_set_server_state( hwnd, surface, flags, 0, 0, &wake );
        if (wake && toplevel) NtUserPostMessage( toplevel, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
    }
    if (renew_identity && toplevel)
    {
        client_surface_release_handoff( surface );
        reset_client_surface_identity( surface );
    }
    pthread_mutex_unlock( &surface->present_lock );

    if (close) finish_client_surface_close( surface );
    else if (hwnd && !toplevel && !client_surface_window_current( surface ))
        detach_client_surface_identity( client_surface_get_identity( surface ) );
    else if (!use && cache) trim_unused_client_surfaces();
}

struct client_surface *get_unused_client_surface( HWND hwnd, int format, BOOL raw )
{
    struct client_surface *surface = NULL, *candidate;
    BOOL reusable = FALSE, close = FALSE, state_locked = FALSE;

    pthread_mutex_lock( &registry_lock );
    LIST_FOR_EACH_ENTRY( candidate, &unused_surfaces, struct client_surface, cache_entry )
    {
        if (candidate->lifecycle != CLIENT_SURFACE_CACHED || !candidate->owner_thread || candidate->hwnd != hwnd ||
            candidate->format != format || candidate->raw != raw) continue;
        candidate->lifecycle = CLIENT_SURFACE_CLAIMED;
        /* Keep window invalidation and cache accounting until commit. Lookup
         * and another checkout cannot acquire this claimed candidate. */
        client_surface_add_ref( candidate );
        surface = candidate;
        break;
    }
    pthread_mutex_unlock( &registry_lock );

    if (surface && !pthread_mutex_trylock( &surface->completion_lock ))
    {
        if (!pthread_mutex_trylock( &surface->present_lock ))
        {
            state_locked = TRUE;
            /* Only registry/checkout ownership may remain. FIFO tickets and
             * callback releases retain independent references; native users
             * and target mutations must also have drained. Never wait here. */
            if (ReadAcquire( &surface->ref ) == 2 && !surface->external_completion_count &&
                !surface->driver_completion_count && !surface->driver_completion_waiters &&
                !surface->native_present_count && !surface->target_update_waiters &&
                surface->hwnd == hwnd && surface->cacheable)
            {
                client_surface_uncache_present_locked( surface );
                if (!InterlockedCompareExchange( &surface->server_cached, 0, 0 ))
                {
                    client_surface_release_handoff( surface );
                    reset_client_surface_identity( surface );
                    if (ensure_client_surface_identity( surface ))
                    {
                        InterlockedExchange( &surface->content_valid, FALSE );
                        reusable = client_surface_update_present_locked( surface );
                        close = !reusable;
                    }
                    else close = TRUE;
                }
            }
        }
        if (!state_locked) pthread_mutex_unlock( &surface->completion_lock );
    }
    if (surface)
    {
        /* Keep target state locked through the registry commit. A successful
         * renewal must still name the same window lifetime at publication. */
        if (reusable && !client_surface_window_current( surface ))
        {
            reusable = FALSE;
            close = TRUE;
        }
        pthread_mutex_lock( &registry_lock );
        if (surface->lifecycle != CLIENT_SURFACE_CLAIMED) reusable = close = FALSE;
        else if (reusable)
        {
            list_remove( &surface->cache_entry );
            list_init( &surface->cache_entry );
            remove_unused_client_surface_locked( surface );
            surface->lifecycle = CLIENT_SURFACE_REGISTERED;
        }
        else if (close) close_client_surface_locked( surface );
        else surface->lifecycle = CLIENT_SURFACE_CACHED;
        pthread_mutex_unlock( &registry_lock );
        if (state_locked)
        {
            pthread_mutex_unlock( &surface->present_lock );
            pthread_mutex_unlock( &surface->completion_lock );
        }
        if (reusable)
        {
            TRACE( "Reusing surface %s, identity %s\n", debugstr_client_surface( surface ),
                   wine_dbgstr_longlong( client_surface_get_identity( surface ) ) );
            return surface; /* checkout reference becomes renderer ownership */
        }
        if (close) finish_client_surface_close( surface );
        client_surface_release( surface );
    }

    /* The factory owns an unpublished object until all native creation has
     * succeeded. Lookups must not expose partially initialized backends. */
    if ((surface = user_driver->pCreateClientSurface( hwnd, format, raw )))
    {
        if (!insert_client_surface_index( surface ))
        {
            /* Native creation succeeded but the window closed before
             * publication. Its binding may already exist, unlike a partial
             * factory failure; transfer the unpublished reference to close. */
            surface->close_identity = client_surface_get_identity( surface );
            surface->lifecycle = CLIENT_SURFACE_CLOSING;
            InterlockedExchange( &surface->closing, TRUE );
            finish_client_surface_close( surface );
            return NULL;
        }
    }
    return surface;
}

BOOL is_client_surface_window( struct client_surface *surface, HWND hwnd )
{
    HWND surface_hwnd;

    if (!surface) return FALSE;
    surface_hwnd = InterlockedCompareExchangePointer( (void **)&surface->hwnd, NULL, NULL );
    return hwnd ? surface_hwnd == hwnd : !!surface_hwnd;
}
