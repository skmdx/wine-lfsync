/*
 * X11 client surface presentation completion
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
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <time.h>

#include "client_surface.h"
#if defined(HAVE_X11_EXTENSIONS_XDAMAGE_H) && defined(SONAME_LIBXDAMAGE)
#include <X11/extensions/Xdamage.h>
#endif
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);

#if defined(HAVE_X11_EXTENSIONS_XDAMAGE_H) && defined(SONAME_LIBXDAMAGE)
static typeof(XDamageQueryExtension) *pXDamageQueryExtension;
static typeof(XDamageCreate) *pXDamageCreate;
static typeof(XDamageDestroy) *pXDamageDestroy;
static typeof(XDamageSubtract) *pXDamageSubtract;
static pthread_once_t xdamage_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t xdamage_lock = PTHREAD_MUTEX_INITIALIZER;
static Display *xdamage_display;
static int xdamage_event_base;
#define XDAMAGE_SURFACE_BUCKETS 256
static struct x11drv_client_surface *xdamage_surfaces[XDAMAGE_SURFACE_BUCKETS];
static struct list xdamage_waiters = LIST_INIT( xdamage_waiters );
static BOOL xdamage_dispatching;
/* A failed dedicated connection is never rearmed or reconnected. XCloseDisplay
 * can perform I/O too; retain its unusable client storage until process exit.
 * Local monitor ownership is still removed without touching that connection. */
static BOOL xdamage_failed;
static void insert_xdamage_surface_locked( struct x11drv_client_surface *surface );

static void init_xdamage(void)
{
    void *handle = dlopen( SONAME_LIBXDAMAGE, RTLD_NOW );

    if (!handle) return;
    pXDamageQueryExtension = dlsym( handle, "XDamageQueryExtension" );
    pXDamageCreate = dlsym( handle, "XDamageCreate" );
    pXDamageDestroy = dlsym( handle, "XDamageDestroy" );
    pXDamageSubtract = dlsym( handle, "XDamageSubtract" );
    if (!pXDamageQueryExtension || !pXDamageCreate || !pXDamageDestroy || !pXDamageSubtract ||
        !(xdamage_display = XOpenDisplay( DisplayString( gdi_display ) )) ||
        !pXDamageQueryExtension( xdamage_display, &xdamage_event_base, &(int){0} ))
    {
        if (xdamage_display) XCloseDisplay( xdamage_display );
        xdamage_display = NULL;
        pXDamageQueryExtension = NULL;
        pXDamageCreate = NULL;
        pXDamageDestroy = NULL;
        pXDamageSubtract = NULL;
        dlclose( handle );
    }
}

static int xdamage_resource_error( Display *display, XErrorEvent *event, void *arg )
{
    int *error = arg;

    *error = event->error_code;
    return 1;
}

static unsigned int xdamage_surface_hash( XID damage )
{
    unsigned long value = damage;

    value ^= value >> 16;
    return value & (XDAMAGE_SURFACE_BUCKETS - 1);
}

static struct x11drv_client_surface *find_xdamage_surface_locked( XID damage )
{
    struct x11drv_client_surface *surface;

    for (surface = xdamage_surfaces[xdamage_surface_hash( damage )]; surface;
         surface = surface->completion.hash_next)
        if (surface->completion.damage == damage) return surface;
    return NULL;
}

static void insert_xdamage_surface_locked( struct x11drv_client_surface *surface )
{
    unsigned int bucket = xdamage_surface_hash( surface->completion.damage );

    surface->completion.hash_next = xdamage_surfaces[bucket];
    xdamage_surfaces[bucket] = surface;
}

static void remove_xdamage_surface_locked( struct x11drv_client_surface *surface )
{
    struct x11drv_client_surface **cursor;

    cursor = &xdamage_surfaces[xdamage_surface_hash( surface->completion.damage )];
    while (*cursor && *cursor != surface) cursor = &(*cursor)->completion.hash_next;
    if (*cursor) *cursor = surface->completion.hash_next;
    surface->completion.hash_next = NULL;
}

static void dispatch_xdamage_events_locked(void)
{
    XEvent event;

    if (xdamage_failed) return;
    while (XPending( xdamage_display ))
    {
        struct x11drv_client_surface *surface;

        XNextEvent( xdamage_display, &event );
        if (event.type != xdamage_event_base + XDamageNotify) continue;
        surface = find_xdamage_surface_locked( ((XDamageNotifyEvent *)&event)->damage );
        if (!surface) continue;
        surface->completion.ready = TRUE;
        if (!list_empty( &surface->completion.wait_entry ))
        {
            list_remove( &surface->completion.wait_entry );
            list_init( &surface->completion.wait_entry );
            pthread_cond_signal( &surface->completion.cond );
        }
    }
}

static void wake_xdamage_dispatcher_locked(void)
{
    struct list *entry;
    struct x11drv_client_surface_completion *completion;
    struct x11drv_client_surface *surface;

    if (!(entry = list_head( &xdamage_waiters ))) return;
    completion = LIST_ENTRY( entry, struct x11drv_client_surface_completion, wait_entry );
    surface = CONTAINING_RECORD( completion, struct x11drv_client_surface, completion );
    list_remove( entry );
    list_init( entry );
    pthread_cond_signal( &surface->completion.cond );
}

static BOOL take_client_surface_damage_locked( struct x11drv_client_surface *surface )
{
    dispatch_xdamage_events_locked();
    if (!surface->completion.ready) return FALSE;

    surface->completion.ready = FALSE;
    return TRUE;
}

static BOOL init_client_surface_damage( struct x11drv_client_surface *surface )
{
    int error = 0;

    if (surface->completion.broken) return FALSE;
    if (surface->completion.damage) return TRUE;
    pthread_once( &xdamage_once, init_xdamage );
    if (!xdamage_display) return FALSE;

    /* The client window is created on gdi_display.  Establish it before the
     * shared event connection creates a Damage object for that XID. */
    XSync( gdi_display, False );
    pthread_mutex_lock( &xdamage_lock );
    if (xdamage_failed)
    {
        pthread_mutex_unlock( &xdamage_lock );
        return FALSE;
    }
    X11DRV_expect_error( xdamage_display, xdamage_resource_error, &error );
    surface->completion.damage = pXDamageCreate( xdamage_display, surface->window, XDamageReportNonEmpty );
    XSync( xdamage_display, False );
    X11DRV_check_error();
    if (error) surface->completion.damage = 0;
    else if (surface->completion.damage)
    {
        insert_xdamage_surface_locked( surface );
    }
    pthread_mutex_unlock( &xdamage_lock );
    return !!surface->completion.damage;
}
#endif

static BOOL x11drv_client_surface_prepare_completion( struct client_surface *client )
{
#if defined(HAVE_X11_EXTENSIONS_XDAMAGE_H) && defined(SONAME_LIBXDAMAGE)
    struct x11drv_client_surface *surface = impl_from_client_surface( client );

    if (!init_client_surface_damage( surface ))
    {
        /* The caller will still submit the host present.  Retrying with a new
         * monitor could then mistake that unobserved operation for a future
         * presentation.  A GDI XSync does not complete asynchronous WSI
         * presentation, which may also use another connection.  Without a
         * usable monitor, do not manufacture completion evidence or authorize
         * composition. */
        surface->completion.broken = TRUE;
        InterlockedExchange( &client->cacheable, FALSE );
        return FALSE;
    }
    do
    {
        pthread_mutex_lock( &xdamage_lock );
        if (xdamage_failed)
        {
            surface->completion.broken = TRUE;
            InterlockedExchange( &client->cacheable, FALSE );
            pthread_mutex_unlock( &xdamage_lock );
            return FALSE;
        }
        while (take_client_surface_damage_locked( surface ));
        pXDamageSubtract( xdamage_display, surface->completion.damage, None, None );
        /* Empty the region before the WSI request is submitted on its own
         * connection.  Repeat if an earlier update raced the subtraction. */
        XSync( xdamage_display, False );
        if (!take_client_surface_damage_locked( surface ))
        {
            TRACE( "event=xdamage_prepare surface=%p damage=%lx\n", client, surface->completion.damage );
            pthread_mutex_unlock( &xdamage_lock );
            break;
        }
        pthread_mutex_unlock( &xdamage_lock );
    } while (1);
    return TRUE;
#else
    InterlockedExchange( &client->cacheable, FALSE );
    return FALSE;
#endif
}

static void x11drv_client_surface_cancel_completion( struct client_surface *client )
{
#if defined(HAVE_X11_EXTENSIONS_XDAMAGE_H) && defined(SONAME_LIBXDAMAGE)
    struct x11drv_client_surface *surface = impl_from_client_surface( client );

    /* No WSI operation has entered native code for this preparation. Keep
     * the monitor reusable: the next prepare drains older damage before its
     * own submission. A native error or abandoned submitted operation still
     * permanently breaks the monitor through abandon(), never this path. */
    pthread_mutex_lock( &xdamage_lock );
    assert( list_empty( &surface->completion.wait_entry ) );
    surface->completion.ready = FALSE;
    TRACE( "event=xdamage_cancel_prepare surface=%p damage=%lx broken=%u\n",
           client, surface->completion.damage, surface->completion.broken );
    pthread_mutex_unlock( &xdamage_lock );
#endif
}

static void x11drv_client_surface_abandon_completion( struct client_surface *client )
{
#if defined(HAVE_X11_EXTENSIONS_XDAMAGE_H) && defined(SONAME_LIBXDAMAGE)
    struct x11drv_client_surface *surface = impl_from_client_surface( client );

    /* An operation which may already have escaped can damage the drawable
     * after any newly created Damage object is armed.  There is no serial in
     * XDamage with which to reject that event, so this drawable can no longer
     * provide causal completion evidence. */
    surface->completion.broken = TRUE;
    InterlockedExchange( &client->cacheable, FALSE );
    if (!surface->completion.damage) return;

    pthread_mutex_lock( &xdamage_lock );
    assert( list_empty( &surface->completion.wait_entry ) );
    remove_xdamage_surface_locked( surface );
    TRACE( "event=xdamage_abandon surface=%p damage=%lx failed=%u\n",
           client, surface->completion.damage, xdamage_failed );
    if (xdamage_failed)
    {
        surface->completion.ready = FALSE;
        surface->completion.damage = 0;
        pthread_mutex_unlock( &xdamage_lock );
        return;
    }
    /* Synchronize and drain before freeing the ID so a queued event cannot be
     * confused with a later Damage object if Xlib recycles the XID. */
    XSync( xdamage_display, False );
    dispatch_xdamage_events_locked();
    surface->completion.ready = FALSE;
    pXDamageDestroy( xdamage_display, surface->completion.damage );
    XSync( xdamage_display, False );
    dispatch_xdamage_events_locked();
    surface->completion.damage = 0;
    pthread_mutex_unlock( &xdamage_lock );
#endif
}

static struct client_surface_completion_result x11drv_client_surface_wait_completion(
    struct client_surface *client, DWORD timeout )
{
#if defined(HAVE_X11_EXTENSIONS_XDAMAGE_H) && defined(SONAME_LIBXDAMAGE)
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    DWORD start = NtGetTickCount();

    if (!surface->completion.damage)
        return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
    for (;;)
    {
        DWORD elapsed, remaining;
        int ret = 0;

        pthread_mutex_lock( &xdamage_lock );
        if (xdamage_failed)
        {
            pthread_mutex_unlock( &xdamage_lock );
            return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
        }
        if (take_client_surface_damage_locked( surface ))
        {
            /* Leave the region nonempty until prepare resets and synchronizes
             * it before the next submission. Rearming here would add a redundant
             * request and allow unrelated updates to generate more notifications. */
            pthread_mutex_unlock( &xdamage_lock );
            return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_SIGNALED );
        }
        elapsed = NtGetTickCount() - start;
        remaining = elapsed < timeout ? timeout - elapsed : 0;
        if (!remaining)
        {
            pthread_mutex_unlock( &xdamage_lock );
            return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_PENDING );
        }

        if (!xdamage_dispatching)
        {
            struct pollfd pollfd = {ConnectionNumber( xdamage_display ), POLLIN, 0};
            int error;

            /* Elect one waiter to service the dedicated Damage connection.
             * Other surfaces sleep on their own condition instead of every
             * worker scanning the same global Xlib event queue. */
            xdamage_dispatching = TRUE;
            pthread_mutex_unlock( &xdamage_lock );
            ret = poll( &pollfd, 1, (int)min( remaining, (DWORD)10 ) );
            error = errno;
            pthread_mutex_lock( &xdamage_lock );
            if ((ret < 0 && error != EINTR) || (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
                xdamage_failed = TRUE;
            /* A terminal transport error may arrive together with readable
             * data. Do not enter Xlib's connection reader on that failure. */
            if (ret > 0 && (pollfd.revents & POLLIN) &&
                !(pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
                dispatch_xdamage_events_locked();
            xdamage_dispatching = FALSE;
            if (xdamage_failed)
                while (!list_empty( &xdamage_waiters )) wake_xdamage_dispatcher_locked();
            else wake_xdamage_dispatcher_locked();
            pthread_mutex_unlock( &xdamage_lock );

            if ((ret < 0 && error != EINTR) || (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
            {
                WARN( "failed waiting for presentation completion for %s, ret %d, revents %#x\n",
                      debugstr_client_surface( client ), ret, pollfd.revents );
                return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
            }
        }
        else
        {
            struct timespec abstime;

            if (clock_gettime( CLOCK_REALTIME, &abstime ))
            {
                pthread_mutex_unlock( &xdamage_lock );
                return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
            }
            abstime.tv_nsec += (long)(remaining % 1000) * 1000000;
            abstime.tv_sec += remaining / 1000 + abstime.tv_nsec / 1000000000;
            abstime.tv_nsec %= 1000000000;
            assert( list_empty( &surface->completion.wait_entry ) );
            list_add_tail( &xdamage_waiters, &surface->completion.wait_entry );
            ret = pthread_cond_timedwait( &surface->completion.cond, &xdamage_lock, &abstime );
            if (!list_empty( &surface->completion.wait_entry ))
            {
                list_remove( &surface->completion.wait_entry );
                list_init( &surface->completion.wait_entry );
            }
            pthread_mutex_unlock( &xdamage_lock );
            if (ret && ret != ETIMEDOUT)
                return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
        }
    }
#else
    return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
#endif
}

const struct client_surface_completion_ops x11drv_client_surface_completion_ops =
{
    .prepare = x11drv_client_surface_prepare_completion,
    .wait = x11drv_client_surface_wait_completion,
    .cancel = x11drv_client_surface_cancel_completion,
    .abandon = x11drv_client_surface_abandon_completion,
};

BOOL x11drv_client_surface_completion_init( struct x11drv_client_surface *surface )
{
    list_init( &surface->completion.wait_entry );
    if (pthread_cond_init( &surface->completion.cond, NULL )) return FALSE;
    surface->completion.cond_initialized = TRUE;
    return TRUE;
}

void x11drv_client_surface_completion_destroy( struct x11drv_client_surface *surface )
{
    x11drv_client_surface_abandon_completion( &surface->client );
    if (surface->completion.cond_initialized) pthread_cond_destroy( &surface->completion.cond );
}
