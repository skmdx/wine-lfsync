/*
 * X11 client surface internals
 *
 * Copyright 2026 Wine contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_X11DRV_CLIENT_SURFACE_H
#define __WINE_X11DRV_CLIENT_SURFACE_H

#include "x11drv.h"

struct x11drv_client_surface;
struct x11drv_client_surface_retirement;

struct x11drv_client_surface_completion
{
    XID damage;
    struct x11drv_client_surface *hash_next;
    struct list wait_entry;
    pthread_cond_t cond;
    BOOL broken; /* shared monitor is tainted until this drawable is destroyed */
    BOOL ready;
    BOOL cond_initialized;
};

struct x11drv_client_source_frame
{
    Pixmap pixmap;
    GC gc;
    unsigned int width, height, depth;
    UINT64 bytes;
    void *image;
    void (*release_image)( void *image );
    BOOL (*image_ready)( void *image );
    BOOL gpu_copy; /* this reservation uses the image's independently owned GPU fence */
};

struct x11drv_client_surface
{
    struct client_surface client;
    XWindowChanges changes;
    Colormap colormap;
    Window window;
    Pixmap snapshot;
    SIZE snapshot_size;
    XImage *snapshot_image;
    GC snapshot_gc;
    UINT64 snapshot_bytes;
    BYTE *snapshot_pixels;
    SIZE_T snapshot_pixels_size;
    VisualID source_visual;
    unsigned int source_depth;
    Pixmap snapshot_import;
    UINT64 snapshot_import_epoch;
    BOOL direct_snapshot;
    Pixmap gpu_snapshot;
    SIZE gpu_snapshot_size;
    struct x11drv_client_source_frame sources[CLIENT_SURFACE_SOURCE_FRAME_COUNT];
    struct x11drv_client_surface_retirement *handoff_retirement;
    struct x11drv_client_surface_retirement *snapshot_retirement;
    struct x11drv_client_surface_completion completion;
    BOOL manual_redirect;   /* client drawable is manually XComposite redirected */

};

extern struct x11drv_client_surface *impl_from_client_surface( struct client_surface *client );
extern const struct client_surface_completion_ops x11drv_client_surface_completion_ops;
extern BOOL x11drv_client_surface_completion_init( struct x11drv_client_surface *surface );
extern void x11drv_client_surface_completion_destroy( struct x11drv_client_surface *surface );
extern BOOL x11drv_client_surface_snapshot( struct client_surface *client, const BYTE *pixels,
                                            unsigned int width, unsigned int height,
                                            BOOL top_down, BOOL bgra );
extern void x11drv_client_surface_release_snapshot_staging( struct x11drv_client_surface *surface );
extern void x11drv_client_surface_set_gpu_snapshot( struct x11drv_client_surface *surface, Pixmap pixmap );
extern BOOL x11drv_client_surface_prepare_retirement( struct x11drv_client_surface *surface );
extern void x11drv_client_surface_retire_handoff( struct client_surface *client );
extern void x11drv_client_surface_destroy_retirement( struct x11drv_client_surface *surface );
extern struct x11drv_client_source_frame *x11drv_client_surface_get_source(
    struct client_surface *client, unsigned int index, unsigned int width,
    unsigned int height, unsigned int depth );

#endif /* __WINE_X11DRV_CLIENT_SURFACE_H */
