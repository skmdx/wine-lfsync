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
    if (surface->composition_gc) XFreeGC( gdi_display, surface->composition_gc );
    if (surface->colormap != default_colormap) XFreeColormap( gdi_display, surface->colormap );
    if (surface->window) destroy_client_window( hwnd, surface->window );
    if (surface->hdc_backing) NtGdiDeleteObjectApp( surface->hdc_backing );
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

    /* Visibility and topology are server scene state.  In particular, the
     * Win32 window can temporarily look unmapped here while its owner thread
     * is applying the show request which made the server scene DIRECT.  Once
     * that scene is selected, attach the producer instead of permanently
     * downgrading it to STAGED.  Backend-local clipping constraints are
     * advertised separately before the server selects DIRECT. */
    if (target->mode != CLIENT_SURFACE_PRESENTATION_DIRECT)
    {
        if (target->mode == CLIENT_SURFACE_PRESENTATION_STAGED ||
            !NtUserIsWindowVisible( hwnd ))
            target->mode = CLIENT_SURFACE_PRESENTATION_STAGED;
        else
            target->mode = CLIENT_SURFACE_PRESENTATION_COMPOSITED;
    }
    offscreen = target->mode != CLIENT_SURFACE_PRESENTATION_DIRECT;

    old_offscreen = surface->client.target.offscreen;
    target->offscreen = offscreen;
    if (old_offscreen == offscreen)
    {
        if (!offscreen && (data = get_win_data( hwnd )))
        {
            attach_client_window( data, surface->window );
            release_win_data( data );
        }
        return !offscreen || (surface->hdc_src && surface->hdc_dst && surface->hdc_backing);
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
        if (surface->hdc_backing)
        {
            NtGdiDeleteObjectApp( surface->hdc_backing );
            surface->hdc_backing = NULL;
        }
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
        HDC hdc_dst, hdc_src, hdc_backing;

        OffsetRect( &rect, -rect.left, -rect.top );
        hdc_dst = NtGdiOpenDCW( &device_str, NULL, NULL, 0, TRUE, NULL, NULL, NULL );
        hdc_src = NtGdiOpenDCW( &device_str, NULL, NULL, 0, TRUE, NULL, NULL, NULL );
        hdc_backing = NtGdiOpenDCW( &device_str, NULL, NULL, 0, TRUE, NULL, NULL, NULL );

        if (!hdc_dst || !hdc_src || !hdc_backing)
        {
            if (hdc_backing) NtGdiDeleteObjectApp( hdc_backing );
            if (hdc_dst) NtGdiDeleteObjectApp( hdc_dst );
            if (hdc_src) NtGdiDeleteObjectApp( hdc_src );
            WARN( "failed to allocate offscreen composition DCs for %s\n",
                  debugstr_client_surface( &surface->client ) );
            return FALSE;
        }
        surface->hdc_dst = hdc_dst;
        surface->hdc_src = hdc_src;
        surface->hdc_backing = hdc_backing;
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
                NtGdiDeleteObjectApp( surface->hdc_backing );
                NtGdiDeleteObjectApp( surface->hdc_dst );
                NtGdiDeleteObjectApp( surface->hdc_src );
                surface->hdc_backing = surface->hdc_dst = surface->hdc_src = NULL;
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

static BOOL update_client_surface_composition_targets( struct x11drv_client_surface *surface,
                                                       const struct client_surface_scene *scene )
{
    HWND toplevel = surface->client.target.toplevel;

    /* A granted native writer lease keeps both the backing and its whole
     * window alive. Replacing either target seals the scene and changes its
     * epoch, so retain the pair together without taking the owner's lock or
     * fetching its window property on every steady-state presentation.
     * Without a backing, keep querying: that lifetime is not covered by the
     * owner's backing barrier. */
    if (surface->composition_backing && surface->composition_window &&
        surface->composition_toplevel == scene->toplevel &&
        surface->composition_scene_epoch == scene->epoch)
        return TRUE;

    surface->composition_window = X11DRV_get_whole_window_property( toplevel );
    surface->composition_visual_checked = FALSE;
    surface->composition_backing = 0;
    if (!surface->composition_window) return FALSE;
    surface->composition_backing = X11DRV_get_client_surface_backing_property( toplevel );
    surface->composition_toplevel = scene->toplevel;
    surface->composition_scene_epoch = scene->epoch;
    return TRUE;
}

static BOOL copy_client_surface( struct x11drv_client_surface *surface, HDC hdc_dst, Drawable target,
                                 const RECT *rect_dst, const RECT *rect_src, HRGN region )
{
    RECT rect;

    if (get_dc_drawable( hdc_dst, &rect ) != target || !EqualRect( &rect, rect_dst ))
        set_dc_drawable( hdc_dst, target, rect_dst, IncludeInferiors );
    /* RGN_COPY with a null region clears a clip left by an earlier present. */
    NtGdiExtSelectClipRgn( hdc_dst, region, RGN_COPY );

    if (rect_dst->right - rect_dst->left == rect_src->right - rect_src->left &&
        rect_dst->bottom - rect_dst->top == rect_src->bottom - rect_src->top)
        return NtGdiBitBlt( hdc_dst, 0, 0, rect_dst->right - rect_dst->left,
                            rect_dst->bottom - rect_dst->top, surface->hdc_src, 0, 0,
                            SRCCOPY, 0, 0 );
    return NtGdiStretchBlt( hdc_dst, 0, 0, rect_dst->right - rect_dst->left,
                            rect_dst->bottom - rect_dst->top, surface->hdc_src, 0, 0,
                            rect_src->right - rect_src->left, rect_src->bottom - rect_src->top,
                            SRCCOPY, 0 );
}

static BOOL X11DRV_client_surface_present( struct client_surface *client,
                                           const struct client_surface_scene *scene,
                                           HDC hdc, HRGN surface_region,
                                           BOOL flush, BOOL defer_visible )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    HWND hwnd = client->hwnd, toplevel = client->target.toplevel;
    RECT rect_dst = client->target.monitor_rect, rect_src, rect_src_dc, rect;
    Drawable window;
    Pixmap backing;
    BOOL ret;
    HRGN region;
    RGNDATA *clip = NULL;
    BOOL native_copy;

    if (!hdc)
    {
        if (flush) XFlush( gdi_display );
        return TRUE;
    }

    if (!surface->hdc_src || !surface->hdc_dst || !surface->hdc_backing) return FALSE;
    if (!update_client_surface_composition_targets( surface, scene )) return FALSE;
    window = surface->composition_window;
    backing = surface->composition_backing;

    /* Exclusive fullscreen ignores normal window clipping. */
    if (hwnd == toplevel && NtUserGetPresentRect( toplevel, &rect, -1 /* raw dpi */ )) region = 0;
    else if (!(region = get_dc_monitor_region( hwnd, hdc ))) return FALSE;

    if (surface_region)
    {
        int ret;

        if (region) ret = NtGdiCombineRgn( region, region, surface_region, RGN_AND );
        else
        {
            if (!(region = NtGdiCreateRectRgn( 0, 0, 0, 0 ))) return FALSE;
            ret = NtGdiCombineRgn( region, surface_region, 0, RGN_COPY );
        }
        if (ret == ERROR)
        {
            NtGdiDeleteObjectApp( region );
            return FALSE;
        }
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
    native_copy = surface->source_visual == default_visual.visualid &&
                  rect_dst.right - rect_dst.left == rect_src_dc.right &&
                  rect_dst.bottom - rect_dst.top == rect_src_dc.bottom;
    if (native_copy && region && !(clip = X11DRV_GetRegionData( region, 0 )))
    {
        NtGdiDeleteObjectApp( region );
        return FALSE;
    }

    /* Scene generations are never written into the visible host piecemeal.
     * Steady frames keep the same backing current, then update the visible
     * region; a live or staged generation becomes visible only in the owner
     * process after all renderer commits have reached the server. */
    /* Keep separate destination DCs: rebinding one DC between these drawables
     * recreates its GC and XRender picture for both copies on every frame. */
    /* The composition operation is a native source-to-target copy, not an
     * application DC operation. Keep its GC on the surface and apply the
     * already resolved clip directly. This also keeps GDI dispatch out of
     * the native write interval for the unscaled, same-visual case. Check
     * the target only after acquiring the native lease, and recheck whenever
     * its scene changes. Scaling / format conversion still uses GDI. */
    if (native_copy && !surface->composition_visual_checked)
    {
        XWindowAttributes attrs;
        surface->composition_same_visual = XGetWindowAttributes( gdi_display, window, &attrs ) &&
                                           XVisualIDFromVisual( attrs.visual ) == surface->source_visual;
        surface->composition_visual_checked = TRUE;
    }
    native_copy = native_copy && surface->composition_same_visual;
    if (native_copy && !surface->composition_gc)
    {
        XGCValues values = {.function = GXcopy, .graphics_exposures = False,
                           .subwindow_mode = IncludeInferiors};
        surface->composition_gc = XCreateGC( gdi_display, root_window,
                                             GCFunction | GCGraphicsExposures | GCSubwindowMode, &values );
        native_copy = !!surface->composition_gc;
    }
    if (native_copy)
    {
        if (clip)
            XSetClipRectangles( gdi_display, surface->composition_gc, rect_dst.left, rect_dst.top,
                                (XRectangle *)clip->Buffer, clip->rdh.nCount, YXBanded );
        else XSetClipMask( gdi_display, surface->composition_gc, None );
        if (backing)
            XCopyArea( gdi_display, surface->window, backing, surface->composition_gc,
                       0, 0, rect_src_dc.right, rect_src_dc.bottom, rect_dst.left, rect_dst.top );
        if (!defer_visible || !backing)
            XCopyArea( gdi_display, surface->window, window, surface->composition_gc,
                       0, 0, rect_src_dc.right, rect_src_dc.bottom, rect_dst.left, rect_dst.top );
        ret = TRUE;
    }
    else
    {
        if (get_dc_drawable( surface->hdc_src, &rect ) != surface->window ||
            !EqualRect( &rect, &rect_src_dc ))
            set_dc_drawable( surface->hdc_src, surface->window, &rect_src_dc, IncludeInferiors );
        ret = backing ? copy_client_surface( surface, surface->hdc_backing, backing,
                                         &rect_dst, &rect_src, region ) : TRUE;
        if (ret && (!defer_visible || !backing))
            ret = copy_client_surface( surface, surface->hdc_dst, window, &rect_dst, &rect_src, region );
    }
    /* A failed copy does not undo earlier native requests.  In particular,
     * the backing copy may have succeeded before the visible copy failed.
     * Complete this connection's writes before returning its writer lease,
     * even when the failed frame will not commit its scene generation. */
    if (flush) XSync( gdi_display, False );
    else XFlush( gdi_display );

    free( clip );
    if (region) NtGdiDeleteObjectApp( region );
    return ret;
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
    BOOL supported = FALSE, required = FALSE, xfixes = FALSE;
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
        if (!X11DRV_XFixes_UpdateClientSurfaceRegion(
                gdi_display, &surface->handoff_clip_region,
                (const XRectangle *)clip->Buffer, count ))
            goto done;
        count = 0;
        xfixes = TRUE;
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
        else
            memcpy( slot->clips, surface->handoff_clip_rects,
                    surface->handoff_clip_count * sizeof(*slot->clips) );
    }
    return TRUE;
}

static BOOL x11drv_client_surface_handoff_prepare(
    struct client_surface *client, struct client_surface_handoff_slot *slot,
    HRGN surface_region )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    RECT source = client->raw ? client->target.monitor_rect : client->target.virtual_rect;
    RECT destination = client->target.monitor_rect;
    unsigned int width, height, destination_width, destination_height;

    if (source.right <= source.left || source.bottom <= source.top ||
        destination.right <= destination.left || destination.bottom <= destination.top)
        return FALSE;
    width = source.right - source.left;
    height = source.bottom - source.top;
    destination_width = destination.right - destination.left;
    destination_height = destination.bottom - destination.top;
    if ((width != destination_width || height != destination_height ||
         surface->source_visual != default_visual.visualid) &&
        !X11DRV_XRender_ClientSurfaceAvailable(
            width != destination_width || height != destination_height ))
        return FALSE;
    slot->source = surface->window;
    slot->source_visual = surface->source_visual;
    slot->flags = CLIENT_SURFACE_HANDOFF_NATIVE_X11 | CLIENT_SURFACE_HANDOFF_FULL_DAMAGE;
    slot->destination = destination;
    slot->width = width;
    slot->height = height;
    SetRect( &slot->damage, 0, 0, width, height );
    if (!usexcomposite || !x11drv_client_surface_prepare_handoff_clip(
                              surface, slot, surface_region ))
        return FALSE;
    return TRUE;
}

static const struct client_surface_backend x11drv_client_surface_backend =
{
    .caps = CLIENT_SURFACE_BACKEND_SCENE_PUBLICATION |
            CLIENT_SURFACE_BACKEND_NATIVE_WRITE_LEASE |
            CLIENT_SURFACE_BACKEND_READ_ONLY_DC |
            CLIENT_SURFACE_BACKEND_DIRECT_PRESENTATION |
            CLIENT_SURFACE_BACKEND_GENERATION_HANDOFF,
    .destroy = x11drv_client_surface_destroy,
    .detach = x11drv_client_surface_detach,
    .direct_ready = x11drv_client_surface_direct_ready,
    .update = x11drv_client_surface_update,
    .present = X11DRV_client_surface_present,
    .handoff_prepare = x11drv_client_surface_handoff_prepare,
    .completion = &x11drv_client_surface_completion_ops,
};

static const struct client_surface_backend x11drv_client_surface_owner_backend =
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
    .present = X11DRV_client_surface_present,
    .handoff_prepare = x11drv_client_surface_handoff_prepare,
    .completion = &x11drv_client_surface_completion_ops,
};

static int visual_class_alloc( int class )
{
    return class == PseudoColor || class == GrayScale || class == DirectColor ? AllocAll : AllocNone;
}

struct x11drv_client_surface *impl_from_client_surface( struct client_surface *client )
{
    assert( client->backend == &x11drv_client_surface_backend ||
            client->backend == &x11drv_client_surface_owner_backend );
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

    /* XComposite is the ownership primitive: it lets the owner connection
     * retain the producer source.  XRender is required only for conversion
     * or scaling, and core X11 handles ordinary same-visual copies.  XFixes
     * remains a backend-lifetime requirement because a later topology can
     * produce a clip too large for the fixed shared handoff slot; falling back
     * to producer writes would target a different backing than the owner's
     * frame pool. */
    if (usexcomposite && X11DRV_XFixes_ClientSurfaceAvailable())
        backend = &x11drv_client_surface_owner_backend;

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
           backend == &x11drv_client_surface_owner_backend );
    return &surface->client;

failed:
    if (surface) client_surface_release( &surface->client );
    else if (colormap != default_colormap) XFreeColormap( gdi_display, colormap );
    return NULL;
}
