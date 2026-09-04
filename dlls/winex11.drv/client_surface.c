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

#include "client_surface.h"
#include "xcomposite.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);

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

static void x11drv_client_surface_destroy( struct client_surface *client )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    HWND hwnd = client->hwnd;

    TRACE( "%s\n", debugstr_client_surface( client ) );

    x11drv_client_surface_completion_destroy( surface );
    if (surface->colormap != default_colormap) XFreeColormap( gdi_display, surface->colormap );
    if (surface->window) destroy_client_window( hwnd, surface->window );
    if (surface->hdc_dst) NtGdiDeleteObjectApp( surface->hdc_dst );
    if (surface->hdc_src) NtGdiDeleteObjectApp( surface->hdc_src );
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

static void client_surface_update_geometry( HWND hwnd, struct x11drv_client_surface *surface,
                                            const struct client_surface_target *target )
{
    RECT rect = surface->client.raw ? target->monitor_rect : target->virtual_rect;
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

static BOOL client_surface_update_offscreen( HWND hwnd, struct x11drv_client_surface *surface,
                                             struct client_surface_target *target )
{
    BOOL offscreen, old_offscreen;
    struct x11drv_win_data *data;

    /* A hidden window needs the mapped dummy parent while it renders.  If it
     * actually presents there, X11DRV_client_surface_present() makes this
     * choice permanent so showing it cannot discard that completed frame. */
    offscreen = !NtUserIsWindowVisible( hwnd ) || surface->keep_offscreen ||
                needs_offscreen_rendering( hwnd, surface->client.raw );

    old_offscreen = surface->client.target.offscreen;
    target->offscreen = offscreen;
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
        RECT rect = target->virtual_rect;
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
    return TRUE;
}

static BOOL x11drv_client_surface_update( struct client_surface *client,
                                          struct client_surface_target *target )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    HWND hwnd = client->hwnd;

    client_surface_update_geometry( hwnd, surface, target );
    return client_surface_update_offscreen( hwnd, surface, target );
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
                                           HRGN surface_region, BOOL flush,
                                           BOOL defer_visible )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    HWND hwnd = client->hwnd, toplevel = client->target.toplevel;
    RECT rect_dst = client->target.monitor_rect, rect_src, rect_src_dc, rect;
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

    rect_src = surface->client.raw ? client->target.monitor_rect : client->target.virtual_rect;
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
    if (ret && (!defer_visible || !backing))
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
    .completion = &x11drv_client_surface_completion_ops,
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
    if (!x11drv_client_surface_completion_init( surface )) goto failed;
    rect = raw ? surface->client.target.monitor_rect : surface->client.target.virtual_rect;
    if (!(surface->window = create_client_window( hwnd, rect, &visual, colormap ))) goto failed;
    TRACE( "Created %s for client window %lx\n", debugstr_client_surface( &surface->client ), surface->window );
    return &surface->client;

failed:
    if (surface) client_surface_release( &surface->client );
    else if (colormap != default_colormap) XFreeColormap( gdi_display, colormap );
    return NULL;
}
