/*
 * Server-side window handling
 *
 * Copyright (C) 2001 Alexandre Julliard
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

#include "config.h"

#include <assert.h>
#include <stdarg.h>
#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "ntuser.h"

#include "object.h"
#include "file.h"
#include "handle.h"
#include "request.h"
#include "thread.h"
#include "process.h"
#include "user.h"
#include "unicode.h"
#include "wine/client_surface.h"
#include "client_surface_handoff.h"

static const struct ratio no_dpi;

/* a window property */
struct property
{
    unsigned short type;     /* property type (see below) */
    atom_t         atom;     /* property atom */
    lparam_t       data;     /* property data (user-defined storage) */
};

enum property_type
{
    PROP_TYPE_FREE,   /* free entry */
    PROP_TYPE_STRING, /* atom that was originally a string */
    PROP_TYPE_ATOM    /* plain atom */
};

struct client_surface_owner
{
    struct list     entry;
    struct list     surfaces;
    struct process *process;
};

enum client_surface_destroy_state
{
    CLIENT_SURFACE_DESTROY_NONE,
    CLIENT_SURFACE_DESTROY_PENDING,
    CLIENT_SURFACE_DESTROY_QUEUED,
};

struct client_surface_ref
{
    struct list     entry;
    struct client_surface_ref *index_next;
    struct client_surface_owner *owner;
    struct process *process; /* queue callback keeps this valid for a retired tombstone */
    struct client_surface_handoff_binding *handoff;
    UINT64         id;
    unsigned long long generation;
    unsigned long long sequence;
    unsigned long long candidate_serial; /* uncompleted native preparation, zero when absent */
    unsigned int    active : 1;
    unsigned int    cached : 1;
    unsigned int    claimed : 1; /* an active surface which completed a host present */
    unsigned int    scene_publication : 1; /* renderer supports owner scene publication */
    unsigned int    direct_presentation : 1; /* renderer can attach its native source directly */
    unsigned int    notification_pending : 1; /* an update for this identity is queued */
    /* pending counts all selected images, including warm ones. This separate
     * obligation selects producer retries after the inventory was consumed
     * and notification_pending was cleared by message/thread teardown. */
    unsigned int    source_required : 1;
    unsigned int    destroy_state : 2; /* renderer destroy delivery state */
};

#define CLIENT_SURFACE_REF_BUCKETS 256
static struct client_surface_ref *client_surface_ref_index[CLIENT_SURFACE_REF_BUCKETS];
/* A truncated 32-bit notification must never name another valid lifetime. */
static UINT64 client_surface_id = (UINT64)1 << 32;

static void retire_client_surface_handoff( struct client_surface_ref *surface );
static void invalidate_client_surface_owner_repair( struct client_surface_ref *surface );
static int client_surface_direct_candidate( struct window *top );
static struct client_surface_ref *get_client_surface_native_candidate( struct window *top,
                                                                      struct client_surface_owner **owner );
static int client_surface_direct_registration_candidate( struct window *top );
static int client_surface_direct_eligible( struct window *top );
static enum client_surface_presentation_mode client_surface_presentation_mode( struct window *top );
static void update_window_first_child( struct window *win );
static void cancel_thread_window_paints( struct thread *thread );
static void cancel_window_paints( struct window *win );
static void wake_window_paint_prepare( struct window *top );

static unsigned int client_surface_ref_hash( struct process *process, UINT64 id )
{
    unsigned long long value = id;

    value ^= value >> 32;
    value ^= (UINT_PTR)process >> 4;
    return value & (CLIENT_SURFACE_REF_BUCKETS - 1);
}

static struct client_surface_ref *find_indexed_client_surface_ref( struct process *process,
                                                                   UINT64 id )
{
    struct client_surface_ref *surface;
    unsigned int bucket = client_surface_ref_hash( process, id );

    for (surface = client_surface_ref_index[bucket]; surface; surface = surface->index_next)
        if (surface->process == process && surface->id == id) return surface;
    return NULL;
}

static void insert_client_surface_ref_index( struct client_surface_ref *surface )
{
    unsigned int bucket = client_surface_ref_hash( surface->process, surface->id );

    assert( !find_indexed_client_surface_ref( surface->process, surface->id ) );
    surface->index_next = client_surface_ref_index[bucket];
    client_surface_ref_index[bucket] = surface;
}

static void remove_client_surface_ref_index( struct client_surface_ref *surface )
{
    unsigned int bucket = client_surface_ref_hash( surface->process, surface->id );
    struct client_surface_ref **cursor;

    for (cursor = &client_surface_ref_index[bucket]; *cursor; cursor = &(*cursor)->index_next)
    {
        if (*cursor != surface) continue;
        *cursor = surface->index_next;
        surface->index_next = NULL;
        return;
    }
    assert( 0 );
}

static int post_client_surface_notification( struct client_surface_ref *surface,
                                             user_handle_t window, unsigned int message )
{
    /* Both halves survive native 32-bit and WOW64 message delivery. */
    return post_process_message( surface->process, window, message,
                                 (UINT32)(surface->id >> 32), (UINT32)surface->id );
}

static void free_client_surface_ref_if_unused( struct client_surface_ref *surface )
{
    if (surface->owner || surface->handoff || surface->notification_pending ||
        surface->destroy_state != CLIENT_SURFACE_DESTROY_NONE)
        return;

    assert( !surface->active && !surface->cached );
    remove_client_surface_ref_index( surface );
    free( surface );
}

/* Queue removal releases notification ownership of a retired lifetime. */
static void retire_client_surface_ref( struct client_surface_ref *surface )
{
    retire_client_surface_handoff( surface );
    list_remove( &surface->entry );
    surface->active = surface->cached = surface->claimed = surface->candidate_serial = 0;
    surface->owner = NULL;
    free_client_surface_ref_if_unused( surface );
}

static void client_surface_handoff_changed( void *context )
{
    invalidate_client_surface_owner_repair( context );
}

static void free_client_surface_handoff( struct client_surface_ref *surface )
{
    client_surface_handoff_free( surface->handoff );
    surface->handoff = NULL;
}

static void retire_client_surface_handoff( struct client_surface_ref *surface )
{
    surface->handoff = client_surface_handoff_retire( surface->handoff );
}

static void retarget_client_surface_handoff( struct client_surface_ref *surface )
{
    surface->handoff = client_surface_handoff_retarget( surface->handoff );
}

static int alloc_client_surface_handoff( struct client_surface_ref *surface,
                                         struct window *win, struct window *top );


enum client_surface_phase
{
    CLIENT_SURFACE_PHASE_IDLE,
    CLIENT_SURFACE_PHASE_PREPARING,
    CLIENT_SURFACE_PHASE_COMPOSING,
    CLIENT_SURFACE_PHASE_READY,
    CLIENT_SURFACE_PHASE_PUBLISHING,
};

struct client_surface_transaction
{
    enum client_surface_phase phase;
    enum
    {
        CLIENT_SURFACE_PUBLICATION_NONE,
        CLIENT_SURFACE_PUBLICATION_COPY = CLIENT_SURFACE_PUBLISH_COPY,
        CLIENT_SURFACE_PUBLICATION_EXPOSE = CLIENT_SURFACE_PUBLISH_EXPOSE,
        CLIENT_SURFACE_PUBLICATION_HANDOFF,
        CLIENT_SURFACE_PUBLICATION_EXPOSURE_READY,
    } publication;
    unsigned long long epoch;       /* stable scene epoch captured at start */
    unsigned int pending;            /* producers which have not completed */
    unsigned int staged : 1;         /* host is mapped into unpublished backing */
    unsigned int prepared : 1;       /* owner snapshot permits a live replay */
    unsigned int owner_repair : 1;   /* warm replay; source recovery supersedes it */
    unsigned int source_pending : 1; /* owner inventory decision, not image proof */
    unsigned int restarting : 1;     /* restart loop owns transaction changes */
    unsigned int restart_pending : 1;/* replay requested during a transition */
    struct timeout_user *timeout;    /* liveness deadline callback */
    abstime_t deadline;              /* fixed deadline for the episode */
};

struct window
{
    struct object    obj;             /* object header */
    struct window   *parent;          /* parent window */
    user_handle_t    owner;           /* owner of this window */
    struct list      children;        /* list of children in Z-order */
    struct list      unlinked;        /* list of children not linked in the Z-order list */
    struct list      entry;           /* entry in parent's children list */
    user_handle_t    handle;          /* full handle for this window */
    struct thread   *thread;          /* thread owning the window */
    struct desktop  *desktop;         /* desktop that the window belongs to */
    struct window_class *class;       /* window class */
    atom_t           atom;            /* class atom */
    user_handle_t    last_active;     /* last active popup */
    struct rectangle window_rect;     /* window rectangle (relative to parent client area) */
    struct rectangle visible_rect;    /* visible part of window rect (relative to parent client area) */
    struct rectangle surface_rect;    /* window surface rectangle (relative to parent client area) */
    struct rectangle client_rect;     /* client rectangle (relative to parent client area) */
    struct rectangle present_rect;    /* exclusive fullscreen placement at window DPI */
    struct region   *win_region;      /* region for shaped windows (relative to window rect) */
    struct region   *update_region;   /* update region (relative to window rect) */
    unsigned int     style;           /* window style */
    unsigned int     ex_style;        /* window extended style */
    unsigned int     is_linked : 1;   /* is it linked into the parent z-order list? */
    unsigned int     is_layered : 1;  /* has layered info been set? */
    unsigned int     is_orphan : 1;   /* is window orphaned */
    unsigned int     set_foreground : 1;/* has window been foreground once */
    unsigned int     color_key;       /* color key for a layered window */
    unsigned int     alpha;           /* alpha value for a layered window */
    unsigned int     layered_flags;   /* flags for a layered window */
    WCHAR           *text;            /* window caption text */
    data_size_t      text_len;        /* length of window caption */
    unsigned int     paint_flags;     /* various painting flags */
    int              pixel_format;    /* pixel format selected for the window */
    struct list      client_surface_owners; /* processes holding active or cached surfaces */
    unsigned int     client_surface_count; /* active client-rendered surfaces for this window */
    unsigned int     client_surface_cached_count; /* cached client-rendered surfaces owned by this window */
    unsigned int     client_surface_subtree_count; /* active and cached identities in this subtree */
    unsigned int     client_surface_backing_required; /* owner host backing mode last published */
    unsigned int     client_surface_dirty; /* top-level composition changed while hidden */
    struct client_surface_transaction client_surface_transaction;
    client_ptr_t     client_surface_native_barrier; /* owner token sealing native target replacement */
    UINT64          paint_serial; /* invalidation / BeginPaint sequence for this backing owner */
    unsigned int    paint_waiting : 1;
    unsigned int    paint_failed : 1;
    unsigned long long client_surface_scene_generation; /* even when the scene is stable */
    unsigned long long client_surface_direct_scene; /* authenticated owner strategy */
    unsigned long long client_surface_ack_scene; /* last successful owner publication */
    unsigned long long client_surface_direct_surface;
    unsigned int     client_surface_scene_change_depth;
    int              prop_inuse;      /* number of in-use window properties */
    int              prop_alloc;      /* number of allocated window properties */
    struct property *properties;      /* window properties array */
    window_shm_t    *shared;          /* window in session shared memory */
};

C_ASSERT( sizeof(window_shm_t) == offsetof(window_shm_t, extra[0]) );

static int alloc_client_surface_handoff( struct client_surface_ref *surface,
                                         struct window *win, struct window *top )
{
    struct process *consumer;

    if (!top->thread || !(consumer = top->thread->process))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return 0;
    }
    surface->handoff = client_surface_handoff_create( surface->process, consumer, &top->obj,
        win->handle, top->handle, surface->id, client_surface_handoff_changed, surface );
    return !!surface->handoff;
}

static int client_surface_is_preparing( const struct window *top )
{
    return top->client_surface_transaction.phase == CLIENT_SURFACE_PHASE_PREPARING;
}

static int client_surface_is_composing( const struct window *top )
{
    return top->client_surface_transaction.phase >= CLIENT_SURFACE_PHASE_COMPOSING;
}

static int client_surface_is_ready( const struct window *top )
{
    return top->client_surface_transaction.phase >= CLIENT_SURFACE_PHASE_READY;
}

static int client_surface_is_publishing( const struct window *top )
{
    return top->client_surface_transaction.phase == CLIENT_SURFACE_PHASE_PUBLISHING;
}

static unsigned long long client_surface_transaction_generation( const struct window *top )
{
    return client_surface_is_composing( top ) ? top->client_surface_transaction.epoch : 0;
}

static void update_client_surface_publication( struct window *top )
{
    struct client_surface_owner *owner;
    struct client_surface_ref *candidate = get_client_surface_native_candidate( top, &owner );
    enum client_surface_presentation_mode mode = client_surface_presentation_mode( top );
    int direct = mode == CLIENT_SURFACE_PRESENTATION_DIRECT;
    int backing_required = top->client_surface_subtree_count &&
                           (top->client_surface_transaction.staged ||
                            (!direct &&
                             !client_surface_direct_registration_candidate( top )) ||
                            (top->client_surface_backing_required &&
                             (!direct || top->client_surface_transaction.phase != CLIENT_SURFACE_PHASE_IDLE)));
    int backing_changed = backing_required != top->client_surface_backing_required;

    assert( client_surface_is_composing( top ) == !!top->client_surface_transaction.epoch );
    assert( !client_surface_is_ready( top ) || !top->client_surface_transaction.pending );
    SHARED_WRITE_BEGIN( top->shared, window_shm_t )
    {
        shared->client_surface_generation = client_surface_transaction_generation( top );
        shared->client_surface_scene_generation = top->client_surface_scene_generation;
        shared->client_surface_paint_serial = top->paint_serial;
        shared->client_surface_native_candidate = !top->client_surface_transaction.staged &&
            !top->client_surface_native_barrier && candidate ? candidate->id : 0;
        shared->client_surface_flags =
            (top->client_surface_transaction.staged ? WINDOW_SHM_CLIENT_SURFACE_STAGED : 0) |
            (client_surface_is_composing( top ) ? WINDOW_SHM_CLIENT_SURFACE_COMPOSING : 0) |
            (client_surface_is_publishing( top ) ? WINDOW_SHM_CLIENT_SURFACE_PUBLISHING : 0) |
            (client_surface_is_preparing( top ) ? WINDOW_SHM_CLIENT_SURFACE_PREPARING : 0) |
            (direct ? WINDOW_SHM_CLIENT_SURFACE_DIRECT : 0) |
            (!top->client_surface_transaction.staged && !top->client_surface_native_barrier &&
             client_surface_direct_candidate( top ) ? WINDOW_SHM_CLIENT_SURFACE_DIRECT_CANDIDATE : 0) |
            (backing_required ? WINDOW_SHM_CLIENT_SURFACE_BACKING : 0) |
            (top->client_surface_transaction.source_pending ? WINDOW_SHM_CLIENT_SURFACE_SOURCE_PENDING : 0);
    }
    SHARED_WRITE_END;
    top->client_surface_backing_required = backing_required;
    if (backing_changed && top->handle)
        post_message_coalesced( top->handle, WM_WINE_UPDATEWINDOWSTATE,
                                WINE_UPDATE_CLIENT_SURFACE_BACKING, 0 );
}

static void window_dump( struct object *obj, int verbose );
static void window_destroy( struct object *obj );
static int is_visible( const struct window *win );
static int has_client_surface( const struct window *win );
static struct window *get_toplevel_window( struct window *win );
static void adjust_client_surface_subtree_count( struct window *win, int delta );
static void begin_client_surface_scene_change( struct window *top );
static void begin_client_surface_cached_scene_change( struct window *top );
static void end_client_surface_scene_change( struct window *top );
static unsigned int clear_client_surface_subtree_generation( struct window *win,
                                                             unsigned long long generation );
static void cancel_client_surface_timeout( struct window *top );
static void finish_client_surface_generation( struct window *top );
static void finish_client_surface_publication( struct window *top );
static void restart_client_surface_generation( struct window *top );
static void restart_client_surface_generation_internal( struct window *top );
static unsigned long long client_surface_ref_sequence;

static const struct object_ops window_ops =
{
    .size    = sizeof(struct window),
    .type    = &no_type,
    .dump    = window_dump,
    .destroy = window_destroy,
};

/* flags that can be set by the client */
#define PAINT_HAS_SURFACE          SET_WINPOS_PAINT_SURFACE
#define PAINT_HAS_PIXEL_FORMAT     SET_WINPOS_PIXEL_FORMAT
#define PAINT_HAS_LAYERED_SURFACE  SET_WINPOS_LAYERED_WINDOW
#define PAINT_CLIENT_FLAGS         (PAINT_HAS_SURFACE | PAINT_HAS_PIXEL_FORMAT | PAINT_HAS_LAYERED_SURFACE)
/* flags only manipulated by the server */
#define PAINT_INTERNAL           0x0010  /* internal WM_PAINT pending */
#define PAINT_ERASE              0x0020  /* needs WM_ERASEBKGND */
#define PAINT_NONCLIENT          0x0040  /* needs WM_NCPAINT */
#define PAINT_DELAYED_ERASE      0x0080  /* still needs erase after WM_ERASEBKGND */
#define PAINT_PIXEL_FORMAT_CHILD 0x0100  /* at least one child has a custom pixel format */

/* growable array of user handles */
struct user_handle_array
{
    user_handle_t *handles;
    int            count;
    int            total;
};

static const struct rectangle empty_rect;

/* magic HWND_TOP etc. pointers */
#define WINPTR_TOP       ((struct window *)1L)
#define WINPTR_BOTTOM    ((struct window *)2L)
#define WINPTR_TOPMOST   ((struct window *)3L)
#define WINPTR_NOTOPMOST ((struct window *)4L)

static void window_dump( struct object *obj, int verbose )
{
    struct window *win = (struct window *)obj;
    assert( obj->ops == &window_ops );
    fprintf( stderr, "window %p handle %x\n", win, win->handle );
}

static void window_destroy( struct object *obj )
{
    struct window *win = (struct window *)obj;
    struct client_surface_owner *owner, *next;
    struct client_surface_ref *surface, *surface_next;

    assert( !win->handle );
    assert( !win->client_surface_transaction.timeout );

    if (win->parent)
    {
        list_remove( &win->entry );
        update_window_first_child( win->parent );
        release_object( win->parent );
    }

    if (win->win_region) free_region( win->win_region );
    if (win->update_region) free_region( win->update_region );
    if (win->class) release_class( win->class );
    free( win->text );

    LIST_FOR_EACH_ENTRY_SAFE( owner, next, &win->client_surface_owners,
                              struct client_surface_owner, entry )
    {
        LIST_FOR_EACH_ENTRY_SAFE( surface, surface_next, &owner->surfaces,
                                  struct client_surface_ref, entry )
            retire_client_surface_ref( surface );
        list_remove( &owner->entry );
        release_object( owner->process );
        free( owner );
    }

    if (win->shared) free_shared_object( win->shared );
}

/* retrieve a pointer to a window from its handle */
static inline struct window *get_window( user_handle_t handle )
{
    struct window *ret = get_user_object( handle, NTUSER_OBJ_WINDOW );
    if (!ret) set_win32_error( ERROR_INVALID_WINDOW_HANDLE );
    return ret;
}

/* check if window is the desktop */
static inline int is_desktop_window( const struct window *win )
{
    return !win->parent;  /* only desktop windows have no parent */
}

/* check if window is a toplevel or desktop window */
static bool is_toplevel( const struct window *win )
{
    return !win->parent || is_desktop_window( win->parent );
}

/* check if window is orphaned */
static int is_orphan_window( struct window *win )
{
    do if (win->is_orphan) return 1;
    while ((win = win->parent));
    return 0;
}

/* get next window in Z-order list */
static inline struct window *get_next_window( struct window *win )
{
    struct list *ptr = list_next( &win->parent->children, &win->entry );
    return ptr ? LIST_ENTRY( ptr, struct window, entry ) : NULL;
}

/* get previous window in Z-order list */
static inline struct window *get_prev_window( struct window *win )
{
    struct list *ptr = list_prev( &win->parent->children, &win->entry );
    return ptr ? LIST_ENTRY( ptr, struct window, entry ) : NULL;
}

/* get first child in Z-order list */
static inline struct window *get_first_child( struct window *win )
{
    struct list *ptr = list_head( &win->children );
    return ptr ? LIST_ENTRY( ptr, struct window, entry ) : NULL;
}

static void update_window_first_child( struct window *win )
{
    struct window *child = get_first_child( win );
    user_handle_t handle = child ? child->handle : 0;

    if (win->shared->first_child == handle) return;
    SHARED_WRITE_BEGIN( win->shared, window_shm_t )
    {
        shared->first_child = handle;
    }
    SHARED_WRITE_END;
}

/* get last child in Z-order list */
static inline struct window *get_last_child( struct window *win )
{
    struct list *ptr = list_tail( &win->children );
    return ptr ? LIST_ENTRY( ptr, struct window, entry ) : NULL;
}

/* set the PAINT_PIXEL_FORMAT_CHILD flag on all the parents */
/* note: we never reset the flag, it's just a heuristic */
static inline void update_pixel_format_flags( struct window *win )
{
    for (win = win->parent; win && win->parent; win = win->parent)
        win->paint_flags |= PAINT_PIXEL_FORMAT_CHILD;
}

static struct rectangle monitors_get_union_rect( struct winstation *winstation, int is_raw )
{
    struct monitor_info *monitor, *end;
    struct rectangle rect = {0};

    for (monitor = winstation->monitors, end = monitor + winstation->monitor_count; monitor < end; monitor++)
    {
        struct rectangle monitor_rect = is_raw ? monitor->raw : monitor->virt;
        if (monitor->flags & (MONITOR_FLAG_CLONE | MONITOR_FLAG_INACTIVE)) continue;
        union_rect( &rect, &rect, &monitor_rect );
    }

    return rect;
}

/* returns the largest intersecting or nearest monitor, keep in sync with win32u/sysparams.c */
static struct monitor_info *get_monitor_from_rect( struct winstation *winstation, const struct rectangle *rect, int is_raw )
{
    struct monitor_info *monitor, *nearest = NULL, *found = NULL, *end;
    unsigned int max_area = 0, min_distance = -1;

    for (monitor = winstation->monitors, end = monitor + winstation->monitor_count; monitor < end; monitor++)
    {
        struct rectangle intersect, target = is_raw ? monitor->raw : monitor->virt;

        if (monitor->flags & (MONITOR_FLAG_CLONE | MONITOR_FLAG_INACTIVE)) continue;

        if (intersect_rect( &intersect, &target, rect ))
        {
            /* check for larger intersecting area */
            unsigned int area = (intersect.right - intersect.left) * (intersect.bottom - intersect.top);

            if (area > max_area)
            {
                max_area = area;
                found = monitor;
            }
        }

        if (!found)  /* if not intersecting, check for min distance */
        {
            unsigned int distance, x, y;

            if (rect->right <= target.left) x = target.left - rect->right;
            else if (target.right <= rect->left) x = rect->left - target.right;
            else x = 0;

            if (rect->bottom <= target.top) y = target.top - rect->bottom;
            else if (target.bottom <= rect->top) y = rect->top - target.bottom;
            else y = 0;

            distance = x * x + y * y;
            if (distance < min_distance)
            {
                min_distance = distance;
                nearest = monitor;
            }
        }
    }

    return found ? found : nearest;
}

static void map_point_raw_to_virt( struct desktop *desktop, int *x, int *y )
{
    int width_from, height_from, width_to, height_to;
    struct rectangle rect = {*x, *y, *x + 1, *y + 1};
    struct monitor_info *monitor;

    if (!(monitor = get_monitor_from_rect( desktop->winstation, &rect, 1 ))) return;
    width_to = monitor->virt.right - monitor->virt.left;
    height_to = monitor->virt.bottom - monitor->virt.top;
    width_from = monitor->raw.right - monitor->raw.left;
    height_from = monitor->raw.bottom - monitor->raw.top;

    *x = *x * 2 - (monitor->raw.left * 2 + width_from);
    *x = (*x * width_to * 2 + width_from) / (width_from * 2);
    *x = (*x + monitor->virt.left * 2 + width_to) / 2;

    *y = *y * 2 - (monitor->raw.top * 2 + height_from);
    *y = (*y * height_to * 2 + height_from) / (height_from * 2);
    *y = (*y + monitor->virt.top * 2 + height_to) / 2;
}

/* get the per-monitor DPI for a window */
static struct ratio get_monitor_dpi( struct window *win )
{
    while (!is_toplevel( win )) win = win->parent;
    return win->shared->dpi;
}

static struct ratio get_window_dpi( struct window *win )
{
    struct ratio dpi = {1, 1};
    if (NTUSER_DPI_CONTEXT_IS_MONITOR_AWARE( win->shared->dpi_context )) return get_monitor_dpi( win );
    dpi.num = NTUSER_DPI_CONTEXT_GET_DPI( win->shared->dpi_context );
    return dpi;
}

/* link a window at the right place in the siblings list */
static int link_window( struct window *win, struct window *previous )
{
    struct list *old_prev;

    if (previous == WINPTR_NOTOPMOST)
    {
        if (!(win->ex_style & WS_EX_TOPMOST) && win->is_linked) return 0;  /* nothing to do */
        win->ex_style &= ~WS_EX_TOPMOST;
        previous = WINPTR_TOP;  /* fallback to the HWND_TOP case */
    }

    old_prev = win->is_linked ? win->entry.prev : NULL;
    list_remove( &win->entry );  /* unlink it from the previous location */

    if (previous == WINPTR_BOTTOM)
    {
        list_add_tail( &win->parent->children, &win->entry );
        win->ex_style &= ~WS_EX_TOPMOST;
    }
    else if (previous == WINPTR_TOPMOST)
    {
        list_add_head( &win->parent->children, &win->entry );
        win->ex_style |= WS_EX_TOPMOST;
    }
    else if (previous == WINPTR_TOP)
    {
        struct list *entry = win->parent->children.next;
        if (!(win->ex_style & WS_EX_TOPMOST))  /* put it above the first non-topmost window */
        {
            while (entry != &win->parent->children)
            {
                struct window *next = LIST_ENTRY( entry, struct window, entry );
                if (!(next->ex_style & WS_EX_TOPMOST)) break;
                if (next->handle == win->owner)  /* keep it above owner */
                {
                    win->ex_style |= WS_EX_TOPMOST;
                    break;
                }
                entry = entry->next;
            }
        }
        list_add_before( entry, &win->entry );
    }
    else
    {
        list_add_after( &previous->entry, &win->entry );
        if (!(previous->ex_style & WS_EX_TOPMOST)) win->ex_style &= ~WS_EX_TOPMOST;
        else
        {
            struct window *next = get_next_window( win );
            if (next && (next->ex_style & WS_EX_TOPMOST)) win->ex_style |= WS_EX_TOPMOST;
        }
    }

    win->is_linked = 1;
    update_window_first_child( win->parent );
    return old_prev != win->entry.prev;
}

static void set_window_monitor_dpi( struct window *win )
{
    struct monitor_info *info;

    if (!(info = get_monitor_from_rect( win->desktop->winstation, &win->window_rect, 0 ))) return;

    SHARED_WRITE_BEGIN( win->shared, window_shm_t )
    {
        shared->dpi     = info->dpi;
        shared->raw_dpi = info->raw_dpi;
    }
    SHARED_WRITE_END;
}

/* attach or detach the parent window thread input if necessary */
static void attach_parent_thread( struct window *win, bool attach )
{
    struct thread *thread = win->thread, *parent;

    if (is_toplevel( win ) || !(parent = win->parent->thread) || parent == thread) return;

    /* if parent belongs to a different thread and the window isn't top-level, attach / detach the two threads */
    if (attach) attach_thread_input( thread->queue, parent->queue );
    else detach_thread_input( thread->queue, parent->queue, win->desktop );
}

static void retarget_client_surface_subtree_handoffs( struct window *win, struct window *top )
{
    struct client_surface_owner *owner;
    struct client_surface_ref *surface;
    struct window *child;

    if (!win->client_surface_subtree_count) return;
    LIST_FOR_EACH_ENTRY( owner, &win->client_surface_owners, struct client_surface_owner, entry )
        LIST_FOR_EACH_ENTRY( surface, &owner->surfaces, struct client_surface_ref, entry )
            if (surface->handoff && client_surface_handoff_get_state( surface->handoff ).owner != &top->obj)
                retarget_client_surface_handoff( surface );
    LIST_FOR_EACH_ENTRY( child, &win->children, struct window, entry )
        retarget_client_surface_subtree_handoffs( child, top );
}

/* Placement changes may reuse images only within the same native owner.
 * The owner still checks every selected image against the new scene; this
 * predicate only excludes changes to the source's coordinate contract. */
static int client_surface_child_placement_compatible( struct window *win, struct window *parent )
{
    if (!win->is_linked || !win->parent || is_desktop_window( win->parent ) ||
        !parent || is_desktop_window( parent ) ||
        get_toplevel_window( win ) != get_toplevel_window( parent )) return 0;
    if (parent != win->parent &&
        ((win->parent->ex_style ^ parent->ex_style) & WS_EX_LAYOUTRTL ||
         win->shared->dpi.num != parent->shared->dpi.num || win->shared->dpi.den != parent->shared->dpi.den ||
         win->shared->raw_dpi.num != parent->shared->raw_dpi.num ||
         win->shared->raw_dpi.den != parent->shared->raw_dpi.den)) return 0;
    return is_rect_empty( &win->present_rect );
}

/* change the parent of a window (or unlink the window if the new parent is NULL) */
static int set_parent_window( struct window *win, struct window *parent )
{
    struct window *ptr, *old_top = get_toplevel_window( win );
    struct window *old_parent = win->parent;
    struct window *new_top = parent && !is_desktop_window( parent ) ?
                             get_toplevel_window( parent ) : win;
    unsigned int subtree_count = win->client_surface_subtree_count;
    const unsigned int old_ex_style = win->ex_style;
    int has_surfaces = !!subtree_count, old_pending, scene_change;

    /* make sure parent is not a child of window */
    for (ptr = parent; ptr; ptr = ptr->parent)
    {
        if (ptr == win)
        {
            set_error( STATUS_INVALID_PARAMETER );
            return 0;
        }
    }

    /* A hidden non-producer has no contribution to either scene. In
     * particular, unlinking an already hidden/detached dying child must not
     * make every surviving producer rebuild its unchanged plan. */
    scene_change = has_surfaces || ((win->style & WS_VISIBLE) && (win->is_linked || parent));
    if (scene_change)
    {
        if (client_surface_child_placement_compatible( win, parent ))
            begin_client_surface_cached_scene_change( old_top );
        else begin_client_surface_scene_change( old_top );
    }
    if (scene_change && new_top != old_top) begin_client_surface_scene_change( new_top );
    /* A top-level keeps its own subtree count when it becomes a child, and a
     * child already owns its count when it becomes a top-level.  Those two
     * topology transitions bypass adjust_client_surface_subtree_count(), so
     * transfer the X backing-store lifetime explicitly. */
    if (subtree_count && old_top == win && new_top != old_top)
        post_message( old_top->handle, WM_WINE_UPDATEWINDOWSTATE,
                      WINE_UPDATE_CLIENT_SURFACE_BACKING, 0 );
    if (subtree_count && win->is_linked)
        adjust_client_surface_subtree_count( win->parent, -(int)subtree_count );

    if (parent)
    {
        attach_parent_thread( win, false );
        win->parent = (struct window *)grab_object( parent );
        attach_parent_thread( win, true );
        link_window( win, WINPTR_TOP );
        if (old_parent)
        {
            update_window_first_child( old_parent );
            release_object( old_parent );
        }
        if (win->ex_style != old_ex_style) old_top->client_surface_transaction.source_pending = 0;
        if (subtree_count) adjust_client_surface_subtree_count( win->parent, subtree_count );
        if (subtree_count && new_top == win && new_top != old_top)
            post_message( new_top->handle, WM_WINE_UPDATEWINDOWSTATE,
                          WINE_UPDATE_CLIENT_SURFACE_BACKING, 1 );

        if (is_desktop_window( parent )) set_window_monitor_dpi( win );
        else SHARED_WRITE_BEGIN( win->shared, window_shm_t )
        {
            shared->dpi         = parent->shared->dpi;
            shared->raw_dpi     = parent->shared->raw_dpi;
        }
        SHARED_WRITE_END;

        if (win->paint_flags & (PAINT_HAS_PIXEL_FORMAT | PAINT_PIXEL_FORMAT_CHILD))
            update_pixel_format_flags( win );

        if (has_surfaces)
        {
            new_top = get_toplevel_window( win );
            if (old_top != new_top)
            {
                retarget_client_surface_subtree_handoffs( win, new_top );
                old_pending = old_top->client_surface_dirty;
                /* The prepared set belongs to the old hierarchy.  Cancel it
                 * even when other surfaces remain there; otherwise commits
                 * from the moved subtree can leave the old top permanently
                 * waiting for surfaces which no longer belong to it. */
                old_top->client_surface_transaction.staged = 0;
                if (old_top == win || !has_client_surface( old_top ))
                    old_top->client_surface_dirty = 0;
                finish_client_surface_generation( old_top );
                if (old_top != win && old_pending && is_visible( old_top ))
                    post_message( old_top->handle, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );

                new_top->client_surface_dirty = 1;
                new_top->client_surface_transaction.staged = 0;
                finish_client_surface_generation( new_top );
                if (is_visible( new_top ))
                    post_message( new_top->handle, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
            }
        }
    }
    else  /* move it to parent unlinked list */
    {
        list_remove( &win->entry );  /* unlink it from the previous location */
        list_add_head( &win->parent->unlinked, &win->entry );
        win->is_linked = 0;
        win->is_orphan = 1;
        update_window_first_child( win->parent );
    }
    if (scene_change && new_top != old_top) end_client_surface_scene_change( new_top );
    if (scene_change) end_client_surface_scene_change( old_top );
    return 1;
}

/* append a user handle to a handle array */
static int add_handle_to_array( struct user_handle_array *array, user_handle_t handle )
{
    if (array->count >= array->total)
    {
        int new_total = max( array->total * 2, 32 );
        user_handle_t *new_array = realloc( array->handles, new_total * sizeof(*new_array) );
        if (!new_array)
        {
            free( array->handles );
            set_error( STATUS_NO_MEMORY );
            return 0;
        }
        array->handles = new_array;
        array->total = new_total;
    }
    array->handles[array->count++] = handle;
    return 1;
}

/* set a window property */
static void set_property( struct window *win, atom_t atom, lparam_t data, enum property_type type )
{
    struct atom_table *table = get_global_atom_table();
    int i, free = -1;
    struct property *new_props;

    /* check if it exists already */
    for (i = 0; i < win->prop_inuse; i++)
    {
        if (win->properties[i].type == PROP_TYPE_FREE)
        {
            free = i;
            continue;
        }
        if (win->properties[i].atom == atom)
        {
            win->properties[i].type = type;
            win->properties[i].data = data;
            return;
        }
    }

    /* need to add an entry */
    if (type == PROP_TYPE_STRING && !grab_atom( table, atom )) return;
    if (free == -1)
    {
        /* no free entry */
        if (win->prop_inuse >= win->prop_alloc)
        {
            /* need to grow the array */
            if (!(new_props = realloc( win->properties,
                                       sizeof(*new_props) * (win->prop_alloc + 16) )))
            {
                set_error( STATUS_NO_MEMORY );
                if (type == PROP_TYPE_STRING) release_atom( table, atom );
                return;
            }
            win->prop_alloc += 16;
            win->properties = new_props;
        }
        free = win->prop_inuse++;
    }
    win->properties[free].atom = atom;
    win->properties[free].type = type;
    win->properties[free].data = data;
}

/* remove a window property */
static lparam_t remove_property( struct window *win, atom_t atom )
{
    struct atom_table *table = get_global_atom_table();
    int i;

    for (i = 0; i < win->prop_inuse; i++)
    {
        struct property *prop = win->properties + i;
        if (prop->type == PROP_TYPE_FREE) continue;
        if (prop->atom == atom)
        {
            if (prop->type == PROP_TYPE_STRING) release_atom( table, atom );
            prop->type = PROP_TYPE_FREE;
            return prop->data;
        }
    }
    /* FIXME: last error? */
    return 0;
}

/* find a window property */
static lparam_t get_property( struct window *win, atom_t atom )
{
    int i;

    for (i = 0; i < win->prop_inuse; i++)
    {
        if (win->properties[i].type == PROP_TYPE_FREE) continue;
        if (win->properties[i].atom == atom) return win->properties[i].data;
    }
    /* FIXME: last error? */
    return 0;
}

/* destroy all properties of a window */
static inline void destroy_properties( struct window *win )
{
    struct atom_table *table = get_global_atom_table();
    int i;

    if (!win->properties) return;
    for (i = 0; i < win->prop_inuse; i++)
    {
        struct property *prop = win->properties + i;
        if (prop->type == PROP_TYPE_FREE) continue;
        if (prop->type == PROP_TYPE_STRING) release_atom( table, prop->atom );
    }
    free( win->properties );
}

/* detach a window from its owner thread but keep the window around */
static void detach_window_thread( struct window *win )
{
    struct thread *thread = win->thread;

    if (!thread) return;
    if (thread->queue)
    {
        if (win->update_region) inc_queue_paint_count( thread, -1 );
        if (win->paint_flags & PAINT_INTERNAL) inc_queue_paint_count( thread, -1 );
        queue_cleanup_window( thread, win->handle );
    }
    assert( thread->desktop_users > 0 );
    thread->desktop_users--;
    release_class( win->class );
    win->class = NULL;

    /* don't hold a reference to the desktop so that the desktop window can be */
    /* destroyed when the desktop ref count reaches zero */
    release_object( win->desktop );
    win->thread = NULL;
}

/* get the process owning the top window of a given desktop */
struct process *get_top_window_owner( struct desktop *desktop )
{
    struct window *win = desktop->top_window;
    if (!win || !win->thread) return NULL;
    return win->thread->process;
}

/* get the top window size of a given desktop */
void get_virtual_screen_rect( struct desktop *desktop, struct rectangle *rect, int is_raw )
{
    *rect = monitors_get_union_rect( desktop->winstation, is_raw );
}

/* post a message to the desktop window */
void post_desktop_message( struct desktop *desktop, unsigned int message,
                           lparam_t wparam, lparam_t lparam )
{
    struct window *win = desktop->top_window;
    if (win && win->thread) post_message( win->handle, message, wparam, lparam );
}

/* create a new window structure (note: the window is not linked in the window tree) */
static struct window *create_window( struct window *parent, struct window *owner, atom_t atom,
                                     mod_handle_t class_instance, bool ansi, unsigned int dpi_context,
                                     struct ratio dpi, struct ratio raw_dpi )
{
    data_size_t extra_size, private_size;
    struct window *win = NULL;
    struct desktop *desktop;
    struct window_class *class;
    struct obj_locator class_locator;
    unsigned int fnid;

    if (!(desktop = get_thread_desktop( current, DESKTOP_CREATEWINDOW ))) return NULL;

    if (!(class = grab_class( current->process, atom, class_instance, &class_locator )))
    {
        release_object( desktop );
        return NULL;
    }
    fnid = get_class_fnid( class, &extra_size, &private_size );

    if (!parent)  /* null parent is only allowed for desktop or HWND_MESSAGE top window */
    {
        if (is_desktop_class( class ))
            parent = desktop->top_window;  /* use existing desktop if any */
        else if (is_message_class( class ))
            /* use desktop window if message window is already created */
            parent = desktop->msg_window ? desktop->top_window : NULL;
        else if (!(parent = desktop->top_window))  /* must already have a desktop then */
        {
            set_error( STATUS_ACCESS_DENIED );
            goto failed;
        }
    }

    /* parent must be on the same desktop */
    if (parent && parent->desktop != desktop)
    {
        set_error( STATUS_ACCESS_DENIED );
        goto failed;
    }

    if (!(win = alloc_object( &window_ops ))) goto failed;
    win->parent         = parent ? (struct window *)grab_object( parent ) : NULL;
    win->owner          = owner ? owner->handle : 0;
    win->thread         = current;
    win->desktop        = desktop;
    win->class          = class;
    win->atom           = atom;
    win->win_region     = NULL;
    win->update_region  = NULL;
    win->style          = 0;
    win->ex_style       = 0;
    win->is_linked      = 0;
    win->is_layered     = 0;
    win->is_orphan      = 0;
    win->set_foreground = 0;
    win->text           = NULL;
    win->text_len       = 0;
    win->paint_flags    = 0;
    win->pixel_format   = 0;
    list_init( &win->client_surface_owners );
    win->client_surface_count  = 0;
    win->client_surface_cached_count = 0;
    win->client_surface_subtree_count = 0;
    win->client_surface_backing_required = 0;
    win->client_surface_dirty  = 0;
    win->client_surface_transaction = (struct client_surface_transaction){0};
    win->client_surface_native_barrier = 0;
    win->paint_serial = 0;
    win->paint_waiting = win->paint_failed = 0;
    win->present_rect = empty_rect;
    win->client_surface_scene_generation = 0;
    win->client_surface_direct_scene = win->client_surface_direct_surface = 0;
    win->client_surface_ack_scene = 0;
    win->client_surface_scene_change_depth = 0;
    win->prop_inuse     = 0;
    win->prop_alloc     = 0;
    win->properties     = NULL;
    win->shared         = NULL;
    win->window_rect = win->visible_rect = win->surface_rect = win->client_rect = empty_rect;
    list_init( &win->children );
    list_init( &win->unlinked );

    if (!(win->shared = alloc_shared_object( offsetof(window_shm_t, extra[extra_size]) ))) goto failed;
    SHARED_WRITE_BEGIN( win->shared, window_shm_t )
    {
        shared->class           = class_locator;
        shared->dpi_context     = is_toplevel( win ) ? dpi_context : parent->shared->dpi_context;
        shared->fnid            = fnid;
        shared->first_child     = 0;
        shared->private_size    = private_size;
        shared->dpi             = dpi;
        shared->raw_dpi         = raw_dpi;
        shared->extra_size      = extra_size;
        shared->client_surface_generation = 0;
        shared->client_surface_scene_generation = 0;
        shared->client_surface_paint_serial = 0;
        shared->client_surface_flags = 0;
        shared->client_surface_process = 0;
        shared->client_surface_id = 0;
        shared->client_surface_producer_sequence = 0;
        shared->client_surface_native_candidate = 0;
        memset( (void *)&shared->info, 0, sizeof(shared->info) );
        memset( (void *)shared->extra, 0, extra_size );
        shared->info.wndproc    = get_class_wndproc( win->class, &ansi );
        shared->ansi            = ansi;
    }
    SHARED_WRITE_END;

    if (!(win->handle = alloc_user_handle( win, win->shared, NTUSER_OBJ_WINDOW ))) goto failed;
    win->last_active = win->handle;

    /* make sure that the thread has a message queue */
    if (!current->queue && !init_thread_queue( current )) goto failed;

    /* attach the parent thread if necessary */
    attach_parent_thread( win, true );

    /* put it on parent unlinked list */
    if (parent) list_add_head( &parent->unlinked, &win->entry );
    else
    {
        list_init( &win->entry );
        if (is_desktop_class( class ))
        {
            assert( !desktop->top_window );
            desktop->top_window = win;
            set_process_default_desktop( current->process, desktop, current->desktop );
        }
        else
        {
            assert( !desktop->msg_window );
            desktop->msg_window = win;
        }
    }

    current->desktop_users++;
    return win;

failed:
    if (win)
    {
        if (win->handle)
        {
            free_user_handle( win->handle );
            win->handle = 0;
        }
        release_object( win );
    }
    release_object( desktop );
    release_class( class );
    return NULL;
}

/* destroy all windows belonging to a given thread */
void destroy_thread_windows( struct thread *thread )
{
    user_handle_t handle = 0;
    struct window *win;

    cancel_thread_window_paints( thread );

    while ((win = next_user_handle( &handle, NTUSER_OBJ_WINDOW )))
    {
        if (win->thread != thread) continue;
        if (is_desktop_window( win )) detach_window_thread( win );
        else free_window_handle( win );
    }
}

/* get the desktop window */
static struct window *get_desktop_window( struct thread *thread )
{
    struct window *top_window;
    struct desktop *desktop = get_thread_desktop( thread, 0 );

    if (!desktop) return NULL;
    top_window = desktop->top_window;
    release_object( desktop );
    return top_window;
}

/* check whether child is a descendant of parent */
int is_child_window( user_handle_t parent, user_handle_t child )
{
    struct window *child_ptr = get_user_object( child, NTUSER_OBJ_WINDOW );
    struct window *parent_ptr = get_user_object( parent, NTUSER_OBJ_WINDOW );

    if (!child_ptr || !parent_ptr) return 0;
    while (child_ptr->parent)
    {
        if (child_ptr->parent == parent_ptr) return 1;
        child_ptr = child_ptr->parent;
    }
    return 0;
}

/* return the window thread if window can be set as foreground window */
struct thread *make_window_foreground( struct desktop *desktop, user_handle_t window,
                                       int *is_desktop, int *set_foreground )
{
    struct window *win = get_user_object( window, NTUSER_OBJ_WINDOW );

    if (!win || !win->thread || win->desktop != desktop) return NULL;
    if ((win->style & (WS_POPUP | WS_CHILD)) == WS_CHILD) return NULL;
    *is_desktop = win == win->desktop->top_window;
    *set_foreground = win->set_foreground;
    win->set_foreground = 1;

    return (struct thread *)grab_object( win->thread );
}

/* make a window active if possible */
int make_window_active( user_handle_t window )
{
    struct window *owner, *win = get_window( window );

    if (!win) return 0;

    /* set last active for window and its owners */
    owner = win;
    while (owner)
    {
        owner->last_active = win->handle;
        owner = get_user_object( owner->owner, NTUSER_OBJ_WINDOW );
    }
    return 1;
}

/* increment (or decrement) the window paint count */
static inline void inc_window_paint_count( struct window *win, int incr )
{
    if (win->thread) inc_queue_paint_count( win->thread, incr );
}

/* map a point between different DPI scaling levels */
static void map_dpi_point( struct window *win, int *x, int *y, struct ratio from, struct ratio to )
{
    if (!from.num) from = get_monitor_dpi( win );
    if (!to.num) to = get_monitor_dpi( win );
    if (from.num == to.num) return;
    *x = scale_dpi( *x, from, to );
    *y = scale_dpi( *y, from, to );
}

/* map a window rectangle between different DPI scaling levels */
static void map_dpi_rect( struct window *win, struct rectangle *rect, struct ratio from, struct ratio to )
{
    if (!from.num) from = get_monitor_dpi( win );
    if (!to.num) to = get_monitor_dpi( win );
    if (from.num == to.num) return;
    scale_dpi_rect( rect, from, to );
}

/* map a region between different DPI scaling levels */
static void map_dpi_region( struct window *win, struct region *region, struct ratio from, struct ratio to )
{
    if (!from.num) from = get_monitor_dpi( win );
    if (!to.num) to = get_monitor_dpi( win );
    if (from.num == to.num) return;
    scale_region( region, from, to );
}

/* convert coordinates from client to screen coords */
static inline void client_to_screen( struct window *win, int *x, int *y )
{
    for ( ; win && !is_desktop_window(win); win = win->parent)
    {
        *x += win->client_rect.left;
        *y += win->client_rect.top;
    }
}

/* convert coordinates from screen to client coords and dpi */
static void screen_to_client( struct window *win, int *x, int *y, struct ratio dpi )
{
    int offset_x = 0, offset_y = 0;

    if (is_desktop_window( win )) return;

    client_to_screen( win, &offset_x, &offset_y );
    map_dpi_point( win, x, y, dpi, get_window_dpi( win ) );
    *x -= offset_x;
    *y -= offset_y;
}

/* check if window and all its ancestors are visible */
static int is_visible( const struct window *win )
{
    while (win)
    {
        if (!(win->style & WS_VISIBLE)) return 0;
        win = win->parent;
        /* if parent is minimized children are not visible */
        if (win && (win->style & WS_MINIMIZE)) return 0;
    }
    return 1;
}

static int has_client_surface( const struct window *win )
{
    return !!win->client_surface_subtree_count;
}

static void adjust_client_surface_subtree_count( struct window *win, int delta )
{
    for (;;)
    {
        unsigned int old_count = win->client_surface_subtree_count;

        assert( delta >= 0 || win->client_surface_subtree_count >= -delta );
        win->client_surface_subtree_count += delta;
        if (win->parent && is_desktop_window( win->parent ) &&
            !!old_count != !!win->client_surface_subtree_count)
            update_client_surface_publication( win );
        if (!win->parent || !win->is_linked) break;
        win = win->parent;
    }
}

/* Scene updates are a server-owned seqlock.  Clients reject odd sequences and
 * validate the same even sequence before and after native composition. */
static void begin_client_surface_scene_change( struct window *top )
{
    top->client_surface_transaction.source_pending = 0;
    if (top->client_surface_scene_change_depth++) return;
    assert( !(top->client_surface_scene_generation & 1) );
    top->client_surface_scene_generation++;
    update_client_surface_publication( top );
}

/* Changes which preserve the selected source extents can ask the owner for
 * its exact new scene's image inventory before scheduling source recovery.
 * An outstanding cold assembly or nested mutation always takes precedence. */
static void begin_client_surface_cached_scene_change( struct window *top )
{
    int probe = !top->client_surface_scene_change_depth &&
                (top->client_surface_transaction.phase == CLIENT_SURFACE_PHASE_IDLE ||
                 (client_surface_is_preparing( top ) &&
                  (top->client_surface_transaction.owner_repair ||
                   top->client_surface_transaction.source_pending)));

    begin_client_surface_scene_change( top );
    top->client_surface_transaction.source_pending = probe;
}

static void end_client_surface_scene_change( struct window *top )
{
    assert( top->client_surface_scene_change_depth );
    if (--top->client_surface_scene_change_depth) return;
    assert( top->client_surface_scene_generation & 1 );
    top->client_surface_scene_generation++;
    update_client_surface_publication( top );
    if (top->handle)
        post_message_coalesced( top->handle, WM_WINE_UPDATEWINDOWSTATE,
                                WINE_UPDATE_CLIENT_SURFACE_HANDOFFS, 0 );
    if (!client_surface_is_publishing( top ) &&
        (top->client_surface_transaction.staged || (is_visible( top ) && has_client_surface( top ))))
    {
        if (top->client_surface_transaction.source_pending)
            restart_client_surface_generation_internal( top );
        else
            restart_client_surface_generation( top );
    }
}

/* A newer frame in the same geometric scene still invalidates a publication
 * which has crossed PUBLISH_BEGIN.  Advance the stable epoch by one full
 * seqlock cycle so every earlier transaction token becomes stale. */
static void invalidate_client_surface_scene( struct window *top )
{
    assert( !(top->client_surface_scene_generation & 1) );
    top->client_surface_scene_generation += 2;
    update_client_surface_publication( top );
    if (top->handle)
        post_message_coalesced( top->handle, WM_WINE_UPDATEWINDOWSTATE,
                                WINE_UPDATE_CLIENT_SURFACE_HANDOFFS, 0 );
}

static struct client_surface_owner *get_client_surface_owner( struct window *win,
                                                              struct process *process, int create )
{
    struct client_surface_owner *owner;

    LIST_FOR_EACH_ENTRY( owner, &win->client_surface_owners, struct client_surface_owner, entry )
        if (owner->process == process) return owner;
    if (!create || !(owner = mem_alloc( sizeof(*owner) ))) return NULL;
    owner->process = (struct process *)grab_object( process );
    list_init( &owner->surfaces );
    list_add_tail( &win->client_surface_owners, &owner->entry );
    return owner;
}

static struct client_surface_owner *get_client_surface_owner_by_id( struct window *win,
                                                                    process_id_t process )
{
    struct client_surface_owner *owner;

    LIST_FOR_EACH_ENTRY( owner, &win->client_surface_owners, struct client_surface_owner, entry )
        if (owner->process->id == process) return owner;
    return NULL;
}

static struct client_surface_ref *get_client_surface_ref( struct client_surface_owner *owner,
                                                          UINT64 id, int create )
{
    struct client_surface_ref *surface;

    if (!owner) return NULL;
    if ((surface = find_indexed_client_surface_ref( owner->process, id )) && surface->owner == owner)
        return surface;
    if (!create) return NULL;
    /* Only a server-issued, unbound reservation may start registration.
     * Retired records remain indexed only while a channel or notification
     * owns them; they can never be rebound to another window lifetime. */
    if (!surface || surface->owner || surface->handoff || surface->notification_pending ||
        surface->destroy_state != CLIENT_SURFACE_DESTROY_NONE)
    {
        set_error( STATUS_INVALID_PARAMETER );
        return NULL;
    }
    surface->owner = owner;
    list_add_tail( &owner->surfaces, &surface->entry );
    return surface;
}

static void release_client_surface_owner( struct client_surface_owner *owner )
{
    if (!list_empty( &owner->surfaces )) return;
    list_remove( &owner->entry );
    release_object( owner->process );
    free( owner );
}

static struct client_surface_ref *select_client_surface_producer( struct window *win,
                                                                  struct client_surface_owner **selected_owner )
{
    struct client_surface_owner *owner;
    struct client_surface_ref *surface, *selected = NULL;
    int selected_active = 0;

    *selected_owner = NULL;
    LIST_FOR_EACH_ENTRY( owner, &win->client_surface_owners, struct client_surface_owner, entry )
    {
        LIST_FOR_EACH_ENTRY( surface, &owner->surfaces, struct client_surface_ref, entry )
        {
            if (surface->active && surface->claimed &&
                (!selected_active || !selected || surface->sequence > selected->sequence))
            {
                selected = surface;
                *selected_owner = owner;
                selected_active = 1;
            }
            else if (!selected_active && surface->cached &&
                     (!selected || surface->sequence > selected->sequence))
            {
                selected = surface;
                *selected_owner = owner;
            }
        }
    }
    return selected;
}

static int client_surface_has_visible_descendant_producer( struct window *win )
{
    struct client_surface_owner *owner;
    struct window *child;

    if (!win->client_surface_subtree_count) return 0;
    if (is_visible( win ) && select_client_surface_producer( win, &owner )) return 1;
    LIST_FOR_EACH_ENTRY( child, &win->children, struct window, entry )
        if (client_surface_has_visible_descendant_producer( child )) return 1;
    return 0;
}

/* The server decides whether a stable scene has exactly one selected producer.
 * Backend-local geometry, visual and clipping constraints may still downgrade
 * this candidate to COMPOSITED.  Restrict the initial direct path to the
 * owner process: foreign HWNDs have no local win_data with which to attach the
 * producer's native child window. */
static struct client_surface_ref *get_client_surface_native_candidate( struct window *top,
                                                                      struct client_surface_owner **owner )
{
    struct client_surface_ref *selected;
    struct client_surface_owner *entry;
    struct window *child;

    *owner = NULL;
    if (!is_visible( top ) || top->client_surface_subtree_count != 1 ||
        top->client_surface_count != 1 || !top->thread)
        return NULL;
    selected = select_client_surface_producer( top, owner );
    if (!selected)
        LIST_FOR_EACH_ENTRY( entry, &top->client_surface_owners, struct client_surface_owner, entry )
        {
            struct client_surface_ref *surface;
            LIST_FOR_EACH_ENTRY( surface, &entry->surfaces, struct client_surface_ref, entry )
                if (surface->active && surface->candidate_serial)
                {
                    selected = surface;
                    *owner = entry;
                }
        }
    if (!selected || !selected->active || !selected->direct_presentation ||
        (*owner)->process != top->thread->process)
        return NULL;
    LIST_FOR_EACH_ENTRY( child, &top->children, struct window, entry )
        if (client_surface_has_visible_descendant_producer( child ))
            return NULL;
    return selected;
}

static int client_surface_direct_candidate( struct window *top )
{
    struct client_surface_owner *owner;
    return !!get_client_surface_native_candidate( top, &owner );
}

/* A scene can prepare the sole native candidate without advertising it as
 * a completed producer. Existing completed images always take precedence. */
static struct client_surface_ref *select_client_surface_scene_producer( struct window *win,
                                                                        struct client_surface_owner **owner )
{
    struct client_surface_ref *surface = select_client_surface_producer( win, owner );
    if (!surface && get_toplevel_window( win ) == win)
        surface = get_client_surface_native_candidate( win, owner );
    return surface;
}

/* Registration precedes the first producer claim.  Avoid provisioning a
 * backing in that gap when the sole possible producer is a same-process
 * DIRECT backend; prepare_present() requests native admission before swapping.
 * Any second identity, descendant or foreign/non-DIRECT producer removes
 * this exemption immediately on the topology slow path. */
static int client_surface_direct_registration_candidate( struct window *top )
{
    struct client_surface_owner *owner;
    struct client_surface_ref *surface;

    if (!is_visible( top ) || !top->thread || top->client_surface_subtree_count != 1 ||
        top->client_surface_count != 1 || top->client_surface_cached_count)
        return 0;
    LIST_FOR_EACH_ENTRY( owner, &top->client_surface_owners,
                         struct client_surface_owner, entry )
        LIST_FOR_EACH_ENTRY( surface, &owner->surfaces,
                             struct client_surface_ref, entry )
            if (surface->active)
                return !surface->claimed && surface->direct_presentation && owner->process == top->thread->process;
    return 0;
}

static int client_surface_direct_eligible( struct window *top )
{
    return !(top->client_surface_scene_generation & 1) &&
           !top->client_surface_transaction.staged && !top->client_surface_native_barrier &&
           top->client_surface_direct_scene == top->client_surface_scene_generation &&
           top->client_surface_direct_surface &&
           client_surface_direct_candidate( top );
}

static enum client_surface_presentation_mode client_surface_presentation_mode( struct window *top )
{
    if (client_surface_direct_eligible( top )) return CLIENT_SURFACE_PRESENTATION_DIRECT;
    if (top->client_surface_transaction.staged) return CLIENT_SURFACE_PRESENTATION_STAGED;
    return CLIENT_SURFACE_PRESENTATION_COMPOSITED;
}

static unsigned int get_client_surface_backend_caps( const struct client_surface_ref *surface )
{
    return surface->scene_publication |
           surface->direct_presentation << 2;
}

static int client_surface_scene_published_recursive( struct window *win,
                                                     unsigned int *selected_count )
{
    struct client_surface_owner *owner;
    struct client_surface_ref *selected;
    struct window *child;

    if (!win->client_surface_subtree_count) return 1;
    selected = select_client_surface_scene_producer( win, &owner );
    if (selected && is_visible( win ))
    {
        (*selected_count)++;
        if (!selected->scene_publication) return 0;
    }
    LIST_FOR_EACH_ENTRY( child, &win->children, struct window, entry )
        if (!client_surface_scene_published_recursive( child, selected_count )) return 0;
    return 1;
}

static int client_surface_scene_published( struct window *top )
{
    unsigned int selected_count = 0;

    return client_surface_scene_published_recursive( top, &selected_count ) && selected_count;
}

static void update_client_surface_producer( struct window *win )
{
    struct client_surface_owner *owner;
    struct client_surface_ref *surface = select_client_surface_producer( win, &owner );

    SHARED_WRITE_BEGIN( win->shared, window_shm_t )
    {
        shared->client_surface_process = surface ? owner->process->id : 0;
        shared->client_surface_id = surface ? surface->id : 0;
        shared->client_surface_producer_sequence = surface ? surface->sequence : 0;
    }
    SHARED_WRITE_END;
}

static void cancel_client_surface_timeout( struct window *top )
{
    if (!top->client_surface_transaction.timeout) return;
    remove_timeout_user( top->client_surface_transaction.timeout );
    top->client_surface_transaction.timeout = NULL;
}

/* Client-side host completion is bounded at five seconds.  Publication must
 * not fail open while a valid completion transaction can still commit. */
#define CLIENT_SURFACE_PUBLICATION_TIMEOUT (6 * TICKS_PER_SEC)

static void finish_client_surface_generation( struct window *top )
{
    cancel_client_surface_timeout( top );
    top->client_surface_transaction.deadline = 0;
    top->client_surface_transaction.phase = CLIENT_SURFACE_PHASE_IDLE;
    top->client_surface_transaction.publication = CLIENT_SURFACE_PUBLICATION_NONE;
    top->client_surface_transaction.pending = 0;
    top->client_surface_transaction.epoch = 0;
    top->client_surface_transaction.owner_repair = 0;
    top->client_surface_transaction.source_pending = 0;
    update_client_surface_publication( top );
}

static void finish_client_surface_publication( struct window *top )
{
    top->client_surface_dirty = 0;
    top->client_surface_transaction.staged = 0;
    if (client_surface_is_preparing( top )) top->client_surface_transaction.phase = CLIENT_SURFACE_PHASE_IDLE;
    top->client_surface_transaction.prepared = 0;
    top->client_surface_transaction.owner_repair = 0;
    finish_client_surface_generation( top );
}

static int mark_client_surface_generation_ready( struct window *top, int notify )
{
    if (!client_surface_is_composing( top ) || top->client_surface_transaction.pending ||
        top->client_surface_transaction.source_pending ||
        top->client_surface_transaction.epoch != top->client_surface_scene_generation ||
        (top->client_surface_scene_generation & 1))
        return 0;

    /* Backends without an owner-managed scene target keep the legacy live
     * behavior: their driver presentation is already visible, so an empty
     * owner prepare/publish round trip cannot add atomicity. */
    if (!top->client_surface_transaction.staged && !client_surface_scene_published( top ))
    {
        finish_client_surface_generation( top );
        return 0;
    }

    if (client_surface_is_ready( top )) return 0;

    top->client_surface_transaction.phase = CLIENT_SURFACE_PHASE_READY;
    top->client_surface_transaction.publication = CLIENT_SURFACE_PUBLICATION_COPY;
    /* Native completion reserves HANDOFF before its server request returns.
     * Its intermediate READY state cannot admit a GUI copy; only a later
     * successful staged handoff needs the existing EXPOSURE_READY wake. */
    if (notify)
        post_message_coalesced( top->handle, WM_WINE_UPDATEWINDOWSTATE,
                                WINE_PUBLISH_CLIENT_SURFACES, 0 );
    return 1;
}

static void fail_client_surface_publication( struct window *top )
{
    clear_client_surface_subtree_generation( top, client_surface_transaction_generation( top ) );
    finish_client_surface_generation( top );
    top->client_surface_transaction.prepared = 0;
    top->client_surface_transaction.owner_repair = 0;
    /* Keep staging and the last complete output. A failed copy, skipped
     * Present, or elapsed deadline proves neither a complete scene nor its
     * publication. A new epoch lets subsequent geometry/renderer activity
     * rebuild the scene; late ACKs cannot expose the rejected output. */
    invalidate_client_surface_scene( top );
}

static void retire_stale_client_surface_exposure( struct window *top, int exposed )
{
    abstime_t deadline = top->client_surface_transaction.deadline;

    assert( top->client_surface_transaction.staged );
    assert( top->client_surface_transaction.epoch != top->client_surface_scene_generation );
    /* The native owner has finished; no future callback can retire this old
     * token. Its scene already changed, so do not invalidate it again (it
     * may be odd inside a native barrier). Preserve staging and the episode's
     * deadline while the current scene takes over. */
    clear_client_surface_subtree_generation( top, client_surface_transaction_generation( top ) );
    /* A successful GUI exposure already released native/local staging. A
     * failed or unreserved exposure still owns it. Neither acknowledges the
     * invalidated scene, but recovery must match that actual native state. */
    if (exposed)
    {
        top->client_surface_dirty = 0;
        top->client_surface_transaction.staged = 0;
    }
    finish_client_surface_generation( top );
    top->client_surface_transaction.prepared = 0;
    top->client_surface_transaction.deadline = deadline;
    if (!top->client_surface_scene_change_depth && is_visible( top ) && has_client_surface( top ))
        restart_client_surface_generation( top );
    /* An active barrier's existing END hook restarts the even scene. */
}

static void client_surface_publication_timeout( void *private )
{
    struct window *top = private;
    unsigned long long generation = client_surface_transaction_generation( top );
    int staged = top->client_surface_transaction.staged;
    int repair = staged && client_surface_is_publishing( top ) &&
                 top->client_surface_transaction.epoch != top->client_surface_scene_generation;

    top->client_surface_transaction.timeout = NULL;
    if (top->handle && client_surface_scene_published( top ))
    {
        fail_client_surface_publication( top );
        return;
    }
    if (client_surface_is_preparing( top ))
    {
        if (!top->handle) return;

        /* Preparing is an owner-side snapshot transaction.  A dropped
         * message, allocation failure, or unresponsive owner must not block
         * every later renderer completion indefinitely.  Keep the visible
         * host as the authoritative image and rebuild the backing lazily. */
        top->client_surface_transaction.deadline = 0;
        top->client_surface_transaction.phase = CLIENT_SURFACE_PHASE_IDLE;
        top->client_surface_transaction.prepared = 0;
        top->client_surface_transaction.owner_repair = 0;
        top->client_surface_transaction.source_pending = 0;
        update_client_surface_publication( top );
        if (top->client_surface_transaction.staged)
        {
            finish_client_surface_publication( top );
            if (is_visible( top )) post_message( top->handle, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
        }
        return;
    }
    if (!top->handle || !client_surface_is_composing( top ) ||
        generation != client_surface_transaction_generation( top )) return;

    clear_client_surface_subtree_generation( top, generation );
    if (staged)
    {
        finish_client_surface_publication( top );
        if (is_visible( top )) post_message( top->handle, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
        /* The owner can stall after PUBLISH_BEGIN while a renderer completes a
         * newer frame.  Fail the host exposure open, but do not lose that frame
         * merely because the matching ACK never arrived. */
        if (repair && is_visible( top ) && has_client_surface( top ))
            restart_client_surface_generation( top );
    }
    else
        finish_client_surface_generation( top );
}

static void arm_client_surface_timeout( struct window *top )
{
    if (!top->client_surface_transaction.deadline)
        top->client_surface_transaction.deadline = -monotonic_time - CLIENT_SURFACE_PUBLICATION_TIMEOUT;
    if (top->client_surface_transaction.timeout) return;
    top->client_surface_transaction.timeout = add_timeout_user( abstime_to_timeout( top->client_surface_transaction.deadline ),
                                                    client_surface_publication_timeout, top );
    if (!top->client_surface_transaction.timeout)
    {
        /* Allocation failure cannot turn an optimization into a permanently
         * invisible host window. */
        clear_error();
        client_surface_publication_timeout( top );
    }
}

static int complete_client_surface_generation( struct window *top, struct client_surface_ref *surface,
                                               unsigned long long generation, int notify )
{
    if (!client_surface_is_composing( top ) ||
        top->client_surface_transaction.source_pending ||
        generation != client_surface_transaction_generation( top ) ||
        surface->generation != generation)
        return 0;

    surface->generation = 0;
    assert( top->client_surface_transaction.pending );
    if (--top->client_surface_transaction.pending) return 0;

    return mark_client_surface_generation_ready( top, notify );
}

/* A composition generation describes a scene, not a single frame.  A selected
 * producer can complete another frame after it committed that scene but before
 * the owner starts exposing it.  Re-open that producer in the same generation
 * so PUBLISH_BEGIN is the content cut-over point as well as the scene cut-over
 * point. */
static int reopen_client_surface_generation( struct window *top, struct window *win,
                                             struct client_surface_ref *surface,
                                             unsigned long long generation )
{
    struct client_surface_owner *selected_owner;

    if (!client_surface_is_composing( top ) || client_surface_is_publishing( top ) ||
        generation != client_surface_transaction_generation( top ) || surface->generation ||
        !is_visible( win ) || select_client_surface_producer( win, &selected_owner ) != surface)
        return 0;

    assert( surface->active || surface->cached );
    assert( top->client_surface_transaction.pending < UINT_MAX );
    surface->generation = generation;
    top->client_surface_transaction.pending++;
    top->client_surface_transaction.phase = CLIENT_SURFACE_PHASE_COMPOSING;
    top->client_surface_transaction.publication = CLIENT_SURFACE_PUBLICATION_NONE;
    update_client_surface_publication( top );
    return 1;
}

static unsigned int clear_client_surface_subtree_generation( struct window *win,
                                                             unsigned long long generation )
{
    struct client_surface_owner *owner;
    struct client_surface_ref *surface;
    struct window *child;
    unsigned int count = 0;

    if (!win->client_surface_subtree_count) return 0;

    LIST_FOR_EACH_ENTRY( owner, &win->client_surface_owners, struct client_surface_owner, entry )
    {
        LIST_FOR_EACH_ENTRY( surface, &owner->surfaces, struct client_surface_ref, entry )
        {
            if (surface->generation != generation) continue;
            surface->generation = 0;
            count++;
        }
    }
    LIST_FOR_EACH_ENTRY( child, &win->children, struct window, entry )
        /* Descendants can remain logically visible while their ancestor is
         * hidden, so retire the complete subtree without a visibility test. */
        count += clear_client_surface_subtree_generation( child, generation );
    return count;
}

static unsigned int clear_client_surface_owner_generation( struct client_surface_owner *owner,
                                                            unsigned long long generation )
{
    struct client_surface_ref *surface;
    unsigned int count = 0;

    LIST_FOR_EACH_ENTRY( surface, &owner->surfaces, struct client_surface_ref, entry )
    {
        if (surface->generation != generation) continue;
        surface->generation = 0;
        count++;
    }
    return count;
}

static int retire_client_surface_owner_generation( struct window *top,
                                                    struct client_surface_owner *owner,
                                                    unsigned long long generation )
{
    unsigned int count;

    if (!client_surface_is_composing( top ) ||
        generation != client_surface_transaction_generation( top )) return 0;
    if (!(count = clear_client_surface_owner_generation( owner, generation ))) return 0;

    assert( top->client_surface_transaction.pending >= count );
    top->client_surface_transaction.pending -= count;
    if (top->client_surface_transaction.pending) return 0;

    return mark_client_surface_generation_ready( top, 1 );
}

static int retire_client_surface_subtree_generation( struct window *top, struct window *win,
                                                     unsigned long long generation )
{
    unsigned int count;

    if (!client_surface_is_composing( top ) ||
        generation != client_surface_transaction_generation( top )) return 0;

    count = clear_client_surface_subtree_generation( win, generation );
    if (!count) return 0;
    assert( top->client_surface_transaction.pending >= count );
    top->client_surface_transaction.pending -= count;
    if (top->client_surface_transaction.pending) return 0;

    return mark_client_surface_generation_ready( top, 1 );
}

static void discard_client_surface_owner( struct window *win, struct client_surface_owner *owner,
                                          struct window *top )
{
    struct client_surface_ref *surface, *next;
    unsigned int removed = 0;

    if (!list_empty( &owner->surfaces )) begin_client_surface_scene_change( top );
    LIST_FOR_EACH_ENTRY_SAFE( surface, next, &owner->surfaces, struct client_surface_ref, entry )
    {
        assert( surface->active || surface->cached );
        if (surface->active)
        {
            assert( win->client_surface_count );
            win->client_surface_count--;
            removed++;
        }
        if (surface->cached)
        {
            assert( win->client_surface_cached_count );
            win->client_surface_cached_count--;
            removed++;
        }
        complete_client_surface_generation( top, surface,
                                            client_surface_transaction_generation( top ), 1 );
        retire_client_surface_ref( surface );
    }
    if (removed) adjust_client_surface_subtree_count( win, -(int)removed );
    if (removed) update_client_surface_producer( win );
    release_client_surface_owner( owner );
    if (removed) end_client_surface_scene_change( top );
}

static int discard_client_surface_owners( struct window *win, struct window *top )
{
    struct client_surface_owner *owner, *next;
    struct client_surface_ref *surface;
    int was_pending = top->client_surface_dirty;

    LIST_FOR_EACH_ENTRY_SAFE( owner, next, &win->client_surface_owners,
                              struct client_surface_owner, entry )
    {
        /* The HWND may be reused before this cross-process message is
         * dispatched.  Address each process-local object by the same opaque
         * identity used for recomposition; an HWND-only destroy could detach
         * surfaces already created for the new window lifecycle. */
        LIST_FOR_EACH_ENTRY( surface, &owner->surfaces, struct client_surface_ref, entry )
        {
            assert( surface->destroy_state == CLIENT_SURFACE_DESTROY_NONE );
            surface->destroy_state = CLIENT_SURFACE_DESTROY_PENDING;
            owner->process->client_surface_destroy_count++;
            if (post_client_surface_notification( surface, 0, WM_WINE_DESTROYCLIENTSURFACE ))
            {
                surface->destroy_state = CLIENT_SURFACE_DESTROY_QUEUED;
            }
            else clear_error();
        }
        discard_client_surface_owner( win, owner, top );
    }

    if (!has_client_surface( top ))
        finish_client_surface_publication( top );
    return was_pending && is_visible( top ) && !top->client_surface_dirty;
}

void cleanup_process_client_surfaces( struct process *process )
{
    struct client_surface_ref *surface, *next;
    user_handle_t handle = 0;
    struct client_surface_owner *owner;
    struct window *win, *top;
    unsigned int bucket;
    int was_pending;

    /* Client surfaces may render a foreign process's HWND and therefore
     * outlive every window owned by this process.  Retire those identities
     * when its last thread exits instead of retaining the dead process until
     * the foreign window happens to change geometry or is destroyed. */
    while ((win = next_user_handle( &handle, NTUSER_OBJ_WINDOW )))
    {
        if (!(owner = get_client_surface_owner( win, process, 0 ))) continue;
        top = get_toplevel_window( win );
        was_pending = top->client_surface_dirty;
        discard_client_surface_owner( win, owner, top );
        if (!has_client_surface( top ))
            finish_client_surface_publication( top );
        if (was_pending && is_visible( top ) && !top->client_surface_dirty)
            post_message( top->handle, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
    }

    /* Either endpoint disappearing closes its channels.
     * Surviving mapped handles retain the section long enough to observe that
     * state; the server binding itself must not retain raw process pointers. */
    for (bucket = 0; bucket < CLIENT_SURFACE_REF_BUCKETS; bucket++)
    {
        for (surface = client_surface_ref_index[bucket]; surface; surface = next)
        {
            next = surface->index_next;
            if (!surface->handoff) continue;
            surface->handoff = client_surface_handoff_cleanup_binding( surface->handoff, process );
            if (!surface->handoff) free_client_surface_ref_if_unused( surface );
        }
    }

    /* All process queues have been destroyed before the final thread leaves.
     * Drop undeliverable destroy tombstones now; no queue callback can still
     * address them, and retaining their raw process key would outlive it. */
    for (bucket = 0; bucket < CLIENT_SURFACE_REF_BUCKETS; bucket++)
    {
        for (surface = client_surface_ref_index[bucket]; surface; surface = next)
        {
            next = surface->index_next;
            if (surface->process != process)
                continue;
            assert( !surface->owner && !surface->notification_pending );
            if (surface->destroy_state != CLIENT_SURFACE_DESTROY_NONE)
            {
                assert( surface->destroy_state == CLIENT_SURFACE_DESTROY_PENDING );
                assert( process->client_surface_destroy_count );
                process->client_surface_destroy_count--;
                surface->destroy_state = CLIENT_SURFACE_DESTROY_NONE;
            }
            free_client_surface_ref_if_unused( surface );
        }
    }
    assert( !process->client_surface_destroy_count );
    client_surface_handoff_cleanup_pools( process );
}

static struct client_surface_ref *get_client_surface_handoff_ref( struct window *win,
                                                                  process_id_t producer,
                                                                  UINT64 id,
                                                                  int owner_view )
{
    struct client_surface_owner *owner, *selected_owner;
    struct client_surface_ref *surface;
    struct window *top = get_toplevel_window( win );

    if (!owner_view)
    {
        if (producer && producer != current->process->id)
        {
            set_error( STATUS_ACCESS_DENIED );
            return NULL;
        }
        owner = get_client_surface_owner( win, current->process, 0 );
    }
    else
    {
        if (!top->thread || top->thread->process != current->process)
        {
            set_error( STATUS_ACCESS_DENIED );
            return NULL;
        }
        owner = get_client_surface_owner_by_id( win, producer );
    }
    surface = get_client_surface_ref( owner, id, 0 );
    if (!surface || (!surface->active && !surface->cached))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return NULL;
    }
    /* A sole native candidate may need an offscreen first image if DIRECT
     * admission fails. Provision its transport without granting publication
     * authority; an existing completed producer always wins scene selection. */
    if (owner_view && select_client_surface_scene_producer( win, &selected_owner ) != surface)
    {
        set_error( STATUS_INVALID_PARAMETER );
        return NULL;
    }
    return surface;
}

DECL_HANDLER(get_client_surface_handoff)
{
    struct client_surface_ref *surface;
    struct client_surface_handoff_state state;
    struct client_surface_handoff_mapping mapping;
    struct window *top, *win;
    int producer_view = !req->owner;

    reply->mapping = 0;
    reply->size = reply->offset = 0;
    reply->mapping_id = reply->cookie = 0;
    if (!(win = get_window( req->handle ))) return;
    top = get_toplevel_window( win );
    if (!(surface = get_client_surface_handoff_ref( win, req->producer, req->surface,
                                                    req->owner ))) return;
    state = client_surface_handoff_get_state( surface->handoff );
    /* A hidden producer hint may become stale before its owner registers.
     * Do not turn that transport-only request into channel allocation or
     * resurrect a closed lifetime. Ordinary visible provisioning is unchanged. */
    if (req->require_producer && (!req->owner || !surface->handoff ||
        !(state.mapped & CLIENT_SURFACE_HANDOFF_ENDPOINT_PRODUCER) || state.owner != &top->obj ||
        state.retired || state.lost))
    {
        set_error( STATUS_INVALID_HANDLE );
        return;
    }
    if (surface->handoff &&
        (state.owner != &top->obj || state.retired || state.lost))
    {
        /* Do not reacquire an endpoint of a failed binding while its peer
         * is retiring it. Otherwise both sides can repeatedly remap the
         * same closed channel cookie and prevent its final release forever. */
        if (state.owner != &top->obj || state.retired)
            retarget_client_surface_handoff( surface );
        else retire_client_surface_handoff( surface );
        if (client_surface_handoff_get_state( surface->handoff ).mapped)
        {
            set_error( STATUS_DEVICE_BUSY );
            return;
        }
        free_client_surface_handoff( surface );
    }
    if (!surface->handoff && !alloc_client_surface_handoff( surface, win, top )) return;
    if (!(reply->mapping = client_surface_handoff_map( surface->handoff, current->process,
                                                       producer_view, &mapping ))) return;
    reply->mapping_id = mapping.id;
    reply->size = mapping.size;
    reply->offset = mapping.offset;
    reply->cookie = mapping.cookie;
}

DECL_HANDLER(get_client_surface_handoff_event)
{
    struct client_surface_ref *surface;
    struct client_surface_handoff_state state;
    struct window *win;

    if (!(win = get_window( req->handle ))) return;
    if (!(surface = get_client_surface_handoff_ref( win, req->producer, req->surface,
                                                    req->owner ))) return;
    state = client_surface_handoff_get_state( surface->handoff );
    if (!surface->handoff || state.cookie != req->cookie ||
        state.owner != &get_toplevel_window( win )->obj)
    {
        set_error( STATUS_INVALID_HANDLE );
        return;
    }
    reply->event = client_surface_handoff_get_event( surface->handoff, current->process, !req->owner );
}

DECL_HANDLER(get_client_surface_handoff_visibility)
{
    struct client_surface_owner *owner;
    struct client_surface_ref *surface;
    struct client_surface_handoff_state state;
    struct window *win;

    if (!(win = get_window( req->handle ))) return;
    if (!(surface = get_client_surface_handoff_ref( win, 0, req->surface, 0 ))) return;
    state = client_surface_handoff_get_state( surface->handoff );
    if (!surface->handoff || !(state.mapped & CLIENT_SURFACE_HANDOFF_ENDPOINT_PRODUCER) ||
        state.cookie != req->cookie || state.retired ||
        state.owner != &get_toplevel_window( win )->obj || state.lost)
    {
        set_error( STATUS_INVALID_HANDLE );
        return;
    }
    if (select_client_surface_producer( win, &owner ) != surface)
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    /* The registered HWND and server-owned binding authenticate this lookup;
     * the writable channel's window/endpoint fields are not authority. */
    reply->visible = is_visible( win );
}

DECL_HANDLER(release_client_surface_handoff)
{
    struct client_surface_ref *surface;
    struct client_surface_handoff_state state;
    user_handle_t refresh = 0, current_refresh = 0;
    int producer_view = !req->owner;

    if (producer_view && req->producer && req->producer != current->process->id)
    {
        set_error( STATUS_ACCESS_DENIED );
        return;
    }

    if (producer_view)
        surface = find_indexed_client_surface_ref( current->process, req->surface );
    else
    {
        unsigned int bucket;

        surface = NULL;
        for (bucket = 0; bucket < CLIENT_SURFACE_REF_BUCKETS && !surface; ++bucket)
        {
            struct client_surface_ref *cursor;

            for (cursor = client_surface_ref_index[bucket]; cursor; cursor = cursor->index_next)
                if (cursor->process->id == req->producer && cursor->id == req->surface &&
                    cursor->handoff && client_surface_handoff_get_state( cursor->handoff ).consumer == current->process)
                {
                    surface = cursor;
                    break;
                }
        }
    }
    state = client_surface_handoff_get_state( surface ? surface->handoff : NULL );
    if (!surface || !surface->handoff || !req->cookie ||
        req->cookie != state.cookie ||
        (req->handle && ((struct window *)state.owner)->handle != req->handle))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    client_surface_handoff_release_endpoint( surface->handoff, producer_view );
    state = client_surface_handoff_get_state( surface->handoff );
    if (!state.mapped && ((!surface->active && !surface->cached) || state.lost))
    {
        if ((surface->active || surface->cached) && state.owner)
        {
            struct client_surface_owner *owner;
            struct window *win;

            refresh = ((struct window *)state.owner)->handle;
            /* A new owner may already have tried to bind while the old
             * mapping was still pinned. Wake it when that last endpoint
             * releases, as well as the old root which needs cleanup. The
             * shared window handle is only a lookup hint: authenticate its
             * current registration and selection before routing the wake. */
            if ((win = get_user_object( state.window, NTUSER_OBJ_WINDOW )) &&
                get_client_surface_owner( win, surface->process, 0 ) == surface->owner &&
                select_client_surface_producer( win, &owner ) == surface)
                current_refresh = get_toplevel_window( win )->handle;
        }
        free_client_surface_handoff( surface );
    }
    free_client_surface_ref_if_unused( surface );
    if (refresh)
        post_message_coalesced( refresh, WM_WINE_UPDATEWINDOWSTATE,
                                WINE_UPDATE_CLIENT_SURFACE_HANDOFFS, 0 );
    if (current_refresh && current_refresh != refresh)
        post_message_coalesced( current_refresh, WM_WINE_UPDATEWINDOWSTATE,
                                WINE_UPDATE_CLIENT_SURFACE_HANDOFFS, 0 );
}

static int validate_client_surface_handoff_generation( struct window *win, struct window *top,
                                                       unsigned long long generation,
                                                       int owner_repair, int partial,
                                                       const struct client_surface_handoff_receipt *receipts,
                                                       unsigned int receipt_count,
                                                       unsigned int *count )
{
    struct client_surface_owner *owner;
    struct client_surface_ref *surface;
    struct client_surface_handoff_state state;
    struct window *child;

    if (!win->client_surface_subtree_count || !is_visible( win )) return 1;
    if ((surface = select_client_surface_producer( win, &owner )))
    {
        const struct client_surface_handoff_receipt *receipt;
        unsigned int low = 0, high = receipt_count;

        while (low < high)
        {
            unsigned int mid = low + (high - low) / 2;

            if (receipts[mid].handle < win->handle) low = mid + 1;
            else high = mid;
        }
        if (low == receipt_count || receipts[low].handle != win->handle)
        {
            if (partial) goto children;
            return 0;
        }
        state = client_surface_handoff_get_state( surface->handoff );
        if ((!owner_repair && surface->generation != generation) || !surface->handoff ||
            state.owner != &top->obj || state.consumer != current->process)
            return 0;
        /* A completed publication may outlive source endpoint retirement, but
         * a new repair must not authorize a binding already being replaced.
         * Once recovery was requested, a remapped endpoint is not fresh proof. */
        if (owner_repair && (!(state.mapped & CLIENT_SURFACE_HANDOFF_ENDPOINT_CONSUMER) || state.retired ||
                            state.lost ||
                            (partial && surface->source_required))) return 0;
        receipt = &receipts[low];
        if (receipt->handle != win->handle || receipt->process != owner->process->id ||
            receipt->surface != surface->id || receipt->cookie != state.cookie ||
            !receipt->source_generation || receipt->buffer_index >= CLIENT_SURFACE_HANDOFF_RING_SIZE)
            return 0;
        (*count)++;
    }
children:
    LIST_FOR_EACH_ENTRY( child, &win->children, struct window, entry )
        if (!validate_client_surface_handoff_generation( child, top, generation,
                                                         owner_repair, partial, receipts, receipt_count, count ))
            return 0;
    return 1;
}

DECL_HANDLER(complete_client_surface_handoffs)
{
    struct window *top, *win;
    const struct client_surface_handoff_receipt *receipts = get_req_data();
    unsigned int count = 0, cleared, i, receipt_count = get_req_data_size() / sizeof(*receipts);

    reply->accepted = 0;
    if (!(win = get_window( req->handle ))) return;
    top = get_toplevel_window( win );
    if (!top->thread || top->thread->process != current->process)
    {
        set_error( STATUS_ACCESS_DENIED );
        return;
    }
    if (get_req_data_size() % sizeof(*receipts))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    for (i = 1; i < receipt_count; ++i)
        if (receipts[i - 1].handle >= receipts[i].handle)
        {
            set_error( STATUS_INVALID_PARAMETER );
            return;
        }
    if (!client_surface_is_composing( top ) || client_surface_is_publishing( top ) ||
        req->generation != client_surface_transaction_generation( top ) ||
        req->scene_generation != top->client_surface_scene_generation ||
        req->scene_generation != top->client_surface_transaction.epoch ||
        !top->client_surface_transaction.pending ||
        top->client_surface_transaction.source_pending ||
        receipt_count != top->client_surface_transaction.pending ||
        !validate_client_surface_handoff_generation( top, top, req->generation,
                                                     0, 0, receipts, receipt_count, &count ) ||
        count != top->client_surface_transaction.pending)
        return;

    cleared = clear_client_surface_subtree_generation( top, req->generation );
    assert( cleared == count );
    top->client_surface_transaction.pending = 0;
    if (!mark_client_surface_generation_ready( top, 0 ) || !client_surface_is_ready( top )) return;

    top->client_surface_transaction.phase = CLIENT_SURFACE_PHASE_PUBLISHING;
    top->client_surface_transaction.publication = CLIENT_SURFACE_PUBLICATION_HANDOFF;
    update_client_surface_publication( top );
    reply->accepted = 1;
}

/* The owner supplies receipts only after checking its retained native images.
 * A cold or incompatible cache continues through producer source recovery. */
DECL_HANDLER(request_client_surface_owner_repair)
{
    const struct client_surface_handoff_receipt *receipts = get_req_data();
    unsigned int count = 0, i, receipt_count = get_req_data_size() / sizeof(*receipts);
    struct window *top = get_window( req->handle );

    reply->accepted = 0;
    if (!top) return;
    if (get_toplevel_window( top ) != top || !top->thread || top->thread->process != current->process)
    {
        set_error( STATUS_ACCESS_DENIED );
        return;
    }
    if (get_req_data_size() % sizeof(*receipts))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    for (i = 1; i < receipt_count; ++i)
        if (receipts[i - 1].handle >= receipts[i].handle)
        {
            set_error( STATUS_INVALID_PARAMETER );
            return;
        }
    if ((req->scene_id & 1) || req->scene_id != top->client_surface_scene_generation ||
        !receipt_count || !is_visible( top ) ||
        !validate_client_surface_handoff_generation( top, top, 0, 1, 0, receipts, receipt_count, &count ) ||
        count != receipt_count)
        return;

    if (!client_surface_is_composing( top ) && !client_surface_is_preparing( top ))
    {
        top->client_surface_transaction.owner_repair = 1;
        restart_client_surface_generation_internal( top );
    }
    /* An existing assembly needs another owner pass after native repair, not
     * another producer publication. PREPARING keeps its original request reason. */
    post_message_coalesced( top->handle, WM_WINE_UPDATEWINDOWSTATE,
                            WINE_UPDATE_CLIENT_SURFACE_HANDOFFS, 0 );
    reply->accepted = 1;
}

DECL_HANDLER(cancel_client_surface_handoffs)
{
    struct window *top, *win = get_window( req->handle );

    if (!win) return;
    top = get_toplevel_window( win );
    if (!top->thread || top->thread->process != current->process)
    {
        set_error( STATUS_ACCESS_DENIED );
        return;
    }
    if (top->client_surface_transaction.phase != CLIENT_SURFACE_PHASE_COMPOSING ||
        req->generation != client_surface_transaction_generation( top ) ||
        req->scene_generation != top->client_surface_scene_generation) return;
    /* Released producer images need a new replay request if their receipts
     * are discarded. Never turn a partial output into a completed scene just
     * because its private assembly was cancelled. */
    begin_client_surface_scene_change( top );
    end_client_surface_scene_change( top );
}

DECL_HANDLER(publish_client_surface_handoff)
{
    struct window *top, *win;
    int invalidated;

    reply->accepted = 0;
    if (!(win = get_window( req->handle ))) return;
    top = get_toplevel_window( win );
    if (!top->thread || top->thread->process != current->process)
    {
        set_error( STATUS_ACCESS_DENIED );
        return;
    }
    if (!client_surface_is_publishing( top ) ||
        (top->client_surface_transaction.publication != CLIENT_SURFACE_PUBLICATION_HANDOFF &&
         (req->success || current != top->thread)) ||
        req->generation != client_surface_transaction_generation( top ) ||
        req->scene_generation != top->client_surface_transaction.epoch)
        return;

    invalidated = top->client_surface_transaction.epoch != top->client_surface_scene_generation;
    reply->accepted = 1;
    if (!req->success)
    {
        if (top->client_surface_transaction.staged && invalidated)
            retire_stale_client_surface_exposure( top, 0 );
        else fail_client_surface_publication( top );
        return;
    }
    if (top->client_surface_transaction.staged)
    {
        if (invalidated)
        {
            /* Native completion of an old image cannot authorize exposure
             * of the new scene. Preserve staging while its replay starts. */
            retire_stale_client_surface_exposure( top, 0 );
            return;
        }
        /* The checked native image is complete, but opacity/unredirect and
         * input shape still belong to the GUI connection. Keep the original
         * epoch and deadline until that asynchronous continuation ACKs. */
        top->client_surface_transaction.publication = CLIENT_SURFACE_PUBLICATION_EXPOSURE_READY;
        update_client_surface_publication( top );
        post_message_coalesced( top->handle, WM_WINE_UPDATEWINDOWSTATE,
                                WINE_PUBLISH_CLIENT_SURFACES, 0 );
        return;
    }
    if (!invalidated) top->client_surface_ack_scene = req->scene_generation;
    finish_client_surface_publication( top );
    if (invalidated && is_visible( top ) && has_client_surface( top ))
        restart_client_surface_generation( top );
}

static void get_client_surface_handoff_desc( struct window *win, struct window *top,
                                              struct client_surface_owner *owner,
                                              struct client_surface_ref *surface,
                                              struct client_surface_handoff_desc *desc )
{
    struct client_surface_handoff_state state = client_surface_handoff_get_state( surface->handoff );

    desc->handle = win->handle;
    desc->process = owner->process->id;
    desc->surface = surface->id;
    desc->visible = is_visible( win );
    /* Provisioning is transport work, independent of scene visibility and
     * whether the first completion has published READY yet. Use the server
     * endpoint lifetime, never the writable shared endpoint hints. */
    desc->producer_mapped = state.owner == &top->obj &&
                            (state.mapped & CLIENT_SURFACE_HANDOFF_ENDPOINT_PRODUCER) &&
                            !state.retired && !state.lost;
    /* Shared endpoints alone cannot authorize reuse: A -> B -> A
     * leaves the old owner mapped until its checked reads finish. */
    desc->cookie = state.owner == &top->obj && (state.mapped & CLIENT_SURFACE_HANDOFF_ENDPOINT_CONSUMER) &&
                   !state.retired && !state.lost ? state.cookie : 0;
}

static unsigned int prepare_client_surface_generation( struct window *win, unsigned long long generation,
                                                       int source_required )
{
    struct client_surface_owner *owner;
    struct client_surface_owner *selected_owner;
    struct client_surface_ref *surface, *selected;
    struct window *child;
    unsigned int count = 0;

    if (!win->client_surface_subtree_count) return 0;
    selected = select_client_surface_scene_producer( win, &selected_owner );
    LIST_FOR_EACH_ENTRY( owner, &win->client_surface_owners, struct client_surface_owner, entry )
    {
        LIST_FOR_EACH_ENTRY( surface, &owner->surfaces, struct client_surface_ref, entry )
        {
            surface->generation = 0;
            surface->source_required = 0;
        }
    }
    if (selected && is_visible( win ))
    {
        selected->generation = generation;
        selected->source_required = source_required;
        count = 1;
    }
    LIST_FOR_EACH_ENTRY( child, &win->children, struct window, entry )
        count += prepare_client_surface_generation( child, generation, source_required );
    return count;
}

static int notify_client_surface_geometry_ready_recursive( struct window *win, struct window *top )
{
    struct client_surface_owner *owner, *next;
    struct client_surface_ref *surface;
    struct window *child;
    int removed_surface = 0;

    if (!win->client_surface_subtree_count) return 0;

    LIST_FOR_EACH_ENTRY_SAFE( owner, next, &win->client_surface_owners,
                              struct client_surface_owner, entry )
    {
        /* A renderer can terminate without detaching its process-local
         * surfaces.  Do not retain its process object or stale counts for the
         * lifetime of a foreign HWND. */
        if (!owner->process->running_threads)
        {
            if (!list_empty( &owner->surfaces )) removed_surface = 1;
            discard_client_surface_owner( win, owner, top );
            continue;
        }
        LIST_FOR_EACH_ENTRY( surface, &owner->surfaces, struct client_surface_ref, entry )
        {
            if (surface->generation != client_surface_transaction_generation( top )) continue;
            if (!surface->source_required) continue;
            if (surface->notification_pending) continue;
            if (post_client_surface_notification( surface, top->handle, WM_WINE_UPDATECLIENTSURFACE ))
            {
                surface->notification_pending = 1;
                continue;
            }
            else
            {
                /* A process can own a surface before any of its threads has
                 * created a message queue.  Failed delivery must not leave
                 * the host window unpublished forever. */
                clear_error();
                retire_client_surface_owner_generation( top, owner,
                                                        client_surface_transaction_generation( top ) );
                break;
            }
        }
    }
    LIST_FOR_EACH_ENTRY( child, &win->children, struct window, entry )
        removed_surface |= notify_client_surface_geometry_ready_recursive( child, top );
    if (removed_surface && !has_client_surface( top ))
        finish_client_surface_publication( top );
    return removed_surface;
}

/* Recovery belongs to the selected identity in this generation. Keep that
 * obligation across queue teardown and endpoint replacement; receipt reuse
 * must not retract a notification which was already required. */
static void mark_client_surface_source_recovery( struct window *win, struct window *top,
                                                 const struct client_surface_handoff_receipt *receipts,
                                                 unsigned int receipt_count )
{
    struct client_surface_owner *owner;
    struct client_surface_ref *surface;
    struct window *child;
    unsigned int low = 0, high = receipt_count;

    if (!win->client_surface_subtree_count || !is_visible( win )) return;
    if ((surface = select_client_surface_scene_producer( win, &owner )) &&
        surface->generation == client_surface_transaction_generation( top ))
    {
        while (low < high)
        {
            unsigned int mid = low + (high - low) / 2;
            if (receipts[mid].handle < win->handle) low = mid + 1;
            else high = mid;
        }
        if (low == receipt_count || receipts[low].handle != win->handle) surface->source_required = 1;
    }
    LIST_FOR_EACH_ENTRY( child, &win->children, struct window, entry )
        mark_client_surface_source_recovery( child, top, receipts, receipt_count );
}

static void notify_client_surface_geometry_ready( struct window *top )
{
    mark_client_surface_source_recovery( top, top, NULL, 0 );
    notify_client_surface_geometry_ready_recursive( top, top );
}

DECL_HANDLER(resolve_client_surface_scene_sources)
{
    const struct client_surface_handoff_receipt *receipts = get_req_data();
    unsigned int count = 0, i, receipt_count = get_req_data_size() / sizeof(*receipts);
    struct window *top = get_window( req->handle );

    reply->accepted = 0;
    if (!top) return;
    if (get_toplevel_window( top ) != top || !top->thread || top->thread->process != current->process)
    {
        set_error( STATUS_ACCESS_DENIED );
        return;
    }
    if (get_req_data_size() % sizeof(*receipts))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    for (i = 1; i < receipt_count; ++i)
        if (receipts[i - 1].handle >= receipts[i].handle)
        {
            set_error( STATUS_INVALID_PARAMETER );
            return;
        }
    if ((req->scene_id & 1) || req->scene_id != top->client_surface_scene_generation ||
        req->scene_id != top->client_surface_transaction.epoch ||
        top->client_surface_transaction.phase != CLIENT_SURFACE_PHASE_COMPOSING ||
        !top->client_surface_transaction.source_pending ||
        !validate_client_surface_handoff_generation( top, top, 0, 1, 1, receipts, receipt_count, &count ) ||
        count != receipt_count)
        return;

    /* This consumes one decision, not an assembly completion. Actual checked
     * copies and native publication still need the full scene's receipts. */
    top->client_surface_transaction.source_pending = 0;
    top->client_surface_transaction.owner_repair = 1;
    mark_client_surface_source_recovery( top, top, receipts, receipt_count );
    update_client_surface_publication( top );
    notify_client_surface_geometry_ready_recursive( top, top );
    reply->accepted = 1;
}

static void invalidate_client_surface_owner_repair( struct client_surface_ref *surface )
{
    struct window *top = (struct window *)client_surface_handoff_get_state( surface->handoff ).owner;
    unsigned int error = get_error();

    if (!top || (!top->client_surface_transaction.owner_repair &&
                 !top->client_surface_transaction.source_pending)) return;
    if (client_surface_is_preparing( top ))
    {
        top->client_surface_transaction.owner_repair = 0;
        top->client_surface_transaction.source_pending = 0;
        update_client_surface_publication( top );
        return;
    }
    /* A cache discarded after preparation needs its exact source again.
     * Retain the warm request reason for the other images, so a later loss
     * can request that source too. Do not run the recursive retirement pass
     * from inside another endpoint/lifetime teardown. */
    if (top->client_surface_transaction.phase == CLIENT_SURFACE_PHASE_COMPOSING &&
        surface->generation == client_surface_transaction_generation( top ) &&
        !top->client_surface_scene_change_depth)
    {
        surface->source_required = 1;
        if (!surface->notification_pending &&
            post_client_surface_notification( surface, top->handle, WM_WINE_UPDATECLIENTSURFACE ))
            surface->notification_pending = 1;
    }
    set_error( error );
}

static int client_surface_owner_repair_channels_live( struct window *win, struct window *top )
{
    struct client_surface_owner *owner;
    struct client_surface_ref *surface;
    struct client_surface_handoff_state state;
    struct window *child;

    if (!win->client_surface_subtree_count || !is_visible( win )) return 1;
    if ((surface = select_client_surface_producer( win, &owner )))
    {
        state = client_surface_handoff_get_state( surface->handoff );
        if (state.owner != &top->obj || !(state.mapped & CLIENT_SURFACE_HANDOFF_ENDPOINT_CONSUMER) ||
            state.retired || state.lost) return 0;
    }
    LIST_FOR_EACH_ENTRY( child, &win->children, struct window, entry )
        if (!client_surface_owner_repair_channels_live( child, top )) return 0;
    return 1;
}

/* A queued update or destroy owns the identity until message removal.  Update
 * delivery additionally releases its coalescing bit so a later generation can
 * route work to the renderer's current message pump. */
void client_surface_notification_removed( struct process *process, UINT64 id,
                                          unsigned int message, int delivered )
{
    struct client_surface_ref *surface = find_indexed_client_surface_ref( process, id );

    if (!surface) return;
    if (message == WM_WINE_UPDATECLIENTSURFACE)
    {
        assert( surface->notification_pending );
        surface->notification_pending = 0;
    }
    else
    {
        assert( message == WM_WINE_DESTROYCLIENTSURFACE );
        assert( surface->destroy_state == CLIENT_SURFACE_DESTROY_QUEUED );
        surface->destroy_state = delivered ? CLIENT_SURFACE_DESTROY_NONE :
                                             CLIENT_SURFACE_DESTROY_PENDING;
        if (delivered)
        {
            assert( process->client_surface_destroy_count );
            process->client_surface_destroy_count--;
        }
    }
    free_client_surface_ref_if_unused( surface );
}

/* Window destruction can race a renderer moving or creating its message
 * pump.  Keep an unqueued tombstone and retry the exact destroy identity on
 * the next live process queue instead of leaking its client-side drawable. */
void retry_process_client_surface_destroys( struct process *process )
{
    struct client_surface_ref *surface;
    unsigned int bucket;

    if (!process->client_surface_destroy_count) return;
    for (bucket = 0; bucket < CLIENT_SURFACE_REF_BUCKETS; bucket++)
    {
        for (surface = client_surface_ref_index[bucket]; surface; surface = surface->index_next)
        {
            if (surface->process != process || surface->owner ||
                surface->destroy_state != CLIENT_SURFACE_DESTROY_PENDING)
                continue;
            if (post_client_surface_notification( surface, 0, WM_WINE_DESTROYCLIENTSURFACE ))
            {
                surface->destroy_state = CLIENT_SURFACE_DESTROY_QUEUED;
            }
            else clear_error();
        }
    }
}

/* A process may move its pump to another thread while a notification is
 * queued.  Thread teardown first releases all queue ownership, then this rare
 * O(windows + surfaces) recovery pass re-routes current generation work. */
void retry_process_client_surface_notifications( struct process *process, user_handle_t exclude )
{
    struct client_surface_owner *owner;
    struct client_surface_ref *surface;
    user_handle_t handle = 0;
    struct window *win, *top;

    while ((win = next_user_handle( &handle, NTUSER_OBJ_WINDOW )))
    {
        if (!(owner = get_client_surface_owner( win, process, 0 ))) continue;
        top = get_toplevel_window( win );
        if (top->handle == exclude) continue;
        LIST_FOR_EACH_ENTRY( surface, &owner->surfaces, struct client_surface_ref, entry )
        {
            if (!surface->generation ||
                surface->generation != client_surface_transaction_generation( top ) ||
                !surface->source_required ||
                surface->notification_pending)
                continue;
            if (post_client_surface_notification( surface, top->handle, WM_WINE_UPDATECLIENTSURFACE ))
            {
                surface->notification_pending = 1;
            }
            else clear_error();
        }
    }
}

/* Any scene mutation invalidates the complete image, not only surfaces which
 * have not committed yet.  Reassign one authoritative producer per visible
 * HWND so both live repair and staged publication use a single scene epoch. */
static void restart_client_surface_generation( struct window *top )
{
    /* A real scene mutation or source recovery supersedes a warm repair,
     * including one waiting for the owner to preserve its GDI backing. */
    top->client_surface_transaction.owner_repair = 0;
    top->client_surface_transaction.source_pending = 0;
    restart_client_surface_generation_internal( top );
}

static void restart_client_surface_generation_internal( struct window *top )
{
    int scene_published, prepare_restart;

    /* The actor discards a cache only while retiring its endpoint. Also
     * observe a shared channel loss which has not yet reached that request. */
    if ((top->client_surface_transaction.owner_repair || top->client_surface_transaction.source_pending) &&
        !client_surface_owner_repair_channels_live( top, top ))
    {
        top->client_surface_transaction.owner_repair = 0;
        top->client_surface_transaction.source_pending = 0;
    }

    /* Reparenting transfers composition to the new top-level.  Do not restart
     * an independent scene on a window which is now a child. */
    if (get_toplevel_window( top ) != top)
    {
        top->client_surface_transaction.restart_pending = 0;
        finish_client_surface_publication( top );
        return;
    }

    if (!top->client_surface_transaction.staged &&
        (!is_visible( top ) || !has_client_surface( top )))
    {
        int was_preparing = client_surface_is_preparing( top );

        if (was_preparing) top->client_surface_transaction.phase = CLIENT_SURFACE_PHASE_IDLE;
        top->client_surface_transaction.prepared = 0;
        if (client_surface_is_composing( top )) finish_client_surface_generation( top );
        else if (was_preparing)
        {
            cancel_client_surface_timeout( top );
            top->client_surface_transaction.deadline = 0;
            update_client_surface_publication( top );
        }
        return;
    }
    if (client_surface_is_publishing( top ))
        return;
    if (top->client_surface_transaction.restarting)
    {
        top->client_surface_transaction.restart_pending = 1;
        return;
    }

    scene_published = client_surface_scene_published( top );
    if (!scene_published) top->client_surface_transaction.source_pending = 0;

    /* Preserve the visible non-client and GDI pixels before the owner
     * compositor assembles the next scene. */
    if (scene_published && !top->client_surface_transaction.staged &&
        !top->client_surface_transaction.prepared)
    {
        if (client_surface_is_composing( top ))
        {
            clear_client_surface_subtree_generation( top,
                                                     client_surface_transaction_generation( top ) );
            finish_client_surface_generation( top );
        }
        top->client_surface_transaction.phase = CLIENT_SURFACE_PHASE_PREPARING;
        update_client_surface_publication( top );
        /* Backing activation and preparation share the owner's checked
         * callback. Coalesce with an already queued backing update instead
         * of leaving a PREPARE wake behind its successful COMMIT. A backing
         * callback already in flight gets another wake for this restart. */
        post_message_coalesced( top->handle, WM_WINE_UPDATEWINDOWSTATE,
                                top->client_surface_backing_required ? WINE_UPDATE_CLIENT_SURFACE_BACKING :
                                                                     WINE_PREPARE_CLIENT_SURFACES, 0 );
        arm_client_surface_timeout( top );
        return;
    }
    if (client_surface_is_preparing( top ))
    {
        cancel_client_surface_timeout( top );
        top->client_surface_transaction.deadline = 0;
    }
    top->client_surface_transaction.phase = CLIENT_SURFACE_PHASE_IDLE;
    top->client_surface_transaction.epoch = 0;
    top->client_surface_transaction.prepared = 0;

    top->client_surface_transaction.restarting = 1;
    do
    {
        top->client_surface_transaction.restart_pending = 0;
        invalidate_client_surface_scene( top );
        top->client_surface_transaction.phase = CLIENT_SURFACE_PHASE_COMPOSING;
        top->client_surface_transaction.publication = CLIENT_SURFACE_PUBLICATION_NONE;
        top->client_surface_transaction.epoch = top->client_surface_scene_generation;
        top->client_surface_transaction.pending =
            prepare_client_surface_generation( top,
                                               client_surface_transaction_generation( top ),
                                               !top->client_surface_transaction.owner_repair &&
                                               !top->client_surface_transaction.source_pending );
        update_client_surface_publication( top );

        if (top->client_surface_transaction.pending)
        {
            if (top->client_surface_transaction.source_pending)
                post_message_coalesced( top->handle, WM_WINE_UPDATEWINDOWSTATE,
                                        WINE_RESOLVE_CLIENT_SURFACE_SOURCES, 0 );
            else if (!top->client_surface_transaction.owner_repair) notify_client_surface_geometry_ready( top );
        }
        else
            mark_client_surface_generation_ready( top, 1 );
        /* A nested mutation can request another pass while this loop owns
         * the transition. Only the first pass has the caller's cache proof;
         * every subsequent restart must retain producer recovery. */
        if (top->client_surface_transaction.restart_pending)
        {
            top->client_surface_transaction.owner_repair = 0;
            top->client_surface_transaction.source_pending = 0;
        }
        /* A nested scene change can retire the final producer and therefore
         * finish a live generation before requesting a restart.  The restart
         * request, not the old generation's composing bit, owns this loop. */
    } while (top->client_surface_transaction.restart_pending && top->client_surface_transaction.staged);
    prepare_restart = top->client_surface_transaction.restart_pending && !top->client_surface_transaction.staged;
    top->client_surface_transaction.restarting = 0;

    if (prepare_restart)
    {
        if (client_surface_is_composing( top ))
        {
            clear_client_surface_subtree_generation( top,
                                                     client_surface_transaction_generation( top ) );
            finish_client_surface_generation( top );
        }
        restart_client_surface_generation_internal( top );
        return;
    }

    if (client_surface_is_composing( top )) arm_client_surface_timeout( top );
}

static struct window *get_toplevel_window( struct window *win )
{
    while (win->parent && !is_desktop_window( win->parent )) win = win->parent;
    return win;
}

/* same as is_visible but takes a window handle */
int is_window_visible( user_handle_t window )
{
    struct window *win = get_user_object( window, NTUSER_OBJ_WINDOW );
    if (!win) return 0;
    return is_visible( win );
}

int is_window_transparent( user_handle_t window )
{
    struct window *win = get_user_object( window, NTUSER_OBJ_WINDOW );
    if (!win) return 0;
    return (win->ex_style & (WS_EX_LAYERED|WS_EX_TRANSPARENT)) == (WS_EX_LAYERED|WS_EX_TRANSPARENT);
}

static int is_window_using_parent_dc( struct window *win )
{
    return (win->style & (WS_POPUP|WS_CHILD)) == WS_CHILD && (get_class_style( win->class ) & CS_PARENTDC) != 0;
}

static int is_window_composited( struct window *win )
{
    return (win->ex_style & WS_EX_COMPOSITED) != 0 && !is_window_using_parent_dc(win);
}

static int is_parent_composited( struct window *win )
{
    return win->parent && is_window_composited( win->parent );
}

/* check if point is inside the window, and map to window dpi */
static int is_point_in_window( struct window *win, int *x, int *y, struct ratio dpi )
{
    if (!(win->style & WS_VISIBLE)) return 0; /* not visible */
    if ((win->style & (WS_POPUP|WS_CHILD|WS_DISABLED)) == (WS_CHILD|WS_DISABLED))
        return 0;  /* disabled child */
    if ((win->ex_style & (WS_EX_LAYERED|WS_EX_TRANSPARENT)) == (WS_EX_LAYERED|WS_EX_TRANSPARENT))
        return 0;  /* transparent */
    map_dpi_point( win, x, y, dpi, get_window_dpi( win ) );
    if (!point_in_rect( &win->visible_rect, *x, *y ))
        return 0;  /* not in window */
    if (win->win_region &&
        !point_in_region( win->win_region, *x - win->window_rect.left, *y - win->window_rect.top ))
        return 0;  /* not in window region */
    return 1;
}

/* helper for get_window_list */
static void append_window_to_list( struct window *win, struct thread *thread, atom_t atom,
                                   user_handle_t *handles, unsigned int *count, unsigned int max_count )
{
    if (thread && win->thread != thread) return;
    if (atom && get_class_atom( win->class ) != atom) return;
    if (*count < max_count) handles[*count] = win->handle;
    (*count)++;
}

/* fill an array with the handles of siblings or children */
static void get_window_list( struct desktop *desktop, struct window *win, struct thread *thread,
                             int children, user_handle_t *handles,
                             unsigned int *count, unsigned int max_count )
{
    struct window *child;

    if (desktop)  /* top-level windows of specified desktop */
    {
        if (children) return;
        if (!desktop->top_window) return;
        LIST_FOR_EACH_ENTRY( child, &desktop->top_window->children, struct window, entry )
            append_window_to_list( child, thread, 0, handles, count, max_count );
    }
    else if (!win)  /* top-level windows of current desktop */
    {
        if (!(win = get_desktop_window( current ))) return;
        LIST_FOR_EACH_ENTRY( child, &win->children, struct window, entry )
            append_window_to_list( child, thread, 0, handles, count, max_count );
    }
    else if (children)  /* children (recursively) of specified window */
    {
        LIST_FOR_EACH_ENTRY( child, &win->children, struct window, entry )
        {
            append_window_to_list( child, thread, 0, handles, count, max_count );
            get_window_list( NULL, child, thread, TRUE, handles, count, max_count );
        }
    }
    else if (!is_desktop_window( win ))  /* siblings starting from specified window */
    {
        for (child = win; child; child = get_next_window( child ))
            append_window_to_list( child, thread, 0, handles, count, max_count );
    }
    else  /* desktop window siblings */
    {
        append_window_to_list( win, thread, 0, handles, count, max_count );
        if (win == win->desktop->top_window && win->desktop->msg_window)
            append_window_to_list( win->desktop->msg_window, thread, 0, handles, count, max_count );
    }
}

/* find child of 'parent' that contains the given point (in parent-relative coords) */
static struct window *child_window_from_point( struct window *parent, int x, int y )
{
    struct window *ptr;

    LIST_FOR_EACH_ENTRY( ptr, &parent->children, struct window, entry )
    {
        int x_child = x, y_child = y;

        if (!is_point_in_window( ptr, &x_child, &y_child, get_window_dpi( parent ) )) continue;  /* skip it */

        /* if window is minimized or disabled, return at once */
        if (ptr->style & (WS_MINIMIZE|WS_DISABLED)) return ptr;

        /* if point is not in client area, return at once */
        if (!point_in_rect( &ptr->client_rect, x_child, y_child )) return ptr;

        return child_window_from_point( ptr, x_child - ptr->client_rect.left,
                                        y_child - ptr->client_rect.top );
    }
    return parent;  /* not found any child */
}

/* find all children of 'parent' that contain the given point */
static int get_window_children_from_point( struct window *parent, int x, int y,
                                           struct user_handle_array *array )
{
    struct window *ptr;

    LIST_FOR_EACH_ENTRY( ptr, &parent->children, struct window, entry )
    {
        int x_child = x, y_child = y;

        if (!is_point_in_window( ptr, &x_child, &y_child, get_window_dpi( parent ) )) continue;  /* skip it */

        /* if point is in client area, and window is not minimized or disabled, check children */
        if (!(ptr->style & (WS_MINIMIZE|WS_DISABLED)) && point_in_rect( &ptr->client_rect, x_child, y_child ))
        {
            if (!get_window_children_from_point( ptr, x_child - ptr->client_rect.left,
                                                 y_child - ptr->client_rect.top, array ))
                return 0;
        }

        /* now add window to the array */
        if (!add_handle_to_array( array, ptr->handle )) return 0;
    }
    return 1;
}

/* get handle of root of top-most window containing point (in absolute raw coords) */
user_handle_t shallow_window_from_point( struct desktop *desktop, int x, int y )
{
    struct window *ptr;

    if (!desktop->top_window) return 0;

    map_point_raw_to_virt( desktop, &x, &y );

    LIST_FOR_EACH_ENTRY( ptr, &desktop->top_window->children, struct window, entry )
    {
        int x_child = x, y_child = y;

        if (!is_point_in_window( ptr, &x_child, &y_child, no_dpi )) continue;  /* skip it */
        return ptr->handle;
    }
    return desktop->top_window->handle;
}

/* return thread of top-most window containing point (in absolute raw coords) */
struct thread *window_thread_from_point( user_handle_t scope, int x, int y )
{
    struct window *win = get_user_object( scope, NTUSER_OBJ_WINDOW );

    if (!win) return NULL;

    map_point_raw_to_virt( win->desktop, &x, &y );

    screen_to_client( win, &x, &y, no_dpi );
    win = child_window_from_point( win, x, y );
    if (!win->thread) return NULL;
    return (struct thread *)grab_object( win->thread );
}

/* return list of all windows containing point (in absolute coords) */
static int all_windows_from_point( struct window *top, int x, int y, struct ratio dpi,
                                   struct user_handle_array *array )
{
    if (!is_desktop_window( top ) && !is_desktop_window( top->parent ))
    {
        screen_to_client( top->parent, &x, &y, dpi );
        dpi = get_window_dpi( top->parent );
    }

    if (!is_point_in_window( top, &x, &y, dpi )) return 1;
    /* if point is in client area, and window is not minimized or disabled, check children */
    if (!(top->style & (WS_MINIMIZE|WS_DISABLED)) && point_in_rect( &top->client_rect, x, y ))
    {
        if (!is_desktop_window(top))
        {
            x -= top->client_rect.left;
            y -= top->client_rect.top;
        }
        if (!get_window_children_from_point( top, x, y, array )) return 0;
    }
    /* now add window to the array */
    if (!add_handle_to_array( array, top->handle )) return 0;
    return 1;
}


/* return the thread owning a window */
struct thread *get_window_thread( user_handle_t handle )
{
    struct window *win = get_user_object( handle, NTUSER_OBJ_WINDOW );
    if (!win || !win->thread) return NULL;
    return (struct thread *)grab_object( win->thread );
}


/* check if any area of a window needs repainting */
static inline int win_needs_repaint( struct window *win )
{
    return win->update_region || (win->paint_flags & PAINT_INTERNAL);
}


/* find a child of the specified window that needs repainting */
static struct window *find_child_to_repaint( struct window *parent, struct thread *thread )
{
    struct window *ptr, *ret = NULL;

    LIST_FOR_EACH_ENTRY( ptr, &parent->children, struct window, entry )
    {
        if (!(ptr->style & WS_VISIBLE)) continue;
        if (ptr->thread == thread && win_needs_repaint( ptr ))
            ret = ptr;
        else if (!(ptr->style & WS_MINIMIZE)) /* explore its children */
            ret = find_child_to_repaint( ptr, thread );
        if (ret) break;
    }

    if (ret && (ret->ex_style & WS_EX_TRANSPARENT))
    {
        /* transparent window, check for non-transparent sibling to paint first */
        for (ptr = get_next_window(ret); ptr; ptr = get_next_window(ptr))
        {
            if (!(ptr->style & WS_VISIBLE)) continue;
            if (ptr->ex_style & WS_EX_TRANSPARENT) continue;
            if (ptr->thread != thread) continue;
            if (win_needs_repaint( ptr )) return ptr;
        }
    }
    return ret;
}


/* find a window that needs to receive a WM_PAINT; also clear its internal paint flag */
user_handle_t find_window_to_repaint( user_handle_t parent, struct thread *thread )
{
    struct window *ptr, *win, *top_window = get_desktop_window( thread );

    if (!top_window) return 0;

    if (top_window->thread == thread && win_needs_repaint( top_window )) win = top_window;
    else win = find_child_to_repaint( top_window, thread );

    if (win && parent)
    {
        /* check that it is a child of the specified parent */
        for (ptr = win; ptr; ptr = ptr->parent)
            if (ptr->handle == parent) break;
        /* otherwise don't return any window, we don't repaint a child before its parent */
        if (!ptr) win = NULL;
    }
    if (!win) return 0;
    win->paint_flags &= ~PAINT_INTERNAL;
    return win->handle;
}


/* intersect the window region with the specified region, relative to the window parent */
static struct region *intersect_window_region( struct region *region, struct window *win )
{
    /* make region relative to window rect */
    offset_region( region, -win->window_rect.left, -win->window_rect.top );
    if (!intersect_region( region, region, win->win_region )) return NULL;
    /* make region relative to parent again */
    offset_region( region, win->window_rect.left, win->window_rect.top );
    return region;
}


/* convert coordinates from client to screen coords */
static inline void client_to_screen_rect( struct window *win, struct rectangle *rect )
{
    for ( ; win && !is_desktop_window(win); win = win->parent)
        offset_rect( rect, win->client_rect.left, win->client_rect.top );
}

/* map the region from window to screen coordinates */
static inline void map_win_region_to_screen( struct window *win, struct region *region )
{
    if (!is_desktop_window(win))
    {
        int x = win->window_rect.left;
        int y = win->window_rect.top;
        client_to_screen( win->parent, &x, &y );
        offset_region( region, x, y );
    }
}


/* clip all children of a given window out of the visible region */
static struct region *clip_children( struct window *parent, struct window *last,
                                     struct region *region, int offset_x, int offset_y )
{
    struct window *ptr;
    struct region *tmp = create_empty_region();

    if (!tmp) return NULL;
    LIST_FOR_EACH_ENTRY( ptr, &parent->children, struct window, entry )
    {
        if (ptr == last) break;
        if (!(ptr->style & WS_VISIBLE)) continue;
        if (ptr->ex_style & WS_EX_TRANSPARENT) continue;
        set_region_rect( tmp, &ptr->visible_rect );
        if (ptr->win_region && !intersect_window_region( tmp, ptr ))
        {
            free_region( tmp );
            return NULL;
        }
        offset_region( tmp, offset_x, offset_y );
        if (!(region = subtract_region( region, region, tmp ))) break;
        if (is_region_empty( region )) break;
    }
    free_region( tmp );
    return region;
}


/* set the region to the client rect clipped by the window rect, in parent-relative coordinates */
static void set_region_client_rect( struct region *region, struct window *win )
{
    struct rectangle rect;

    intersect_rect( &rect, &win->window_rect, &win->client_rect );
    intersect_rect( &rect, &rect, &win->surface_rect );
    set_region_rect( region, &rect );
}


/* set the region to the visible rect clipped by the window surface, in parent-relative coordinates */
static void set_region_visible_rect( struct region *region, struct window *win )
{
    struct rectangle rect;

    intersect_rect( &rect, &win->visible_rect, &win->surface_rect );
    set_region_rect( region, &rect );
}


/* get the top-level window to clip against for a given window */
static inline struct window *get_top_clipping_window( struct window *win )
{
    while (!(win->paint_flags & PAINT_HAS_SURFACE) && win->parent && !is_desktop_window(win->parent))
        win = win->parent;
    return win;
}


/* compute the visible region of a window, in window coordinates */
static struct region *get_visible_region_ex( struct window *win, unsigned int flags,
                                             int clip_siblings )
{
    struct region *tmp = NULL, *region;
    int offset_x, offset_y;

    if (!(region = create_empty_region())) return NULL;

    /* first check if all ancestors are visible */

    if (!is_visible( win )) return region;  /* empty region */

    if (is_desktop_window( win ))
    {
        set_region_rect( region, &win->window_rect );
        return region;
    }

    /* create a region relative to the window itself */

    if ((flags & DCX_PARENTCLIP) && !is_desktop_window( win->parent ))
    {
        set_region_client_rect( region, win->parent );
        offset_region( region, -win->parent->client_rect.left, -win->parent->client_rect.top );
    }
    else if (flags & DCX_WINDOW)
    {
        set_region_visible_rect( region, win );
        if (win->win_region && !intersect_window_region( region, win )) goto error;
    }
    else
    {
        set_region_client_rect( region, win );
        if (win->win_region && !intersect_window_region( region, win )) goto error;
    }

    /* clip children */

    if (flags & DCX_CLIPCHILDREN)
    {
        if (!clip_children( win, NULL, region, win->client_rect.left, win->client_rect.top )) goto error;
    }

    /* clip siblings of ancestors */

    offset_x = win->window_rect.left;
    offset_y = win->window_rect.top;

    if ((tmp = create_empty_region()) != NULL)
    {
        while (!is_desktop_window( win->parent ))
        {
            /* we don't clip out top-level siblings as that's up to the native windowing system */
            if (clip_siblings && (win->style & WS_CLIPSIBLINGS))
            {
                if (!clip_children( win->parent, win, region, 0, 0 )) goto error;
                if (is_region_empty( region )) break;
            }
            /* clip to parent client area */
            win = win->parent;
            offset_x += win->client_rect.left;
            offset_y += win->client_rect.top;
            offset_region( region, win->client_rect.left, win->client_rect.top );
            set_region_client_rect( tmp, win );
            if (win->win_region && !intersect_window_region( tmp, win )) goto error;
            if (!intersect_region( region, region, tmp )) goto error;
            if (is_region_empty( region )) break;
        }
        free_region( tmp );
    }
    offset_region( region, -offset_x, -offset_y );  /* make it relative to target window */
    return region;

error:
    if (tmp) free_region( tmp );
    free_region( region );
    return NULL;
}

static struct region *get_visible_region( struct window *win, unsigned int flags )
{
    return get_visible_region_ex( win, flags, 1 );
}


/* clip all children with a custom pixel format out of the visible region */
static struct region *clip_pixel_format_children( struct window *parent, struct region *parent_clip,
                                                  struct region *region, int offset_x, int offset_y )
{
    struct window *ptr;
    struct region *clip = create_empty_region();

    if (!clip) return NULL;

    LIST_FOR_EACH_ENTRY_REV( ptr, &parent->children, struct window, entry )
    {
        if (!(ptr->style & WS_VISIBLE)) continue;
        if (ptr->ex_style & WS_EX_TRANSPARENT) continue;

        /* add the visible rect */
        set_region_rect( clip, &ptr->visible_rect );
        if (ptr->win_region && !intersect_window_region( clip, ptr )) break;
        offset_region( clip, offset_x, offset_y );
        if (!intersect_region( clip, clip, parent_clip )) break;
        if (!union_region( region, region, clip )) break;
        if (!(ptr->paint_flags & (PAINT_HAS_PIXEL_FORMAT | PAINT_PIXEL_FORMAT_CHILD))) continue;

        /* subtract the client rect if it uses a custom pixel format */
        set_region_rect( clip, &ptr->client_rect );
        if (ptr->win_region && !intersect_window_region( clip, ptr )) break;
        offset_region( clip, offset_x, offset_y );
        if (!intersect_region( clip, clip, parent_clip )) break;
        if ((ptr->paint_flags & PAINT_HAS_PIXEL_FORMAT) && !subtract_region( region, region, clip ))
            break;

        if (!clip_pixel_format_children( ptr, clip, region, offset_x + ptr->client_rect.left,
                                         offset_y + ptr->client_rect.top ))
            break;
    }
    free_region( clip );
    return region;
}


/* compute the visible surface region of a window, in parent coordinates */
static struct region *get_surface_region( struct window *win )
{
    struct region *region, *clip;
    int offset_x, offset_y;

    /* create a region relative to the window itself */

    if (!(region = create_empty_region())) return NULL;
    if (!(clip = create_empty_region())) goto error;
    set_region_rect( region, &win->visible_rect );
    if (win->win_region && !intersect_window_region( region, win )) goto error;
    set_region_rect( clip, &win->client_rect );
    if (win->win_region && !intersect_window_region( clip, win )) goto error;

    if ((win->paint_flags & PAINT_HAS_PIXEL_FORMAT) && !subtract_region( region, region, clip ))
        goto error;

    /* clip children */

    if (!is_desktop_window(win))
    {
        offset_x = win->client_rect.left;
        offset_y = win->client_rect.top;
    }
    else offset_x = offset_y = 0;

    if (!clip_pixel_format_children( win, clip, region, offset_x, offset_y )) goto error;

    free_region( clip );
    return region;

error:
    if (clip) free_region( clip );
    free_region( region );
    return NULL;
}


/* get the window class of a window */
struct window_class* get_window_class( user_handle_t window )
{
    struct window *win;
    if (!(win = get_window( window ))) return NULL;
    if (!win->class) set_error( STATUS_ACCESS_DENIED );
    return win->class;
}

/* determine the window visible rectangle, i.e. window or client rect cropped by parent rects */
/* the returned rectangle is in window coordinates; return 0 if rectangle is empty */
static int get_window_visible_rect( struct window *win, struct rectangle *rect, int frame )
{
    int offset_x = win->window_rect.left, offset_y = win->window_rect.top;

    *rect = frame ? win->window_rect : win->client_rect;

    if (!(win->style & WS_VISIBLE)) return 0;
    if (is_desktop_window( win )) return 1;

    while (!is_desktop_window( win->parent ))
    {
        win = win->parent;
        if (!(win->style & WS_VISIBLE) || win->style & WS_MINIMIZE) return 0;
        offset_x += win->client_rect.left;
        offset_y += win->client_rect.top;
        offset_rect( rect, win->client_rect.left, win->client_rect.top );
        if (!intersect_rect( rect, rect, &win->client_rect )) return 0;
        if (!intersect_rect( rect, rect, &win->window_rect )) return 0;
    }
    offset_rect( rect, -offset_x, -offset_y );
    return 1;
}

/* return a copy of the specified region cropped to the window client or frame rectangle, */
/* and converted from client to window coordinates. Helper for (in)validate_window. */
static struct region *crop_region_to_win_rect( struct window *win, struct region *region, int frame )
{
    struct rectangle rect;
    struct region *tmp;

    if (!get_window_visible_rect( win, &rect, frame )) return NULL;
    if (!(tmp = create_empty_region())) return NULL;
    set_region_rect( tmp, &rect );

    if (region)
    {
        /* map it to client coords */
        offset_region( tmp, win->window_rect.left - win->client_rect.left,
                       win->window_rect.top - win->client_rect.top );

        /* intersect specified region with bounding rect */
        if (!intersect_region( tmp, region, tmp )) goto done;
        if (is_region_empty( tmp )) goto done;

        /* map it back to window coords */
        offset_region( tmp, win->client_rect.left - win->window_rect.left,
                       win->client_rect.top - win->window_rect.top );
    }
    return tmp;

done:
    free_region( tmp );
    return NULL;
}


static void invalidate_window_paint( struct window *win )
{
    struct window *top = get_toplevel_window( win );

    if (top->paint_serial == ~(UINT64)0)
    {
        win->paint_failed = 1;
        return;
    }
    ++top->paint_serial;
    SHARED_WRITE_BEGIN( top->shared, window_shm_t )
    {
        shared->client_surface_paint_serial = top->paint_serial;
    }
    SHARED_WRITE_END;
    /* Paint is a content change. PREPARING checkpoints compare its own serial;
     * only an already running publication needs the existing scene restart. */
    if (!has_client_surface( top ) || !client_surface_is_composing( top ) ||
        top->client_surface_scene_change_depth) return;
    if (client_surface_is_publishing( top ))
    {
        if (top->client_surface_transaction.epoch == top->client_surface_scene_generation)
            invalidate_client_surface_scene( top );
    }
    else
    {
        begin_client_surface_cached_scene_change( top );
        end_client_surface_scene_change( top );
    }
}

/* set a region as new update region for the window */
static void set_update_region( struct window *win, struct region *region )
{
    if (region && !is_region_empty( region ))
    {
        if (!win->update_region) inc_window_paint_count( win, 1 );
        else free_region( win->update_region );
        win->update_region = region;
        invalidate_window_paint( win );
    }
    else
    {
        if (win->update_region)
        {
            inc_window_paint_count( win, -1 );
            free_region( win->update_region );
        }
        win->paint_flags &= ~(PAINT_ERASE | PAINT_DELAYED_ERASE | PAINT_NONCLIENT);
        win->update_region = NULL;
        if (region) free_region( region );
        wake_window_paint_prepare( get_toplevel_window( win ) );
    }
}


/* add a region to the update region; the passed region is freed or reused */
static int add_update_region( struct window *win, struct region *region )
{
    if (win->update_region && !union_region( region, win->update_region, region ))
    {
        free_region( region );
        return 0;
    }
    set_update_region( win, region );
    return 1;
}


/* crop the update region of children to the specified rectangle, in client coords */
static void crop_children_update_region( struct window *win, struct rectangle *rect )
{
    struct window *child;
    struct region *tmp;
    struct rectangle child_rect;

    LIST_FOR_EACH_ENTRY( child, &win->children, struct window, entry )
    {
        if (!(child->style & WS_VISIBLE)) continue;
        if (!rect)  /* crop everything out */
        {
            crop_children_update_region( child, NULL );
            set_update_region( child, NULL );
            continue;
        }

        /* nothing to do if child is completely inside rect */
        if (child->window_rect.left >= rect->left &&
            child->window_rect.top >= rect->top &&
            child->window_rect.right <= rect->right &&
            child->window_rect.bottom <= rect->bottom) continue;

        /* map to child client coords and crop grand-children */
        child_rect = *rect;
        offset_rect( &child_rect, -child->client_rect.left, -child->client_rect.top );
        crop_children_update_region( child, &child_rect );

        /* now crop the child itself */
        if (!child->update_region) continue;
        if (!(tmp = create_empty_region())) continue;
        set_region_rect( tmp, rect );
        offset_region( tmp, -child->window_rect.left, -child->window_rect.top );
        if (intersect_region( tmp, child->update_region, tmp )) set_update_region( child, tmp );
        else free_region( tmp );
    }
}


/* validate the non client area of a window */
static void validate_non_client( struct window *win )
{
    struct region *tmp;
    struct rectangle rect;

    if (!win->update_region) return;  /* nothing to do */

    /* get client rect in window coords */
    rect.left   = win->client_rect.left - win->window_rect.left;
    rect.top    = win->client_rect.top - win->window_rect.top;
    rect.right  = win->client_rect.right - win->window_rect.left;
    rect.bottom = win->client_rect.bottom - win->window_rect.top;

    if ((tmp = create_empty_region()))
    {
        set_region_rect( tmp, &rect );
        if (intersect_region( tmp, win->update_region, tmp ))
            set_update_region( win, tmp );
        else
            free_region( tmp );
    }
    win->paint_flags &= ~PAINT_NONCLIENT;
}


/* validate a window completely so that we don't get any further paint messages for it */
static void validate_whole_window( struct window *win )
{
    set_update_region( win, NULL );

    if (win->paint_flags & PAINT_INTERNAL)
    {
        win->paint_flags &= ~PAINT_INTERNAL;
        inc_window_paint_count( win, -1 );
    }
    wake_window_paint_prepare( get_toplevel_window( win ) );
}


/* validate a window's children so that we don't get any further paint messages for it */
static void validate_children( struct window *win )
{
    struct window *child;

    LIST_FOR_EACH_ENTRY( child, &win->children, struct window, entry )
    {
        if (!(child->style & WS_VISIBLE)) continue;
        validate_children(child);
        validate_whole_window(child);
    }
}


/* validate the update region of a window on all parents; helper for get_update_region */
static void validate_parents( struct window *child )
{
    int offset_x = 0, offset_y = 0;
    struct window *win = child;
    struct region *tmp = NULL;

    if (!child->update_region) return;

    while (win->parent)
    {
        /* map to parent client coords */
        offset_x += win->window_rect.left;
        offset_y += win->window_rect.top;

        win = win->parent;

        /* and now map to window coords */
        offset_x += win->client_rect.left - win->window_rect.left;
        offset_y += win->client_rect.top - win->window_rect.top;

        if (win->update_region && !(win->style & WS_CLIPCHILDREN))
        {
            if (!tmp && !(tmp = create_empty_region())) return;
            offset_region( child->update_region, offset_x, offset_y );
            if (subtract_region( tmp, win->update_region, child->update_region ))
            {
                set_update_region( win, tmp );
                tmp = NULL;
            }
            /* restore child coords */
            offset_region( child->update_region, -offset_x, -offset_y );
        }
    }
    if (tmp) free_region( tmp );
}


/* add/subtract a region (in client coordinates) to the update region of the window */
static void redraw_window( struct window *win, struct region *region, unsigned int flags, int nested )
{
    struct region *child_rgn, *tmp;
    struct window *child;

    if (flags & RDW_INVALIDATE)
    {
        const int frame = !!(flags & RDW_FRAME);
        if (!(tmp = crop_region_to_win_rect( win, region, frame ))) return;

        if (!add_update_region( win, tmp )) return;

        if (flags & RDW_FRAME) win->paint_flags |= PAINT_NONCLIENT;
        if (flags & RDW_ERASE) win->paint_flags |= PAINT_ERASE;
    }
    else if (flags & RDW_VALIDATE)
    {
        if (!region && (flags & RDW_NOFRAME))  /* shortcut: validate everything */
        {
            set_update_region( win, NULL );
        }
        else if (win->update_region)
        {
            const int frame = nested;  /* validating nested child; include frame */
            if ((tmp = crop_region_to_win_rect( win, region, frame )))
            {
                if (!subtract_region( tmp, win->update_region, tmp ))
                {
                    free_region( tmp );
                    return;
                }
                set_update_region( win, tmp );
            }
            if (flags & RDW_NOFRAME) validate_non_client( win );
            if (flags & RDW_NOERASE) win->paint_flags &= ~(PAINT_ERASE | PAINT_DELAYED_ERASE);
        }
    }

    if ((flags & RDW_INTERNALPAINT) && !(win->paint_flags & PAINT_INTERNAL))
    {
        win->paint_flags |= PAINT_INTERNAL;
        inc_window_paint_count( win, 1 );
        invalidate_window_paint( win );
    }
    else if ((flags & RDW_NOINTERNALPAINT) && (win->paint_flags & PAINT_INTERNAL))
    {
        win->paint_flags &= ~PAINT_INTERNAL;
        inc_window_paint_count( win, -1 );
        wake_window_paint_prepare( get_toplevel_window( win ) );
    }

    /* now process children recursively */

    if (flags & RDW_NOCHILDREN) return;
    if (win->style & WS_MINIMIZE) return;
    if ((win->style & WS_CLIPCHILDREN) && !(flags & RDW_ALLCHILDREN)) return;

    if (!(tmp = crop_region_to_win_rect( win, region, 0 ))) return;

    /* map to client coordinates */
    offset_region( tmp, win->window_rect.left - win->client_rect.left,
                   win->window_rect.top - win->client_rect.top );

    if (flags & RDW_INVALIDATE) flags |= RDW_FRAME | RDW_ERASE;

    LIST_FOR_EACH_ENTRY( child, &win->children, struct window, entry )
    {
        if (!(child->style & WS_VISIBLE)) continue;
        if (!(child_rgn = create_empty_region())) continue;
        if (copy_region( child_rgn, tmp ))
        {
            map_dpi_region( child, child_rgn, get_window_dpi( win ), get_window_dpi( child ) );
            if (rect_in_region( child_rgn, &child->window_rect ))
            {
                offset_region( child_rgn, -child->client_rect.left, -child->client_rect.top );
                redraw_window( child, child_rgn, flags, 1 );
            }
        }
        free_region( child_rgn );
    }

    free_region( tmp );
}


/* retrieve the update flags for a window depending on the state of the update region */
static unsigned int get_update_flags( struct window *win, unsigned int flags )
{
    unsigned int ret = 0;

    if (flags & UPDATE_NONCLIENT)
    {
        if ((win->paint_flags & PAINT_NONCLIENT) && win->update_region) ret |= UPDATE_NONCLIENT;
    }
    if (flags & UPDATE_ERASE)
    {
        if ((win->paint_flags & PAINT_ERASE) && win->update_region) ret |= UPDATE_ERASE;
    }
    if (flags & UPDATE_PAINT)
    {
        if (win->update_region)
        {
            if (win->paint_flags & PAINT_DELAYED_ERASE) ret |= UPDATE_DELAYED_ERASE;
            ret |= UPDATE_PAINT;
        }
    }
    if (flags & UPDATE_INTERNALPAINT)
    {
        if (win->paint_flags & PAINT_INTERNAL)
        {
            ret |= UPDATE_INTERNALPAINT;
            if (win->paint_flags & PAINT_DELAYED_ERASE) ret |= UPDATE_DELAYED_ERASE;
        }
    }
    return ret;
}


/* iterate through the children of the given window until we find one with some update flags */
static unsigned int get_child_update_flags( struct window *win, struct window *from_child,
                                            unsigned int flags, struct window **child )
{
    struct window *ptr;
    unsigned int ret = 0;

    /* first make sure we want to iterate children at all */

    if (win->style & WS_MINIMIZE) return 0;

    /* note: the WS_CLIPCHILDREN test is the opposite of the invalidation case,
     * here we only want to repaint children of windows that clip them, others
     * need to wait for WM_PAINT to be done in the parent first.
     */
    if (!(flags & UPDATE_ALLCHILDREN) && !(win->style & WS_CLIPCHILDREN)) return 0;

    LIST_FOR_EACH_ENTRY( ptr, &win->children, struct window, entry )
    {
        if (from_child)  /* skip all children until from_child is found */
        {
            if (ptr == from_child) from_child = NULL;
            continue;
        }
        if (!(ptr->style & WS_VISIBLE)) continue;
        if ((ret = get_update_flags( ptr, flags )) != 0)
        {
            *child = ptr;
            break;
        }
        if ((ret = get_child_update_flags( ptr, NULL, flags, child ))) break;
    }
    return ret;
}

/* iterate through children and siblings of the given window until we find one with some update flags */
static unsigned int get_window_update_flags( struct window *win, struct window *from_child,
                                             unsigned int flags, struct window **child )
{
    unsigned int ret;
    struct window *ptr, *from_sibling = NULL;

    /* if some parent is not visible start from the next sibling */

    if (!is_visible( win )) return 0;
    for (ptr = from_child; ptr; ptr = ptr->parent)
    {
        if (!(ptr->style & WS_VISIBLE) || (ptr->style & WS_MINIMIZE)) from_sibling = ptr;
        if (ptr == win) break;
    }

    /* non-client painting must be delayed if one of the parents is going to
     * be repainted and doesn't clip children */

    if ((flags & UPDATE_NONCLIENT) && !(flags & (UPDATE_PAINT|UPDATE_INTERNALPAINT)))
    {
        for (ptr = win->parent; ptr; ptr = ptr->parent)
        {
            if (!(ptr->style & WS_CLIPCHILDREN) && win_needs_repaint( ptr ))
                return 0;
        }
        if (from_child && !(flags & UPDATE_ALLCHILDREN))
        {
            for (ptr = from_sibling ? from_sibling : from_child; ptr; ptr = ptr->parent)
            {
                if (!(ptr->style & WS_CLIPCHILDREN) && win_needs_repaint( ptr )) from_sibling = ptr;
                if (ptr == win) break;
            }
        }
    }


    /* check window itself (only if not restarting from a child) */

    if (!from_child)
    {
        if ((ret = get_update_flags( win, flags )))
        {
            *child = win;
            return ret;
        }
        from_child = win;
    }

    /* now check children */

    if (flags & UPDATE_NOCHILDREN) return 0;
    if (!from_sibling)
    {
        if ((ret = get_child_update_flags( from_child, NULL, flags, child ))) return ret;
        from_sibling = from_child;
    }

    /* then check siblings and parent siblings */

    while (from_sibling->parent && from_sibling != win)
    {
        if ((ret = get_child_update_flags( from_sibling->parent, from_sibling, flags, child )))
            return ret;
        from_sibling = from_sibling->parent;
    }
    return 0;
}


struct window_paint
{
    struct list entry, thread_entry;
    struct thread *thread;
    struct window *window, *top;
    struct region *region; /* window-coordinate update actually consumed by paint validation */
    struct rectangle window_rect, client_rect;
    struct ratio dpi;
    UINT64 token, scene;
    int ended, failed;
};

static struct list window_paints = LIST_INIT( window_paints );
static UINT64 window_paint_token;
static unsigned int window_paint_count;

#define WINDOW_PAINT_THREAD_LIMIT 64
#define WINDOW_PAINT_LIMIT 4096

static int window_paint_failed( struct window *win )
{
    struct window *child;

    if (win->paint_failed) return 1;
    LIST_FOR_EACH_ENTRY( child, &win->children, struct window, entry )
        if (is_visible( child ) && window_paint_failed( child )) return 1;
    return 0;
}

static int window_paint_pending( struct window *top )
{
    struct window_paint *paint;
    struct window *child;

    if (window_paint_failed( top )) return 1;
    if (get_window_update_flags( top, NULL, UPDATE_PAINT | UPDATE_INTERNALPAINT |
                                 UPDATE_NONCLIENT | UPDATE_ERASE | UPDATE_ALLCHILDREN, &child ))
        return 1;
    LIST_FOR_EACH_ENTRY( paint, &window_paints, struct window_paint, entry )
        if (paint->top == top || get_toplevel_window( paint->window ) == top) return 1;
    return 0;
}

static void wake_window_paint_prepare( struct window *top )
{
    if (!top->handle || !top->thread || !top->paint_waiting) return;
    if (!window_paint_failed( top ) && window_paint_pending( top )) return;
    post_message_coalesced( top->handle, WM_WINE_UPDATEWINDOWSTATE,
                           client_surface_is_preparing( top ) ? WINE_PREPARE_CLIENT_SURFACES :
                           WINE_UPDATE_CLIENT_SURFACE_HANDOFFS, 0 );
}

static void release_window_paint( struct window_paint *paint, int failed )
{
    struct window *top = get_toplevel_window( paint->window );
    struct ratio dpi = get_window_dpi( paint->window );

    list_remove( &paint->entry );
    list_remove( &paint->thread_entry );
    --paint->thread->window_paint_count;
    --window_paint_count;
    /* Failed paints restore the consumed region to ordinary WM_PAINT. This
     * is error recovery, not an extra paint used to manufacture a checkpoint. */
    if (failed && paint->window->handle && has_client_surface( top ))
    {
        int restored = 0;

        /* A saved update region is not transformed along with subsequent
         * frame, DPI, or parent changes. Repaint the current window instead. */
        if (paint->region && top == paint->top && paint->scene == top->client_surface_scene_generation &&
            is_rect_equal( &paint->window_rect, &paint->window->window_rect ) &&
            is_rect_equal( &paint->client_rect, &paint->window->client_rect ) &&
            dpi.num == paint->dpi.num && dpi.den == paint->dpi.den)
        {
            struct region *region = paint->region;
            paint->region = NULL;
            restored = add_update_region( paint->window, region );
        }
        else
        {
            redraw_window( paint->window, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME, 0 );
            restored = !get_error() && !!paint->window->update_region;
        }
        if (restored) paint->window->paint_flags |= PAINT_ERASE;
        /* Never clear a different writer's untracked/admission failure. */
        if (!restored) paint->window->paint_failed = 1;
    }
    if (paint->region) free_region( paint->region );
    wake_window_paint_prepare( top );
    if (top != paint->top) wake_window_paint_prepare( paint->top );
    release_object( paint->window );
    release_object( paint->top );
    release_object( paint->thread );
    free( paint );
}

static void cancel_thread_window_paints( struct thread *thread )
{
    struct window_paint *paint, *next;

    LIST_FOR_EACH_ENTRY_SAFE( paint, next, &thread->window_paints, struct window_paint, thread_entry )
        release_window_paint( paint, 1 );
}

static void cancel_window_paints( struct window *win )
{
    struct window_paint *paint, *next;

    LIST_FOR_EACH_ENTRY_SAFE( paint, next, &window_paints, struct window_paint, entry )
        if (paint->window == win) release_window_paint( paint, 0 );
}

static struct window_paint *get_window_paint( UINT64 token )
{
    struct window_paint *paint;

    LIST_FOR_EACH_ENTRY( paint, &current->window_paints, struct window_paint, thread_entry )
        if (paint->token == token)
            return paint;
    set_error( STATUS_INVALID_PARAMETER );
    return NULL;
}

DECL_HANDLER(begin_window_paint)
{
    struct window *win = get_window( req->handle ), *top;
    struct window_paint *paint;

    if (!win) return;
    top = get_toplevel_window( win );
    invalidate_window_paint( win );
    if (!req->tracked || window_paint_token == ~(UINT64)0 || window_paint_count == WINDOW_PAINT_LIMIT ||
        current->window_paint_count == WINDOW_PAINT_THREAD_LIMIT || !(paint = mem_alloc( sizeof(*paint) )))
    {
        win->paint_failed = 1;
        if (window_paint_token == ~(UINT64)0) set_error( STATUS_TOO_MANY_CONTEXT_IDS );
        else if (!req->tracked || window_paint_count == WINDOW_PAINT_LIMIT ||
                 current->window_paint_count == WINDOW_PAINT_THREAD_LIMIT)
            set_error( STATUS_NO_MEMORY );
        return;
    }
    paint->thread = (struct thread *)grab_object( current );
    paint->window = (struct window *)grab_object( win );
    paint->top = (struct window *)grab_object( top );
    paint->token = ++window_paint_token;
    paint->scene = top->client_surface_scene_generation;
    paint->window_rect = win->window_rect;
    paint->client_rect = win->client_rect;
    paint->dpi = get_window_dpi( win );
    paint->ended = paint->failed = 0;
    paint->region = NULL;
    list_add_tail( &window_paints, &paint->entry );
    list_add_head( &current->window_paints, &paint->thread_entry );
    ++current->window_paint_count;
    ++window_paint_count;
    reply->token = paint->token;
}

DECL_HANDLER(end_window_paint)
{
    struct window_paint *paint = get_window_paint( req->token );

    if (!paint) return;
    if (paint->ended) set_error( STATUS_INVALID_PARAMETER );
    else if (req->cancel) release_window_paint( paint, !!paint->region );
    else if (paint->failed) release_window_paint( paint, 1 );
    else paint->ended = 1;
}

DECL_HANDLER(complete_window_paint)
{
    struct window_paint *paint = get_window_paint( req->token );

    if (!paint) return;
    if (!paint->ended && !req->success) paint->failed = 1;
    else if (!paint->ended) set_error( STATUS_INVALID_PARAMETER );
    else
    {
        /* Invalidate checkpoints admitted while this writer's native upload
         * was still pending, even when geometry and the paint set stayed fixed. */
        if (req->success) invalidate_window_paint( paint->window );
        release_window_paint( paint, !req->success );
    }
}

static int record_window_paint_region( struct window *win )
{
    struct window_paint *paint;

    if (!win->update_region) return 1;
    LIST_FOR_EACH_ENTRY( paint, &current->window_paints, struct window_paint, thread_entry )
    {
        if (paint->window != win || paint->ended) continue;
        if (!paint->region && !(paint->region = create_empty_region())) return 0;
        return !!union_region( paint->region, paint->region, win->update_region );
    }
    return 1;
}

/* expose the areas revealed by a vis region change on the window parent */
/* returns the region exposed on the window itself (in client coordinates) */
static struct region *expose_window( struct window *win, const struct rectangle *old_window_rect,
                                     struct region *old_vis_rgn, int zorder_changed )
{
    struct region *new_vis_rgn, *exposed_rgn;
    int is_composited = is_parent_composited( win );

    if (!(new_vis_rgn = get_visible_region( win, DCX_WINDOW ))) return NULL;

    if (is_composited && !zorder_changed &&
        is_rect_equal( old_window_rect, &win->window_rect ) &&
        is_region_equal( old_vis_rgn, new_vis_rgn ))
    {
        free_region( new_vis_rgn );
        return NULL;
    }

    if ((exposed_rgn = create_empty_region()))
    {
        if ((is_composited ? union_region( exposed_rgn, new_vis_rgn, old_vis_rgn )
                           : subtract_region( exposed_rgn, new_vis_rgn, old_vis_rgn )) &&
            !is_region_empty( exposed_rgn ))
        {
            /* make it relative to the new client area */
            offset_region( exposed_rgn, win->window_rect.left - win->client_rect.left,
                           win->window_rect.top - win->client_rect.top );
        }
        else
        {
            free_region( exposed_rgn );
            exposed_rgn = NULL;
        }
    }

    if (!is_toplevel( win ))
    {
        /* make it relative to the old window pos for subtracting */
        offset_region( new_vis_rgn, win->window_rect.left - old_window_rect->left,
                       win->window_rect.top - old_window_rect->top  );

        if (is_region_empty( old_vis_rgn ) ||
            (is_composited ? union_region( new_vis_rgn, old_vis_rgn, new_vis_rgn )
                           : subtract_region( new_vis_rgn, old_vis_rgn, new_vis_rgn )))
        {
            if (!is_region_empty( new_vis_rgn ))
            {
                /* make it relative to parent */
                offset_region( new_vis_rgn, old_window_rect->left, old_window_rect->top );
                redraw_window( win->parent, new_vis_rgn, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN, 0 );
            }
        }
    }
    free_region( new_vis_rgn );
    return exposed_rgn;
}


/* Scene clipping observes the covered visible/client rectangles, not the
 * allocation extent of a rounded GDI surface. Retiring that padding after
 * DIRECT publication must not revoke unchanged native geometry. Keep both
 * intersections: the client can extend beyond the visible rectangle. */
static int client_surface_surface_clip_changed( struct window *win, const struct rectangle *surface_rect )
{
    struct rectangle client, before, after;

    if (is_rect_equal( &win->surface_rect, surface_rect )) return 0;
    if (!intersect_rect( &before, &win->visible_rect, &win->surface_rect )) before = empty_rect;
    if (!intersect_rect( &after, &win->visible_rect, surface_rect )) after = empty_rect;
    if (!is_rect_equal( &before, &after )) return 1;
    intersect_rect( &client, &win->window_rect, &win->client_rect );
    if (!intersect_rect( &before, &client, &win->surface_rect )) before = empty_rect;
    if (!intersect_rect( &after, &client, surface_rect )) after = empty_rect;
    return !is_rect_equal( &before, &after );
}

/* set the window and client rectangles, updating the update region if necessary */
static void set_window_pos( struct window *win, struct window *previous,
                            unsigned int swp_flags, const struct rectangle *window_rect,
                            const struct rectangle *client_rect, const struct rectangle *visible_rect,
                            const struct rectangle *surface_rect, const struct rectangle *valid_rect )
{
    struct region *old_vis_rgn = NULL, *exposed_rgn = NULL;
    const struct rectangle old_window_rect = win->window_rect;
    const struct rectangle old_visible_rect = win->visible_rect;
    const struct rectangle old_client_rect = win->client_rect;
    const unsigned int old_ex_style = win->ex_style;
    struct window *client_surface_top = NULL;
    struct window *scene_top = get_toplevel_window( win );
    struct rectangle rect;
    int client_changed, frame_changed, scene_change, placement_only;
    int visible = (win->style & WS_VISIBLE) || (swp_flags & SWP_SHOWWINDOW);
    int zorder_changed = 0;

    if (win->parent && !is_visible( win->parent )) visible = 0;

    if (visible && !(old_vis_rgn = get_visible_region( win, DCX_WINDOW ))) return;

    /* set the new window info before invalidating anything */

    scene_change = memcmp( window_rect, &old_window_rect, sizeof(*window_rect) ) ||
                   memcmp( visible_rect, &old_visible_rect, sizeof(*visible_rect) ) ||
                   memcmp( client_rect, &old_client_rect, sizeof(*client_rect) ) ||
                   client_surface_surface_clip_changed( win, surface_rect );
    /* Client and frame extents must survive the same translation. Visible
     * and surface rectangles may change with clipping, but are not source
     * image bounds. Visibility and order only change the selected roster's
     * contribution; newly visible cold images still require owner inventory. */
    rect = old_window_rect;
    offset_rect( &rect, client_rect->left - old_client_rect.left,
                        client_rect->top - old_client_rect.top );
    placement_only = client_surface_child_placement_compatible( win, win->parent ) &&
                     !(swp_flags & (SWP_FRAMECHANGED | SWP_STATECHANGED)) &&
                     is_rect_equal( window_rect, &rect ) &&
                     client_rect->right - client_rect->left == old_client_rect.right - old_client_rect.left &&
                     client_rect->bottom - client_rect->top == old_client_rect.bottom - old_client_rect.top;
    scene_change = scene_change ||
                   (swp_flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW)) ||
                   (!(swp_flags & SWP_NOZORDER) && win->parent);
    scene_change = scene_change &&
                   (scene_top->client_surface_subtree_count || win->client_surface_subtree_count);
    if (scene_change)
    {
        if (placement_only) begin_client_surface_cached_scene_change( scene_top );
        else begin_client_surface_scene_change( scene_top );
    }

    if (has_client_surface( win ))
        client_surface_top = get_toplevel_window( win );

    win->window_rect  = *window_rect;
    win->visible_rect = *visible_rect;
    win->surface_rect = *surface_rect;
    win->client_rect  = *client_rect;
    if (!(swp_flags & SWP_NOZORDER) && win->parent)
    {
        int was_linked = win->is_linked;

        zorder_changed |= link_window( win, previous );
        if (!was_linked && win->client_surface_subtree_count)
            adjust_client_surface_subtree_count( win->parent, win->client_surface_subtree_count );
        /* link_window() can also change WS_EX_TOPMOST. Such a style change
         * is outside the retained-source ordering decision. */
        if (win->ex_style != old_ex_style) scene_top->client_surface_transaction.source_pending = 0;
    }
    if (swp_flags & SWP_SHOWWINDOW) win->style |= WS_VISIBLE;
    else if (swp_flags & SWP_HIDEWINDOW) win->style &= ~WS_VISIBLE;
    if (client_surface_top)
    {
        if (!is_visible( client_surface_top ))
        {
            client_surface_top->client_surface_dirty = 1;
            client_surface_top->client_surface_transaction.staged = 0;
            finish_client_surface_generation( client_surface_top );
        }
        else if (!is_visible( win ) &&
                 retire_client_surface_subtree_generation( client_surface_top, win,
                                                           client_surface_transaction_generation(
                                                               client_surface_top ) ))
            post_message( client_surface_top->handle, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
    }

    /* update window monitor dpi for toplevel windows */
    if (is_toplevel( win )) set_window_monitor_dpi( win );

    /* keep children at the same position relative to top right corner when the parent is mirrored */
    if (win->ex_style & WS_EX_LAYOUTRTL)
    {
        struct window *child;
        int old_size = old_client_rect.right - old_client_rect.left;
        int new_size = win->client_rect.right - win->client_rect.left;

        if (old_size != new_size) LIST_FOR_EACH_ENTRY( child, &win->children, struct window, entry )
        {
            offset_rect( &child->window_rect, new_size - old_size, 0 );
            offset_rect( &child->visible_rect, new_size - old_size, 0 );
            offset_rect( &child->surface_rect, new_size - old_size, 0 );
            offset_rect( &child->client_rect, new_size - old_size, 0 );
        }
    }

    if (scene_change) end_client_surface_scene_change( scene_top );

    /* reset cursor clip rectangle when the desktop changes size */
    if (win == win->desktop->top_window) set_clip_rectangle( win->desktop, NULL, SET_CURSOR_NOCLIP, 1 );

    /* if the window is not visible, everything is easy */
    if (!visible) return;

    /* expose anything revealed by the change */

    if (!(swp_flags & SWP_NOREDRAW))
        exposed_rgn = expose_window( win, &old_window_rect, old_vis_rgn, zorder_changed );

    if (!(win->style & WS_VISIBLE))
    {
        /* clear the update region since the window is no longer visible */
        validate_whole_window( win );
        validate_children( win );
        goto done;
    }

    /* crop update region to the new window rect */

    if (win->update_region)
    {
        if (get_window_visible_rect( win, &rect, 1 ))
        {
            struct region *tmp = create_empty_region();
            if (tmp)
            {
                set_region_rect( tmp, &rect );
                if (intersect_region( tmp, win->update_region, tmp ))
                    set_update_region( win, tmp );
                else
                    free_region( tmp );
            }
        }
        else set_update_region( win, NULL ); /* visible rect is empty */
    }

    /* crop children regions to the new window rect */

    if (get_window_visible_rect( win, &rect, 0 ))
    {
        /* map to client coords */
        offset_rect( &rect, win->window_rect.left - win->client_rect.left,
                     win->window_rect.top - win->client_rect.top );
        crop_children_update_region( win, &rect );
    }
    else crop_children_update_region( win, NULL );

    if (swp_flags & SWP_NOREDRAW) goto done;  /* do not repaint anything */

    /* expose the whole non-client area if it changed in any way */

    if (swp_flags & SWP_NOCOPYBITS)
    {
        frame_changed = ((swp_flags & SWP_FRAMECHANGED) ||
                         memcmp( window_rect, &old_window_rect, sizeof(old_window_rect) ) ||
                         memcmp( visible_rect, &old_visible_rect, sizeof(old_visible_rect) ));
        client_changed = memcmp( client_rect, &old_client_rect, sizeof(old_client_rect) );
    }
    else
    {
        /* assume the bits have been moved to follow the window rect */
        int x_offset = window_rect->left - old_window_rect.left;
        int y_offset = window_rect->top - old_window_rect.top;
        frame_changed = ((swp_flags & SWP_FRAMECHANGED) ||
                         window_rect->right  - old_window_rect.right != x_offset ||
                         window_rect->bottom - old_window_rect.bottom != y_offset ||
                         visible_rect->left   - old_visible_rect.left   != x_offset ||
                         visible_rect->right  - old_visible_rect.right  != x_offset ||
                         visible_rect->top    - old_visible_rect.top    != y_offset ||
                         visible_rect->bottom - old_visible_rect.bottom != y_offset);
        client_changed = (client_rect->left   - old_client_rect.left   != x_offset ||
                          client_rect->right  - old_client_rect.right  != x_offset ||
                          client_rect->top    - old_client_rect.top    != y_offset ||
                          client_rect->bottom - old_client_rect.bottom != y_offset ||
                          memcmp( valid_rect, client_rect, sizeof(*client_rect) ));
    }

    if (frame_changed || client_changed)
    {
        struct region *win_rgn = old_vis_rgn;  /* reuse previous region */

        set_region_rect( win_rgn, window_rect );
        if (!is_rect_empty( valid_rect ))
        {
            /* subtract the valid portion of client rect from the total region */
            struct region *tmp = create_empty_region();
            if (tmp)
            {
                set_region_rect( tmp, valid_rect );
                /* subtract update region since invalid parts of the valid rect won't be copied */
                if (win->update_region)
                {
                    offset_region( tmp, -window_rect->left, -window_rect->top );
                    subtract_region( tmp, tmp, win->update_region );
                    offset_region( tmp, window_rect->left, window_rect->top );
                }
                if (subtract_region( tmp, win_rgn, tmp )) win_rgn = tmp;
                else free_region( tmp );
            }
        }
        if (!is_desktop_window(win))
            offset_region( win_rgn, -client_rect->left, -client_rect->top );
        if (exposed_rgn)
        {
            union_region( exposed_rgn, exposed_rgn, win_rgn );
            if (win_rgn != old_vis_rgn) free_region( win_rgn );
        }
        else
        {
            exposed_rgn = win_rgn;
            if (win_rgn == old_vis_rgn) old_vis_rgn = NULL;
        }
    }

    if (exposed_rgn)
        redraw_window( win, exposed_rgn, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN, 0 );

done:
    if (old_vis_rgn) free_region( old_vis_rgn );
    if (exposed_rgn) free_region( exposed_rgn );
    clear_error();  /* we ignore out of memory errors once the new rects have been set */
}


/* set the window region, updating the update region if necessary */
static void set_window_region( struct window *win, struct region *region, int redraw )
{
    struct region *old_vis_rgn = NULL, *exposed_rgn;
    struct window *scene_top = get_toplevel_window( win );
    int scene_change = !!scene_top->client_surface_subtree_count;

    /* no need to redraw if window is not visible */
    if (redraw && !is_visible( win )) redraw = 0;

    if (redraw) old_vis_rgn = get_visible_region( win, DCX_WINDOW );

    if (scene_change) begin_client_surface_cached_scene_change( scene_top );
    if (win->win_region) free_region( win->win_region );
    win->win_region = region;
    if (scene_change) end_client_surface_scene_change( scene_top );

    /* expose anything revealed by the change */
    if (old_vis_rgn && ((exposed_rgn = expose_window( win, &win->window_rect, old_vis_rgn, 0 ))))
    {
        redraw_window( win, exposed_rgn, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN, 0 );
        free_region( exposed_rgn );
    }

    if (old_vis_rgn) free_region( old_vis_rgn );
    clear_error();  /* we ignore out of memory errors since the region has been set */
}

/* check if DPI awareness contexts are compatible */
static bool is_dpi_awareness_compatible( struct window *win, struct window *other )
{
    unsigned int awareness = NTUSER_DPI_CONTEXT_GET_AWARENESS( win->shared->dpi_context );
    return awareness == NTUSER_DPI_CONTEXT_GET_AWARENESS( other->shared->dpi_context );
}


/* destroy a window */
void free_window_handle( struct window *win )
{
    struct window *child, *next;
    struct window *client_surface_top;
    int scene_change, wake_client_surface;

    assert( win->handle );
    cancel_window_paints( win );
    client_surface_top = get_toplevel_window( win );
    scene_change = client_surface_top->client_surface_subtree_count &&
                   (win->client_surface_subtree_count || (win->is_linked && (win->style & WS_VISIBLE)));
    if (scene_change)
    {
        /* Removing a source-free leaf only exposes the surviving images.
         * The owner must still validate those images against the new scene.
         * Keep subtree teardown and any existing cold assembly conservative. */
        if (!win->client_surface_subtree_count && is_visible( win ) &&
            list_empty( &win->children ) && list_empty( &win->unlinked ) &&
            client_surface_child_placement_compatible( win, win->parent ))
            begin_client_surface_cached_scene_change( client_surface_top );
        else begin_client_surface_scene_change( client_surface_top );
    }
    if (client_surface_top == win) finish_client_surface_generation( win );

    /* hide the window */
    if (is_visible(win))
    {
        struct region *vis_rgn = get_visible_region( win, DCX_WINDOW );
        win->style &= ~WS_VISIBLE;
        if (vis_rgn)
        {
            struct region *exposed_rgn = expose_window( win, &win->window_rect, vis_rgn, 0 );
            if (exposed_rgn) free_region( exposed_rgn );
            free_region( vis_rgn );
        }
        validate_whole_window( win );
        validate_children( win );
    }

    /* Children owned by another thread are destroyed asynchronously.  Stop
     * waiting for their now-hidden surfaces before this subtree is moved to
     * the unlinked list; the foreign thread may not service its notification. */
    if (client_surface_top != win &&
        retire_client_surface_subtree_generation( client_surface_top, win,
                                                  client_surface_transaction_generation(
                                                      client_surface_top ) ))
        post_message( client_surface_top->handle, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );

    /* destroy all children */
    LIST_FOR_EACH_ENTRY_SAFE( child, next, &win->children, struct window, entry )
    {
        if (!child->handle) continue;
        if (!win->thread || !child->thread || win->thread == child->thread)
            free_window_handle( child );
        else
            send_notify_message( child->handle, WM_WINE_DESTROYWINDOW, 0, 0 );
    }
    LIST_FOR_EACH_ENTRY_SAFE( child, next, &win->unlinked, struct window, entry )
    {
        if (!child->handle) continue;
        if (!win->thread || !child->thread || win->thread == child->thread)
            free_window_handle( child );
        else
            send_notify_message( child->handle, WM_WINE_DESTROYWINDOW, 0, 0 );
    }

    /* A dying process cannot send the normal UNREGISTER/UNCACHE requests.
     * Remove its server-side surface identities before unlinking the window,
     * while the original top-level generation is still reachable. */
    wake_client_surface = discard_client_surface_owners( win, client_surface_top );
    if (wake_client_surface && client_surface_top != win)
        post_message( client_surface_top->handle, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );

    /* reset global window pointers, if the corresponding window is destroyed */
    if (win == win->desktop->shell_window) win->desktop->shell_window = NULL;
    if (win == win->desktop->shell_listview) win->desktop->shell_listview = NULL;
    if (win == win->desktop->progman_window) win->desktop->progman_window = NULL;
    if (win == win->desktop->taskman_window) win->desktop->taskman_window = NULL;
    free_hotkeys( win->desktop, win->handle );
    cleanup_clipboard_window( win->desktop, win->handle );
    destroy_properties( win );
    if (is_desktop_window(win))
    {
        struct desktop *desktop = win->desktop;
        assert( desktop->top_window == win || desktop->msg_window == win );
        if (desktop->top_window == win) desktop->top_window = NULL;
        else desktop->msg_window = NULL;
    }
    else if (is_desktop_window( win->parent ))
    {
        post_message( win->parent->handle, WM_PARENTNOTIFY, WM_DESTROY, win->handle );
    }

    detach_window_thread( win );

    if (win->parent) set_parent_window( win, NULL );
    if (scene_change) end_client_surface_scene_change( client_surface_top );
    free_user_handle( win->handle );
    win->handle = 0;
    release_object( win );
}

static void fix_window_ex_style( struct window *win )
{
    if (win->ex_style & WS_EX_DLGMODALFRAME) win->ex_style |= WS_EX_WINDOWEDGE;
    else if (win->ex_style & WS_EX_STATICEDGE) win->ex_style &= ~WS_EX_WINDOWEDGE;
    else if (win->style & (WS_DLGFRAME | WS_THICKFRAME)) win->ex_style |= WS_EX_WINDOWEDGE;
    else win->ex_style &= ~WS_EX_WINDOWEDGE;
}

static void set_window_ex_style( struct window *win, unsigned int ex_style )
{
    /* WS_EX_TOPMOST can only be changed for unlinked windows */
    if (!win->is_linked) win->ex_style = ex_style;
    else win->ex_style = (ex_style & ~WS_EX_TOPMOST) | (win->ex_style & WS_EX_TOPMOST);
    if (!(win->ex_style & WS_EX_LAYERED)) win->is_layered = 0;
}


/* create a window */
DECL_HANDLER(create_window)
{
    struct window *win, *parent = NULL, *owner = NULL;
    struct unicode_str cls_name = get_req_unicode_str();
    struct atom_table *table = get_user_atom_table();
    atom_t atom = req->atom;

    reply->handle = 0;
    if (req->parent)
    {
        if (!(parent = get_window( req->parent ))) return;
        if (is_orphan_window( parent ))
        {
            set_error( STATUS_INVALID_PARAMETER );
            return;
        }
    }

    if (req->owner)
    {
        if (!(owner = get_window( req->owner ))) return;
        if (is_desktop_window(owner)) owner = NULL;
        else if (parent && !is_desktop_window(parent))
        {
            /* an owned window must be created as top-level */
            set_error( STATUS_ACCESS_DENIED );
            return;
        }
        else /* owner must be a top-level window */
            while ((owner->style & (WS_POPUP|WS_CHILD)) == WS_CHILD && !is_desktop_window(owner->parent))
                owner = owner->parent;
    }

    if (!atom) atom = find_atom( table, cls_name );

    if (!(win = create_window( parent, owner, atom, req->class_instance, !!req->ansi,
                               req->dpi_context, req->dpi, req->raw_dpi )))
        return;

    SHARED_WRITE_BEGIN( win->shared, window_shm_t )
    {
        shared->info.instance   = req->instance;
        if (parent && !is_desktop_window( parent ))
        {
            shared->dpi         = parent->shared->dpi;
            shared->raw_dpi     = parent->shared->raw_dpi;
        }
    }
    SHARED_WRITE_END;

    win->style = req->style;
    win->ex_style = req->ex_style;

    reply->handle      = win->handle;
    reply->parent      = win->parent ? win->parent->handle : 0;
    reply->owner       = win->owner;
    reply->class_ptr   = get_class_client_ptr( win->class );
}


/* Set the window builtin class FNID */
DECL_HANDLER(set_window_fnid)
{
    data_size_t extra_size, private_size;
    struct obj_locator class_locator;
    struct window_class *class;
    struct window *win;
    unsigned int fnid;

    if (!(win = get_window( req->handle ))) return;
    if (is_desktop_window( win ) && win->thread != current) return set_error( STATUS_ACCESS_DENIED );

    if (!(class = grab_class( current->process, req->atom, 0, &class_locator ))) return;
    fnid = get_class_fnid( class, &extra_size, &private_size );

    if (win->shared->fnid && win->shared->fnid != fnid) set_error( STATUS_INVALID_PARAMETER );
    else SHARED_WRITE_BEGIN( win->shared, window_shm_t )
    {
        shared->fnid            = fnid;
        shared->private_size    = private_size;
    }
    SHARED_WRITE_END;
    release_class( class );
}


/* set the parent of a window */
DECL_HANDLER(set_parent)
{
    struct window *win, *parent = NULL;

    if (!(win = get_window( req->handle ))) return;
    if (req->parent && !(parent = get_window( req->parent ))) return;

    /* reparenting to a window with a different DPI awareness isn't allowed */
    if (parent && !is_desktop_window( parent ) && !is_dpi_awareness_compatible( win, parent ))
        return set_error( STATUS_INVALID_STATE_TRANSITION );

    if (is_desktop_window(win) || is_orphan_window( win ) || (parent && is_orphan_window( parent )))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    reply->old_parent  = win->parent->handle;
    reply->full_parent = parent ? parent->handle : 0;
    set_parent_window( win, parent );
}


/* destroy a window */
DECL_HANDLER(destroy_window)
{
    struct window *win;

    if (!req->handle)
    {
        destroy_thread_windows( current );
    }
    else if ((win = get_window( req->handle )))
    {
        if (!is_desktop_window(win)) free_window_handle( win );
        else if (win->thread == current) detach_window_thread( win );
        else set_error( STATUS_ACCESS_DENIED );
    }
}


/* retrieve the desktop window for the current thread */
DECL_HANDLER(get_desktop_window)
{
    static const struct monitor_info default_info = { .dpi = { USER_DEFAULT_SCREEN_DPI, 1 }, .raw_dpi = { USER_DEFAULT_SCREEN_DPI, 1 } };
    static const struct rectangle desktop_rect = { 0, 0, 1, 1 };
    const struct monitor_info *info = NULL;

    struct desktop *desktop = get_thread_desktop( current, 0 );

    if (!desktop) return;

    if (!desktop->top_window && req->force)  /* create it */
    {
        if (!(info = get_monitor_from_rect( desktop->winstation, &desktop_rect, false ))) info = &default_info;
        if ((desktop->top_window = create_window( NULL, NULL, DESKTOP_ATOM, 0, false, NTUSER_DPI_PER_MONITOR_AWARE, info->dpi, info->raw_dpi )))
        {
            detach_window_thread( desktop->top_window );
            desktop->top_window->style  = WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
        }
    }

    if (!desktop->msg_window && req->force)  /* create it */
    {
        static const WCHAR messageW[] = {'M','e','s','s','a','g','e'};
        static const struct unicode_str name = { messageW, sizeof(messageW) };
        struct atom_table *table = get_user_atom_table();
        atom_t atom = add_atom( table, name );

        if (!info && !(info = get_monitor_from_rect( desktop->winstation, &desktop_rect, false ))) info = &default_info;
        if (atom && (desktop->msg_window = create_window( NULL, NULL, atom, 0, false, NTUSER_DPI_PER_MONITOR_AWARE, info->dpi, info->raw_dpi )))
        {
            detach_window_thread( desktop->msg_window );
            desktop->msg_window->style = WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
        }
    }

    reply->top_window = desktop->top_window ? desktop->top_window->handle : 0;
    reply->msg_window = desktop->msg_window ? desktop->msg_window->handle : 0;
    release_object( desktop );
}


/* set a window owner */
DECL_HANDLER(set_window_owner)
{
    struct window *win = get_window( req->handle );
    struct window *owner = NULL, *ptr;

    if (!win) return;
    if (req->owner && !(owner = get_window( req->owner ))) return;
    if (is_desktop_window(win))
    {
        set_error( STATUS_ACCESS_DENIED );
        return;
    }

    /* make sure owner is not a successor of window */
    for (ptr = owner; ptr; ptr = ptr->owner ? get_window( ptr->owner ) : NULL)
    {
        if (ptr == win)
        {
            set_error( STATUS_INVALID_PARAMETER );
            return;
        }
    }

    reply->prev_owner = win->owner;
    reply->full_owner = win->owner = owner ? owner->handle : 0;
}


/* get information from a window handle */
DECL_HANDLER(get_window_info)
{
    struct window *win;

    if (!(win = get_window( req->handle ))) return;

    reply->last_active = win->handle;
    if (get_user_object( win->last_active, NTUSER_OBJ_WINDOW )) reply->last_active = win->last_active;

    switch (req->offset)
    {
    case GWL_STYLE:       reply->info = win->style;  break;
    case GWL_EXSTYLE:     reply->info = win->ex_style;  break;
    case GWLP_WINE_PIXEL_FORMAT:
        if (!req->size) reply->info = win->pixel_format;
        else set_win32_error( ERROR_INVALID_INDEX );
        break;
    default:
        if (req->size) set_win32_error( ERROR_INVALID_INDEX );
        break;
    }
}


/* initialize some window information */
DECL_HANDLER(init_window_info)
{
    struct window *win;

    if (!(win = get_window( req->handle ))) return;
    win->style = req->style;
    win->ex_style = req->ex_style;

    /* changing window style triggers a non-client paint */
    win->paint_flags |= PAINT_NONCLIENT;
}


/* set some information in a window */
DECL_HANDLER(set_window_info)
{
    struct window *win, *scene_top;
    int scene_change;
    bool ansi;

    if (!(win = get_window( req->handle ))) return;
    if (is_desktop_window( win ) && win->thread != current)
    {
        set_error( STATUS_ACCESS_DENIED );
        return;
    }

    scene_top = get_toplevel_window( win );
    scene_change = (req->offset == GWL_STYLE || req->offset == GWL_EXSTYLE) &&
                   scene_top->client_surface_subtree_count;
    if (scene_change) begin_client_surface_scene_change( scene_top );

    SHARED_WRITE_BEGIN( win->shared, window_shm_t )
    {
        switch (req->offset)
        {
        case GWL_STYLE:
            reply->old_info = win->style;
            win->style = req->new_info;
            fix_window_ex_style( win );
            /* changing window style triggers a non-client paint */
            win->paint_flags |= PAINT_NONCLIENT;
            break;
        case GWL_EXSTYLE:
            reply->old_info = win->ex_style;
            set_window_ex_style( win, req->new_info );
            break;
        case GWLP_ID:
            reply->old_info = shared->info.id;
            shared->info.id = req->new_info;
            break;
        case GWLP_HINSTANCE:
            reply->old_info = shared->info.instance;
            shared->info.instance = req->new_info;
            break;
        case GWLP_WNDPROC:
            reply->old_info = shared->info.wndproc;
            reply->old_ansi = shared->ansi;
            if (req->new_info) shared->info.wndproc = req->new_info;
            else shared->info.wndproc = get_class_wndproc( win->class, &ansi );
            shared->ansi = req->new_ansi; /* class ansi is actually ignored */
            break;
        case GWLP_USERDATA:
            reply->old_info = shared->info.user_data;
            if (req->size > sizeof(WORD)) shared->info.user_data = req->new_info;
            else shared->info.user_data = MAKELONG(req->new_info, shared->info.user_data >> 16);
            break;
        case GWLP_WINE_PIXEL_FORMAT:
            if (!req->internal || req->size) set_win32_error( ERROR_INVALID_INDEX );
            else
            {
                reply->old_info = win->pixel_format;
                win->pixel_format = req->new_info;
            }
            break;
        default:
            if (req->size > sizeof(req->new_info) || req->offset < 0 ||
                req->offset > shared->extra_size - (int)req->size ||
                (!req->internal && req->offset < shared->private_size))
            {
                set_win32_error( ERROR_INVALID_INDEX );
                break;
            }
            memcpy( &reply->old_info, (char *)shared->extra + req->offset, req->size );
            memcpy( (char *)shared->extra + req->offset, &req->new_info, req->size );
            break;
        }
    }
    SHARED_WRITE_END;
    if (scene_change) end_client_surface_scene_change( scene_top );
}


/* get a list of the window parents, up to the root of the tree */
DECL_HANDLER(get_window_parents)
{
    struct window *ptr, *win = get_window( req->handle );
    int total = 0;
    user_handle_t *data;
    data_size_t len;

    if (win) for (ptr = win->parent; ptr; ptr = ptr->parent) total++;

    reply->count = total;
    len = min( get_reply_max_size(), total * sizeof(user_handle_t) );
    if (len && ((data = set_reply_data_size( len ))))
    {
        for (ptr = win->parent; ptr && len; ptr = ptr->parent, len -= sizeof(*data))
            *data++ = ptr->handle;
    }
}


/* get a list of window siblings or children */
DECL_HANDLER(get_window_list)
{
    struct window *win = NULL;
    struct desktop *desktop = NULL;
    struct thread *thread = NULL;
    user_handle_t *data;
    unsigned int count = 0, max_count = get_reply_max_size() / sizeof(*data);

    if (req->handle && !(win = get_window( req->handle )))
    {
        set_error( STATUS_INVALID_HANDLE );
        return;
    }
    if (req->tid && !(thread = get_thread_from_id( req->tid )))
    {
        set_error( STATUS_INVALID_HANDLE );
        return;
    }
    if (req->desktop && !(desktop = get_desktop_obj( current->process, req->desktop, DESKTOP_READOBJECTS )))
    {
        if (thread) release_object( thread );
        return;
    }

    max_count = min( max_count, MAX_USER_HANDLES );
    if ((data = mem_alloc( max_count * sizeof(*data) )))
    {
        get_window_list( desktop, win, thread, req->children, data, &count, max_count );
        if (count > max_count)
        {
            free( data );
            set_error( STATUS_BUFFER_TOO_SMALL );
        }
        else set_reply_data_ptr( data, count * sizeof(*data) );
        reply->count = count;
    }

    if (thread) release_object( thread );
    if (desktop) release_object( desktop );
}


/* get a list of the window siblings of a specified class */
DECL_HANDLER(get_class_windows)
{
    struct desktop *desktop = NULL;
    struct window *parent = NULL, *win = NULL;
    struct unicode_str cls_name = get_req_unicode_str();
    struct atom_table *table = get_user_atom_table();
    atom_t atom = req->atom;
    user_handle_t *data;
    unsigned int count = 0, max_count = get_reply_max_size() / sizeof(*data);

    if (!atom && cls_name.len && !(atom = find_atom( table, cls_name ))) return;
    if (req->parent && !(parent = get_window( req->parent ))) return;

    if (req->child)
    {
        if (!parent) parent = get_desktop_window( current );
        if (!(win = get_window( req->child ))) return;
        if (win->parent != parent) return;
        if (!(win = get_next_window( win ))) return;
    }
    else if (parent && !(win = get_first_child( parent ))) return;

    if (!win && !(desktop = get_thread_desktop( current, 0 ))) return;

    max_count = min( max_count, MAX_USER_HANDLES );
    if ((data = mem_alloc( max_count * sizeof(*data) )))
    {
        if (desktop) /* top-level and message windows of current desktop */
        {
            if (desktop->top_window)
                for (win = get_first_child( desktop->top_window ); win; win = get_next_window( win ))
                    append_window_to_list( win, NULL, atom, data, &count, max_count );
            if (desktop->msg_window)
                for (win = get_first_child( desktop->msg_window ); win; win = get_next_window( win ))
                    append_window_to_list( win, NULL, atom, data, &count, max_count );
        }
        else
        {
            for ( ; win; win = get_next_window( win ))
                append_window_to_list( win, NULL, atom, data, &count, max_count );
        }
        if (count > max_count)
        {
            free( data );
            set_error( STATUS_BUFFER_TOO_SMALL );
        }
        else set_reply_data_ptr( data, count * sizeof(*data) );
        reply->count = count;
    }

    if (desktop) release_object( desktop );
}


/* get a list of the window children that contain a given point */
DECL_HANDLER(get_window_children_from_point)
{
    struct user_handle_array array;
    struct window *parent = get_window( req->parent );
    data_size_t len;

    if (!parent) return;

    array.handles = NULL;
    array.count = 0;
    array.total = 0;
    if (!all_windows_from_point( parent, req->x, req->y, req->dpi, &array )) return;

    reply->count = array.count;
    len = min( get_reply_max_size(), array.count * sizeof(user_handle_t) );
    if (len) set_reply_data_ptr( array.handles, len );
    else free( array.handles );
}


/* get window tree information from a window handle */
DECL_HANDLER(get_window_tree)
{
    struct window *ptr, *win = get_window( req->handle );

    if (!win) return;

    reply->parent        = 0;
    reply->owner         = 0;
    reply->next_sibling  = 0;
    reply->prev_sibling  = 0;
    reply->first_sibling = 0;
    reply->last_sibling  = 0;
    reply->first_child   = 0;
    reply->last_child    = 0;

    if (win->parent)
    {
        struct window *parent = win->parent;
        reply->parent = parent->handle;
        reply->owner  = win->owner;
        if (win->is_linked)
        {
            if ((ptr = get_next_window( win ))) reply->next_sibling = ptr->handle;
            if ((ptr = get_prev_window( win ))) reply->prev_sibling = ptr->handle;
        }
        if ((ptr = get_first_child( parent ))) reply->first_sibling = ptr->handle;
        if ((ptr = get_last_child( parent ))) reply->last_sibling = ptr->handle;
    }
    if ((ptr = get_first_child( win ))) reply->first_child = ptr->handle;
    if ((ptr = get_last_child( win ))) reply->last_child = ptr->handle;
}


/* set the position and Z order of a window */
DECL_HANDLER(set_window_pos)
{
    struct rectangle window_rect, client_rect, visible_rect, surface_rect, valid_rect, old_window, old_client;
    const struct rectangle *extra_rects = get_req_data();
    struct window *previous = NULL;
    struct window *top, *win = get_window( req->handle );
    unsigned int flags = req->swp_flags, old_style;

    if (!win) return;
    if (!win->parent) flags |= SWP_NOZORDER;  /* no Z order for the desktop */

    if (!(flags & SWP_NOZORDER))
    {
        switch ((int)req->previous)
        {
        case 0:   /* HWND_TOP */
            previous = WINPTR_TOP;
            break;
        case 1:   /* HWND_BOTTOM */
            previous = WINPTR_BOTTOM;
            break;
        case -1:  /* HWND_TOPMOST */
            previous = WINPTR_TOPMOST;
            break;
        case -2:  /* HWND_NOTOPMOST */
            previous = WINPTR_NOTOPMOST;
            break;
        default:
            if (!(previous = get_window( req->previous ))) return;
            /* previous must be a sibling */
            if (previous->parent != win->parent)
            {
                set_error( STATUS_INVALID_PARAMETER );
                return;
            }
            break;
        }
        if (previous == win) flags |= SWP_NOZORDER;  /* nothing to do */
    }

    /* windows that use UpdateLayeredWindow don't trigger repaints */
    if ((win->ex_style & WS_EX_LAYERED) && !win->is_layered) flags |= SWP_NOREDRAW;

    /* window rectangle must be ordered properly */
    if (req->window.right < req->window.left || req->window.bottom < req->window.top)
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    window_rect = req->window;
    client_rect = req->client;
    if (get_req_data_size() >= sizeof(struct rectangle)) visible_rect = extra_rects[0];
    else visible_rect = window_rect;
    if (get_req_data_size() >= 2 * sizeof(struct rectangle)) surface_rect = extra_rects[1];
    else surface_rect = visible_rect;
    if (get_req_data_size() >= 3 * sizeof(struct rectangle)) valid_rect = extra_rects[2];
    else valid_rect = empty_rect;
    if (win->parent && win->parent->ex_style & WS_EX_LAYOUTRTL)
    {
        mirror_rect( &win->parent->client_rect, &window_rect );
        mirror_rect( &win->parent->client_rect, &visible_rect );
        mirror_rect( &win->parent->client_rect, &client_rect );
        mirror_rect( &win->parent->client_rect, &surface_rect );
        mirror_rect( &win->parent->client_rect, &valid_rect );
    }

    win->paint_flags = (win->paint_flags & ~PAINT_CLIENT_FLAGS) | (req->paint_flags & PAINT_CLIENT_FLAGS);
    if (win->paint_flags & PAINT_HAS_PIXEL_FORMAT) update_pixel_format_flags( win );

    old_style = win->style;
    old_window = win->window_rect;
    old_client = win->client_rect;
    set_window_pos( win, previous, flags, &window_rect, &client_rect,
                    &visible_rect, &surface_rect, &valid_rect );
    if ((win->style & old_style & WS_VISIBLE) && (memcmp( &old_client, &win->client_rect, sizeof(old_client) )
        || memcmp( &old_window, &win->window_rect, sizeof(old_window) )))
        update_cursor_pos( win->desktop );

    if (win->paint_flags & SET_WINPOS_LAYERED_WINDOW) validate_whole_window( win );

    reply->new_style = win->style;
    reply->new_ex_style = win->ex_style;

    top = get_top_clipping_window( win );
    if (is_visible( top ) && (top->paint_flags & PAINT_HAS_SURFACE)) reply->surface_win = top->handle;
    reply->client_surface_pending = is_toplevel( win ) && is_visible( win ) && win->client_surface_dirty;
}


static int client_surface_clip_intersects_bounds( struct window *win, struct ratio dpi,
                                                  const struct rectangle *top_visible,
                                                  const struct rectangle *bounds )
{
    struct rectangle rect;

    if (!bounds) return 1;
    /* The exact region is a subset of this client rectangle. Use the same
     * screen/DPI mapping as the region before rejecting a disjoint source. */
    if (!intersect_rect( &rect, &win->window_rect, &win->client_rect ) ||
        !intersect_rect( &rect, &rect, &win->surface_rect )) return 0;
    client_to_screen_rect( win->parent, &rect );
    map_dpi_rect( win, &rect, get_window_dpi( win ), dpi );
    offset_rect( &rect, -top_visible->left, -top_visible->top );
    return intersect_rect( &rect, &rect, bounds );
}

static int collect_client_surface_clip_subtree( struct window *win, struct ratio dpi,
                                                const struct rectangle *top_visible,
                                                const struct rectangle *bounds,
                                                struct client_surface_clip_window *data,
                                                unsigned int max_count, unsigned int *count )
{
    struct client_surface_owner *owner;
    const struct rectangle *rects;
    struct region *region;
    struct window *child;
    unsigned int i, rect_count;

    if (!win->client_surface_subtree_count || !is_visible( win )) return 1;
    /* A dormant registration does not own any pixels or advance the scene
     * epoch.  Derive occlusion from the same producer choice as composition,
     * otherwise identical scene tokens can describe different clip regions. */
    if (select_client_surface_producer( win, &owner ) &&
        client_surface_clip_intersects_bounds( win, dpi, top_visible, bounds ))
    {
        /* A rectangular HWND snapshot over-clips shaped windows and ignores
         * ancestor clipping.  Serialize the exact client-visible region into
         * tagged rectangles; the client can subtract the union unchanged. */
        if (!(region = get_visible_region_ex( win, 0, 0 ))) return 0;
        if (!is_region_empty( region ))
        {
            map_win_region_to_screen( win, region );
            map_dpi_region( win, region, get_window_dpi( win ), dpi );
            offset_region( region, -top_visible->left, -top_visible->top );
            rects = get_region_rectangles( region, &rect_count );
            for (i = 0; i < rect_count; ++i)
            {
                struct rectangle rect = rects[i];

                if (bounds && !intersect_rect( &rect, &rect, bounds )) continue;
                if (*count == UINT_MAX)
                {
                    free_region( region );
                    set_error( STATUS_INTEGER_OVERFLOW );
                    return 0;
                }
                if (*count < max_count)
                {
                    data[*count].handle = win->handle;
                    data[*count].rect = rect;
                }
                (*count)++;
            }
            free_region( region );
        }
        else free_region( region );
    }
    LIST_FOR_EACH_ENTRY( child, &win->children, struct window, entry )
        if (!collect_client_surface_clip_subtree( child, dpi, top_visible, bounds,
                                                  data, max_count, count )) return 0;
    return 1;
}

static int collect_client_surface_clips( struct window *win, struct window *top, struct ratio dpi,
                                         const struct rectangle *bounds,
                                         struct client_surface_clip_window *data,
                                         unsigned int max_count, unsigned int *count )
{
    struct window *child, *current, *parent;
    struct rectangle top_visible = top->visible_rect;

    client_to_screen_rect( top->parent, &top_visible );
    map_dpi_rect( top, &top_visible, get_window_dpi( top ), dpi );
    if (bounds && is_rect_empty( bounds )) return 1;
    LIST_FOR_EACH_ENTRY( child, &win->children, struct window, entry )
        if (!collect_client_surface_clip_subtree( child, dpi, &top_visible, bounds,
                                                  data, max_count, count )) return 0;

    for (current = win; current != top; current = parent)
    {
        if (!current->is_linked) break;
        parent = current->parent;
        LIST_FOR_EACH_ENTRY( child, &parent->children, struct window, entry )
        {
            if (child == current) break;
            if (!collect_client_surface_clip_subtree( child, dpi, &top_visible, bounds,
                                                       data, max_count, count )) return 0;
        }
    }
    return 1;
}

/* Return the exact set which the client compositor must subtract.  Walking
 * upward from the target visits only preceding sibling subtrees, while all
 * target descendants are above their ancestor in the Win32 child scene. */
DECL_HANDLER(get_client_surface_clip_windows)
{
    unsigned int count = 0, max_count = get_reply_max_size() / sizeof(struct client_surface_clip_window);
    struct window *top, *win = get_window( req->handle );
    struct client_surface_clip_window *data = NULL;
    const struct rectangle *bounds = get_req_data_size() ? get_req_data() : NULL;

    reply->toplevel = 0;
    reply->count = 0;
    reply->scene_generation = 0;
    if (!win) return;
    if (!req->dpi.num || !req->dpi.den ||
        (get_req_data_size() && get_req_data_size() != sizeof(*bounds)))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    top = get_toplevel_window( win );
    reply->toplevel = top->handle;
    reply->scene_generation = top->client_surface_scene_generation;
    if (bounds && is_rect_empty( bounds )) return;
    /* Entries are region rectangles, not HWNDs.  One shaped producer can
     * contribute more rectangles than the handle table has slots.  The
     * caller's reply buffer already bounds the allocation and its product. */
    if (max_count && !(data = mem_alloc( max_count * sizeof(*data) ))) return;

    if (!collect_client_surface_clips( win, top, req->dpi, bounds, data, max_count, &count )) goto failed;

    reply->count = count;
    if (data) set_reply_data_ptr( data, min( count, max_count ) * sizeof(*data) );
    return;

failed:
    free( data );
}

/* Match DCX_USESTYLE without consulting any process-local DC or window data. */
static unsigned int get_client_surface_scene_clip_flags( struct window *win )
{
    unsigned int flags = 0;

    if (win->style & WS_CLIPSIBLINGS) flags |= DCX_CLIPSIBLINGS;
    if (get_class_style( win->class ) & CS_PARENTDC) flags |= DCX_PARENTCLIP;
    if ((win->style & WS_CLIPCHILDREN) && !(win->style & WS_MINIMIZE)) flags |= DCX_CLIPCHILDREN;
    if (is_toplevel( win )) flags = (flags & ~DCX_PARENTCLIP) | DCX_CLIPSIBLINGS;
    if (flags & (DCX_CLIPSIBLINGS | DCX_CLIPCHILDREN)) flags &= ~DCX_PARENTCLIP;
    /* get_visible_region() handles ancestor sibling clipping from window
     * styles; PARENTCLIP needs no additional DCX_CLIPSIBLINGS flag here. */
    return flags;
}

static void get_client_surface_scene_geometry( struct window *win, struct window *top,
                                               struct client_surface_scene_layer *layer )
{
    struct rectangle client = top->client_rect;

    layer->window_dpi = get_window_dpi( win );
    layer->raw_dpi = win->shared->raw_dpi;
    layer->top_window = top->window_rect;
    layer->top_client = top->client_rect;
    layer->top_visible = top->visible_rect;
    map_dpi_rect( top, &layer->top_window, get_window_dpi( top ), layer->window_dpi );
    map_dpi_rect( top, &layer->top_client, get_window_dpi( top ), layer->window_dpi );
    map_dpi_rect( top, &layer->top_visible, get_window_dpi( top ), layer->window_dpi );
    if (!is_rect_empty( &win->present_rect ))
    {
        layer->source = win->present_rect;
        offset_rect( &layer->source, -layer->top_client.left, -layer->top_client.top );
        if (win == top) layer->flags = CLIENT_SURFACE_SCENE_PRESENT_RECT;
        return;
    }
    layer->source = win->client_rect;
    client_to_screen_rect( win->parent, &layer->source );
    client_to_screen_rect( top->parent, &client );
    map_dpi_rect( top, &client, get_window_dpi( top ), layer->window_dpi );
    offset_rect( &layer->source, -client.left, -client.top );
    if (top->ex_style & WS_EX_LAYOUTRTL)
    {
        struct rectangle bounds = {0, 0, client.right - client.left, client.bottom - client.top};
        mirror_rect( &bounds, &layer->source );
    }
}

static int collect_client_surface_scene_snapshot( struct window *win, struct window *top,
                                                  unsigned char *data, unsigned int max_size,
                                                  unsigned int *total, unsigned int *count )
{
    struct client_surface_owner *owner;
    struct client_surface_ref *surface;
    struct window *child;

    if (!win->client_surface_subtree_count) return 1;
    if ((surface = select_client_surface_scene_producer( win, &owner )))
    {
        struct client_surface_scene_layer layer = {0};
        struct client_surface_clip_window *clips = NULL;
        struct region *visible = NULL;
        const struct rectangle *rects = NULL;
        unsigned int clip_max = 0;
        size_t clip_offset, block_size;

        get_client_surface_handoff_desc( win, top, owner, surface, &layer.producer );
        get_client_surface_scene_geometry( win, top, &layer );
        if (win == top && !top->client_surface_transaction.staged &&
            !top->client_surface_native_barrier && client_surface_direct_candidate( top ))
            layer.flags |= CLIENT_SURFACE_SCENE_DIRECT_CANDIDATE;
        if (layer.producer.visible && !(layer.flags & CLIENT_SURFACE_SCENE_PRESENT_RECT))
        {
            if (!(visible = get_visible_region( win, get_client_surface_scene_clip_flags( win ) ))) return 0;
            /* A parent DC may draw outside its own window shape. An image
             * contribution still belongs to that window, including when
             * the general DC visibility starts with its parent's region. */
            if (win->win_region && !intersect_region( visible, visible, win->win_region ))
            {
                free_region( visible );
                return 0;
            }
            offset_region( visible, win->window_rect.left - win->client_rect.left,
                            win->window_rect.top - win->client_rect.top );
            rects = get_region_rectangles( visible, &layer.visible_count );
        }
        if (layer.visible_count > (UINT_MAX - sizeof(layer)) / sizeof(*rects))
        {
            if (visible) free_region( visible );
            set_error( STATUS_INTEGER_OVERFLOW );
            return 0;
        }
        clip_offset = sizeof(layer) + (size_t)layer.visible_count * sizeof(*rects);
        if (clip_offset <= max_size && *total <= max_size - clip_offset)
        {
            if (layer.visible_count) memcpy( data + *total + sizeof(layer), rects,
                                               layer.visible_count * sizeof(*rects) );
            clips = (struct client_surface_clip_window *)(data + *total + clip_offset);
            clip_max = (max_size - *total - clip_offset) / sizeof(*clips);
        }
        if (visible) free_region( visible );
        /* The native monitor transform belongs to the owner target. Keep the
         * exact occluders here; the owner intersects them with the transformed
         * source extent without querying mutable window geometry again. */
        if (layer.producer.visible && !collect_client_surface_clips( win, top, layer.raw_dpi,
                NULL, clips, clip_max, &layer.clip_count )) return 0;
        if (layer.clip_count > (UINT_MAX - clip_offset) / sizeof(*clips))
        {
            set_error( STATUS_INTEGER_OVERFLOW );
            return 0;
        }
        block_size = clip_offset + (size_t)layer.clip_count * sizeof(*clips);
        if (block_size > UINT_MAX - *total || *count == UINT_MAX)
        {
            set_error( STATUS_INTEGER_OVERFLOW );
            return 0;
        }
        if (block_size <= max_size && *total <= max_size - block_size)
            memcpy( data + *total, &layer, sizeof(layer) );
        *total += block_size;
        ++*count;
    }
    LIST_FOR_EACH_ENTRY( child, &win->children, struct window, entry )
        if (!collect_client_surface_scene_snapshot( child, top, data, max_size, total, count )) return 0;
    return 1;
}

DECL_HANDLER(get_client_surface_scene_snapshot)
{
    struct window *top = get_window( req->handle );
    unsigned int max_size = get_reply_max_size(), total = 0, count = 0;
    unsigned char *data = NULL;

    if (!top) return;
    if (get_toplevel_window( top ) != top || !top->thread || top->thread->process != current->process)
    {
        set_error( STATUS_ACCESS_DENIED );
        return;
    }
    reply->scene_id = top->client_surface_scene_generation;
    if ((reply->scene_id & 1) || (req->scene_id && req->scene_id != reply->scene_id))
    {
        set_error( STATUS_RETRY );
        return;
    }
    if (max_size && !(data = mem_alloc( max_size ))) return;
    if (!collect_client_surface_scene_snapshot( top, top, data, max_size, &total, &count )) goto failed;
    reply->total_size = total;
    reply->count = count;
    if (total > max_size)
    {
        set_error( STATUS_BUFFER_OVERFLOW );
        goto failed;
    }
    if (data) set_reply_data_ptr( data, total );
    return;
failed:
    free( data );
}

/* Prepare a final scene for the owner's native attachment. A strategy-only
 * change reuses acknowledged geometry. Renewing a previous DIRECT attachment
 * instead requires the owner to check its current native geometry first. */
DECL_HANDLER(prepare_client_surface_direct_plan)
{
    struct client_surface_owner *owner;
    struct client_surface_ref *surface;
    struct window *top = get_window( req->handle );

    reply->scene_id = 0;
    if (!top) return;
    if (get_toplevel_window( top ) != top || !top->thread || top->thread->process != current->process)
    {
        set_error( STATUS_ACCESS_DENIED );
        return;
    }
    if (!req->scene_id || (req->scene_id & 1) || req->scene_id != top->client_surface_scene_generation ||
        top->client_surface_transaction.staged || top->client_surface_transaction.restarting ||
        top->client_surface_transaction.restart_pending || top->client_surface_native_barrier ||
        top->client_surface_scene_change_depth || client_surface_direct_eligible( top ) ||
        !client_surface_direct_candidate( top ) ||
        !(surface = get_client_surface_native_candidate( top, &owner )) || surface->id != req->surface ||
        owner->process != current->process || !surface->scene_publication || surface->generation)
        return;

    if (req->previous_scene)
    {
        if (!client_surface_is_preparing( top ) || req->previous_scene == req->scene_id ||
            req->previous_scene != top->client_surface_ack_scene ||
            req->previous_scene != top->client_surface_direct_scene ||
            req->surface != top->client_surface_direct_surface)
            return;
    }
    else if (top->client_surface_transaction.phase != CLIENT_SURFACE_PHASE_IDLE ||
             req->scene_id != top->client_surface_ack_scene)
        return;

    /* A retained native attachment needs no owner copy of GDI/background
     * pixels. Select it atomically with the new final scene; a failed renewal
     * must never leave an unprepared scene available to ordinary composition.
     * This authorizes a strategy only, not a completed new-size image. */
    top->client_surface_transaction.prepared = 1;
    restart_client_surface_generation_internal( top );
    top->client_surface_transaction.prepared = 0;
    if (top->client_surface_transaction.phase == CLIENT_SURFACE_PHASE_COMPOSING &&
        top->client_surface_scene_generation != req->scene_id &&
        top->client_surface_transaction.epoch == top->client_surface_scene_generation &&
        top->client_surface_transaction.pending == 1 && client_surface_direct_candidate( top ) &&
        (surface = get_client_surface_native_candidate( top, &owner )) && surface->id == req->surface &&
        surface->generation == top->client_surface_scene_generation)
    {
        if (req->previous_scene)
        {
            top->client_surface_direct_scene = top->client_surface_scene_generation;
            top->client_surface_direct_surface = req->surface;
            top->client_surface_transaction.source_pending = 0;
            top->client_surface_transaction.owner_repair = 0;
            update_client_surface_publication( top );
        }
        reply->scene_id = top->client_surface_scene_generation;
    }
    else if (req->previous_scene)
        restart_client_surface_generation( top );
}

/* The candidate flag is only input to the owner's planner. Native attachment
 * becomes an authorized strategy after it accepts this exact final scene. */
DECL_HANDLER(select_client_surface_direct_plan)
{
    struct client_surface_owner *owner;
    struct client_surface_ref *surface;
    struct window *top = get_window( req->handle );

    reply->accepted = 0;
    if (!top) return;
    if (get_toplevel_window( top ) != top || !top->thread || top->thread->process != current->process)
    {
        set_error( STATUS_ACCESS_DENIED );
        return;
    }
    if (top->client_surface_transaction.phase != CLIENT_SURFACE_PHASE_COMPOSING ||
        req->scene_id != top->client_surface_scene_generation || (req->scene_id & 1) ||
        req->scene_id != top->client_surface_transaction.epoch ||
        top->client_surface_transaction.staged || top->client_surface_native_barrier ||
        !client_surface_direct_candidate( top ) ||
        !(surface = get_client_surface_native_candidate( top, &owner )) || surface->id != req->surface ||
        !surface->scene_publication || surface->generation != req->scene_id ||
        top->client_surface_transaction.pending != 1)
        return;

    top->client_surface_direct_scene = req->scene_id;
    top->client_surface_direct_surface = req->surface;
    top->client_surface_transaction.source_pending = 0;
    top->client_surface_transaction.owner_repair = 0;
    update_client_surface_publication( top );
    reply->accepted = 1;
}

DECL_HANDLER(complete_client_surface_direct_plan)
{
    struct client_surface_owner *owner;
    struct client_surface_ref *surface;
    struct window *top = get_window( req->handle );

    reply->accepted = 0;
    if (!top) return;
    if (get_toplevel_window( top ) != top || !top->thread || top->thread->process != current->process)
    {
        set_error( STATUS_ACCESS_DENIED );
        return;
    }
    if (!client_surface_direct_eligible( top ) ||
        top->client_surface_transaction.phase != CLIENT_SURFACE_PHASE_COMPOSING ||
        req->scene_id != top->client_surface_direct_scene || req->surface != top->client_surface_direct_surface ||
        !(surface = select_client_surface_producer( top, &owner )) || surface->id != req->surface ||
        !complete_client_surface_generation( top, surface, req->scene_id, 0 ))
        return;

    /* The same reservation and ACK protect both owner copy and attachment.
     * Only the native presentation which supplies the image proof differs. */
    top->client_surface_transaction.phase = CLIENT_SURFACE_PHASE_PUBLISHING;
    top->client_surface_transaction.publication = CLIENT_SURFACE_PUBLICATION_HANDOFF;
    update_client_surface_publication( top );
    reply->accepted = 1;
}

DECL_HANDLER(set_window_present_rect)
{
    struct window *win = get_window( req->handle ), *top;
    struct rectangle rect = req->rect;

    if (!win) return;
    if (!win->thread || win->thread->process != current->process)
    {
        set_error( STATUS_ACCESS_DENIED );
        return;
    }
    if (!req->dpi.num || !req->dpi.den)
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    map_dpi_rect( win, &rect, req->dpi, get_window_dpi( win ) );
    if (is_rect_empty( &rect )) rect = empty_rect;
    if (!memcmp( &rect, &win->present_rect, sizeof(rect) )) return;
    top = get_toplevel_window( win );
    begin_client_surface_scene_change( top );
    win->present_rect = rect;
    end_client_surface_scene_change( top );
}


/* Reserve a lifetime before the client publishes its notification index. */
DECL_HANDLER(allocate_client_surface)
{
    struct client_surface_ref *surface;

    if (client_surface_id == ~(UINT64)0)
    {
        set_error( STATUS_TOO_MANY_CONTEXT_IDS );
        return;
    }
    if (!(surface = mem_alloc( sizeof(*surface) ))) return;
    memset( surface, 0, sizeof(*surface) );
    list_init( &surface->entry );
    surface->process = current->process;
    surface->id = ++client_surface_id;
    insert_client_surface_ref_index( surface );
    reply->surface = surface->id;
}

DECL_HANDLER(release_client_surface)
{
    struct client_surface_ref *surface = find_indexed_client_surface_ref( current->process, req->surface );

    if (!surface) set_error( STATUS_INVALID_PARAMETER );
    else if (surface->owner) set_error( STATUS_DEVICE_BUSY );
    else free_client_surface_ref_if_unused( surface );
}

/* A barrier belongs to the requested native owner even after reparenting. */
DECL_HANDLER(set_client_surface_native_barrier)
{
    struct window *win;

    if (!(win = get_window( req->handle ))) return;
    if (!req->token || (req->begin != 0 && req->begin != 1) ||
        !win->thread || win->thread->process != current->process)
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    if (req->begin)
    {
        if (!win->client_surface_native_barrier)
        {
            win->client_surface_native_barrier = req->token;
            begin_client_surface_scene_change( win );
        }
        else if (win->client_surface_native_barrier != req->token)
        {
            set_error( STATUS_DEVICE_BUSY );
            return;
        }
    }
    else
    {
        if (win->client_surface_native_barrier != req->token)
        {
            set_error( STATUS_INVALID_PARAMETER );
            return;
        }
        win->client_surface_native_barrier = 0;
        end_client_surface_scene_change( win );
    }
    reply->generation = client_surface_transaction_generation( win );
    reply->scene_generation = win->client_surface_scene_generation;
}

/* Track client-rendered content across process boundaries.  A presentation
 * made while an ancestor is hidden invalidates the top-level composition.
 * Every visible surface must commit the same scene generation before the
 * owner is allowed to publish the staged host window. */
DECL_HANDLER(set_client_surface_state)
{
    struct client_surface_owner *owner, *selected_owner;
    struct client_surface_ref *surface, *selected_before, *selected_after, *producer_before;
    struct window *win, *top;
    unsigned long long producer_sequence_before;
    unsigned int selected_caps_before;
    int scene_change, was_pending, direct_before;

    reply->toplevel = 0;
    reply->wake = 0;
    reply->generation = 0;
    reply->scene_generation = 0;
    reply->pending = 0;
    reply->staged = 0;
    reply->ready = 0;
    reply->publish = 0;
    reply->compose = 0;
    reply->mode = CLIENT_SURFACE_PRESENTATION_INVALID;
    reply->active = 0;
    reply->cached = 0;
    if (!(win = get_window( req->handle ))) return;
    top = get_toplevel_window( win );
    was_pending = top->client_surface_dirty;
    if (req->flags & (CLIENT_SURFACE_STATE_STAGED | CLIENT_SURFACE_STATE_FAILED |
                      CLIENT_SURFACE_STATE_PREPARE_COMMIT))
    {
        if ((req->flags != CLIENT_SURFACE_STATE_STAGED && req->flags != CLIENT_SURFACE_STATE_FAILED &&
             req->flags != CLIENT_SURFACE_STATE_PREPARE_COMMIT) ||
            req->surface)
        {
            set_error( STATUS_INVALID_PARAMETER );
            return;
        }
        if (!top->thread || top->thread->process != current->process ||
            (req->flags == CLIENT_SURFACE_STATE_PREPARE_COMMIT && top->thread != current))
        {
            set_error( STATUS_ACCESS_DENIED );
            return;
        }
        /* The native result belongs to the scene captured before the call.
         * PREPARING has a stable scene but no composition generation yet.
         * A reparented owner or superseded scene must remain untouched. */
        if (win != top || req->scene_toplevel != top->handle ||
            !req->scene_generation || (req->scene_generation & 1) ||
            req->scene_generation != top->client_surface_scene_generation ||
            req->generation != client_surface_transaction_generation( top ))
            return;

        if (req->flags == CLIENT_SURFACE_STATE_PREPARE_COMMIT && !client_surface_is_preparing( top )) return;
        if (req->producer_sequence != top->paint_serial)
        {
            top->paint_waiting = 1;
            wake_window_paint_prepare( top );
            return;
        }
        if (req->flags != CLIENT_SURFACE_STATE_FAILED)
        {
            if (window_paint_failed( top ))
            {
                fail_client_surface_publication( top );
                set_error( STATUS_UNSUCCESSFUL );
                return;
            }
            if (window_paint_pending( top ))
            {
                top->paint_waiting = 1;
                return;
            }
        }
        top->paint_waiting = 0;
        if (req->flags == CLIENT_SURFACE_STATE_PREPARE_COMMIT)
        {
            top->client_surface_transaction.phase = CLIENT_SURFACE_PHASE_IDLE;
            top->client_surface_transaction.prepared = 1;
            restart_client_surface_generation_internal( top );
        }
        else if (req->flags == CLIENT_SURFACE_STATE_FAILED)
            fail_client_surface_publication( top );
        else
        {
            /* A show transition starts a new staged episode. Retire a live
             * replay first, preserving the deadline across staged restarts.
             * Accepting this result legitimately advances the scene epoch. */
            if (!top->client_surface_transaction.staged && client_surface_is_composing( top ))
            {
                clear_client_surface_subtree_generation( top, client_surface_transaction_generation( top ) );
                finish_client_surface_generation( top );
            }
            if (client_surface_is_preparing( top )) top->client_surface_transaction.phase = CLIENT_SURFACE_PHASE_IDLE;
            top->client_surface_transaction.prepared = 0;
            top->client_surface_transaction.staged = top->client_surface_dirty && is_visible( top );
            /* Producers may already have complete frames from while hidden.
             * Restart notifies their owners to recompose those cached frames. */
            if (top->client_surface_transaction.staged)
                restart_client_surface_generation( top );
            else
                finish_client_surface_publication( top );
        }
        goto done;
    }
    owner = get_client_surface_owner( win, current->process,
                                      req->flags & (CLIENT_SURFACE_STATE_REGISTER |
                                                    CLIENT_SURFACE_STATE_CACHE) );
    if ((req->flags & (CLIENT_SURFACE_STATE_REGISTER | CLIENT_SURFACE_STATE_CACHE)) && !owner)
        return;
    surface = get_client_surface_ref( owner, req->surface,
                                      req->flags & (CLIENT_SURFACE_STATE_REGISTER |
                                                    CLIENT_SURFACE_STATE_CACHE) );
    if ((req->flags & (CLIENT_SURFACE_STATE_REGISTER | CLIENT_SURFACE_STATE_CACHE)) && !surface)
    {
        release_client_surface_owner( owner );
        return;
    }
    if (req->surface && !surface)
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    producer_before = select_client_surface_producer( win, &selected_owner );
    producer_sequence_before = producer_before ? producer_before->sequence : 0;
    selected_before = select_client_surface_scene_producer( win, &selected_owner );
    selected_caps_before = selected_before ? get_client_surface_backend_caps( selected_before ) : 0;
    direct_before = client_surface_direct_candidate( top );

    if ((req->flags & (CLIENT_SURFACE_STATE_REGISTER | CLIENT_SURFACE_STATE_CACHE |
                       CLIENT_SURFACE_STATE_UPDATE_CAPS)) && surface)
    {
        surface->scene_publication = !!(req->flags & CLIENT_SURFACE_STATE_SCENE_PUBLICATION);
        surface->direct_presentation = !!(req->flags & CLIENT_SURFACE_STATE_DIRECT_PRESENTATION);
    }

    if ((req->flags & CLIENT_SURFACE_STATE_CACHE) && !surface->cached)
    {
        surface->cached = 1;
        if (!++client_surface_ref_sequence) ++client_surface_ref_sequence;
        surface->sequence = client_surface_ref_sequence;
        win->client_surface_cached_count++;
        adjust_client_surface_subtree_count( win, 1 );
    }
    if ((req->flags & CLIENT_SURFACE_STATE_UNCACHE) && surface && surface->cached)
    {
        surface->cached = 0;
        assert( win->client_surface_cached_count );
        win->client_surface_cached_count--;
        adjust_client_surface_subtree_count( win, -1 );
    }
    if ((req->flags & CLIENT_SURFACE_STATE_REGISTER) && !surface->active)
    {
        surface->active = 1;
        win->client_surface_count++;
        adjust_client_surface_subtree_count( win, 1 );
        if (!is_visible( top )) top->client_surface_dirty = 1;
    }
    if ((req->flags & CLIENT_SURFACE_STATE_UNREGISTER) && surface && surface->active)
    {
        surface->active = 0;
        surface->candidate_serial = 0;
        assert( win->client_surface_count );
        win->client_surface_count--;
        adjust_client_surface_subtree_count( win, -1 );
    }
    if ((req->flags & CLIENT_SURFACE_STATE_NATIVE_CANDIDATE) && surface && surface->active && !surface->claimed &&
        req->generation > surface->candidate_serial)
        surface->candidate_serial = req->generation;
    /* Cancellation identifies its own preparation. It neither rolls back a
     * completed producer nor erases a later preparation on the same surface,
     * including after an owner scene restart or reparent. */
    if ((req->flags & CLIENT_SURFACE_STATE_CANCEL_CANDIDATE) && surface && !surface->claimed &&
        req->generation == surface->candidate_serial)
        surface->candidate_serial = 0;
    if ((req->flags & CLIENT_SURFACE_STATE_CLAIM) && surface && surface->active &&
        req->scene_toplevel == top->handle && !(req->scene_generation & 1) &&
        req->producer_sequence == (producer_before ? producer_before->sequence : 0) &&
        req->scene_generation <= top->client_surface_scene_generation &&
        (!surface->claimed || select_client_surface_producer( win, &selected_owner ) != surface))
    {
        surface->claimed = 1;
        surface->candidate_serial = 0;
        if (!++client_surface_ref_sequence) ++client_surface_ref_sequence;
        surface->sequence = client_surface_ref_sequence;
    }
    selected_after = select_client_surface_scene_producer( win, &selected_owner );
    scene_change = selected_before != selected_after ||
                   (selected_after && selected_caps_before !=
                    get_client_surface_backend_caps( selected_after )) ||
                   direct_before != client_surface_direct_candidate( top );
    if (scene_change)
    {
        /* Membership is private server state.  Publish the changed producer
         * only after the scene seqlock turns odd. Dormant registrations need
         * no replay unless they change the sole DIRECT candidate's eligibility:
         * the owner must choose that strategy from a new immutable scene. */
        begin_client_surface_scene_change( top );
        update_client_surface_producer( win );
    }
    else if (producer_before != select_client_surface_producer( win, &selected_owner ) ||
             (producer_before && producer_sequence_before != producer_before->sequence))
    {
        /* Completing the already admitted sole DIRECT candidate changes
         * producer authority, not the scene's chosen identity or native
         * target. Preserve that exact plan for its completion receipt. */
        update_client_surface_producer( win );
        /* The same candidate may instead complete after owner preparation
         * failed. Its first usable image is new work for that unpublished
         * scene, even though the selected identity did not change. Resume
         * owner admission from this completion, without restarting an active
         * DIRECT plan or requiring the producer to draw another frame. */
        if ((req->flags & CLIENT_SURFACE_STATE_CLAIM) &&
            top->client_surface_transaction.phase == CLIENT_SURFACE_PHASE_IDLE &&
            top->client_surface_ack_scene != top->client_surface_scene_generation &&
            client_surface_scene_published( top ))
            restart_client_surface_generation( top );
    }
    if (surface && !surface->active && !surface->cached)
    {
        complete_client_surface_generation( top, surface,
                                            client_surface_transaction_generation( top ), 1 );
        retire_client_surface_ref( surface );
        surface = NULL;
    }
    if ((req->flags & (CLIENT_SURFACE_STATE_UNREGISTER | CLIENT_SURFACE_STATE_UNCACHE)) &&
        !has_client_surface( top ))
        finish_client_surface_publication( top );
    if (owner) release_client_surface_owner( owner );
    if (req->flags & CLIENT_SURFACE_STATE_BYPASS)
        finish_client_surface_publication( top );
    if ((req->flags & CLIENT_SURFACE_STATE_PREPARE_BEGIN) && win == top && top->thread == current &&
        client_surface_is_preparing( top ) && top->client_surface_scene_generation &&
        !(top->client_surface_scene_generation & 1))
    {
        if (window_paint_failed( top ))
        {
            fail_client_surface_publication( top );
            set_error( STATUS_UNSUCCESSFUL );
        }
        else if (window_paint_pending( top ))
        {
            top->paint_waiting = 1;
            set_error( STATUS_PENDING );
        }
        else
        {
            if (get_reply_max_size() < sizeof(top->paint_serial))
            {
                set_error( STATUS_BUFFER_TOO_SMALL );
                return;
            }
            set_reply_data( &top->paint_serial, sizeof(top->paint_serial) );
            if (get_error()) return;
            top->paint_waiting = 0;
            reply->publish = 1;
        }
    }
    if (req->flags & CLIENT_SURFACE_STATE_GEOMETRY_READY)
    {
        /* Source recovery and backends without a complete owner cache still
         * require the producer to update its target and republish content. */
        top->client_surface_transaction.owner_repair = 0;
        top->client_surface_transaction.source_pending = 0;
        if (!client_surface_is_composing( top ) && is_visible( top ) && has_client_surface( top ))
            restart_client_surface_generation( top );
        else if (client_surface_is_composing( top ) && top->client_surface_transaction.pending)
        {
            update_client_surface_publication( top );
            notify_client_surface_geometry_ready( top );
        }
    }
    if ((req->flags & CLIENT_SURFACE_STATE_PRESENT_BEGIN) && surface &&
        !top->client_surface_transaction.restart_pending &&
        !(top->client_surface_scene_generation & 1) &&
        req->scene_generation == top->client_surface_scene_generation &&
        is_visible( win ) && select_client_surface_producer( win, &selected_owner ) == surface)
    {
        int compose = 0;

        if (client_surface_is_composing( top ) &&
            req->generation == client_surface_transaction_generation( top ))
        {
            if (surface->generation == req->generation ||
                reopen_client_surface_generation( top, win, surface, req->generation ))
                compose = 1;
            else if (client_surface_is_publishing( top ))
            {
                /* Host exposure has already crossed PUBLISH_BEGIN.  Preserve
                 * this source frame and repair the visible host after ACK. */
                invalidate_client_surface_scene( top );
            }
        }
        else if (surface->scene_publication && !req->generation &&
                 !top->client_surface_transaction.staged && !client_surface_is_preparing( top ) &&
                 !client_surface_is_publishing( top ))
            compose = 1;

        reply->compose = compose;
    }
    if ((req->flags & CLIENT_SURFACE_STATE_PRESENT_COMMIT) && surface &&
        (surface->active || surface->cached) && is_visible( win ) &&
        !client_surface_direct_eligible( top ))
    {
        if (req->scene_generation == top->client_surface_scene_generation)
            complete_client_surface_generation( top, surface, req->generation, 1 );
    }

    if (scene_change) end_client_surface_scene_change( top );
    /* Copy completion and host exposure are separate transactions for both
     * hidden-to-visible staging and live scene repair.  Scene changes in
     * between invalidate the image but cannot strand the publication; a
     * freshly prepared live generation follows the ACK. */
    if ((req->flags & CLIENT_SURFACE_STATE_PUBLISH_BEGIN) && top->thread == current &&
        client_surface_is_publishing( top ) &&
        top->client_surface_transaction.publication == CLIENT_SURFACE_PUBLICATION_EXPOSURE_READY &&
        top->client_surface_transaction.epoch != top->client_surface_scene_generation)
    {
        retire_stale_client_surface_exposure( top, 0 );
    }
    else if ((req->flags & CLIENT_SURFACE_STATE_PUBLISH_BEGIN) && top->thread == current &&
        (top->client_surface_transaction.phase == CLIENT_SURFACE_PHASE_READY ||
         (client_surface_is_publishing( top ) && top->client_surface_transaction.publication ==
                                                CLIENT_SURFACE_PUBLICATION_EXPOSURE_READY)) &&
        !(top->client_surface_scene_generation & 1) &&
        top->client_surface_transaction.epoch == top->client_surface_scene_generation)
    {
        top->client_surface_transaction.phase = CLIENT_SURFACE_PHASE_PUBLISHING;
        if (top->client_surface_transaction.publication == CLIENT_SURFACE_PUBLICATION_EXPOSURE_READY)
            top->client_surface_transaction.publication = CLIENT_SURFACE_PUBLICATION_EXPOSE;
        update_client_surface_publication( top );
        reply->publish = top->client_surface_transaction.publication;
    }
    if ((req->flags & CLIENT_SURFACE_STATE_PUBLISH_COMMIT) && top->thread == current &&
        client_surface_is_publishing( top ) &&
        (top->client_surface_transaction.publication == CLIENT_SURFACE_PUBLICATION_COPY ||
         top->client_surface_transaction.publication == CLIENT_SURFACE_PUBLICATION_EXPOSE) &&
        req->generation == client_surface_transaction_generation( top ) &&
        req->scene_generation == top->client_surface_transaction.epoch)
    {
        int invalidated = top->client_surface_transaction.epoch !=
                          top->client_surface_scene_generation;

        if (invalidated && top->client_surface_transaction.publication == CLIENT_SURFACE_PUBLICATION_EXPOSE)
            retire_stale_client_surface_exposure( top, 1 );
        else
        {
            if (!invalidated) top->client_surface_ack_scene = req->scene_generation;
            finish_client_surface_publication( top );
            if (invalidated && is_visible( top ) && has_client_surface( top ))
                restart_client_surface_generation( top );
        }
        reply->publish = 1;
    }

done:
    reply->toplevel = top->handle;
    reply->wake = req->flags != CLIENT_SURFACE_STATE_FAILED &&
                  was_pending && is_visible( top ) && !top->client_surface_dirty;
    reply->generation = client_surface_transaction_generation( top );
    reply->scene_generation = top->client_surface_scene_generation;
    reply->pending = top->client_surface_transaction.pending;
    reply->staged = top->client_surface_transaction.staged;
    reply->ready = client_surface_is_ready( top );
    reply->mode = client_surface_presentation_mode( top );
    reply->active = win->client_surface_count;
    reply->cached = win->client_surface_cached_count;
}


/* get the window and client rectangles of a window */
DECL_HANDLER(get_window_rectangles)
{
    struct window *win = get_window( req->handle );

    if (!win) return;

    reply->window  = win->window_rect;
    reply->client  = win->client_rect;
    reply->visible  = win->visible_rect;

    switch (req->relative)
    {
    case COORDS_CLIENT:
        offset_rect( &reply->window, -win->client_rect.left, -win->client_rect.top );
        offset_rect( &reply->client, -win->client_rect.left, -win->client_rect.top );
        offset_rect( &reply->visible, -win->client_rect.left, -win->client_rect.top );
        if (win->ex_style & WS_EX_LAYOUTRTL)
        {
            mirror_rect( &win->client_rect, &reply->window );
            mirror_rect( &win->client_rect, &reply->visible );
        }
        break;
    case COORDS_WINDOW:
        offset_rect( &reply->window, -win->window_rect.left, -win->window_rect.top );
        offset_rect( &reply->client, -win->window_rect.left, -win->window_rect.top );
        if (win->ex_style & WS_EX_LAYOUTRTL)
        {
            mirror_rect( &win->window_rect, &reply->client );
            mirror_rect( &win->window_rect, &reply->visible );
        }
        break;
    case COORDS_PARENT:
        if (win->parent && win->parent->ex_style & WS_EX_LAYOUTRTL)
        {
            mirror_rect( &win->parent->client_rect, &reply->window );
            mirror_rect( &win->parent->client_rect, &reply->client );
            mirror_rect( &win->parent->client_rect, &reply->visible );
        }
        break;
    case COORDS_SCREEN:
        client_to_screen_rect( win->parent, &reply->window );
        client_to_screen_rect( win->parent, &reply->client );
        client_to_screen_rect( win->parent, &reply->visible );
        break;
    default:
        set_error( STATUS_INVALID_PARAMETER );
        break;
    }
    map_dpi_rect( win, &reply->window, get_window_dpi( win ), req->dpi );
    map_dpi_rect( win, &reply->client, get_window_dpi( win ), req->dpi );
    map_dpi_rect( win, &reply->visible, get_window_dpi( win ), req->dpi );
}


/* get the window text */
DECL_HANDLER(get_window_text)
{
    struct window *win = get_window( req->handle );

    if (win && win->text_len)
    {
        reply->length = win->text_len / sizeof(WCHAR);
        set_reply_data( win->text, min( win->text_len, get_reply_max_size() ));
    }
}


/* set the window text */
DECL_HANDLER(set_window_text)
{
    data_size_t len;
    WCHAR *text = NULL;
    struct window *win = get_window( req->handle );

    if (!win) return;
    len = (get_req_data_size() / sizeof(WCHAR)) * sizeof(WCHAR);
    if (len && !(text = memdup( get_req_data(), len ))) return;
    free( win->text );
    win->text = text;
    win->text_len = len;
}


/* get the coordinates offset between two windows */
DECL_HANDLER(get_windows_offset)
{
    struct window *win;
    int x, y, mirror_from = 0, mirror_to = 0;

    reply->x = reply->y = 0;
    if (req->from)
    {
        if (!(win = get_window( req->from ))) return;
        if (win->ex_style & WS_EX_LAYOUTRTL) mirror_from = 1;
        x = mirror_from ? win->client_rect.right - win->client_rect.left : 0;
        y = 0;
        client_to_screen( win, &x, &y );
        map_dpi_point( win, &x, &y, get_window_dpi( win ), req->dpi );
        reply->x += x;
        reply->y += y;
    }
    if (req->to)
    {
        if (!(win = get_window( req->to ))) return;
        if (win->ex_style & WS_EX_LAYOUTRTL) mirror_to = 1;
        x = mirror_to ? win->client_rect.right - win->client_rect.left : 0;
        y = 0;
        client_to_screen( win, &x, &y );
        map_dpi_point( win, &x, &y, get_window_dpi( win ), req->dpi );
        reply->x -= x;
        reply->y -= y;
    }
    if (mirror_from) reply->x = -reply->x;
    reply->mirror = mirror_from ^ mirror_to;
}


/* get the visible region of a window */
DECL_HANDLER(get_visible_region)
{
    struct region *region;
    struct window *top, *win = get_window( req->window );

    if (!win) return;

    top = get_top_clipping_window( win );
    if ((region = get_visible_region( win, req->flags )))
    {
        struct rectangle *data;
        map_win_region_to_screen( win, region );
        data = get_region_data_and_free( region, get_reply_max_size(), &reply->total_size );
        if (data) set_reply_data_ptr( data, reply->total_size );
    }
    reply->top_win  = top->handle;
    reply->top_rect = top->surface_rect;

    if (!is_desktop_window(win))
    {
        reply->win_rect = (req->flags & DCX_WINDOW) ? win->window_rect : win->client_rect;
        client_to_screen_rect( top->parent, &reply->top_rect );
        client_to_screen_rect( win->parent, &reply->win_rect );
    }
    else
    {
        reply->win_rect.left   = 0;
        reply->win_rect.top    = 0;
        reply->win_rect.right  = win->client_rect.right - win->client_rect.left;
        reply->win_rect.bottom = win->client_rect.bottom - win->client_rect.top;
    }
    reply->paint_flags = win->paint_flags & PAINT_CLIENT_FLAGS;
}


/* get the window regions */
DECL_HANDLER(get_window_region)
{
    struct rectangle *data;
    struct region *region;
    struct window *win = get_window( req->window );

    if (!win) return;

    reply->visible_rect = win->visible_rect;
    if (req->surface)
    {
        if (!is_visible( win )) return;

        if ((region = get_surface_region( win )))
        {
            struct rectangle *data = get_region_data_and_free( region, get_reply_max_size(), &reply->total_size );
            if (data) set_reply_data_ptr( data, reply->total_size );
        }
        return;
    }

    if (!win->win_region) return;

    if (win->ex_style & WS_EX_LAYOUTRTL)
    {
        struct region *region = create_empty_region();

        if (!region) return;
        if (!copy_region( region, win->win_region ))
        {
            free_region( region );
            return;
        }
        mirror_region( &win->window_rect, region );
        data = get_region_data_and_free( region, get_reply_max_size(), &reply->total_size );
    }
    else data = get_region_data( win->win_region, get_reply_max_size(), &reply->total_size );

    if (data) set_reply_data_ptr( data, reply->total_size );
}


/* set the window region */
DECL_HANDLER(set_window_region)
{
    struct region *region = NULL;
    struct window *win = get_window( req->window );

    if (!win) return;

    if (get_req_data_size())  /* no data means remove the region completely */
    {
        if (!(region = create_region_from_req_data( get_req_data(), get_req_data_size() )))
            return;
        if (win->ex_style & WS_EX_LAYOUTRTL) mirror_region( &win->window_rect, region );
    }
    set_window_region( win, region, req->redraw );
}


/* get a window update region */
DECL_HANDLER(get_update_region)
{
    struct rectangle *data;
    unsigned int flags = req->flags;
    struct window *from_child = NULL;
    struct window *win = get_window( req->window );

    reply->flags = 0;
    if (!win) return;

    if (req->from_child)
    {
        struct window *ptr;

        if (!(from_child = get_window( req->from_child ))) return;

        /* make sure from_child is a child of win */
        ptr = from_child;
        while (ptr && ptr != win) ptr = ptr->parent;
        if (!ptr)
        {
            set_error( STATUS_INVALID_PARAMETER );
            return;
        }
    }

    if (flags & UPDATE_DELAYED_ERASE)  /* this means that the previous call didn't erase */
    {
        if (from_child) from_child->paint_flags |= PAINT_DELAYED_ERASE;
        else win->paint_flags |= PAINT_DELAYED_ERASE;
    }

    reply->flags = get_window_update_flags( win, from_child, flags, &win );
    reply->child = win->handle;

    if (flags & UPDATE_NOREGION) return;

    if (win->update_region)
    {
        /* convert update region to screen coordinates */
        struct region *region = create_empty_region();

        if (!region) return;
        if (!copy_region( region, win->update_region ))
        {
            free_region( region );
            return;
        }
        if ((flags & UPDATE_CLIPCHILDREN) && (win->style & WS_CLIPCHILDREN))
            clip_children( win, NULL, region, win->client_rect.left - win->window_rect.left,
                           win->client_rect.top - win->window_rect.top );
        map_win_region_to_screen( win, region );
        if (!(data = get_region_data_and_free( region, get_reply_max_size(),
                                               &reply->total_size ))) return;
        set_reply_data_ptr( data, reply->total_size );
    }

    if (reply->flags & (UPDATE_PAINT | UPDATE_INTERNALPAINT | UPDATE_NONCLIENT | UPDATE_ERASE))
        if (!record_window_paint_region( win )) return;

    if (reply->flags & (UPDATE_PAINT|UPDATE_INTERNALPAINT)) /* validate everything */
    {
        validate_parents( win );
        validate_whole_window( win );
    }
    else
    {
        if (reply->flags & UPDATE_NONCLIENT) validate_non_client( win );
        if (reply->flags & UPDATE_ERASE)
        {
            win->paint_flags &= ~(PAINT_ERASE | PAINT_DELAYED_ERASE);
            /* desktop window only gets erased, not repainted */
            if (is_desktop_window(win)) validate_whole_window( win );
        }
    }
}


/* update the z order of a window so that a given rectangle is fully visible */
void set_window_rect_visible( user_handle_t window, struct rectangle rect )
{
    struct window *ptr, *win;
    struct rectangle tmp;

    if (!(win = get_window( window )) || !win->parent || !is_visible( win )) return;  /* nothing to do */

    map_point_raw_to_virt( win->desktop, &rect.left, &rect.top );
    map_point_raw_to_virt( win->desktop, &rect.right, &rect.bottom );
    rect.right = max( rect.left + 1, rect.right );
    rect.bottom = max( rect.top + 1, rect.bottom );

    LIST_FOR_EACH_ENTRY( ptr, &win->parent->children, struct window, entry )
    {
        if (ptr == win) break;
        if (!(ptr->style & WS_VISIBLE)) continue;
        if (ptr->ex_style & WS_EX_TRANSPARENT) continue;
        if (ptr->is_layered && (ptr->layered_flags & LWA_COLORKEY)) continue;
        tmp = rect;
        map_dpi_rect( win, &tmp, get_window_dpi( win->parent ), get_window_dpi( win ) );
        if (!intersect_rect( &tmp, &tmp, &ptr->visible_rect )) continue;
        if (ptr->win_region)
        {
            offset_rect( &tmp, -ptr->window_rect.left, -ptr->window_rect.top );
            if (!rect_in_region( ptr->win_region, &tmp )) continue;
        }
        /* found a window obscuring the rectangle, now move win above this one */
        /* making sure to not violate the topmost rule */
        if (!(ptr->ex_style & WS_EX_TOPMOST) || (win->ex_style & WS_EX_TOPMOST))
        {
            list_remove( &win->entry );
            list_add_before( &ptr->entry, &win->entry );
            update_window_first_child( win->parent );
        }
        break;
    }
}

DECL_HANDLER(update_window_zorder)
{
    set_window_rect_visible( req->window, req->rect );
}

/* mark parts of a window as needing a redraw */
DECL_HANDLER(redraw_window)
{
    unsigned int flags = req->flags;
    struct region *region = NULL;
    struct window *win;

    if (!req->window)
    {
        if (!(win = get_desktop_window( current ))) return;
    }
    else
    {
        if (!(win = get_window( req->window ))) return;
        if (is_desktop_window( win )) flags &= ~RDW_ALLCHILDREN;
    }

    if (!is_visible( win )) return;  /* nothing to do */

    if (flags & (RDW_VALIDATE|RDW_INVALIDATE))
    {
        if (get_req_data_size())  /* no data means whole rectangle */
        {
            if (!(region = create_region_from_req_data( get_req_data(), get_req_data_size() )))
                return;
            if (win->ex_style & WS_EX_LAYOUTRTL) mirror_region( &win->client_rect, region );
        }
    }

    redraw_window( win, region, flags, 0 );
    if (region) free_region( region );
}


/* set a window property */
DECL_HANDLER(set_window_property)
{
    struct unicode_str name = get_req_unicode_str();
    struct atom_table *table = get_global_atom_table();
    struct window *win = get_window( req->window );

    if (!win) return;

    if (name.len)
    {
        atom_t atom = add_atom( table, name );
        if (atom)
        {
            set_property( win, atom, req->data, PROP_TYPE_STRING );
            release_atom( table, atom );
        }
    }
    else set_property( win, req->atom, req->data, PROP_TYPE_ATOM );
}


/* remove a window property */
DECL_HANDLER(remove_window_property)
{
    struct unicode_str name = get_req_unicode_str();
    struct atom_table *table = get_global_atom_table();
    struct window *win = get_window( req->window );

    if (win)
    {
        atom_t atom = name.len ? find_atom( table, name ) : req->atom;
        if (atom) reply->data = remove_property( win, atom );
    }
}


/* get a window property */
DECL_HANDLER(get_window_property)
{
    struct unicode_str name = get_req_unicode_str();
    struct atom_table *table = get_global_atom_table();
    struct window *win = get_window( req->window );

    if (win)
    {
        atom_t atom = name.len ? find_atom( table, name ) : req->atom;
        if (atom) reply->data = get_property( win, atom );
    }
}


/* get the list of properties of a window */
DECL_HANDLER(get_window_properties)
{
    struct property_data *data;
    int i, count, max = get_reply_max_size() / sizeof(*data);
    struct window *win = get_window( req->window );

    reply->total = 0;
    if (!win) return;

    for (i = count = 0; i < win->prop_inuse; i++)
        if (win->properties[i].type != PROP_TYPE_FREE) count++;
    reply->total = count;

    if (count > max) count = max;
    if (!count || !(data = set_reply_data_size( count * sizeof(*data) ))) return;

    for (i = 0; i < win->prop_inuse && count; i++)
    {
        if (win->properties[i].type == PROP_TYPE_FREE) continue;
        data->atom   = win->properties[i].atom;
        data->string = (win->properties[i].type == PROP_TYPE_STRING);
        data->data   = win->properties[i].data;
        data++;
        count--;
    }
}


/* get the new window pointer for a desktop shell window, checking permissions */
/* helper for set_desktop_shell_windows request */
static int get_new_shell_window( struct window **win, user_handle_t handle )
{
    if (!handle)
    {
        *win = NULL;
        return 1;
    }
    else if (*win)
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }
    *win = get_window( handle );
    return (*win != NULL);
}

/* Set/get the desktop shell windows */
DECL_HANDLER(set_desktop_shell_windows)
{
    struct desktop *desktop;
    struct window *new_shell_window, *new_shell_listview, *new_progman_window, *new_taskman_window;

    if (!(desktop = get_desktop_obj( current->process, current->desktop, 0 ))) return;

    new_shell_window   = desktop->shell_window;
    new_shell_listview = desktop->shell_listview;
    new_progman_window = desktop->progman_window;
    new_taskman_window = desktop->taskman_window;

    reply->old_shell_window   = new_shell_window ? new_shell_window->handle : 0;
    reply->old_shell_listview = new_shell_listview ? new_shell_listview->handle : 0;
    reply->old_progman_window = new_progman_window ? new_progman_window->handle : 0;
    reply->old_taskman_window = new_taskman_window ? new_taskman_window->handle : 0;

    if (req->flags & SET_DESKTOP_SHELL_WINDOWS)
    {
        if (!get_new_shell_window( &new_shell_window, req->shell_window )) goto done;
        if (!get_new_shell_window( &new_shell_listview, req->shell_listview )) goto done;
    }
    if (req->flags & SET_DESKTOP_PROGMAN_WINDOW)
    {
        if (!get_new_shell_window( &new_progman_window, req->progman_window )) goto done;
    }
    if (req->flags & SET_DESKTOP_TASKMAN_WINDOW)
    {
        if (!get_new_shell_window( &new_taskman_window, req->taskman_window )) goto done;
    }
    desktop->shell_window   = new_shell_window;
    desktop->shell_listview = new_shell_listview;
    desktop->progman_window = new_progman_window;
    desktop->taskman_window = new_taskman_window;

done:
    release_object( desktop );
}

/* retrieve layered info for a window */
DECL_HANDLER(get_window_layered_info)
{
    struct window *win = get_window( req->handle );

    if (!win) return;

    if (win->is_layered)
    {
        reply->color_key = win->color_key;
        reply->alpha     = win->alpha;
        reply->flags     = win->layered_flags;
    }
    else set_win32_error( ERROR_INVALID_WINDOW_HANDLE );
}


/* set layered info for a window */
DECL_HANDLER(set_window_layered_info)
{
    struct window *win = get_window( req->handle );

    if (!win) return;

    if (win->ex_style & WS_EX_LAYERED)
    {
        int was_layered = win->is_layered;

        if (req->flags & LWA_ALPHA) win->alpha = req->alpha;
        else if (!win->is_layered) win->alpha = 0;  /* alpha init value is 0 */

        win->color_key     = req->color_key;
        win->layered_flags = req->flags;
        win->is_layered    = 1;
        /* repaint since we know now it's not going to use UpdateLayeredWindow */
        if (!was_layered) redraw_window( win, 0, RDW_ALLCHILDREN | RDW_INVALIDATE | RDW_ERASE | RDW_FRAME, 0 );
    }
    else set_win32_error( ERROR_INVALID_WINDOW_HANDLE );
}
