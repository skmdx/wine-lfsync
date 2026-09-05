/*
 * X11 client surface server-side clip regions
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

#include <dlfcn.h>
#include <limits.h>

#include "x11drv.h"

#if defined(HAVE_X11_EXTENSIONS_XFIXES_H) && defined(SONAME_LIBXFIXES)

#include <X11/extensions/Xfixes.h>

static typeof(XFixesQueryExtension) *pXFixesQueryExtension;
static typeof(XFixesQueryVersion) *pXFixesQueryVersion;
static typeof(XFixesCreateRegion) *pXFixesCreateRegion;
static typeof(XFixesSetRegion) *pXFixesSetRegion;
static typeof(XFixesDestroyRegion) *pXFixesDestroyRegion;
static typeof(XFixesSetGCClipRegion) *pXFixesSetGCClipRegion;
static typeof(XFixesSetPictureClipRegion) *pXFixesSetPictureClipRegion;
static pthread_once_t client_surface_xfixes_once = PTHREAD_ONCE_INIT;
static BOOL client_surface_xfixes_available;

static int client_surface_xfixes_error( Display *display, XErrorEvent *event, void *arg )
{
    int *error = arg;

    *error = event->error_code;
    return 1;
}

static void client_surface_xfixes_init(void)
{
    int event_base, error_base, major = 2, minor = 0;
    void *handle;

    if (!(handle = dlopen( SONAME_LIBXFIXES, RTLD_NOW ))) return;
#define LOAD_FUNCPTR(f) if (!(p##f = dlsym( handle, #f ))) goto failed
    LOAD_FUNCPTR( XFixesQueryExtension );
    LOAD_FUNCPTR( XFixesQueryVersion );
    LOAD_FUNCPTR( XFixesCreateRegion );
    LOAD_FUNCPTR( XFixesSetRegion );
    LOAD_FUNCPTR( XFixesDestroyRegion );
    LOAD_FUNCPTR( XFixesSetGCClipRegion );
    LOAD_FUNCPTR( XFixesSetPictureClipRegion );
#undef LOAD_FUNCPTR
    if (!pXFixesQueryExtension( gdi_display, &event_base, &error_base ) ||
        !pXFixesQueryVersion( gdi_display, &major, &minor ) || major < 2)
        goto failed;
    client_surface_xfixes_available = TRUE;
    return;

failed:
    dlclose( handle );
}

BOOL X11DRV_XFixes_ClientSurfaceAvailable(void)
{
    pthread_once( &client_surface_xfixes_once, client_surface_xfixes_init );
    return client_surface_xfixes_available;
}

BOOL X11DRV_XFixes_UpdateClientSurfaceRegion( Display *display, XID *region,
                                              const XRectangle *rects,
                                              unsigned int count )
{
    int error = 0;
    XID old_region;

    if (!region || !rects || !count || count > INT_MAX ||
        !X11DRV_XFixes_ClientSurfaceAvailable())
        return FALSE;
    old_region = *region;
    X11DRV_expect_error( display, client_surface_xfixes_error, &error );
    if (old_region)
        pXFixesSetRegion( display, old_region, rects, count );
    else
        *region = pXFixesCreateRegion( display, rects, count );
    XSync( display, False );
    X11DRV_check_error();
    if (error && !old_region) *region = 0;
    return !error && *region;
}

void X11DRV_XFixes_DestroyClientSurfaceRegion( Display *display, XID region )
{
    if (!region || !X11DRV_XFixes_ClientSurfaceAvailable()) return;
    pXFixesDestroyRegion( display, region );
    XFlush( display );
}

BOOL X11DRV_XFixes_SetClientSurfaceGCClip( Display *display, GC gc,
                                           int x, int y, XID region )
{
    if (!gc || !region || !X11DRV_XFixes_ClientSurfaceAvailable()) return FALSE;
    pXFixesSetGCClipRegion( display, gc, x, y, region );
    return TRUE;
}

BOOL X11DRV_XFixes_SetClientSurfacePictureClip( Display *display, XID picture,
                                                int x, int y, XID region )
{
    if (!picture || !region || !X11DRV_XFixes_ClientSurfaceAvailable()) return FALSE;
    pXFixesSetPictureClipRegion( display, picture, x, y, region );
    return TRUE;
}

#else

BOOL X11DRV_XFixes_ClientSurfaceAvailable(void)
{
    return FALSE;
}

BOOL X11DRV_XFixes_UpdateClientSurfaceRegion( Display *display, XID *region,
                                              const XRectangle *rects,
                                              unsigned int count )
{
    return FALSE;
}

void X11DRV_XFixes_DestroyClientSurfaceRegion( Display *display, XID region )
{
}

BOOL X11DRV_XFixes_SetClientSurfaceGCClip( Display *display, GC gc,
                                           int x, int y, XID region )
{
    return FALSE;
}

BOOL X11DRV_XFixes_SetClientSurfacePictureClip( Display *display, XID picture,
                                                int x, int y, XID region )
{
    return FALSE;
}

#endif
