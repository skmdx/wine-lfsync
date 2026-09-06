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

struct x11drv_client_surface
{
    struct client_surface client;
    XWindowChanges changes;
    Colormap colormap;
    Window window;
    Pixmap composition_backing;
    Window composition_window;
    UINT64 composition_scene_epoch;
    HWND composition_toplevel;
    GC composition_gc;
    VisualID source_visual;
    UINT64 handoff_clip_scene_epoch;
    UINT64 handoff_clip_target_seq;
    XID handoff_clip_region;
    Pixmap handoff_clip_mask;
    struct client_surface_handoff_clip_rect
        handoff_clip_rects[CLIENT_SURFACE_HANDOFF_MAX_CLIP_RECTS];
    unsigned int handoff_clip_count;
    BOOL composition_visual_checked;
    BOOL composition_same_visual;
    BOOL handoff_clip_valid;
    BOOL handoff_clip_supported;
    BOOL handoff_clip_required;
    BOOL handoff_clip_xfixes;
    BOOL handoff_clip_pixmap;
    struct x11drv_client_surface_completion completion;
    BOOL manual_redirect;   /* client drawable is manually XComposite redirected */

    HDC hdc_src;
    HDC hdc_dst;
    HDC hdc_backing;
};

extern struct x11drv_client_surface *impl_from_client_surface( struct client_surface *client );
extern const struct client_surface_completion_ops x11drv_client_surface_completion_ops;
extern BOOL x11drv_client_surface_completion_init( struct x11drv_client_surface *surface );
extern void x11drv_client_surface_completion_destroy( struct x11drv_client_surface *surface );

#endif /* __WINE_X11DRV_CLIENT_SURFACE_H */
