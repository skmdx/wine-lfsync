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
    BOOL waiting;
    BOOL cond_initialized;
};

struct x11drv_client_surface
{
    struct client_surface client;
    XWindowChanges changes;
    Colormap colormap;
    Window window;
    Pixmap composition_backing;
    UINT64 composition_scene_generation;
    HWND composition_toplevel;
    struct x11drv_client_surface_completion completion;
    BOOL keep_offscreen;    /* preserve a drawable which was used while hidden */
    BOOL manual_redirect;   /* client drawable is manually XComposite redirected */

    HDC hdc_src;
    HDC hdc_dst;
};

extern struct x11drv_client_surface *impl_from_client_surface( struct client_surface *client );
extern const struct client_surface_completion_ops x11drv_client_surface_completion_ops;
extern BOOL x11drv_client_surface_completion_init( struct x11drv_client_surface *surface );
extern void x11drv_client_surface_completion_destroy( struct x11drv_client_surface *surface );

#endif /* __WINE_X11DRV_CLIENT_SURFACE_H */
