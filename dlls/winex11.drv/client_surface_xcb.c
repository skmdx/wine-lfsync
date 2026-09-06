/*
 * Asynchronous X11 owner compositor request validation
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
#include <assert.h>

#include "x11drv.h"
#include "client_surface_xcb.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);

#if defined(SONAME_LIBX11_XCB) && defined(SONAME_LIBXCB) && defined(SONAME_LIBXCB_PRESENT)

#include <X11/Xlib-xcb.h>
#include <xcb/xcbext.h>
#include <xcb/present.h>

static typeof(XGetXCBConnection) *pXGetXCBConnection;
static typeof(xcb_present_pixmap_checked) *pxcb_present_pixmap_checked;
static typeof(xcb_get_input_focus) *pxcb_get_input_focus;
static typeof(xcb_poll_for_reply) *pxcb_poll_for_reply;
static typeof(xcb_request_check) *pxcb_request_check;
static typeof(xcb_flush) *pxcb_flush;
static typeof(xcb_generate_id) *pxcb_generate_id;
static typeof(xcb_create_gc_checked) *pxcb_create_gc_checked;
static typeof(xcb_change_gc_checked) *pxcb_change_gc_checked;
static typeof(xcb_set_clip_rectangles_checked) *pxcb_set_clip_rectangles_checked;
static typeof(xcb_copy_area_checked) *pxcb_copy_area_checked;
static typeof(xcb_free_gc_checked) *pxcb_free_gc_checked;
static pthread_once_t client_surface_xcb_once = PTHREAD_ONCE_INIT;
static BOOL client_surface_xcb_initialized;

static void client_surface_xcb_init(void)
{
    void *xlib = NULL, *xcb = NULL, *present = NULL;

    if (!(xlib = dlopen( SONAME_LIBX11_XCB, RTLD_NOW )) ||
        !(xcb = dlopen( SONAME_LIBXCB, RTLD_NOW )) ||
        !(present = dlopen( SONAME_LIBXCB_PRESENT, RTLD_NOW ))) goto failed;
#define LOAD_FUNCPTR(handle, f) if (!(p##f = dlsym( handle, #f ))) goto failed
    LOAD_FUNCPTR( xlib, XGetXCBConnection );
    LOAD_FUNCPTR( present, xcb_present_pixmap_checked );
    LOAD_FUNCPTR( xcb, xcb_get_input_focus );
    LOAD_FUNCPTR( xcb, xcb_poll_for_reply );
    LOAD_FUNCPTR( xcb, xcb_request_check );
    LOAD_FUNCPTR( xcb, xcb_flush );
    LOAD_FUNCPTR( xcb, xcb_generate_id );
    LOAD_FUNCPTR( xcb, xcb_create_gc_checked );
    LOAD_FUNCPTR( xcb, xcb_change_gc_checked );
    LOAD_FUNCPTR( xcb, xcb_set_clip_rectangles_checked );
    LOAD_FUNCPTR( xcb, xcb_copy_area_checked );
    LOAD_FUNCPTR( xcb, xcb_free_gc_checked );
#undef LOAD_FUNCPTR
    client_surface_xcb_initialized = TRUE;
    return;

failed:
    if (present) dlclose( present );
    if (xcb) dlclose( xcb );
    if (xlib) dlclose( xlib );
}

BOOL client_surface_xcb_available( Display *display )
{
    pthread_once( &client_surface_xcb_once, client_surface_xcb_init );
    return client_surface_xcb_initialized && pXGetXCBConnection( display );
}

void client_surface_xcb_flush( Display *display, struct client_surface_xcb_request *request )
{
    xcb_connection_t *connection = pXGetXCBConnection( display );

    request->barrier = pxcb_get_input_focus( connection ).sequence;
    pxcb_flush( connection );
}

BOOL client_surface_xcb_present( Display *display, Window window, Pixmap pixmap,
                                 unsigned int serial, struct client_surface_xcb_request *request )
{
    xcb_connection_t *connection;

    if (!client_surface_xcb_available( display )) return FALSE;
    connection = pXGetXCBConnection( display );

    /* Keep Xlib's event queue ownership and Present event decoding. Flush its
     * buffered requests before appending checked requests on that connection. */
    XFlush( display );
    request->count = 1;
    request->cookies[0] = pxcb_present_pixmap_checked( connection, window, pixmap, serial,
        0, 0, 0, 0, 0, 0, 0, XCB_PRESENT_OPTION_ASYNC | XCB_PRESENT_OPTION_COPY,
        0, 0, 0, 0, NULL ).sequence;
    client_surface_xcb_flush( display, request );
    return TRUE;
}

BOOL client_surface_xcb_copy( Display *display, Pixmap source, Pixmap destination,
                              unsigned int *gc, Pixmap checkpoint, const RECT *catchup,
                              const RECT *damage, const RECT *placement,
                              const XRectangle *clips, unsigned int clip_count, BOOL clipped,
                              struct client_surface_xcb_request *request, BOOL flush )
{
    xcb_connection_t *connection;
    uint32_t value = 0;

    C_ASSERT( sizeof(XRectangle) == sizeof(xcb_rectangle_t) );
    if (!client_surface_xcb_available( display )) return FALSE;
    connection = pXGetXCBConnection( display );
    XFlush( display );
    request->count = 0;
    if (!*gc)
    {
        *gc = pxcb_generate_id( connection );
        request->cookies[request->count++] = pxcb_create_gc_checked( connection, *gc,
            destination, XCB_GC_GRAPHICS_EXPOSURES, &value ).sequence;
    }
    /* This GC belongs to XCB. Changing an Xlib GC behind its cached state
     * would leave the synchronous visual/scale fallback with a stale clip. */
    request->cookies[request->count++] = pxcb_change_gc_checked( connection, *gc,
        XCB_GC_CLIP_MASK, &value ).sequence;
    if (checkpoint)
        request->cookies[request->count++] = pxcb_copy_area_checked( connection, checkpoint,
            destination, *gc, catchup->left, catchup->top,
            catchup->left, catchup->top, catchup->right - catchup->left,
            catchup->bottom - catchup->top ).sequence;
    if (clip_count)
    {
        if (clipped)
            request->cookies[request->count++] = pxcb_set_clip_rectangles_checked( connection,
                XCB_CLIP_ORDERING_YX_BANDED, *gc, placement->left, placement->top,
                clip_count, (const xcb_rectangle_t *)clips ).sequence;
        request->cookies[request->count++] = pxcb_copy_area_checked( connection, source,
            destination, *gc, damage->left, damage->top,
            placement->left + damage->left, placement->top + damage->top,
            damage->right - damage->left, damage->bottom - damage->top ).sequence;
    }
    assert( request->count <= ARRAY_SIZE(request->cookies) );
    if (flush) client_surface_xcb_flush( display, request );
    return TRUE;
}

void client_surface_xcb_free_gc( Display *display, unsigned int *gc )
{
    xcb_connection_t *connection;
    xcb_generic_error_t *error;

    if (!*gc) return;
    connection = pXGetXCBConnection( display );
    XFlush( display );
    /* Allocation failure can leave an invalid GC id. Consume the checked
     * free error here; cold teardown is allowed to synchronize. */
    error = pxcb_request_check( connection, pxcb_free_gc_checked( connection, *gc ));
    free( error );
    *gc = 0;
}

BOOL client_surface_xcb_poll( Display *display, struct client_surface_xcb_request *request,
                              BOOL *success )
{
    return client_surface_xcb_poll_batch( display, request, 1, success );
}

BOOL client_surface_xcb_poll_batch( Display *display, struct client_surface_xcb_request *requests,
                                    unsigned int count, BOOL *success )
{
    xcb_connection_t *connection = pXGetXCBConnection( display );
    xcb_generic_error_t *error = NULL;
    unsigned int i, j;
    void *reply = NULL;

    assert( count );
    if (!pxcb_poll_for_reply( connection, requests[count - 1].barrier, &reply, &error )) return FALSE;
    *success = reply && !error;
    free( reply );
    free( error );
    /* A later reply has arrived: request_check cannot need another sync.
     * Checked errors are owned by this cookie, not Xlib's global error trap. */
    for (j = 0; j < count; ++j)
    {
        const struct client_surface_xcb_request *request = &requests[j];

        for (i = 0; i < request->count; ++i)
        {
            xcb_void_cookie_t cookie = {request->cookies[i]};

            error = pxcb_request_check( connection, cookie );
            if (error)
            {
                WARN( "owner request %u failed with X error %u opcode %u:%u\n",
                      cookie.sequence, error->error_code, error->major_code, error->minor_code );
                *success = FALSE;
            }
            free( error );
        }
    }
    return TRUE;
}

#else

BOOL client_surface_xcb_available( Display *display )
{
    return FALSE;
}

void client_surface_xcb_flush( Display *display, struct client_surface_xcb_request *request )
{
}

BOOL client_surface_xcb_present( Display *display, Window window, Pixmap pixmap,
                                 unsigned int serial, struct client_surface_xcb_request *request )
{
    return FALSE;
}

BOOL client_surface_xcb_poll( Display *display, struct client_surface_xcb_request *request,
                              BOOL *success )
{
    return FALSE;
}

BOOL client_surface_xcb_poll_batch( Display *display, struct client_surface_xcb_request *requests,
                                    unsigned int count, BOOL *success )
{
    return FALSE;
}

BOOL client_surface_xcb_copy( Display *display, Pixmap source, Pixmap destination,
                              unsigned int *gc, Pixmap checkpoint, const RECT *catchup,
                              const RECT *damage, const RECT *placement,
                              const XRectangle *clips, unsigned int clip_count, BOOL clipped,
                              struct client_surface_xcb_request *request, BOOL flush )
{
    return FALSE;
}

void client_surface_xcb_free_gc( Display *display, unsigned int *gc )
{
}

#endif
