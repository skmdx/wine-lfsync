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

#include <fcntl.h>

#include "client_surface.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);

struct x11drv_client_snapshot
{
    struct x11drv_error_handler errors;
    Display *display;
    Pixmap pixmap;
    GC gc;
    XImage *image;
    SIZE size;
    UINT64 bytes;
    int error;
    BOOL acquired;
};

static int snapshot_error( Display *display, XErrorEvent *event, void *arg )
{
    struct x11drv_client_snapshot *snapshot = arg;

    snapshot->error = event->error_code;
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

void x11drv_client_snapshot_destroy( struct x11drv_client_snapshot *snapshot )
{
    if (!snapshot) return;
    x11drv_client_snapshot_release_staging( snapshot );
    if (snapshot->acquired)
        x11drv_client_surface_trace_image( "retire", "producer_snapshot", snapshot->display,
                                          snapshot->pixmap, snapshot->bytes );
    if (snapshot->display)
    {
        /* Closing this private connection releases all its server resources,
         * including XIDs whose allocation failed. Keep the error sink alive
         * through the close; an unsuccessful XID is never freed separately. */
        if (snapshot->gc) XFreeGC( snapshot->display, snapshot->gc );
        XCloseDisplay( snapshot->display );
        X11DRV_unregister_error_handler( &snapshot->errors );
    }
    if (snapshot->acquired)
        x11drv_client_surface_trace_image( "free", "producer_snapshot", snapshot->display,
                                          snapshot->pixmap, snapshot->bytes );
    client_surface_release_memory( CLIENT_SURFACE_MEMORY_SOURCE, snapshot->bytes );
    client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING, sizeof(*snapshot) );
    free( snapshot );
}

static BOOL snapshot_create_image( struct x11drv_client_snapshot *snapshot )
{
    XImage *image;
    unsigned int width = snapshot->size.cx, height = snapshot->size.cy;
    UINT64 bytes;

    if (snapshot->image) return TRUE;
    if (!(image = XCreateImage( snapshot->display, default_visual.visual, default_visual.depth,
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

static struct x11drv_client_snapshot *snapshot_create( unsigned int width, unsigned int height )
{
    struct x11drv_client_snapshot *snapshot;
    UINT64 bytes = (UINT64)width * height * (default_visual.depth > 16 ? 4 : default_visual.depth > 8 ? 2 : 1);

    if (!client_surface_reserve_memory( CLIENT_SURFACE_MEMORY_STAGING, sizeof(*snapshot) )) return NULL;
    if (!(snapshot = calloc( 1, sizeof(*snapshot) )))
    {
        client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING, sizeof(*snapshot) );
        return NULL;
    }
    snapshot->size = (SIZE){width, height};
    if (!client_surface_reserve_memory( CLIENT_SURFACE_MEMORY_SOURCE, bytes )) goto failed;
    snapshot->bytes = bytes;
    if (!(snapshot->display = XOpenDisplay( DisplayString( gdi_display ) ))) goto failed;
    snapshot->errors.display = snapshot->display;
    snapshot->errors.callback = snapshot_error;
    snapshot->errors.arg = snapshot;
    X11DRV_register_error_handler( &snapshot->errors );
    if (fcntl( ConnectionNumber( snapshot->display ), F_SETFD, FD_CLOEXEC ) == -1) goto failed;
    if (!snapshot_create_image( snapshot )) goto failed;
    snapshot->pixmap = XCreatePixmap( snapshot->display, DefaultRootWindow( snapshot->display ),
                                      width, height, default_visual.depth );
    snapshot->gc = XCreateGC( snapshot->display, snapshot->pixmap, 0, NULL );
    XSync( snapshot->display, False );
    if (!snapshot->pixmap || !snapshot->gc || snapshot->error) goto failed;
    snapshot->acquired = TRUE;
    x11drv_client_surface_trace_image( "acquire", "producer_snapshot", snapshot->display,
                                      snapshot->pixmap, snapshot->bytes );
    return snapshot;

failed:
    x11drv_client_snapshot_destroy( snapshot );
    return NULL;
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

/* No surface or target state is borrowed by this operation. Its sole owner
 * may exchange a completed snapshot, but cannot upload into a borrowed one. */
BOOL x11drv_client_snapshot_upload( struct x11drv_client_snapshot **storage, const BYTE *pixels,
                                    unsigned int width, unsigned int height, BOOL top_down,
                                    const struct x11drv_snapshot_format *format )
{
    struct x11drv_client_snapshot *snapshot = *storage, *next;
    XImage *image;
    unsigned int x, y;
    unsigned long alpha = ((1ull << default_visual.depth) - 1) &
                          ~(default_visual.red_mask | default_visual.green_mask | default_visual.blue_mask);

    if (!width || !height || width > 0xffff || height > 0xffff) return FALSE;
    if (!snapshot || snapshot->size.cx != width || snapshot->size.cy != height)
    {
        if (!(next = snapshot_create( width, height ))) return FALSE;
        x11drv_client_snapshot_destroy( snapshot );
        *storage = snapshot = next;
    }
    if (!snapshot_create_image( snapshot )) return FALSE;
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

    snapshot->error = 0;
    XPutImage( snapshot->display, snapshot->pixmap, snapshot->gc, image, 0, 0, 0, 0, width, height );
    XSync( snapshot->display, False );
    if (snapshot->error)
    {
        TRACE( "snapshot upload failed with X error %d\n", snapshot->error );
        x11drv_client_snapshot_destroy( snapshot );
        *storage = NULL;
        return FALSE;
    }
    TRACE( "uploaded producer snapshot %#lx on private display %p size %ux%u\n",
           snapshot->pixmap, snapshot->display, width, height );
    return TRUE;
}
