/*
 * X11DRV OpenGL functions
 *
 * Copyright 2000 Lionel Ulmer
 * Copyright 2005 Alex Woods
 * Copyright 2005 Raphael Junqueira
 * Copyright 2006-2009 Roderick Colenbrander
 * Copyright 2006 Tomas Carnecky
 * Copyright 2012 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/socket.h>
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif

#include "ntstatus.h"
#include "client_surface.h"
#include "xcomposite.h"
#include "winternl.h"
#include "wine/debug.h"

#ifdef SONAME_LIBGL

WINE_DEFAULT_DEBUG_CHANNEL(wgl);
WINE_DECLARE_DEBUG_CHANNEL(winediag);
WINE_DECLARE_DEBUG_CHANNEL(csperf);

static unsigned long long client_surface_perf_time(void)
{
    LARGE_INTEGER counter;

    NtQueryPerformanceCounter( &counter, NULL );
    return counter.QuadPart;
}

#include "wine/opengl_driver.h"

typedef struct __GLXcontextRec *GLXContext;
typedef struct __GLXFBConfigRec *GLXFBConfig;
typedef XID GLXPixmap;
typedef XID GLXDrawable;
typedef XID GLXFBConfigID;
typedef XID GLXContextID;
typedef XID GLXWindow;
typedef XID GLXPbuffer;

#define GLX_USE_GL                        1
#define GLX_BUFFER_SIZE                   2
#define GLX_LEVEL                         3
#define GLX_RGBA                          4
#define GLX_DOUBLEBUFFER                  5
#define GLX_STEREO                        6
#define GLX_AUX_BUFFERS                   7
#define GLX_RED_SIZE                      8
#define GLX_GREEN_SIZE                    9
#define GLX_BLUE_SIZE                     10
#define GLX_ALPHA_SIZE                    11
#define GLX_DEPTH_SIZE                    12
#define GLX_STENCIL_SIZE                  13
#define GLX_ACCUM_RED_SIZE                14
#define GLX_ACCUM_GREEN_SIZE              15
#define GLX_ACCUM_BLUE_SIZE               16
#define GLX_ACCUM_ALPHA_SIZE              17

#define GLX_BAD_SCREEN                    1
#define GLX_BAD_ATTRIBUTE                 2
#define GLX_NO_EXTENSION                  3
#define GLX_BAD_VISUAL                    4
#define GLX_BAD_CONTEXT                   5
#define GLX_BAD_VALUE                     6
#define GLX_BAD_ENUM                      7

#define GLX_VENDOR                        1
#define GLX_VERSION                       2
#define GLX_EXTENSIONS                    3

#define GLX_CONFIG_CAVEAT                 0x20
#define GLX_DONT_CARE                     0xFFFFFFFF
#define GLX_X_VISUAL_TYPE                 0x22
#define GLX_TRANSPARENT_TYPE              0x23
#define GLX_TRANSPARENT_INDEX_VALUE       0x24
#define GLX_TRANSPARENT_RED_VALUE         0x25
#define GLX_TRANSPARENT_GREEN_VALUE       0x26
#define GLX_TRANSPARENT_BLUE_VALUE        0x27
#define GLX_TRANSPARENT_ALPHA_VALUE       0x28
#define GLX_WINDOW_BIT                    0x00000001
#define GLX_PIXMAP_BIT                    0x00000002
#define GLX_PBUFFER_BIT                   0x00000004
#define GLX_AUX_BUFFERS_BIT               0x00000010
#define GLX_FRONT_LEFT_BUFFER_BIT         0x00000001
#define GLX_FRONT_RIGHT_BUFFER_BIT        0x00000002
#define GLX_BACK_LEFT_BUFFER_BIT          0x00000004
#define GLX_BACK_RIGHT_BUFFER_BIT         0x00000008
#define GLX_DEPTH_BUFFER_BIT              0x00000020
#define GLX_STENCIL_BUFFER_BIT            0x00000040
#define GLX_ACCUM_BUFFER_BIT              0x00000080
#define GLX_NONE                          0x8000
#define GLX_SLOW_CONFIG                   0x8001
#define GLX_TRUE_COLOR                    0x8002
#define GLX_DIRECT_COLOR                  0x8003
#define GLX_PSEUDO_COLOR                  0x8004
#define GLX_STATIC_COLOR                  0x8005
#define GLX_GRAY_SCALE                    0x8006
#define GLX_STATIC_GRAY                   0x8007
#define GLX_TRANSPARENT_RGB               0x8008
#define GLX_TRANSPARENT_INDEX             0x8009
#define GLX_VISUAL_ID                     0x800B
#define GLX_SCREEN                        0x800C
#define GLX_NON_CONFORMANT_CONFIG         0x800D
#define GLX_DRAWABLE_TYPE                 0x8010
#define GLX_RENDER_TYPE                   0x8011
#define GLX_X_RENDERABLE                  0x8012
#define GLX_FBCONFIG_ID                   0x8013
#define GLX_RGBA_TYPE                     0x8014
#define GLX_COLOR_INDEX_TYPE              0x8015
#define GLX_MAX_PBUFFER_WIDTH             0x8016
#define GLX_MAX_PBUFFER_HEIGHT            0x8017
#define GLX_MAX_PBUFFER_PIXELS            0x8018
#define GLX_PRESERVED_CONTENTS            0x801B
#define GLX_LARGEST_PBUFFER               0x801C
#define GLX_WIDTH                         0x801D
#define GLX_HEIGHT                        0x801E
#define GLX_EVENT_MASK                    0x801F
#define GLX_DAMAGED                       0x8020
#define GLX_SAVED                         0x8021
#define GLX_WINDOW                        0x8022
#define GLX_PBUFFER                       0x8023
#define GLX_PBUFFER_HEIGHT                0x8040
#define GLX_PBUFFER_WIDTH                 0x8041
#define GLX_SWAP_METHOD_OML               0x8060
#define GLX_SWAP_EXCHANGE_OML             0x8061
#define GLX_SWAP_COPY_OML                 0x8062
#define GLX_SWAP_UNDEFINED_OML            0x8063
#define GLX_RGBA_BIT                      0x00000001
#define GLX_COLOR_INDEX_BIT               0x00000002
#define GLX_PBUFFER_CLOBBER_MASK          0x08000000

/** GLX_ARB_multisample */
#define GLX_SAMPLE_BUFFERS_ARB            100000
#define GLX_SAMPLES_ARB                   100001
/** GLX_ARB_framebuffer_sRGB */
#define GLX_FRAMEBUFFER_SRGB_CAPABLE_EXT  0x20B2
/** GLX_EXT_fbconfig_packed_float */
#define GLX_RGBA_UNSIGNED_FLOAT_TYPE_EXT  0x20B1
#define GLX_RGBA_UNSIGNED_FLOAT_BIT_EXT   0x00000008
/** GLX_ARB_create_context */
#define GLX_CONTEXT_MAJOR_VERSION_ARB     0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB     0x2092
#define GLX_CONTEXT_FLAGS_ARB             0x2094
/** GLX_ARB_create_context_no_error */
#define GLX_CONTEXT_OPENGL_NO_ERROR_ARB   0x31B3
/** GLX_ARB_create_context_profile */
#define GLX_CONTEXT_PROFILE_MASK_ARB      0x9126
/** GLX_ATI_pixel_format_float */
#define GLX_RGBA_FLOAT_ATI_BIT            0x00000100
/** GLX_ARB_pixel_format_float */
#define GLX_RGBA_FLOAT_BIT                0x00000004
#define GLX_RGBA_FLOAT_TYPE               0x20B9
/** GLX_MESA_query_renderer */
#define GLX_RENDERER_ID_MESA              0x818E
/** GLX_NV_float_buffer */
#define GLX_FLOAT_COMPONENTS_NV           0x20B0


static const char *glExtensions;
static const char *glxExtensions;
static int glxVersion[2];
static int glx_opcode;

/* Serialize runtime context operations before taking an X error scope. GLX
 * context creation can hold the display lock while updating libGLX's context
 * table, whereas binding and destruction can hold native context locks while
 * sending X requests. Do not hold the display lock around ordinary binding:
 * releasing the old context can wait for presentation while the GUI needs it.
 * The initialization probe runs before the driver is published. */
static pthread_mutex_t glx_context_mutex = PTHREAD_MUTEX_INITIALIZER;

struct glx_pixel_format
{
    GLXFBConfig fbconfig;
    XVisualInfo *visual;
    int         fmt_id;
    int         render_type;
    DWORD       dwFlags; /* We store some PFD_* flags in here for emulated bitmap formats */
};

struct glx_completion_context
{
    pthread_mutex_t lock;
    GLXContext context;
    GLXPbuffer pbuffer;
};

struct gl_drawable
{
    struct opengl_drawable         base;
    GLXDrawable                    drawable;     /* drawable for rendering with GL */
    BOOL                           gpu_snapshot_failed;
    LONG64                         egl_geometry_epoch; /* last native target observed by EGL */
    struct glx_completion_context *completion;
};

static void *alloc_client_surface_metadata( SIZE_T size )
{
    void *data;

    if (!client_surface_reserve_memory( CLIENT_SURFACE_MEMORY_STAGING, size )) return NULL;
    if (!(data = calloc( 1, size )))
        client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING, size );
    return data;
}

static void free_client_surface_metadata( void *data, SIZE_T size )
{
    if (!data) return;
    free( data );
    client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING, size );
}

static struct gl_drawable *impl_from_opengl_drawable( struct opengl_drawable *base )
{
    return CONTAINING_RECORD( base, struct gl_drawable, base );
}

enum glx_swap_control_method
{
    GLX_SWAP_CONTROL_NONE,
    GLX_SWAP_CONTROL_EXT,
    GLX_SWAP_CONTROL_SGI,
    GLX_SWAP_CONTROL_MESA
};

static struct glx_pixel_format *pixel_formats;
static int nb_pixel_formats;
static const struct egl_platform *egl;
static BOOL (*p_egl_describe_pixel_format)( int format, struct wgl_pixel_format *pf );

/* Selects the preferred GLX swap control method for use by wglSwapIntervalEXT */
static enum glx_swap_control_method swap_control_method = GLX_SWAP_CONTROL_NONE;
/* Set when GLX_EXT_swap_control_tear is supported, requires GLX_SWAP_CONTROL_EXT */
static BOOL has_swap_control_tear = FALSE;
static BOOL has_swap_method = FALSE;

static const BOOL is_win64 = sizeof(void *) > sizeof(int);

static BOOL glxRequireVersion(int requiredVersion);

static void dump_PIXELFORMATDESCRIPTOR(const PIXELFORMATDESCRIPTOR *ppfd) {
  TRACE( "size %u version %u flags %#x type %u color %u %u,%u,%u,%u "
         "accum %u depth %u stencil %u aux %u ",
         ppfd->nSize, ppfd->nVersion, ppfd->dwFlags, ppfd->iPixelType,
         ppfd->cColorBits, ppfd->cRedBits, ppfd->cGreenBits, ppfd->cBlueBits, ppfd->cAlphaBits,
         ppfd->cAccumBits, ppfd->cDepthBits, ppfd->cStencilBits, ppfd->cAuxBuffers );
#define TEST_AND_DUMP(t,tv) if ((t) & (tv)) TRACE(#tv " ")
  TEST_AND_DUMP(ppfd->dwFlags, PFD_DEPTH_DONTCARE);
  TEST_AND_DUMP(ppfd->dwFlags, PFD_DOUBLEBUFFER);
  TEST_AND_DUMP(ppfd->dwFlags, PFD_DOUBLEBUFFER_DONTCARE);
  TEST_AND_DUMP(ppfd->dwFlags, PFD_DRAW_TO_WINDOW);
  TEST_AND_DUMP(ppfd->dwFlags, PFD_DRAW_TO_BITMAP);
  TEST_AND_DUMP(ppfd->dwFlags, PFD_GENERIC_ACCELERATED);
  TEST_AND_DUMP(ppfd->dwFlags, PFD_GENERIC_FORMAT);
  TEST_AND_DUMP(ppfd->dwFlags, PFD_NEED_PALETTE);
  TEST_AND_DUMP(ppfd->dwFlags, PFD_NEED_SYSTEM_PALETTE);
  TEST_AND_DUMP(ppfd->dwFlags, PFD_STEREO);
  TEST_AND_DUMP(ppfd->dwFlags, PFD_STEREO_DONTCARE);
  TEST_AND_DUMP(ppfd->dwFlags, PFD_SUPPORT_GDI);
  TEST_AND_DUMP(ppfd->dwFlags, PFD_SUPPORT_OPENGL);
  TEST_AND_DUMP(ppfd->dwFlags, PFD_SWAP_COPY);
  TEST_AND_DUMP(ppfd->dwFlags, PFD_SWAP_EXCHANGE);
  TEST_AND_DUMP(ppfd->dwFlags, PFD_SWAP_LAYER_BUFFERS);
  /* PFD_SUPPORT_COMPOSITION is new in Vista, it is similar to composition
   * under X e.g. COMPOSITE + GLX_EXT_TEXTURE_FROM_PIXMAP. */
  TEST_AND_DUMP(ppfd->dwFlags, PFD_SUPPORT_COMPOSITION);
#undef TEST_AND_DUMP
  TRACE("\n");
}

/* GLX 1.0 */
static XVisualInfo* (*pglXChooseVisual)( Display *dpy, int screen, int *attribList );
static GLXContext (*pglXCreateContext)( Display *dpy, XVisualInfo *vis, GLXContext shareList, Bool direct );
static void (*pglXDestroyContext)( Display *dpy, GLXContext ctx );
static Bool (*pglXMakeCurrent)( Display *dpy, GLXDrawable drawable, GLXContext ctx);
static void (*pglXCopyContext)( Display *dpy, GLXContext src, GLXContext dst, unsigned long mask );
static void (*pglXSwapBuffers)( Display *dpy, GLXDrawable drawable );
static Bool (*pglXQueryVersion)( Display *dpy, int *maj, int *min );
static Bool (*pglXIsDirect)( Display *dpy, GLXContext ctx );
static GLXContext (*pglXGetCurrentContext)( void );
static GLXDrawable (*pglXGetCurrentDrawable)( void );

/* GLX 1.1 */
static const char *(*pglXQueryExtensionsString)( Display *dpy, int screen );
static const char *(*pglXQueryServerString)( Display *dpy, int screen, int name );
static const char *(*pglXGetClientString)( Display *dpy, int name );

/* GLX 1.2 */
static Display *(*pglXGetCurrentDisplay)( void );

/* GLX 1.3 */
static int (*pglXGetFBConfigAttrib)( Display *dpy, GLXFBConfig config, int attribute, int *value );
static GLXFBConfig *(*pglXGetFBConfigs)( Display *dpy, int screen, int *nelements );
static XVisualInfo *(*pglXGetVisualFromFBConfig)( Display *dpy, GLXFBConfig config );
static GLXPbuffer (*pglXCreatePbuffer)( Display *dpy, GLXFBConfig config, const int *attribList );
static void (*pglXDestroyPbuffer)( Display *dpy, GLXPbuffer pbuf );
static void (*pglXQueryDrawable)( Display *dpy, GLXDrawable draw, int attribute, unsigned int *value );
static GLXContext (*pglXCreateNewContext)( Display *dpy, GLXFBConfig config, int renderType, GLXContext shareList, Bool direct );
static Bool (*pglXMakeContextCurrent)( Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext ctx );
static GLXPixmap (*pglXCreatePixmap)( Display *dpy, GLXFBConfig config, Pixmap pixmap, const int *attrib_list );
static void (*pglXDestroyPixmap)( Display *dpy, GLXPixmap pixmap );
static GLXWindow (*pglXCreateWindow)( Display *dpy, GLXFBConfig config, Window win, const int *attrib_list );
static void (*pglXDestroyWindow)( Display *dpy, GLXWindow win );

/* GLX Extensions */
static GLXContext (*pglXCreateContextAttribsARB)(Display *dpy, GLXFBConfig config, GLXContext share_context, Bool direct, const int *attrib_list);
static void* (*pglXGetProcAddressARB)(const GLubyte *);
static void (*pglXSwapIntervalEXT)(Display *dpy, GLXDrawable drawable, int interval);
static int   (*pglXSwapIntervalSGI)(int);

/* NV GLX Extension */
static void* (*pglXAllocateMemoryNV)(GLsizei size, GLfloat readfreq, GLfloat writefreq, GLfloat priority);
static void  (*pglXFreeMemoryNV)(GLvoid *pointer);

/* MESA GLX Extensions */
static int (*pglXSwapIntervalMESA)(unsigned int interval);
static Bool (*pglXQueryCurrentRendererIntegerMESA)(int attribute, unsigned int *value);
static const char *(*pglXQueryCurrentRendererStringMESA)(int attribute);
static Bool (*pglXQueryRendererIntegerMESA)(Display *dpy, int screen, int renderer, int attribute, unsigned int *value);
static const char *(*pglXQueryRendererStringMESA)(Display *dpy, int screen, int renderer, int attribute);

/* OpenML GLX Extensions */
static Bool (*pglXGetSyncValuesOML)( Display *dpy, GLXDrawable drawable,
                                    INT64 *ust, INT64 *msc, INT64 *sbc );
static INT64 (*pglXSwapBuffersMscOML)( Display *dpy, GLXDrawable drawable,
                                      INT64 target_msc, INT64 divisor, INT64 remainder );

/* Standard OpenGL */
static const GLubyte *(*pglGetString)(GLenum name);

static void *opengl_handle;
static const struct opengl_funcs *funcs;
static PFN_eglCreateImageKHR snapshot_create_image;
static PFN_eglDestroyImageKHR snapshot_destroy_image;
static PFN_glEGLImageTargetRenderbufferStorageOES snapshot_bind_image;
static PFN_eglCreateSyncKHR snapshot_create_sync;
static PFN_eglDestroySyncKHR snapshot_destroy_sync;
static PFN_eglClientWaitSyncKHR snapshot_wait_sync;
static pthread_once_t snapshot_once = PTHREAD_ONCE_INIT;
static struct opengl_driver_funcs x11drv_driver_funcs;
static const struct opengl_drawable_funcs x11drv_surface_funcs;
static const struct opengl_drawable_funcs x11drv_pbuffer_funcs;
static const struct opengl_drawable_funcs x11drv_egl_surface_funcs;
static const struct opengl_drawable_funcs x11drv_egl_snapshot_surface_funcs;

/* check if the extension is present in the list */
static BOOL has_extension( const char *list, const char *ext )
{
    size_t len = strlen( ext );

    while (list)
    {
        while (*list == ' ') list++;
        if (!strncmp( list, ext, len ) && (!list[len] || list[len] == ' ')) return TRUE;
        list = strchr( list, ' ' );
    }
    return FALSE;
}

static int GLXErrorHandler(Display *dpy, XErrorEvent *event, void *arg)
{
    /* In the future we might want to find the exact X or GLX error to report back to the app */
    if (event->request_code != glx_opcode)
        return 0;
    return 1;
}

static BOOL X11DRV_WineGL_InitOpenglInfo(void)
{
    int screen = DefaultScreen(gdi_display);
    Window win = 0, root = 0;
    const char *gl_version;
    const char *gl_renderer;
    const char *gl_extensions;
    BOOL glx_direct;
    XVisualInfo *vis;
    GLXContext ctx = NULL;
    XSetWindowAttributes attr;
    BOOL ret = FALSE;
    int attribList[] = {GLX_RGBA, GLX_DOUBLEBUFFER, None};

    attr.override_redirect = True;
    attr.colormap = None;
    attr.border_pixel = 0;

    vis = pglXChooseVisual(gdi_display, screen, attribList);
    if (vis) {
#ifdef __i386__
        WORD old_fs, new_fs;
        __asm__( "mov %%fs,%0" : "=r" (old_fs) );
        /* Create a GLX Context. Without one we can't query GL information */
        ctx = pglXCreateContext(gdi_display, vis, None, GL_TRUE);
        __asm__( "mov %%fs,%0" : "=r" (new_fs) );
        __asm__( "mov %0,%%fs" :: "r" (old_fs) );
        if (old_fs != new_fs)
        {
            ERR( "%%fs register corrupted, probably broken ATI driver, disabling OpenGL.\n" );
            ERR( "You need to set the \"UseFastTls\" option to \"2\" in your X config file.\n" );
            goto done;
        }
#else
        ctx = pglXCreateContext(gdi_display, vis, None, GL_TRUE);
#endif
    }
    if (!ctx) goto done;

    root = RootWindow( gdi_display, vis->screen );
    if (vis->visual != DefaultVisual( gdi_display, vis->screen ))
        attr.colormap = XCreateColormap( gdi_display, root, vis->visual, AllocNone );
    if ((win = XCreateWindow( gdi_display, root, -1, -1, 1, 1, 0, vis->depth, InputOutput,
                              vis->visual, CWBorderPixel | CWOverrideRedirect | CWColormap, &attr )))
        XMapWindow( gdi_display, win );
    else
        win = root;

    if(pglXMakeCurrent(gdi_display, win, ctx) == 0)
    {
        ERR_(winediag)( "Unable to activate OpenGL context, most likely your %s OpenGL drivers haven't been "
                        "installed correctly\n", is_win64 ? "64-bit" : "32-bit" );
        goto done;
    }
    gl_renderer = (const char *)pglGetString(GL_RENDERER);
    gl_version  = (const char *)pglGetString(GL_VERSION);
    gl_extensions = (const char *)pglGetString(GL_EXTENSIONS);
    glExtensions = gl_extensions ? strdup( gl_extensions ) : NULL;

    /* Get the common GLX version supported by GLX client and server ( major/minor) */
    pglXQueryVersion(gdi_display, &glxVersion[0], &glxVersion[1]);

    glxExtensions = pglXQueryExtensionsString(gdi_display, screen);
    glx_direct = pglXIsDirect(gdi_display, ctx);

    TRACE("GL version             : %s.\n", gl_version);
    TRACE("GL renderer            : %s.\n", gl_renderer);
    TRACE("GLX version            : %d.%d.\n", glxVersion[0], glxVersion[1]);
    TRACE("Server GLX version     : %s.\n", pglXQueryServerString(gdi_display, screen, GLX_VERSION));
    TRACE("Server GLX vendor:     : %s.\n", pglXQueryServerString(gdi_display, screen, GLX_VENDOR));
    TRACE("Client GLX version     : %s.\n", pglXGetClientString(gdi_display, GLX_VERSION));
    TRACE("Client GLX vendor:     : %s.\n", pglXGetClientString(gdi_display, GLX_VENDOR));
    TRACE("Direct rendering enabled: %s\n", glx_direct ? "True" : "False");

    if(!glx_direct)
    {
        int fd = ConnectionNumber(gdi_display);
        struct sockaddr_un uaddr;
        unsigned int uaddrlen = sizeof(struct sockaddr_un);

        /* In general indirect rendering on a local X11 server indicates a driver problem.
         * Detect a local X11 server by checking whether the X11 socket is a Unix socket.
         */
        if(!getsockname(fd, (struct sockaddr *)&uaddr, &uaddrlen) && uaddr.sun_family == AF_UNIX)
            ERR_(winediag)("Direct rendering is disabled, most likely your %s OpenGL drivers "
                           "haven't been installed correctly (using GL renderer %s, version %s).\n",
                           is_win64 ? "64-bit" : "32-bit", debugstr_a(gl_renderer),
                           debugstr_a(gl_version));
    }
    else
    {
        /* In general you would expect that if direct rendering is returned, that you receive hardware
         * accelerated OpenGL rendering. The definition of direct rendering is that rendering is performed
         * client side without sending all GL commands to X using the GLX protocol. When Mesa falls back to
         * software rendering, it shows direct rendering.
         *
         * Depending on the cause of software rendering a different rendering string is shown. In case Mesa fails
         * to load a DRI module 'Software Rasterizer' is returned. When Mesa is compiled as a OpenGL reference driver
         * it shows 'Mesa X11'.
         */
        if(!strcmp(gl_renderer, "Software Rasterizer") || !strcmp(gl_renderer, "Mesa X11"))
            ERR_(winediag)("The Mesa OpenGL driver is using software rendering, most likely your %s OpenGL "
                           "drivers haven't been installed correctly (using GL renderer %s, version %s).\n",
                           is_win64 ? "64-bit" : "32-bit", debugstr_a(gl_renderer),
                           debugstr_a(gl_version));
    }
    ret = TRUE;

done:
    if(vis) XFree(vis);
    if(ctx) {
        pglXMakeCurrent(gdi_display, None, NULL);    
        pglXDestroyContext(gdi_display, ctx);
    }
    if (win != root) XDestroyWindow( gdi_display, win );
    if (attr.colormap) XFreeColormap( gdi_display, attr.colormap );
    if (!ret) ERR(" couldn't initialize OpenGL, expect problems\n");
    return ret;
}

static void x11drv_init_egl_platform( struct egl_platform *platform )
{
    platform->type = EGL_PLATFORM_X11_KHR;
    platform->native_display = gdi_display;
    egl = platform;
}

static EGLConfig egl_config_for_format( int format )
{
    return egl->configs[(format - 1) % egl->config_count];
}

static struct glx_pixel_format *glx_pixel_format_from_format( int format )
{
    assert( format > 0 && format <= nb_pixel_formats );
    return &pixel_formats[format - 1];
}

BOOL visual_from_pixel_format( int format, XVisualInfo *visual )
{
    if (use_egl)
    {
        EGLConfig config = egl_config_for_format( format );
        XVisualInfo *visuals;
        int count;

        memset( visual, 0, sizeof(*visual) );
        funcs->p_eglGetConfigAttrib( egl->display, config, EGL_NATIVE_VISUAL_ID, (EGLint *)&visual->visualid );
        if (!(visuals = XGetVisualInfo( gdi_display, VisualIDMask, visual, &count ))) return FALSE;
        *visual = *visuals;
        XFree( visuals );
        return TRUE;
    }
    else
    {
        struct glx_pixel_format *fmt = glx_pixel_format_from_format( format );
        *visual = *fmt->visual;
        return TRUE;
    }
}

static BOOL x11drv_egl_describe_pixel_format( int format, struct wgl_pixel_format *pf )
{
    XVisualInfo visual;

    if (!p_egl_describe_pixel_format( format, pf )) return FALSE;
    if (!visual_from_pixel_format( format, &visual ))
        pf->pfd.dwFlags &= ~PFD_DRAW_TO_WINDOW;

    return TRUE;
}

static void init_snapshot_image_funcs(void)
{
    snapshot_create_image = (void *)funcs->p_eglGetProcAddress( "eglCreateImageKHR" );
    snapshot_destroy_image = (void *)funcs->p_eglGetProcAddress( "eglDestroyImageKHR" );
    snapshot_bind_image = (void *)funcs->p_eglGetProcAddress( "glEGLImageTargetRenderbufferStorageOES" );
    if (has_extension( funcs->p_eglQueryString( egl->display, EGL_EXTENSIONS ), "EGL_KHR_fence_sync" ))
    {
        snapshot_create_sync = (void *)funcs->p_eglGetProcAddress( "eglCreateSyncKHR" );
        snapshot_destroy_sync = (void *)funcs->p_eglGetProcAddress( "eglDestroySyncKHR" );
        snapshot_wait_sync = (void *)funcs->p_eglGetProcAddress( "eglClientWaitSyncKHR" );
    }
}

static BOOL x11drv_egl_surface_create( struct client_surface *client, int format, struct opengl_drawable **drawable )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    struct gl_drawable *gl;
    DWORD process = 0;

    if (!(gl = opengl_drawable_create( sizeof(*gl), &x11drv_egl_surface_funcs, format, client ))) return FALSE;
    /* A foreign-process target always uses the owner compositor. Retain its
     * application front/back buffers in the existing FBO wrapper and copy
     * completed images directly into independent GPU source storage. */
    NtUserGetWindowThread( client->hwnd, &process );
    if (usexcomposite && process && process != GetCurrentProcessId())
        pthread_once( &snapshot_once, init_snapshot_image_funcs );
    surface->direct_snapshot = usexcomposite && process && process != GetCurrentProcessId() &&
        !gl->base.stereo && snapshot_create_image && snapshot_destroy_image && snapshot_bind_image &&
        has_extension( funcs->p_eglQueryString( egl->display, EGL_EXTENSIONS ), "EGL_KHR_image_pixmap" );
    gl->base.needs_framebuffer = !usexcomposite || surface->direct_snapshot;
    if (gl->base.needs_framebuffer) gl->base.funcs = &x11drv_egl_snapshot_surface_funcs;

    opengl_drawable_map_buffer( &gl->base, GL_FRONT_LEFT, GL_BACK_LEFT );
    opengl_drawable_map_buffer( &gl->base, GL_FRONT, GL_BACK );
    opengl_drawable_map_buffer( &gl->base, GL_FRONT_AND_BACK, GL_BACK );
    if (gl->base.stereo) opengl_drawable_map_buffer( &gl->base, GL_FRONT_RIGHT, GL_BACK_RIGHT );

    if (!(gl->base.surface = funcs->p_eglCreateWindowSurface( egl->display, egl_config_for_format( format ),
                                                              (void *)surface->window, NULL )))
    {
        opengl_drawable_release( &gl->base );
        return FALSE;
    }

    TRACE( "Created drawable %s with client window %lx\n", debugstr_opengl_drawable( &gl->base ), surface->window );
    XFlush( gdi_display );

    *drawable = &gl->base;
    return TRUE;
}

/**********************************************************************
 *           X11DRV_OpenglInit
 */
UINT X11DRV_OpenGLInit( UINT version, const struct opengl_funcs *opengl_funcs, const struct opengl_driver_funcs **driver_funcs )
{
    int error_base, event_base;

    if (version != WINE_OPENGL_DRIVER_VERSION)
    {
        ERR( "version mismatch, opengl32 wants %u but driver has %u\n", version, WINE_OPENGL_DRIVER_VERSION );
        return STATUS_INVALID_PARAMETER;
    }
    funcs = opengl_funcs;

    if (use_egl && opengl_funcs->egl_handle)
    {
        TRACE( "Using EGL OpenGL backend\n" );
        x11drv_driver_funcs = **driver_funcs;
        x11drv_driver_funcs.p_init_egl_platform = x11drv_init_egl_platform;
        x11drv_driver_funcs.p_surface_create = x11drv_egl_surface_create;
        x11drv_driver_funcs.p_describe_pixel_format = x11drv_egl_describe_pixel_format;
        p_egl_describe_pixel_format = (*driver_funcs)->p_describe_pixel_format;
        *driver_funcs = &x11drv_driver_funcs;
        return STATUS_SUCCESS;
    }

    use_egl = FALSE;

    /* No need to load any other libraries as according to the ABI, libGL should be self-sufficient
       and include all dependencies */
    opengl_handle = dlopen( SONAME_LIBGL, RTLD_NOW | RTLD_GLOBAL );
    if (opengl_handle == NULL)
    {
        ERR( "Failed to load libGL: %s\n", dlerror() );
        ERR( "OpenGL support is disabled.\n");
        return STATUS_NOT_SUPPORTED;
    }

    /* redirect some standard OpenGL functions */
#define LOAD_FUNCPTR(func) \
        if (!(p##func = dlsym( opengl_handle, #func ))) \
        { \
            ERR( "%s not found in libGL, disabling OpenGL.\n", #func ); \
            goto failed; \
        }
    LOAD_FUNCPTR( glGetString );
#undef LOAD_FUNCPTR

    pglXGetProcAddressARB = dlsym(opengl_handle, "glXGetProcAddressARB");
    if (pglXGetProcAddressARB == NULL) {
        ERR("Could not find glXGetProcAddressARB in libGL, disabling OpenGL.\n");
        goto failed;
    }

#define LOAD_FUNCPTR(f) do if((p##f = (void*)pglXGetProcAddressARB((const unsigned char*)#f)) == NULL) \
    { \
        ERR( "%s not found in libGL, disabling OpenGL.\n", #f ); \
        goto failed; \
    } while(0)

    /* GLX 1.0 */
    LOAD_FUNCPTR(glXChooseVisual);
    LOAD_FUNCPTR(glXCopyContext);
    LOAD_FUNCPTR(glXCreateContext);
    LOAD_FUNCPTR(glXGetCurrentContext);
    LOAD_FUNCPTR(glXGetCurrentDrawable);
    LOAD_FUNCPTR(glXDestroyContext);
    LOAD_FUNCPTR(glXIsDirect);
    LOAD_FUNCPTR(glXMakeCurrent);
    LOAD_FUNCPTR(glXSwapBuffers);
    LOAD_FUNCPTR(glXQueryVersion);

    /* GLX 1.1 */
    LOAD_FUNCPTR(glXGetClientString);
    LOAD_FUNCPTR(glXQueryExtensionsString);
    LOAD_FUNCPTR(glXQueryServerString);

    /* GLX 1.2 */
    LOAD_FUNCPTR(glXGetCurrentDisplay);

    /* GLX 1.3 */
    LOAD_FUNCPTR(glXCreatePbuffer);
    LOAD_FUNCPTR(glXCreateNewContext);
    LOAD_FUNCPTR(glXDestroyPbuffer);
    LOAD_FUNCPTR(glXMakeContextCurrent);
    LOAD_FUNCPTR(glXGetFBConfigs);
    LOAD_FUNCPTR(glXCreatePixmap);
    LOAD_FUNCPTR(glXDestroyPixmap);
    LOAD_FUNCPTR(glXCreateWindow);
    LOAD_FUNCPTR(glXDestroyWindow);
#undef LOAD_FUNCPTR

/* It doesn't matter if these fail. They'll only be used if the driver reports
   the associated extension is available (and if a driver reports the extension
   is available but fails to provide the functions, it's quite broken) */
#define LOAD_FUNCPTR(f) p##f = pglXGetProcAddressARB((const GLubyte *)#f)
    /* ARB GLX Extension */
    LOAD_FUNCPTR(glXCreateContextAttribsARB);
    /* EXT GLX Extension */
    LOAD_FUNCPTR(glXSwapIntervalEXT);
    /* MESA GLX Extension */
    LOAD_FUNCPTR(glXSwapIntervalMESA);
    /* SGI GLX Extension */
    LOAD_FUNCPTR(glXSwapIntervalSGI);
    /* NV GLX Extension */
    LOAD_FUNCPTR(glXAllocateMemoryNV);
    LOAD_FUNCPTR(glXFreeMemoryNV);
#undef LOAD_FUNCPTR

    if(!X11DRV_WineGL_InitOpenglInfo()) goto failed;

    if (XQueryExtension( gdi_display, "GLX", &glx_opcode, &event_base, &error_base ))
    {
        TRACE("GLX is up and running error_base = %d\n", error_base);
    } else {
        ERR( "GLX extension is missing, disabling OpenGL.\n" );
        goto failed;
    }

    /* In case of GLX you have direct and indirect rendering. Most of the time direct rendering is used
     * as in general only that is hardware accelerated. In some cases like in case of remote X indirect
     * rendering is used.
     *
     * The main problem for our OpenGL code is that we need certain GLX calls but their presence
     * depends on the reported GLX client / server version and on the client / server extension list.
     * Those don't have to be the same.
     *
     * In general the server GLX information lists the capabilities in case of indirect rendering.
     * When direct rendering is used, the OpenGL client library is responsible for which GLX calls are
     * available and in that case the client GLX informat can be used.
     * OpenGL programs should use the 'intersection' of both sets of information which is advertised
     * in the GLX version/extension list. When a program does this it works for certain for both
     * direct and indirect rendering.
     *
     * The problem we are having in this area is that ATI's Linux drivers are broken. For some reason
     * they haven't added some very important GLX extensions like GLX_SGIX_fbconfig to their client
     * extension list which causes this extension not to be listed. (Wine requires this extension).
     * ATI advertises a GLX client version of 1.3 which implies that this fbconfig extension among
     * pbuffers is around.
     *
     * In order to provide users of Ati's proprietary drivers with OpenGL support, we need to detect
     * the ATI drivers and from then on use GLX client information for them.
     */

    if(glxRequireVersion(3)) {
        pglXGetFBConfigAttrib = pglXGetProcAddressARB((const GLubyte *) "glXGetFBConfigAttrib");
        pglXGetVisualFromFBConfig = pglXGetProcAddressARB((const GLubyte *) "glXGetVisualFromFBConfig");
        pglXQueryDrawable = pglXGetProcAddressARB((const GLubyte *) "glXQueryDrawable");
    } else if (has_extension( glxExtensions, "GLX_SGIX_fbconfig")) {
        pglXGetFBConfigAttrib = pglXGetProcAddressARB((const GLubyte *) "glXGetFBConfigAttribSGIX");
        pglXGetVisualFromFBConfig = pglXGetProcAddressARB((const GLubyte *) "glXGetVisualFromFBConfigSGIX");

        /* The mesa libGL client library seems to forward glXQueryDrawable to the Xserver, so only
         * enable this function when the Xserver understand GLX 1.3 or newer
         */
        pglXQueryDrawable = NULL;
    } else if(strcmp("ATI", pglXGetClientString(gdi_display, GLX_VENDOR)) == 0) {
        TRACE("Overriding ATI GLX capabilities!\n");
        pglXGetFBConfigAttrib = pglXGetProcAddressARB((const GLubyte *) "glXGetFBConfigAttrib");
        pglXGetVisualFromFBConfig = pglXGetProcAddressARB((const GLubyte *) "glXGetVisualFromFBConfig");
        pglXQueryDrawable = pglXGetProcAddressARB((const GLubyte *) "glXQueryDrawable");

        /* Use client GLX information in case of the ATI drivers. We override the
         * capabilities over here and not somewhere else as ATI might better their
         * life in the future. In case they release proper drivers this block of
         * code won't be called. */
        glxExtensions = pglXGetClientString(gdi_display, GLX_EXTENSIONS);
    } else {
        ERR(" glx_version is %s and GLX_SGIX_fbconfig extension is unsupported. Expect problems.\n",
            pglXQueryServerString(gdi_display, DefaultScreen(gdi_display), GLX_VERSION));
    }

    if (has_extension( glxExtensions, "GLX_MESA_query_renderer" ))
    {
        pglXQueryCurrentRendererIntegerMESA = pglXGetProcAddressARB(
                (const GLubyte *)"glXQueryCurrentRendererIntegerMESA" );
        pglXQueryCurrentRendererStringMESA = pglXGetProcAddressARB(
                (const GLubyte *)"glXQueryCurrentRendererStringMESA" );
        pglXQueryRendererIntegerMESA = pglXGetProcAddressARB( (const GLubyte *)"glXQueryRendererIntegerMESA" );
        pglXQueryRendererStringMESA = pglXGetProcAddressARB( (const GLubyte *)"glXQueryRendererStringMESA" );
    }

    if (has_extension( glxExtensions, "GLX_OML_sync_control" ))
    {
        pglXGetSyncValuesOML = pglXGetProcAddressARB( (const GLubyte *)"glXGetSyncValuesOML" );
        pglXSwapBuffersMscOML = pglXGetProcAddressARB( (const GLubyte *)"glXSwapBuffersMscOML" );
    }

    *driver_funcs = &x11drv_driver_funcs;
    return STATUS_SUCCESS;

failed:
    dlclose(opengl_handle);
    opengl_handle = NULL;
    return STATUS_NOT_SUPPORTED;
}

static int get_render_type_from_fbconfig(Display *display, GLXFBConfig fbconfig)
{
    int render_type, render_type_bit;
    pglXGetFBConfigAttrib(display, fbconfig, GLX_RENDER_TYPE, &render_type_bit);
    switch(render_type_bit)
    {
        case GLX_RGBA_BIT:
            render_type = GLX_RGBA_TYPE;
            break;
        case GLX_COLOR_INDEX_BIT:
            render_type = GLX_COLOR_INDEX_TYPE;
            break;
        case GLX_RGBA_FLOAT_BIT:
            render_type = GLX_RGBA_FLOAT_TYPE;
            break;
        case GLX_RGBA_UNSIGNED_FLOAT_BIT_EXT:
            render_type = GLX_RGBA_UNSIGNED_FLOAT_TYPE_EXT;
            break;
        default:
            ERR("Unknown render_type: %x\n", render_type_bit);
            render_type = 0;
    }
    return render_type;
}

/* Check whether a fbconfig is suitable for Windows-style bitmap rendering */
static BOOL check_fbconfig_bitmap_capability( GLXFBConfig fbconfig, const XVisualInfo *vis )
{
    int dbuf, value;

    pglXGetFBConfigAttrib( gdi_display, fbconfig, GLX_BUFFER_SIZE, &value );
    if (vis && value != vis->depth) return FALSE;

    pglXGetFBConfigAttrib( gdi_display, fbconfig, GLX_DOUBLEBUFFER, &dbuf );
    pglXGetFBConfigAttrib(gdi_display, fbconfig, GLX_DRAWABLE_TYPE, &value);

    /* Windows only supports bitmap rendering on single buffered formats. The fbconfig also needs to
     * have the GLX_PBUFFER_BIT set, because Wine's implementation of bitmap rendering uses
     * pbuffers. */
    return !dbuf && (value & GLX_PBUFFER_BIT);
}

static UINT x11drv_init_pixel_formats( UINT *onscreen_count )
{
    struct glx_pixel_format *list;
    int size = 0, onscreen_size = 0;
    int fmt_id, nCfgs, i, run;
    GLXFBConfig* cfgs;
    XVisualInfo *visinfo;

    cfgs = pglXGetFBConfigs(gdi_display, DefaultScreen(gdi_display), &nCfgs);
    if (NULL == cfgs || 0 == nCfgs) {
        if(cfgs != NULL) XFree(cfgs);
        ERR("glXChooseFBConfig returns NULL\n");
        return 0;
    }

    list = calloc( 1, (nCfgs * 2) * sizeof(*list) );

    /* Fill the pixel format list. Put onscreen formats at the top and offscreen ones at the bottom.
     * Do this as GLX doesn't guarantee that the list is sorted */
    for(run=0; run < 2; run++)
    {
        for(i=0; i<nCfgs; i++) {
            pglXGetFBConfigAttrib(gdi_display, cfgs[i], GLX_FBCONFIG_ID, &fmt_id);
            visinfo = pglXGetVisualFromFBConfig(gdi_display, cfgs[i]);

            /* The first run we only add onscreen formats (ones which have an associated X Visual).
             * The second run we only set offscreen formats. */
            if(!run && visinfo)
            {
                TRACE("Found onscreen format FBCONFIG_ID 0x%x corresponding to iPixelFormat %d at GLX index %d\n", fmt_id, size+1, i);
                list[size].fbconfig = cfgs[i];
                list[size].visual = visinfo;
                list[size].fmt_id = fmt_id;
                list[size].render_type = get_render_type_from_fbconfig(gdi_display, cfgs[i]);
                list[size].dwFlags = 0;
                size++;
                onscreen_size++;

                /* Clone a format if it is bitmap capable for indirect rendering to bitmaps */
                if (check_fbconfig_bitmap_capability( cfgs[i], visinfo ))
                {
                    TRACE("Found bitmap capable format FBCONFIG_ID 0x%x corresponding to iPixelFormat %d at GLX index %d\n", fmt_id, size+1, i);
                    list[size].fbconfig = cfgs[i];
                    list[size].visual = visinfo;
                    list[size].fmt_id = fmt_id;
                    list[size].render_type = get_render_type_from_fbconfig(gdi_display, cfgs[i]);
                    list[size].dwFlags = PFD_DRAW_TO_BITMAP | PFD_SUPPORT_GDI | PFD_GENERIC_FORMAT;
                    size++;
                    onscreen_size++;
                }
            } else if(run && !visinfo) {
                int window_drawable=0;
                pglXGetFBConfigAttrib(gdi_display, cfgs[i], GLX_DRAWABLE_TYPE, &window_drawable);

                /* Recent Nvidia drivers and DRI drivers offer window drawable formats without a visual.
                 * This are formats like 16-bit rgb on a 24-bit desktop. In order to support these formats
                 * onscreen we would have to use glXCreateWindow instead of XCreateWindow. Further it will
                 * likely make our child window opengl rendering more complicated since likely you can't use
                 * XCopyArea on a GLX Window.
                 * For now ignore fbconfigs which are window drawable but lack a visual. */
                if(window_drawable & GLX_WINDOW_BIT)
                {
                    TRACE("Skipping FBCONFIG_ID 0x%x as an offscreen format because it is window_drawable\n", fmt_id);
                    continue;
                }

                TRACE("Found offscreen format FBCONFIG_ID 0x%x corresponding to iPixelFormat %d at GLX index %d\n", fmt_id, size+1, i);
                list[size].fbconfig = cfgs[i];
                list[size].fmt_id = fmt_id;
                list[size].render_type = get_render_type_from_fbconfig(gdi_display, cfgs[i]);
                if (!check_fbconfig_bitmap_capability( cfgs[i], NULL )) list[size].dwFlags = 0;
                else list[size].dwFlags = PFD_DRAW_TO_BITMAP | PFD_SUPPORT_GDI | PFD_GENERIC_FORMAT;
                size++;
            }
            else if (visinfo) XFree(visinfo);
        }
    }

    XFree(cfgs);

    pixel_formats = list;
    nb_pixel_formats = size;

    *onscreen_count = onscreen_size;
    return size;
}

static int glx_completion_error_handler( Display *display, XErrorEvent *event, void *arg )
{
    /* This scope owns the display lock and requests from a private resource
     * operation only. Direct-rendering drivers can also issue core X or DRI3
     * allocation requests, which the GLX-opcode-only handler would reject. */
    return 1;
}

static void destroy_glx_completion_objects( struct glx_completion_context *completion )
{
    /* The caller owns the query domain, or the drawable's final reference.
     * A failed creation can leave a client handle with no server resource.
     * Release it under a fresh error scope, including asynchronous errors. */
    if (!completion->context && !completion->pbuffer) return;
    pthread_mutex_lock( &glx_context_mutex );
    X11DRV_expect_error( gdi_display, glx_completion_error_handler, NULL );
    if (completion->context) pglXDestroyContext( gdi_display, completion->context );
    if (completion->pbuffer) pglXDestroyPbuffer( gdi_display, completion->pbuffer );
    XSync( gdi_display, False );
    if (X11DRV_check_error()) TRACE( "Failed to destroy GLX completion objects\n" );
    completion->context = NULL;
    completion->pbuffer = None;
    pthread_mutex_unlock( &glx_context_mutex );
}

static void x11drv_surface_destroy( struct opengl_drawable *base )
{
    struct gl_drawable *gl = impl_from_opengl_drawable( base );

    TRACE( "drawable %s\n", debugstr_opengl_drawable( base ) );

    if (gl->completion)
    {
        destroy_glx_completion_objects( gl->completion );
        pthread_mutex_destroy( &gl->completion->lock );
        free_client_surface_metadata( gl->completion, sizeof(*gl->completion) );
    }
    if (gl->drawable)
    {
        /* The last reference can belong to a completion worker. DRI3 teardown
         * sends raw XCB requests, whose socket-return callback takes the Xlib
         * display lock. Establish the same order as context creation before
         * entering the driver, just as for completion counter queries. */
        XLockDisplay( gdi_display );
        pglXDestroyWindow( gdi_display, gl->drawable );
        XUnlockDisplay( gdi_display );
    }
}

static BOOL set_swap_interval( struct gl_drawable *gl, int interval )
{
    BOOL ret = TRUE;

    if (interval < 0 && !has_swap_control_tear) interval = -interval;

    switch (swap_control_method)
    {
    case GLX_SWAP_CONTROL_EXT:
        X11DRV_expect_error(gdi_display, GLXErrorHandler, NULL);
        pglXSwapIntervalEXT( gdi_display, gl->drawable, interval );
        XSync(gdi_display, False);
        ret = !X11DRV_check_error();
        break;

    case GLX_SWAP_CONTROL_MESA:
        ret = !pglXSwapIntervalMESA(interval);
        break;

    case GLX_SWAP_CONTROL_SGI:
        /* wglSwapIntervalEXT considers an interval value of zero to mean that
         * vsync should be disabled, but glXSwapIntervalSGI considers such a
         * value to be an error. Just silently ignore the request for now.
         */
        if (!interval)
            WARN("Request to disable vertical sync is not handled\n");
        else
            ret = !pglXSwapIntervalSGI(interval);
        break;

    case GLX_SWAP_CONTROL_NONE:
        /* Unlikely to happen on modern GLX implementations */
        WARN("Request to adjust swap interval is not handled\n");
        break;
    }

    return ret;
}

static GLXContext create_glxcontext( int format, GLXContext share, const int *attribs )
{
    struct glx_pixel_format *fmt = glx_pixel_format_from_format( format );
    GLXContext ctx;

    if (attribs) ctx = pglXCreateContextAttribsARB( gdi_display, fmt->fbconfig, share, TRUE, attribs );
    else if (fmt->visual) ctx = pglXCreateContext( gdi_display, fmt->visual, share, TRUE );
    else ctx = pglXCreateNewContext( gdi_display, fmt->fbconfig, fmt->render_type, share, TRUE );

    return ctx;
}

static BOOL x11drv_surface_create( struct client_surface *client, int format, struct opengl_drawable **drawable )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    struct glx_pixel_format *fmt = glx_pixel_format_from_format( format );
    struct gl_drawable *gl;

    if (!(gl = opengl_drawable_create( sizeof(*gl), &x11drv_surface_funcs, format, client ))) return FALSE;
    if (pglXGetSyncValuesOML && pglXSwapBuffersMscOML)
    {
        struct glx_completion_context *completion = alloc_client_surface_metadata( sizeof(*completion) );

        if (completion && pthread_mutex_init( &completion->lock, NULL ))
        {
            free_client_surface_metadata( completion, sizeof(*completion) );
            completion = NULL;
        }
        /* No private query domain means using the shared completion monitor,
         * armed before the native swap, rather than inventing OML evidence. */
        gl->completion = completion;
    }
    gl->base.needs_framebuffer = !usexcomposite;
    if (!(gl->drawable = pglXCreateWindow( gdi_display, fmt->fbconfig, surface->window, NULL )))
    {
        opengl_drawable_release( &gl->base );
        return FALSE;
    }

    TRACE( "Created drawable %s with client window %lx\n", debugstr_opengl_drawable( &gl->base ), surface->window );
    XFlush( gdi_display );

    *drawable = &gl->base;
    return TRUE;
}

static BOOL x11drv_describe_pixel_format( int format, struct wgl_pixel_format *pf )
{
    int value, drawable_type = 0, render_type = 0;
    struct glx_pixel_format *fmt = glx_pixel_format_from_format( format );
    int rb, gb, bb, ab;

    /* If we can't get basic information, there is no point continuing */
    if (pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_DRAWABLE_TYPE, &drawable_type )) return 0;
    if (pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_RENDER_TYPE, &render_type )) return 0;

    memset( pf, 0, sizeof(*pf) );
    pf->pfd.nSize = sizeof(PIXELFORMATDESCRIPTOR);
    pf->pfd.nVersion = 1;

    /* These flags are always the same... */
    pf->pfd.dwFlags = PFD_SUPPORT_OPENGL;
    /* Now the flags extracted from the Visual */

    if (drawable_type & GLX_WINDOW_BIT) pf->pfd.dwFlags |= PFD_DRAW_TO_WINDOW;

    /* On Windows bitmap rendering is only offered using the GDI Software
     * renderer. We reserve some formats (see get_formats for more info) for
     * bitmap rendering since we require indirect rendering for this. Further
     * pixel format logs of a GeforceFX, Geforce8800GT, Radeon HD3400 and a
     * Radeon 9000 indicated that all bitmap formats have PFD_SUPPORT_GDI.
     * Except for 2 formats on the Radeon 9000 none of the hw accelerated
     * formats offered the GDI bit either. */
    pf->pfd.dwFlags |= fmt->dwFlags & (PFD_DRAW_TO_BITMAP | PFD_SUPPORT_GDI);

    /* PFD_GENERIC_FORMAT - gdi software rendering
     * PFD_GENERIC_ACCELERATED - some parts are accelerated by a display driver
     * (MCD e.g. 3dfx minigl) none set - full hardware accelerated by a ICD
     *
     * We only set PFD_GENERIC_FORMAT on bitmap formats (see get_formats) as
     * that's what ATI and Nvidia Windows drivers do  */
    pf->pfd.dwFlags |= fmt->dwFlags & (PFD_GENERIC_FORMAT | PFD_GENERIC_ACCELERATED);

    if (!(pf->pfd.dwFlags & PFD_GENERIC_FORMAT)) pf->pfd.dwFlags |= PFD_SUPPORT_COMPOSITION;

    pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_DOUBLEBUFFER, &value );
    if (value)
    {
        pf->pfd.dwFlags |= PFD_DOUBLEBUFFER;
        pf->pfd.dwFlags &= ~PFD_SUPPORT_GDI;
    }
    pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_STEREO, &value );
    if (value) pf->pfd.dwFlags |= PFD_STEREO;

    /* Pixel type */
    if (render_type & GLX_RGBA_BIT) pf->pfd.iPixelType = PFD_TYPE_RGBA;
    else pf->pfd.iPixelType = PFD_TYPE_COLORINDEX;

    /* Color bits */
    pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_BUFFER_SIZE, &value );
    pf->pfd.cColorBits = value;

    /* Red, green, blue and alpha bits / shifts */
    if (pf->pfd.iPixelType == PFD_TYPE_RGBA)
    {
        pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_RED_SIZE, &rb );
        pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_GREEN_SIZE, &gb );
        pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_BLUE_SIZE, &bb );
        pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_ALPHA_SIZE, &ab );

        pf->pfd.cBlueBits = bb;
        pf->pfd.cBlueShift = 0;
        pf->pfd.cGreenBits = gb;
        pf->pfd.cGreenShift = bb;
        pf->pfd.cRedBits = rb;
        pf->pfd.cRedShift = gb + bb;
        pf->pfd.cAlphaBits = ab;
        if (ab) pf->pfd.cAlphaShift = rb + gb + bb;
        else pf->pfd.cAlphaShift = 0;
    }
    else
    {
        pf->pfd.cRedBits = 0;
        pf->pfd.cRedShift = 0;
        pf->pfd.cBlueBits = 0;
        pf->pfd.cBlueShift = 0;
        pf->pfd.cGreenBits = 0;
        pf->pfd.cGreenShift = 0;
        pf->pfd.cAlphaBits = 0;
        pf->pfd.cAlphaShift = 0;
    }

    /* Accum RGBA bits */
    pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_ACCUM_RED_SIZE, &rb );
    pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_ACCUM_GREEN_SIZE, &gb );
    pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_ACCUM_BLUE_SIZE, &bb );
    pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_ACCUM_ALPHA_SIZE, &ab );

    pf->pfd.cAccumBits = rb + gb + bb + ab;
    pf->pfd.cAccumRedBits = rb;
    pf->pfd.cAccumGreenBits = gb;
    pf->pfd.cAccumBlueBits = bb;
    pf->pfd.cAccumAlphaBits = ab;

    /* Aux bits */
    pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_AUX_BUFFERS, &value );
    pf->pfd.cAuxBuffers = value;

    /* Depth bits */
    pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_DEPTH_SIZE, &value );
    pf->pfd.cDepthBits = value;

    /* stencil bits */
    pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_STENCIL_SIZE, &value );
    pf->pfd.cStencilBits = value;

    pf->pfd.iLayerType = PFD_MAIN_PLANE;

    if (!has_swap_method) pf->swap_method = WGL_SWAP_EXCHANGE_ARB;
    else if (!pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_SWAP_METHOD_OML, &value ))
    {
        switch (value) {
        case GLX_SWAP_EXCHANGE_OML: pf->swap_method = WGL_SWAP_EXCHANGE_ARB; break;
        case GLX_SWAP_COPY_OML: pf->swap_method = WGL_SWAP_COPY_ARB; break;
        case GLX_SWAP_UNDEFINED_OML: pf->swap_method = WGL_SWAP_UNDEFINED_ARB; break;
        default: { ERR( "Unexpected swap method %x.\n", value ); pf->swap_method = -1; break; }
        }
    }
    else pf->swap_method = -1;

    if (pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_TRANSPARENT_TYPE, &value) ) pf->transparent = -1;
    else pf->transparent = value != GLX_NONE;

    if (render_type & GLX_RGBA_BIT) pf->pixel_type = WGL_TYPE_RGBA_ARB;
    else if (render_type & GLX_COLOR_INDEX_BIT) pf->pixel_type = WGL_TYPE_COLORINDEX_ARB;
    else if (render_type & GLX_RGBA_FLOAT_BIT) pf->pixel_type = WGL_TYPE_RGBA_FLOAT_ATI;
    else if (render_type & GLX_RGBA_FLOAT_ATI_BIT) pf->pixel_type = WGL_TYPE_RGBA_FLOAT_ATI;
    else if (render_type & GLX_RGBA_UNSIGNED_FLOAT_BIT_EXT) pf->pixel_type = WGL_TYPE_RGBA_UNSIGNED_FLOAT_EXT;
    else { ERR( "unexpected RenderType(%x)\n", render_type ); pf->pixel_type = -1; }

    pf->draw_to_pbuffer = (drawable_type & GLX_PBUFFER_BIT) ? GL_TRUE : GL_FALSE;
    if (pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_MAX_PBUFFER_PIXELS, &value )) value = -1;
    pf->max_pbuffer_pixels = value;
    if (pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_MAX_PBUFFER_WIDTH, &value )) value = -1;
    pf->max_pbuffer_width = value;
    if (pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_MAX_PBUFFER_HEIGHT, &value )) value = -1;
    pf->max_pbuffer_height = value;

    if (!pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_TRANSPARENT_RED_VALUE, &value ))
    {
        pf->transparent_red_value_valid = GL_TRUE;
        pf->transparent_red_value = value;
    }
    if (!pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_TRANSPARENT_GREEN_VALUE, &value ))
    {
        pf->transparent_green_value_valid = GL_TRUE;
        pf->transparent_green_value = value;
    }
    if (!pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_TRANSPARENT_BLUE_VALUE, &value ))
    {
        pf->transparent_blue_value_valid = GL_TRUE;
        pf->transparent_blue_value = value;
    }
    if (!pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_TRANSPARENT_ALPHA_VALUE, &value ))
    {
        pf->transparent_alpha_value_valid = GL_TRUE;
        pf->transparent_alpha_value = value;
    }
    if (!pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_TRANSPARENT_INDEX_VALUE, &value ))
    {
        pf->transparent_index_value_valid = GL_TRUE;
        pf->transparent_index_value = value;
    }

    if (pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_SAMPLE_BUFFERS_ARB, &value )) value = -1;
    pf->sample_buffers = value;
    if (pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_SAMPLES_ARB, &value )) value = -1;
    pf->samples = value;

    if (pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_FRAMEBUFFER_SRGB_CAPABLE_EXT, &value )) value = -1;
    pf->framebuffer_srgb_capable = value;

    pf->bind_to_texture_rgb = pf->bind_to_texture_rgba = render_type != GLX_COLOR_INDEX_BIT && (drawable_type & GLX_PBUFFER_BIT);
    pf->bind_to_texture_rectangle_rgb = pf->bind_to_texture_rectangle_rgba = GL_FALSE;

    if (pglXGetFBConfigAttrib( gdi_display, fmt->fbconfig, GLX_FLOAT_COMPONENTS_NV, &value )) value = -1;
    pf->float_components = value;

    if (TRACE_ON(wgl)) dump_PIXELFORMATDESCRIPTOR( &pf->pfd );

    return TRUE;
}

/***********************************************************************
 *		glxdrv_wglDeleteContext
 */
static BOOL x11drv_context_destroy( void *context )
{
    TRACE("(%p)\n", context);
    pthread_mutex_lock( &glx_context_mutex );
    pglXDestroyContext( gdi_display, context );
    pthread_mutex_unlock( &glx_context_mutex );
    return TRUE;
}

static void *x11drv_get_proc_address( const char *name )
{
    void *ptr;
    if ((ptr = dlsym( opengl_handle, name ))) return ptr;
    return pglXGetProcAddressARB( (const GLubyte *)name );
}

static BOOL x11drv_make_current( struct opengl_drawable *draw_base, struct opengl_drawable *read_base, void *context )
{
    struct gl_drawable *draw = impl_from_opengl_drawable( draw_base ), *read = impl_from_opengl_drawable( read_base );
    BOOL ret;

    TRACE( "draw %s, read %s, context %p\n", debugstr_opengl_drawable( draw_base ), debugstr_opengl_drawable( read_base ), context );

    pthread_mutex_lock( &glx_context_mutex );
    if (!pglXMakeContextCurrent || !context) ret = pglXMakeCurrent( gdi_display, context ? draw->drawable : None, context );
    else ret = pglXMakeContextCurrent( gdi_display, draw->drawable, read->drawable, context );
    pthread_mutex_unlock( &glx_context_mutex );
    if (ret) NtCurrentTeb()->glReserved2 = context;
    return ret;
}

static BOOL snapshot_client_surface( struct opengl_drawable *base, struct client_surface_frame *present,
                                     GLuint source_framebuffer, GLenum source_buffer );

static void release_opengl_capture( struct client_surface_frame *present )
{
    if (present->capture.release) present->capture.release( present->capture.context );
    memset( &present->capture, 0, sizeof(present->capture) );
}

static BOOL complete_opengl_present( struct client_surface *client, struct client_surface_frame *present,
                                     BOOL submitted, BOOL completed, const SIZE *size, DWORD timeout )
{
    struct client_surface_capture capture = present->capture;
    BOOL ret;

    /* Native shared-monitor deferral has no private GL capture. Exact CPU
     * capture and synchronous GPU fallback keep their owner until completion
     * returns; the asynchronous path transfers it to the completion FIFO. */
    assert( !capture.release || present->completion.kind != CLIENT_SURFACE_COMPLETION_SHARED );
    ret = client_surface_complete_present( client, present, submitted, completed, size, timeout );
    if (capture.release) capture.release( capture.context );
    return ret;
}

static void x11drv_surface_flush( struct opengl_drawable *base, UINT flags )
{
    struct gl_drawable *gl = impl_from_opengl_drawable( base );
    struct client_surface_frame present;
    BOOL ready = TRUE;

    TRACE( "%s flags %#x\n", debugstr_opengl_drawable( base ), flags );

    if (flags & GL_FLUSH_INTERVAL) set_swap_interval( gl, base->interval );
    if (!(flags & GL_FLUSH_PRESENT)) return;

    if (!client_surface_prepare_present( base->client, &present, TRUE, FALSE )) return;
    client_surface_begin_present( base->client );
    /* Native completion remains required while PREPARING blocks publication. */
    if (present.completion.kind == CLIENT_SURFACE_COMPLETION_EXACT)
    {
        if (!usexcomposite) ready = snapshot_client_surface( base, &present, 0, GL_FRONT );
        else if (!(flags & GL_FLUSH_FINISHED)) funcs->p_glFinish();
        XFlush( gdi_display );
    }
    client_surface_submit_present( base->client, &present );
    complete_opengl_present( base->client, &present, ready, ready, NULL, 0 );
}

/***********************************************************************
 *		X11DRV_wglCreateContextAttribsARB
 */
static BOOL x11drv_context_create( int format, void *share, const int *attribList, void **context, BOOL *shared )
{
    int glx_attribs[16] = {0}, *pContextAttribList = glx_attribs;
    int err = 0;

    TRACE("(%d %p %p)\n", format, share, attribList);

    if (attribList)
    {
        /* attribList consists of pairs {token, value] terminated with 0 */
        while(attribList[0] != 0)
        {
            TRACE("%#x %#x\n", attribList[0], attribList[1]);
            switch(attribList[0])
            {
            case WGL_CONTEXT_MAJOR_VERSION_ARB:
                pContextAttribList[0] = GLX_CONTEXT_MAJOR_VERSION_ARB;
                pContextAttribList[1] = attribList[1];
                pContextAttribList += 2;
                break;
            case WGL_CONTEXT_MINOR_VERSION_ARB:
                pContextAttribList[0] = GLX_CONTEXT_MINOR_VERSION_ARB;
                pContextAttribList[1] = attribList[1];
                pContextAttribList += 2;
                break;
            case WGL_CONTEXT_LAYER_PLANE_ARB:
                break;
            case WGL_CONTEXT_FLAGS_ARB:
                pContextAttribList[0] = GLX_CONTEXT_FLAGS_ARB;
                pContextAttribList[1] = attribList[1];
                pContextAttribList += 2;
                break;
            case WGL_CONTEXT_OPENGL_NO_ERROR_ARB:
                pContextAttribList[0] = GLX_CONTEXT_OPENGL_NO_ERROR_ARB;
                pContextAttribList[1] = attribList[1];
                pContextAttribList += 2;
                break;
            case WGL_CONTEXT_PROFILE_MASK_ARB:
                pContextAttribList[0] = GLX_CONTEXT_PROFILE_MASK_ARB;
                pContextAttribList[1] = attribList[1];
                pContextAttribList += 2;
                break;
            default:
                ERR("Unhandled attribList pair: %#x %#x\n", attribList[0], attribList[1]);
            }
            attribList += 2;
        }
    }

    pthread_mutex_lock( &glx_context_mutex );
    X11DRV_expect_error(gdi_display, GLXErrorHandler, NULL);
    *context = create_glxcontext( format, share, attribList ? glx_attribs : NULL );
    XSync(gdi_display, False);
    err = X11DRV_check_error();
    pthread_mutex_unlock( &glx_context_mutex );
    if (err || !*context)
    {
        /* In the future we should convert the GLX error to a win32 one here if needed */
        WARN("Context creation failed (error %#x).\n", err);
        return FALSE;
    }

    TRACE( "-> %p\n", *context );
    return TRUE;
}

static BOOL x11drv_pbuffer_create( HDC hdc, int format, BOOL largest, GLenum texture_format, GLenum texture_target,
                                   GLint max_level, GLsizei *width, GLsizei *height, struct opengl_drawable **drawable )
{
    const struct glx_pixel_format *fmt = glx_pixel_format_from_format( format );
    int glx_attribs[7], count = 0;
    struct gl_drawable *gl;
    RECT rect;

    TRACE( "hdc %p, format %d, largest %u, texture_format %#x, texture_target %#x, max_level %#x, width %d, height %d, drawable %p\n",
           hdc, format, largest, texture_format, texture_target, max_level, *width, *height, drawable );

    glx_attribs[count++] = GLX_PBUFFER_WIDTH;
    glx_attribs[count++] = *width;
    glx_attribs[count++] = GLX_PBUFFER_HEIGHT;
    glx_attribs[count++] = *height;
    if (largest)
    {
        glx_attribs[count++] = GLX_LARGEST_PBUFFER;
        glx_attribs[count++] = 1;
    }
    glx_attribs[count++] = 0;

    if (!(gl = opengl_drawable_create( sizeof(*gl), &x11drv_pbuffer_funcs, format, NULL ))) return FALSE;

    gl->drawable = pglXCreatePbuffer( gdi_display, fmt->fbconfig, glx_attribs );
    TRACE( "new Pbuffer drawable as %p (%lx)\n", gl, gl->drawable );
    if (!gl->drawable)
    {
        opengl_drawable_release( &gl->base );
        return FALSE;
    }
    pglXQueryDrawable( gdi_display, gl->drawable, GLX_WIDTH, (unsigned int *)width );
    pglXQueryDrawable( gdi_display, gl->drawable, GLX_HEIGHT, (unsigned int *)height );
    SetRect( &rect, 0, 0, *width, *height );
    set_dc_drawable( hdc, gl->drawable, &rect, IncludeInferiors );

    *drawable = &gl->base;
    return TRUE;
}

static void x11drv_pbuffer_destroy( struct opengl_drawable *base )
{
    struct gl_drawable *gl = impl_from_opengl_drawable( base );

    TRACE( "drawable %s\n", debugstr_opengl_drawable( base ) );

    if (gl->drawable) pglXDestroyPbuffer( gdi_display, gl->drawable );
}

static BOOL x11drv_pbuffer_updated( HDC hdc, struct opengl_drawable *base, GLenum cube_face, GLint mipmap_level )
{
    return GL_TRUE;
}

static UINT x11drv_pbuffer_bind( HDC hdc, struct opengl_drawable *base, GLenum buffer )
{
    return -1; /* use default implementation */
}

static BOOL x11drv_null_surface_create( int format, struct opengl_drawable **drawable )
{
    const struct glx_pixel_format *fmt = glx_pixel_format_from_format( format );
    int glx_attribs[7], count = 0;
    struct gl_drawable *gl;

    glx_attribs[count++] = GLX_PBUFFER_WIDTH;
    glx_attribs[count++] = 1;
    glx_attribs[count++] = GLX_PBUFFER_HEIGHT;
    glx_attribs[count++] = 1;
    glx_attribs[count++] = 0;

    if (!(gl = opengl_drawable_create( sizeof(*gl), &x11drv_pbuffer_funcs, format, NULL ))) return FALSE;
    if (!(gl->drawable = pglXCreatePbuffer( gdi_display, fmt->fbconfig, glx_attribs )))
    {
        opengl_drawable_release( &gl->base );
        return FALSE;
    }

    *drawable = &gl->base;
    return TRUE;
}

static BOOL X11DRV_wglQueryCurrentRendererIntegerWINE( GLenum attribute, GLuint *value )
{
    return pglXQueryCurrentRendererIntegerMESA( attribute, value );
}

static const char *X11DRV_wglQueryCurrentRendererStringWINE( GLenum attribute )
{
    return pglXQueryCurrentRendererStringMESA( attribute );
}

static BOOL X11DRV_wglQueryRendererIntegerWINE( HDC dc, GLint renderer, GLenum attribute, GLuint *value )
{
    return pglXQueryRendererIntegerMESA( gdi_display, DefaultScreen(gdi_display), renderer, attribute, value );
}

static const char *X11DRV_wglQueryRendererStringWINE( HDC dc, GLint renderer, GLenum attribute )
{
    return pglXQueryRendererStringMESA( gdi_display, DefaultScreen(gdi_display), renderer, attribute );
}

/**
 * glxRequireVersion (internal)
 *
 * Check if the supported GLX version matches requiredVersion.
 */
static BOOL glxRequireVersion(int requiredVersion)
{
    /* Both requiredVersion and glXVersion[1] contains the minor GLX version */
    return (requiredVersion <= glxVersion[1]);
}

static void x11drv_init_extensions( struct opengl_funcs *funcs, BOOLEAN extensions[GL_EXTENSION_COUNT] )
{
    /* ARB Extensions */

    if (has_extension( glxExtensions, "GLX_ARB_multisample"))
        extensions[WGL_ARB_multisample] = 1;

    extensions[WGL_ARB_pixel_format] = 1;

    if (has_extension( glxExtensions, "GLX_ARB_fbconfig_float"))
    {
        extensions[WGL_ARB_pixel_format_float] = 1;
        extensions[WGL_ATI_pixel_format_float] = 1;
    }

    /* Support WGL_ARB_render_texture when there's support or pbuffer based emulation */
    if (has_extension( glxExtensions, "GLX_ARB_render_texture" ) || glxRequireVersion( 3 ))
    {
        /* The WGL version of GLX_NV_float_buffer requires render_texture */
        if (has_extension( glxExtensions, "GLX_NV_float_buffer"))
            extensions[WGL_NV_float_buffer] = 1;

        /* Again there's no GLX equivalent for this extension, so depend on the required GL extension */
        if (has_extension(glExtensions, "GL_NV_texture_rectangle"))
            extensions[WGL_NV_render_texture_rectangle] = 1;
    }

    /* EXT Extensions */

    if (has_extension( glxExtensions, "GLX_EXT_framebuffer_sRGB"))
        extensions[WGL_EXT_framebuffer_sRGB] = 1;

    if (has_extension( glxExtensions, "GLX_EXT_fbconfig_packed_float"))
        extensions[WGL_EXT_pixel_format_packed_float] = 1;

    if (has_extension( glxExtensions, "GLX_EXT_swap_control"))
    {
        swap_control_method = GLX_SWAP_CONTROL_EXT;
        has_swap_control_tear = has_extension( glxExtensions, "GLX_EXT_swap_control_tear" );
    }
    else if (has_extension( glxExtensions, "GLX_MESA_swap_control"))
    {
        swap_control_method = GLX_SWAP_CONTROL_MESA;
    }
    else if (has_extension( glxExtensions, "GLX_SGI_swap_control"))
    {
        swap_control_method = GLX_SWAP_CONTROL_SGI;
    }

    /* The OpenGL extension GL_NV_vertex_array_range adds wgl/glX functions which aren't exported as 'real' wgl/glX extensions. */
    if (has_extension(glExtensions, "GL_NV_vertex_array_range"))
    {
        extensions[WGL_NV_vertex_array_range] = 1;
        funcs->p_wglAllocateMemoryNV = pglXAllocateMemoryNV;
        funcs->p_wglFreeMemoryNV = pglXFreeMemoryNV;
    }

    if (has_extension(glxExtensions, "GLX_OML_swap_method"))
        has_swap_method = TRUE;

    /* WINE-specific WGL Extensions */

    if (has_extension( glxExtensions, "GLX_MESA_query_renderer" ))
    {
        extensions[WGL_WINE_query_renderer] = 1;
        funcs->p_wglQueryCurrentRendererIntegerWINE = X11DRV_wglQueryCurrentRendererIntegerWINE;
        funcs->p_wglQueryCurrentRendererStringWINE = X11DRV_wglQueryCurrentRendererStringWINE;
        funcs->p_wglQueryRendererIntegerWINE = X11DRV_wglQueryRendererIntegerWINE;
        funcs->p_wglQueryRendererStringWINE = X11DRV_wglQueryRendererStringWINE;
    }
}

static BOOL prepare_glx_completion_context( struct glx_completion_context *completion )
{
    static const int attribs[] = {GLX_PBUFFER_WIDTH, 1, GLX_PBUFFER_HEIGHT, 1, None};
    GLXFBConfig *configs, config = NULL;
    int count, i, drawable_type, render_type, error;

    if (completion->context) return TRUE;
    /* Queries address an explicit drawable. Their current context need not
     * use its format, so choose a pbuffer-capable RGBA config independently
     * of application window formats which may not support pbuffers. */
    XLockDisplay( gdi_display );
    configs = pglXGetFBConfigs( gdi_display, DefaultScreen( gdi_display ), &count );
    for (i = 0; configs && i < count; ++i)
    {
        if (pglXGetFBConfigAttrib( gdi_display, configs[i], GLX_DRAWABLE_TYPE, &drawable_type ) ||
            pglXGetFBConfigAttrib( gdi_display, configs[i], GLX_RENDER_TYPE, &render_type )) continue;
        if (!(drawable_type & GLX_PBUFFER_BIT) || !(render_type & GLX_RGBA_BIT)) continue;
        config = configs[i];
        break;
    }
    if (configs) XFree( configs );
    XUnlockDisplay( gdi_display );
    if (!config) return FALSE;

    pthread_mutex_lock( &glx_context_mutex );
    X11DRV_expect_error( gdi_display, glx_completion_error_handler, NULL );
    completion->context = pglXCreateNewContext( gdi_display, config, GLX_RGBA_TYPE, NULL, True );
    XSync( gdi_display, False );
    error = X11DRV_check_error();
    if (error || !completion->context) goto failed;

    X11DRV_expect_error( gdi_display, glx_completion_error_handler, NULL );
    completion->pbuffer = pglXCreatePbuffer( gdi_display, config, attribs );
    XSync( gdi_display, False );
    error = X11DRV_check_error();
    /* A failed server allocation can still return a client-side handle.
     * Cleanup must release that handle while accepting the missing XID. */
    if (error || !completion->pbuffer) goto failed;
    pthread_mutex_unlock( &glx_context_mutex );
    TRACE( "created GLX completion context %p pbuffer %lx\n", completion->context, completion->pbuffer );
    return TRUE;

failed:
    pthread_mutex_unlock( &glx_context_mutex );
    destroy_glx_completion_objects( completion );
    return FALSE;
}

static struct client_surface_completion_result query_glx_swap_serial( struct gl_drawable *gl, INT64 target_sbc )
{
    INT64 ust, msc, sbc;
    Bool ret;

    /* A direct-rendering GLX driver may send XCB requests. Take the display
     * lock first, matching X11DRV_expect_error(), or XCB's socket-return
     * callback can wait for a display lock held by another XCB sender.
     * This query has no native timeout; the core bounds only its retries. */
    XLockDisplay( gdi_display );
    ret = pglXGetSyncValuesOML( gdi_display, gl->drawable, &ust, &msc, &sbc );
    XUnlockDisplay( gdi_display );
    if (!ret) return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
    return client_surface_completion_result( sbc >= target_sbc ? CLIENT_SURFACE_COMPLETION_SIGNALED :
                                                               CLIENT_SURFACE_COMPLETION_PENDING );
}

static BOOL make_glx_completion_current( GLXDrawable drawable, GLXContext context, BOOL legacy )
{
    BOOL ret;
    int error;

    /* GLX may report a binding failure through an X error, including after
     * returning a client-side result. Do not extend this global error scope
     * across the completion query or its polling delay. Only an unbound
     * thread or our private, never-rendered pbuffer context enters here, so
     * this cannot flush an application's old window while holding Display. */
    pthread_mutex_lock( &glx_context_mutex );
    X11DRV_expect_error( gdi_display, glx_completion_error_handler, NULL );
    if (legacy) ret = pglXMakeCurrent( gdi_display, drawable, context );
    else ret = pglXMakeContextCurrent( gdi_display, drawable, drawable, context );
    XSync( gdi_display, False );
    error = X11DRV_check_error();
    pthread_mutex_unlock( &glx_context_mutex );
    return ret && !error && pglXGetCurrentContext() == context;
}

static struct client_surface_completion_result wait_glx_swap_serial( struct gl_drawable *gl, INT64 target_sbc )
{
    struct client_surface_completion_result result = client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
    struct glx_completion_context *completion = gl->completion;
    GLXContext current = pglXGetCurrentContext();
    BOOL bound, unbound;

    /* OML queries require a current GLX context. An inline caller already
     * owns its context on this thread; leave its binding and GL state alone.
     * A context from another display must not select a different GLX driver. */
    if (current)
    {
        /* Only x11drv_make_current() records application contexts in the TEB.
         * If both private unbind operations failed, deletion does not clear
         * the native current binding. Never mistake it for an inline caller. */
        if (current != NtCurrentTeb()->glReserved2)
        {
            WARN( "Refusing a non-Wine current GLX context %p during completion\n", current );
            result.worker = CLIENT_SURFACE_COMPLETION_WORKER_RETIRE;
            return result;
        }
        if (pglXGetCurrentDisplay() != gdi_display) return result;
        return query_glx_swap_serial( gl, target_sbc );
    }
    if (!completion) return result;

    /* The token retains gl through query and release. This domain belongs to
     * that native drawable, not a thread or the process; another worker may
     * use it after we unbind. Never make the producer's window current here. */
    pthread_mutex_lock( &completion->lock );
    if (!prepare_glx_completion_context( completion )) goto done;
    bound = make_glx_completion_current( completion->pbuffer, completion->context, FALSE );
    if (bound) result = query_glx_swap_serial( gl, target_sbc );
    /* Even a failed bind can have changed the client-side current context
     * before an asynchronous server error was delivered. Unwind that binding. */
    unbound = TRUE;
    if (pglXGetCurrentContext() == completion->context)
    {
        unbound = make_glx_completion_current( None, NULL, FALSE );
        if (!unbound)
        {
            WARN( "Failed to unbind GLX completion context %p\n", completion->context );
            if (pglXGetCurrentContext() == completion->context &&
                !make_glx_completion_current( None, NULL, TRUE ))
                ERR( "Failed to release the current GLX completion context\n" );
        }
    }
    if (!bound || !unbound)
    {
        /* GLX defers deletion of a current context until its thread releases
         * it. Do not let another worker attempt to bind that context again. */
        destroy_glx_completion_objects( completion );
        result.status = CLIENT_SURFACE_COMPLETION_FAILED;
    }
done:
    /* Destruction only marks a current context for deletion. The exact
     * native binding, not the unbind function's return alone, decides whether
     * this thread can execute another callback. Do not transfer that binding
     * to a replacement worker or terminate an inline application thread. */
    if (pglXGetCurrentContext())
    {
        result.status = CLIENT_SURFACE_COMPLETION_FAILED;
        result.worker = CLIENT_SURFACE_COMPLETION_WORKER_RETIRE;
    }
    pthread_mutex_unlock( &completion->lock );
    return result;
}

struct glx_present_completion
{
    struct opengl_drawable *drawable;
    INT64 target_sbc;
};

static struct client_surface_completion_result wait_glx_present_completion( void *context, DWORD timeout )
{
    struct glx_present_completion *completion = context;

    return wait_glx_swap_serial( impl_from_opengl_drawable( completion->drawable ),
                                 completion->target_sbc );
}

static void release_glx_present_completion( void *context )
{
    struct glx_present_completion *completion = context;

    opengl_drawable_release( completion->drawable );
    free_client_surface_metadata( completion, sizeof(*completion) );
}

static BOOL snapshot_client_surface( struct opengl_drawable *base, struct client_surface_frame *present,
                                     GLuint source_framebuffer, GLenum source_buffer )
{
    static const GLenum pack_params[] = {GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH,
                                        GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS};
    GLint pack_values[ARRAY_SIZE(pack_params)], framebuffer, buffer, read_buffer;
    struct x11drv_client_surface *surface = impl_from_client_surface( base->client );
    SIZE size = base->virtual_size;
    RECT source;
    BYTE *pixels;
    SIZE_T bytes;
    unsigned int i;

    if (present->target != CLIENT_SURFACE_FRAME_TARGET_OFFSCREEN &&
        present->completion.kind != CLIENT_SURFACE_COMPLETION_EXACT && !source_framebuffer) return TRUE;
    pthread_mutex_lock( &base->client->present_lock );
    source = base->client->target.virtual_rect;
    pthread_mutex_unlock( &base->client->present_lock );
    if (size.cx != source.right - source.left || size.cy != source.bottom - source.top)
    {
        /* Preparing the scene can observe a resize after the FBO was drawn.
         * Discard that old-size frame instead of advertising a larger source
         * than the snapshot actually contains and losing the owner binding. */
        TRACE( "Discarding resized snapshot %dx%d for %s\n", (int)size.cx, (int)size.cy,
               wine_dbgstr_rect( &source ) );
        present->target = CLIENT_SURFACE_FRAME_TARGET_INVALID;
        present->result = CLIENT_SURFACE_FRAME_SUPERSEDED;
        return TRUE;
    }
    if (size.cx <= 0 || size.cy <= 0 || (SIZE_T)size.cx > ~(SIZE_T)0 / 4 / size.cy)
        return FALSE;
    bytes = (SIZE_T)size.cx * size.cy * 4;
    if (surface->snapshot_pixels_size != bytes)
    {
        if (!client_surface_reserve_memory( CLIENT_SURFACE_MEMORY_STAGING, bytes )) return FALSE;
        if (!(pixels = malloc( bytes )))
        {
            client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING, bytes );
            return FALSE;
        }
        free( surface->snapshot_pixels );
        client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING, surface->snapshot_pixels_size );
        surface->snapshot_pixels = pixels;
        surface->snapshot_pixels_size = bytes;
    }
    pixels = surface->snapshot_pixels;

    /* Capture the completed GL buffer, not the clipped native window. This
     * exceptional path needs neither XDamage nor native swap completion. The
     * snapshot belongs to the producer and is reused only after owner release. */
    funcs->p_glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING, &framebuffer );
    funcs->p_glBindFramebuffer( GL_READ_FRAMEBUFFER, source_framebuffer );
    funcs->p_glGetIntegerv( GL_READ_BUFFER, &read_buffer );
    funcs->p_glReadBuffer( source_buffer );
    funcs->p_glGetIntegerv( GL_PIXEL_PACK_BUFFER_BINDING, &buffer );
    funcs->p_glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
    for (i = 0; i < ARRAY_SIZE(pack_params); ++i)
    {
        funcs->p_glGetIntegerv( pack_params[i], &pack_values[i] );
        funcs->p_glPixelStorei( pack_params[i], i ? 0 : 1 );
    }
    funcs->p_glReadPixels( 0, 0, size.cx, size.cy, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
    for (i = 0; i < ARRAY_SIZE(pack_params); ++i)
        funcs->p_glPixelStorei( pack_params[i], pack_values[i] );
    funcs->p_glBindBuffer( GL_PIXEL_PACK_BUFFER, buffer );
    funcs->p_glReadBuffer( read_buffer );
    funcs->p_glBindFramebuffer( GL_READ_FRAMEBUFFER, framebuffer );
    if (source_framebuffer && funcs->p_glGetError() != GL_NO_ERROR) return FALSE;

    return x11drv_client_surface_snapshot( base->client, present, pixels, size.cx, size.cy,
                                           FALSE, &x11drv_snapshot_rgba8 );
}

static BOOL blit_client_surface_output( struct opengl_drawable *base,
                                        struct client_surface_frame *present,
                                        struct opengl_drawable *source,
                                        opengl_drawable_blit_func blit, BOOL snapshot )
{
    struct client_surface_target target;
    SIZE size = source->virtual_size, destination = size;
    BOOL ret;

    pthread_mutex_lock( &base->client->present_lock );
    target = base->client->target;
    pthread_mutex_unlock( &base->client->present_lock );
    if (!target.valid || target.epoch != present->target_epoch ||
        size.cx != base->virtual_size.cx || size.cy != base->virtual_size.cy ||
        size.cx != target.virtual_rect.right - target.virtual_rect.left ||
        size.cy != target.virtual_rect.bottom - target.virtual_rect.top)
    {
        /* A concurrent resize supersedes this image, not the application's
         * native Swap result. Do not copy stale storage or authorize its ACK. */
        present->target = CLIENT_SURFACE_FRAME_TARGET_INVALID;
        present->result = CLIENT_SURFACE_FRAME_SUPERSEDED;
        return TRUE;
    }
    if (!snapshot && base->client->raw)
        destination = (SIZE){target.monitor_rect.right - target.monitor_rect.left,
                             target.monitor_rect.bottom - target.monitor_rect.top};

    /* Preparing this Present may attach DIRECT. Only then is it known whether
     * the resolved/gamma-corrected image feeds a virtual-sized private snapshot
     * or the native monitor-sized back buffer. No surface mutex spans GL or WSI. */
    ret = blit( source, &destination );
    TRACE( "generic framebuffer presentation for %s snapshot %u source %dx%d destination %dx%d returned %u\n",
           debugstr_opengl_drawable( base ), snapshot, (int)size.cx, (int)size.cy,
           (int)destination.cx, (int)destination.cy, ret );
    return ret;
}

static BOOL x11drv_surface_swap_blit( struct opengl_drawable *base, struct opengl_drawable *source,
                                      opengl_drawable_blit_func blit )
{
    struct glx_present_completion *completion = NULL;
    GLXContext ctx = NtCurrentTeb()->glReserved2;
    struct gl_drawable *gl = impl_from_opengl_drawable( base );
    struct client_surface_frame present;
    BOOL completed = FALSE, submitted = TRUE, use_oml;
    INT64 target_sbc = 0;
    SIZE size = source ? source->virtual_size : base->virtual_size;
    const SIZE *expected_size = source ? &size : NULL;

    TRACE( "drawable %s\n", debugstr_opengl_drawable( base ) );

    use_oml = ctx && gl->completion;
    if (!client_surface_prepare_present( base->client, &present, use_oml || !usexcomposite,
                                         usexcomposite )) return FALSE;
    client_surface_begin_present( base->client );
    if (usexcomposite && present.completion.kind == CLIENT_SURFACE_COMPLETION_EXACT &&
        !(completion = alloc_client_surface_metadata( sizeof(*completion) )))
    {
        client_surface_submit_present( base->client, &present );
        complete_opengl_present( base->client, &present, FALSE, FALSE, expected_size, 0 );
        RtlSetLastWin32Error( ERROR_NOT_ENOUGH_MEMORY );
        return FALSE;
    }
    if (blit && !blit_client_surface_output( base, &present, source, blit,
                   !usexcomposite && present.completion.kind == CLIENT_SURFACE_COMPLETION_EXACT ))
    {
        free_client_surface_metadata( completion, sizeof(*completion) );
        client_surface_submit_present( base->client, &present );
        complete_opengl_present( base->client, &present, FALSE, FALSE, expected_size, 0 );
        return FALSE;
    }
    /* A native offscreen target has an exact token even while the owner scene
     * is preparing. Capture the application's first image before any swap. */
    if (!usexcomposite && present.completion.kind == CLIENT_SURFACE_COMPLETION_EXACT)
    {
        submitted = completed = snapshot_client_surface( base, &present, 0,
                                                          base->doublebuffer ? GL_BACK : GL_FRONT );
        /* The FBO owns front/back storage and the owner publishes the copied
         * source. A native swap on this hidden scratch window adds no pixels
         * and can leave glFinish waiting for an unviewable DRI3 presentation. */
        client_surface_submit_present( base->client, &present );
    }
    else if (present.completion.kind == CLIENT_SURFACE_COMPLETION_EXACT)
    {
        /* The swap buffer count identifies this exact GLX presentation, unlike
         * unrelated XDamage on the drawable. The polling deadline bounds retries,
         * but cannot bound the native query itself. */
        funcs->p_glFlush();
        target_sbc = pglXSwapBuffersMscOML( gdi_display, gl->drawable, 0, 0, 0 );
        submitted = target_sbc >= 0;
        if (blit) TRACE( "native GLX generic framebuffer presentation for %s SBC %s submitted %u\n",
                         debugstr_opengl_drawable( base ), wine_dbgstr_longlong( target_sbc ), submitted );
        client_surface_submit_present( base->client, &present );
        if (submitted)
        {
            completion->drawable = base;
            completion->target_sbc = target_sbc;
            opengl_drawable_add_ref( base );
            client_surface_set_present_completion( &present, wait_glx_present_completion,
                                                   release_glx_present_completion, completion );
            client_surface_defer_present( base->client, &present, expected_size );
            return TRUE;
        }
    }
    else
    {
        pglXSwapBuffers( gdi_display, gl->drawable );
        if (blit) TRACE( "native GLX generic framebuffer presentation for %s submitted\n",
                         debugstr_opengl_drawable( base ) );
        client_surface_submit_present( base->client, &present );
    }

    free_client_surface_metadata( completion, sizeof(*completion) );
    if (!complete_opengl_present( base->client, &present, submitted, completed, expected_size,
                                          CLIENT_SURFACE_PRESENT_TIMEOUT ))
        WARN( "client-surface composition did not complete for %s\n",
              debugstr_opengl_drawable( base ) );
    return submitted;
}

static BOOL x11drv_surface_swap( struct opengl_drawable *base )
{
    return x11drv_surface_swap_blit( base, NULL, NULL );
}

struct egl_snapshot_completion
{
    LONG refs;
    EGLSyncKHR sync;
    /* Only the queued completion owns the drawable and mapping references.
     * Image retirement must test the fence without dereferencing these. */
    struct opengl_drawable *drawable;
    const struct client_surface_source *source;
    const struct client_surface_handoff_channel *channel;
    UINT64 control;
};

struct egl_snapshot_image
{
    EGLImageKHR image;
    struct egl_snapshot_completion *pending;
};

static void release_snapshot_sync( struct egl_snapshot_completion *completion )
{
    if (!completion || InterlockedDecrement( &completion->refs )) return;
    snapshot_destroy_sync( egl->display, completion->sync );
    free_client_surface_metadata( completion, sizeof(*completion) );
}

static struct client_surface_completion_result wait_snapshot_completion( void *context, DWORD timeout )
{
    struct egl_snapshot_completion *completion = context;
    EGLint result;

    /* The completion token keeps this mapping alive. A revoked handoff
     * cannot publish the write, even if its GPU fence eventually signals.
     * Retire its callback promptly while the image retains the real fence. */
    if (__atomic_load_n( &completion->source->reservation, __ATOMIC_ACQUIRE ) != completion->control ||
        __atomic_load_n( &completion->channel->closed, __ATOMIC_ACQUIRE ))
    {
        TRACE( "cancelled EGL source completion for control %s\n",
               wine_dbgstr_longlong( completion->control ) );
        return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
    }
    result = snapshot_wait_sync( egl->display, completion->sync, 0, (EGLTimeKHR)timeout * 1000000 );
    if (result == EGL_CONDITION_SATISFIED_KHR)
        return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_SIGNALED );
    if (result == EGL_TIMEOUT_EXPIRED_KHR)
        return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_PENDING );
    return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
}

static void release_snapshot_completion( void *context )
{
    struct egl_snapshot_completion *completion = context;
    struct opengl_drawable *drawable = completion->drawable;

    release_snapshot_sync( completion );
    opengl_drawable_release( drawable );
}

static BOOL snapshot_image_ready( void *context )
{
    struct egl_snapshot_image *image = context;

    return !image->pending || snapshot_wait_sync( egl->display, image->pending->sync, 0, 0 ) ==
                             EGL_CONDITION_SATISFIED_KHR;
}

static void release_snapshot_image( void *context )
{
    struct egl_snapshot_image *image = context;

    snapshot_destroy_image( egl->display, image->image );
    release_snapshot_sync( image->pending );
    free_client_surface_metadata( image, sizeof(*image) );
}

static const struct x11drv_client_snapshot_image_ops snapshot_image_ops =
{
    snapshot_image_ready,
    release_snapshot_image,
};

static struct egl_snapshot_completion *prepare_snapshot_completion( struct egl_snapshot_image *image )
{
    struct egl_snapshot_completion *completion = image->pending;

    /* Storage preparation proved sole image ownership and GPU completion.
     * Once its callback has also released its reference, the image's existing
     * metadata can carry another write without competing for process budget.
     * Neither a pending fence nor another callback's references are reused. */
    if (!completion || InterlockedCompareExchange( &completion->refs, 0, 0 ) != 1)
        return alloc_client_surface_metadata( sizeof(*completion) );
    image->pending = NULL;
    snapshot_destroy_sync( egl->display, completion->sync );
    memset( completion, 0, sizeof(*completion) );
    return completion;
}

/* Runs in the FBO wrapper's internal context, after its color/gamma blit.
 * Return zero for an unsupported import, negative for a failed GPU copy. */
static int snapshot_client_surface_gpu( struct opengl_drawable *base,
                                        struct client_surface_frame *present, GLuint source_framebuffer )
{
    static const EGLint attribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    struct x11drv_client_snapshot *snapshot;
    struct egl_snapshot_completion *completion = NULL;
    struct egl_snapshot_image *image;
    struct x11drv_client_surface *surface = impl_from_client_surface( base->client );
    struct client_surface_source *source = present->handoff_source;
    GLint read_fbo, draw_fbo, read_buffer, renderbuffer;
    GLuint fbo = 0, buffer = 0;
    GLboolean scissor, srgb;
    GLenum status, error;
    Pixmap pixmap;
    int ret = 0;
    unsigned long long begin = TRACE_ON(csperf) ? client_surface_perf_time() : 0;
    unsigned long long imported, blit = 0, copied = 0, flushed = 0;

    if (source->width != base->virtual_size.cx || source->height != base->virtual_size.cy)
    {
        present->target = CLIENT_SURFACE_FRAME_TARGET_INVALID;
        present->result = CLIENT_SURFACE_FRAME_SUPERSEDED;
        return 1;
    }
    if (!(snapshot = x11drv_client_surface_prepare_gpu_snapshot( base->client, present ))) return -1;
    pixmap = x11drv_client_snapshot_pixmap( snapshot );
    if (!(image = x11drv_client_snapshot_get_image( snapshot )))
    {
        if (!(image = alloc_client_surface_metadata( sizeof(*image) ))) return -1;
        image->image = snapshot_create_image( egl->display, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR,
                                               (EGLClientBuffer)pixmap, attribs );
        if (!image->image)
        {
            TRACE( "EGL source pixmap import unavailable, error %#x\n", funcs->p_eglGetError() );
            free_client_surface_metadata( image, sizeof(*image) );
            release_opengl_capture( present );
            return 0;
        }
        x11drv_client_snapshot_set_image( snapshot, image, &snapshot_image_ops );
        TRACE( "imported EGL source pixmap %#lx size %ux%u from visual %#lx to %#lx\n",
               pixmap, source->width, source->height, surface->source_visual, default_visual.visualid );
    }
    imported = begin ? client_surface_perf_time() : 0;
    if (funcs->p_glGetError() != GL_NO_ERROR) return -1;
    /* Reserve completion ownership before issuing the private GPU write.
     * Metadata pressure must not turn an otherwise asynchronous capture
     * into a caller-side glFinish after the copy has already been submitted. */
    if (snapshot_create_sync && snapshot_destroy_sync && snapshot_wait_sync &&
        !(completion = prepare_snapshot_completion( image ))) return -1;
    funcs->p_glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING, &read_fbo );
    funcs->p_glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo );
    funcs->p_glGetIntegerv( GL_RENDERBUFFER_BINDING, &renderbuffer );
    scissor = funcs->p_glIsEnabled( GL_SCISSOR_TEST );
    srgb = funcs->p_glIsEnabled( GL_FRAMEBUFFER_SRGB );
    funcs->p_glBindFramebuffer( GL_READ_FRAMEBUFFER, source_framebuffer );
    funcs->p_glGetIntegerv( GL_READ_BUFFER, &read_buffer );
    funcs->p_glGenRenderbuffers( 1, &buffer );
    funcs->p_glBindRenderbuffer( GL_RENDERBUFFER, buffer );
    snapshot_bind_image( GL_RENDERBUFFER, image->image );
    funcs->p_glGenFramebuffers( 1, &fbo );
    funcs->p_glBindFramebuffer( GL_DRAW_FRAMEBUFFER, fbo );
    funcs->p_glFramebufferRenderbuffer( GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, buffer );
    status = funcs->p_glCheckFramebufferStatus( GL_DRAW_FRAMEBUFFER );
    error = funcs->p_glGetError();
    if (status != GL_FRAMEBUFFER_COMPLETE || error) goto done;

    funcs->p_glReadBuffer( source_framebuffer ? GL_COLOR_ATTACHMENT0 : GL_BACK );
    funcs->p_glDrawBuffer( GL_COLOR_ATTACHMENT0 );
    funcs->p_glDisable( GL_SCISSOR_TEST );
    funcs->p_glDisable( GL_FRAMEBUFFER_SRGB );
    /* Native X pixmap coordinates have their origin at the top left. */
    blit = begin ? client_surface_perf_time() : 0;
    funcs->p_glBlitFramebuffer( 0, 0, source->width, source->height, 0, source->height, source->width, 0,
                                GL_COLOR_BUFFER_BIT, GL_NEAREST );
    ret = funcs->p_glGetError() == GL_NO_ERROR ? 1 : -1;
    copied = begin ? client_surface_perf_time() : 0;
    /* Keep the source's fence reference even if the common completion times
     * out or discards a stale scene. Storage preparation refuses reuse until
     * this exact GPU write completes. The worker owns a separate reference. */
    if (ret > 0 && completion)
    {
        completion->sync = snapshot_create_sync( egl->display, EGL_SYNC_FENCE_KHR, NULL );
        if (completion->sync)
        {
            funcs->p_glFlush();
            completion->refs = 2;
            completion->drawable = base;
            completion->source = present->handoff_source;
            completion->channel = present->handoff_channel;
            completion->control = present->handoff_control;
            opengl_drawable_add_ref( base );
            release_snapshot_sync( image->pending );
            image->pending = completion;
            client_surface_set_present_completion( present, wait_snapshot_completion,
                                                     release_snapshot_completion, completion );
            completion = NULL; /* The image and queued callback own both references. */
        }
    }
    if (!present->completion.wait)
    {
        funcs->p_glFinish();
        if (funcs->p_glGetError() != GL_NO_ERROR) ret = -1;
    }
    flushed = begin ? client_surface_perf_time() : 0;
    if (ret > 0)
    {
        /* A first PREPARING frame may have needed a CPU snapshot. Its upload
         * already completed XSync, and no completion borrows these staging
         * buffers. Native-present serialization excludes another writer.
         * Keep the pixmap for replay, but stop retaining CPU storage once
         * independent GPU capture works. A later fallback reallocates it. */
        pthread_mutex_lock( &base->client->present_lock );
        if (surface->snapshot_pixels || surface->snapshot)
            x11drv_client_surface_release_snapshot_staging( surface );
        pthread_mutex_unlock( &base->client->present_lock );
        source->source = pixmap;
        present->capture.size = (SIZE){source->width, source->height};
    }
done:
    free_client_surface_metadata( completion, sizeof(*completion) );
    if (scissor) funcs->p_glEnable( GL_SCISSOR_TEST );
    if (srgb) funcs->p_glEnable( GL_FRAMEBUFFER_SRGB );
    funcs->p_glBindRenderbuffer( GL_RENDERBUFFER, renderbuffer );
    funcs->p_glReadBuffer( read_buffer );
    funcs->p_glBindFramebuffer( GL_READ_FRAMEBUFFER, read_fbo );
    funcs->p_glBindFramebuffer( GL_DRAW_FRAMEBUFFER, draw_fbo );
    if (fbo) funcs->p_glDeleteFramebuffers( 1, &fbo );
    if (buffer) funcs->p_glDeleteRenderbuffers( 1, &buffer );
    /* Host submission spans plus the worker's actual fence wait distinguish
     * queueing from native readiness; they are not GPU execution timestamps. */
    TRACE_(csperf)( "ticks=%llu event=gpu_snapshot identity=%s control=%s target_epoch=%s "
                   "begin=%llu imported=%llu blit=%llu copied=%llu flushed=%llu "
                   "width=%u height=%u pixmap=%lx fence=%u result=%d\n", client_surface_perf_time(),
                   wine_dbgstr_longlong( __atomic_load_n( &base->client->identity, __ATOMIC_ACQUIRE ) ),
                   wine_dbgstr_longlong( present->handoff_control ), wine_dbgstr_longlong( present->target_epoch ),
                   begin, imported, blit, copied, flushed, source->width, source->height,
                   pixmap, !!present->completion.wait, ret );
    if (!ret) release_opengl_capture( present );
    return ret;
}

static void x11drv_egl_surface_destroy( struct opengl_drawable *base )
{
    TRACE( "drawable %s\n", debugstr_opengl_drawable( base ) );
}

static void x11drv_egl_surface_flush( struct opengl_drawable *base, UINT flags )
{
    struct client_surface_frame present;
    BOOL ready = TRUE;

    TRACE( "%s flags %#x\n", debugstr_opengl_drawable( base ), flags );

    if (flags & GL_FLUSH_INTERVAL) funcs->p_eglSwapInterval( egl->display, abs( base->interval ) );
    if (flags & GL_FLUSH_UPDATED)
    {
        EGLint width, height;

        /* Mesa's X11 platform updates the native drawable geometry and
         * invalidates its old buffers while querying the EGL surface size.
         * Do that before the application's next GL command can render into
         * buffers which still have the previous X window dimensions. */
        if (!funcs->p_eglQuerySurface( egl->display, base->surface, EGL_WIDTH, &width ) ||
            !funcs->p_eglQuerySurface( egl->display, base->surface, EGL_HEIGHT, &height ))
            ERR( "Failed to refresh resized EGL surface %s\n", debugstr_opengl_drawable( base ) );
        else
            TRACE( "Refreshed EGL surface %s to %dx%d\n",
                   debugstr_opengl_drawable( base ), width, height );
    }
    if (!(flags & GL_FLUSH_PRESENT)) return;

    if (!client_surface_prepare_present( base->client, &present, TRUE, FALSE )) return;
    client_surface_begin_present( base->client );
    /* Native completion remains required while PREPARING blocks publication. */
    if (present.completion.kind == CLIENT_SURFACE_COMPLETION_EXACT)
    {
        if (!usexcomposite) ready = snapshot_client_surface( base, &present, 0, GL_BACK );
        else if (!(flags & GL_FLUSH_FINISHED)) funcs->p_glFinish();
        XFlush( gdi_display );
    }

    client_surface_submit_present( base->client, &present );
    complete_opengl_present( base->client, &present, ready, ready, NULL, 0 );
}

struct egl_present_completion
{
    struct opengl_drawable *drawable;
    EGLuint64KHR frame_id;
};

static struct client_surface_completion_result wait_egl_present_completion( void *context, DWORD timeout )
{
    struct egl_present_completion *completion = context;
    struct opengl_drawable *base = completion->drawable;
    EGLint timestamp_name = EGL_DISPLAY_PRESENT_TIME_ANDROID;
    EGLnsecsANDROID timestamp = EGL_TIMESTAMP_PENDING_ANDROID;

    if (!funcs->p_eglGetFrameTimestampsANDROID( egl->display, base->surface,
                                               completion->frame_id, 1, &timestamp_name, &timestamp ))
        return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
    if (timestamp == EGL_TIMESTAMP_PENDING_ANDROID)
        return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_PENDING );
    return client_surface_completion_result( timestamp != EGL_TIMESTAMP_INVALID_ANDROID ?
        CLIENT_SURFACE_COMPLETION_SIGNALED : CLIENT_SURFACE_COMPLETION_FAILED );
}

static void release_egl_present_completion( void *context )
{
    struct egl_present_completion *completion = context;

    opengl_drawable_release( completion->drawable );
    free_client_surface_metadata( completion, sizeof(*completion) );
}

static BOOL blit_client_surface_framebuffer( struct opengl_drawable *base,
                                             const struct client_surface_frame *present, GLuint framebuffer )
{
    struct client_surface_target target;
    GLint read_fbo, draw_fbo, read_buffer, draw_buffer;
    SIZE size = base->virtual_size, destination;
    GLboolean scissor, srgb;
    BOOL ret;

    pthread_mutex_lock( &base->client->present_lock );
    target = base->client->target;
    pthread_mutex_unlock( &base->client->present_lock );
    if (base->client->raw)
        destination = (SIZE){target.monitor_rect.right - target.monitor_rect.left,
                             target.monitor_rect.bottom - target.monitor_rect.top};
    else destination = size;
    if (!target.valid || target.offscreen || target.epoch != present->target_epoch ||
        size.cx != target.virtual_rect.right - target.virtual_rect.left ||
        size.cy != target.virtual_rect.bottom - target.virtual_rect.top)
        return FALSE;

    /* swap_framebuffer runs in the wrapper's internal context with resolved
     * COLOR_ATTACHMENT0 and default gamma. DIRECT may have been selected
     * while preparing this frame; its pixels must reach the native back
     * buffer before eglSwapBuffers can authorize the owner publication. */
    if (funcs->p_glGetError() != GL_NO_ERROR) return FALSE;
    funcs->p_glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING, &read_fbo );
    funcs->p_glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo );
    scissor = funcs->p_glIsEnabled( GL_SCISSOR_TEST );
    srgb = funcs->p_glIsEnabled( GL_FRAMEBUFFER_SRGB );
    funcs->p_glBindFramebuffer( GL_READ_FRAMEBUFFER, framebuffer );
    funcs->p_glGetIntegerv( GL_READ_BUFFER, &read_buffer );
    funcs->p_glBindFramebuffer( GL_DRAW_FRAMEBUFFER, 0 );
    funcs->p_glGetIntegerv( GL_DRAW_BUFFER, &draw_buffer );
    funcs->p_glReadBuffer( GL_COLOR_ATTACHMENT0 );
    funcs->p_glDrawBuffer( base->buffer_map[(base->doublebuffer ? GL_BACK : GL_FRONT) - GL_FRONT_LEFT] );
    funcs->p_glDisable( GL_SCISSOR_TEST );
    if (base->srgb) funcs->p_glEnable( GL_FRAMEBUFFER_SRGB );
    else funcs->p_glDisable( GL_FRAMEBUFFER_SRGB );
    funcs->p_glBlitFramebuffer( 0, 0, size.cx, size.cy, 0, 0, destination.cx, destination.cy,
                                GL_COLOR_BUFFER_BIT, GL_LINEAR );
    ret = funcs->p_glGetError() == GL_NO_ERROR;
    if (scissor) funcs->p_glEnable( GL_SCISSOR_TEST );
    if (srgb) funcs->p_glEnable( GL_FRAMEBUFFER_SRGB );
    else funcs->p_glDisable( GL_FRAMEBUFFER_SRGB );
    funcs->p_glReadBuffer( read_buffer );
    funcs->p_glDrawBuffer( draw_buffer );
    funcs->p_glBindFramebuffer( GL_READ_FRAMEBUFFER, read_fbo );
    funcs->p_glBindFramebuffer( GL_DRAW_FRAMEBUFFER, draw_fbo );
    return ret;
}

static BOOL refresh_prepared_egl_surface( struct opengl_drawable *base,
                                          const struct client_surface_frame *present )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( base->client );
    struct gl_drawable *gl = impl_from_opengl_drawable( base );
    struct client_surface_target target;
    EGLint width, height;
    BOOL queried, current;
    SIZE size;

    pthread_mutex_lock( &base->client->present_lock );
    target = base->client->target;
    size = (SIZE){surface->changes.width, surface->changes.height};
    pthread_mutex_unlock( &base->client->present_lock );
    /* The existing blit/capture checks discard stale targets. Never cache a
     * native observation for one of those frames. */
    if (!target.valid || target.epoch != present->target_epoch) return TRUE;
    if (ReadAcquire64( &gl->egl_geometry_epoch ) == target.epoch) return TRUE;

    /* The internal context was made current before prepare_present could
     * resize a private scratch window or attach DIRECT. A logical rectangle
     * need not change, so GL_FLUSH_UPDATED alone cannot invalidate EGL's old
     * default buffers. Query after the native geometry update and before any
     * blit. This observes the existing target epoch, not a new lifetime. */
    queried = funcs->p_eglQuerySurface( egl->display, base->surface, EGL_WIDTH, &width ) &&
              funcs->p_eglQuerySurface( egl->display, base->surface, EGL_HEIGHT, &height );
    pthread_mutex_lock( &base->client->present_lock );
    current = base->client->target.valid && base->client->target.epoch == target.epoch;
    pthread_mutex_unlock( &base->client->present_lock );
    /* A concurrent resize can make the query observe a newer native target.
     * Leave that frame to the existing SUPERSEDED checks without changing
     * its native swap result or caching this obsolete observation. */
    if (!current) return TRUE;
    if (!queried)
    {
        ERR( "Failed to refresh prepared EGL surface %s\n", debugstr_opengl_drawable( base ) );
        return FALSE;
    }
    if (width != size.cx || height != size.cy)
    {
        WARN( "Prepared EGL surface %s is %dx%d, expected %dx%d\n", debugstr_opengl_drawable( base ),
              width, height, (int)size.cx, (int)size.cy );
        return FALSE;
    }
    WriteRelease64( &gl->egl_geometry_epoch, target.epoch );
    TRACE( "Refreshed prepared EGL surface %s target %s to %dx%d\n",
           debugstr_opengl_drawable( base ), wine_dbgstr_longlong( target.epoch ), width, height );
    return TRUE;
}

static BOOL x11drv_egl_surface_present( struct opengl_drawable *base, GLuint framebuffer,
                                       struct opengl_drawable *source, opengl_drawable_blit_func blit )
{
    struct egl_present_completion *completion = NULL;
    struct gl_drawable *gl = impl_from_opengl_drawable( base );
    struct x11drv_client_surface *surface = impl_from_client_surface( base->client );
    struct client_surface_frame present;
    EGLuint64KHR frame_id = 0;
    BOOL timestamp_completion;
    EGLint err;
    EGLBoolean ret;
    SIZE size = source ? source->virtual_size : base->virtual_size;
    const SIZE *expected_size = source || framebuffer ? &size : NULL;

    TRACE( "%s\n", debugstr_opengl_drawable( base ) );

    timestamp_completion = !framebuffer && egl->has_EGL_ANDROID_get_frame_timestamps &&
        funcs->p_eglGetFrameTimestampSupportedANDROID( egl->display, gl->base.surface,
                                                       EGL_DISPLAY_PRESENT_TIME_ANDROID );
    if (!client_surface_prepare_present( base->client, &present,
                                         timestamp_completion || !usexcomposite || surface->direct_snapshot,
                                         TRUE )) return FALSE;
    if (usexcomposite && !surface->direct_snapshot && present.completion.kind == CLIENT_SURFACE_COMPLETION_EXACT &&
        !funcs->p_eglGetNextFrameIdANDROID( egl->display, gl->base.surface, &frame_id ))
    {
        WARN( "Failed to allocate EGL presentation frame ID for %s\n",
              debugstr_opengl_drawable( base ) );
        frame_id = 0;
    }
    client_surface_begin_present( base->client );
    if (frame_id && !(completion = alloc_client_surface_metadata( sizeof(*completion) )))
    {
        client_surface_submit_present( base->client, &present );
        complete_opengl_present( base->client, &present, FALSE, FALSE, expected_size, 0 );
        RtlSetLastWin32Error( ERROR_NOT_ENOUGH_MEMORY );
        return FALSE;
    }
    if ((blit || framebuffer) && !refresh_prepared_egl_surface( base, &present ))
    {
        free_client_surface_metadata( completion, sizeof(*completion) );
        client_surface_submit_present( base->client, &present );
        complete_opengl_present( base->client, &present, FALSE, FALSE, expected_size, 0 );
        return FALSE;
    }
    if (blit && !blit_client_surface_output( base, &present, source, blit,
                   (!usexcomposite || surface->direct_snapshot) &&
                   present.completion.kind == CLIENT_SURFACE_COMPLETION_EXACT ))
    {
        free_client_surface_metadata( completion, sizeof(*completion) );
        client_surface_submit_present( base->client, &present );
        complete_opengl_present( base->client, &present, FALSE, FALSE, expected_size, 0 );
        return FALSE;
    }
    /* The private native target has an exact completion even when PREPARING
     * still invalidates publication. Preserve its new image independently,
     * just as the GLX snapshot path does, before any native swap can lose it. */
    if ((!usexcomposite || surface->direct_snapshot) && present.completion.kind == CLIENT_SURFACE_COMPLETION_EXACT)
    {
        int gpu = 0;
        BOOL copied;

        if (surface->direct_snapshot && !gl->gpu_snapshot_failed && present.handoff_control)
        {
            gpu = snapshot_client_surface_gpu( base, &present, framebuffer );
            if (!gpu) gl->gpu_snapshot_failed = TRUE;
        }
        copied = gpu > 0 || (!gpu && snapshot_client_surface( base, &present, framebuffer,
                                                              framebuffer ? GL_COLOR_ATTACHMENT0 : GL_BACK ));

        ret = copied;
        client_surface_submit_present( base->client, &present );
        if (copied && present.completion.wait)
        {
            client_surface_defer_present( base->client, &present, expected_size );
            return TRUE;
        }
        if (!complete_opengl_present( base->client, &present, ret && copied, copied, NULL, 0 ))
            WARN( "client-surface snapshot did not complete for %s\n", debugstr_opengl_drawable( base ) );
        return ret;
    }
    if (framebuffer && present.target != CLIENT_SURFACE_FRAME_TARGET_ONSCREEN)
    {
        /* A native scene transition does not invalidate the rendered FBO.
         * Freeze it for replay when the owner has installed the new plan. */
        ret = snapshot_client_surface( base, &present, framebuffer, GL_COLOR_ATTACHMENT0 );
        client_surface_submit_present( base->client, &present );
        complete_opengl_present( base->client, &present, ret, ret, NULL, 0 );
        return ret;
    }
    if (framebuffer && !blit_client_surface_framebuffer( base, &present, framebuffer ))
    {
        client_surface_submit_present( base->client, &present );
        complete_opengl_present( base->client, &present, FALSE, FALSE, NULL, 0 );
        return FALSE;
    }
    ret = funcs->p_eglSwapBuffers( egl->display, gl->base.surface );
    if (blit) TRACE( "native EGL generic framebuffer presentation for %s returned %u\n",
                     debugstr_opengl_drawable( base ), ret );
    if (framebuffer) TRACE( "native EGL framebuffer %u presentation for %s returned %u\n",
                            framebuffer, debugstr_opengl_drawable( base ), ret );
    client_surface_submit_present( base->client, &present );
    if (!ret)
    {
        free_client_surface_metadata( completion, sizeof(*completion) );
        err = funcs->p_eglGetError();
        WARN( "Failed to swap EGL surface %s, error %#x\n", debugstr_opengl_drawable( base ), err );
        complete_opengl_present( base->client, &present, FALSE, FALSE, NULL, 0 );
        return FALSE;
    }

    if (present.completion.kind == CLIENT_SURFACE_COMPLETION_EXACT && frame_id)
    {
        completion->drawable = base;
        completion->frame_id = frame_id;
        opengl_drawable_add_ref( base );
        client_surface_set_present_completion( &present, wait_egl_present_completion,
                                               release_egl_present_completion, completion );
        client_surface_defer_present( base->client, &present, expected_size );
        return TRUE;
    }

    if (!complete_opengl_present( base->client, &present, TRUE,
                                          FALSE, expected_size,
                                          CLIENT_SURFACE_PRESENT_TIMEOUT ))
        WARN( "client-surface composition did not complete for %s\n",
              debugstr_opengl_drawable( base ) );
    return TRUE;
}

static BOOL x11drv_egl_surface_swap( struct opengl_drawable *base )
{
    return x11drv_egl_surface_present( base, 0, NULL, NULL );
}

static BOOL x11drv_egl_surface_swap_framebuffer( struct opengl_drawable *base, GLuint framebuffer )
{
    return x11drv_egl_surface_present( base, framebuffer, NULL, NULL );
}

static BOOL x11drv_egl_surface_swap_blit( struct opengl_drawable *base, struct opengl_drawable *source,
                                          opengl_drawable_blit_func blit )
{
    return x11drv_egl_surface_present( base, 0, source, blit );
}

static struct opengl_driver_funcs x11drv_driver_funcs =
{
    .p_get_proc_address = x11drv_get_proc_address,
    .p_init_pixel_formats = x11drv_init_pixel_formats,
    .p_describe_pixel_format = x11drv_describe_pixel_format,
    .p_init_extensions = x11drv_init_extensions,
    .p_surface_create = x11drv_surface_create,
    .p_context_create = x11drv_context_create,
    .p_context_destroy = x11drv_context_destroy,
    .p_make_current = x11drv_make_current,
    .p_pbuffer_create = x11drv_pbuffer_create,
    .p_pbuffer_updated = x11drv_pbuffer_updated,
    .p_pbuffer_bind = x11drv_pbuffer_bind,
    .p_null_surface_create = x11drv_null_surface_create,
};

static const struct opengl_drawable_funcs x11drv_surface_funcs =
{
    .destroy = x11drv_surface_destroy,
    .flush = x11drv_surface_flush,
    .swap = x11drv_surface_swap,
    .swap_blit = x11drv_surface_swap_blit,
};

static const struct opengl_drawable_funcs x11drv_pbuffer_funcs =
{
    .destroy = x11drv_pbuffer_destroy,
};

static const struct opengl_drawable_funcs x11drv_egl_surface_funcs =
{
    .destroy = x11drv_egl_surface_destroy,
    .flush = x11drv_egl_surface_flush,
    .swap = x11drv_egl_surface_swap,
    .swap_blit = x11drv_egl_surface_swap_blit,
};

static const struct opengl_drawable_funcs x11drv_egl_snapshot_surface_funcs =
{
    .destroy = x11drv_egl_surface_destroy,
    .flush = x11drv_egl_surface_flush,
    .swap = x11drv_egl_surface_swap,
    .swap_framebuffer = x11drv_egl_surface_swap_framebuffer,
    .swap_blit = x11drv_egl_surface_swap_blit,
};

#else  /* no OpenGL includes */

/**********************************************************************
 *           X11DRV_OpenglInit
 */
UINT X11DRV_OpenGLInit( UINT version, const struct opengl_funcs *opengl_funcs, const struct opengl_driver_funcs **driver_funcs )
{
    return STATUS_NOT_IMPLEMENTED;
}

void sync_gl_drawable( HWND hwnd )
{
}

void destroy_gl_drawable( HWND hwnd )
{
}

BOOL visual_from_pixel_format( int format, XVisualInfo *visual )
{
    return FALSE;
}

#endif /* defined(SONAME_LIBGL) */
