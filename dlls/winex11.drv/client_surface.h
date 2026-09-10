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
struct x11drv_client_snapshot;

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
    /* The snapshot owns native storage and its pending GPU write. The source
     * slot owns a reference until its checked consumer read completes. */
    struct x11drv_client_snapshot *snapshot;
    Pixmap pixmap;
    unsigned int width, height, depth;
    BOOL gpu_copy; /* this reservation uses the image's independently owned GPU fence */
};

struct x11drv_client_surface
{
    struct client_surface client;
    XWindowChanges changes;
    Colormap colormap;
    Window window;
    struct x11drv_client_snapshot *snapshot;
    BYTE *snapshot_pixels;
    SIZE_T snapshot_pixels_size;
    VisualID source_visual;
    unsigned int source_depth;
    BOOL direct_snapshot;
    struct x11drv_client_snapshot *gpu_snapshot;
    struct x11drv_client_source_frame sources[CLIENT_SURFACE_SOURCE_FRAME_COUNT];
    struct x11drv_client_surface_retirement *handoff_retirement;
    struct x11drv_client_surface_completion completion;
    BOOL manual_redirect;   /* client drawable is manually XComposite redirected */

};

extern struct x11drv_client_surface *impl_from_client_surface( struct client_surface *client );
extern const struct client_surface_completion_ops x11drv_client_surface_completion_ops;
extern BOOL x11drv_client_surface_completion_init( struct x11drv_client_surface *surface );
extern void x11drv_client_surface_completion_destroy( struct x11drv_client_surface *surface );
struct x11drv_snapshot_format
{
    unsigned int texel_size;
    unsigned int red_mask, green_mask, blue_mask, alpha_mask;
};
extern const struct x11drv_snapshot_format x11drv_snapshot_rgba8;
/* Upload replaces shared storage rather than modifying a published image.
 * Every native image has an admitted connection worker for its last release.
 * Only metadata without native storage can be destroyed by its caller. */
extern BOOL x11drv_client_snapshot_upload( struct x11drv_client_snapshot **snapshot, const BYTE *pixels,
                                          unsigned int width, unsigned int height, BOOL top_down,
                                          const struct x11drv_snapshot_format *format );
extern Pixmap x11drv_client_snapshot_pixmap( const struct x11drv_client_snapshot *snapshot );
extern SIZE x11drv_client_snapshot_size( const struct x11drv_client_snapshot *snapshot );
extern void x11drv_client_snapshot_release_staging( struct x11drv_client_snapshot *snapshot );
extern struct x11drv_client_snapshot *x11drv_client_snapshot_share( struct x11drv_client_snapshot *snapshot );
extern void x11drv_client_snapshot_release( struct x11drv_client_snapshot *snapshot );
struct x11drv_client_snapshot_image_ops
{
    BOOL (*ready)( void *image );
    void (*destroy)( void *image );
};
extern BOOL x11drv_client_snapshot_prepare_storage( struct x11drv_client_snapshot **storage,
                                                   unsigned int width, unsigned int height, unsigned int depth );
extern void *x11drv_client_snapshot_get_image( struct x11drv_client_snapshot *snapshot );
extern void x11drv_client_snapshot_set_image( struct x11drv_client_snapshot *snapshot, void *image,
                                              const struct x11drv_client_snapshot_image_ops *ops );
/* Called after handoff retirement admission. Preparation reserves storage
 * charges without native I/O; read only uses the transferred private object. */
extern BOOL x11drv_client_snapshot_prepare_native( struct x11drv_client_snapshot **storage, Window window,
                                                  unsigned int width, unsigned int height,
                                                  unsigned int depth, UINT64 epoch );
extern BOOL x11drv_client_snapshot_read_native( void *context );
extern BOOL x11drv_client_snapshot_prepare_read( struct x11drv_client_snapshot **storage );
extern BOOL x11drv_client_surface_snapshot( struct client_surface *client, struct client_surface_frame *present,
                                            const BYTE *pixels,
                                            unsigned int width, unsigned int height,
                                            BOOL top_down, const struct x11drv_snapshot_format *format );
extern void x11drv_client_surface_release_snapshot_staging( struct x11drv_client_surface *surface );
extern void x11drv_client_surface_trace_image( const char *event, const char *kind,
                                              Display *display, Pixmap pixmap, UINT64 bytes );
extern void x11drv_client_surface_set_gpu_snapshot( struct x11drv_client_surface *surface,
                                                   struct x11drv_client_snapshot *snapshot );
extern BOOL x11drv_client_surface_prepare_retirement( struct x11drv_client_surface *surface );
extern void x11drv_client_surface_retire_handoff( struct client_surface *client,
                                                 const struct client_surface_handoff_lease *lease );
extern void x11drv_client_surface_destroy_retirement( struct x11drv_client_surface *surface );
extern void x11drv_client_surface_release_source_frame( struct x11drv_client_source_frame *frame );

/* Embedded in an already charged resource. Reclamation waits for the owned
 * thread to exit before closing its handle and invoking metadata-only release.
 * Enqueue neither allocates nor returns the resource's memory charge. */
struct x11drv_client_surface_retired_resource
{
    struct list entry;
    HANDLE thread;
    void (*release)( struct x11drv_client_surface_retired_resource *resource );
};
extern BOOL x11drv_client_surface_prepare_resource_retirement(void);
extern void x11drv_client_surface_retire_resource( struct x11drv_client_surface_retired_resource *resource );
/* The prepared frame owns the capture, including when preparation fails.
 * Native allocation/upload runs outside the surface's present lock. */
extern struct x11drv_client_snapshot *x11drv_client_surface_prepare_gpu_snapshot(
    struct client_surface *client, struct client_surface_frame *present );

#endif /* __WINE_X11DRV_CLIENT_SURFACE_H */
