/*
 * Independently owned X11 client source snapshots
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
#include <fcntl.h>

#include "client_surface.h"
#include "xcomposite.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);

/* Source images own references, not connections. Serial native work in one
 * execution domain can share transport without sharing an image lifetime. */
struct snapshot_connection
{
    struct list entry;
    struct x11drv_client_surface_retired_resource retirement;
    struct list retired_snapshots;
    pthread_cond_t cond;
    UINT64 domain;
    unsigned int refs;
    pthread_mutex_t lock;
    struct x11drv_error_handler errors;
    Display *display;
    int error;
};

static pthread_mutex_t snapshot_connections_lock = PTHREAD_MUTEX_INITIALIZER;
static struct list snapshot_connections = LIST_INIT( snapshot_connections );
static unsigned int snapshot_connection_count;
#define SNAPSHOT_CONNECTION_LIMIT 64

struct x11drv_client_snapshot
{
    struct list retirement_entry;
    LONG refs;
    struct snapshot_connection *connection;
    Pixmap pixmap;
    GC gc;
    XImage *image;
    SIZE size;
    unsigned int depth;
    Window window;
    Pixmap import;
    UINT64 target_epoch, import_epoch;
    UINT64 bytes;
    BOOL acquired;
};

static void destroy_snapshot( struct x11drv_client_snapshot *snapshot );

static void free_snapshot_connection( struct x11drv_client_surface_retired_resource *resource )
{
    struct snapshot_connection *connection = CONTAINING_RECORD( resource, struct snapshot_connection, retirement );

    assert( !connection->refs && list_empty( &connection->retired_snapshots ) );
    pthread_cond_destroy( &connection->cond );
    pthread_mutex_destroy( &connection->lock );
    pthread_mutex_lock( &snapshot_connections_lock );
    --snapshot_connection_count;
    pthread_mutex_unlock( &snapshot_connections_lock );
    TRACE( "released snapshot connection %p domain %s worker %p\n",
           connection, wine_dbgstr_longlong( connection->domain ), resource->thread );
    client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING, sizeof(*connection) );
    free( connection );
}

static void snapshot_retirement_thread( void *context )
{
    struct snapshot_connection *connection = context;
    struct x11drv_client_snapshot *snapshot;

    TRACE( "started snapshot retirement worker for connection %p domain %s\n",
           connection, wine_dbgstr_longlong( connection->domain ) );
    for (;;)
    {
        pthread_mutex_lock( &snapshot_connections_lock );
        while (list_empty( &connection->retired_snapshots ) && connection->refs)
            pthread_cond_wait( &connection->cond, &snapshot_connections_lock );
        if (!connection->refs)
        {
            assert( list_empty( &connection->retired_snapshots ) );
            pthread_mutex_unlock( &snapshot_connections_lock );
            break;
        }
        snapshot = LIST_ENTRY( list_head( &connection->retired_snapshots ), struct x11drv_client_snapshot, retirement_entry );
        list_remove( &snapshot->retirement_entry );
        pthread_mutex_unlock( &snapshot_connections_lock );
        destroy_snapshot( snapshot );
    }
    /* The native connection and its admitted worker have the same fault
     * domain. Neither a stopped copy nor destruction stalls another domain's
     * retirement or the shared mapping/handle observer. */
    if (connection->display)
    {
        XCloseDisplay( connection->display );
        X11DRV_unregister_error_handler( &connection->errors );
        TRACE( "closed snapshot connection %p domain %s\n", connection->display,
               wine_dbgstr_longlong( connection->domain ) );
    }
    x11drv_client_surface_retire_resource( &connection->retirement );
}

static int snapshot_error( Display *display, XErrorEvent *event, void *arg )
{
    struct snapshot_connection *connection = arg;

    connection->error = event->error_code;
    return TRUE;
}

static struct snapshot_connection *snapshot_connection_acquire( UINT64 domain )
{
    struct snapshot_connection *connection;

    pthread_mutex_lock( &snapshot_connections_lock );
    LIST_FOR_EACH_ENTRY( connection, &snapshot_connections, struct snapshot_connection, entry )
    {
        if (connection->domain != domain) continue;
        ++connection->refs;
        pthread_mutex_unlock( &snapshot_connections_lock );
        return connection;
    }
    connection = NULL;
    if (snapshot_connection_count == SNAPSHOT_CONNECTION_LIMIT) goto done;
    if (!client_surface_reserve_memory( CLIENT_SURFACE_MEMORY_STAGING, sizeof(*connection) )) goto done;
    if (!(connection = calloc( 1, sizeof(*connection) )))
    {
        client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING, sizeof(*connection) );
        goto done;
    }
    if (pthread_mutex_init( &connection->lock, NULL ))
    {
        free( connection );
        connection = NULL;
        client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING, sizeof(*connection) );
        goto done;
    }
    if (client_surface_cond_init( &connection->cond ))
    {
        pthread_mutex_destroy( &connection->lock );
        free( connection );
        connection = NULL;
        client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING, sizeof(*connection) );
        goto done;
    }
    list_init( &connection->retired_snapshots );
    connection->retirement.release = free_snapshot_connection;
    connection->domain = domain;
    connection->refs = 1;
    list_add_tail( &snapshot_connections, &connection->entry );
    ++snapshot_connection_count;
done:
    pthread_mutex_unlock( &snapshot_connections_lock );
    return connection;
}

static void snapshot_connection_release( struct snapshot_connection *connection )
{
    if (!connection) return;
    pthread_mutex_lock( &snapshot_connections_lock );
    if (--connection->refs)
    {
        pthread_mutex_unlock( &snapshot_connections_lock );
        return;
    }
    list_remove( &connection->entry );
    if (connection->retirement.thread)
    {
        pthread_cond_signal( &connection->cond );
        pthread_mutex_unlock( &snapshot_connections_lock );
        return;
    }
    pthread_mutex_unlock( &snapshot_connections_lock );
    assert( !connection->display );
    free_snapshot_connection( &connection->retirement );
}

/* The caller holds this domain's native lock. Error attribution belongs to
 * the connection and every request window is drained before the lock returns. */
static BOOL snapshot_connection_open( struct snapshot_connection *connection )
{
    HANDLE thread;
    NTSTATUS status;

    if (connection->display) return TRUE;
    if (!connection->retirement.thread)
    {
        /* Admit native destruction before allocating anything on this Display.
         * Connection initialization is serialized by its native lock. Metadata
         * capacity remains charged until the thread has actually exited. */
        if (!x11drv_client_surface_prepare_resource_retirement()) return FALSE;
        status = PsCreateSystemThread( &thread, THREAD_ALL_ACCESS, NULL, 0, NULL,
                                       snapshot_retirement_thread, connection );
        if (status) return FALSE;
        pthread_mutex_lock( &snapshot_connections_lock );
        connection->retirement.thread = thread;
        pthread_mutex_unlock( &snapshot_connections_lock );
    }
    if (!(connection->display = XOpenDisplay( DisplayString( gdi_display ) ))) return FALSE;
    connection->errors.display = connection->display;
    connection->errors.callback = snapshot_error;
    connection->errors.arg = connection;
    X11DRV_register_error_handler( &connection->errors );
    if (fcntl( ConnectionNumber( connection->display ), F_SETFD, FD_CLOEXEC ) == -1)
    {
        XCloseDisplay( connection->display );
        X11DRV_unregister_error_handler( &connection->errors );
        connection->display = NULL;
        return FALSE;
    }
    TRACE( "opened snapshot connection %p domain %s\n", connection->display,
           wine_dbgstr_longlong( connection->domain ) );
    return TRUE;
}

Pixmap x11drv_client_snapshot_pixmap( const struct x11drv_client_snapshot *snapshot )
{
    return snapshot ? snapshot->pixmap : 0;
}

SIZE x11drv_client_snapshot_size( const struct x11drv_client_snapshot *snapshot )
{
    return snapshot ? snapshot->size : (SIZE){0};
}

void x11drv_client_snapshot_release_staging( struct x11drv_client_snapshot *snapshot )
{
    if (!snapshot || !snapshot->image) return;
    client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING,
        (UINT64)snapshot->image->bytes_per_line * snapshot->image->height );
    XDestroyImage( snapshot->image );
    snapshot->image = NULL;
}

static void destroy_snapshot( struct x11drv_client_snapshot *snapshot )
{
    struct snapshot_connection *connection;
    Display *display;

    if (!snapshot) return;
    assert( !snapshot->refs );
    connection = snapshot->connection;
    display = NULL;
    x11drv_client_snapshot_release_staging( snapshot );
    if (connection && (snapshot->gc || snapshot->import || snapshot->pixmap))
    {
        pthread_mutex_lock( &connection->lock );
        display = connection->display;
        if (snapshot->acquired)
            x11drv_client_surface_trace_image( "retire", "producer_snapshot", display,
                                              snapshot->pixmap, snapshot->bytes );
        if (connection->display && (snapshot->gc || snapshot->import || snapshot->pixmap))
        {
            connection->error = 0;
            if (snapshot->gc) XFreeGC( connection->display, snapshot->gc );
            if (snapshot->import) XFreePixmap( connection->display, snapshot->import );
            if (snapshot->pixmap) XFreePixmap( connection->display, snapshot->pixmap );
            XSync( connection->display, False );
            if (connection->error) WARN( "snapshot destruction returned X error %d\n", connection->error );
        }
        pthread_mutex_unlock( &connection->lock );
    }
    if (snapshot->acquired)
        x11drv_client_surface_trace_image( "free", "producer_snapshot", display,
                                          snapshot->pixmap, snapshot->bytes );
    client_surface_release_memory( CLIENT_SURFACE_MEMORY_SOURCE, snapshot->bytes );
    snapshot_connection_release( connection );
    client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING, sizeof(*snapshot) );
    free( snapshot );
}

struct x11drv_client_snapshot *x11drv_client_snapshot_share( struct x11drv_client_snapshot *snapshot )
{
    /* A frame holds this reference until its checked consumer read finishes.
     * The retained surface image and a working reservation own separate refs. */
    InterlockedIncrement( &snapshot->refs );
    return snapshot;
}

void x11drv_client_snapshot_release( struct x11drv_client_snapshot *snapshot )
{
    struct snapshot_connection *connection;

    if (!snapshot || InterlockedDecrement( &snapshot->refs )) return;
    connection = snapshot->connection;
    pthread_mutex_lock( &snapshot_connections_lock );
    if (connection && connection->retirement.thread)
    {
        list_add_tail( &connection->retired_snapshots, &snapshot->retirement_entry );
        pthread_cond_signal( &connection->cond );
        pthread_mutex_unlock( &snapshot_connections_lock );
        return;
    }
    pthread_mutex_unlock( &snapshot_connections_lock );
    /* No native allocation can precede the connection worker's admission. */
    assert( !snapshot->pixmap && !snapshot->gc && !snapshot->import );
    destroy_snapshot( snapshot );
}

static BOOL snapshot_create_image( struct x11drv_client_snapshot *snapshot )
{
    XImage *image;
    unsigned int width = snapshot->size.cx, height = snapshot->size.cy;
    UINT64 bytes;

    if (snapshot->image) return TRUE;
    if (!(image = XCreateImage( snapshot->connection->display, default_visual.visual, default_visual.depth,
                               ZPixmap, 0, NULL, width, height, 32, 0 ))) return FALSE;
    if (image->bytes_per_line <= 0 || height > ~(SIZE_T)0 / image->bytes_per_line)
    {
        XDestroyImage( image );
        return FALSE;
    }
    bytes = (UINT64)height * image->bytes_per_line;
    if (!client_surface_reserve_memory( CLIENT_SURFACE_MEMORY_STAGING, bytes ))
    {
        XDestroyImage( image );
        return FALSE;
    }
    if (!(image->data = calloc( height, image->bytes_per_line )))
    {
        client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING, bytes );
        XDestroyImage( image );
        return FALSE;
    }
    snapshot->image = image;
    return TRUE;
}

static struct x11drv_client_snapshot *snapshot_alloc( unsigned int width, unsigned int height, unsigned int depth )
{
    struct x11drv_client_snapshot *snapshot;
    UINT64 bytes = (UINT64)width * height * (depth > 16 ? 4 : depth > 8 ? 2 : 1);

    if (!client_surface_reserve_memory( CLIENT_SURFACE_MEMORY_STAGING, sizeof(*snapshot) )) return NULL;
    if (!(snapshot = calloc( 1, sizeof(*snapshot) )))
    {
        client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING, sizeof(*snapshot) );
        return NULL;
    }
    snapshot->size = (SIZE){width, height};
    snapshot->depth = depth;
    snapshot->refs = 1;
    if (!client_surface_reserve_memory( CLIENT_SURFACE_MEMORY_SOURCE, bytes )) goto failed;
    snapshot->bytes = bytes;
    return snapshot;

failed:
    x11drv_client_snapshot_release( snapshot );
    return NULL;
}

static BOOL snapshot_create_native_image( struct x11drv_client_snapshot *snapshot )
{
    struct snapshot_connection *connection = snapshot->connection;
    XGCValues values = {.graphics_exposures = False};
    Display *display;

    if (snapshot->acquired) return TRUE;
    if (!snapshot_connection_open( connection )) return FALSE;
    display = connection->display;
    connection->error = 0;
    snapshot->pixmap = XCreatePixmap( display, DefaultRootWindow( display ),
                                      snapshot->size.cx, snapshot->size.cy, snapshot->depth );
    XSync( display, False );
    if (connection->error || !snapshot->pixmap)
    {
        snapshot->pixmap = 0; /* A rejected allocation is not a resource to free. */
        return FALSE;
    }
    snapshot->gc = XCreateGC( display, snapshot->pixmap, GCGraphicsExposures, &values );
    XSync( display, False );
    if (!snapshot->gc || connection->error)
    {
        /* Xlib may retain a client GC even if the server rejected its XID. */
        if (snapshot->gc) XFreeGC( display, snapshot->gc );
        XFreePixmap( display, snapshot->pixmap );
        XSync( display, False );
        snapshot->gc = NULL;
        snapshot->pixmap = 0;
        return FALSE;
    }
    snapshot->acquired = TRUE;
    x11drv_client_surface_trace_image( "acquire", "producer_snapshot", display,
                                      snapshot->pixmap, snapshot->bytes );
    return TRUE;
}

BOOL x11drv_client_snapshot_prepare_native( struct x11drv_client_snapshot **storage, Window window,
                                           unsigned int width, unsigned int height, unsigned int depth, UINT64 epoch )
{
    struct x11drv_client_snapshot *snapshot = *storage, *next;

    if (!width || !height || width > 0xffff || height > 0xffff) return FALSE;
    if (!snapshot || snapshot->size.cx != width || snapshot->size.cy != height || snapshot->depth != depth ||
        InterlockedCompareExchange( &snapshot->refs, 0, 0 ) != 1)
    {
        if (!(next = snapshot_alloc( width, height, depth ))) return FALSE;
        x11drv_client_snapshot_release( snapshot );
        *storage = snapshot = next;
    }
    /* Native storage is allocated only after admitting its connection worker. */
    assert( !snapshot->window || snapshot->window == window );
    snapshot->window = window;
    snapshot->target_epoch = epoch;
    return TRUE;
}

BOOL x11drv_client_snapshot_prepare_read( struct x11drv_client_snapshot **storage )
{
    struct x11drv_client_snapshot *snapshot = *storage, *next;
    UINT64 domain;

    assert( snapshot && snapshot->refs == 1 );
    if (!client_surface_get_execution_domain( &domain )) return FALSE;
    if (snapshot->connection && snapshot->connection->domain != domain)
    {
        if (!(next = snapshot_alloc( snapshot->size.cx, snapshot->size.cy, snapshot->depth ))) return FALSE;
        next->window = snapshot->window;
        next->target_epoch = snapshot->target_epoch;
        x11drv_client_snapshot_release( snapshot );
        *storage = snapshot = next;
    }
    if (!snapshot->connection) snapshot->connection = snapshot_connection_acquire( domain );
    return !!snapshot->connection;
}

BOOL x11drv_client_snapshot_read_native( void *context )
{
    struct x11drv_client_snapshot *snapshot = context;
    struct snapshot_connection *connection = snapshot->connection;
    Display *display;
    BOOL ret = FALSE;

    assert( snapshot->refs == 1 && snapshot->window && connection );
    pthread_mutex_lock( &connection->lock );
    if (!snapshot_create_native_image( snapshot )) goto done;
    display = connection->display;
    connection->error = 0;
#ifdef SONAME_LIBXCOMPOSITE
    if (!snapshot->import || snapshot->import_epoch != snapshot->target_epoch)
    {
        if (snapshot->import) XFreePixmap( display, snapshot->import );
        snapshot->import = pXCompositeNameWindowPixmap( display, snapshot->window );
        XSync( display, False );
        if (connection->error)
        {
            /* An unsuccessful NameWindowPixmap XID must not be freed. */
            snapshot->import = 0;
            goto done;
        }
        snapshot->import_epoch = snapshot->target_epoch;
    }
#endif
    if (!snapshot->import) goto done;
    XCopyArea( display, snapshot->import, snapshot->pixmap, snapshot->gc,
               0, 0, snapshot->size.cx, snapshot->size.cy, 0, 0 );
    XSync( display, False );
    if (connection->error)
    {
        TRACE( "native source copy failed with X error %d\n", connection->error );
        goto done;
    }
    TRACE( "copied native window %#lx into private snapshot %#lx display %p epoch %s\n",
           snapshot->window, snapshot->pixmap, display, wine_dbgstr_longlong( snapshot->target_epoch ) );
    ret = TRUE;
done:
    pthread_mutex_unlock( &connection->lock );
    return ret;
}

const struct x11drv_snapshot_format x11drv_snapshot_rgba8 = {4, 0xff, 0xff00, 0xff0000, 0xff000000};

static unsigned long snapshot_component( unsigned int pixel, unsigned int source_mask, unsigned long mask )
{
    unsigned int shift = 0, source_shift = 0;

    if (!mask) return 0;
    if (!source_mask) return mask; /* formats without alpha are opaque */
    while (!(mask & (1ul << shift))) ++shift;
    while (!(source_mask & (1u << source_shift))) ++source_shift;
    return ((UINT64)((pixel & source_mask) >> source_shift) * (mask >> shift) /
            (source_mask >> source_shift)) << shift;
}

/* No surface or target state is borrowed by this operation. A sole owner may
 * reuse storage; another reference instead requires an independent image. */
BOOL x11drv_client_snapshot_upload( struct x11drv_client_snapshot **storage, const BYTE *pixels,
                                    unsigned int width, unsigned int height, BOOL top_down,
                                    const struct x11drv_snapshot_format *format )
{
    struct x11drv_client_snapshot *snapshot = *storage, *next;
    struct snapshot_connection *connection;
    XImage *image;
    unsigned int x, y;
    UINT64 domain;
    BOOL ret;
    unsigned long alpha = ((1ull << default_visual.depth) - 1) &
                          ~(default_visual.red_mask | default_visual.green_mask | default_visual.blue_mask);

    if (!width || !height || width > 0xffff || height > 0xffff) return FALSE;
    if (!client_surface_get_execution_domain( &domain )) return FALSE;
    if (!snapshot || snapshot->size.cx != width || snapshot->size.cy != height ||
        !snapshot->connection || snapshot->connection->domain != domain ||
        InterlockedCompareExchange( &snapshot->refs, 0, 0 ) != 1)
    {
        if (!(next = snapshot_alloc( width, height, default_visual.depth ))) return FALSE;
        if ((next->connection = snapshot_connection_acquire( domain )))
        {
            pthread_mutex_lock( &next->connection->lock );
            ret = snapshot_create_native_image( next ) && snapshot_create_image( next );
            pthread_mutex_unlock( &next->connection->lock );
        }
        else ret = FALSE;
        if (!ret)
        {
            x11drv_client_snapshot_release( next );
            return FALSE;
        }
        x11drv_client_snapshot_release( snapshot );
        *storage = snapshot = next;
    }
    connection = snapshot->connection;
    pthread_mutex_lock( &connection->lock );
    ret = snapshot_create_image( snapshot );
    pthread_mutex_unlock( &connection->lock );
    if (!ret) return FALSE;
    image = snapshot->image;
    if (format->texel_size == 4 && format->green_mask == 0xff00 &&
        (format->red_mask == 0xff || format->red_mask == 0xff0000) &&
        image->bits_per_pixel == 32 && image->byte_order == LSBFirst &&
        default_visual.red_mask == 0xff0000 && default_visual.green_mask == 0xff00 &&
        default_visual.blue_mask == 0xff)
    {
        for (y = 0; y < height; ++y)
        {
            BYTE *row = (BYTE *)image->data +
                        (SIZE_T)(top_down ? y : height - y - 1) * image->bytes_per_line;

            if (format->red_mask == 0xff0000)
            {
                memcpy( row, pixels, (SIZE_T)width * 4 );
                pixels += (SIZE_T)width * 4;
                continue;
            }

            for (x = 0; x < width; ++x, pixels += 4, row += 4)
            {
                row[0] = pixels[2];
                row[1] = pixels[1];
                row[2] = pixels[0];
                row[3] = pixels[3];
            }
        }
    }
    else
        for (y = 0; y < height; ++y)
            for (x = 0; x < width; ++x, pixels += format->texel_size)
            {
                unsigned int pixel = 0;

                memcpy( &pixel, pixels, format->texel_size );
                XPutPixel( image, x, top_down ? y : height - y - 1,
                           snapshot_component( pixel, format->red_mask, default_visual.red_mask ) |
                           snapshot_component( pixel, format->green_mask, default_visual.green_mask ) |
                           snapshot_component( pixel, format->blue_mask, default_visual.blue_mask ) |
                           snapshot_component( pixel, format->alpha_mask, alpha ) );
            }

    pthread_mutex_lock( &connection->lock );
    connection->error = 0;
    XPutImage( connection->display, snapshot->pixmap, snapshot->gc, image, 0, 0, 0, 0, width, height );
    XSync( connection->display, False );
    ret = !connection->error;
    if (!ret) TRACE( "snapshot upload failed with X error %d\n", connection->error );
    pthread_mutex_unlock( &connection->lock );
    if (!ret)
    {
        x11drv_client_snapshot_release( snapshot );
        *storage = NULL;
        return FALSE;
    }
    TRACE( "uploaded producer snapshot %#lx on private display %p size %ux%u\n",
           snapshot->pixmap, connection->display, width, height );
    return TRUE;
}
