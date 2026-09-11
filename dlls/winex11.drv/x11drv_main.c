/*
 * X11DRV initialization code
 *
 * Copyright 1998 Patrik Stridvall
 * Copyright 2000 Alexandre Julliard
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

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <assert.h>
#include <dlfcn.h>
#include <X11/cursorfont.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#ifdef HAVE_X11_EXTENSIONS_XRENDER_H
#include <X11/extensions/Xrender.h>
#endif

#include "ntstatus.h"

#include "x11drv.h"
#include "client_surface_cache.h"
#include "winreg.h"
#include "xcomposite.h"
#include "xpresent.h"
#include "wine/server.h"
#include "wine/debug.h"
#include "wine/list.h"
#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);
WINE_DECLARE_DEBUG_CHANNEL(synchronous);
WINE_DECLARE_DEBUG_CHANNEL(winediag);
WINE_DECLARE_DEBUG_CHANNEL(csperf);

struct x11drv_display_owner
{
    struct client_surface_memory_scope memory;
    struct client_surface_native_work close_work;
    struct x11drv_error_handler errors;
    struct list native_errors;
    Display *display;
    Window clip_window;
    LONG refs;
    BOOL detached, clipboard;
};

static int display_owner_error( Display *display, XErrorEvent *event, void *arg );

XVisualInfo default_visual = { 0 };
XVisualInfo argb_visual = { 0 };
Colormap default_colormap = None;
XPixmapFormatValues **pixmap_formats;
Atom systray_atom = 0;
HWND systray_hwnd = 0;
unsigned int screen_bpp;
Window root_window;
Atom net_wm_cm_selection;
BOOL usexvidmode = TRUE;
BOOL usexrandr = TRUE;
BOOL usexcomposite = TRUE;
BOOL usexpresent = FALSE;
BOOL use_egl = TRUE;
BOOL use_take_focus = TRUE;
BOOL use_primary_selection = FALSE;
BOOL use_system_cursors = TRUE;
BOOL grab_fullscreen = FALSE;
BOOL managed_mode = TRUE;
BOOL private_color_map = FALSE;
int primary_monitor = 0;
BOOL client_side_graphics = TRUE;
BOOL client_side_with_render = TRUE;
BOOL shape_layered_windows = TRUE;
int copy_default_colors = 128;
int alloc_system_colors = 256;
int xrender_error_base = 0;
char *process_name = NULL;
pthread_key_t x11drv_thread_data_key = 0;

static x11drv_error_callback err_callback;   /* current callback for error */
static Display *err_callback_display;        /* display callback is set for */
static void *err_callback_arg;               /* error callback argument */
static int err_callback_result;              /* error callback result */
static unsigned long err_serial;             /* serial number of first request */
static int (*old_error_handler)( Display *, XErrorEvent * );
static BOOL use_xim = TRUE;
static WCHAR input_style[20];

static pthread_mutex_t error_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t error_handlers_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct list error_handlers = LIST_INIT(error_handlers);

#define IS_OPTION_TRUE(ch) \
    ((ch) == 'y' || (ch) == 'Y' || (ch) == 't' || (ch) == 'T' || (ch) == '1')
#define IS_OPTION_FALSE(ch) \
    ((ch) == 'n' || (ch) == 'N' || (ch) == 'f' || (ch) == 'F' || (ch) == '0')

Atom X11DRV_Atoms[NB_XATOMS - FIRST_XATOM];

const char * const X11DRV_atom_names[NB_XATOMS - FIRST_XATOM] =
{
    "CLIPBOARD",
    "COMPOUND_TEXT",
    "EDID",
    "INCR",
    "MANAGER",
    "MULTIPLE",
    "SELECTION_DATA",
    "TARGETS",
    "TEXT",
    "TIMESTAMP",
    "UTF8_STRING",
    "RAW_ASCENT",
    "RAW_DESCENT",
    "RAW_CAP_HEIGHT",
    "WM_PROTOCOLS",
    "WM_DELETE_WINDOW",
    "WM_HINTS",
    "WM_NORMAL_HINTS",
    "WM_STATE",
    "WM_TAKE_FOCUS",
    "XIM_SERVERS",
    "DndProtocol",
    "DndSelection",
    "_ICC_PROFILE",
    "_KDE_NET_WM_STATE_SKIP_SWITCHER",
    "_MOTIF_WM_HINTS",
    "_NET_ACTIVE_WINDOW",
    "_NET_STARTUP_INFO_BEGIN",
    "_NET_STARTUP_INFO",
    "_NET_SUPPORTED",
    "_NET_SYSTEM_TRAY_OPCODE",
    "_NET_SYSTEM_TRAY_S0",
    "_NET_SYSTEM_TRAY_VISUAL",
    "_NET_WM_FULLSCREEN_MONITORS",
    "_NET_WM_ICON",
    "_NET_WM_MOVERESIZE",
    "_NET_WM_NAME",
    "_NET_WM_PID",
    "_NET_WM_PING",
    "_NET_WM_STATE",
    "_NET_WM_STATE_ABOVE",
    "_NET_WM_STATE_DEMANDS_ATTENTION",
    "_NET_WM_STATE_FULLSCREEN",
    "_NET_WM_STATE_HIDDEN",
    "_NET_WM_STATE_MAXIMIZED_HORZ",
    "_NET_WM_STATE_MAXIMIZED_VERT",
    "_NET_WM_STATE_SKIP_PAGER",
    "_NET_WM_STATE_SKIP_TASKBAR",
    "_NET_WM_USER_TIME",
    "_NET_WM_USER_TIME_WINDOW",
    "_NET_WM_WINDOW_OPACITY",
    "_NET_WM_WINDOW_TYPE",
    "_NET_WM_WINDOW_TYPE_DIALOG",
    "_NET_WM_WINDOW_TYPE_NORMAL",
    "_NET_WM_WINDOW_TYPE_UTILITY",
    "_NET_WORKAREA",
    "_GTK_WORKAREAS_D0",
    "_XEMBED",
    "_XEMBED_INFO",
    "XdndAware",
    "XdndEnter",
    "XdndPosition",
    "XdndStatus",
    "XdndLeave",
    "XdndFinished",
    "XdndDrop",
    "XdndActionCopy",
    "XdndActionMove",
    "XdndActionLink",
    "XdndActionAsk",
    "XdndActionPrivate",
    "XdndSelection",
    "XdndTypeList",
    "HTML Format",
    "WCF_DIF",
    "WCF_ENHMETAFILE",
    "WCF_HDROP",
    "WCF_PENDATA",
    "WCF_RIFF",
    "WCF_SYLK",
    "WCF_TIFF",
    "WCF_WAVE",
    "image/bmp",
    "image/gif",
    "image/jpeg",
    "image/png",
    "text/html",
    "text/plain",
    "text/rtf",
    "text/richtext",
    "text/uri-list"
};

/***********************************************************************
 *		ignore_error
 *
 * Check if the X error is one we can ignore.
 */
static inline BOOL ignore_error( Display *display, XErrorEvent *event )
{
    if ((event->request_code == X_SetInputFocus ||
         event->request_code == X_ChangeWindowAttributes ||
         event->request_code == X_ConfigureWindow ||
         event->request_code == X_SendEvent) &&
        (event->error_code == BadMatch ||
         event->error_code == BadWindow)) return TRUE;

    /* ignore a number of errors on gdi display caused by creating/destroying windows */
    if (display == gdi_display)
    {
        if (event->error_code == BadDrawable ||
            event->error_code == BadGC ||
            event->error_code == BadWindow)
            return TRUE;
#ifdef HAVE_X11_EXTENSIONS_XRENDER_H
        if (xrender_error_base)  /* check for XRender errors */
        {
            if (event->error_code == xrender_error_base + BadPicture) return TRUE;
        }
#endif
    }
    return FALSE;
}


/***********************************************************************
 *		X11DRV_sync_window_changes
 */
void X11DRV_sync_window_changes( Display *display )
{
    unsigned long serial, processed;

    XLockDisplay( display );
    serial = NextRequest( display ) - 1;
    processed = LastKnownRequestProcessed( display );
    /* A reply or event may already acknowledge all native window changes on
     * this connection. Only outstanding requests need another round trip. */
    if (processed != serial) XSync( display, False );
    TRACE( "display %p serial %lu processed %lu synchronized %u\n",
           display, serial, processed, processed != serial );
    XUnlockDisplay( display );
}


/***********************************************************************
 *		X11DRV_expect_error
 *
 * Setup a callback function that will be called on an X error.  The
 * callback must return non-zero if the error is the one it expected.
 */
void X11DRV_expect_error( Display *display, x11drv_error_callback callback, void *arg )
{
    pthread_mutex_lock( &error_mutex );
    XLockDisplay( display );
    err_callback         = callback;
    err_callback_display = display;
    err_callback_arg     = arg;
    err_callback_result  = 0;
    err_serial           = NextRequest(display);
}


/***********************************************************************
 *		X11DRV_check_error
 *
 * Check if an expected X11 error occurred; return non-zero if yes.
 * The caller is responsible for calling XSync first if necessary.
 */
int X11DRV_check_error(void)
{
    int res = err_callback_result;
    err_callback = NULL;
    XUnlockDisplay( err_callback_display );
    pthread_mutex_unlock( &error_mutex );
    return res;
}


void X11DRV_register_error_handler( struct x11drv_error_handler *handler )
{
    pthread_mutex_lock( &error_handlers_mutex );
    /* XCloseDisplay may have just released the address of a connection whose
     * owner has not yet removed its sink. A newly opened connection wins. */
    list_add_head( &error_handlers, &handler->entry );
    pthread_mutex_unlock( &error_handlers_mutex );
}

void X11DRV_unregister_error_handler( struct x11drv_error_handler *handler )
{
    pthread_mutex_lock( &error_handlers_mutex );
    list_remove( &handler->entry );
    pthread_mutex_unlock( &error_handlers_mutex );
}

/***********************************************************************
 *		error_handler
 */
static int error_handler( Display *display, XErrorEvent *error_evt )
{
    struct x11drv_error_handler *handler;
    int handled = 0;
    BOOL clipboard = FALSE;

    /* This lock protects registration and the short error callback only.
     * A stopped request on a private connection cannot hold it across I/O. */
    pthread_mutex_lock( &error_handlers_mutex );
    LIST_FOR_EACH_ENTRY( handler, &error_handlers, struct x11drv_error_handler, entry )
        if (handler->display == display)
        {
            handled = handler->callback( display, error_evt, handler->arg );
            if (handler->callback == display_owner_error)
                clipboard = ((struct x11drv_display_owner *)handler->arg)->clipboard;
            break;
        }
    pthread_mutex_unlock( &error_handlers_mutex );
    if (handled) return 0;

    if (err_callback && display == err_callback_display &&
        (!error_evt->serial || error_evt->serial >= err_serial))
    {
        if ((err_callback_result = err_callback( display, error_evt, err_callback_arg )))
        {
            TRACE( "got expected error %d req %d\n",
                   error_evt->error_code, error_evt->request_code );
            return 0;
        }
    }
    /* External clipboard windows may disappear, but expected errors must be
     * delivered first. The role survives logical detach until native close. */
    if (clipboard || ignore_error( display, error_evt ))
    {
        TRACE( "got ignored error %d req %d\n",
               error_evt->error_code, error_evt->request_code );
        return 0;
    }
    if (TRACE_ON(synchronous))
    {
        ERR( "X protocol error: serial=%ld, request_code=%d - breaking into debugger\n",
             error_evt->serial, error_evt->request_code );
        assert( 0 );
    }
    old_error_handler( display, error_evt );
    return 0;
}

/***********************************************************************
 *		init_pixmap_formats
 */
static void init_pixmap_formats( Display *display )
{
    int i, count, max = 32;
    XPixmapFormatValues *formats = XListPixmapFormats( display, &count );

    for (i = 0; i < count; i++)
    {
        TRACE( "depth %u, bpp %u, pad %u\n",
               formats[i].depth, formats[i].bits_per_pixel, formats[i].scanline_pad );
        if (formats[i].depth > max) max = formats[i].depth;
    }
    pixmap_formats = calloc( 1, sizeof(*pixmap_formats) * (max + 1) );
    for (i = 0; i < count; i++) pixmap_formats[formats[i].depth] = &formats[i];
}


HKEY reg_open_key( HKEY root, const WCHAR *name, ULONG name_len )
{
    UNICODE_STRING nameW = { name_len, name_len, (WCHAR *)name };
    OBJECT_ATTRIBUTES attr;
    HANDLE ret;

    attr.Length = sizeof(attr);
    attr.RootDirectory = root;
    attr.ObjectName = &nameW;
    attr.Attributes = 0;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;

    return NtOpenKeyEx( &ret, MAXIMUM_ALLOWED, &attr, 0 ) ? 0 : ret;
}


HKEY open_hkcu_key( const char *name )
{
    WCHAR bufferW[256];
    static HKEY hkcu;

    if (!hkcu)
    {
        char buffer[256];
        DWORD_PTR sid_data[(sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE) / sizeof(DWORD_PTR)];
        DWORD i, len = sizeof(sid_data);
        SID *sid;

        if (NtQueryInformationToken( GetCurrentThreadEffectiveToken(), TokenUser, sid_data, len, &len ))
            return 0;

        sid = ((TOKEN_USER *)sid_data)->User.Sid;
        len = sprintf( buffer, "\\Registry\\User\\S-%u-%u", sid->Revision,
                       MAKELONG( MAKEWORD( sid->IdentifierAuthority.Value[5],
                                           sid->IdentifierAuthority.Value[4] ),
                                 MAKEWORD( sid->IdentifierAuthority.Value[3],
                                           sid->IdentifierAuthority.Value[2] )));
        for (i = 0; i < sid->SubAuthorityCount; i++)
            len += sprintf( buffer + len, "-%u", sid->SubAuthority[i] );

        ascii_to_unicode( bufferW, buffer, len );
        hkcu = reg_open_key( NULL, bufferW, len * sizeof(WCHAR) );
    }

    return reg_open_key( hkcu, bufferW, asciiz_to_unicode( bufferW, name ) - sizeof(WCHAR) );
}


ULONG query_reg_value( HKEY hkey, const WCHAR *name, KEY_VALUE_PARTIAL_INFORMATION *info, ULONG size )
{
    unsigned int name_size = name ? lstrlenW( name ) * sizeof(WCHAR) : 0;
    UNICODE_STRING nameW = { name_size, name_size, (WCHAR *)name };

    if (NtQueryValueKey( hkey, &nameW, KeyValuePartialInformation,
                         info, size, &size ))
        return 0;

    return size - FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data);
}


/***********************************************************************
 *		get_config_key
 *
 * Get a config key from either the app-specific or the default config
 */
static inline DWORD get_config_key( HKEY defkey, HKEY appkey, const char *name,
                                    WCHAR *buffer, DWORD size )
{
    WCHAR nameW[128];
    char buf[2048];
    KEY_VALUE_PARTIAL_INFORMATION *info = (void *)buf;

    asciiz_to_unicode( nameW, name );

    if (appkey && query_reg_value( appkey, nameW, info, sizeof(buf) ))
    {
        size = min( info->DataLength, size - sizeof(WCHAR) );
        memcpy( buffer, info->Data, size );
        buffer[size / sizeof(WCHAR)] = 0;
        return 0;
    }

    if (defkey && query_reg_value( defkey, nameW, info, sizeof(buf) ))
    {
        size = min( info->DataLength, size - sizeof(WCHAR) );
        memcpy( buffer, info->Data, size );
        buffer[size / sizeof(WCHAR)] = 0;
        return 0;
    }

    return ERROR_FILE_NOT_FOUND;
}


/***********************************************************************
 *		setup_options
 *
 * Setup the x11drv options.
 */
static void setup_options(void)
{
    static const WCHAR x11driverW[] = {'\\','X','1','1',' ','D','r','i','v','e','r',0};
    WCHAR buffer[MAX_PATH+16], *p, *appname;
    HKEY hkey, appkey = 0;
    DWORD len;

    /* @@ Wine registry key: HKCU\Software\Wine\X11 Driver */
    hkey = open_hkcu_key( "Software\\Wine\\X11 Driver" );

    /* open the app-specific key */

    appname = RtlGetCurrentPeb()->ProcessParameters->ImagePathName.Buffer;
    if ((p = wcsrchr( appname, '/' ))) appname = p + 1;
    if ((p = wcsrchr( appname, '\\' ))) appname = p + 1;
    len = lstrlenW( appname );

    if (len && len < MAX_PATH)
    {
        HKEY tmpkey;
        int i;
        for (i = 0; appname[i]; i++) buffer[i] = RtlDowncaseUnicodeChar( appname[i] );
        buffer[i] = 0;
        appname = buffer;
        if ((process_name = malloc( len * 3 + 1 )))
            ntdll_wcstoumbs( appname, len + 1, process_name, len * 3 + 1, FALSE );
        memcpy( appname + i, x11driverW, sizeof(x11driverW) );
        /* @@ Wine registry key: HKCU\Software\Wine\AppDefaults\app.exe\X11 Driver */
        if ((tmpkey = open_hkcu_key( "Software\\Wine\\AppDefaults" )))
        {
            appkey = reg_open_key( tmpkey, appname, lstrlenW( appname ) * sizeof(WCHAR) );
            NtClose( tmpkey );
        }
    }

    if (!get_config_key( hkey, appkey, "Managed", buffer, sizeof(buffer) ))
        managed_mode = IS_OPTION_TRUE( buffer[0] );

    if (!get_config_key( hkey, appkey, "UseEGL", buffer, sizeof(buffer) ))
        use_egl = IS_OPTION_TRUE( buffer[0] );

    if (!get_config_key( hkey, appkey, "UseXVidMode", buffer, sizeof(buffer) ))
        usexvidmode = IS_OPTION_TRUE( buffer[0] );

    if (!get_config_key( hkey, appkey, "UseXRandR", buffer, sizeof(buffer) ))
        usexrandr = IS_OPTION_TRUE( buffer[0] );

    if (!get_config_key( hkey, appkey, "UseTakeFocus", buffer, sizeof(buffer) ))
        use_take_focus = IS_OPTION_TRUE( buffer[0] );

    if (!get_config_key( hkey, appkey, "UsePrimarySelection", buffer, sizeof(buffer) ))
        use_primary_selection = IS_OPTION_TRUE( buffer[0] );

    if (!get_config_key( hkey, appkey, "UseSystemCursors", buffer, sizeof(buffer) ))
        use_system_cursors = IS_OPTION_TRUE( buffer[0] );

    if (!get_config_key( hkey, appkey, "GrabFullscreen", buffer, sizeof(buffer) ))
        grab_fullscreen = IS_OPTION_TRUE( buffer[0] );

    if (!get_config_key( hkey, appkey, "ScreenDepth", buffer, sizeof(buffer) ))
        default_visual.depth = wcstol( buffer, NULL, 0 );

    if (!get_config_key( hkey, appkey, "ClientSideGraphics", buffer, sizeof(buffer) ))
        client_side_graphics = IS_OPTION_TRUE( buffer[0] );

    if (!get_config_key( hkey, appkey, "ClientSideWithRender", buffer, sizeof(buffer) ))
        client_side_with_render = IS_OPTION_TRUE( buffer[0] );

    if (!get_config_key( hkey, appkey, "UseXIM", buffer, sizeof(buffer) ))
        use_xim = IS_OPTION_TRUE( buffer[0] );

    if (!get_config_key( hkey, appkey, "ShapeLayeredWindows", buffer, sizeof(buffer) ))
        shape_layered_windows = IS_OPTION_TRUE( buffer[0] );

    if (!get_config_key( hkey, appkey, "PrivateColorMap", buffer, sizeof(buffer) ))
        private_color_map = IS_OPTION_TRUE( buffer[0] );

    if (!get_config_key( hkey, appkey, "PrimaryMonitor", buffer, sizeof(buffer) ))
        primary_monitor = wcstol( buffer, NULL, 0 );

    if (!get_config_key( hkey, appkey, "CopyDefaultColors", buffer, sizeof(buffer) ))
        copy_default_colors = wcstol( buffer, NULL, 0 );

    if (!get_config_key( hkey, appkey, "AllocSystemColors", buffer, sizeof(buffer) ))
        alloc_system_colors = wcstol( buffer, NULL, 0 );

    get_config_key( hkey, appkey, "InputStyle", input_style, sizeof(input_style) );

    NtClose( appkey );
    NtClose( hkey );
}

#ifdef SONAME_LIBXCOMPOSITE

#define MAKE_FUNCPTR(f) typeof(f) * p##f;
MAKE_FUNCPTR(XCompositeQueryExtension)
MAKE_FUNCPTR(XCompositeQueryVersion)
MAKE_FUNCPTR(XCompositeVersion)
MAKE_FUNCPTR(XCompositeRedirectWindow)
MAKE_FUNCPTR(XCompositeRedirectSubwindows)
MAKE_FUNCPTR(XCompositeUnredirectWindow)
MAKE_FUNCPTR(XCompositeUnredirectSubwindows)
MAKE_FUNCPTR(XCompositeCreateRegionFromBorderClip)
MAKE_FUNCPTR(XCompositeNameWindowPixmap)
#undef MAKE_FUNCPTR

static int xcomp_event_base;
static int xcomp_error_base;

static void X11DRV_XComposite_Init(void)
{
    void *xcomposite_handle = dlopen(SONAME_LIBXCOMPOSITE, RTLD_NOW);
    if (!xcomposite_handle)
    {
        TRACE("Unable to open %s, XComposite disabled\n", SONAME_LIBXCOMPOSITE);
        usexcomposite = FALSE;
        return;
    }

#define LOAD_FUNCPTR(f) \
    if((p##f = dlsym(xcomposite_handle, #f)) == NULL) goto sym_not_found
    LOAD_FUNCPTR(XCompositeQueryExtension);
    LOAD_FUNCPTR(XCompositeQueryVersion);
    LOAD_FUNCPTR(XCompositeVersion);
    LOAD_FUNCPTR(XCompositeRedirectWindow);
    LOAD_FUNCPTR(XCompositeRedirectSubwindows);
    LOAD_FUNCPTR(XCompositeUnredirectWindow);
    LOAD_FUNCPTR(XCompositeUnredirectSubwindows);
    LOAD_FUNCPTR(XCompositeCreateRegionFromBorderClip);
    LOAD_FUNCPTR(XCompositeNameWindowPixmap);
#undef LOAD_FUNCPTR

    if(!pXCompositeQueryExtension(gdi_display, &xcomp_event_base,
                                  &xcomp_error_base)) {
        TRACE("XComposite extension could not be queried; disabled\n");
        dlclose(xcomposite_handle);
        xcomposite_handle = NULL;
        usexcomposite = FALSE;
        return;
    }
    TRACE("XComposite is up and running error_base = %d\n", xcomp_error_base);
    return;

sym_not_found:
    TRACE("Unable to load function pointers from %s, XComposite disabled\n", SONAME_LIBXCOMPOSITE);
    dlclose(xcomposite_handle);
    xcomposite_handle = NULL;
    usexcomposite = FALSE;
}
#endif /* defined(SONAME_LIBXCOMPOSITE) */

#ifdef SONAME_LIBXPRESENT

#define MAKE_FUNCPTR(f) typeof(f) * p##f;
MAKE_FUNCPTR(XPresentQueryExtension)
MAKE_FUNCPTR(XPresentQueryVersion)
MAKE_FUNCPTR(XPresentPixmap)
MAKE_FUNCPTR(XPresentSelectInput)
MAKE_FUNCPTR(XPresentFreeInput)
MAKE_FUNCPTR(XPresentQueryCapabilities)
#undef MAKE_FUNCPTR

static void X11DRV_XPresent_Init(void)
{
    void *handle = dlopen( SONAME_LIBXPRESENT, RTLD_NOW );
    int major_opcode, event_base, error_base;
    int major, minor;

    if (!handle)
    {
        TRACE( "Unable to open %s, X Present disabled\n", SONAME_LIBXPRESENT );
        return;
    }

#define LOAD_FUNCPTR(f) if (!(p##f = dlsym( handle, #f ))) goto failed
    LOAD_FUNCPTR(XPresentQueryExtension);
    LOAD_FUNCPTR(XPresentQueryVersion);
    LOAD_FUNCPTR(XPresentPixmap);
    LOAD_FUNCPTR(XPresentSelectInput);
    LOAD_FUNCPTR(XPresentFreeInput);
    LOAD_FUNCPTR(XPresentQueryCapabilities);
#undef LOAD_FUNCPTR

    if (!pXPresentQueryExtension( gdi_display, &major_opcode, &event_base, &error_base ) ||
        !pXPresentQueryVersion( gdi_display, &major, &minor ))
    {
        TRACE( "X Present extension could not be queried; disabled\n" );
        goto failed;
    }
    usexpresent = TRUE;
    TRACE( "X Present %d.%d is up and running opcode %d error_base %d\n",
           major, minor, major_opcode, error_base );
    return;

failed:
    TRACE( "Unable to initialize %s, X Present disabled\n", SONAME_LIBXPRESENT );
    dlclose( handle );
}

#endif

static void init_visuals( Display *display, int screen )
{
    int count;
    XVisualInfo *info;

    argb_visual.screen     = screen;
    argb_visual.class      = TrueColor;
    argb_visual.depth      = 32;
    argb_visual.red_mask   = 0xff0000;
    argb_visual.green_mask = 0x00ff00;
    argb_visual.blue_mask  = 0x0000ff;

    if ((info = XGetVisualInfo( display, VisualScreenMask | VisualDepthMask | VisualClassMask |
                                VisualRedMaskMask | VisualGreenMaskMask | VisualBlueMaskMask,
                                &argb_visual, &count )))
    {
        argb_visual = *info;
        XFree( info );
    }

    default_visual.screen = screen;
    if (default_visual.depth)  /* depth specified */
    {
        if (default_visual.depth == 32 && argb_visual.visual)
        {
            default_visual = argb_visual;
        }
        else if ((info = XGetVisualInfo( display, VisualScreenMask | VisualDepthMask, &default_visual, &count )))
        {
            default_visual = *info;
            XFree( info );
        }
        else WARN( "no visual found for depth %d\n", default_visual.depth );
    }

    if (!default_visual.visual)
    {
        default_visual.depth         = DefaultDepth( display, screen );
        default_visual.visual        = DefaultVisual( display, screen );
        default_visual.visualid      = default_visual.visual->visualid;
        default_visual.class         = default_visual.visual->class;
        default_visual.red_mask      = default_visual.visual->red_mask;
        default_visual.green_mask    = default_visual.visual->green_mask;
        default_visual.blue_mask     = default_visual.visual->blue_mask;
        default_visual.colormap_size = default_visual.visual->map_entries;
        default_visual.bits_per_rgb  = default_visual.visual->bits_per_rgb;
    }
    default_colormap = XCreateColormap( display, root_window, default_visual.visual, AllocNone );

    TRACE( "default visual %lx class %u argb %lx\n",
           default_visual.visualid, default_visual.class, argb_visual.visualid );
}

static void detect_window_manager( Display *display )
{
    XWindowAttributes attr;

    if (!managed_mode) return;
    if (!XGetWindowAttributes( display, DefaultRootWindow( display ), &attr )) return;

    /* ICCCM window managers must select SubstructureRedirectMask on the root
     * window. Without one, managed windows would wait forever for WM_STATE
     * transitions which no client can produce. */
    if (!(attr.all_event_masks & SubstructureRedirectMask))
    {
        TRACE( "no window manager detected, disabling managed mode\n" );
        managed_mode = FALSE;
    }
}

/***********************************************************************
 *           X11DRV process initialisation routine
 */
NTSTATUS __wine_unix_lib_init(void)
{
    Display *display;
    char selection[32];
    void *libx11 = dlopen( SONAME_LIBX11, RTLD_NOW|RTLD_GLOBAL );

    if (!libx11)
    {
        ERR( "failed to load %s: %s\n", SONAME_LIBX11, dlerror() );
        return STATUS_UNSUCCESSFUL;
    }
    pXGetEventData = dlsym( libx11, "XGetEventData" );
    pXFreeEventData = dlsym( libx11, "XFreeEventData" );
#ifdef SONAME_LIBXEXT
    dlopen( SONAME_LIBXEXT, RTLD_NOW|RTLD_GLOBAL );
#endif

    setup_options();

    /* Open display */

    if (!XInitThreads()) ERR( "XInitThreads failed, trouble ahead\n" );
    if (!(display = XOpenDisplay( NULL ))) return STATUS_UNSUCCESSFUL;

    fcntl( ConnectionNumber(display), F_SETFD, 1 ); /* set close on exec flag */
    root_window = DefaultRootWindow( display );
    gdi_display = display;
    old_error_handler = XSetErrorHandler( error_handler );
    detect_window_manager( display );

    pthread_key_create( &x11drv_thread_data_key, NULL );
    init_pixmap_formats( display );
    init_visuals( display, DefaultScreen( display ));
    screen_bpp = pixmap_formats[default_visual.depth]->bits_per_pixel;

    XInternAtoms( display, (char **)X11DRV_atom_names, NB_XATOMS - FIRST_XATOM, False, X11DRV_Atoms );
    snprintf( selection, sizeof(selection), "_NET_WM_CM_S%d", DefaultScreen( display ) );
    net_wm_cm_selection = XInternAtom( display, selection, False );

    init_win_context();

    if (TRACE_ON(synchronous)) XSynchronize( display, True );

    xinerama_init( DisplayWidth( display, default_visual.screen ),
                   DisplayHeight( display, default_visual.screen ));
    X11DRV_Settings_Init();

    /* initialize XVidMode */
    X11DRV_XF86VM_Init();
    /* initialize XRandR */
    X11DRV_XRandR_Init();
#ifdef SONAME_LIBXCOMPOSITE
    X11DRV_XComposite_Init();
#endif
#ifdef SONAME_LIBXPRESENT
    X11DRV_XPresent_Init();
#endif
    x11drv_xinput2_load();

    x11drv_init_keyboard( gdi_display );
    if (use_xim) use_xim = xim_init( input_style );

    init_icm_profile();
    init_user_driver();
    return STATUS_SUCCESS;
}


static int display_owner_error( Display *display, XErrorEvent *event, void *arg )
{
    struct x11drv_display_owner *owner = arg;
    struct x11drv_error_handler *handler;

    LIST_FOR_EACH_ENTRY( handler, &owner->native_errors, struct x11drv_error_handler, entry )
        if (handler->callback( display, event, handler->arg )) return 1;

    /* An owned parent can already have destroyed its clip Window. */
    return owner->clip_window && event->request_code == X_DestroyWindow &&
           event->error_code == BadWindow && event->resourceid == owner->clip_window;
}

void x11drv_display_owner_register_error_handler( struct x11drv_display_owner *owner,
                                                 struct x11drv_error_handler *handler )
{
    assert( handler->display == owner->display );
    pthread_mutex_lock( &error_handlers_mutex );
    list_add_head( &owner->native_errors, &handler->entry );
    pthread_mutex_unlock( &error_handlers_mutex );
}

void x11drv_display_owner_unregister_error_handler( struct x11drv_display_owner *owner,
                                                   struct x11drv_error_handler *handler )
{
    assert( handler->display == owner->display );
    pthread_mutex_lock( &error_handlers_mutex );
    list_remove( &handler->entry );
    pthread_mutex_unlock( &error_handlers_mutex );
}

static void close_display_owner( struct client_surface_native_work *work )
{
    struct x11drv_display_owner *owner = CONTAINING_RECORD( work, struct x11drv_display_owner, close_work );

    assert( list_empty( &owner->native_errors ));
    XCloseDisplay( owner->display );
    X11DRV_unregister_error_handler( &owner->errors );
    TRACE_(csperf)( "event=display_close_return owner=%p display=%p\n", owner, owner->display );
}

static void free_display_owner( struct client_surface_native_work *work )
{
    struct x11drv_display_owner *owner = CONTAINING_RECORD( work, struct x11drv_display_owner, close_work );
    Display *display = owner->display;
    ULONG_PTR identity = (ULONG_PTR)owner;

    client_surface_free_owned_metadata( &owner->memory, owner, sizeof(*owner) );
    x11drv_return_release_capacity( 1, sizeof(*owner) );
    TRACE_(csperf)( "event=display_owner_return owner=0x%lx display=%p bytes=%zu\n",
                   identity, display, sizeof(*owner) );
}

static struct x11drv_display_owner *create_display_owner(void)
{
    struct client_surface_memory_scope memory = {0};
    struct x11drv_display_owner *owner = NULL;
    UINT64 domain;

    if (!x11drv_reserve_release_capacity( 1, sizeof(*owner) )) return NULL;
    domain = client_surface_allocate_completion_domains( 1 );
    if (!domain || !client_surface_memory_scope_init( &memory, 0, domain )) goto failed;
    if (!(owner = client_surface_alloc_scoped_metadata( &memory, 1, sizeof(*owner) ))) goto failed;
    owner->memory = memory;
    owner->refs = 1;
    list_init( &owner->native_errors );
    owner->close_work.execute = close_display_owner;
    owner->close_work.finished = free_display_owner;
    if (!client_surface_prepare_native_work()) goto failed;
    TRACE_(csperf)( "event=display_owner_reserve owner=%p domain=%llu bytes=%zu\n",
                   owner, (unsigned long long)domain, sizeof(*owner) );
    return owner;
failed:
    if (owner) client_surface_free_owned_metadata( &owner->memory, owner, sizeof(*owner) );
    else client_surface_memory_scope_destroy( &memory );
    x11drv_return_release_capacity( 1, sizeof(*owner) );
    return NULL;
}

struct x11drv_display_owner *x11drv_display_owner_acquire( struct x11drv_display_owner *owner )
{
    LONG refs = InterlockedIncrement( &owner->refs );

    assert( refs > 1 );
    TRACE_(csperf)( "event=display_acquire owner=%p display=%p refs=%u\n", owner, owner->display, (unsigned int)refs );
    return owner;
}

void x11drv_display_owner_release( struct x11drv_display_owner *owner )
{
    Display *display = owner->display;
    LONG refs = InterlockedDecrement( &owner->refs );

    assert( refs >= 0 );
    TRACE_(csperf)( "event=display_release owner=%p display=%p refs=%u\n", owner, display, (unsigned int)refs );
    if (refs) return;
    assert( owner->detached );
    TRACE_(csperf)( "event=display_close_queue owner=%p display=%p\n", owner, owner->display );
    client_surface_submit_native_work( &owner->close_work );
}

void x11drv_display_owner_set_clipboard( struct x11drv_display_owner *owner )
{
    pthread_mutex_lock( &error_handlers_mutex );
    owner->clipboard = TRUE;
    pthread_mutex_unlock( &error_handlers_mutex );
}

/***********************************************************************
 *           ThreadDetach (X11DRV.@)
 */
void X11DRV_ThreadDetach(void)
{
    struct x11drv_thread_data *data = x11drv_thread_data();

    if (data)
    {
        struct x11drv_display_owner *owner = data->display_owner;

        x11drv_window_thread_detach( data );
        xim_thread_detach( data );
        x11drv_clipboard_thread_detach( data );
        pthread_mutex_lock( &error_handlers_mutex );
        owner->clip_window = data->owns_clip_window ? data->clip_window : None;
        pthread_mutex_unlock( &error_handlers_mutex );
        x11drv_mouse_thread_detach( data );
        XSelectInput( data->display, DefaultRootWindow( data->display ), 0 );
        if (RootWindow( data->display, 0 ) != DefaultRootWindow( data->display ))
            XSelectInput( data->display, RootWindow( data->display, 0 ), 0 );
        if (data->net_supported) XFree( data->net_supported );
        XSync( gdi_display, False ); /* make sure XReparentWindow requests have completed before closing the thread display */
        XFlush( data->display );
        x11drv_native_window_thread_detach( data->display );
        owner->detached = TRUE;
        TRACE_(csperf)( "event=display_detach owner=%p display=%p refs=%u\n", owner, owner->display,
                       (unsigned int)InterlockedCompareExchange( &owner->refs, 0, 0 ) );
        x11drv_display_owner_release( owner );
        free( data );
        /* clear data in case we get re-entered from user32 before the thread is truly dead */
        pthread_setspecific( x11drv_thread_data_key, NULL );
    }
}


/* store the display fd into the message queue */
static void set_queue_display_fd( Display *display )
{
    HANDLE handle;
    int ret;

    if (wine_server_fd_to_handle( ConnectionNumber(display), GENERIC_READ | SYNCHRONIZE, 0, &handle ))
    {
        MESSAGE( "x11drv: Can't allocate handle for display fd\n" );
        NtTerminateProcess( 0, 1 );
    }
    SERVER_START_REQ( set_queue_fd )
    {
        req->handle = wine_server_obj_handle( handle );
        ret = wine_server_call( req );
    }
    SERVER_END_REQ;
    if (ret)
    {
        MESSAGE( "x11drv: Can't store handle for display fd\n" );
        NtTerminateProcess( 0, 1 );
    }
    NtClose( handle );
}


/***********************************************************************
 *           X11DRV thread initialisation routine
 */
struct x11drv_thread_data *x11drv_init_thread_data(void)
{
    struct x11drv_thread_data *data = x11drv_thread_data();

    if (data) return data;

    if (!(data = calloc( 1, sizeof(*data) )))
    {
        ERR( "could not create data\n" );
        NtTerminateProcess( 0, 1 );
    }
    list_init( &data->windows );
    if (!(data->display_owner = create_display_owner()))
    {
        free( data );
        ERR( "could not reserve Display ownership and retirement\n" );
        NtTerminateProcess( 0, 1 );
    }
    if (!(data->display = XOpenDisplay(NULL)))
    {
        free_display_owner( &data->display_owner->close_work );
        free( data );
        ERR_(winediag)( "x11drv: Can't open display: %s. Please ensure that your X server is running and that $DISPLAY is set correctly.\n", XDisplayName(NULL));
        NtTerminateProcess( 0, 1 );
    }

    data->display_owner->display = data->display;
    data->display_owner->errors.display = data->display;
    data->display_owner->errors.callback = display_owner_error;
    data->display_owner->errors.arg = data->display_owner;
    X11DRV_register_error_handler( &data->display_owner->errors );
    TRACE_(csperf)( "event=display_create owner=%p display=%p\n", data->display_owner, data->display );
    fcntl( ConnectionNumber(data->display), F_SETFD, 1 ); /* set close on exec flag */

    XkbUseExtension( data->display, NULL, NULL );
    XkbSetDetectableAutoRepeat( data->display, True, NULL );
    if (TRACE_ON(synchronous)) XSynchronize( data->display, True );

    set_queue_display_fd( data->display );
    pthread_setspecific( x11drv_thread_data_key, data );

    XSelectInput( data->display, DefaultRootWindow( data->display ), PropertyChangeMask );
    if (use_xim) xim_thread_attach( data );
    x11drv_xinput2_init( data );
    net_supported_init( data );
    net_active_window_init( data );

    return data;
}


/***********************************************************************
 *              SystemParametersInfo (X11DRV.@)
 */
BOOL X11DRV_SystemParametersInfo( UINT action, UINT int_param, void *ptr_param, UINT flags )
{
    switch (action)
    {
    case SPI_GETSCREENSAVEACTIVE:
        if (ptr_param)
        {
            int timeout, temp;
            XGetScreenSaver(gdi_display, &timeout, &temp, &temp, &temp);
            *(BOOL *)ptr_param = timeout != 0;
            return TRUE;
        }
        break;
    case SPI_SETSCREENSAVEACTIVE:
        {
            int timeout, interval, prefer_blanking, allow_exposures;
            static int last_timeout = 15 * 60;

            XLockDisplay( gdi_display );
            XGetScreenSaver(gdi_display, &timeout, &interval, &prefer_blanking,
                            &allow_exposures);
            if (timeout) last_timeout = timeout;

            timeout = int_param ? last_timeout : 0;
            XSetScreenSaver(gdi_display, timeout, interval, prefer_blanking,
                            allow_exposures);
            XUnlockDisplay( gdi_display );
        }
        break;
    }
    return FALSE;  /* let user32 handle it */
}
