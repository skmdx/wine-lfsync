/*
 * X11 client surface backend
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

#include "x11drv.h"
#if defined(HAVE_X11_EXTENSIONS_XDAMAGE_H) && defined(SONAME_LIBXDAMAGE)
#include <X11/extensions/Xdamage.h>
#endif
#include "xcomposite.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);

static const WCHAR client_surface_backing_prop[] =
    {'_','_','w','i','n','e','_','x','1','1','_','c','l','i','e','n','t','_','s','u','r','f','a','c','e','_','b','a','c','k','i','n','g',0};

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
#endif

struct x11drv_retired_pixmap
{
    struct x11drv_retired_pixmap *next;
    Pixmap pixmap;
};

static unsigned int client_surface_backing_extent( int size )
{
    unsigned int extent = 64, requested = min( max( size, 1 ), 65535 );

    while (extent < requested)
    {
        if (extent >= 32768) return 65535;
        extent <<= 1;
    }
    return extent;
}

static BOOL get_client_surface_window_extent( struct x11drv_win_data *data,
                                              unsigned int *width, unsigned int *height )
{
    Window root;
    unsigned int border, depth;
    int x, y;

    return XGetGeometry( data->display, data->whole_window, &root, &x, &y,
                         width, height, &border, &depth );
}

static int client_surface_backing_error( Display *display, XErrorEvent *event, void *arg )
{
    int *error = arg;

    *error = event->error_code;
    return 1;
}

void X11DRV_client_surface_backing_destroy( struct x11drv_win_data *data )
{
    struct x11drv_retired_pixmap *retired, *next;

    NtUserRemoveProp( data->hwnd, client_surface_backing_prop );
    if (data->client_surface_backing)
        XFreePixmap( data->display, data->client_surface_backing );
    if (data->client_surface_gc) XFreeGC( data->display, data->client_surface_gc );
    for (retired = data->client_surface_retired; retired; retired = next)
    {
        next = retired->next;
        XFreePixmap( data->display, retired->pixmap );
        free( retired );
    }
    data->client_surface_backing = 0;
    data->client_surface_gc = 0;
    data->client_surface_backing_width = 0;
    data->client_surface_backing_height = 0;
    data->client_surface_retired = NULL;
    data->client_surface_backing_valid = FALSE;
}

/* Grow geometrically and retain old XIDs until the top-level is destroyed.
 * A renderer from another process can pass its scene validation immediately
 * before a resize replaces the property.  Reusing or freeing that Pixmap
 * would turn the race into a cross-client Drawable ABA.  Geometric growth
 * bounds the retained area by a small multiple of the largest allocation. */
BOOL X11DRV_client_surface_backing_ensure( struct x11drv_win_data *data )
{
    struct x11drv_retired_pixmap *retired = NULL;
    unsigned int width, height, window_width, window_height;
    BOOL new_gc, old_valid;
    int error = 0;
    Pixmap pixmap;
    GC gc;

    if (!data->whole_window) return FALSE;
    if (!get_client_surface_window_extent( data, &window_width, &window_height )) return FALSE;
    width = client_surface_backing_extent( data->rects.visible.right - data->rects.visible.left );
    height = client_surface_backing_extent( data->rects.visible.bottom - data->rects.visible.top );
    if (data->client_surface_backing && data->client_surface_backing_width >= width &&
        data->client_surface_backing_height >= height)
        return TRUE;

    /* Grow both axes monotonically.  If alternating wide and tall windows
     * replaced one undersized axis while shrinking the other, each resize
     * would retire another Pixmap of the opposite aspect ratio forever.
     * Monotonic extents make every replacement at least double in area and
     * bound all retired allocations by a geometric series. */
    width = max( width, data->client_surface_backing_width );
    height = max( height, data->client_surface_backing_height );
    old_valid = data->client_surface_backing_valid;

    /* Until a larger capability has been committed, the old Pixmap no longer
     * represents the complete host extent and must not satisfy Expose. */
    data->client_surface_backing_valid = FALSE;

    if (data->client_surface_backing && !(retired = malloc( sizeof(*retired) ))) return FALSE;
    pixmap = XCreatePixmap( data->display, data->whole_window, width, height, data->vis.depth );
    gc = data->client_surface_gc;
    new_gc = !gc;
    if (!pixmap || (!gc && !(gc = XCreateGC( data->display, pixmap, 0, NULL ))))
    {
        if (pixmap) XFreePixmap( data->display, pixmap );
        free( retired );
        return FALSE;
    }

    X11DRV_expect_error( data->display, client_surface_backing_error, &error );
    XCopyArea( data->display, data->whole_window, pixmap, gc, 0, 0,
               min( window_width, width ), min( window_height, height ), 0, 0 );
    if (data->client_surface_backing && old_valid)
    {
        XCopyArea( data->display, data->client_surface_backing, pixmap, gc, 0, 0,
                   data->client_surface_backing_width, data->client_surface_backing_height, 0, 0 );
    }
    XSync( data->display, False );
    X11DRV_check_error();

    /* The HWND property is the renderer-visible capability for this backing.
     * Commit it before replacing local state.  Otherwise a failed property
     * allocation could make the owner publish one Pixmap while renderers keep
     * writing the old one. */
    if (error || !NtUserSetProp( data->hwnd, client_surface_backing_prop, (HANDLE)pixmap ))
    {
        int cleanup_error = 0;

        X11DRV_expect_error( data->display, client_surface_backing_error, &cleanup_error );
        XFreePixmap( data->display, pixmap );
        if (new_gc) XFreeGC( data->display, gc );
        XSync( data->display, False );
        X11DRV_check_error();
        free( retired );
        return FALSE;
    }

    if (data->client_surface_backing)
    {
        retired->pixmap = data->client_surface_backing;
        retired->next = data->client_surface_retired;
        data->client_surface_retired = retired;
    }

    data->client_surface_backing = pixmap;
    data->client_surface_gc = gc;
    data->client_surface_backing_width = width;
    data->client_surface_backing_height = height;
    data->client_surface_backing_valid = TRUE;
    return TRUE;
}

BOOL X11DRV_client_surface_backing_snapshot( struct x11drv_win_data *data, BOOL invalidate )
{
    unsigned int width, height, window_width, window_height;
    int error = 0;

    if (!X11DRV_client_surface_backing_ensure( data )) return FALSE;
    if (!get_client_surface_window_extent( data, &window_width, &window_height )) return FALSE;
    width = min( data->client_surface_backing_width, window_width );
    height = min( data->client_surface_backing_height, window_height );
    data->client_surface_backing_valid = FALSE;
    X11DRV_expect_error( data->display, client_surface_backing_error, &error );
    XCopyArea( data->display, data->whole_window, data->client_surface_backing,
               data->client_surface_gc,
               0, 0, width, height, 0, 0 );
    XSync( data->display, False );
    X11DRV_check_error();
    if (error) return FALSE;
    data->client_surface_backing_valid = !invalidate;
    return TRUE;
}

BOOL X11DRV_client_surface_backing_publish( struct x11drv_win_data *data )
{
    unsigned int width, height, window_width, window_height;
    int error = 0;

    if (!data->whole_window || !data->client_surface_backing || !data->client_surface_gc)
        return FALSE;
    if (!get_client_surface_window_extent( data, &window_width, &window_height )) return FALSE;
    width = min( data->client_surface_backing_width, window_width );
    height = min( data->client_surface_backing_height, window_height );
    X11DRV_expect_error( data->display, client_surface_backing_error, &error );
    XCopyArea( data->display, data->client_surface_backing, data->whole_window,
               data->client_surface_gc,
               0, 0, width, height, 0, 0 );
    XSync( data->display, False );
    X11DRV_check_error();
    if (error) return FALSE;
    data->client_surface_backing_valid = TRUE;
    return TRUE;
}

BOOL X11DRV_client_surface_backing_restore( struct x11drv_win_data *data,
                                           Window window, const RECT *rect )
{
    if (window != data->whole_window || !data->client_surface_backing ||
        !data->client_surface_gc || !data->client_surface_backing_valid || IsRectEmpty( rect ))
        return FALSE;
    if (rect->left < 0 || rect->top < 0 ||
        (unsigned int)rect->right > data->client_surface_backing_width ||
        (unsigned int)rect->bottom > data->client_surface_backing_height)
        return FALSE;
    XCopyArea( data->display, data->client_surface_backing, data->whole_window,
               data->client_surface_gc,
               rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
               rect->left, rect->top );
    XFlush( data->display );
    return TRUE;
}

Pixmap X11DRV_get_client_surface_backing( HWND hwnd )
{
    struct x11drv_win_data *data = get_win_data( hwnd );
    Pixmap ret;

    if (!data) return (Pixmap)NtUserGetProp( hwnd, client_surface_backing_prop );
    ret = data->client_surface_backing;
    release_win_data( data );
    return ret;
}

Pixmap X11DRV_get_client_surface_backing_property( HWND hwnd )
{
    return (Pixmap)NtUserGetProp( hwnd, client_surface_backing_prop );
}

static BOOL needs_client_window_clipping( HWND hwnd )
{
    RECT rect, client;
    UINT ret = 0;
    HRGN region;
    HDC hdc;

    if (NtUserGetPresentRect( hwnd, &client, 0 )) return FALSE;
    if (!NtUserGetClientRect( hwnd, &client, NtUserGetDpiForWindow( hwnd ) )) return FALSE;
    OffsetRect( &client, -client.left, -client.top );

    if (!(hdc = NtUserGetDCEx( hwnd, 0, DCX_CACHE | DCX_USESTYLE ))) return FALSE;
    if ((region = NtGdiCreateRectRgn( 0, 0, 0, 0 )))
    {
        ret = NtGdiGetRandomRgn( hdc, region, SYSRGN );
        if (ret > 0 && (ret = NtGdiGetRgnBox( region, &rect )) < NULLREGION) ret = 0;
        if (ret == SIMPLEREGION && EqualRect( &rect, &client )) ret = 0;
        NtGdiDeleteObjectApp( region );
    }
    NtUserReleaseDC( hwnd, hdc );

    return ret > 0;
}

static BOOL needs_offscreen_rendering( HWND hwnd, BOOL raw )
{
    HWND toplevel = NtUserGetAncestor( hwnd, GA_ROOT );

    /* Owner-managed publication makes the backing Pixmap authoritative for
     * both Expose repair and multi-surface cut-over.  A directly attached
     * client window would update only the visible host after the generation
     * completed, leaving that backing stale.  Keep every participating X11
     * surface on the composition path while the owner advertises a backing. */
    if (toplevel && X11DRV_get_client_surface_backing( toplevel )) return TRUE;
    if (!raw && NtUserGetDpiForWindow( hwnd ) != NtUserGetWinMonitorDpi( hwnd, MDT_RAW_DPI )) return TRUE; /* needs DPI scaling */
    if (NtUserGetAncestor( hwnd, GA_PARENT ) != NtUserGetDesktopWindow()) return TRUE; /* child window, needs compositing */
    if (NtUserGetWindowRelative( hwnd, GW_CHILD )) return needs_client_window_clipping( hwnd ); /* window has children, needs compositing */
    return FALSE;
}

void set_dc_drawable( HDC hdc, Drawable drawable, const RECT *rect, int mode )
{
    struct x11drv_escape_set_drawable escape =
    {
        .code = X11DRV_SET_DRAWABLE,
        .drawable = drawable,
        .dc_rect = *rect,
        .mode = mode,
    };
    NtGdiExtEscape( hdc, NULL, 0, X11DRV_ESCAPE, sizeof(escape), (LPSTR)&escape, 0, NULL );
}

Drawable get_dc_drawable( HDC hdc, RECT *rect )
{
    struct x11drv_escape_get_drawable escape = {.code = X11DRV_GET_DRAWABLE};
    NtGdiExtEscape( hdc, NULL, 0, X11DRV_ESCAPE, sizeof(escape), (LPSTR)&escape, sizeof(escape), (LPSTR)&escape );
    *rect = escape.dc_rect;
    return escape.drawable;
}

HRGN get_dc_monitor_region( HWND hwnd, HDC hdc )
{
    HRGN region;

    if (!(region = NtGdiCreateRectRgn( 0, 0, 0, 0 ))) return 0;
    if (NtGdiGetRandomRgn( hdc, region, SYSRGN | NTGDI_RGN_MONITOR_DPI ) > 0) return region;
    NtGdiDeleteObjectApp( region );
    return 0;
}

static void x11drv_client_surface_abandon_completion( struct client_surface *client );

static void x11drv_client_surface_destroy( struct client_surface *client )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    HWND hwnd = client->hwnd;

    TRACE( "%s\n", debugstr_client_surface( client ) );

    x11drv_client_surface_abandon_completion( client );
    if (surface->damage_cond_initialized) pthread_cond_destroy( &surface->damage_cond );
    if (surface->colormap != default_colormap) XFreeColormap( gdi_display, surface->colormap );
    if (surface->window) destroy_client_window( hwnd, surface->window );
    if (surface->hdc_dst) NtGdiDeleteObjectApp( surface->hdc_dst );
    if (surface->hdc_src) NtGdiDeleteObjectApp( surface->hdc_src );
}

#if defined(HAVE_X11_EXTENSIONS_XDAMAGE_H) && defined(SONAME_LIBXDAMAGE)
static int xdamage_resource_error( Display *display, XErrorEvent *event, void *arg )
{
    int *error = arg;

    *error = event->error_code;
    return 1;
}
#endif

static BOOL x11drv_client_surface_init_completion( struct x11drv_client_surface *surface )
{
#if defined(HAVE_X11_EXTENSIONS_XDAMAGE_H) && defined(SONAME_LIBXDAMAGE)
    int error = 0;

    if (surface->completion_broken) return FALSE;
    if (surface->damage) return TRUE;
    pthread_once( &xdamage_once, init_xdamage );
    if (!xdamage_display) return FALSE;

    /* The client window is created on gdi_display.  Establish it before the
     * shared event connection creates a Damage object for that XID. */
    XSync( gdi_display, False );
    pthread_mutex_lock( &xdamage_lock );
    X11DRV_expect_error( xdamage_display, xdamage_resource_error, &error );
    surface->damage = pXDamageCreate( xdamage_display, surface->window, XDamageReportNonEmpty );
    XSync( xdamage_display, False );
    X11DRV_check_error();
    if (error) surface->damage = 0;
    else if (surface->damage)
    {
        insert_xdamage_surface_locked( surface );
    }
    pthread_mutex_unlock( &xdamage_lock );
    return !!surface->damage;
#else
    return FALSE;
#endif
}

#if defined(HAVE_X11_EXTENSIONS_XDAMAGE_H) && defined(SONAME_LIBXDAMAGE)
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
         surface = surface->damage_next)
        if (surface->damage == damage) return surface;
    return NULL;
}

static void insert_xdamage_surface_locked( struct x11drv_client_surface *surface )
{
    unsigned int bucket = xdamage_surface_hash( surface->damage );

    surface->damage_next = xdamage_surfaces[bucket];
    xdamage_surfaces[bucket] = surface;
}

static void remove_xdamage_surface_locked( struct x11drv_client_surface *surface )
{
    struct x11drv_client_surface **cursor;

    cursor = &xdamage_surfaces[xdamage_surface_hash( surface->damage )];
    while (*cursor && *cursor != surface) cursor = &(*cursor)->damage_next;
    if (*cursor) *cursor = surface->damage_next;
    surface->damage_next = NULL;
}

static void dispatch_xdamage_events_locked(void)
{
    XEvent event;

    while (XPending( xdamage_display ))
    {
        struct x11drv_client_surface *surface;

        XNextEvent( xdamage_display, &event );
        if (event.type != xdamage_event_base + XDamageNotify) continue;
        surface = find_xdamage_surface_locked( ((XDamageNotifyEvent *)&event)->damage );
        if (!surface) continue;
        surface->completion_ready = TRUE;
        if (surface->completion_waiting)
        {
            list_remove( &surface->damage_wait_entry );
            list_init( &surface->damage_wait_entry );
            surface->completion_waiting = FALSE;
            pthread_cond_signal( &surface->damage_cond );
        }
    }
}

static void wake_xdamage_dispatcher_locked(void)
{
    struct list *entry;
    struct x11drv_client_surface *surface;

    if (!(entry = list_head( &xdamage_waiters ))) return;
    surface = LIST_ENTRY( entry, struct x11drv_client_surface, damage_wait_entry );
    list_remove( entry );
    list_init( entry );
    surface->completion_waiting = FALSE;
    pthread_cond_signal( &surface->damage_cond );
}

static BOOL take_client_surface_damage_locked( struct x11drv_client_surface *surface )
{
    dispatch_xdamage_events_locked();
    if (!surface->completion_ready) return FALSE;

    surface->completion_ready = FALSE;
    return TRUE;
}
#endif

static BOOL x11drv_client_surface_prepare_completion( struct client_surface *client )
{
#if defined(HAVE_X11_EXTENSIONS_XDAMAGE_H) && defined(SONAME_LIBXDAMAGE)
    struct x11drv_client_surface *surface = impl_from_client_surface( client );

    if (!x11drv_client_surface_init_completion( surface ))
    {
        /* The caller will still submit the host present.  Retrying with a new
         * monitor could then mistake that unobserved operation for a future
         * presentation.  Keep the shared monitor disabled, but return a
         * completion token whose wait path supplies an ordered XSync fallback
         * for servers without usable XDamage. */
        surface->completion_broken = TRUE;
        InterlockedExchange( &client->cacheable, FALSE );
        return TRUE;
    }
    do
    {
        pthread_mutex_lock( &xdamage_lock );
        while (take_client_surface_damage_locked( surface ));
        pXDamageSubtract( xdamage_display, surface->damage, None, None );
        /* Empty the region before the WSI request is submitted on its own
         * connection.  Repeat if an earlier update raced the subtraction. */
        XSync( xdamage_display, False );
        if (!take_client_surface_damage_locked( surface ))
        {
            pthread_mutex_unlock( &xdamage_lock );
            break;
        }
        pthread_mutex_unlock( &xdamage_lock );
    } while (1);
    return TRUE;
#else
    return TRUE;
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
    surface->completion_broken = TRUE;
    InterlockedExchange( &client->cacheable, FALSE );
    if (!surface->damage) return;

    pthread_mutex_lock( &xdamage_lock );
    assert( !surface->completion_waiting );
    remove_xdamage_surface_locked( surface );
    /* Synchronize and drain before freeing the ID so a queued event cannot be
     * confused with a later Damage object if Xlib recycles the XID. */
    XSync( xdamage_display, False );
    dispatch_xdamage_events_locked();
    surface->completion_ready = FALSE;
    pXDamageDestroy( xdamage_display, surface->damage );
    XSync( xdamage_display, False );
    dispatch_xdamage_events_locked();
    surface->damage = 0;
    pthread_mutex_unlock( &xdamage_lock );
#endif
}

static BOOL x11drv_client_surface_wait_completion( struct client_surface *client, DWORD timeout )
{
#if defined(HAVE_X11_EXTENSIONS_XDAMAGE_H) && defined(SONAME_LIBXDAMAGE)
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    DWORD start = NtGetTickCount();

    if (!surface->damage)
    {
        XSync( gdi_display, False );
        return TRUE;
    }
    for (;;)
    {
        DWORD elapsed, remaining;
        int ret = 0;

        pthread_mutex_lock( &xdamage_lock );
        if (take_client_surface_damage_locked( surface ))
        {
            pXDamageSubtract( xdamage_display, surface->damage, None, None );
            XFlush( xdamage_display );
            pthread_mutex_unlock( &xdamage_lock );
            return TRUE;
        }
        elapsed = NtGetTickCount() - start;
        remaining = elapsed < timeout ? timeout - elapsed : 0;
        if (!remaining)
        {
            pthread_mutex_unlock( &xdamage_lock );
            WARN( "timed out waiting for presentation completion for %s\n",
                  debugstr_client_surface( client ) );
            x11drv_client_surface_abandon_completion( client );
            return FALSE;
        }

        if (!xdamage_dispatching)
        {
            struct pollfd pollfd = {ConnectionNumber( xdamage_display ), POLLIN, 0};

            /* Elect one waiter to service the dedicated Damage connection.
             * Other surfaces sleep on their own condition instead of every
             * worker scanning the same global Xlib event queue. */
            xdamage_dispatching = TRUE;
            pthread_mutex_unlock( &xdamage_lock );
            ret = poll( &pollfd, 1, (int)min( remaining, (DWORD)10 ) );
            pthread_mutex_lock( &xdamage_lock );
            if (ret > 0 && pollfd.revents & POLLIN) dispatch_xdamage_events_locked();
            xdamage_dispatching = FALSE;
            wake_xdamage_dispatcher_locked();
            pthread_mutex_unlock( &xdamage_lock );

            if (ret < 0 && errno != EINTR)
            {
                WARN( "failed waiting for presentation completion for %s, ret %d, revents %#x\n",
                      debugstr_client_surface( client ), ret, pollfd.revents );
                x11drv_client_surface_abandon_completion( client );
                return FALSE;
            }
        }
        else
        {
            struct timespec abstime;

            clock_gettime( CLOCK_REALTIME, &abstime );
            abstime.tv_nsec += (long)(remaining % 1000) * 1000000;
            abstime.tv_sec += remaining / 1000 + abstime.tv_nsec / 1000000000;
            abstime.tv_nsec %= 1000000000;
            surface->completion_waiting = TRUE;
            list_add_tail( &xdamage_waiters, &surface->damage_wait_entry );
            pthread_cond_timedwait( &surface->damage_cond, &xdamage_lock, &abstime );
            if (surface->completion_waiting)
            {
                list_remove( &surface->damage_wait_entry );
                list_init( &surface->damage_wait_entry );
                surface->completion_waiting = FALSE;
            }
            pthread_mutex_unlock( &xdamage_lock );
        }
    }
#else
    XSync( gdi_display, False );
    return TRUE;
#endif
}

static void x11drv_client_surface_detach( struct client_surface *client )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    Window client_window = surface->window;
    struct x11drv_win_data *data;
    HWND hwnd = client->hwnd;

    TRACE( "%s\n", debugstr_client_surface( client ) );

    if ((data = get_win_data( hwnd )))
    {
        detach_client_window( data, client_window );
        release_win_data( data );
    }
}

static void client_surface_update_geometry( HWND hwnd, struct x11drv_client_surface *surface )
{
    RECT rect = surface->client.raw ? surface->client.monitor_rect : surface->client.virtual_rect;
    XWindowChanges changes = surface->changes;
    int mask = 0;

    changes.x = rect.left;
    changes.y = rect.top;
    changes.width  = min( max( 1, rect.right - rect.left ), 65535 );
    changes.height = min( max( 1, rect.bottom - rect.top ), 65535 );

    if (changes.x != surface->changes.x) mask |= CWX;
    if (changes.y != surface->changes.y) mask |= CWY;
    if (changes.width != surface->changes.width) mask |= CWWidth;
    if (changes.height != surface->changes.height) mask |= CWHeight;
    if (!mask) return;

    surface->changes = changes;
    TRACE( "client window %p/%lx, requesting position %d,%d size %d,%d mask %#x\n", hwnd,
           surface->window, changes.x, changes.y, changes.width, changes.height, mask );
    XConfigureWindow( gdi_display, surface->window, mask, &changes );
    /* The Vulkan WSI connection can submit a present before Xlib's geometry
     * request has reached the server.  If the drawable is still at its old
     * size, the following composition copies only that old extent and leaves
     * the newly exposed area at the host's background pixel.  A size change
     * therefore needs a server round trip before composition; position-only
     * changes only need ordering on this Xlib connection. */
    if (mask & (CWWidth | CWHeight)) XSync( gdi_display, False );
    else XFlush( gdi_display );
}

#ifdef SONAME_LIBXCOMPOSITE
static int client_surface_redirect_error( Display *display, XErrorEvent *event, void *arg )
{
    int *error = arg;

    if (event->error_code != BadAccess && event->error_code != BadWindow) return FALSE;
    *error = event->error_code;
    return TRUE;
}
#endif

static BOOL client_surface_update_offscreen( HWND hwnd, struct x11drv_client_surface *surface )
{
    BOOL offscreen, old_offscreen;
    struct x11drv_win_data *data;

    /* A hidden window needs the mapped dummy parent while it renders.  If it
     * actually presents there, X11DRV_client_surface_present() makes this
     * choice permanent so showing it cannot discard that completed frame. */
    offscreen = !NtUserIsWindowVisible( hwnd ) || surface->keep_offscreen ||
                needs_offscreen_rendering( hwnd, surface->client.raw );

    old_offscreen = InterlockedCompareExchange( &surface->client.offscreen, 0, 0 );
    if (old_offscreen == offscreen)
    {
        if (!offscreen && (data = get_win_data( hwnd )))
        {
            attach_client_window( data, surface->window );
            release_win_data( data );
        }
        return !offscreen || (surface->hdc_src && surface->hdc_dst);
    }
    else
    {
        TRACE( "%s offscreen %u\n", debugstr_client_surface( &surface->client ), offscreen );
    }

    if (!offscreen)
    {
#ifdef SONAME_LIBXCOMPOSITE
        if (surface->manual_redirect)
            pXCompositeUnredirectWindow( gdi_display, surface->window, CompositeRedirectManual );
        surface->manual_redirect = FALSE;
#endif
        if (surface->hdc_dst)
        {
            NtGdiDeleteObjectApp( surface->hdc_dst );
            surface->hdc_dst = NULL;
        }
        if (surface->hdc_src)
        {
            NtGdiDeleteObjectApp( surface->hdc_src );
            surface->hdc_src = NULL;
        }
    }
    else
    {
        static const WCHAR displayW[] = {'D','I','S','P','L','A','Y', 0};
        UNICODE_STRING device_str = RTL_CONSTANT_STRING(displayW);
        RECT rect = surface->client.virtual_rect;
        HDC hdc_dst, hdc_src;

        OffsetRect( &rect, -rect.left, -rect.top );
        hdc_dst = NtGdiOpenDCW( &device_str, NULL, NULL, 0, TRUE, NULL, NULL, NULL );
        hdc_src = NtGdiOpenDCW( &device_str, NULL, NULL, 0, TRUE, NULL, NULL, NULL );

        if (!hdc_dst || !hdc_src)
        {
            if (hdc_dst) NtGdiDeleteObjectApp( hdc_dst );
            if (hdc_src) NtGdiDeleteObjectApp( hdc_src );
            WARN( "failed to allocate offscreen composition DCs for %s\n",
                  debugstr_client_surface( &surface->client ) );
            return FALSE;
        }
        surface->hdc_dst = hdc_dst;
        surface->hdc_src = hdc_src;
        set_dc_drawable( surface->hdc_src, surface->window, &rect, IncludeInferiors );

#ifdef SONAME_LIBXCOMPOSITE
        if (usexcomposite)
        {
            int error = 0;

            X11DRV_expect_error( gdi_display, client_surface_redirect_error, &error );
            pXCompositeRedirectWindow( gdi_display, surface->window, CompositeRedirectManual );
            XSync( gdi_display, False );
            X11DRV_check_error();
            /* BadAccess means a compositing window manager already provides
             * the backing pixmap. Do not later unredirect its ownership. */
            if (!error) surface->manual_redirect = TRUE;
            else if (error == BadAccess)
                TRACE( "client window %p/%lx is compositor-redirected\n", hwnd, surface->window );
            else
            {
                WARN( "failed to redirect client window %lx, X error %d\n", surface->window, error );
                NtGdiDeleteObjectApp( surface->hdc_dst );
                NtGdiDeleteObjectApp( surface->hdc_src );
                surface->hdc_dst = surface->hdc_src = NULL;
                return FALSE;
            }
        }
#endif
    }

    if ((data = get_win_data( hwnd )))
    {
        if (offscreen) detach_client_window( data, surface->window );
        else attach_client_window( data, surface->window );
        release_win_data( data );
    }
    InterlockedExchange( &surface->client.offscreen, offscreen );
    return TRUE;
}

static BOOL x11drv_client_surface_update( struct client_surface *client )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    HWND hwnd = client->hwnd;

    client_surface_update_geometry( hwnd, surface );
    return client_surface_update_offscreen( hwnd, surface );
}

static BOOL copy_client_surface( struct x11drv_client_surface *surface, Drawable target,
                                 const RECT *rect_dst, const RECT *rect_src, HRGN region )
{
    RECT rect;

    if (get_dc_drawable( surface->hdc_dst, &rect ) != target || !EqualRect( &rect, rect_dst ))
        set_dc_drawable( surface->hdc_dst, target, rect_dst, IncludeInferiors );
    /* RGN_COPY with a null region clears a clip left by an earlier present. */
    NtGdiExtSelectClipRgn( surface->hdc_dst, region, RGN_COPY );

    if (rect_dst->right - rect_dst->left == rect_src->right - rect_src->left &&
        rect_dst->bottom - rect_dst->top == rect_src->bottom - rect_src->top)
        return NtGdiBitBlt( surface->hdc_dst, 0, 0, rect_dst->right - rect_dst->left,
                            rect_dst->bottom - rect_dst->top, surface->hdc_src, 0, 0,
                            SRCCOPY, 0, 0 );
    return NtGdiStretchBlt( surface->hdc_dst, 0, 0, rect_dst->right - rect_dst->left,
                            rect_dst->bottom - rect_dst->top, surface->hdc_src, 0, 0,
                            rect_src->right - rect_src->left, rect_src->bottom - rect_src->top,
                            SRCCOPY, 0 );
}

static BOOL X11DRV_client_surface_present( struct client_surface *client, HDC hdc,
                                           HRGN surface_region, BOOL flush )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    HWND hwnd = client->hwnd, toplevel = client->toplevel;
    RECT rect_dst = client->monitor_rect, rect_src, rect_src_dc, rect;
    Drawable window;
    Pixmap backing;
    BOOL ret;
    HRGN region;

    if (!hdc)
    {
        if (flush) XFlush( gdi_display );
        return TRUE;
    }

    /* Reparenting or unredirecting this drawable after a hidden presentation
     * destroys its native back buffer.  Keep only surfaces which really
     * presented while hidden on the stable offscreen composition path. */
    if (!NtUserIsWindowVisible( hwnd )) surface->keep_offscreen = TRUE;

    window = X11DRV_get_whole_window_property( toplevel );
    if (!window || !surface->hdc_src || !surface->hdc_dst) return FALSE;
    if (!surface->composition_backing ||
        surface->composition_toplevel != client->composition_toplevel ||
        surface->composition_scene_generation != client->composition_scene_generation)
    {
        surface->composition_toplevel = client->composition_toplevel;
        surface->composition_scene_generation = client->composition_scene_generation;
        surface->composition_backing = X11DRV_get_client_surface_backing_property( toplevel );
    }
    backing = surface->composition_backing;

    /* Exclusive fullscreen ignores normal window clipping. */
    if (hwnd == toplevel && NtUserGetPresentRect( toplevel, &rect, -1 /* raw dpi */ )) region = 0;
    else if (!(region = get_dc_monitor_region( hwnd, hdc ))) return FALSE;

    if (surface_region)
    {
        if (region) NtGdiCombineRgn( region, region, surface_region, RGN_AND );
        else if ((region = NtGdiCreateRectRgn( 0, 0, 0, 0 )))
            NtGdiCombineRgn( region, surface_region, 0, RGN_COPY );
    }

    rect_src = surface->client.raw ? surface->client.monitor_rect : surface->client.virtual_rect;
    TRACE( "hwnd %p %s to toplevel %p %s region %p\n", hwnd, wine_dbgstr_rect(&rect_src),
           toplevel, wine_dbgstr_rect(&rect_dst), region );

    /* The drawable can change size while this DC remains cached, notably when
     * a hidden Chromium popup grows from its initial 64x64 surface.  Refresh
     * both the drawable and its DC extent before every copy; otherwise GDI
     * clips the source to the stale extent and publishes an unpainted tail. */
    SetRect( &rect_src_dc, 0, 0, rect_src.right - rect_src.left,
             rect_src.bottom - rect_src.top );
    if (get_dc_drawable( surface->hdc_src, &rect ) != surface->window ||
        !EqualRect( &rect, &rect_src_dc ))
        set_dc_drawable( surface->hdc_src, surface->window, &rect_src_dc, IncludeInferiors );

    /* Scene generations are never written into the visible host piecemeal.
     * Steady frames keep the same backing current, then update the visible
     * region; a live or staged generation becomes visible only in the owner
     * process after all renderer commits have reached the server. */
    ret = backing ? copy_client_surface( surface, backing, &rect_dst, &rect_src, region ) : TRUE;
    if (ret && (!flush || !backing))
        ret = copy_client_surface( surface, window, &rect_dst, &rect_src, region );
    if (ret)
    {
        /* A staged generation may aggregate surfaces from several renderer
         * processes.  Complete this connection's copy before acknowledging
         * its generation to the server; ordering only the last renderer's
         * commit event cannot order work submitted on the other connections. */
        if (flush) XSync( gdi_display, False );
        else XFlush( gdi_display );
    }

    if (region) NtGdiDeleteObjectApp( region );
    return ret;
}

static const struct client_surface_backend x11drv_client_surface_backend =
{
    .caps = CLIENT_SURFACE_BACKEND_SCENE_PUBLICATION |
            CLIENT_SURFACE_BACKEND_NATIVE_WRITE_LEASE,
    .destroy = x11drv_client_surface_destroy,
    .detach = x11drv_client_surface_detach,
    .update = x11drv_client_surface_update,
    .present = X11DRV_client_surface_present,
    .prepare_completion = x11drv_client_surface_prepare_completion,
    .wait_completion = x11drv_client_surface_wait_completion,
    .abandon_completion = x11drv_client_surface_abandon_completion,
};

static int visual_class_alloc( int class )
{
    return class == PseudoColor || class == GrayScale || class == DirectColor ? AllocAll : AllocNone;
}

struct x11drv_client_surface *impl_from_client_surface( struct client_surface *client )
{
    assert( client->backend == &x11drv_client_surface_backend );
    return CONTAINING_RECORD( client, struct x11drv_client_surface, client );
}

struct client_surface *X11DRV_CreateClientSurface( HWND hwnd, int format, BOOL raw )
{
    struct x11drv_client_surface *surface;
    XVisualInfo visual = default_visual;
    Colormap colormap;
    RECT rect;

    if (format && !visual_from_pixel_format( format, &visual )) return NULL;

    if (visual.visualid == default_visual.visualid) colormap = default_colormap;
    else colormap = XCreateColormap( gdi_display, get_dummy_parent(), visual.visual, visual_class_alloc( visual.class ) );
    if (!colormap) return NULL;

    if (!(surface = client_surface_create( sizeof(*surface), &x11drv_client_surface_backend, hwnd, format, raw ))) goto failed;
    surface->colormap = colormap;
    list_init( &surface->damage_wait_entry );
    if (pthread_cond_init( &surface->damage_cond, NULL )) goto failed;
    surface->damage_cond_initialized = TRUE;
    rect = raw ? surface->client.monitor_rect : surface->client.virtual_rect;
    if (!(surface->window = create_client_window( hwnd, rect, &visual, colormap ))) goto failed;
    TRACE( "Created %s for client window %lx\n", debugstr_client_surface( &surface->client ), surface->window );
    return &surface->client;

failed:
    if (surface) client_surface_release( &surface->client );
    else if (colormap != default_colormap) XFreeColormap( gdi_display, colormap );
    return NULL;
}
