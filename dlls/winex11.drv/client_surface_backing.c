/*
 * X11 client surface backing store
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

#include <errno.h>
#include <fcntl.h>
#include <poll.h>

#include "x11drv.h"
#include "xpresent.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);

static const WCHAR client_surface_backing_prop[] =
    {'_','_','w','i','n','e','_','x','1','1','_','c','l','i','e','n','t','_','s','u','r','f','a','c','e','_','b','a','c','k','i','n','g',0};

struct x11drv_retired_pixmap
{
    struct x11drv_retired_pixmap *next;
    Pixmap pixmaps[2];
};

#ifdef SONAME_LIBXPRESENT

static pthread_mutex_t client_surface_compositor_mutex = PTHREAD_MUTEX_INITIALIZER;
static Display *client_surface_compositor_display;
static uint32_t client_surface_present_serial;
static int client_surface_present_opcode;

static int client_surface_present_error( Display *display, XErrorEvent *event, void *arg )
{
    int *error = arg;

    *error = event->error_code;
    return 1;
}

static BOOL client_surface_compositor_open(void)
{
    int event_base, error_base, major, minor;
    Display *display;

    if (client_surface_compositor_display) return TRUE;
    if (!pXGetEventData || !pXFreeEventData) return FALSE;
    if (!(display = XOpenDisplay( NULL ))) return FALSE;
    fcntl( ConnectionNumber( display ), F_SETFD, FD_CLOEXEC );
    if (!pXPresentQueryExtension( display, &client_surface_present_opcode,
                                  &event_base, &error_base ) ||
        !pXPresentQueryVersion( display, &major, &minor ))
    {
        XCloseDisplay( display );
        return FALSE;
    }
    client_surface_compositor_display = display;
    TRACE( "client-surface compositor connection opened with X Present %d.%d\n", major, minor );
    return TRUE;
}

static BOOL wait_client_surface_present_events( Window window, Pixmap pixmap, uint32_t serial )
{
    Display *display = client_surface_compositor_display;
    int fd = ConnectionNumber( display );
    BOOL complete = FALSE, idle = FALSE, skipped = FALSE;
    DWORD start = NtGetTickCount();

    while ((!complete || !idle) && !skipped)
    {
        XEvent event;
        int timeout, ret;

        while (!XPending( display ))
        {
            struct pollfd pfd = {.fd = fd, .events = POLLIN};
            DWORD elapsed = NtGetTickCount() - start;

            if (elapsed >= 5000) return FALSE;
            timeout = 5000 - elapsed;
            do ret = poll( &pfd, 1, timeout ); while (ret < 0 && errno == EINTR);
            if (ret <= 0) return FALSE;
        }
        XNextEvent( display, &event );
        if (event.type != GenericEvent || event.xcookie.extension != client_surface_present_opcode ||
            !pXGetEventData || !pXGetEventData( display, &event ))
            continue;
        if (event.xcookie.evtype == PresentCompleteNotify)
        {
            XPresentCompleteNotifyEvent *notify = event.xcookie.data;

            if (notify->window == window && notify->serial_number == serial &&
                notify->kind == PresentCompleteKindPixmap)
            {
                skipped = notify->mode == PresentCompleteModeSkip;
                complete = !skipped;
            }
        }
        else if (event.xcookie.evtype == PresentIdleNotify)
        {
            XPresentIdleNotifyEvent *notify = event.xcookie.data;

            if (notify->window == window && notify->serial_number == serial &&
                notify->pixmap == pixmap)
                idle = TRUE;
        }
        pXFreeEventData( display, &event );
    }
    if (skipped) return FALSE;
    TRACE( "X Present serial %u pixmap %#lx completed and became idle\n", serial, pixmap );
    return TRUE;
}

/* Serialize the initial implementation on the dedicated compositor
 * connection.  CompleteNotify is the publication boundary; IdleNotify is a
 * separate storage-reuse boundary even when PresentOptionCopy makes both
 * arrive together. */
static BOOL client_surface_backing_present( Window window, Pixmap pixmap )
{
    Display *display;
    XID event_id;
    uint32_t serial;
    BOOL ret = FALSE;
    int error = 0;

    if (!usexpresent) return FALSE;
    pthread_mutex_lock( &client_surface_compositor_mutex );
    if (!client_surface_compositor_open()) goto done;
    display = client_surface_compositor_display;
    if (!(serial = ++client_surface_present_serial)) serial = ++client_surface_present_serial;

    X11DRV_expect_error( display, client_surface_present_error, &error );
    event_id = pXPresentSelectInput( display, window,
                                    PresentCompleteNotifyMask | PresentIdleNotifyMask );
    pXPresentPixmap( display, window, pixmap, serial, None, None, 0, 0, None,
                     None, None, PresentOptionAsync | PresentOptionCopy,
                     0, 0, 0, NULL, 0 );
    XSync( display, False );
    X11DRV_check_error();
    if (!error) ret = wait_client_surface_present_events( window, pixmap, serial );
    pXPresentFreeInput( display, window, event_id );
    XFlush( display );

done:
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    return ret;
}

#else

static BOOL client_surface_backing_present( Window window, Pixmap pixmap )
{
    return FALSE;
}

#endif

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
    if (data->client_surface_backing_spare)
        XFreePixmap( data->display, data->client_surface_backing_spare );
    if (data->client_surface_gc) XFreeGC( data->display, data->client_surface_gc );
    for (retired = data->client_surface_retired; retired; retired = next)
    {
        next = retired->next;
        if (retired->pixmaps[0]) XFreePixmap( data->display, retired->pixmaps[0] );
        if (retired->pixmaps[1]) XFreePixmap( data->display, retired->pixmaps[1] );
        free( retired );
    }
    data->client_surface_backing = 0;
    data->client_surface_backing_spare = 0;
    data->client_surface_gc = 0;
    data->client_surface_backing_width = 0;
    data->client_surface_backing_height = 0;
    data->client_surface_backing_valid_width = 0;
    data->client_surface_backing_valid_height = 0;
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
    unsigned int old_valid_width, old_valid_height;
    BOOL new_gc, old_valid, valid;
    int error = 0;
    Pixmap pixmap, spare;
    GC gc;

    if (!data->whole_window) return FALSE;
    if (!get_client_surface_window_extent( data, &window_width, &window_height )) return FALSE;
    width = client_surface_backing_extent( data->rects.visible.right - data->rects.visible.left );
    height = client_surface_backing_extent( data->rects.visible.bottom - data->rects.visible.top );
    if (data->client_surface_backing && data->client_surface_backing_spare &&
        data->client_surface_backing_width >= width &&
        data->client_surface_backing_height >= height)
    {
        /* Allocation capacity is not content validity.  After a shrink, the
         * unused tail can contain an older scene (or allocation black).  A
         * later growth within the same geometric allocation must therefore
         * invalidate the backing and request a complete recomposition. */
        if (data->client_surface_backing_valid &&
            (window_width > data->client_surface_backing_valid_width ||
             window_height > data->client_surface_backing_valid_height))
        {
            data->client_surface_backing_valid = FALSE;
            data->client_surface_backing_valid_width = 0;
            data->client_surface_backing_valid_height = 0;
        }
        else if (data->client_surface_backing_valid)
        {
            data->client_surface_backing_valid_width =
                min( data->client_surface_backing_valid_width, window_width );
            data->client_surface_backing_valid_height =
                min( data->client_surface_backing_valid_height, window_height );
        }
        return TRUE;
    }

    /* Grow both axes monotonically.  If alternating wide and tall windows
     * replaced one undersized axis while shrinking the other, each resize
     * would retire another Pixmap of the opposite aspect ratio forever.
     * Monotonic extents make every replacement at least double in area and
     * bound all retired allocations by a geometric series. */
    width = max( width, data->client_surface_backing_width );
    height = max( height, data->client_surface_backing_height );
    old_valid = data->client_surface_backing_valid;
    old_valid_width = data->client_surface_backing_valid_width;
    old_valid_height = data->client_surface_backing_valid_height;

    /* Until a larger capability has been committed, the old Pixmap no longer
     * represents the complete host extent and must not satisfy Expose. */
    data->client_surface_backing_valid = FALSE;
    data->client_surface_backing_valid_width = 0;
    data->client_surface_backing_valid_height = 0;

    if (data->client_surface_backing && !(retired = malloc( sizeof(*retired) ))) return FALSE;
    pixmap = XCreatePixmap( data->display, data->whole_window, width, height, data->vis.depth );
    spare = pixmap ? XCreatePixmap( data->display, data->whole_window, width, height,
                                    data->vis.depth ) : 0;
    gc = data->client_surface_gc;
    new_gc = !gc;
    if (!pixmap || !spare || (!gc && !(gc = XCreateGC( data->display, pixmap, 0, NULL ))))
    {
        if (pixmap) XFreePixmap( data->display, pixmap );
        if (spare) XFreePixmap( data->display, spare );
        free( retired );
        return FALSE;
    }

    X11DRV_expect_error( data->display, client_surface_backing_error, &error );
    XCopyArea( data->display, data->whole_window, pixmap, gc, 0, 0,
               min( window_width, width ), min( window_height, height ), 0, 0 );
    XCopyArea( data->display, data->whole_window, spare, gc, 0, 0,
               min( window_width, width ), min( window_height, height ), 0, 0 );
    if (data->client_surface_backing && old_valid)
    {
        XCopyArea( data->display, data->client_surface_backing, pixmap, gc, 0, 0,
                   old_valid_width, old_valid_height, 0, 0 );
        XCopyArea( data->display, data->client_surface_backing, spare, gc, 0, 0,
                   old_valid_width, old_valid_height, 0, 0 );
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
        XFreePixmap( data->display, spare );
        if (new_gc) XFreeGC( data->display, gc );
        XSync( data->display, False );
        X11DRV_check_error();
        free( retired );
        return FALSE;
    }

    if (data->client_surface_backing)
    {
        retired->pixmaps[0] = data->client_surface_backing;
        retired->pixmaps[1] = data->client_surface_backing_spare;
        retired->next = data->client_surface_retired;
        data->client_surface_retired = retired;
    }

    data->client_surface_backing = pixmap;
    data->client_surface_backing_spare = spare;
    data->client_surface_gc = gc;
    data->client_surface_backing_width = width;
    data->client_surface_backing_height = height;
    valid = old_valid && old_valid_width >= window_width && old_valid_height >= window_height;
    data->client_surface_backing_valid = valid;
    if (valid)
    {
        data->client_surface_backing_valid_width = window_width;
        data->client_surface_backing_valid_height = window_height;
    }
    return TRUE;
}

BOOL X11DRV_client_surface_backing_snapshot( struct x11drv_win_data *data, BOOL invalidate )
{
    unsigned int width, height, window_width, window_height;
    Pixmap previous;
    int error = 0;

    if (!X11DRV_client_surface_backing_ensure( data )) return FALSE;
    if (!get_client_surface_window_extent( data, &window_width, &window_height )) return FALSE;
    width = min( data->client_surface_backing_width, window_width );
    height = min( data->client_surface_backing_height, window_height );
    X11DRV_expect_error( data->display, client_surface_backing_error, &error );
    XCopyArea( data->display, data->whole_window, data->client_surface_backing_spare,
               data->client_surface_gc,
               0, 0, width, height, 0, 0 );
    XSync( data->display, False );
    X11DRV_check_error();
    if (error) return FALSE;
    if (width != window_width || height != window_height) return FALSE;
    if (!NtUserSetProp( data->hwnd, client_surface_backing_prop,
                        (HANDLE)data->client_surface_backing_spare ))
        return FALSE;
    previous = data->client_surface_backing;
    data->client_surface_backing = data->client_surface_backing_spare;
    data->client_surface_backing_spare = previous;
    TRACE( "rotated client-surface frame pool to %#lx (idle %#lx)\n",
           data->client_surface_backing, data->client_surface_backing_spare );
    data->client_surface_backing_valid = FALSE;
    data->client_surface_backing_valid_width = 0;
    data->client_surface_backing_valid_height = 0;
    if (!invalidate)
    {
        data->client_surface_backing_valid = TRUE;
        data->client_surface_backing_valid_width = window_width;
        data->client_surface_backing_valid_height = window_height;
    }
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
    if (width != window_width || height != window_height) return FALSE;
    if (!client_surface_backing_present( data->whole_window, data->client_surface_backing ))
    {
        TRACE( "falling back to XCopyArea publication for pixmap %#lx\n",
               data->client_surface_backing );
        X11DRV_expect_error( data->display, client_surface_backing_error, &error );
        XCopyArea( data->display, data->client_surface_backing, data->whole_window,
                   data->client_surface_gc, 0, 0, width, height, 0, 0 );
        XSync( data->display, False );
        X11DRV_check_error();
        if (error) return FALSE;
    }
    data->client_surface_backing_valid = TRUE;
    data->client_surface_backing_valid_width = window_width;
    data->client_surface_backing_valid_height = window_height;
    return TRUE;
}

BOOL X11DRV_client_surface_backing_restore( struct x11drv_win_data *data,
                                           Window window, const RECT *rect )
{
    if (window != data->whole_window || !data->client_surface_backing ||
        !data->client_surface_gc || !data->client_surface_backing_valid || IsRectEmpty( rect ))
        return FALSE;
    if (rect->left < 0 || rect->top < 0 ||
        (unsigned int)rect->right > data->client_surface_backing_valid_width ||
        (unsigned int)rect->bottom > data->client_surface_backing_valid_height)
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
