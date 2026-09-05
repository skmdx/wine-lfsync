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

static pthread_mutex_t client_surface_compositor_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t client_surface_compositor_cond = PTHREAD_COND_INITIALIZER;
static Display *client_surface_compositor_display;
static BOOL client_surface_compositor_started;

enum client_surface_compositor_op
{
    CLIENT_SURFACE_COMPOSITOR_ALLOC_POOL,
    CLIENT_SURFACE_COMPOSITOR_COPY,
    CLIENT_SURFACE_COMPOSITOR_FREE_POOL,
    CLIENT_SURFACE_COMPOSITOR_PRESENT,
};

struct client_surface_compositor_job
{
    struct client_surface_compositor_job *next;
    enum client_surface_compositor_op op;
    Drawable source;
    Drawable destination;
    int source_x;
    int source_y;
    int destination_x;
    int destination_y;
    unsigned int width;
    unsigned int height;
    unsigned int depth;
    Pixmap pixmaps[2];
    BOOL result;
    BOOL complete;
};

static struct client_surface_compositor_job *client_surface_compositor_head;
static struct client_surface_compositor_job **client_surface_compositor_tail =
    &client_surface_compositor_head;

#ifdef SONAME_LIBXPRESENT

static uint32_t client_surface_present_serial;
static int client_surface_present_opcode;

#endif

static int client_surface_compositor_error( Display *display, XErrorEvent *event, void *arg )
{
    int *error = arg;

    *error = event->error_code;
    return 1;
}

static BOOL client_surface_compositor_open(void)
{
    Display *display;

    if (client_surface_compositor_display) return TRUE;
    if (!(display = XOpenDisplay( DisplayString( gdi_display ) ))) return FALSE;
    fcntl( ConnectionNumber( display ), F_SETFD, FD_CLOEXEC );

#ifdef SONAME_LIBXPRESENT
    if (usexpresent)
    {
        int event_base, error_base, major, minor;

        if (!pXGetEventData || !pXFreeEventData ||
            !pXPresentQueryExtension( display, &client_surface_present_opcode,
                                      &event_base, &error_base ) ||
            !pXPresentQueryVersion( display, &major, &minor ))
            usexpresent = FALSE;
        else
            TRACE( "client-surface compositor connection opened with X Present %d.%d\n",
                   major, minor );
    }
#endif

    client_surface_compositor_display = display;
    if (!usexpresent) TRACE( "client-surface compositor connection opened with XCopy fallback\n" );
    return TRUE;
}

static BOOL client_surface_copy_on_compositor( Drawable source, Drawable destination,
                                               int source_x, int source_y,
                                               int destination_x, int destination_y,
                                               unsigned int width, unsigned int height )
{
    Display *display = client_surface_compositor_display;
    int error = 0;
    GC gc;

    X11DRV_expect_error( display, client_surface_compositor_error, &error );
    gc = XCreateGC( display, destination, 0, NULL );
    if (gc)
    {
        XCopyArea( display, source, destination, gc, source_x, source_y,
                   width, height, destination_x, destination_y );
        XFreeGC( display, gc );
    }
    XSync( display, False );
    X11DRV_check_error();
    return gc && !error;
}

static BOOL client_surface_alloc_on_compositor( Drawable drawable, unsigned int width,
                                                unsigned int height, unsigned int depth,
                                                Pixmap pixmaps[2] )
{
    Display *display = client_surface_compositor_display;
    int error = 0;

    X11DRV_expect_error( display, client_surface_compositor_error, &error );
    pixmaps[0] = XCreatePixmap( display, drawable, width, height, depth );
    pixmaps[1] = XCreatePixmap( display, drawable, width, height, depth );
    XSync( display, False );
    X11DRV_check_error();
    if (!error) return TRUE;

    X11DRV_expect_error( display, client_surface_compositor_error, &error );
    XFreePixmap( display, pixmaps[0] );
    XFreePixmap( display, pixmaps[1] );
    XSync( display, False );
    X11DRV_check_error();
    pixmaps[0] = pixmaps[1] = 0;
    return FALSE;
}

static BOOL client_surface_free_on_compositor( const Pixmap pixmaps[2] )
{
    Display *display = client_surface_compositor_display;
    int error = 0;

    X11DRV_expect_error( display, client_surface_compositor_error, &error );
    if (pixmaps[0]) XFreePixmap( display, pixmaps[0] );
    if (pixmaps[1]) XFreePixmap( display, pixmaps[1] );
    XSync( display, False );
    X11DRV_check_error();
    return !error;
}

#ifdef SONAME_LIBXPRESENT

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
static BOOL client_surface_present_on_compositor( Window window, Pixmap pixmap )
{
    Display *display = client_surface_compositor_display;
    XID event_id;
    uint32_t serial;
    BOOL ret = FALSE;
    int error = 0;

    if (!usexpresent) return FALSE;
    if (!(serial = ++client_surface_present_serial)) serial = ++client_surface_present_serial;

    X11DRV_expect_error( display, client_surface_compositor_error, &error );
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
    return ret;
}

#else

static BOOL client_surface_present_on_compositor( Window window, Pixmap pixmap )
{
    return FALSE;
}

#endif

static BOOL execute_client_surface_compositor_job( struct client_surface_compositor_job *job )
{
    if (!client_surface_compositor_open()) return FALSE;
    switch (job->op)
    {
    case CLIENT_SURFACE_COMPOSITOR_ALLOC_POOL:
        return client_surface_alloc_on_compositor( job->destination, job->width,
                                                   job->height, job->depth, job->pixmaps );
    case CLIENT_SURFACE_COMPOSITOR_FREE_POOL:
        return client_surface_free_on_compositor( job->pixmaps );
    case CLIENT_SURFACE_COMPOSITOR_PRESENT:
        return client_surface_present_on_compositor( job->destination, job->source );
    case CLIENT_SURFACE_COMPOSITOR_COPY:
        break;
    default:
        return FALSE;
    }
    return client_surface_copy_on_compositor( job->source, job->destination,
                                              job->source_x, job->source_y,
                                              job->destination_x, job->destination_y,
                                              job->width, job->height );
}

static void client_surface_compositor_thread( void *context )
{
    (void)context;

    for (;;)
    {
        struct client_surface_compositor_job *job;

        pthread_mutex_lock( &client_surface_compositor_mutex );
        while (!(job = client_surface_compositor_head))
            pthread_cond_wait( &client_surface_compositor_cond,
                               &client_surface_compositor_mutex );
        client_surface_compositor_head = job->next;
        if (!client_surface_compositor_head)
            client_surface_compositor_tail = &client_surface_compositor_head;
        pthread_mutex_unlock( &client_surface_compositor_mutex );

        job->result = execute_client_surface_compositor_job( job );

        pthread_mutex_lock( &client_surface_compositor_mutex );
        job->complete = TRUE;
        pthread_cond_broadcast( &client_surface_compositor_cond );
        pthread_mutex_unlock( &client_surface_compositor_mutex );
    }
}

static BOOL submit_client_surface_compositor_job( struct client_surface_compositor_job *job )
{
    HANDLE thread;
    NTSTATUS status;

    job->next = NULL;
    job->complete = FALSE;
    pthread_mutex_lock( &client_surface_compositor_mutex );
    if (!client_surface_compositor_started)
    {
        status = PsCreateSystemThread( &thread, THREAD_ALL_ACCESS, NULL, 0, NULL,
                                       client_surface_compositor_thread, NULL );
        if (status)
        {
            pthread_mutex_unlock( &client_surface_compositor_mutex );
            WARN( "failed to create client-surface compositor, status %#lx\n",
                  (unsigned long)status );
            return FALSE;
        }
        NtClose( thread );
        client_surface_compositor_started = TRUE;
    }
    *client_surface_compositor_tail = job;
    client_surface_compositor_tail = &job->next;
    pthread_cond_broadcast( &client_surface_compositor_cond );
    while (!job->complete)
        pthread_cond_wait( &client_surface_compositor_cond,
                           &client_surface_compositor_mutex );
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    return job->result;
}

static BOOL client_surface_backing_copy_area( Drawable source, Drawable destination,
                                              int source_x, int source_y,
                                              int destination_x, int destination_y,
                                              unsigned int width, unsigned int height )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_COPY,
        .source = source,
        .destination = destination,
        .source_x = source_x,
        .source_y = source_y,
        .destination_x = destination_x,
        .destination_y = destination_y,
        .width = width,
        .height = height,
    };

    return submit_client_surface_compositor_job( &job );
}

static BOOL client_surface_backing_alloc( Drawable drawable, unsigned int width,
                                          unsigned int height, unsigned int depth,
                                          Pixmap *first, Pixmap *second )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_ALLOC_POOL,
        .destination = drawable,
        .width = width,
        .height = height,
        .depth = depth,
    };

    if (!submit_client_surface_compositor_job( &job )) return FALSE;
    *first = job.pixmaps[0];
    *second = job.pixmaps[1];
    return TRUE;
}

static void client_surface_backing_free( Pixmap first, Pixmap second )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_FREE_POOL,
        .pixmaps = {first, second},
    };

    if (!submit_client_surface_compositor_job( &job ))
        WARN( "failed to release client-surface frame pool %#lx/%#lx\n", first, second );
}

static BOOL client_surface_backing_copy( Drawable source, Drawable destination,
                                         unsigned int width, unsigned int height )
{
    return client_surface_backing_copy_area( source, destination, 0, 0, 0, 0,
                                             width, height );
}

static BOOL client_surface_backing_present( Window window, Pixmap pixmap )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_PRESENT,
        .source = pixmap,
        .destination = window,
    };

    return submit_client_surface_compositor_job( &job );
}

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

void X11DRV_client_surface_backing_destroy( struct x11drv_win_data *data )
{
    struct x11drv_retired_pixmap *retired, *next;

    NtUserRemoveProp( data->hwnd, client_surface_backing_prop );
    if (data->client_surface_backing || data->client_surface_backing_spare)
        client_surface_backing_free( data->client_surface_backing,
                                     data->client_surface_backing_spare );
    for (retired = data->client_surface_retired; retired; retired = next)
    {
        next = retired->next;
        client_surface_backing_free( retired->pixmaps[0], retired->pixmaps[1] );
        free( retired );
    }
    data->client_surface_backing = 0;
    data->client_surface_backing_spare = 0;
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
    BOOL old_valid, valid;
    Pixmap pixmap, spare;

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
    if (!client_surface_backing_alloc( data->whole_window, width, height,
                                       data->vis.depth, &pixmap, &spare ))
    {
        free( retired );
        return FALSE;
    }

    /* The HWND property is the renderer-visible capability for this backing.
     * Commit it before replacing local state.  Otherwise a failed property
     * allocation could make the owner publish one Pixmap while renderers keep
     * writing the old one. */
    if (!client_surface_backing_copy( data->whole_window, pixmap,
                                      min( window_width, width ), min( window_height, height ) ) ||
        !client_surface_backing_copy( data->whole_window, spare,
                                      min( window_width, width ), min( window_height, height ) ) ||
        (data->client_surface_backing && old_valid &&
         (!client_surface_backing_copy( data->client_surface_backing, pixmap,
                                        old_valid_width, old_valid_height ) ||
          !client_surface_backing_copy( data->client_surface_backing, spare,
                                        old_valid_width, old_valid_height ))) ||
        !NtUserSetProp( data->hwnd, client_surface_backing_prop, (HANDLE)pixmap ))
    {
        client_surface_backing_free( pixmap, spare );
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

    if (!X11DRV_client_surface_backing_ensure( data )) return FALSE;
    if (!get_client_surface_window_extent( data, &window_width, &window_height )) return FALSE;
    width = min( data->client_surface_backing_width, window_width );
    height = min( data->client_surface_backing_height, window_height );
    /* The compositor uses another X connection.  Establish all preceding
     * owner-window drawing before it snapshots that drawable. */
    XSync( data->display, False );
    if (!client_surface_backing_copy( data->whole_window,
                                      data->client_surface_backing_spare, width, height ))
        return FALSE;
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

    if (!data->whole_window || !data->client_surface_backing)
        return FALSE;
    if (!get_client_surface_window_extent( data, &window_width, &window_height )) return FALSE;
    width = min( data->client_surface_backing_width, window_width );
    height = min( data->client_surface_backing_height, window_height );
    if (width != window_width || height != window_height) return FALSE;
    if (!client_surface_backing_present( data->whole_window, data->client_surface_backing ))
    {
        TRACE( "falling back to XCopyArea publication for pixmap %#lx\n",
               data->client_surface_backing );
        if (!client_surface_backing_copy( data->client_surface_backing,
                                          data->whole_window, width, height ))
            return FALSE;
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
        !data->client_surface_backing_valid || IsRectEmpty( rect ))
        return FALSE;
    if (rect->left < 0 || rect->top < 0 ||
        (unsigned int)rect->right > data->client_surface_backing_valid_width ||
        (unsigned int)rect->bottom > data->client_surface_backing_valid_height)
        return FALSE;
    return client_surface_backing_copy_area( data->client_surface_backing,
                                             data->whole_window,
                                             rect->left, rect->top,
                                             rect->left, rect->top,
                                             rect->right - rect->left,
                                             rect->bottom - rect->top );
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
