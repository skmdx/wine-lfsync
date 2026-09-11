/* Optional checked XCB requests on the owner compositor's Xlib connection. */

#ifndef __WINE_CLIENT_SURFACE_XCB_H
#define __WINE_CLIENT_SURFACE_XCB_H

struct client_surface_xcb_request
{
    unsigned int cookies[5];
    unsigned int count;
    unsigned int barrier;
};

BOOL client_surface_xcb_present( Display *display, Window window, Pixmap pixmap,
                                 unsigned int serial, struct client_surface_xcb_request *request );
BOOL client_surface_xcb_available( Display *display );
BOOL client_surface_xcb_check_direct( Display *display, Window owner, Window drawable,
                                      unsigned int width, unsigned int height, const RECT *rect );
void client_surface_xcb_flush( Display *display, struct client_surface_xcb_request *request );
BOOL client_surface_xcb_poll( Display *display, struct client_surface_xcb_request *request,
                              BOOL *success );
BOOL client_surface_xcb_poll_batch( Display *display, struct client_surface_xcb_request *requests,
                                    unsigned int count, BOOL *success );
BOOL client_surface_xcb_copy( Display *display, Pixmap source, Pixmap destination,
                              unsigned int *gc, Pixmap checkpoint, const RECT *catchup,
                              const RECT *damage, const RECT *placement,
                              const XRectangle *clips, unsigned int clip_count, BOOL clipped,
                              struct client_surface_xcb_request *request, BOOL flush );
void client_surface_xcb_free_gc( Display *display, unsigned int *gc );
void client_surface_xcb_free_gc_async( Display *display, unsigned int gc,
                                      struct client_surface_xcb_request *request );

#endif
