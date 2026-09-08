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
WINE_DECLARE_DEBUG_CHANNEL(csperf);

/* Image ownership receipts, not new quota or allocation state. The analyser
 * joins a native XID's acquire/retire/release lifetime, including XID reuse.
 * Release records the existing XFreePixmap boundary, not server/VRAM timing. */
void x11drv_client_surface_trace_image( const char *event, const char *kind,
                                       Display *display, Pixmap pixmap, UINT64 bytes )
{
    LARGE_INTEGER ticks;

    if (!TRACE_ON(csperf) || !kind || !pixmap || !bytes) return;
    NtQueryPerformanceCounter( &ticks, NULL );
    TRACE_(csperf)( "ticks=%llu event=image_%s kind=%s display=%p pixmap=%lx bytes=%llu\n",
                   (unsigned long long)ticks.QuadPart, event, kind, display, pixmap,
                   (unsigned long long)bytes );
}

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

void x11drv_client_surface_release_snapshot_staging( struct x11drv_client_surface *surface )
{
    UINT64 bytes = surface->snapshot_pixels_size;

    free( surface->snapshot_pixels );
    surface->snapshot_pixels = NULL;
    surface->snapshot_pixels_size = 0;
    if (surface->snapshot_image)
    {
        bytes += (UINT64)surface->snapshot_image->bytes_per_line * surface->snapshot_image->height;
        XDestroyImage( surface->snapshot_image );
        surface->snapshot_image = NULL;
    }
    client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING, bytes );
    if (bytes) TRACE( "released CPU snapshot staging for %s, bytes %s\n",
                      debugstr_client_surface( &surface->client ), wine_dbgstr_longlong( bytes ) );
}

static void x11drv_client_surface_destroy( struct client_surface *client )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    HWND hwnd = client->hwnd;
    unsigned int i;

    TRACE( "%s\n", debugstr_client_surface( client ) );

    x11drv_client_surface_completion_destroy( surface );
    x11drv_client_surface_destroy_retirement( surface );
    x11drv_client_surface_release_snapshot_staging( surface );
    if (surface->snapshot_import) XFreePixmap( gdi_display, surface->snapshot_import );
    for (i = 0; i < ARRAY_SIZE(surface->sources); ++i)
    {
        x11drv_client_surface_trace_image( "retire", "producer_slot", gdi_display,
                                          surface->sources[i].pixmap, surface->sources[i].bytes );
        if (surface->sources[i].image) surface->sources[i].release_image( surface->sources[i].image );
        if (surface->sources[i].gc) XFreeGC( gdi_display, surface->sources[i].gc );
        if (surface->sources[i].pixmap) XFreePixmap( gdi_display, surface->sources[i].pixmap );
        x11drv_client_surface_trace_image( "free", "producer_slot", gdi_display,
                                          surface->sources[i].pixmap, surface->sources[i].bytes );
        client_surface_release_memory( CLIENT_SURFACE_MEMORY_SOURCE, surface->sources[i].bytes );
    }
    if (surface->snapshot_gc) XFreeGC( gdi_display, surface->snapshot_gc );
    if (surface->snapshot) XFreePixmap( gdi_display, surface->snapshot );
    client_surface_release_memory( CLIENT_SURFACE_MEMORY_SOURCE, surface->snapshot_bytes );
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

static unsigned int client_surface_update_geometry( HWND hwnd, struct x11drv_client_surface *surface,
                                                    const struct client_surface_target *target )
{
    BOOL native = usexcomposite && !surface->direct_snapshot;
    /* A private offscreen window is also the generic GL resolve/gamma scratch
     * buffer. It must contain the whole virtual image before snapshot capture,
     * including when the native destination is smaller. DIRECT and named
     * native sources still use monitor pixels. This update precedes attach
     * and the actual Present, so a DIRECT transition restores native geometry
     * before any pixels or publication proof can be submitted. */
    RECT rect = surface->client.raw && (native || target->mode == CLIENT_SURFACE_PRESENTATION_DIRECT)
                ? target->monitor_rect : target->virtual_rect;
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
    if (!mask) return 0;

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
    return mask;
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
                                          struct client_surface_target *target,
                                          enum client_surface_target_update *update )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    HWND hwnd = client->hwnd;
    unsigned int mask;

    mask = client_surface_update_geometry( hwnd, surface, target );
    if (!client_surface_update_offscreen( hwnd, surface, target )) return FALSE;
    if (mask & (CWWidth | CWHeight)) *update = CLIENT_SURFACE_TARGET_UPDATE_CHANGED;
    /* update_offscreen returns without redirecting or attaching an existing
     * offscreen window. Moving that window preserves its image; resizing it
     * does not. The core also requires unchanged ownership, extent and DPI. */
    else if (client->target.offscreen && target->offscreen && client->target.mode == target->mode)
        *update = CLIENT_SURFACE_TARGET_UPDATE_PRESERVED;
    return TRUE;
}

static int client_surface_clip_error( Display *display, XErrorEvent *event, void *arg )
{
    *(int *)arg = event->error_code;
    return TRUE;
}

static void discard_client_surface_source( Pixmap *pixmap, GC *gc, UINT64 *bytes, const char *kind )
{
    int error = 0;

    /* Xlib may return handles before the server reports BadAlloc. Consume
     * cleanup errors locally and never cache a failed allocation for reuse. */
    x11drv_client_surface_trace_image( "retire", kind, gdi_display, *pixmap, *bytes );
    X11DRV_expect_error( gdi_display, client_surface_clip_error, &error );
    if (*gc) XFreeGC( gdi_display, *gc );
    if (*pixmap) XFreePixmap( gdi_display, *pixmap );
    XSync( gdi_display, False );
    X11DRV_check_error();
    x11drv_client_surface_trace_image( "free", kind, gdi_display, *pixmap, *bytes );
    *gc = NULL;
    *pixmap = 0;
    client_surface_release_memory( CLIENT_SURFACE_MEMORY_SOURCE, *bytes );
    *bytes = 0;
}

struct x11drv_client_source_frame *x11drv_client_surface_get_source(
    struct client_surface *client, unsigned int index, unsigned int width,
    unsigned int height, unsigned int depth )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    struct x11drv_client_source_frame *frame = &surface->sources[index];
    UINT64 bytes = (UINT64)width * height * (depth > 16 ? 4 : depth > 8 ? 2 : 1);
    struct x11drv_client_source_frame next = {.width = width, .height = height, .depth = depth, .bytes = bytes};
    BOOL preserve = frame->pixmap && frame->pixmap == surface->gpu_snapshot &&
                    frame->width >= width && frame->height >= height && frame->depth == depth;
    int error = 0;

    assert( index < ARRAY_SIZE(surface->sources) );
    /* A timed-out completion returns the control token, but does not make
     * storage that the GPU is still writing reusable. */
    if (frame->image && frame->image_ready && !frame->image_ready( frame->image )) return NULL;
    if (frame->pixmap && frame->width == width && frame->height == height && frame->depth == depth)
        return frame;
    if (!client_surface_reserve_memory( CLIENT_SURFACE_MEMORY_SOURCE, bytes )) return NULL;
    X11DRV_expect_error( gdi_display, client_surface_clip_error, &error );
    next.pixmap = XCreatePixmap( gdi_display, root_window, width, height, depth );
    if (preserve && (next.gc = XCreateGC( gdi_display, next.pixmap, 0, NULL )))
        XCopyArea( gdi_display, frame->pixmap, next.pixmap, next.gc, 0, 0, width, height, 0, 0 );
    XSync( gdi_display, False );
    X11DRV_check_error();
    if (error || !next.pixmap || (preserve && !next.gc))
    {
        discard_client_surface_source( &next.pixmap, &next.gc, &next.bytes, NULL );
        return NULL;
    }
    x11drv_client_surface_trace_image( "acquire", "producer_slot", gdi_display, next.pixmap, next.bytes );
    if (frame->pixmap == surface->gpu_snapshot)
    {
        x11drv_client_surface_set_gpu_snapshot( surface, preserve ? next.pixmap : 0 );
        surface->gpu_snapshot_size = (SIZE){width, height};
    }
    x11drv_client_surface_trace_image( "retire", "producer_slot", gdi_display, frame->pixmap, frame->bytes );
    if (frame->image) frame->release_image( frame->image );
    if (frame->gc) XFreeGC( gdi_display, frame->gc );
    if (frame->pixmap) XFreePixmap( gdi_display, frame->pixmap );
    x11drv_client_surface_trace_image( "free", "producer_slot", gdi_display, frame->pixmap, frame->bytes );
    client_surface_release_memory( CLIENT_SURFACE_MEMORY_SOURCE, frame->bytes );
    *frame = next;
    return frame;
}

static unsigned long snapshot_component( BYTE value, unsigned long mask )
{
    unsigned int shift = 0;

    if (!mask) return 0;
    while (!(mask & (1ul << shift))) ++shift;
    return ((UINT64)value * (mask >> shift) / 255) << shift;
}

BOOL x11drv_client_surface_snapshot( struct client_surface *client, const BYTE *pixels,
                                     unsigned int width, unsigned int height,
                                     BOOL top_down, BOOL bgra )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    XImage *image;
    GC gc;
    unsigned int x, y;
    unsigned long alpha = ((1ull << default_visual.depth) - 1) &
                          ~(default_visual.red_mask | default_visual.green_mask | default_visual.blue_mask);
    int error = 0;

    image = surface->snapshot_image;
    if (!image || image->width != width || image->height != height)
    {
        if (!(image = XCreateImage( gdi_display, default_visual.visual, default_visual.depth,
                                ZPixmap, 0, NULL, width, height, 32, 0 )))
            return FALSE;
        if (image->bytes_per_line <= 0 || height > ~(SIZE_T)0 / image->bytes_per_line ||
            !client_surface_reserve_memory( CLIENT_SURFACE_MEMORY_STAGING, (UINT64)height * image->bytes_per_line ))
        {
            XDestroyImage( image );
            return FALSE;
        }
        if (!(image->data = calloc( height, image->bytes_per_line )))
        {
            client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING, (UINT64)height * image->bytes_per_line );
            XDestroyImage( image );
            return FALSE;
        }
        if (surface->snapshot_image)
        {
            client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING,
                (UINT64)surface->snapshot_image->bytes_per_line * surface->snapshot_image->height );
            XDestroyImage( surface->snapshot_image );
        }
        surface->snapshot_image = image;
    }
    if (image->bits_per_pixel == 32 && image->byte_order == LSBFirst &&
        default_visual.red_mask == 0xff0000 && default_visual.green_mask == 0xff00 &&
        default_visual.blue_mask == 0xff)
    {
        for (y = 0; y < height; ++y)
        {
            BYTE *row = (BYTE *)image->data +
                        (SIZE_T)(top_down ? y : height - y - 1) * image->bytes_per_line;

            if (bgra)
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
            for (x = 0; x < width; ++x, pixels += 4)
                XPutPixel( image, x, top_down ? y : height - y - 1,
                           snapshot_component( pixels[bgra ? 2 : 0], default_visual.red_mask ) |
                           snapshot_component( pixels[1], default_visual.green_mask ) |
                           snapshot_component( pixels[bgra ? 0 : 2], default_visual.blue_mask ) |
                           snapshot_component( pixels[3], alpha ) );

    X11DRV_expect_error( gdi_display, client_surface_clip_error, &error );
    if (!surface->snapshot || surface->snapshot_size.cx != width || surface->snapshot_size.cy != height)
    {
        UINT64 bytes = (UINT64)width * height * (default_visual.depth > 16 ? 4 : default_visual.depth > 8 ? 2 : 1);

        if (!client_surface_reserve_memory( CLIENT_SURFACE_MEMORY_SOURCE, bytes ))
        {
            X11DRV_check_error();
            return FALSE;
        }
        if (surface->snapshot_gc) XFreeGC( gdi_display, surface->snapshot_gc );
        surface->snapshot_gc = NULL;
        if (surface->snapshot) XFreePixmap( gdi_display, surface->snapshot );
        client_surface_release_memory( CLIENT_SURFACE_MEMORY_SOURCE, surface->snapshot_bytes );
        surface->snapshot_bytes = bytes;
        surface->snapshot = XCreatePixmap( gdi_display, root_window, width, height, default_visual.depth );
        surface->snapshot_size = (SIZE){width, height};
    }
    if (!(gc = surface->snapshot_gc))
        gc = surface->snapshot_gc = XCreateGC( gdi_display, surface->snapshot, 0, NULL );
    if (gc)
    {
        XPutImage( gdi_display, surface->snapshot, gc, image, 0, 0, 0, 0, width, height );
    }
    XSync( gdi_display, False );
    X11DRV_check_error();
    if (!gc || error)
    {
        discard_client_surface_source( &surface->snapshot, &surface->snapshot_gc,
                                        &surface->snapshot_bytes, NULL );
        return FALSE;
    }
    TRACE( "uploaded producer snapshot %#lx from visual %#lx to %#lx\n",
           surface->snapshot, surface->source_visual, default_visual.visualid );
    x11drv_client_surface_set_gpu_snapshot( surface, 0 );
    return TRUE;
}

static BOOL x11drv_client_surface_handoff_prepare(
    struct client_surface *client, struct client_surface_source *image )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    BOOL native = usexcomposite && !surface->direct_snapshot;
    /* Native raw drawables use monitor pixels. Private GL/Vulkan snapshots
     * retain the rendered virtual extent; the owner scales their immutable
     * pixels to its monitor layout. Advertise the storage actually captured,
     * including when PREPARING delayed this reservation until after capture. */
    RECT source = native && client->raw ? client->target.monitor_rect : client->target.virtual_rect;
    unsigned int width, height;

    if (!x11drv_client_surface_prepare_retirement( surface )) return FALSE;
    if (source.right <= source.left || source.bottom <= source.top) return FALSE;
    width = source.right - source.left;
    height = source.bottom - source.top;
    image->source = native ? surface->window : surface->snapshot;
    if (surface->gpu_snapshot) image->source = surface->gpu_snapshot;
    image->source_visual = native ? surface->source_visual : default_visual.visualid;
    image->flags = CLIENT_SURFACE_HANDOFF_NATIVE_X11 | CLIENT_SURFACE_HANDOFF_FULL_DAMAGE;
    image->width = width;
    image->height = height;
    if (!native) image->flags |= CLIENT_SURFACE_HANDOFF_COPY_SOURCE;
    surface->sources[image - client->handoff_source].gpu_copy = FALSE;
    return TRUE;
}

static BOOL x11drv_client_surface_handoff_serialize( struct client_surface *client )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );

    /* A host Present completion does not retain an old mutable X window's
     * pixels. Freeze it before permitting another offscreen native swap.
     * Independent snapshots instead wait for one writable source in the
     * common code; filling their storage does not require a full drain. */
    return usexcomposite && !surface->direct_snapshot && client->target.offscreen;
}

static BOOL x11drv_client_surface_handoff_complete( struct client_surface *client,
                                                   struct client_surface_source *image )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    unsigned int index = image - client->handoff_source;
    struct x11drv_client_source_frame *frame = &surface->sources[index];
    BOOL native = usexcomposite && !surface->direct_snapshot;
    unsigned int depth = native ? surface->source_depth : default_visual.depth;
    Pixmap source = surface->snapshot;
    int error = 0;

    assert( index < ARRAY_SIZE(surface->sources) );
    /* The core validated this reservation and consumed its native completion.
     * gpu_copy selects storage; it is not itself proof of GPU completion. */
    if (frame->gpu_copy)
    {
        image->source = frame->pixmap;
        x11drv_client_surface_set_gpu_snapshot( surface, frame->pixmap );
        surface->gpu_snapshot_size = (SIZE){image->width, image->height};
        image->source_visual = default_visual.visualid;
        image->flags |= CLIENT_SURFACE_HANDOFF_COPY_SOURCE;
        return TRUE;
    }
    if (!(frame = x11drv_client_surface_get_source( client, index, image->width, image->height, depth )))
        return FALSE;
    if (surface->gpu_snapshot && surface->gpu_snapshot_size.cx >= image->width &&
        surface->gpu_snapshot_size.cy >= image->height)
        source = surface->gpu_snapshot;
    X11DRV_expect_error( gdi_display, client_surface_clip_error, &error );
#ifdef SONAME_LIBXCOMPOSITE
    if (native)
    {
        if (!surface->snapshot_import || surface->snapshot_import_epoch != image->target_epoch)
        {
            if (surface->snapshot_import) XFreePixmap( gdi_display, surface->snapshot_import );
            surface->snapshot_import = pXCompositeNameWindowPixmap( gdi_display, surface->window );
            surface->snapshot_import_epoch = image->target_epoch;
            XSync( gdi_display, False );
            X11DRV_check_error();
            if (error)
            {
                /* NameWindowPixmap allocates an XID before the server checks
                 * the source. An unsuccessful name must never be freed. */
                surface->snapshot_import = 0;
                return FALSE;
            }
            X11DRV_expect_error( gdi_display, client_surface_clip_error, &error );
        }
        source = surface->snapshot_import;
    }
#endif
    if (!frame->gc) frame->gc = XCreateGC( gdi_display, frame->pixmap, 0, NULL );
    if (source && frame->gc)
        XCopyArea( gdi_display, source, frame->pixmap, frame->gc, 0, 0, image->width, image->height, 0, 0 );
    /* This boundary proves the source image is immutable before READY, not
     * merely that its copy request was queued. The other slot remains usable
     * while the owner reads this independent pixmap. */
    XSync( gdi_display, False );
    X11DRV_check_error();
    if (error || !source || !frame->gc)
    {
        if (frame->pixmap == surface->gpu_snapshot) x11drv_client_surface_set_gpu_snapshot( surface, 0 );
        if (frame->image) frame->release_image( frame->image );
        frame->image = NULL;
        discard_client_surface_source( &frame->pixmap, &frame->gc, &frame->bytes, "producer_slot" );
        return FALSE;
    }
    if (source == surface->gpu_snapshot)
    {
        x11drv_client_surface_set_gpu_snapshot( surface, frame->pixmap );
        surface->gpu_snapshot_size = (SIZE){image->width, image->height};
    }
    image->source = frame->pixmap;
    image->flags |= CLIENT_SURFACE_HANDOFF_COPY_SOURCE;
    return TRUE;
}

static const struct client_surface_backend x11drv_client_surface_backend =
{
    .caps = CLIENT_SURFACE_BACKEND_SCENE_PUBLICATION |
            CLIENT_SURFACE_BACKEND_READ_ONLY_DC |
            CLIENT_SURFACE_BACKEND_DIRECT_PRESENTATION |
            CLIENT_SURFACE_BACKEND_GENERATION_HANDOFF |
            CLIENT_SURFACE_BACKEND_OWNER_COMPOSITOR |
            CLIENT_SURFACE_BACKEND_OWNER_SCENE_PLAN,
    .destroy = x11drv_client_surface_destroy,
    .detach = x11drv_client_surface_detach,
    .direct_ready = x11drv_client_surface_direct_ready,
    .prepare_direct = X11DRV_client_surface_prepare_direct,
    .complete_direct = X11DRV_client_surface_complete_direct,
    .update = x11drv_client_surface_update,
    .handoff_prepare = x11drv_client_surface_handoff_prepare,
    .handoff_complete = x11drv_client_surface_handoff_complete,
    .handoff_serialize = x11drv_client_surface_handoff_serialize,
    .handoff_retire = x11drv_client_surface_retire_handoff,
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
    surface->source_depth = visual.depth;
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
