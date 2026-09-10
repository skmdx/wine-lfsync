/*
 * Definitions for Wine GDI drivers
 *
 * Copyright 2011 Alexandre Julliard
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

#ifndef __WINE_WINE_GDI_DRIVER_H
#define __WINE_WINE_GDI_DRIVER_H

#ifndef WINE_UNIX_LIB
#error The GDI driver can only be used on the Unix side
#endif

#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>

#include <pthread.h>

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "ntuser.h"
#include "immdev.h"
#include "shellapi.h"
#include "ddk/d3dkmthk.h"
#include "kbd.h"
#include "wine/list.h"
#include "wine/debug.h"
#include "wine/client_surface.h"

struct gdi_dc_funcs;
struct opengl_funcs;
struct vulkan_funcs;

struct window_rects
{
    RECT window;    /* window area, including non-client frame */
    RECT client;    /* client area, excluding non-client frame */
    RECT visible;   /* area currently visible on the host screen, backed with a surface */
};

static inline const char *debugstr_window_rects( const struct window_rects *rects )
{
    return wine_dbg_sprintf( "{ window %s, client %s, visible %s }", wine_dbgstr_rect( &rects->window ),
                             wine_dbgstr_rect( &rects->client ), wine_dbgstr_rect( &rects->visible ) );
}

/* convert a visible rect to the corresponding window rect, using the window_rects offsets */
static inline RECT window_rect_from_visible( const struct window_rects *rects, RECT visible_rect )
{
    RECT rect = visible_rect;

    rect.left += rects->window.left - rects->visible.left;
    rect.top += rects->window.top - rects->visible.top;
    rect.right += rects->window.right - rects->visible.right;
    rect.bottom += rects->window.bottom - rects->visible.bottom;

    return rect;
}

/* convert a window rect to the corresponding visible rect, using the window_rects offsets */
static inline RECT visible_rect_from_window( struct window_rects *rects, RECT window_rect )
{
    RECT rect = window_rect;

    rect.left += rects->visible.left - rects->window.left;
    rect.top += rects->visible.top - rects->window.top;
    rect.right += rects->visible.right - rects->window.right;
    rect.bottom += rects->visible.bottom - rects->window.bottom;

    return rect;
}

typedef struct gdi_physdev
{
    const struct gdi_dc_funcs *funcs;
    struct gdi_physdev        *next;
    HDC                        hdc;
} *PHYSDEV;

struct bitblt_coords
{
    int  log_x;     /* original position and size, in logical coords */
    int  log_y;
    int  log_width;
    int  log_height;
    int  x;         /* mapped position and size, in device coords */
    int  y;
    int  width;
    int  height;
    RECT visrect;   /* rectangle clipped to the visible part, in device coords */
    DWORD layout;   /* DC layout */
};

struct gdi_image_bits
{
    void   *ptr;       /* pointer to the bits */
    BOOL    is_copy;   /* whether this is a copy of the bits that can be modified */
    void  (*free)(struct gdi_image_bits *);  /* callback for freeing the bits */
    void   *param;     /* extra parameter for callback private use */
};

struct brush_pattern
{
    BITMAPINFO           *info;     /* DIB info */
    struct gdi_image_bits bits;     /* DIB bits */
    UINT                  usage;    /* color usage for DIB info */
};

typedef int (*font_enum_proc)(const LOGFONTW *, const TEXTMETRICW *, DWORD, LPARAM);

struct gdi_dc_funcs
{
    INT      (*pAbortDoc)(PHYSDEV);
    BOOL     (*pAbortPath)(PHYSDEV);
    BOOL     (*pAlphaBlend)(PHYSDEV,struct bitblt_coords*,PHYSDEV,struct bitblt_coords*,BLENDFUNCTION);
    BOOL     (*pAngleArc)(PHYSDEV,INT,INT,DWORD,FLOAT,FLOAT);
    BOOL     (*pArc)(PHYSDEV,INT,INT,INT,INT,INT,INT,INT,INT);
    BOOL     (*pArcTo)(PHYSDEV,INT,INT,INT,INT,INT,INT,INT,INT);
    BOOL     (*pBeginPath)(PHYSDEV);
    DWORD    (*pBlendImage)(PHYSDEV,BITMAPINFO*,const struct gdi_image_bits*,struct bitblt_coords*,struct bitblt_coords*,BLENDFUNCTION);
    BOOL     (*pChord)(PHYSDEV,INT,INT,INT,INT,INT,INT,INT,INT);
    BOOL     (*pCloseFigure)(PHYSDEV);
    BOOL     (*pCreateCompatibleDC)(PHYSDEV,PHYSDEV*);
    BOOL     (*pCreateDC)(PHYSDEV*,LPCWSTR,LPCWSTR,const DEVMODEW*);
    BOOL     (*pDeleteDC)(PHYSDEV);
    BOOL     (*pDeleteObject)(PHYSDEV,HGDIOBJ);
    BOOL     (*pEllipse)(PHYSDEV,INT,INT,INT,INT);
    INT      (*pEndDoc)(PHYSDEV);
    INT      (*pEndPage)(PHYSDEV);
    BOOL     (*pEndPath)(PHYSDEV);
    BOOL     (*pEnumFonts)(PHYSDEV,LPLOGFONTW,font_enum_proc,LPARAM);
    INT      (*pExtEscape)(PHYSDEV,INT,INT,LPCVOID,INT,LPVOID);
    BOOL     (*pExtFloodFill)(PHYSDEV,INT,INT,COLORREF,UINT);
    BOOL     (*pExtTextOut)(PHYSDEV,INT,INT,UINT,const RECT*,LPCWSTR,UINT,const INT*);
    BOOL     (*pFillPath)(PHYSDEV);
    BOOL     (*pFillRgn)(PHYSDEV,HRGN,HBRUSH);
    BOOL     (*pFontIsLinked)(PHYSDEV);
    BOOL     (*pFrameRgn)(PHYSDEV,HRGN,HBRUSH,INT,INT);
    UINT     (*pGetBoundsRect)(PHYSDEV,RECT*,UINT);
    BOOL     (*pGetCharABCWidths)(PHYSDEV,UINT,UINT,WCHAR*,LPABC);
    BOOL     (*pGetCharABCWidthsI)(PHYSDEV,UINT,UINT,WORD*,LPABC);
    BOOL     (*pGetCharWidth)(PHYSDEV,UINT,UINT,const WCHAR*,LPINT);
    BOOL     (*pGetCharWidthInfo)(PHYSDEV,void*);
    INT      (*pGetDeviceCaps)(PHYSDEV,INT);
    UINT     (*pGetDeviceGammaRamp)(PHYSDEV,LPVOID);
    DWORD    (*pGetFontData)(PHYSDEV,DWORD,DWORD,LPVOID,DWORD);
    BOOL     (*pGetFontRealizationInfo)(PHYSDEV,void*);
    DWORD    (*pGetFontUnicodeRanges)(PHYSDEV,LPGLYPHSET);
    DWORD    (*pGetGlyphIndices)(PHYSDEV,LPCWSTR,INT,LPWORD,DWORD);
    DWORD    (*pGetGlyphOutline)(PHYSDEV,UINT,UINT,LPGLYPHMETRICS,DWORD,LPVOID,const MAT2*);
    DWORD    (*pGetImage)(PHYSDEV,BITMAPINFO*,struct gdi_image_bits*,struct bitblt_coords*);
    DWORD    (*pGetKerningPairs)(PHYSDEV,DWORD,LPKERNINGPAIR);
    COLORREF (*pGetNearestColor)(PHYSDEV,COLORREF);
    UINT     (*pGetOutlineTextMetrics)(PHYSDEV,UINT,LPOUTLINETEXTMETRICW);
    COLORREF (*pGetPixel)(PHYSDEV,INT,INT);
    UINT     (*pGetSystemPaletteEntries)(PHYSDEV,UINT,UINT,LPPALETTEENTRY);
    UINT     (*pGetTextCharsetInfo)(PHYSDEV,LPFONTSIGNATURE,DWORD);
    BOOL     (*pGetTextExtentExPoint)(PHYSDEV,LPCWSTR,INT,LPINT);
    BOOL     (*pGetTextExtentExPointI)(PHYSDEV,const WORD*,INT,LPINT);
    INT      (*pGetTextFace)(PHYSDEV,INT,LPWSTR);
    BOOL     (*pGetTextMetrics)(PHYSDEV,TEXTMETRICW*);
    BOOL     (*pGradientFill)(PHYSDEV,TRIVERTEX*,ULONG,void*,ULONG,ULONG);
    BOOL     (*pInvertRgn)(PHYSDEV,HRGN);
    BOOL     (*pLineTo)(PHYSDEV,INT,INT);
    BOOL     (*pMoveTo)(PHYSDEV,INT,INT);
    BOOL     (*pPaintRgn)(PHYSDEV,HRGN);
    BOOL     (*pPatBlt)(PHYSDEV,struct bitblt_coords*,DWORD);
    BOOL     (*pPie)(PHYSDEV,INT,INT,INT,INT,INT,INT,INT,INT);
    BOOL     (*pPolyBezier)(PHYSDEV,const POINT*,DWORD);
    BOOL     (*pPolyBezierTo)(PHYSDEV,const POINT*,DWORD);
    BOOL     (*pPolyDraw)(PHYSDEV,const POINT*,const BYTE *,DWORD);
    BOOL     (*pPolyPolygon)(PHYSDEV,const POINT*,const INT*,UINT);
    BOOL     (*pPolyPolyline)(PHYSDEV,const POINT*,const DWORD*,DWORD);
    BOOL     (*pPolylineTo)(PHYSDEV,const POINT*,INT);
    DWORD    (*pPutImage)(PHYSDEV,HRGN,BITMAPINFO*,const struct gdi_image_bits*,struct bitblt_coords*,struct bitblt_coords*,DWORD);
    UINT     (*pRealizeDefaultPalette)(PHYSDEV);
    UINT     (*pRealizePalette)(PHYSDEV,HPALETTE,BOOL);
    BOOL     (*pRectangle)(PHYSDEV,INT,INT,INT,INT);
    BOOL     (*pResetDC)(PHYSDEV,const DEVMODEW*);
    BOOL     (*pRoundRect)(PHYSDEV,INT,INT,INT,INT,INT,INT);
    HBITMAP  (*pSelectBitmap)(PHYSDEV,HBITMAP);
    HBRUSH   (*pSelectBrush)(PHYSDEV,HBRUSH,const struct brush_pattern*);
    HFONT    (*pSelectFont)(PHYSDEV,HFONT,UINT*);
    HPEN     (*pSelectPen)(PHYSDEV,HPEN,const struct brush_pattern*);
    COLORREF (*pSetBkColor)(PHYSDEV,COLORREF);
    UINT     (*pSetBoundsRect)(PHYSDEV,RECT*,UINT);
    COLORREF (*pSetDCBrushColor)(PHYSDEV, COLORREF);
    COLORREF (*pSetDCPenColor)(PHYSDEV, COLORREF);
    INT      (*pSetDIBitsToDevice)(PHYSDEV,INT,INT,DWORD,DWORD,INT,INT,UINT,UINT,LPCVOID,BITMAPINFO*,UINT);
    VOID     (*pSetDeviceClipping)(PHYSDEV,HRGN);
    UINT     (*pSetDeviceGammaRamp)(PHYSDEV,LPVOID);
    COLORREF (*pSetPixel)(PHYSDEV,INT,INT,COLORREF);
    COLORREF (*pSetTextColor)(PHYSDEV,COLORREF);
    INT      (*pStartDoc)(PHYSDEV,const DOCINFOW*);
    INT      (*pStartPage)(PHYSDEV);
    BOOL     (*pStretchBlt)(PHYSDEV,struct bitblt_coords*,PHYSDEV,struct bitblt_coords*,DWORD);
    INT      (*pStretchDIBits)(PHYSDEV,INT,INT,INT,INT,INT,INT,INT,INT,const void*,BITMAPINFO*,UINT,DWORD);
    BOOL     (*pStrokeAndFillPath)(PHYSDEV);
    BOOL     (*pStrokePath)(PHYSDEV);
    BOOL     (*pUnrealizePalette)(HPALETTE);

    /* priority order for the driver on the stack */
    UINT       priority;
};

/* increment this when changing driver tables or shared driver-facing structures */
#define WINE_GDI_DRIVER_VERSION 148

#define GDI_PRIORITY_NULL_DRV        0  /* null driver */
#define GDI_PRIORITY_FONT_DRV      100  /* any font driver */
#define GDI_PRIORITY_GRAPHICS_DRV  200  /* any graphics driver */
#define GDI_PRIORITY_DIB_DRV       300  /* the DIB driver */
#define GDI_PRIORITY_PATH_DRV      400  /* the path driver */

static inline PHYSDEV get_physdev_entry_point( PHYSDEV dev, size_t offset )
{
    while (!((void **)dev->funcs)[offset / sizeof(void *)]) dev = dev->next;
    return dev;
}

#define GET_NEXT_PHYSDEV(dev,func) \
    get_physdev_entry_point( (dev)->next, FIELD_OFFSET(struct gdi_dc_funcs,func))

static inline void push_dc_driver( PHYSDEV *dev, PHYSDEV physdev, const struct gdi_dc_funcs *funcs )
{
    while ((*dev)->funcs->priority > funcs->priority) dev = &(*dev)->next;
    physdev->funcs = funcs;
    physdev->next = *dev;
    physdev->hdc = (*dev)->hdc;
    *dev = physdev;
}

/* support for client surfaces */

struct client_surface;
struct client_surface_scene;

/* Producer-private native storage metadata. Preparation and capture may change
 * it before completion; only the core's completed frame enters the channel. */
struct client_surface_source
{
    LONG64 reservation; /* producer-private token, zero once completed/abandoned */
    UINT64 publication; /* consumer sequence which releases this image */
    BOOL published;
    UINT64 source;
    UINT64 source_visual;
    UINT64 target_epoch;
    UINT width, height;
    UINT flags;
};
/* Driver-ready target. seq protects the complete geometry snapshot; epoch
 * identifies the native lifetime against which a producer submits frames. */
struct client_surface_target
{
    LONG64 seq;
    UINT64 epoch;
    HWND toplevel;
    RECT virtual_rect;
    RECT monitor_rect;
    UINT dpi_num;
    UINT dpi_den;
    enum client_surface_presentation_mode mode;
    LONG offscreen;
    LONG valid;
};

enum client_surface_target_update
{
    CLIENT_SURFACE_TARGET_UPDATE_DEFAULT,   /* invalidate when the snapshot changes */
    CLIENT_SURFACE_TARGET_UPDATE_PRESERVED, /* backend preserved the native target */
    CLIENT_SURFACE_TARGET_UPDATE_CHANGED,   /* native change, even with identical geometry */
};

enum client_surface_backend_caps
{
    CLIENT_SURFACE_BACKEND_SCENE_PUBLICATION = 0x01,
    /* present() only queries the supplied DC; all drawing uses private state. */
    CLIENT_SURFACE_BACKEND_READ_ONLY_DC = 0x04,
    CLIENT_SURFACE_BACKEND_DIRECT_PRESENTATION = 0x08,
    CLIENT_SURFACE_BACKEND_GENERATION_HANDOFF = 0x10,
    /* Offscreen presentation is owner-only; present() must never write an
     * owner native target when generation handoff is unavailable. */
    CLIENT_SURFACE_BACKEND_OWNER_COMPOSITOR = 0x20,
    CLIENT_SURFACE_BACKEND_OWNER_SCENE_PLAN = 0x40,
};

struct client_surface_completion_ops
{
    /* Arm and poll a host presentation boundary. PENDING preserves the same
     * monitor. Cancel is only valid before entering native submission. */
    BOOL (*prepare)( struct client_surface *surface );
    struct client_surface_completion_result (*wait)( struct client_surface *surface, DWORD timeout );
    void (*cancel)( struct client_surface *surface );
    /* retire an armed boundary when host submission may have partially failed */
    void (*abandon)( struct client_surface *surface );
};

/* Retirement takes this mapped endpoint and notification fd together. Native
 * readers may outlive the producer surface; only retirement releases the lease. */
struct client_surface_handoff_lease
{
    void *view;
    struct client_surface_handoff_channel *channel;
    UINT64 cookie;
    int ready_fd;
};

struct client_surface_backend
{
    unsigned int caps;
    /* Resource and presentation hooks are optional.  An omitted lifecycle
     * hook is a no-op; omitted update and present hooks succeed immediately. */
    void (*destroy)( struct client_surface *surface );
    /* detach the surface from its window, called from window owner thread */
    void (*detach)( struct client_surface *surface );
    /* backend-local geometry and clipping allow the server-selected DIRECT mode */
    BOOL (*direct_ready)( struct client_surface *surface );
    BOOL (*prepare_direct)( struct client_surface *surface, const struct client_surface_scene *scene );
    void (*complete_direct)( struct client_surface *surface, const struct client_surface_frame *frame );
    /* Prepare target for publication, reporting native mutations separately
     * from geometry. Omitted reports retain conservative invalidation. */
    BOOL (*update)( struct client_surface *surface, struct client_surface_target *target,
                    enum client_surface_target_update *update );
    /* present the client surface if necessary, hdc != NULL when offscreen, called from render thread;
     * flush requires host completion before returning, defer_visible keeps a scene generation staged */
    BOOL (*present)( struct client_surface *surface, const struct client_surface_scene *scene,
                     HDC hdc, HRGN surface_region, BOOL flush, BOOL defer_visible );
    /* Prepare producer-private storage. This does not publish a frame. */
    BOOL (*handoff_prepare)( struct client_surface *surface,
                             struct client_surface_source *source, const struct client_surface_frame *frame );
    /* Transfer a prepared source operation to this capture. Read owns its
     * storage without surface locks; apply and release only change references.
     * The core retains native submission order and revalidates before apply. */
    BOOL (*handoff_capture)( struct client_surface *surface, struct client_surface_frame *frame,
                             struct client_surface_capture *capture );
    /* Freeze a completed native drawable into independent producer storage. */
    BOOL (*handoff_complete)( struct client_surface *surface,
                              struct client_surface_source *source, unsigned int index );
    BOOL (*handoff_serialize)( struct client_surface *surface );
    /* Take ownership of the mapped handoff, its notification fd and source
     * storage. Retire them after readers finish, without retaining surface. */
    void (*handoff_retire)( struct client_surface *surface, const struct client_surface_handoff_lease *lease );
    const struct client_surface_completion_ops *completion;
};

struct client_surface_scene
{
    UINT64 generation;
    UINT64 epoch;
    UINT64 native_candidate; /* may prepare DIRECT, but owns no completed image */
    UINT64 producer_sequence; /* completed selection on the producer's HWND */
    HWND toplevel;
    enum client_surface_presentation_mode mode;
    BOOL valid;
    BOOL authoritative;
    BOOL source_pending; /* current assembly awaits the owner's image inventory */
    BOOL direct_candidate; /* server eligibility; the owner still selects the strategy */
    BOOL publication_pending; /* assembly accepted; native output or GUI exposure still owns its token */
};

enum client_surface_frame_result
{
    CLIENT_SURFACE_FRAME_PENDING,
    CLIENT_SURFACE_FRAME_COMPLETION_FAILED,
    CLIENT_SURFACE_FRAME_SUPERSEDED,
};

enum client_surface_frame_target
{
    CLIENT_SURFACE_FRAME_TARGET_INVALID,
    CLIENT_SURFACE_FRAME_TARGET_ONSCREEN,
    CLIENT_SURFACE_FRAME_TARGET_OFFSCREEN,
};

struct client_surface_frame
{
    struct client_surface_scene scene;
    enum client_surface_presentation_mode mode;
    LONG64 serial;
    DWORD submission_time;
    UINT64 target_epoch;
    UINT64 memory_domain; /* native allocation domain; resources retain their own account references */
    enum client_surface_frame_target target;
    BOOL replay; /* publish retained completed storage without another native source read */
    UINT64 handoff_control;
    unsigned int handoff_index;
    /* Borrowed from the prepared reservation until its completion/capture is
     * released. Native callbacks retain no access to the producer registry. */
    struct client_surface_source *handoff_source;
    struct client_surface_handoff_channel *handoff_channel;
    RECT damage;
    UINT64 damage_base_sequence;
    struct client_surface_completion completion;
    struct client_surface_capture capture;
    /* An unused admission belongs to this frame until defer or completion. */
    struct client_surface_completion_job *completion_job;
    enum client_surface_frame_result result;
};

/* Backend completion and publication must share one bounded wait contract. */
#define CLIENT_SURFACE_PRESENT_TIMEOUT 5000

enum client_surface_memory_class
{
    CLIENT_SURFACE_MEMORY_SOURCE,
    CLIENT_SURFACE_MEMORY_STAGING,
    CLIENT_SURFACE_MEMORY_OUTPUT,
    CLIENT_SURFACE_MEMORY_CLASS_COUNT,
};

struct client_surface_memory_account;

/* A resource keeps its original allocation owners through retirement. A copy
 * acquires new account references, but never copies another resource's charge. */
struct client_surface_memory_scope
{
    struct client_surface_memory_account *owner, *domain;
    UINT64 used[CLIENT_SURFACE_MEMORY_CLASS_COUNT + 1];
};

W32KAPI BOOL client_surface_memory_scope_init( struct client_surface_memory_scope *scope, HWND hwnd, UINT64 domain );
W32KAPI void client_surface_memory_scope_copy( struct client_surface_memory_scope *dst,
                                              const struct client_surface_memory_scope *src, BOOL include_owner );
W32KAPI void client_surface_memory_scope_destroy( struct client_surface_memory_scope *scope );
W32KAPI BOOL client_surface_reserve_scoped_memory( struct client_surface_memory_scope *scope,
                                                  enum client_surface_memory_class type, UINT64 bytes );
W32KAPI void client_surface_release_scoped_memory( struct client_surface_memory_scope *scope,
                                                  enum client_surface_memory_class type, UINT64 bytes );
W32KAPI BOOL client_surface_reserve_scoped_metadata( struct client_surface_memory_scope *scope, UINT64 bytes );
W32KAPI void client_surface_release_scoped_metadata( struct client_surface_memory_scope *scope, UINT64 bytes );

W32KAPI void client_surface_fail_scene( HWND hwnd );

W32KAPI BOOL client_surface_reserve_memory( enum client_surface_memory_class type, UINT64 bytes );
W32KAPI void client_surface_release_memory( enum client_surface_memory_class type, UINT64 bytes );
W32KAPI BOOL client_surface_reserve_metadata_memory( UINT64 bytes );
W32KAPI void client_surface_release_metadata_memory( UINT64 bytes );

/* Transfer and retirement metadata has protected admission within the image
 * budget. Callers retain the allocated byte count when a native enumeration
 * later changes its count. Native use must finish before the actual free. */
static inline void *client_surface_alloc_scoped_metadata( struct client_surface_memory_scope *scope,
                                                          SIZE_T count, SIZE_T size )
{
    void *data;

    if (size && count > ~(SIZE_T)0 / size) return NULL;
    size *= count;
    if (!client_surface_reserve_scoped_metadata( scope, size )) return NULL;
    if (!(data = calloc( 1, size ))) client_surface_release_scoped_metadata( scope, size );
    return data;
}

static inline void client_surface_free_scoped_metadata( struct client_surface_memory_scope *scope,
                                                         void *data, SIZE_T size )
{
    if (!data) return;
    free( data );
    client_surface_release_scoped_metadata( scope, size );
}

static inline void *client_surface_alloc_metadata( SIZE_T count, SIZE_T size )
{
    return client_surface_alloc_scoped_metadata( NULL, count, size );
}

static inline void client_surface_free_metadata( void *data, SIZE_T size )
{
    client_surface_free_scoped_metadata( NULL, data, size );
}

/* The object embeds its own scope. Its native resources have already been
 * released; preserve only the final metadata charge across the actual free. */
static inline void client_surface_free_owned_metadata( struct client_surface_memory_scope *scope,
                                                        void *data, SIZE_T size )
{
    struct client_surface_memory_scope memory;

    if (!data) return;
    memory = *scope;
    client_surface_free_scoped_metadata( &memory, data, size );
    client_surface_memory_scope_destroy( &memory );
}

struct client_surface
{
    const struct client_surface_backend *backend;
    struct list                        entry;          /* entry in win32u managed list */
    DECLSPEC_ALIGN(8) UINT64            identity;       /* server-issued surface lifetime, atomic */
    struct client_surface             *identity_next; /* process-local identity hash chain */
    struct client_surface             *toplevel_next; /* driver-ready top-level hash chain */
    HWND                               indexed_toplevel;
    pthread_mutex_t                    present_lock;   /* serializes driver operations for this surface */
    pthread_mutex_t                    completion_lock; /* protects host completion state */
    pthread_cond_t                     completion_cond; /* completion-mode and target handoff */
    /* Private scheduler state; only the completion module owns its FIFO and
     * admission counts, independently of native presentation locks. */
    struct client_surface_completion_queue *completion_queue;
    LONG                               ref;            /* reference count */
    HWND                               hwnd;           /* window the surface was created for */
    int                                format;         /* pixel format of the surface */
    LONG                               updated;        /* has been moved / resized / reparented */
    struct client_surface_target       target;         /* driver-ready native target snapshot */
    LONG                               active;         /* registered as active with the Wine server */
    LONG                               content_valid;  /* complete content exists at the current size */
    LONG                               direct_ready;   /* backend-local DIRECT eligibility advertised to server */
    LONG                               cacheable;      /* native completion state is safe to reuse */
    LONG                               server_cached;  /* registered as a cached owner with the Wine server */
    LONG                               target_update_pending; /* coalesced owner-to-present geometry handoff */
    UINT64                             cache_cost;     /* estimated bytes while on the unused list */
    UINT64                             target_scene_epoch; /* last server scene applied to native target */
    enum client_surface_presentation_mode target_scene_mode; /* last server mode applied to native target */
    LONG64                             present_serial; /* producer submission order */
    LONG64                             composed_serial; /* newest source accepted or invalidated */
    LONG64                             completed_image_serial; /* independently frozen image, not mutable DIRECT content */
    LONG                               external_completion_count; /* causal tokens currently in flight */
    LONG                               driver_completion_count; /* shared native monitor tokens in flight */
    LONG                               driver_completion_waiters; /* pending shared-monitor mode transitions */
    LONG                               native_present_count; /* native presentation calls currently in flight */
    LONG                               target_update_waiters; /* pending native target mutations */
    LONG64                             recompose_seq;  /* latest requested cached replay */
    LONG64                             recompose_done; /* latest completed cached replay */
    LONG                               recompose_queued; /* a consumer owns the pending request */
    LONG64                             scene_retry_generation; /* newest server generation granted one retry */
    UINT64                             clip_scene_epoch; /* scene owning the cached cross-process clip */
    LONG64                             clip_target_seq;
    HRGN                               clip_region;
    BOOL                               clip_region_valid;
    struct client_surface_handoff      *handoff;        /* private producer transport and source reservations */
    BOOL                               raw;            /* use the raw physical position and size for the host client surface */
};

static inline BOOL client_surface_backend_has_cap( const struct client_surface *surface,
                                                   enum client_surface_backend_caps cap )
{
    return !!(surface->backend->caps & cap);
}

W32KAPI void *client_surface_create( UINT size, const struct client_surface_backend *backend,
                                    HWND hwnd, int format, BOOL raw );
W32KAPI BOOL client_surface_update( struct client_surface *surface );
W32KAPI void client_surface_add_ref( struct client_surface *surface );
W32KAPI void client_surface_release( struct client_surface *surface );
/* Timed conditions pair this initializer with the monotonic/relative wait. */
W32KAPI int client_surface_cond_init( pthread_cond_t *cond );
W32KAPI int client_surface_cond_timedwait( pthread_cond_t *cond, pthread_mutex_t *mutex, DWORD timeout );
W32KAPI void client_surface_present( struct client_surface *surface );
/* Success holds submission ordering until begin_present. Failure leaves no
 * lock or reservation owned by the caller and has not submitted native work. */
W32KAPI BOOL client_surface_prepare_present( struct client_surface *surface,
                                              struct client_surface_frame *present,
                                              BOOL external_completion, BOOL asynchronous );
W32KAPI void client_surface_begin_present( struct client_surface *surface );
W32KAPI void client_surface_submit_present( struct client_surface *surface,
                                             struct client_surface_frame *present );
W32KAPI void client_surface_submit_present_locked( struct client_surface *surface,
                                                    struct client_surface_frame *present );
W32KAPI BOOL client_surface_complete_present( struct client_surface *surface,
                                              struct client_surface_frame *present,
                                              BOOL submitted, BOOL external_completed,
                                              const SIZE *expected_size, DWORD timeout );
W32KAPI struct client_surface_completion_result client_surface_wait_present_completion( struct client_surface *surface,
                                                      const struct client_surface_frame *present,
                                                      DWORD timeout );
W32KAPI void client_surface_set_present_completion( struct client_surface_frame *present,
                                                     client_surface_completion_wait_func wait,
                                                     client_surface_completion_release_func release,
                                                     void *context );
W32KAPI void client_surface_defer_present( struct client_surface *surface,
                                           struct client_surface_frame *present,
                                           const SIZE *expected_size );
/* Reserve before any native submission and outside every surface/native lock.
 * Cancellation returns an unused reservation; defer transfers frame ownership. */
W32KAPI struct client_surface_completion_job *client_surface_reserve_completion( struct client_surface *surface );
/* Native source I/O may share transport within this execution domain. Keys
 * are process-local, never reused, and do not identify an image or its owner.
 * Synchronous callers have a distinct domain from asynchronous completion. */
W32KAPI BOOL client_surface_get_execution_domain( UINT64 *domain );
W32KAPI void client_surface_cancel_completion( struct client_surface_completion_job *job );
W32KAPI void client_surface_defer_reserved_present( struct client_surface_completion_job *job,
                                                   struct client_surface_frame *present,
                                                   const SIZE *expected_size );
W32KAPI void client_surface_lock_present( struct client_surface *surface );
W32KAPI void client_surface_unlock_present( struct client_surface *surface );
W32KAPI void client_surface_prepare_present_locked( struct client_surface *surface,
                                                    struct client_surface_frame *present,
                                                    BOOL external_completion, UINT64 memory_domain );
W32KAPI BOOL client_surface_complete_present_locked( struct client_surface *surface,
                                                     struct client_surface_frame *present,
                                                     BOOL submitted, BOOL external_completed,
                                                     const SIZE *expected_size, DWORD timeout );
W32KAPI void client_surface_geometry_ready( HWND hwnd );
W32KAPI void client_surface_repair_owner( HWND hwnd );
W32KAPI void client_surface_resolve_sources( HWND hwnd );
W32KAPI BOOL client_surface_get_toplevel_scene( HWND toplevel, struct client_surface_scene *scene );
struct client_surface_scene_member
{
    HWND hwnd;
    UINT process;
    UINT64 identity, cookie;
    BOOL visible;
    BOOL direct_candidate;
    BOOL producer_mapped;
    struct client_surface_target target;
    HRGN region;
};
W32KAPI BOOL client_surface_get_scene_snapshot( HWND toplevel, UINT64 *scene_id, UINT *count,
                                                struct client_surface_scene_member **members );
W32KAPI BOOL client_surface_scene_snapshot_current( HWND toplevel, UINT64 scene_id );
W32KAPI void client_surface_free_scene_snapshot( UINT count, struct client_surface_scene_member *members );
W32KAPI void client_surface_set_staged( HWND hwnd );
W32KAPI void client_surface_bypass_staging( HWND hwnd );
W32KAPI BOOL client_surface_begin_native_barrier( HWND hwnd, UINT_PTR token );
W32KAPI BOOL client_surface_end_native_barrier( HWND hwnd, UINT_PTR token );
W32KAPI UINT client_surface_begin_publish( HWND hwnd, UINT64 *generation, UINT64 *scene_generation );
W32KAPI BOOL client_surface_end_publish( HWND hwnd, UINT64 generation, UINT64 scene_generation, BOOL success );
W32KAPI BOOL client_surface_begin_prepare( HWND hwnd, UINT64 *scene_generation );
W32KAPI void client_surface_end_prepare( HWND hwnd, UINT64 scene_generation );
W32KAPI void update_client_surfaces( HWND hwnd );
W32KAPI void detach_client_surfaces( HWND hwnd );

static inline const char *debugstr_client_surface( struct client_surface *surface )
{
    if (!surface) return "(null)";
    return wine_dbg_sprintf( "%p/%p", surface->hwnd, surface );
}

/* support for window surfaces */

struct window_surface;

struct window_surface_funcs
{
    void  (*set_clip)( struct window_surface *surface, const RECT *rects, UINT count );
    BOOL  (*flush)( struct window_surface *surface, const RECT *rect, const RECT *dirty,
                    const BITMAPINFO *color_info, const void *color_bits, BOOL shape_changed,
                    const BITMAPINFO *shape_info, const void *shape_bits );
    void  (*destroy)( struct window_surface *surface );
};

struct window_surface
{
    const struct window_surface_funcs *funcs; /* driver-specific implementations  */
    struct list                        entry; /* entry in global list managed by user32 */
    LONG                               ref;   /* reference count */
    HWND                               hwnd;  /* window the surface was created for */
    RECT                               rect;  /* constant, no locking needed */

    pthread_mutex_t                    mutex;        /* mutex needed for any field below */
    RECT                               bounds;       /* dirty area rectangle */
    HRGN                               clip_region;  /* visible region of the surface, fully visible if 0 */
    DWORD                              draw_start_ticks; /* start ticks of fresh draw */
    COLORREF                           color_key;    /* layered window surface color key, invalid if CLR_INVALID */
    UINT                               alpha_bits;   /* layered window global alpha bits, invalid if -1 */
    UINT                               alpha_mask;   /* layered window per-pixel alpha mask, invalid if 0 */
    HRGN                               shape_region; /* shape of the window surface, unshaped if 0 */
    HBITMAP                            shape_bitmap; /* bitmap for the surface shape (1bpp) */
    HBITMAP                            color_bitmap; /* bitmap for the surface colors */
    /* driver-specific fields here */
};

W32KAPI struct window_surface *window_surface_create( UINT size, const struct window_surface_funcs *funcs, HWND hwnd,
                                                      const RECT *rect, BITMAPINFO *info, HBITMAP bitmap );
W32KAPI void window_surface_add_ref( struct window_surface *surface );
W32KAPI void window_surface_release( struct window_surface *surface );
W32KAPI void window_surface_set_shape( struct window_surface *surface, HRGN shape_region );

/* display manager interface, used to initialize display device registry data */

struct pci_id
{
    UINT16 vendor;
    UINT16 device;
    UINT16 subsystem;
    UINT16 revision;
};

struct gdi_monitor
{
    RECT rc_monitor;      /* RcMonitor in MONITORINFO struct */
    RECT rc_work;         /* RcWork in MONITORINFO struct */
    unsigned char *edid;  /* Extended Device Identification Data */
    UINT edid_len;
    BOOL hdr_enabled;
};

struct gdi_device_manager
{
    void (*add_gpu)( const char *name, const struct pci_id *pci_id, const GUID *vulkan_uuid, void *param );
    void (*add_source)( const char *name, UINT state_flags, UINT dpi, void *param );
    void (*add_monitor)( const struct gdi_monitor *monitor, void *param );
    void (*add_modes)( const DEVMODEW *current, UINT modes_count, const DEVMODEW *modes, void *param );
};

#define WINE_DM_UNSUPPORTED 0x80000000
#define WINE_SWP_FULLSCREEN 0x80000000
#define WINE_SWP_RESIZABLE  0x40000000
#define WINE_SWP_CLIENT_SURFACE_PENDING 0x20000000
#define WINE_SWP_CLIENT_SURFACE_PUBLISH 0x10000000
#define WINE_SWP_CLIENT_SURFACE_BACKING_ENABLE 0x08000000
#define WINE_SWP_CLIENT_SURFACE_BACKING_DISABLE 0x04000000
#define WINE_SWP_CLIENT_SURFACE_PREPARE 0x02000000

struct vulkan_driver_funcs;
struct opengl_driver_funcs;

struct user_driver_funcs
{
    struct gdi_dc_funcs dc_funcs;

    /* keyboard functions */
    BOOL    (*pActivateKeyboardLayout)(HKL, UINT);
    void    (*pBeep)(void);
    INT     (*pGetKeyNameText)(LONG,LPWSTR,INT);
    UINT    (*pGetKeyboardLayoutList)(INT, HKL *);
    UINT    (*pMapVirtualKeyEx)(UINT,UINT,HKL);
    BOOL    (*pRegisterHotKey)(HWND,UINT,UINT);
    INT     (*pToUnicodeEx)(UINT,UINT,const BYTE *,LPWSTR,int,UINT,HKL);
    void    (*pUnregisterHotKey)(HWND, UINT, UINT);
    SHORT   (*pVkKeyScanEx)(WCHAR, HKL);
    const KBDTABLES *(*pKbdLayerDescriptor)(HKL);
    void    (*pReleaseKbdTables)(const KBDTABLES *);
    /* IME functions */
    UINT    (*pImeToAsciiEx)(UINT,UINT,const BYTE*,HIMC);
    void    (*pNotifyIMEStatus)(HWND,UINT);
    BOOL    (*pSetIMECompositionRect)(HWND,RECT);
    /* cursor/icon functions */
    void    (*pDestroyCursorIcon)(HCURSOR);
    void    (*pSetCursor)(HWND,HCURSOR);
    BOOL    (*pGetCursorPos)(LPPOINT);
    BOOL    (*pSetCursorPos)(INT,INT);
    BOOL    (*pClipCursor)(const RECT*,BOOL);
    /* notify icon functions */
    LRESULT (*pNotifyIcon)(HWND,UINT,NOTIFYICONDATAW *);
    void    (*pCleanupIcons)(HWND);
    void    (*pSystrayDockInit)(HWND);
    BOOL    (*pSystrayDockInsert)(HWND,UINT,UINT,void *);
    void    (*pSystrayDockClear)(HWND);
    BOOL    (*pSystrayDockRemove)(HWND);
    /* clipboard functions */
    LRESULT (*pClipboardWindowProc)(HWND,UINT,WPARAM,LPARAM);
    void    (*pUpdateClipboard)(void);
    /* display modes */
    LONG    (*pChangeDisplaySettings)(LPDEVMODEW,LPCWSTR,HWND,DWORD,LPVOID);
    UINT    (*pUpdateDisplayDevices)(const struct gdi_device_manager *,void*);
    /* windowing functions */
    BOOL    (*pCreateDesktop)(const WCHAR *,UINT,UINT);
    BOOL    (*pCreateWindow)(HWND);
    LRESULT (*pDesktopWindowProc)(HWND,UINT,WPARAM,LPARAM);
    void    (*pDestroyWindow)(HWND);
    void    (*pFlashWindowEx)(FLASHWINFO*);
    void    (*pGetDC)(HDC,HWND,HWND,const RECT *,const RECT *,DWORD,UINT,UINT);
    BOOL    (*pProcessEvents)(DWORD);
    void    (*pReleaseDC)(HWND,HDC);
    BOOL    (*pScrollDC)(HDC,INT,INT,HRGN);
    void    (*pSetCapture)(HWND,UINT,HWND);
    void    (*pSetDesktopWindow)(HWND);
    void    (*pActivateWindow)(HWND,HWND);
    void    (*pSetLayeredWindowAttributes)(HWND,COLORREF,BYTE,DWORD);
    void    (*pSetParent)(HWND,HWND,HWND);
    void    (*pSetWindowRgn)(HWND,HRGN,BOOL);
    void    (*pSetWindowIcons)(HWND,HICON,const ICONINFO*,HICON,const ICONINFO*);
    void    (*pSetWindowStyle)(HWND,INT,STYLESTRUCT*);
    void    (*pSetWindowText)(HWND,LPCWSTR);
    UINT    (*pShowWindow)(HWND,INT,RECT*,UINT);
    LRESULT (*pSysCommand)(HWND,WPARAM,LPARAM,const POINT*);
    void    (*pUpdateLayeredWindow)(HWND,BYTE,UINT);
    LRESULT (*pWindowMessage)(HWND,UINT,WPARAM,LPARAM);
    BOOL    (*pWindowPosChanging)(HWND,UINT,BOOL,const struct window_rects *);
    BOOL    (*pGetWindowStyleMasks)(HWND,UINT,UINT,UINT*,UINT*);
    BOOL    (*pGetWindowStateUpdates)(HWND,UINT*,UINT*,RECT*,HWND*);
    struct client_surface *(*pCreateClientSurface)(HWND,int,BOOL);
    /* TRUE if native owner images can service this repair without a producer. */
    BOOL    (*pRepairClientSurfaceOwner)(HWND,BOOL);
    /* Release staging after the actor completed this exact native scene. */
    BOOL    (*pExposeClientSurface)(HWND,UINT64);
    /* State-only backing update; STATUS_NOT_SUPPORTED requests a full window update. */
    NTSTATUS (*pUpdateClientSurfaceBacking)(HWND,BOOL,BOOL,const struct window_rects *);
    BOOL    (*pCreateWindowSurface)(HWND,BOOL,const RECT *,struct window_surface**);
    void    (*pMoveWindowBits)(HWND,const struct window_rects *,const struct window_rects *,const RECT *);
    BOOL    (*pWindowPosChanged)(HWND,HWND,HWND,UINT,const struct window_rects*,struct window_surface*);
    /* system parameters */
    BOOL    (*pSystemParametersInfo)(UINT,UINT,void*,UINT);
    /* wintab support */
    LRESULT (*pWintabProc)(HWND,UINT,WPARAM,LPARAM,void*);
    /* vulkan support */
    UINT    (*pVulkanInit)(UINT,void *,const struct vulkan_driver_funcs **);
    /* opengl support */
    UINT    (*pOpenGLInit)(UINT,const struct opengl_funcs *,const struct opengl_driver_funcs **);
    /* thread management */
    void    (*pThreadDetach)(void);
};

W32KAPI void __wine_set_user_driver( const struct user_driver_funcs *funcs, UINT version );

#endif /* __WINE_WINE_GDI_DRIVER_H */
