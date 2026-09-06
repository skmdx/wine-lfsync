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

static BOOL client_window_region_is_full( HWND hwnd )
{
    RECT rect = {0}, client;
    UINT ret;
    HRGN region;
    HDC hdc;

    if (NtUserGetPresentRect( hwnd, &client, 0 )) return TRUE;
    if (!NtUserGetClientRect( hwnd, &client, NtUserGetDpiForWindow( hwnd ) )) return FALSE;
    OffsetRect( &client, -client.left, -client.top );
    NtUserMapWindowPoints( hwnd, 0, (POINT *)&client, 2, 0 /* per-monitor DPI */ );

    if (!(hdc = NtUserGetDCEx( hwnd, 0, DCX_CACHE | DCX_USESTYLE ))) return FALSE;
    if (!(region = NtGdiCreateRectRgn( 0, 0, 0, 0 )))
    {
        NtUserReleaseDC( hwnd, hdc );
        return FALSE;
    }
    ret = NtGdiGetRandomRgn( hdc, region, SYSRGN );
    if (ret > 0) ret = NtGdiGetRgnBox( region, &rect );
    NtGdiDeleteObjectApp( region );
    NtUserReleaseDC( hwnd, hdc );

    TRACE( "hwnd %p client %s SYSRGN %s type %u full %u\n", hwnd,
           wine_dbgstr_rect( &client ), wine_dbgstr_rect( &rect ), ret,
           ret == SIMPLEREGION && EqualRect( &rect, &client ) );
    return ret == SIMPLEREGION && EqualRect( &rect, &client );
}

static BOOL needs_client_window_clipping( HWND hwnd )
{
    return !client_window_region_is_full( hwnd );
}

static BOOL needs_composited_rendering( HWND hwnd, BOOL raw )
{
    if (!raw && NtUserGetDpiForWindow( hwnd ) != NtUserGetWinMonitorDpi( hwnd, MDT_RAW_DPI )) return TRUE; /* needs DPI scaling */
    if (NtUserGetAncestor( hwnd, GA_PARENT ) != NtUserGetDesktopWindow()) return TRUE; /* child window, needs compositing */
    if (NtUserGetWindowRelative( hwnd, GW_CHILD )) return needs_client_window_clipping( hwnd ); /* window has children, needs compositing */
    return FALSE;
}

static BOOL x11drv_client_surface_direct_ready( struct client_surface *client )
{
    return !needs_composited_rendering( client->hwnd, client->raw );
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
    X11DRV_XFixes_DestroyClientSurfaceRegion( gdi_display, surface->handoff_clip_region );
    if (surface->handoff_clip_mask) XFreePixmap( gdi_display, surface->handoff_clip_mask );
    if (surface->snapshot) XFreePixmap( gdi_display, surface->snapshot );
    if (surface->colormap != default_colormap) XFreeColormap( gdi_display, surface->colormap );
    if (surface->window) destroy_client_window( hwnd, surface->window );
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
    BOOL offscreen, old_offscreen = surface->client.target.offscreen;
    struct x11drv_win_data *data;

    if (target->mode != CLIENT_SURFACE_PRESENTATION_DIRECT)
    {
        if (target->mode == CLIENT_SURFACE_PRESENTATION_STAGED || !NtUserIsWindowVisible( hwnd ))
            target->mode = CLIENT_SURFACE_PRESENTATION_STAGED;
        else
            target->mode = CLIENT_SURFACE_PRESENTATION_COMPOSITED;
    }
    offscreen = target->offscreen = target->mode != CLIENT_SURFACE_PRESENTATION_DIRECT;
    if (offscreen == old_offscreen && offscreen) return TRUE;

#ifdef SONAME_LIBXCOMPOSITE
    if (usexcomposite && offscreen != old_offscreen)
    {
        if (offscreen)
        {
            int error = 0;

            X11DRV_expect_error( gdi_display, client_surface_redirect_error, &error );
            pXCompositeRedirectWindow( gdi_display, surface->window, CompositeRedirectManual );
            XSync( gdi_display, False );
            X11DRV_check_error();
            /* BadAccess means the window manager already owns redirection. */
            if (!error) surface->manual_redirect = TRUE;
            else if (error != BadAccess)
            {
                WARN( "failed to redirect client window %lx, X error %d\n", surface->window, error );
                return FALSE;
            }
        }
        else if (surface->manual_redirect)
        {
            pXCompositeUnredirectWindow( gdi_display, surface->window, CompositeRedirectManual );
            surface->manual_redirect = FALSE;
        }
    }
#endif
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

static int client_surface_clip_error( Display *display, XErrorEvent *event, void *arg )
{
    *(int *)arg = event->error_code;
    return TRUE;
}

static BOOL client_surface_create_clip_mask( struct x11drv_client_surface *surface,
                                             const XRectangle *rects, unsigned int count,
                                             unsigned int width, unsigned int height )
{
    Pixmap mask;
    XGCValues values = {.foreground = 0, .graphics_exposures = False};
    GC gc;
    int error = 0;

    /* The producer owns this mask until the same slot is released. Allocate a
     * new mask for a new scene; never mutate a mask held by an owner GC. */
    X11DRV_expect_error( gdi_display, client_surface_clip_error, &error );
    mask = XCreatePixmap( gdi_display, root_window, width, height, 1 );
    gc = XCreateGC( gdi_display, mask, GCForeground | GCGraphicsExposures, &values );
    if (gc)
    {
        XFillRectangle( gdi_display, mask, gc, 0, 0, width, height );
        XSetForeground( gdi_display, gc, 1 );
        XFillRectangles( gdi_display, mask, gc, (XRectangle *)rects, count );
        XFreeGC( gdi_display, gc );
    }
    XSync( gdi_display, False );
    X11DRV_check_error();
    if (error || !gc || !mask)
    {
        X11DRV_expect_error( gdi_display, client_surface_clip_error, &error );
        if (mask) XFreePixmap( gdi_display, mask );
        XSync( gdi_display, False );
        X11DRV_check_error();
        return FALSE;
    }
    if (surface->handoff_clip_mask) XFreePixmap( gdi_display, surface->handoff_clip_mask );
    surface->handoff_clip_mask = mask;
    return TRUE;
}

static BOOL x11drv_client_surface_prepare_handoff_clip(
    struct x11drv_client_surface *surface,
    struct client_surface_handoff_slot *slot, HRGN surface_region )
{
    struct client_surface *client = &surface->client;
    RECT rect;
    RGNDATA *clip = NULL;
    HRGN region = 0;
    HDC hdc = 0;
    BOOL supported = FALSE, required = FALSE, xfixes = FALSE, pixmap = FALSE;
    unsigned int count = 0, i;

    if (surface->handoff_clip_valid &&
        surface->handoff_clip_scene_epoch == slot->scene_epoch &&
        surface->handoff_clip_target_seq == slot->target_seq)
        goto publish;

    /* Build the exact region used by the legacy native-copy path once per
     * scene. The rectangles are already in client-surface coordinates; the
     * owner applies destination as the X clip origin. */
    if (client->hwnd != client->target.toplevel ||
        !NtUserGetPresentRect( client->target.toplevel, &rect, -1 /* raw dpi */ ))
    {
        DWORD flags = DCX_CACHE | DCX_USESTYLE | DCX_NORESETATTRS |
                      WINE_DCX_CLIENT_SURFACE;

        if (!(hdc = NtUserGetDCEx( client->hwnd, 0, flags ))) goto done;
        region = get_dc_monitor_region( client->hwnd, hdc );
        NtUserReleaseDC( client->hwnd, hdc );
        hdc = 0;
        if (!region) goto done;
    }
    if (surface_region)
    {
        int ret;

        if (region) ret = NtGdiCombineRgn( region, region, surface_region, RGN_AND );
        else if (!(region = NtGdiCreateRectRgn( 0, 0, 0, 0 ))) goto done;
        else
        {
            ret = NtGdiCombineRgn( region, surface_region, 0, RGN_COPY );
        }
        if (ret == ERROR) goto done;
    }
    if (!region)
    {
        supported = TRUE;
        goto done;
    }
    if (!(clip = X11DRV_GetRegionData( region, 0 ))) goto done;
    count = clip->rdh.nCount;
    if (count == 1)
    {
        const XRectangle *full = (const XRectangle *)clip->Buffer;
        unsigned int width = slot->destination.right - slot->destination.left;
        unsigned int height = slot->destination.bottom - slot->destination.top;

        if (!full->x && !full->y && full->width == width && full->height == height)
        {
            supported = TRUE;
            count = 0;
            goto done;
        }
    }
    if (count > CLIENT_SURFACE_HANDOFF_MAX_CLIP_RECTS)
    {
        xfixes = X11DRV_XFixes_UpdateClientSurfaceRegion(
                gdi_display, &surface->handoff_clip_region,
                (const XRectangle *)clip->Buffer, count );
        if (!xfixes && !client_surface_create_clip_mask(
                surface, (const XRectangle *)clip->Buffer, count,
                slot->destination.right - slot->destination.left,
                slot->destination.bottom - slot->destination.top ))
            goto done;
        count = 0;
        pixmap = !xfixes;
        required = TRUE;
        supported = TRUE;
        goto done;
    }
    for (i = 0; i < count; ++i)
    {
        const XRectangle *rect = (const XRectangle *)clip->Buffer + i;
        struct client_surface_handoff_clip_rect *out = &surface->handoff_clip_rects[i];

        out->x = rect->x;
        out->y = rect->y;
        out->width = rect->width;
        out->height = rect->height;
    }
    required = TRUE;
    supported = TRUE;

done:
    if (hdc) NtUserReleaseDC( client->hwnd, hdc );
    free( clip );
    if (region) NtGdiDeleteObjectApp( region );

    surface->handoff_clip_scene_epoch = slot->scene_epoch;
    surface->handoff_clip_target_seq = slot->target_seq;
    surface->handoff_clip_count = count;
    surface->handoff_clip_supported = supported;
    surface->handoff_clip_required = required;
    surface->handoff_clip_xfixes = xfixes;
    surface->handoff_clip_pixmap = pixmap;
    surface->handoff_clip_valid = TRUE;
    TRACE( "handoff clip hwnd %p source %ux%u visual %#lx destination %s region %p "
           "supported %u required %u xfixes %u count %u\n", client->hwnd, slot->width, slot->height,
           slot->source_visual, wine_dbgstr_rect( &client->target.monitor_rect ),
           surface_region, supported, required, xfixes, count );

publish:
    if (!surface->handoff_clip_supported) return FALSE;
    slot->clip_count = surface->handoff_clip_count;
    slot->clip_region = 0;
    if (surface->handoff_clip_required)
    {
        slot->flags |= CLIENT_SURFACE_HANDOFF_CLIPPED;
        if (surface->handoff_clip_xfixes)
        {
            slot->flags |= CLIENT_SURFACE_HANDOFF_XFIXES_CLIP;
            slot->clip_region = surface->handoff_clip_region;
        }
        else if (surface->handoff_clip_pixmap)
        {
            slot->flags |= CLIENT_SURFACE_HANDOFF_PIXMAP_CLIP;
            slot->clip_region = surface->handoff_clip_mask;
        }
        else
            memcpy( slot->clips, surface->handoff_clip_rects,
                    surface->handoff_clip_count * sizeof(*slot->clips) );
    }
    return TRUE;
}

static unsigned long snapshot_component( BYTE value, unsigned long mask )
{
    unsigned int shift = 0;

    if (!mask) return 0;
    while (!(mask & (1ul << shift))) ++shift;
    return ((UINT64)value * (mask >> shift) / 255) << shift;
}

BOOL x11drv_client_surface_snapshot( struct client_surface *client, const BYTE *pixels,
                                     unsigned int width, unsigned int height )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    XImage *image;
    GC gc;
    unsigned int x, y;
    unsigned long alpha = ((1ull << default_visual.depth) - 1) &
                          ~(default_visual.red_mask | default_visual.green_mask | default_visual.blue_mask);
    int error = 0;

    if (!(image = XCreateImage( gdi_display, default_visual.visual, default_visual.depth,
                                ZPixmap, 0, NULL, width, height, 32, 0 )))
        return FALSE;
    if (image->bytes_per_line <= 0 || height > ~(SIZE_T)0 / image->bytes_per_line ||
        !(image->data = calloc( height, image->bytes_per_line )))
    {
        XDestroyImage( image );
        return FALSE;
    }
    for (y = 0; y < height; ++y)
        for (x = 0; x < width; ++x, pixels += 4)
            XPutPixel( image, x, height - y - 1,
                       snapshot_component( pixels[0], default_visual.red_mask ) |
                       snapshot_component( pixels[1], default_visual.green_mask ) |
                       snapshot_component( pixels[2], default_visual.blue_mask ) |
                       snapshot_component( pixels[3], alpha ) );

    X11DRV_expect_error( gdi_display, client_surface_clip_error, &error );
    if (!surface->snapshot || surface->snapshot_size.cx != width || surface->snapshot_size.cy != height)
    {
        if (surface->snapshot) XFreePixmap( gdi_display, surface->snapshot );
        surface->snapshot = XCreatePixmap( gdi_display, root_window, width, height, default_visual.depth );
        surface->snapshot_size = (SIZE){width, height};
    }
    gc = XCreateGC( gdi_display, surface->snapshot, 0, NULL );
    if (gc)
    {
        XPutImage( gdi_display, surface->snapshot, gc, image, 0, 0, 0, 0, width, height );
        XFreeGC( gdi_display, gc );
    }
    XSync( gdi_display, False );
    X11DRV_check_error();
    XDestroyImage( image );
    TRACE( "uploaded producer snapshot %#lx from visual %#lx to %#lx\n",
           surface->snapshot, surface->source_visual, default_visual.visualid );
    return gc && !error;
}

static BOOL x11drv_client_surface_handoff_prepare(
    struct client_surface *client, struct client_surface_handoff_slot *slot,
    HRGN surface_region )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    RECT source = client->raw ? client->target.monitor_rect : client->target.virtual_rect;
    RECT destination = client->target.monitor_rect;
    unsigned int width, height;

    if (source.right <= source.left || source.bottom <= source.top ||
        destination.right <= destination.left || destination.bottom <= destination.top)
        return FALSE;
    width = source.right - source.left;
    height = source.bottom - source.top;
    slot->source = usexcomposite ? surface->window : surface->snapshot;
    slot->source_visual = usexcomposite ? surface->source_visual : default_visual.visualid;
    slot->flags = CLIENT_SURFACE_HANDOFF_NATIVE_X11 | CLIENT_SURFACE_HANDOFF_FULL_DAMAGE;
    slot->destination = destination;
    slot->width = width;
    slot->height = height;
    SetRect( &slot->damage, 0, 0, width, height );
    if (!usexcomposite) slot->flags |= CLIENT_SURFACE_HANDOFF_COPY_SOURCE;
    return x11drv_client_surface_prepare_handoff_clip( surface, slot, surface_region );
}

static const struct client_surface_backend x11drv_client_surface_backend =
{
    .caps = CLIENT_SURFACE_BACKEND_SCENE_PUBLICATION |
            CLIENT_SURFACE_BACKEND_READ_ONLY_DC |
            CLIENT_SURFACE_BACKEND_DIRECT_PRESENTATION |
            CLIENT_SURFACE_BACKEND_GENERATION_HANDOFF |
            CLIENT_SURFACE_BACKEND_OWNER_COMPOSITOR,
    .destroy = x11drv_client_surface_destroy,
    .detach = x11drv_client_surface_detach,
    .direct_ready = x11drv_client_surface_direct_ready,
    .update = x11drv_client_surface_update,
    .handoff_prepare = x11drv_client_surface_handoff_prepare,
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
    const struct client_surface_backend *backend = &x11drv_client_surface_backend;
    XVisualInfo visual = default_visual;
    Colormap colormap;
    RECT rect;

    if (format && !visual_from_pixel_format( format, &visual )) return NULL;

    /* All conversion and clipping, including extension fallbacks, is done on
     * the owner connection. The producer never writes the owner's backing. */
    if (visual.visualid == default_visual.visualid) colormap = default_colormap;
    else colormap = XCreateColormap( gdi_display, get_dummy_parent(), visual.visual, visual_class_alloc( visual.class ) );
    if (!colormap) return NULL;

    if (!(surface = client_surface_create( sizeof(*surface), backend, hwnd, format, raw ))) goto failed;
    surface->colormap = colormap;
    surface->source_visual = visual.visualid;
    if (!x11drv_client_surface_completion_init( surface )) goto failed;
    rect = raw ? surface->client.target.monitor_rect : surface->client.target.virtual_rect;
    if (!(surface->window = create_client_window( hwnd, rect, &visual, colormap ))) goto failed;
    TRACE( "Created %s for client window %lx, owner compositor %u\n",
           debugstr_client_surface( &surface->client ), surface->window,
           backend == &x11drv_client_surface_backend );
    return &surface->client;

failed:
    if (surface) client_surface_release( &surface->client );
    else if (colormap != default_colormap) XFreeColormap( gdi_display, colormap );
    return NULL;
}
