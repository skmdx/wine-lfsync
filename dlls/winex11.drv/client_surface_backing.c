/*
 * X11 client surface backing store
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
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>

#ifdef __linux__
#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/eventfd.h>
#endif

#include "x11drv.h"
#include "xcomposite.h"
#include "xpresent.h"
#include "client_surface_xcb.h"
#include "wine/server.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);
WINE_DECLARE_DEBUG_CHANNEL(csperf);

static unsigned long long client_surface_perf_time(void)
{
    LARGE_INTEGER counter;

    NtQueryPerformanceCounter( &counter, NULL );
    return counter.QuadPart;
}

struct client_surface_compositor_pool
{
    struct client_surface_compositor_pool *next;
    struct client_surface_handoff_shared *shared;
    UINT64 id;
    SIZE_T size;
    unsigned int refs;
    unsigned int next_word;
    int ready_fd;
    struct client_surface_compositor_binding *bindings[CLIENT_SURFACE_HANDOFF_CHANNELS];
};

struct client_surface_source_cache
{
    Pixmap pixmap;
    UINT64 target_epoch;
    VisualID visual;
    unsigned int width, height, depth;
};

struct client_surface_cached_image
{
    Pixmap pixmap;
    GC gc;
    UINT64 bytes;
    unsigned int width, height, depth;
};

struct client_surface_compositor_binding
{
    struct client_surface_compositor_binding *next;
    struct client_surface_compositor_pool *pool;
    struct client_surface_handoff_channel *channel;
    struct client_surface_source_cache sources[CLIENT_SURFACE_HANDOFF_RING_SIZE];
    HWND toplevel;
    HWND window;
    process_id_t process;
    UINT64 identity;
    UINT64 cookie;
    UINT64 mark;
    unsigned int scene_index;
    UINT64 source_sequence;
    UINT64 source_epoch;
    struct client_surface_cached_image latest_image, spare_image;
    struct client_surface_handoff_slot latest_frame;
    UINT64 latest_control;
    UINT64 replay_epoch;
    unsigned int latest_index;
};

static void trace_client_surface_source( const char *event,
    const struct client_surface_compositor_binding *binding, UINT64 control, UINT64 sequence,
    Window window, Pixmap pixmap, BOOL success )
{
    TRACE_(csperf)( "ticks=%llu event=%s identity=%s cookie=%s token=%s sequence=%s "
                   "window=%lx pixmap=%lx success=%u\n", client_surface_perf_time(), event,
                   wine_dbgstr_longlong( binding->identity ), wine_dbgstr_longlong( binding->cookie ),
                   wine_dbgstr_longlong( control ),
                   wine_dbgstr_longlong( sequence ), window, pixmap, success );
}

struct client_surface_scene_layout
{
    HWND window;
    process_id_t process;
    UINT64 identity;
    struct client_surface_target geometry;
    RGNDATA *clip;
};

struct client_surface_scene_plan
{
    struct client_surface_compositor_binding **members;
    struct client_surface_scene_layout *layouts;
    unsigned int count;
    UINT64 epoch;
    BOOL valid;
};

#define CLIENT_SURFACE_COMPOSITOR_FRAME_COUNT 3
#define CLIENT_SURFACE_COMPOSITOR_MAX_INFLIGHT 2
#define CLIENT_SURFACE_COMPOSITOR_DAMAGE_HISTORY 64

struct client_surface_output_allocation
{
    struct client_surface_output_allocation *next;
    Pixmap pixmaps[2];
    UINT64 bytes;
};

static struct client_surface_output_allocation *client_surface_output_allocations;

static UINT64 client_surface_pixmap_bytes( unsigned int width, unsigned int height, unsigned int depth )
{
    return (UINT64)width * height * (depth > 16 ? 4 : depth > 8 ? 2 : 1);
}

struct client_surface_compositor_damage
{
    UINT64 revision;
    RECT rect;
};

struct client_surface_compositor_frame
{
    struct client_surface_compositor_job *waiter;
    Pixmap pixmap;
    GC gc;
    UINT64 revision;
    uint32_t serial;
    uint32_t last_complete_serial;
    unsigned int width;
    unsigned int height;
    UINT64 publish_generation;
    UINT64 publish_epoch;
    BOOL complete;
    BOOL idle;
    BOOL last_complete_success;
    BOOL publish_pending;
    BOOL request_pending;
    struct client_surface_xcb_request request;
    unsigned int xcb_gc;
    struct client_surface_compositor_binding *copy_binding;
    unsigned int copy_index;
    UINT64 copy_control;
    UINT64 copy_sequence;
    BOOL copy_replay;
    RECT copy_damage;
    struct client_surface_xcb_request copy_request;
};

struct client_surface_compositor_target
{
    struct client_surface_compositor_target *next;
    HWND toplevel;
    Window window;
    struct client_surface_compositor_frame frames[CLIENT_SURFACE_COMPOSITOR_FRAME_COUNT];
    struct client_surface_compositor_frame *copy_frame;
    Pixmap backing;
    Pixmap latest;
    Pixmap published;
    UINT64 revision;
    struct client_surface_compositor_damage damages[CLIENT_SURFACE_COMPOSITOR_DAMAGE_HISTORY];
    unsigned int published_width;
    unsigned int published_height;
    RECT restore_rect;
    unsigned int next_frame;
    unsigned int mailbox_frame;
    unsigned int assembly_frame;
    UINT64 mailbox_publish_generation;
    UINT64 mailbox_publish_epoch;
    UINT64 assembly_generation;
    UINT64 assembly_epoch;
    struct client_surface_scene_plan scene;
    struct client_surface_handoff_receipt *receipts;
    unsigned int received;
    unsigned int replay_member;
    BOOL mailbox_pending;
    BOOL assembly_pending;
    BOOL quiescing;
    unsigned int native_updates;
    UINT64 deferred_update;
    BOOL update_notified;
    UINT deferred_update_types;
    UINT64 mailbox_bytes;
    DWORD shrink_start;
    XID present_event;
    unsigned int width;
    unsigned int height;
    unsigned int window_width;
    unsigned int window_height;
    unsigned int depth;
    VisualID visual;
};

static pthread_mutex_t client_surface_compositor_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t client_surface_compositor_cond = PTHREAD_COND_INITIALIZER;
static Display *client_surface_compositor_display;
static BOOL client_surface_compositor_started;
static int client_surface_compositor_notify[2] = {-1, -1};
static struct client_surface_compositor_pool *client_surface_compositor_pools;
static struct client_surface_compositor_binding *client_surface_compositor_bindings;
static struct client_surface_compositor_target *client_surface_compositor_targets;
static UINT64 client_surface_compositor_mark;

enum client_surface_compositor_op
{
    CLIENT_SURFACE_COMPOSITOR_ALLOC_POOL,
    CLIENT_SURFACE_COMPOSITOR_COPY,
    CLIENT_SURFACE_COMPOSITOR_FREE_POOL,
    CLIENT_SURFACE_COMPOSITOR_PRESENT,
    CLIENT_SURFACE_COMPOSITOR_REGISTER_HANDOFF,
    CLIENT_SURFACE_COMPOSITOR_REUSE_HANDOFFS,
    CLIENT_SURFACE_COMPOSITOR_CHECK_SCENE,
    CLIENT_SURFACE_COMPOSITOR_REPAIR_OWNER,
    CLIENT_SURFACE_COMPOSITOR_SWEEP_HANDOFFS,
    CLIENT_SURFACE_COMPOSITOR_UPDATE_TARGET,
    CLIENT_SURFACE_COMPOSITOR_REMOVE_TARGET,
    CLIENT_SURFACE_COMPOSITOR_RESTORE_TARGET,
    CLIENT_SURFACE_COMPOSITOR_BEGIN_UPDATE,
    CLIENT_SURFACE_COMPOSITOR_TRY_BEGIN_UPDATE,
    CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE,
    CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE,
    CLIENT_SURFACE_COMPOSITOR_END_UPDATE,
};

struct client_surface_compositor_job
{
    struct client_surface_compositor_job *next;
    enum client_surface_compositor_op op;
    Drawable source;
    Drawable destination;
    int source_x;
    int source_y;
    int destination_x;
    int destination_y;
    unsigned int width;
    unsigned int height;
    unsigned int window_width;
    unsigned int window_height;
    unsigned int valid_width;
    unsigned int valid_height;
    unsigned int depth;
    Pixmap pixmaps[2];
    HANDLE mapping;
    SIZE_T view_size;
    SIZE_T offset;
    UINT64 mapping_id;
    UINT64 cookie;
    UINT64 identity;
    UINT64 mark;
    UINT64 scene_epoch;
    int ready_fd;
    process_id_t process;
    HWND handoff_window;
    HWND handoff_toplevel;
    VisualID visual;
    BOOL result;
    BOOL complete;
    BOOL async;
    BOOL present_started;
    BOOL present_done;
    DWORD present_start;
    struct client_surface_scene_layout *layouts;
    unsigned int layout_count;
    const struct client_surface_handoff_desc *handoffs;
    unsigned int handoff_count;
    BOOL *handoff_reused;
    BOOL invalidate_scene;
    BOOL update_deferred;
    UINT update_types;
    DWORD shrink_start;
};

static struct client_surface_compositor_job *client_surface_compositor_head;
static struct client_surface_compositor_job **client_surface_compositor_tail =
    &client_surface_compositor_head;
/* Only the compositor thread touches pending jobs. Their submitting threads
 * retain ownership until completion, including while another target runs. */
static struct client_surface_compositor_job *client_surface_compositor_pending;
static UINT64 client_surface_native_update_serial;

#define CLIENT_SURFACE_COPY_BATCH_SIZE 64
struct client_surface_owner_copy
{
    struct client_surface_compositor_binding *binding;
    unsigned int buffer_index;
    UINT64 control, generation, epoch;
    UINT64 sequence;
    BOOL replay;
};

struct client_surface_copy_batch
{
    struct client_surface_compositor_target *target;
    struct client_surface_compositor_frame *frame;
    struct client_surface_owner_copy copies[CLIENT_SURFACE_COPY_BATCH_SIZE];
    unsigned int count;
    int error;
    BOOL asynchronous;
    struct client_surface_xcb_request requests[CLIENT_SURFACE_COPY_BATCH_SIZE];
};

static struct client_surface_copy_batch client_surface_copy_batch;
static struct client_surface_copy_batch client_surface_pending_batches[64];
static unsigned int client_surface_pending_batch_count;

static void flush_client_surface_copy_batch(void);

static struct client_surface_compositor_frame *get_client_surface_compositor_pixmap(
    struct client_surface_compositor_target *target, Pixmap pixmap )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        if (target->frames[i].pixmap == pixmap) return &target->frames[i];
    return NULL;
}

static void note_client_surface_compositor_damage(
    struct client_surface_compositor_target *target,
    struct client_surface_compositor_frame *frame, const RECT *rect )
{
    struct client_surface_compositor_damage *damage;
    RECT bounds = {0, 0, target->window_width, target->window_height};

    if (!(++target->revision))
    {
        unsigned int i;

        target->revision = 1;
        memset( target->damages, 0, sizeof(target->damages) );
        for (i = 0; i < ARRAY_SIZE(target->frames); ++i) target->frames[i].revision = 0;
    }
    damage = &target->damages[target->revision % ARRAY_SIZE(target->damages)];
    damage->revision = target->revision;
    /* A child can extend beyond its owner. Only pixels inside the output
     * need catching up when this frame is reused as a later checkpoint. */
    intersect_rect( &damage->rect, rect, &bounds );
    frame->revision = target->revision;
    target->latest = frame->pixmap;
}

static void note_client_surface_compositor_snapshot(
    struct client_surface_compositor_target *target, Pixmap pixmap )
{
    struct client_surface_compositor_frame *frame;
    RECT rect = {0, 0, target->window_width, target->window_height};

    if ((frame = get_client_surface_compositor_pixmap( target, pixmap )))
        note_client_surface_compositor_damage( target, frame, &rect );
}

#ifdef SONAME_LIBXPRESENT

static uint32_t client_surface_present_serial;
static int client_surface_present_opcode;

#endif

static struct client_surface_compositor_target *find_client_surface_compositor_target( HWND toplevel );
static BOOL publish_client_surface_handoff_generation( HWND toplevel, UINT64 generation,
                                                       UINT64 scene_generation, BOOL success );
static BOOL client_surface_present_on_compositor( struct client_surface_compositor_job *job );
static BOOL process_client_surface_compositor_jobs(void);
static BOOL get_client_surface_window_extent( struct x11drv_win_data *data,
                                              unsigned int *width, unsigned int *height );
#ifdef SONAME_LIBXPRESENT
static BOOL wait_client_surface_compositor_pixmap_idle( Pixmap pixmap );
static void flush_client_surface_compositor_mailbox(
    struct client_surface_compositor_target *target );
#endif

static int client_surface_compositor_error( Display *display, XErrorEvent *event, void *arg )
{
    int *error = arg;

    *error = event->error_code;
    return 1;
}

static BOOL client_surface_compositor_open(void)
{
    Display *display;

    if (client_surface_compositor_display) return TRUE;
    if (!(display = XOpenDisplay( DisplayString( gdi_display ) ))) return FALSE;
    fcntl( ConnectionNumber( display ), F_SETFD, FD_CLOEXEC );

#ifdef SONAME_LIBXPRESENT
    if (usexpresent)
    {
        int event_base, error_base, major, minor;

        if (!pXGetEventData || !pXFreeEventData ||
            !pXPresentQueryExtension( display, &client_surface_present_opcode,
                                      &event_base, &error_base ) ||
            !pXPresentQueryVersion( display, &major, &minor ))
            usexpresent = FALSE;
        else
            TRACE( "client-surface compositor connection opened with X Present %d.%d\n",
                   major, minor );
    }
#endif

    client_surface_compositor_display = display;
    if (!usexpresent) TRACE( "client-surface compositor connection opened with XCopy fallback\n" );
    return TRUE;
}

static BOOL client_surface_copy_on_compositor_unchecked(
    Drawable source, Drawable destination, int source_x, int source_y,
    int destination_x, int destination_y, unsigned int width, unsigned int height )
{
    Display *display = client_surface_compositor_display;
    int error = 0;
    GC gc;

    X11DRV_expect_error( display, client_surface_compositor_error, &error );
    gc = XCreateGC( display, destination, 0, NULL );
    if (gc)
    {
        XCopyArea( display, source, destination, gc, source_x, source_y,
                   width, height, destination_x, destination_y );
        XFreeGC( display, gc );
    }
    XSync( display, False );
    X11DRV_check_error();
    return gc && !error;
}

static BOOL client_surface_copy_on_compositor( Drawable source, Drawable destination,
                                               int source_x, int source_y,
                                               int destination_x, int destination_y,
                                               unsigned int width, unsigned int height )
{
#ifdef SONAME_LIBXPRESENT
    if (usexpresent && !wait_client_surface_compositor_pixmap_idle( destination )) return FALSE;
#endif
    return client_surface_copy_on_compositor_unchecked(
        source, destination, source_x, source_y, destination_x, destination_y, width, height );
}

static BOOL client_surface_alloc_on_compositor( Drawable drawable, unsigned int width,
                                                unsigned int height, unsigned int depth,
                                                Pixmap pixmaps[2] )
{
    Display *display = client_surface_compositor_display;
    struct client_surface_output_allocation *allocation;
    int error = 0;

    if (!(allocation = malloc( sizeof(*allocation) ))) return FALSE;
    allocation->bytes = 2 * client_surface_pixmap_bytes( width, height, depth );
    if (!client_surface_reserve_memory( CLIENT_SURFACE_MEMORY_OUTPUT, allocation->bytes ))
    {
        free( allocation );
        return FALSE;
    }
    X11DRV_expect_error( display, client_surface_compositor_error, &error );
    pixmaps[0] = XCreatePixmap( display, drawable, width, height, depth );
    pixmaps[1] = XCreatePixmap( display, drawable, width, height, depth );
    XSync( display, False );
    X11DRV_check_error();
    if (!error)
    {
        memcpy( allocation->pixmaps, pixmaps, sizeof(allocation->pixmaps) );
        allocation->next = client_surface_output_allocations;
        client_surface_output_allocations = allocation;
        return TRUE;
    }

    X11DRV_expect_error( display, client_surface_compositor_error, &error );
    XFreePixmap( display, pixmaps[0] );
    XFreePixmap( display, pixmaps[1] );
    XSync( display, False );
    X11DRV_check_error();
    pixmaps[0] = pixmaps[1] = 0;
    client_surface_release_memory( CLIENT_SURFACE_MEMORY_OUTPUT, allocation->bytes );
    free( allocation );
    return FALSE;
}

static BOOL client_surface_free_on_compositor( const Pixmap pixmaps[2] )
{
    Display *display = client_surface_compositor_display;
    struct client_surface_output_allocation **cursor, *allocation;
    int error = 0;

    X11DRV_expect_error( display, client_surface_compositor_error, &error );
    if (pixmaps[0]) XFreePixmap( display, pixmaps[0] );
    if (pixmaps[1]) XFreePixmap( display, pixmaps[1] );
    XSync( display, False );
    X11DRV_check_error();
    for (cursor = &client_surface_output_allocations; (allocation = *cursor); cursor = &allocation->next)
    {
        if (!((allocation->pixmaps[0] == pixmaps[0] && allocation->pixmaps[1] == pixmaps[1]) ||
              (allocation->pixmaps[0] == pixmaps[1] && allocation->pixmaps[1] == pixmaps[0]))) continue;
        *cursor = allocation->next;
        client_surface_release_memory( CLIENT_SURFACE_MEMORY_OUTPUT, allocation->bytes );
        free( allocation );
        break;
    }
    return !error;
}

#ifdef SONAME_LIBXPRESENT

static struct client_surface_compositor_target *find_client_surface_compositor_window( Window window )
{
    struct client_surface_compositor_target *target;

    for (target = client_surface_compositor_targets; target; target = target->next)
        if (target->window == window) return target;
    return NULL;
}

static struct client_surface_compositor_frame *find_client_surface_compositor_frame(
    struct client_surface_compositor_target *target, uint32_t serial, Pixmap pixmap )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        if (target->frames[i].serial == serial &&
            (!pixmap || target->frames[i].pixmap == pixmap)) return &target->frames[i];
    return NULL;
}

static void finish_client_surface_compositor_frame(
    struct client_surface_compositor_target *target,
    struct client_surface_compositor_frame *frame )
{
    if (!frame->serial || frame->request_pending || !frame->complete || !frame->idle) return;
    TRACE( "X Present serial %u pixmap %#lx completed and became idle\n",
           frame->serial, frame->pixmap );
    frame->serial = 0;
    frame->complete = frame->idle = FALSE;
    /* Give completed sources a chance to replace a steady mailbox before it is
     * submitted. A reserved topology publication keeps its original order. */
    if (target->mailbox_publish_generation) flush_client_surface_compositor_mailbox( target );
}

static void complete_client_surface_compositor_frame(
    struct client_surface_compositor_target *target,
    struct client_surface_compositor_frame *frame )
{
    BOOL success = frame->last_complete_success;

    if (frame->request_pending || !frame->complete) return;
    if (frame->waiter)
    {
        frame->waiter->present_done = TRUE;
        frame->waiter->result = success;
        frame->waiter = NULL;
    }
    if (success)
    {
        target->published = frame->pixmap;
        target->published_width = frame->width;
        target->published_height = frame->height;
    }
    if (frame->publish_pending)
    {
        publish_client_surface_handoff_generation( target->toplevel,
            frame->publish_generation, frame->publish_epoch, success );
        frame->publish_pending = FALSE;
    }
}

static BOOL process_client_surface_present_replies(void)
{
    struct client_surface_compositor_target *target;
    unsigned int i;
    BOOL progressed = FALSE, success;

    for (target = client_surface_compositor_targets; target; target = target->next)
        for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        {
            struct client_surface_compositor_frame *frame = &target->frames[i];

            if (!frame->request_pending ||
                !client_surface_xcb_poll( client_surface_compositor_display, &frame->request,
                                          &success )) continue;
            progressed = TRUE;
            frame->request_pending = FALSE;
            TRACE( "validated X Present request %u serial %u success %u\n",
                   frame->request.cookies[0], frame->serial, success );
            if (!success)
            {
                TRACE_(csperf)( "ticks=%llu event=present_error window=%lx pixmap=%lx serial=%u\n",
                               client_surface_perf_time(), target->window, frame->pixmap, frame->serial );
                /* A rejected Present cannot generate Complete/Idle. Preserve
                 * the previous synchronous path's checked XCopy fallback. */
                frame->last_complete_success = client_surface_copy_on_compositor(
                    frame->pixmap, target->window, 0, 0, 0, 0, target->width, target->height );
                frame->last_complete_serial = frame->serial;
                frame->complete = frame->idle = TRUE;
            }
            complete_client_surface_compositor_frame( target, frame );
            finish_client_surface_compositor_frame( target, frame );
        }
    return progressed;
}

static void process_client_surface_present_events(void)
{
    Display *display = client_surface_compositor_display;
    unsigned int budget = 128;

    /* XCopy fallback also produces NoExpose events. Drain those when
     * Present is disabled, or the pre-poll queue check would spin forever. */
    if (!display) return;
    while (budget-- && XPending( display ))
    {
        XEvent event;
        struct client_surface_compositor_target *target = NULL;
        struct client_surface_compositor_frame *frame = NULL;

        XNextEvent( display, &event );
        if (event.type != GenericEvent || event.xcookie.extension != client_surface_present_opcode ||
            !pXGetEventData || !pXGetEventData( display, &event ))
            continue;
        if (event.xcookie.evtype == PresentCompleteNotify)
        {
            XPresentCompleteNotifyEvent *notify = event.xcookie.data;

            if (notify->kind == PresentCompleteKindPixmap &&
                (target = find_client_surface_compositor_window( notify->window )) &&
                (frame = find_client_surface_compositor_frame( target,
                                                               notify->serial_number, 0 )))
            {
                BOOL success = notify->mode != PresentCompleteModeSkip;

                TRACE_(csperf)( "ticks=%llu event=complete window=%lx pixmap=%lx serial=%u "
                               "mode=%u ust=%s msc=%s\n", client_surface_perf_time(), target->window,
                               frame->pixmap, frame->serial, notify->mode,
                               wine_dbgstr_longlong( notify->ust ), wine_dbgstr_longlong( notify->msc ) );
                frame->complete = TRUE;
                frame->last_complete_serial = frame->serial;
                frame->last_complete_success = success;
                complete_client_surface_compositor_frame( target, frame );
            }
        }
        else if (event.xcookie.evtype == PresentIdleNotify)
        {
            XPresentIdleNotifyEvent *notify = event.xcookie.data;

            if ((target = find_client_surface_compositor_window( notify->window )) &&
                (frame = find_client_surface_compositor_frame( target,
                    notify->serial_number, notify->pixmap )))
            {
                TRACE_(csperf)( "ticks=%llu event=idle window=%lx pixmap=%lx serial=%u\n",
                               client_surface_perf_time(), target->window, frame->pixmap, frame->serial );
                frame->idle = TRUE;
            }
        }
        pXFreeEventData( display, &event );
        if (target && frame) finish_client_surface_compositor_frame( target, frame );
    }
}

static struct client_surface_compositor_frame *acquire_client_surface_compositor_frame(
    struct client_surface_compositor_target *target, Pixmap requested )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
    {
        struct client_surface_compositor_frame *frame = &target->frames[i];

        if (frame->pixmap == requested && !frame->serial) return frame;
    }
    return NULL;
}

static unsigned int count_client_surface_compositor_frames(
    const struct client_surface_compositor_target *target )
{
    unsigned int count = 0, i;

    for (i = 0; i < ARRAY_SIZE(target->frames); ++i) count += !!target->frames[i].serial;
    return count;
}

static struct client_surface_compositor_frame *get_client_surface_compositor_frame(
    struct client_surface_compositor_target *target )
{
    unsigned int i;

    process_client_surface_present_events();
    if (target->mailbox_pending) return &target->frames[target->mailbox_frame];
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
    {
        unsigned int index = (target->next_frame + i) % ARRAY_SIZE(target->frames);

        if (target->frames[index].serial ||
            (target->assembly_pending && index == target->assembly_frame)) continue;
        target->next_frame = (index + 1) % ARRAY_SIZE(target->frames);
        return &target->frames[index];
    }
    return NULL;
}

static BOOL wait_client_surface_compositor_pixmap_idle( Pixmap pixmap )
{
    struct client_surface_compositor_target *target;
    unsigned int i;

    for (target = client_surface_compositor_targets; target; target = target->next)
        for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
            if (target->frames[i].pixmap == pixmap)
                return !!acquire_client_surface_compositor_frame( target, pixmap );
    return TRUE;
}

static BOOL submit_client_surface_present( struct client_surface_compositor_target *target,
                                           struct client_surface_compositor_frame *frame,
                                           UINT64 publish_generation, UINT64 publish_epoch,
                                           uint32_t *serial_ret )
{
    Display *display = client_surface_compositor_display;
    uint32_t serial;
    int error = 0;

    if (!usexpresent || !target->present_event || frame->serial) return FALSE;
    if (!(serial = ++client_surface_present_serial)) serial = ++client_surface_present_serial;

    frame->serial = serial;
    frame->last_complete_serial = 0;
    frame->last_complete_success = FALSE;
    frame->complete = frame->idle = FALSE;
    frame->publish_generation = publish_generation;
    frame->publish_epoch = publish_epoch;
    frame->publish_pending = !!publish_generation;
    frame->request_pending = client_surface_xcb_present( display, target->window, frame->pixmap,
                                                        serial, &frame->request );
    if (!frame->request_pending)
    {
        X11DRV_expect_error( display, client_surface_compositor_error, &error );
        pXPresentPixmap( display, target->window, frame->pixmap, serial, None, None, 0, 0, None,
                         None, None, PresentOptionAsync | PresentOptionCopy,
                         0, 0, 0, NULL, 0 );
        XSync( display, False );
        X11DRV_check_error();
    }
    if (error)
    {
        frame->serial = 0;
        frame->publish_pending = FALSE;
        return FALSE;
    }
    if (serial_ret) *serial_ret = serial;
    TRACE_(csperf)( "ticks=%llu event=present window=%lx pixmap=%lx serial=%u\n",
                   client_surface_perf_time(), target->window, frame->pixmap, serial );
    TRACE( "queued X Present serial %u pixmap %#lx generation %s\n", serial,
           frame->pixmap, wine_dbgstr_longlong( publish_generation ) );
    return TRUE;
}

static void flush_client_surface_compositor_mailbox(
    struct client_surface_compositor_target *target )
{
    struct client_surface_compositor_frame *frame;
    BOOL copied;

    if (target->quiescing || target->copy_frame || !target->mailbox_pending ||
        count_client_surface_compositor_frames( target ) >= CLIENT_SURFACE_COMPOSITOR_MAX_INFLIGHT)
        return;
    frame = &target->frames[target->mailbox_frame];
    if (submit_client_surface_present( target, frame,
                                       target->mailbox_publish_generation,
                                       target->mailbox_publish_epoch, NULL ))
    {
        target->mailbox_pending = FALSE;
        target->mailbox_publish_generation = 0;
        target->mailbox_publish_epoch = 0;
        return;
    }

    /* A mailbox slot is reserved from generic copies and publications.  If
     * Present cannot consume it, publish it synchronously rather than leaving
     * the reservation behind with no X event capable of retrying it. */
    copied = client_surface_copy_on_compositor( frame->pixmap, target->window,
                                                0, 0, 0, 0,
                                                target->width, target->height );
    if (copied)
    {
        target->published = frame->pixmap;
        target->published_width = frame->width;
        target->published_height = frame->height;
    }
    if (target->mailbox_publish_generation)
        publish_client_surface_handoff_generation( target->toplevel,
            target->mailbox_publish_generation, target->mailbox_publish_epoch, copied );
    target->mailbox_pending = FALSE;
    target->mailbox_publish_generation = 0;
    target->mailbox_publish_epoch = 0;
}

#else

static BOOL process_client_surface_present_replies(void)
{
    return FALSE;
}

static void process_client_surface_present_events(void)
{
    Display *display = client_surface_compositor_display;
    unsigned int budget = 128;
    XEvent event;

    if (!display) return;
    while (budget-- && XPending( display )) XNextEvent( display, &event );
}

#endif

static void client_surface_handoff_futex_wake( LONG *address )
{
#ifdef __linux__
    syscall( SYS_futex, address, FUTEX_WAKE, INT_MAX, NULL, NULL, 0 );
#else
    (void)address;
#endif
}

static void wake_client_surface_compositor(void)
{
    UINT64 value = 1;
    int ret;

    do
#ifdef __linux__
        ret = write( client_surface_compositor_notify[1], &value, sizeof(value) );
#else
        ret = send( client_surface_compositor_notify[1], &value, sizeof(value), 0 );
#endif
    while (ret < 0 && errno == EINTR);
}

static void client_surface_handoff_wake_release( struct client_surface_handoff_shared *shared )
{
    if (!__atomic_exchange_n( &shared->release_parked, 0, __ATOMIC_ACQ_REL )) return;
    __atomic_add_fetch( &shared->release_sequence, 1, __ATOMIC_RELEASE );
    client_surface_handoff_futex_wake( &shared->release_sequence );
}

static void finish_client_surface_compositor_assembly(
    struct client_surface_compositor_target *target, BOOL invalidate )
{
    struct client_surface_compositor_frame *frame;

    if (!target->assembly_pending) return;
    frame = &target->frames[target->assembly_frame];
    target->received = 0;
    if (target->receipts) memset( target->receipts, 0, target->scene.count * sizeof(*target->receipts) );
    if (invalidate)
    {
        SERVER_START_REQ( cancel_client_surface_handoffs )
        {
            req->handle = wine_server_user_handle( target->toplevel );
            req->generation = target->assembly_generation;
            req->scene_generation = target->assembly_epoch;
            wine_server_call( req );
        }
        SERVER_END_REQ;
        /* A cancelled transaction may already have overwritten arbitrary
         * regions of its private frame.  Remove it from the damage lineage so
         * its next use starts with a full copy of the last complete frame. */
        assert( frame->pixmap != target->latest );
        assert( frame->pixmap != target->published );
        frame->revision = 0;
        TRACE( "aborted owner assembly generation %s epoch %s pixmap %#lx\n",
               wine_dbgstr_longlong( target->assembly_generation ),
               wine_dbgstr_longlong( target->assembly_epoch ), frame->pixmap );
    }
    target->assembly_pending = FALSE;
    target->assembly_generation = 0;
    target->assembly_epoch = 0;
    target->assembly_frame = 0;
}

static struct client_surface_compositor_frame *acquire_client_surface_compositor_assembly_frame(
    struct client_surface_compositor_target *target )
{
    unsigned int i;

    process_client_surface_present_events();
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
    {
        unsigned int index = (target->next_frame + i) % ARRAY_SIZE(target->frames);
        struct client_surface_compositor_frame *frame = &target->frames[index];

        /* A GUI snapshot may have advanced latest while published still
         * names the previous visible image. Keep both checkpoints intact
         * until the new assembly has completed and become visible. */
        if (frame->serial || frame->pixmap == target->latest || frame->pixmap == target->published ||
            (target->mailbox_pending && index == target->mailbox_frame))
            continue;
        target->next_frame = (index + 1) % ARRAY_SIZE(target->frames);
        return frame;
    }
    return NULL;
}

static struct client_surface_compositor_pool *find_client_surface_compositor_pool( UINT64 id )
{
    struct client_surface_compositor_pool *pool;

    for (pool = client_surface_compositor_pools; pool; pool = pool->next)
        if (pool->id == id) return pool;
    return NULL;
}

static struct client_surface_compositor_target *find_client_surface_compositor_target( HWND toplevel )
{
    struct client_surface_compositor_target *target;

    for (target = client_surface_compositor_targets; target; target = target->next)
        if (target->toplevel == toplevel) return target;
    return NULL;
}

static BOOL client_surface_present_on_compositor( struct client_surface_compositor_job *job )
{
#ifdef SONAME_LIBXPRESENT
    struct client_surface_compositor_target *target =
        find_client_surface_compositor_window( job->destination );
    struct client_surface_compositor_frame *frame;

    if (!usexpresent || !target ||
        !(frame = acquire_client_surface_compositor_frame( target, job->source )))
        return FALSE;
    frame->width = job->width;
    frame->height = job->height;
    /* This owner scene snapshot includes GDI pixels written outside the
     * compositor connection.  Record a complete checkpoint for later partial
     * handoffs into other pool entries. */
    note_client_surface_compositor_snapshot( target, job->source );
    if (!submit_client_surface_present( target, frame, 0, 0, NULL ))
        return FALSE;
    frame->waiter = job;
    job->present_started = TRUE;
    job->present_start = NtGetTickCount();
    return TRUE;
#else
    return FALSE;
#endif
}

static void release_client_surface_compositor_binding_server(
    const struct client_surface_compositor_binding *binding )
{
    SERVER_START_REQ( release_client_surface_handoff )
    {
        req->handle = wine_server_user_handle( binding->toplevel );
        req->producer = binding->process;
        req->surface = binding->identity;
        req->cookie = binding->cookie;
        req->owner = 1;
        wine_server_call( req );
    }
    SERVER_END_REQ;
}

static void release_client_surface_compositor_pool( struct client_surface_compositor_pool *pool )
{
    struct client_surface_compositor_pool **cursor;

    assert( pool->refs );
    if (--pool->refs) return;
    for (cursor = &client_surface_compositor_pools; *cursor; cursor = &(*cursor)->next)
    {
        if (*cursor != pool) continue;
        *cursor = pool->next;
        NtUnmapViewOfSection( NtCurrentProcess(), pool->shared );
        close( pool->ready_fd );
        free( pool );
        return;
    }
    assert( 0 );
}

static void free_client_surface_cached_image( struct client_surface_cached_image *image )
{
    int error = 0;

    if (image->pixmap || image->gc)
        X11DRV_expect_error( client_surface_compositor_display, client_surface_compositor_error, &error );
    if (image->gc) XFreeGC( client_surface_compositor_display, image->gc );
    if (image->pixmap) XFreePixmap( client_surface_compositor_display, image->pixmap );
    if (image->pixmap || image->gc)
    {
        XSync( client_surface_compositor_display, False );
        X11DRV_check_error();
        TRACE( "freed owner cache pixmap %#lx bytes %s error %u\n", image->pixmap,
               wine_dbgstr_longlong( image->bytes ), error );
    }
    client_surface_release_memory( CLIENT_SURFACE_MEMORY_SOURCE, image->bytes );
    memset( image, 0, sizeof(*image) );
}

static void remove_client_surface_compositor_binding(
    struct client_surface_compositor_binding **cursor )
{
    struct client_surface_compositor_binding *binding = *cursor;
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( binding->toplevel );
    unsigned int index = binding->channel - binding->pool->shared->channels;

    assert( !target || !target->copy_frame );
    *cursor = binding->next;
    if (binding->pool->bindings[index] == binding) binding->pool->bindings[index] = NULL;
    if (target)
    {
        finish_client_surface_compositor_assembly( target, TRUE );
        target->scene.valid = FALSE;
    }
    /* Acknowledged descriptors refer to this binding's cached images. Once
     * the cache is discarded, a new consumer needs a new channel and frame;
     * it cannot resume at the old consumer sequence without those images. */
    __atomic_store_n( &binding->channel->closed, 1, __ATOMIC_RELEASE );
    release_client_surface_compositor_binding_server( binding );
    release_client_surface_compositor_pool( binding->pool );
    free_client_surface_cached_image( &binding->latest_image );
    free_client_surface_cached_image( &binding->spare_image );
    free( binding );
}

static struct client_surface_compositor_pool *acquire_client_surface_compositor_pool(
    struct client_surface_compositor_job *job )
{
    struct client_surface_handoff_shared *shared;
    struct client_surface_compositor_pool *pool;
    SIZE_T size = job->view_size;
    void *view = NULL;

    if ((pool = find_client_surface_compositor_pool( job->mapping_id )))
    {
        size = pool->size;
        view = pool->shared;
    }
    else
    {
        /* The job's caller keeps its section handle until this synchronous job
         * returns. Only the first binding maps a pool; later registrations use
         * this connection's retained view, including across scene changes. */
        if (NtMapViewOfSection( job->mapping, NtCurrentProcess(), &view, 0, 0, NULL,
                               &size, ViewShare, 0, PAGE_READWRITE )) return NULL;
    }
    shared = view;

    if (size < sizeof(*shared) ||
        __atomic_load_n( &shared->magic, __ATOMIC_ACQUIRE ) != CLIENT_SURFACE_HANDOFF_MAGIC ||
        shared->version != CLIENT_SURFACE_HANDOFF_VERSION ||
        shared->channel_count != CLIENT_SURFACE_HANDOFF_CHANNELS ||
        shared->mapping_id != job->mapping_id)
        goto failed;
    if (pool)
    {
        ++pool->refs;
        return pool;
    }
    if (!(pool = calloc( 1, sizeof(*pool) ))) goto failed;
    pool->next = client_surface_compositor_pools;
    pool->shared = shared;
    pool->id = job->mapping_id;
    pool->size = size;
    pool->refs = 1;
    pool->ready_fd = job->ready_fd;
    job->ready_fd = -1;
    client_surface_compositor_pools = pool;
    return pool;

failed:
    if (!pool) NtUnmapViewOfSection( NtCurrentProcess(), view );
    return NULL;
}

static BOOL client_surface_compositor_binding_is_live( const struct client_surface_compositor_binding *binding )
{
    return !__atomic_load_n( &binding->channel->closed, __ATOMIC_ACQUIRE ) &&
           (__atomic_load_n( &binding->channel->endpoints, __ATOMIC_ACQUIRE ) &
            CLIENT_SURFACE_HANDOFF_ENDPOINT_CONSUMER);
}

static BOOL register_client_surface_compositor_handoff(
    struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_binding **cursor, *binding;
    struct client_surface_compositor_pool *pool;
    struct client_surface_handoff_channel *channel;
    BOOL ret = FALSE;

    if (!(pool = acquire_client_surface_compositor_pool( job ))) goto done;
    if (job->offset < offsetof(struct client_surface_handoff_shared, channels) ||
        job->offset > sizeof(*pool->shared) - sizeof(*channel) ||
        (job->offset - offsetof(struct client_surface_handoff_shared, channels)) % sizeof(*channel)) goto done;
    channel = (struct client_surface_handoff_channel *)((char *)pool->shared + job->offset);
    if (channel->cookie != job->cookie || channel->identity != job->identity ||
        channel->producer_process != job->process ||
        channel->window != wine_server_user_handle( job->handoff_window ) ||
        channel->toplevel != wine_server_user_handle( job->handoff_toplevel ) ||
        __atomic_load_n( &channel->closed, __ATOMIC_ACQUIRE )) goto done;

    for (cursor = &client_surface_compositor_bindings; *cursor; cursor = &(*cursor)->next)
    {
        binding = *cursor;
        if (binding->toplevel != job->handoff_toplevel ||
            binding->process != job->process || binding->identity != job->identity)
            continue;
        if (binding->cookie == job->cookie)
        {
            binding->mark = job->mark;
            ret = TRUE;
            goto done;
        }
        /* Keep the acquired pool alive if this was its last old binding. */
        remove_client_surface_compositor_binding( cursor );
        break;
    }

    if (!(binding = calloc( 1, sizeof(*binding) ))) goto done;
    binding->next = client_surface_compositor_bindings;
    binding->pool = pool;
    binding->channel = channel;
    pool->bindings[channel - pool->shared->channels] = binding;
    binding->toplevel = job->handoff_toplevel;
    binding->window = job->handoff_window;
    binding->process = job->process;
    binding->identity = job->identity;
    binding->cookie = job->cookie;
    binding->mark = job->mark;
    pool->refs++;
    client_surface_compositor_bindings = binding;
    TRACE( "registered handoff hwnd %p identity %s producer %04x pool %s cookie %s\n",
           binding->window, wine_dbgstr_longlong( binding->identity ), binding->process,
           wine_dbgstr_longlong( pool->id ), wine_dbgstr_longlong( binding->cookie ) );
    ret = TRUE;

done:
    if (pool) release_client_surface_compositor_pool( pool );
    if (job->ready_fd >= 0) close( job->ready_fd );
    job->ready_fd = -1;
    return ret;
}

static void update_client_surface_compositor_scene(
    struct client_surface_compositor_target *target, UINT64 scene_epoch )
{
#ifdef SONAME_LIBXPRESENT
    unsigned int i;
#endif

    if (target->scene.epoch == scene_epoch) return;
    finish_client_surface_compositor_assembly( target, TRUE );
#ifdef SONAME_LIBXPRESENT
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
    {
        struct client_surface_compositor_frame *frame = &target->frames[i];

        if (!frame->publish_pending || frame->publish_epoch == scene_epoch) continue;
        publish_client_surface_handoff_generation( target->toplevel,
            frame->publish_generation, frame->publish_epoch, FALSE );
        frame->publish_pending = FALSE;
    }
    if (target->mailbox_pending && target->mailbox_publish_generation &&
        target->mailbox_publish_epoch != scene_epoch)
    {
        publish_client_surface_handoff_generation( target->toplevel,
            target->mailbox_publish_generation, target->mailbox_publish_epoch, FALSE );
        target->mailbox_pending = FALSE;
        target->mailbox_publish_generation = 0;
        target->mailbox_publish_epoch = 0;
    }
#endif
    target->scene.epoch = scene_epoch;
}

static int compare_client_surface_scene_members( const void *a, const void *b )
{
    const struct client_surface_compositor_binding *left = *(const struct client_surface_compositor_binding * const *)a;
    const struct client_surface_compositor_binding *right = *(const struct client_surface_compositor_binding * const *)b;
    user_handle_t l = wine_server_user_handle( left->window ), r = wine_server_user_handle( right->window );

    return (l > r) - (l < r);
}

static int compare_client_surface_scene_layouts( const void *a, const void *b )
{
    const struct client_surface_scene_layout *left = a, *right = b;
    user_handle_t l = wine_server_user_handle( left->window ), r = wine_server_user_handle( right->window );

    return (l > r) - (l < r);
}

static int compare_client_surface_handoff_descs( const void *a, const void *b )
{
    const struct client_surface_handoff_desc *left = a, *right = b;

    return (left->handle > right->handle) - (left->handle < right->handle);
}

/* Only the owner thread touches these binding references. Build an index for
 * this job rather than borrowing pointers from an invalidated ScenePlan. */
static BOOL reuse_client_surface_compositor_handoffs( const struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_binding *binding, **members;
    unsigned int count = 0, i = 0;

    for (i = 0; i < job->handoff_count; ++i) job->handoff_reused[i] = FALSE;
    for (binding = client_surface_compositor_bindings; binding; binding = binding->next)
        if (binding->toplevel == job->handoff_toplevel) ++count;
    if (!count) return TRUE;
    if (!(members = calloc( count, sizeof(*members) ))) return FALSE;
    i = 0;
    for (binding = client_surface_compositor_bindings; binding; binding = binding->next)
        if (binding->toplevel == job->handoff_toplevel) members[i++] = binding;
    qsort( members, count, sizeof(*members), compare_client_surface_scene_members );

    for (i = 0; i < job->handoff_count; ++i)
    {
        const struct client_surface_handoff_desc *desc = &job->handoffs[i];
        unsigned int low = 0, high = count;

        /* A nonzero roster cookie includes server-side retirement checks;
         * shared slot endpoints alone cannot authorize A -> B -> A reuse. */
        if (!desc->cookie) continue;
        while (low < high)
        {
            unsigned int mid = low + (high - low) / 2;
            if (wine_server_user_handle( members[mid]->window ) < desc->handle) low = mid + 1;
            else high = mid;
        }
        for (; low < count; ++low)
        {
            binding = members[low];
            if (wine_server_user_handle( binding->window ) != desc->handle) break;
            if (binding->process != desc->process || binding->identity != desc->surface ||
                binding->cookie != desc->cookie) continue;
            if ((job->handoff_reused[i] = client_surface_compositor_binding_is_live( binding )))
                binding->mark = job->mark;
            break;
        }
    }
    free( members );
    return TRUE;
}

static void free_client_surface_scene_layouts( struct client_surface_scene_layout *layouts,
                                               unsigned int count )
{
    unsigned int i;

    for (i = 0; i < count; ++i) free( layouts[i].clip );
    free( layouts );
}

static BOOL check_client_surface_compositor_scene( const struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_target *target =
        find_client_surface_compositor_target( job->handoff_toplevel );
    struct client_surface_compositor_binding *binding;
    unsigned int i, count = 0, found = 0;

    for (i = 0; i < job->handoff_count; ++i) count += !!job->handoffs[i].visible;
    if (!target || !target->scene.valid || target->scene.epoch != job->scene_epoch ||
        target->scene.count != count) return FALSE;
    for (binding = client_surface_compositor_bindings; binding; binding = binding->next)
    {
        const struct client_surface_handoff_desc *desc;
        unsigned int low = 0, high = job->handoff_count;

        if (binding->toplevel != job->handoff_toplevel) continue;
        /* The caller sorted this authoritative roster. Validate retained
         * hidden bindings too; a changed producer or retired cookie must not
         * survive merely because the visible scene is unchanged. */
        while (low < high)
        {
            unsigned int mid = low + (high - low) / 2;

            if (job->handoffs[mid].handle < wine_server_user_handle( binding->window )) low = mid + 1;
            else high = mid;
        }
        if (low == job->handoff_count) return FALSE;
        desc = &job->handoffs[low];
        if (wine_server_user_handle( binding->window ) != desc->handle ||
            binding->process != desc->process || binding->identity != desc->surface ||
            binding->cookie != desc->cookie || !client_surface_compositor_binding_is_live( binding )) return FALSE;
        if (!desc->visible) continue;
        if (binding->scene_index >= target->scene.count ||
            target->scene.members[binding->scene_index] != binding) return FALSE;
        ++found;
    }
    return found == count;
}

static BOOL install_client_surface_scene_plan( struct client_surface_compositor_target *target,
                                               struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_binding *binding, **members = NULL;
    struct client_surface_handoff_receipt *receipts = NULL;
    unsigned int count = job->layout_count, available = 0, i = 0, next = 0;

    for (binding = client_surface_compositor_bindings; binding; binding = binding->next)
        if (binding->toplevel == target->toplevel) ++available;
    if (count > available) return FALSE;
    if (count)
    {
        if (!(members = malloc( available * sizeof(*members) ))) return FALSE;
        if (!(receipts = calloc( count, sizeof(*receipts) )))
        {
            free( members );
            return FALSE;
        }
        for (binding = client_surface_compositor_bindings; binding; binding = binding->next)
            if (binding->toplevel == target->toplevel) members[i++] = binding;
        qsort( members, available, sizeof(*members), compare_client_surface_scene_members );
        qsort( job->layouts, count, sizeof(*job->layouts), compare_client_surface_scene_layouts );
        for (i = 0; i < count; ++i)
        {
            /* The binding cache includes hidden producers. Select only the
             * visible scene's layers from this sorted list; visibility does
             * not discard their last completed images. */
            while (next < available && wine_server_user_handle( members[next]->window ) <
                                       wine_server_user_handle( job->layouts[i].window )) ++next;
            if (next == available || members[next]->window != job->layouts[i].window ||
                members[next]->process != job->layouts[i].process ||
                members[next]->identity != job->layouts[i].identity)
            {
                free( members );
                free( receipts );
                return FALSE;
            }
            members[i] = members[next++];
        }
    }
    if (target->scene.valid && target->scene.epoch == job->scene_epoch && target->scene.count == count &&
        (!count || !memcmp( members, target->scene.members, count * sizeof(*members) )))
    {
        free( receipts );
        free( members );
        return TRUE;
    }
    finish_client_surface_compositor_assembly( target, TRUE );
    update_client_surface_compositor_scene( target, job->scene_epoch );
    free( target->receipts );
    free( target->scene.members );
    free_client_surface_scene_layouts( target->scene.layouts, target->scene.count );
    target->scene.members = members;
    target->scene.layouts = job->layouts;
    job->layouts = NULL;
    job->layout_count = 0;
    target->scene.count = count;
    target->scene.valid = TRUE;
    target->quiescing = target->native_updates || target->deferred_update;
    target->receipts = receipts;
    target->replay_member = 0;
    for (i = 0; i < count; ++i)
    {
        members[i]->scene_index = i;
        TRACE( "owner scene member hwnd %p epoch %s destination %s clip count %lu\n",
               members[i]->window, wine_dbgstr_longlong( target->scene.epoch ),
               wine_dbgstr_rect( &target->scene.layouts[i].geometry.monitor_rect ),
               (unsigned long)target->scene.layouts[i].clip->rdh.nCount );
    }
    return TRUE;
}

static BOOL sweep_client_surface_compositor_handoffs( HWND toplevel, UINT64 mark,
                                                       struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_binding **cursor = &client_surface_compositor_bindings;
    struct client_surface_compositor_target *target;

    while (*cursor)
    {
        struct client_surface_compositor_binding *binding = *cursor;

        if (binding->toplevel == toplevel && binding->mark != mark)
            remove_client_surface_compositor_binding( cursor );
        else
            cursor = &binding->next;
    }
    if (job && (target = find_client_surface_compositor_target( toplevel )))
        return install_client_surface_scene_plan( target, job );
    return TRUE;
}

static void drain_client_surface_compositor_target(
    struct client_surface_compositor_target *target )
{
#ifdef SONAME_LIBXPRESENT
    unsigned int i;
#endif

    assert( !target->copy_frame );
#ifdef SONAME_LIBXPRESENT

    /* The scheduler reaches this boundary only after Complete and Idle.
     * A timeout must never manufacture permission to reuse a pixmap. */
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i) assert( !target->frames[i].serial );
    if (target->mailbox_pending && target->mailbox_publish_generation)
        publish_client_surface_handoff_generation( target->toplevel,
            target->mailbox_publish_generation, target->mailbox_publish_epoch, FALSE );
    target->mailbox_pending = FALSE;
    target->mailbox_publish_generation = 0;
    target->mailbox_publish_epoch = 0;
    if (target->present_event)
    {
        pXPresentFreeInput( client_surface_compositor_display, target->window,
                            target->present_event );
        target->present_event = 0;
        XFlush( client_surface_compositor_display );
    }
#else
    (void)target;
#endif
}

static BOOL update_client_surface_compositor_target( struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_target *target;
    Pixmap old_mailbox = 0;
    BOOL same_pool, checkpoint;

    if (!(target = find_client_surface_compositor_target( job->handoff_toplevel )))
    {
        if (!(target = calloc( 1, sizeof(*target) ))) return FALSE;
        target->next = client_surface_compositor_targets;
        target->toplevel = job->handoff_toplevel;
        client_surface_compositor_targets = target;
    }
    same_pool = target->frames[2].pixmap &&
                ((target->frames[0].pixmap == job->pixmaps[0] &&
                 target->frames[1].pixmap == job->pixmaps[1]) ||
                 (target->frames[0].pixmap == job->pixmaps[1] &&
                 target->frames[1].pixmap == job->pixmaps[0]));
    checkpoint = !same_pool || target->backing != job->pixmaps[0];
    if (target->window != job->destination || target->window_width != job->window_width ||
        target->window_height != job->window_height || target->depth != job->depth || target->visual != job->visual)
    {
        target->scene.valid = FALSE;
        SetRectEmpty( &target->restore_rect );
    }
    if (target->assembly_pending &&
        (target->window != job->destination || target->backing != job->pixmaps[0] ||
         target->window_width != job->window_width || target->window_height != job->window_height ||
         target->depth != job->depth || target->visual != job->visual))
        finish_client_surface_compositor_assembly( target, TRUE );
    if ((target->window && target->window != job->destination) ||
        (target->frames[0].pixmap && !same_pool))
    {
        drain_client_surface_compositor_target( target );
        old_mailbox = target->frames[2].pixmap;
    }
    target->window = job->destination;
    if (!same_pool)
    {
        int error = 0;
        unsigned int i;

        if (old_mailbox)
        {
            XFreePixmap( client_surface_compositor_display, old_mailbox );
            client_surface_release_memory( CLIENT_SURFACE_MEMORY_OUTPUT, target->mailbox_bytes );
            target->mailbox_bytes = 0;
        }
        for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        {
            client_surface_xcb_free_gc( client_surface_compositor_display, &target->frames[i].xcb_gc );
            if (target->frames[i].gc) XFreeGC( client_surface_compositor_display, target->frames[i].gc );
        }
        memset( target->frames, 0, sizeof(target->frames) );
        target->frames[0].pixmap = job->pixmaps[0];
        target->frames[1].pixmap = job->pixmaps[1];
        target->mailbox_bytes = client_surface_pixmap_bytes( job->width, job->height, job->depth );
        if (!client_surface_reserve_memory( CLIENT_SURFACE_MEMORY_OUTPUT, target->mailbox_bytes ))
        {
            target->mailbox_bytes = 0;
            return FALSE;
        }
        X11DRV_expect_error( client_surface_compositor_display,
                             client_surface_compositor_error, &error );
        target->frames[2].pixmap = XCreatePixmap( client_surface_compositor_display,
            target->window, job->width, job->height, job->depth );
        XSync( client_surface_compositor_display, False );
        X11DRV_check_error();
        if (error || !target->frames[2].pixmap)
        {
            target->frames[2].pixmap = 0;
            client_surface_release_memory( CLIENT_SURFACE_MEMORY_OUTPUT, target->mailbox_bytes );
            target->mailbox_bytes = 0;
            return FALSE;
        }
        target->published = job->pixmaps[0];
        target->published_width = job->valid_width;
        target->published_height = job->valid_height;
        target->next_frame = 0;
        target->mailbox_pending = FALSE;
    }
    target->backing = job->pixmaps[0];
    target->shrink_start = job->shrink_start;
    target->width = job->width;
    target->height = job->height;
    target->window_width = job->window_width;
    target->window_height = job->window_height;
    target->depth = job->depth;
    target->visual = job->visual;
    target->quiescing = target->native_updates || target->deferred_update;
    TRACE( "updated compositor target hwnd %p window %#lx size %ux%u depth %u visual %#lx\n",
           target->toplevel, target->window, target->window_width, target->window_height,
           target->depth, target->visual );
    /* Ensuring capacity or refreshing topology does not write an image.
     * Only a replacement pool or an actual GUI snapshot rotation introduces
     * a new checkpoint. Otherwise this would replace the latest completed
     * output with an older spare and corrupt the next incremental copy. */
    if (checkpoint) note_client_surface_compositor_snapshot( target, target->backing );
#ifdef SONAME_LIBXPRESENT
    if (usexpresent && !target->present_event)
    {
        int error = 0;

        X11DRV_expect_error( client_surface_compositor_display,
                             client_surface_compositor_error, &error );
        target->present_event = pXPresentSelectInput(
            client_surface_compositor_display, target->window,
            PresentCompleteNotifyMask | PresentIdleNotifyMask );
        XSync( client_surface_compositor_display, False );
        X11DRV_check_error();
        if (error) target->present_event = 0;
    }
#endif
    return TRUE;
}

static BOOL remove_client_surface_compositor_target( HWND toplevel )
{
    struct client_surface_compositor_target **cursor;

    sweep_client_surface_compositor_handoffs( toplevel, 0, 0 );
    for (cursor = &client_surface_compositor_targets; *cursor; cursor = &(*cursor)->next)
    {
        struct client_surface_compositor_target *target = *cursor;
        unsigned int i;

        if (target->toplevel != toplevel) continue;
        drain_client_surface_compositor_target( target );
        for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        {
            client_surface_xcb_free_gc( client_surface_compositor_display, &target->frames[i].xcb_gc );
            if (target->frames[i].gc) XFreeGC( client_surface_compositor_display, target->frames[i].gc );
        }
        if (target->frames[2].pixmap)
        {
            XFreePixmap( client_surface_compositor_display, target->frames[2].pixmap );
            XSync( client_surface_compositor_display, False );
            client_surface_release_memory( CLIENT_SURFACE_MEMORY_OUTPUT, target->mailbox_bytes );
        }
        *cursor = target->next;
        free( target->scene.members );
        free_client_surface_scene_layouts( target->scene.layouts, target->scene.count );
        free( target->receipts );
        free( target );
        return TRUE;
    }
    return TRUE;
}

static BOOL client_surface_compositor_restore_ready( const struct client_surface_compositor_target *target )
{
    unsigned int i;

    if (target->copy_frame) return FALSE;
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        if (target->frames[i].serial &&
            (target->frames[i].request_pending || !target->frames[i].complete)) return FALSE;
    return TRUE;
}

static BOOL restore_client_surface_compositor_pixels( struct client_surface_compositor_target *target )
{
    RECT rect = target->restore_rect;

    SetRectEmpty( &target->restore_rect );
    if (!target->published || target->published_width < target->window_width ||
        target->published_height < target->window_height) return FALSE;
    TRACE( "restoring target %p window %#lx from published pixmap %#lx rect %s\n",
           target->toplevel, target->window, target->published, wine_dbgstr_rect( &rect ) );
    return client_surface_copy_on_compositor( target->published, target->window,
                                              rect.left, rect.top, rect.left, rect.top,
                                              rect.right - rect.left, rect.bottom - rect.top );
}

static BOOL process_client_surface_compositor_restores(void)
{
    struct client_surface_compositor_target *target;
    BOOL progressed = FALSE;

    for (target = client_surface_compositor_targets; target; target = target->next)
    {
        if (IsRectEmpty( &target->restore_rect ) || !client_surface_compositor_restore_ready( target )) continue;
        progressed = TRUE;
        if (!restore_client_surface_compositor_pixels( target ))
            NtUserPostMessage( target->toplevel, WM_WINE_UPDATEWINDOWSTATE,
                               WINE_UPDATE_CLIENT_SURFACE_HANDOFFS, 0 );
    }
    return progressed;
}

static BOOL restore_client_surface_compositor_target(
    struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_target *target =
        find_client_surface_compositor_target( job->handoff_toplevel );
    RECT rect = {job->destination_x, job->destination_y,
                 job->destination_x + job->width, job->destination_y + job->height};

    process_client_surface_present_events();
    if (!target || !target->window ||
        target->window != job->destination ||
        target->window_width != job->window_width ||
        target->window_height != job->window_height)
        return FALSE;
    job->valid_width = target->published_width;
    job->valid_height = target->published_height;
    if (IsRectEmpty( &target->restore_rect )) target->restore_rect = rect;
    else add_bounds_rect( &target->restore_rect, &rect );
    /* A newer Present may have reached the window before its Complete event
     * reaches us. Do not overwrite it with the previously published image,
     * or make the GUI caller wait for that Present. */
    if (!client_surface_compositor_restore_ready( target ))
    {
        TRACE( "deferring restore for target %p rect %s\n",
               target->toplevel, wine_dbgstr_rect( &target->restore_rect ) );
        return TRUE;
    }
    return restore_client_surface_compositor_pixels( target );
}

static BOOL validate_client_surface_pixmap( Pixmap pixmap, unsigned int min_width,
                                            unsigned int min_height,
                                            unsigned int *source_depth )
{
    Window root;
    unsigned int width, height, border, pixmap_depth;
    int x, y, error = 0;
    BOOL ret;

    X11DRV_expect_error( client_surface_compositor_display,
                         client_surface_compositor_error, &error );
    ret = XGetGeometry( client_surface_compositor_display, pixmap, &root, &x, &y,
                        &width, &height, &border, &pixmap_depth );
    XSync( client_surface_compositor_display, False );
    X11DRV_check_error();
    if (!ret || error || width < min_width || height < min_height) return FALSE;
    *source_depth = pixmap_depth;
    return TRUE;
}

static BOOL client_surface_source_cache_matches( const struct client_surface_source_cache *cache,
                                                 const struct client_surface_handoff_slot *slot )
{
    return cache->pixmap == slot->source && cache->target_epoch == slot->target_epoch &&
           cache->width == slot->width && cache->height == slot->height && cache->visual == slot->source_visual;
}

static BOOL get_client_surface_compositor_source(
    struct client_surface_compositor_binding *binding, unsigned int index,
    const struct client_surface_handoff_slot *slot, Pixmap *source,
    unsigned int *source_depth )
{
    struct client_surface_source_cache *cache = &binding->sources[index];

    if (!(slot->flags & CLIENT_SURFACE_HANDOFF_COPY_SOURCE)) return FALSE;
    /* Cache validated metadata, without owning or retaining the producer's
     * XID. The slot pins this independent image through copy completion, and
     * every actual read still participates in the X error validation boundary. */
    if (!client_surface_source_cache_matches( cache, slot ))
    {
        if (!validate_client_surface_pixmap( slot->source, slot->width, slot->height, source_depth ))
            return FALSE;
        *cache = (struct client_surface_source_cache){slot->source, slot->target_epoch,
            slot->source_visual, slot->width, slot->height, *source_depth};
        TRACE( "validated source pixmap %#lx visual %#lx depth %u size %ux%u slot %u\n",
               cache->pixmap, cache->visual, cache->depth, cache->width, cache->height, index );
    }
    *source = cache->pixmap;
    *source_depth = cache->depth;
    return TRUE;
}

static BOOL cache_client_surface_handoff( struct client_surface_compositor_binding *binding,
                                          unsigned int index, UINT64 control )
{
    struct client_surface_handoff_channel *channel = binding->channel;
    struct client_surface_handoff_slot frame = channel->slots[index];
    struct client_surface_cached_image *image = &binding->spare_image, previous;
    Display *display = client_surface_compositor_display;
    unsigned int depth;
    Pixmap source;
    BOOL success = FALSE;
    int error = 0;

    TRACE( "reading handoff hwnd %p identity %s sequence %s into owner cache\n", binding->window,
           wine_dbgstr_longlong( binding->identity ),
           wine_dbgstr_longlong( control ) );
    trace_client_surface_source( "claim", binding, control, frame.source_sequence, 0, frame.source, TRUE );
    if (frame.cookie != binding->cookie || frame.identity != binding->identity ||
        frame.producer_process != binding->process ||
        frame.window != wine_server_user_handle( binding->window ) ||
        frame.toplevel != wine_server_user_handle( binding->toplevel ) ||
        !(frame.flags & CLIENT_SURFACE_HANDOFF_NATIVE_X11) || !frame.width || !frame.height ||
        !frame.source_visual) goto done;
    if (binding->latest_image.pixmap && frame.source_sequence < binding->latest_frame.source_sequence)
    {
        success = TRUE;
        goto done;
    }
    if (!get_client_surface_compositor_source( binding, index, &frame, &source, &depth )) goto done;
    if (image->width != frame.width || image->height != frame.height || image->depth != depth)
    {
        UINT64 bytes = client_surface_pixmap_bytes( frame.width, frame.height, depth );

        free_client_surface_cached_image( image );
        if (!client_surface_reserve_memory( CLIENT_SURFACE_MEMORY_SOURCE, bytes )) goto done;
        image->bytes = bytes;
        image->width = frame.width;
        image->height = frame.height;
        image->depth = depth;
    }
    /* Keep the last complete cache intact until the new full image and its
     * error check succeed. Neither buffer borrows a producer XID. */
    X11DRV_expect_error( display, client_surface_compositor_error, &error );
    if (!image->pixmap)
        image->pixmap = XCreatePixmap( display, root_window, image->width, image->height, image->depth );
    if (image->pixmap && !image->gc) image->gc = XCreateGC( display, image->pixmap, 0, NULL );
    if (image->gc) XCopyArea( display, source, image->pixmap, image->gc,
                            0, 0, frame.width, frame.height, 0, 0 );
    XSync( display, False );
    X11DRV_check_error();
    if (!image->gc || error)
    {
        free_client_surface_cached_image( image );
        goto done;
    }
    previous = binding->latest_image;
    binding->latest_image = *image;
    *image = previous;
    frame.source = binding->latest_image.pixmap;
    binding->latest_frame = frame;
    binding->latest_control = control;
    binding->latest_index = index;
    success = TRUE;
done:
    trace_client_surface_source( "cache_copy", binding, control, frame.source_sequence,
                                 0, binding->latest_image.pixmap, success );
    /* The acquire load of producer_sequence made this descriptor immutable.
     * Return it only after every source read and its X error check completed.
     * A failed replacement preserves the previous owner-local image. */
    __atomic_store_n( &channel->consumer_sequence, control, __ATOMIC_RELEASE );
    if (!success && !binding->latest_image.pixmap)
        __atomic_store_n( &channel->closed, 1, __ATOMIC_RELEASE );
    trace_client_surface_source( "cache_release", binding, control, frame.source_sequence,
                                 0, binding->latest_image.pixmap, success );
    TRACE( "%s handoff hwnd %p identity %s sequence %s after owner cache copy\n",
           success || binding->latest_image.pixmap ? "released" : "lost", binding->window,
           wine_dbgstr_longlong( binding->identity ),
           wine_dbgstr_longlong( control ) );
    client_surface_handoff_wake_release( binding->pool->shared );
    return success;
}

static BOOL get_client_surface_compositor_catchup(
    const struct client_surface_compositor_target *target,
    const struct client_surface_compositor_frame *frame, RECT *rect )
{
    UINT64 revision;
    BOOL initialized = FALSE;

    if (frame->revision == target->revision) return TRUE;
    if (!frame->revision || frame->revision > target->revision ||
        target->revision - frame->revision > ARRAY_SIZE(target->damages))
    {
        *rect = (RECT){0, 0, target->window_width, target->window_height};
        return TRUE;
    }

    for (revision = frame->revision + 1; revision <= target->revision; ++revision)
    {
        const struct client_surface_compositor_damage *damage =
            &target->damages[revision % ARRAY_SIZE(target->damages)];

        if (damage->revision != revision)
        {
            *rect = (RECT){0, 0, target->window_width, target->window_height};
            return TRUE;
        }
        if (!initialized)
        {
            *rect = damage->rect;
            initialized = TRUE;
        }
        else
        {
            rect->left = min( rect->left, damage->rect.left );
            rect->top = min( rect->top, damage->rect.top );
            rect->right = max( rect->right, damage->rect.right );
            rect->bottom = max( rect->bottom, damage->rect.bottom );
        }
    }
    return TRUE;
}

static unsigned long convert_client_surface_component( unsigned long pixel,
                                                        unsigned long source_mask,
                                                        unsigned long destination_mask )
{
    unsigned int source_shift = 0, destination_shift = 0;
    UINT64 value, source_max, destination_max;

    if (!destination_mask) return 0;
    if (!source_mask) return destination_mask;
    while (!(source_mask & (1ul << source_shift))) ++source_shift;
    while (!(destination_mask & (1ul << destination_shift))) ++destination_shift;
    source_max = source_mask >> source_shift;
    destination_max = destination_mask >> destination_shift;
    value = (pixel & source_mask) >> source_shift;
    return ((value * destination_max + source_max / 2) / source_max) << destination_shift;
}

static BOOL copy_client_surface_image( Display *display, Pixmap source, Pixmap destination,
                                       GC gc, VisualID source_id, VisualID destination_id,
                                       unsigned int source_width, unsigned int source_height,
                                       const RECT *rect )
{
    XVisualInfo source_template = {.visualid = source_id};
    XVisualInfo destination_template = {.visualid = destination_id};
    XVisualInfo *source_visual = NULL, *destination_visual = NULL;
    const XPixmapFormatValues *format;
    XImage *input = NULL, *output = NULL;
    unsigned int width = rect->right - rect->left, height = rect->bottom - rect->top;
    unsigned int x, y, source_y;
    unsigned long source_alpha, destination_alpha;
    UINT64 input_bytes, output_bytes, reserved = 0;
    int count;
    BOOL ret = FALSE;

    /* Keep the exceptional conversion path on the owner connection too.
     * The caller retains the source until its final XSync, covering both readback
     * and upload. The destination GC carries the exact scene clip. */
    if (!(source_visual = XGetVisualInfo( display, VisualIDMask, &source_template, &count )) ||
        !count || (source_visual->class != TrueColor && source_visual->class != DirectColor))
        goto done;
    if (!(destination_visual = XGetVisualInfo( display, VisualIDMask, &destination_template, &count )) ||
        !count || (destination_visual->class != TrueColor && destination_visual->class != DirectColor))
        goto done;
    if (!(output = XCreateImage( display, destination_visual->visual, destination_visual->depth,
                                ZPixmap, 0, NULL, width, height, 32, 0 )))
        goto done;
    if (output->bytes_per_line <= 0 || height > ~(SIZE_T)0 / output->bytes_per_line)
        goto done;
    if (!(format = pixmap_formats[source_visual->depth]) || format->bits_per_pixel <= 0 ||
        format->scanline_pad <= 0 || format->scanline_pad % 8)
        goto done;
    input_bytes = ((UINT64)source_width * format->bits_per_pixel + format->scanline_pad - 1) /
                  format->scanline_pad * (format->scanline_pad / 8);
    if (!input_bytes || source_height > ~(UINT64)0 / input_bytes) goto done;
    input_bytes *= source_height;
    output_bytes = (UINT64)output->bytes_per_line * height;
    if (input_bytes > ~(UINT64)0 - output_bytes ||
        !client_surface_reserve_memory( CLIENT_SURFACE_MEMORY_STAGING, input_bytes + output_bytes ))
        goto done;
    reserved = input_bytes + output_bytes;
    if (!(input = XGetImage( display, source, 0, 0, source_width, source_height,
                             AllPlanes, ZPixmap )) ||
        !(output->data = calloc( height, output->bytes_per_line )))
        goto done;
    source_alpha = ((1ull << source_visual->depth) - 1) &
                   ~(source_visual->red_mask | source_visual->green_mask | source_visual->blue_mask);
    destination_alpha = ((1ull << destination_visual->depth) - 1) &
                        ~(destination_visual->red_mask | destination_visual->green_mask |
                          destination_visual->blue_mask);
    for (y = 0; y < height; ++y)
    {
        source_y = (UINT64)y * source_height / height;
        for (x = 0; x < width; ++x)
        {
            unsigned long pixel = XGetPixel( input, (UINT64)x * source_width / width, source_y );
            unsigned long converted =
                convert_client_surface_component( pixel, source_visual->red_mask, destination_visual->red_mask ) |
                convert_client_surface_component( pixel, source_visual->green_mask, destination_visual->green_mask ) |
                convert_client_surface_component( pixel, source_visual->blue_mask, destination_visual->blue_mask ) |
                convert_client_surface_component( pixel, source_alpha, destination_alpha );

            XPutPixel( output, x, y, converted );
        }
    }
    XPutImage( display, destination, gc, output, 0, 0, rect->left, rect->top, width, height );
    TRACE( "software owner copy %ux%u to %ux%u visual %#lx -> %#lx\n",
           source_width, source_height, width, height, source_id, destination_id );
    ret = TRUE;
done:
    if (output) XDestroyImage( output );
    if (input) XDestroyImage( input );
    client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING, reserved );
    if (destination_visual) XFree( destination_visual );
    if (source_visual) XFree( source_visual );
    return ret;
}

static void discard_client_surface_compositor_gc( struct client_surface_compositor_frame *frame )
{
    Display *display = client_surface_compositor_display;
    int error = 0;

    client_surface_xcb_free_gc( display, &frame->xcb_gc );
    if (!frame->gc) return;
    X11DRV_expect_error( display, client_surface_compositor_error, &error );
    XFreeGC( display, frame->gc );
    XSync( display, False );
    X11DRV_check_error();
    frame->gc = NULL;
}

/* Owner-local placement of an immutable completed image. Source damage may
 * become a full copy for scene replay without changing the cached frame. */
struct client_surface_composition_plan
{
    UINT64 generation;
    UINT64 epoch;
    RECT destination;
    RECT source_damage;
    const RGNDATA *clip;
};

static BOOL copy_client_surface_handoff_to_frame(
    struct client_surface_compositor_target *target,
    struct client_surface_compositor_frame *frame, Pixmap source, unsigned int source_depth,
    const struct client_surface_handoff_slot *slot,
    const struct client_surface_composition_plan *plan, const RECT *damage,
    BOOL batch, BOOL *pending )
{
    Display *display = client_surface_compositor_display;
    const XRectangle *clips = (const XRectangle *)plan->clip->Buffer;
    unsigned int clip_count = plan->clip->rdh.nCount;
    RECT catchup = {0};
    BOOL clipped, incoming_full, needs_catchup, native, overlay_copied = TRUE;
    unsigned int destination_width = plan->destination.right - plan->destination.left;
    unsigned int destination_height = plan->destination.bottom - plan->destination.top;
    int error = 0;
    GC gc;

    if (!target->latest || !get_client_surface_compositor_catchup( target, frame, &catchup ))
        return FALSE;
    clipped = clip_count != 1 || clips[0].x || clips[0].y ||
              clips[0].width != destination_width || clips[0].height != destination_height;
    incoming_full = !clipped && damage->left == 0 && damage->top == 0 &&
                    (unsigned int)damage->right >= target->window_width &&
                    (unsigned int)damage->bottom >= target->window_height;
    needs_catchup = !incoming_full && frame->pixmap != target->latest &&
                    frame->revision != target->revision && !IsRectEmpty( &catchup );
    native = source_depth == target->depth && slot->source_visual == target->visual &&
             slot->width == destination_width && slot->height == destination_height;

    if (batch && client_surface_copy_batch.asynchronous)
    {
        BOOL copied;

        assert( native );
        copied = client_surface_xcb_copy( display, source, frame->pixmap, &frame->xcb_gc,
            needs_catchup ? target->latest : 0, &catchup,
            &plan->source_damage, &plan->destination, clips, clip_count, clipped,
            &client_surface_copy_batch.requests[client_surface_copy_batch.count - 1], FALSE );
        /* Subsequent members append to this private image in request order;
         * another checkpoint copy would overwrite their earlier neighbors.
         * The revision is invalidated if any request in the batch fails. */
        if (copied && needs_catchup) frame->revision = target->revision;
        return copied;
    }

    TRACE_(csperf)( "ticks=%llu event=copy_route native=%u full=%u transaction=%u assembly=%u "
                   "mailbox=%u ticket=%u latest=%u published=%u inflight=%u\n",
                   client_surface_perf_time(), native, incoming_full, !!plan->generation,
                   target->assembly_pending, target->mailbox_pending,
                   !!target->mailbox_publish_generation, frame->pixmap == target->latest,
                   frame->pixmap == target->published, !!frame->serial );
    if (!batch && !plan->generation && native &&
        !target->assembly_pending && !target->mailbox_pending &&
        frame->pixmap != target->latest && frame->pixmap != target->published &&
        client_surface_xcb_copy( display, source, frame->pixmap, &frame->xcb_gc,
                                 needs_catchup ? target->latest : 0, &catchup,
                                 &plan->source_damage, &plan->destination, clips, clip_count, clipped,
                                 &frame->copy_request, TRUE ))
    {
        *pending = TRUE;
        return TRUE;
    }

    if (!batch) X11DRV_expect_error( display, client_surface_compositor_error, &error );
    if (!(gc = frame->gc)) gc = frame->gc = XCreateGC( display, frame->pixmap, 0, NULL );
    if (gc)
    {
        /* Clip belongs to this copy's immutable scene member. Reset it before
         * copying the owner checkpoint into a reused output frame. */
        XSetClipMask( display, gc, None );
        XSetClipOrigin( display, gc, 0, 0 );
        if (needs_catchup)
            XCopyArea( display, target->latest, frame->pixmap, gc,
                       catchup.left, catchup.top,
                       catchup.right - catchup.left, catchup.bottom - catchup.top,
                       catchup.left, catchup.top );
        if (clip_count)
        {
            if (clipped)
                XSetClipRectangles( display, gc, plan->destination.left,
                                    plan->destination.top, (XRectangle *)clips, clip_count, YXBanded );
            if (overlay_copied && native)
                XCopyArea( display, source, frame->pixmap, gc,
                           plan->source_damage.left, plan->source_damage.top,
                           plan->source_damage.right - plan->source_damage.left,
                           plan->source_damage.bottom - plan->source_damage.top,
                           plan->destination.left + plan->source_damage.left,
                           plan->destination.top + plan->source_damage.top );
            else if (overlay_copied)
            {
                overlay_copied = X11DRV_XRender_CopyClientSurface(
                    display, source, slot->source_visual, frame->pixmap, target->visual,
                    slot->width, slot->height, &plan->destination,
                    clipped ? clips : NULL, clipped ? clip_count : 0, 0, 0 );
                if (!overlay_copied)
                    overlay_copied = copy_client_surface_image(
                        display, source, frame->pixmap, gc, slot->source_visual, target->visual,
                        slot->width, slot->height, &plan->destination );
            }
        }
    }
    if (!batch)
    {
        XSync( display, False );
        X11DRV_check_error();
    }
    if (!gc || error || !overlay_copied)
    {
        if (!batch) discard_client_surface_compositor_gc( frame );
        return FALSE;
    }
    if (needs_catchup) frame->revision = target->revision;
    return TRUE;
}

static BOOL complete_client_surface_handoff_generation( struct client_surface_compositor_target *target,
                                                        UINT64 generation,
                                                        UINT64 scene_generation )
{
    BOOL accepted = FALSE;
    NTSTATUS status;

    SERVER_START_REQ( complete_client_surface_handoffs )
    {
        req->handle = wine_server_user_handle( target->toplevel );
        req->generation = generation;
        req->scene_generation = scene_generation;
        wine_server_add_data( req, target->receipts, target->scene.count * sizeof(*target->receipts) );
        status = wine_server_call( req );
        if (!status) accepted = reply->accepted;
    }
    SERVER_END_REQ;
    TRACE( "completed owner generation %s epoch %s status %#lx accepted %u\n",
           wine_dbgstr_longlong( generation ), wine_dbgstr_longlong( scene_generation ),
           (unsigned long)status, accepted );
    return !status && accepted;
}

static BOOL publish_client_surface_handoff_generation( HWND toplevel, UINT64 generation,
                                                       UINT64 scene_generation, BOOL success )
{
    BOOL accepted = FALSE;
    NTSTATUS status;

    SERVER_START_REQ( publish_client_surface_handoff )
    {
        req->handle = wine_server_user_handle( toplevel );
        req->generation = generation;
        req->scene_generation = scene_generation;
        req->success = success;
        status = wine_server_call( req );
        if (!status) accepted = reply->accepted;
    }
    SERVER_END_REQ;
    TRACE( "published owner generation %s epoch %s status %#lx accepted %u success %u\n",
           wine_dbgstr_longlong( generation ), wine_dbgstr_longlong( scene_generation ),
           (unsigned long)status, accepted, success );
    return !status && accepted && success;
}

static struct client_surface_compositor_frame *find_client_surface_pending_publication(
    struct client_surface_compositor_target *target, UINT64 generation, UINT64 epoch )
{
    unsigned int i;

    if (target->mailbox_pending && target->mailbox_publish_generation == generation &&
        target->mailbox_publish_epoch == epoch)
        return &target->frames[target->mailbox_frame];
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        if (target->frames[i].publish_pending &&
            target->frames[i].publish_generation == generation &&
            target->frames[i].publish_epoch == epoch) return &target->frames[i];
    return NULL;
}

static BOOL client_surface_handoff_generation_assembled(
    struct client_surface_compositor_target *target,
    struct client_surface_compositor_frame *frame, UINT64 generation, UINT64 epoch )
{
    return target->scene.valid && target->scene.epoch == epoch && target->scene.count &&
           target->assembly_pending && target->assembly_generation == generation &&
           target->assembly_epoch == epoch && target->assembly_frame == frame - target->frames &&
           target->received == target->scene.count;
}

static BOOL publish_client_surface_handoff_assembly(
    struct client_surface_compositor_target *target,
    struct client_surface_compositor_frame *frame, UINT64 generation, UINT64 epoch )
{
    BOOL visible = FALSE, queued = FALSE, deferred = FALSE;
    RECT full = {0, 0, target->window_width, target->window_height};

    if (!complete_client_surface_handoff_generation( target, generation, epoch ))
        goto done;

#ifdef SONAME_LIBXPRESENT
    if (usexpresent && !target->mailbox_pending &&
        count_client_surface_compositor_frames( target ) < CLIENT_SURFACE_COMPOSITOR_MAX_INFLIGHT)
        visible = queued = submit_client_surface_present( target, frame, generation, epoch, NULL );
    else if (usexpresent)
    {
        target->mailbox_frame = frame - target->frames;
        target->mailbox_pending = TRUE;
        target->mailbox_publish_generation = generation;
        target->mailbox_publish_epoch = epoch;
        visible = deferred = TRUE;
    }
#endif
    if (!visible)
        visible = client_surface_copy_on_compositor( frame->pixmap, target->window,
                                                     0, 0, 0, 0,
                                                     target->width, target->height );
    if (visible && !queued && !deferred)
    {
        target->published = frame->pixmap;
        target->published_width = frame->width;
        target->published_height = frame->height;
    }
    if (!queued && !deferred)
        publish_client_surface_handoff_generation( target->toplevel, generation, epoch, visible );
    if (visible) note_client_surface_compositor_damage( target, frame, &full );

done:
    /* Receipts describe the private assembled image. Source storage was
     * returned after each copy and is independent of this publication. */
    finish_client_surface_compositor_assembly( target, !visible );
    return visible;
}

static void complete_client_surface_copy_batch( struct client_surface_copy_batch *batch, BOOL success )
{
    struct client_surface_compositor_target *target = batch->target;
    struct client_surface_compositor_frame *frame = batch->frame;
    unsigned int i, count = batch->count;
    UINT64 generation, epoch;
    BOOL assembly_valid;

    if (!count) return;
    generation = batch->copies[0].generation;
    epoch = batch->copies[0].epoch;
    assembly_valid = target->assembly_pending && target->scene.valid &&
        target->assembly_generation == generation && target->assembly_epoch == epoch &&
        target->scene.epoch == epoch && target->assembly_frame == frame - target->frames;
    batch->count = 0;
    for (i = 0; i < count; ++i)
    {
        const struct client_surface_owner_copy *copy = &batch->copies[i];
        struct client_surface_compositor_binding *binding = copy->binding;

        trace_client_surface_source( copy->replay ?
                                     (batch->asynchronous ? "replay_copy_async" : "replay_copy_sync") :
                                     (batch->asynchronous ? "copy_async" : "copy_sync"),
                                     binding, copy->control, copy->sequence,
                                     target->window, frame->pixmap, success && assembly_valid );
        if (success && assembly_valid)
        {
            struct client_surface_handoff_receipt *receipt = &target->receipts[binding->scene_index];

            if (!receipt->source_generation) ++target->received;
            *receipt = (struct client_surface_handoff_receipt){
                .handle = wine_server_user_handle( binding->window ),
                .process = binding->process,
                .surface = binding->identity,
                .cookie = binding->cookie,
                .source_generation = copy->sequence,
                .buffer_index = copy->buffer_index,
            };
        }
        else if (!success)
        {
            binding->source_epoch = binding->source_sequence = 0;
            binding->replay_epoch = 0;
        }
    }
    TRACE( "owner copy batch %u sources, success %u, generation %s epoch %s pixmap %#lx async %u\n",
           count, success, wine_dbgstr_longlong( generation ), wine_dbgstr_longlong( epoch ),
           frame->pixmap, batch->asynchronous );
    if (!success)
    {
        discard_client_surface_compositor_gc( frame );
        finish_client_surface_compositor_assembly( target, TRUE );
        wake_client_surface_compositor();
    }
    else if (client_surface_handoff_generation_assembled( target, frame, generation, epoch ))
        publish_client_surface_handoff_assembly( target, frame, generation, epoch );
}

static void flush_client_surface_copy_batch(void)
{
    unsigned int i;

    if (!client_surface_copy_batch.count) return;
    if (client_surface_copy_batch.asynchronous)
    {
        for (i = 0; i < ARRAY_SIZE(client_surface_pending_batches); ++i)
            if (!client_surface_pending_batches[i].count) break;
        assert( i < ARRAY_SIZE(client_surface_pending_batches) );
        client_surface_xcb_flush( client_surface_compositor_display,
            &client_surface_copy_batch.requests[client_surface_copy_batch.count - 1] );
        client_surface_pending_batches[i] = client_surface_copy_batch;
        ++client_surface_pending_batch_count;
        client_surface_copy_batch.target->copy_frame = client_surface_copy_batch.frame;
        client_surface_copy_batch.count = 0;
        return;
    }
    /* Visual/scale fallback still uses Xlib. One synchronous error scope
     * proves this batch, before fixing receipts and releasing its sources. */
    XSync( client_surface_compositor_display, False );
    X11DRV_check_error();
    complete_client_surface_copy_batch( &client_surface_copy_batch, !client_surface_copy_batch.error );
}

static BOOL publish_client_surface_handoff_frame(
    struct client_surface_compositor_target *target,
    struct client_surface_compositor_frame *frame,
    struct client_surface_compositor_frame *previous_publish )
{
    BOOL composed = FALSE;

#ifdef SONAME_LIBXPRESENT
    if (usexpresent && !target->mailbox_pending &&
        count_client_surface_compositor_frames( target ) < CLIENT_SURFACE_COMPOSITOR_MAX_INFLIGHT)
        composed = submit_client_surface_present( target, frame, 0, 0, NULL );
    else if (usexpresent)
    {
        target->mailbox_frame = frame - target->frames;
        target->mailbox_pending = TRUE;
        if (previous_publish != frame)
        {
            target->mailbox_publish_generation = 0;
            target->mailbox_publish_epoch = 0;
        }
        composed = TRUE;
    }
#endif
    if (!composed)
        composed = client_surface_copy_on_compositor( frame->pixmap, target->window,
                                                      0, 0, 0, 0, target->width, target->height );
    if (composed && !frame->serial && !target->mailbox_pending)
    {
        target->published = frame->pixmap;
        target->published_width = frame->width;
        target->published_height = frame->height;
    }
    return composed;
}

static BOOL process_client_surface_copy_replies(void)
{
    struct client_surface_compositor_target *target;
    unsigned int i;
    BOOL progressed = FALSE, success;

    for (i = 0; client_surface_pending_batch_count && i < ARRAY_SIZE(client_surface_pending_batches); ++i)
    {
        struct client_surface_copy_batch *batch = &client_surface_pending_batches[i];

        if (!batch->count || !client_surface_xcb_poll_batch( client_surface_compositor_display,
                                                            batch->requests, batch->count, &success )) continue;
        progressed = TRUE;
        batch->target->copy_frame = NULL;
        --client_surface_pending_batch_count;
        complete_client_surface_copy_batch( batch, success && !batch->error );
    }
    for (target = client_surface_compositor_targets; target; target = target->next)
    {
        struct client_surface_compositor_frame *frame = target->copy_frame;
        struct client_surface_compositor_binding *binding;
        UINT64 control;

        if (!frame || !frame->copy_binding || !client_surface_xcb_poll( client_surface_compositor_display,
                                               &frame->copy_request, &success )) continue;
        progressed = TRUE;
        binding = frame->copy_binding;
        control = frame->copy_control;
        target->copy_frame = NULL;
        frame->copy_binding = NULL;
        TRACE( "validated owner copy request %u pixmap %#lx success %u\n",
               frame->copy_request.cookies[0], frame->pixmap, success );
        trace_client_surface_source( frame->copy_replay ? "replay_copy_async" : "copy_async",
                                     binding, control, frame->copy_sequence,
                                     target->window, frame->pixmap, success );
        if (!success)
        {
            binding->source_epoch = binding->source_sequence = 0;
            binding->replay_epoch = 0;
            frame->revision = 0;
            discard_client_surface_compositor_gc( frame );
        }
        if (success)
        {
            note_client_surface_compositor_damage( target, frame, &frame->copy_damage );
            publish_client_surface_handoff_frame( target, frame, NULL );
        }
    }
    return progressed;
}

/* Use the same immutable source predicate for composition and native owner
 * repair. Backend capability and consumed channel sequences are not cache proof. */
static BOOL client_surface_cached_frame_matches_layout( const struct client_surface_compositor_binding *binding,
                                                         const struct client_surface_scene_layout *layout )
{
    const struct client_surface_handoff_slot *slot = &binding->latest_frame;
    const RECT *destination = &layout->geometry.monitor_rect;
    unsigned int width = destination->right > destination->left ? destination->right - destination->left : 0;
    unsigned int height = destination->bottom > destination->top ? destination->bottom - destination->top : 0;

    return binding->latest_image.pixmap && slot->cookie == binding->cookie && slot->identity == binding->identity &&
           slot->producer_process == binding->process && slot->window == wine_server_user_handle( binding->window ) &&
           slot->toplevel == wine_server_user_handle( binding->toplevel ) &&
           (slot->flags & CLIENT_SURFACE_HANDOFF_NATIVE_X11) && width && height &&
           slot->width && slot->height && slot->source_visual &&
           ((slot->width == width && slot->height == height) ||
            (slot->width == layout->geometry.virtual_rect.right - layout->geometry.virtual_rect.left &&
             slot->height == layout->geometry.virtual_rect.bottom - layout->geometry.virtual_rect.top)) &&
           slot->damage.left >= 0 && slot->damage.top >= 0 && !IsRectEmpty( &slot->damage ) &&
           (unsigned int)slot->damage.right <= slot->width && (unsigned int)slot->damage.bottom <= slot->height;
}

static BOOL repair_client_surface_compositor_owner( HWND toplevel )
{
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( toplevel );
    struct client_surface_handoff_receipt *receipts;
    unsigned int i;
    BOOL accepted = FALSE;

    if (!target || !target->scene.valid || !target->scene.count ||
        !client_surface_scene_snapshot_current( toplevel, target->scene.epoch )) return FALSE;
    if (!(receipts = calloc( target->scene.count, sizeof(*receipts) ))) return FALSE;
    for (i = 0; i < target->scene.count; ++i)
    {
        struct client_surface_compositor_binding *binding = target->scene.members[i];

        if (!client_surface_compositor_binding_is_live( binding ) ||
            !client_surface_cached_frame_matches_layout( binding, &target->scene.layouts[i] )) goto done;
        receipts[i] = (struct client_surface_handoff_receipt){
            .handle = wine_server_user_handle( binding->window ), .process = binding->process,
            .surface = binding->identity, .cookie = binding->cookie,
            .source_generation = binding->latest_frame.source_sequence, .buffer_index = binding->latest_index,
        };
    }
    SERVER_START_REQ( request_client_surface_owner_repair )
    {
        req->handle = wine_server_user_handle( toplevel );
        req->scene_id = target->scene.epoch;
        wine_server_add_data( req, receipts, target->scene.count * sizeof(*receipts) );
        if (!wine_server_call( req )) accepted = reply->accepted;
    }
    SERVER_END_REQ;
    TRACE( "owner cache repair hwnd %p scene %s images %u accepted %u\n", toplevel,
           wine_dbgstr_longlong( target->scene.epoch ), target->scene.count, accepted );
    if (accepted)
    {
        /* A repair of an existing assembly can keep its scene ID. Backing
         * damage must nevertheless replay every retained image in that scene. */
        target->replay_member = 0;
        for (i = 0; i < target->scene.count; ++i) target->scene.members[i]->replay_epoch = 0;
    }
done:
    free( receipts );
    return accepted;
}

static BOOL compose_client_surface_cached_frame( struct client_surface_compositor_binding *binding )
{
    const struct client_surface_handoff_slot *slot;
    struct client_surface_composition_plan plan;
    struct client_surface_scene current;
    const struct client_surface_scene_layout *layout;
    struct client_surface_compositor_target *target;
    struct client_surface_compositor_frame *frame = NULL, *previous_publish = NULL;
    UINT64 control;
    unsigned int buffer_index;
    Pixmap source = 0;
    RECT damage;
    unsigned int destination_width, destination_height, source_depth = 0;
    BOOL composed = FALSE, copied = FALSE, dropped = TRUE, batch, pending = FALSE, asynchronous, replay;

    target = find_client_surface_compositor_target( binding->toplevel );
    if (!target || target->copy_frame || target->quiescing || !target->scene.valid) return FALSE;
    if (client_surface_pending_batch_count == ARRAY_SIZE(client_surface_pending_batches)) return FALSE;
    if (!binding->latest_image.pixmap) return FALSE;
    /* From here onwards only owner storage is read. The producer has already
     * received its slot, and may overwrite or destroy it during publication. */
    slot = &binding->latest_frame;
    control = binding->latest_control;
    buffer_index = binding->latest_index;
    source = binding->latest_image.pixmap;
    source_depth = binding->latest_image.depth;
    replay = binding->source_sequence == slot->source_sequence;
    if (!client_surface_get_toplevel_scene( binding->toplevel, &current ) ||
        current.epoch != target->scene.epoch ||
        current.mode == CLIENT_SURFACE_PRESENTATION_DIRECT) return FALSE;
    if (replay) trace_client_surface_source( "replay_cache", binding, control,
                                             slot->source_sequence, target->window, source, TRUE );
    /* Only the owner's current transaction can publish this image. A new
     * scene may reuse its pixels, and a late frame cannot reopen a completed
     * assembly merely because it was submitted during that publication. */
    plan.generation = current.generation;
    plan.epoch = current.epoch;
    plan.source_damage = slot->damage;
    /* Cache reception already validated the native source. Compatible scene
     * members can share the existing checked output batch without opening
     * another Xlib error scope or retaining any producer storage. */
    if (client_surface_copy_batch.count &&
        (client_surface_copy_batch.target != target || !plan.generation ||
         client_surface_copy_batch.copies[0].generation != plan.generation ||
         client_surface_copy_batch.copies[0].epoch != plan.epoch))
        flush_client_surface_copy_batch();
    if (target->copy_frame ||
        client_surface_pending_batch_count == ARRAY_SIZE(client_surface_pending_batches)) goto retry;
    if (target->scene.epoch != plan.epoch || binding->scene_index >= target->scene.count ||
        target->scene.members[binding->scene_index] != binding ||
        (binding->source_epoch == plan.epoch && slot->source_sequence < binding->source_sequence))
    {
        dropped = TRUE;
        goto release;
    }
    layout = &target->scene.layouts[binding->scene_index];
    plan.destination = layout->geometry.monitor_rect;
    plan.clip = layout->clip;
    destination_width = plan.destination.right > plan.destination.left ?
                        plan.destination.right - plan.destination.left : 0;
    destination_height = plan.destination.bottom > plan.destination.top ?
                         plan.destination.bottom - plan.destination.top : 0;
    if (target->backing && target->frames[0].pixmap && target->frames[1].pixmap && target->window &&
        client_surface_cached_frame_matches_layout( binding, layout ))
    {
        dropped = FALSE;
        /* Placement and clip come from the owner's scene. Children may
         * extend outside the top-level; the scene clip and destination
         * drawable bound the copy, without changing its source mapping. */
        damage = plan.destination;
        /* Transactions need each participant's complete visible contribution.
         * Steady frames may use source damage; align the owner journal with
         * the actual source rectangle instead of declaring it fully replaced. */
        /* A missed or superseded source breaks incremental continuity. The
         * immutable image always contains the full frame, so recover by
         * copying it in full. Scaled copies use a conservative full journal. */
        if (plan.generation || (slot->flags & CLIENT_SURFACE_HANDOFF_FULL_DAMAGE) ||
            binding->source_epoch != plan.epoch || !slot->damage_base_sequence ||
            slot->damage_base_sequence != binding->source_sequence ||
            slot->width != destination_width || slot->height != destination_height)
            SetRect( &plan.source_damage, 0, 0, slot->width, slot->height );
        else
        {
            TRACE( "incremental owner copy hwnd %p sequence %s base %s damage %s\n", binding->window,
                   wine_dbgstr_longlong( slot->source_sequence ),
                   wine_dbgstr_longlong( slot->damage_base_sequence ), wine_dbgstr_rect( &plan.source_damage ) );
            damage.left += (UINT64)plan.source_damage.left * destination_width / slot->width;
            damage.top += (UINT64)plan.source_damage.top * destination_height / slot->height;
            damage.right = plan.destination.left +
                ((UINT64)plan.source_damage.right * destination_width + slot->width - 1) / slot->width;
            damage.bottom = plan.destination.top +
                ((UINT64)plan.source_damage.bottom * destination_height + slot->height - 1) / slot->height;
        }
        if (plan.generation)
            previous_publish = find_client_surface_pending_publication(
                target, plan.generation, plan.epoch );
        if (!previous_publish && target->assembly_pending &&
            (target->assembly_generation != plan.generation ||
             target->assembly_epoch != plan.epoch))
            finish_client_surface_compositor_assembly( target, TRUE );
        if (!plan.generation || previous_publish)
            frame = get_client_surface_compositor_frame( target );
        else if (target->assembly_pending &&
                 target->assembly_generation == plan.generation &&
                 target->assembly_epoch == plan.epoch)
            frame = &target->frames[target->assembly_frame];
        else
            frame = acquire_client_surface_compositor_assembly_frame( target );

        if (!frame)
        {
retry:
            /* Output backpressure retains the owner cache, never a producer
             * slot. Retry this image when output work makes progress. */
            binding->replay_epoch = 0;
            target->replay_member = min( target->replay_member, binding->scene_index );
            return FALSE;
        }
        if (plan.generation && !previous_publish && !target->assembly_pending)
        {
            target->assembly_pending = TRUE;
            target->assembly_generation = plan.generation;
            target->assembly_epoch = plan.epoch;
            target->assembly_frame = frame - target->frames;
        }

        /* Every participant is copied into one private assembly frame. Its
         * receipt survives the source release, allowing the producer to make
         * progress while other participants are still completing. */
        batch = plan.generation && !previous_publish;
        if (batch)
        {
            asynchronous = source_depth == target->depth && slot->source_visual == target->visual &&
                           slot->width == destination_width && slot->height == destination_height &&
                           client_surface_xcb_available( client_surface_compositor_display );
            if (client_surface_copy_batch.count &&
                client_surface_copy_batch.asynchronous != asynchronous)
            {
                flush_client_surface_copy_batch();
                if (target->copy_frame) goto retry;
            }
            if (!client_surface_copy_batch.count)
            {
                client_surface_copy_batch.target = target;
                client_surface_copy_batch.frame = frame;
                client_surface_copy_batch.error = 0;
                client_surface_copy_batch.asynchronous = asynchronous;
                if (!asynchronous)
                    X11DRV_expect_error( client_surface_compositor_display, client_surface_compositor_error,
                                         &client_surface_copy_batch.error );
            }
            assert( client_surface_copy_batch.frame == frame );
            client_surface_copy_batch.copies[client_surface_copy_batch.count++] =
                (struct client_surface_owner_copy){binding, buffer_index, control, plan.generation, plan.epoch,
                                                   slot->source_sequence, replay};
            client_surface_copy_batch.requests[client_surface_copy_batch.count - 1] =
                (struct client_surface_xcb_request){0};
        }
        copied = copy_client_surface_handoff_to_frame( target, frame, source, source_depth,
                                                       slot, &plan, &damage, batch, &pending );
        if (copied)
        {
            binding->source_epoch = plan.epoch;
            binding->source_sequence = slot->source_sequence;
            binding->replay_epoch = plan.epoch;
            frame->width = target->window_width;
            frame->height = target->window_height;
        }
        if (pending)
        {
            target->copy_frame = frame;
            frame->copy_binding = binding;
            frame->copy_index = buffer_index;
            frame->copy_control = control;
            frame->copy_sequence = slot->source_sequence;
            frame->copy_replay = replay;
            frame->copy_damage = damage;
            return TRUE;
        }
        if (batch)
        {
            if (!copied) client_surface_copy_batch.error = 1;
            if (!copied || client_surface_copy_batch.count == CLIENT_SURFACE_COPY_BATCH_SIZE)
                flush_client_surface_copy_batch();
            return TRUE;
        }
        else if (copied)
        {
            trace_client_surface_source( replay ? "replay_copy_sync" : "copy_sync", binding, control, slot->source_sequence,
                                         target->window, frame->pixmap, TRUE );
            note_client_surface_compositor_damage( target, frame, &damage );
            composed = publish_client_surface_handoff_frame( target, frame, previous_publish );
        }
    }
release:
    /* A resized or otherwise incompatible image is still valid storage.
     * Metadata rejection leaves it returned; a real copy/import error
     * still retires the binding through the normal failure path. */
    if (replay) binding->replay_epoch = target->scene.epoch;
    if (!copied)
        trace_client_surface_source( "discard", binding, control, slot->source_sequence,
                                     target->window, frame ? frame->pixmap : 0, dropped );
    if (!copied && !dropped) flush_client_surface_copy_batch();
    if (!copied && !dropped && plan.generation && !previous_publish && target &&
        target->assembly_pending && target->assembly_generation == plan.generation &&
        target->assembly_epoch == plan.epoch)
        finish_client_surface_compositor_assembly( target, TRUE );
    return composed;
}

static BOOL process_client_surface_handoffs(void)
{
    static UINT64 next_pool_id;
    struct client_surface_compositor_pool *pool, *next, *first = client_surface_compositor_pools;
    unsigned int pools = 0;
    BOOL progressed = FALSE;
    int budget = CLIENT_SURFACE_COPY_BATCH_SIZE;

    for (pool = client_surface_compositor_pools; pool; pool = pool->next)
    {
        if (pool->id == next_pool_id) first = pool;
        ++pools;
    }
    for (pool = first; pools--; pool = next)
    {
        unsigned int n, start = pool->next_word;

        next = pool->next ? pool->next : client_surface_compositor_pools;
        next_pool_id = next->id;
        ++pool->refs; /* Removing a lost final binding must not unmap the scan. */
        for (n = 0; n < CLIENT_SURFACE_HANDOFF_BITMAP_WORDS; ++n)
        {
            unsigned int word = (start + n) % CLIENT_SURFACE_HANDOFF_BITMAP_WORDS;
            UINT64 bits = __atomic_load_n( &pool->shared->ready_bitmap[word], __ATOMIC_ACQUIRE );

            while (bits)
            {
                unsigned int bit = __builtin_ctzll( bits ), index = word * 64 + bit;
                struct client_surface_compositor_binding *binding = pool->bindings[index], **cursor;
                struct client_surface_handoff_channel *channel = &pool->shared->channels[index];
                UINT64 consumed, produced;
                unsigned int frames = 0;

                bits &= bits - 1;
                if (!binding) continue;
                /* Clear the hint before acquiring the sequence. A concurrent
                 * publisher either appears in that load or leaves its bit set. */
                __atomic_fetch_and( &pool->shared->ready_bitmap[word], ~((UINT64)1 << bit), __ATOMIC_ACQ_REL );
                while (!__atomic_load_n( &channel->closed, __ATOMIC_ACQUIRE ))
                {
                    struct client_surface_compositor_target *target;

                    consumed = __atomic_load_n( &channel->consumer_sequence, __ATOMIC_RELAXED );
                    produced = __atomic_load_n( &channel->producer_sequence, __ATOMIC_ACQUIRE );
                    if (produced == consumed) break;
                    if (produced - consumed > CLIENT_SURFACE_HANDOFF_RING_SIZE)
                    {
                        __atomic_store_n( &channel->closed, 1, __ATOMIC_RELEASE );
                        break;
                    }
                    if (frames++ == CLIENT_SURFACE_HANDOFF_RING_SIZE)
                    {
                        __atomic_fetch_or( &pool->shared->ready_bitmap[word], (UINT64)1 << bit, __ATOMIC_RELEASE );
                        break;
                    }
                    target = find_client_surface_compositor_target( binding->toplevel );
                    flush_client_surface_copy_batch();
                    if (cache_client_surface_handoff( binding, consumed & (CLIENT_SURFACE_HANDOFF_RING_SIZE - 1),
                                                      consumed + 1 ))
                    {
                        binding->replay_epoch = 0;
                        if (target) target->replay_member = min( target->replay_member, binding->scene_index );
                    }
                    --budget;
                    progressed = TRUE;
                }
                if (!__atomic_load_n( &channel->closed, __ATOMIC_ACQUIRE )) continue;
                flush_client_surface_copy_batch();
                /* A checked copy retains the binding and owner cache until
                 * its reply arrives, including after producer death. */
                {
                    struct client_surface_compositor_target *target =
                        find_client_surface_compositor_target( binding->toplevel );

                    if (target && target->copy_frame)
                    {
                        __atomic_fetch_or( &pool->shared->ready_bitmap[word], (UINT64)1 << bit, __ATOMIC_RELEASE );
                        continue;
                    }
                }
                __atomic_fetch_and( &pool->shared->ready_bitmap[word], ~((UINT64)1 << bit), __ATOMIC_ACQ_REL );
                for (cursor = &client_surface_compositor_bindings; *cursor != binding; cursor = &(*cursor)->next)
                    assert( *cursor );
                remove_client_surface_compositor_binding( cursor );
            }
            pool->next_word = (word + 1) % CLIENT_SURFACE_HANDOFF_BITMAP_WORDS;
            /* Finish the bitmap word to prevent its first busy producer
             * from starving later bits, then give events and jobs a turn. */
            if (budget <= 0) break;
        }
        flush_client_surface_copy_batch();
        release_client_surface_compositor_pool( pool );
        if (budget <= 0) return TRUE;
    }
    return progressed;
}

static BOOL replay_client_surface_scene_sources(void)
{
    static HWND next_toplevel;
    struct client_surface_compositor_target *target, *next, *first = client_surface_compositor_targets;
    unsigned int targets = 0, budget = CLIENT_SURFACE_COPY_BATCH_SIZE;
    BOOL progressed = FALSE;

    /* Scene replay reads owner-local images. It never claims a returned
     * producer slot or depends on the producer retaining its previous XID. */
    for (target = client_surface_compositor_targets; target; target = target->next)
    {
        if (target->toplevel == next_toplevel) first = target;
        ++targets;
    }
    for (target = first; targets-- && budget; target = next)
    {
        next = target->next ? target->next : client_surface_compositor_targets;
        next_toplevel = next->toplevel;
        if (!target->scene.valid || target->quiescing || target->copy_frame) continue;
        while (target->replay_member < target->scene.count && budget)
        {
            struct client_surface_compositor_binding *binding = target->scene.members[target->replay_member];

            if (binding->latest_image.pixmap && binding->replay_epoch != target->scene.epoch)
            {
                compose_client_surface_cached_frame( binding );
                if (binding->latest_image.pixmap && binding->replay_epoch != target->scene.epoch) break;
            }
            ++target->replay_member;
            --budget;
            progressed = TRUE;
            if (target->copy_frame) break;
        }
        flush_client_surface_copy_batch();
    }
    return progressed;
}

static void drain_client_surface_notification( int fd )
{
    UINT64 value;
    int ret;

    do ret = read( fd, &value, sizeof(value) ); while (ret > 0 || (ret < 0 && errno == EINTR));
}

static void process_client_surface_compositor_mailboxes(void)
{
#ifdef SONAME_LIBXPRESENT
    struct client_surface_compositor_target *target;

    for (target = client_surface_compositor_targets; target; target = target->next)
        flush_client_surface_compositor_mailbox( target );
#endif
}

static void wait_client_surface_compositor_work(void)
{
    struct pollfd waiters[CLIENT_SURFACE_HANDOFF_MAX_POOLS_PER_CONSUMER + 2];
    struct client_surface_compositor_pool *pool;
    struct client_surface_compositor_job *job;
    struct client_surface_compositor_target *target;
    unsigned int count = 0;
    int ret, timeout = -1;

    drain_client_surface_notification( client_surface_compositor_notify[0] );
    for (pool = client_surface_compositor_pools; pool; pool = pool->next)
    {
        drain_client_surface_notification( pool->ready_fd );
        __atomic_store_n( &pool->shared->ready_parked, 1, __ATOMIC_RELEASE );
    }

    /* Drain before parking, then recheck authoritative work. A publisher
     * racing this scan leaves an unread notification; an earlier publisher
     * is found by the ready bitmap. Do not drain again before poll(). */
    process_client_surface_present_events();
    if (process_client_surface_compositor_jobs()) return;
    if (client_surface_compositor_display && XPending( client_surface_compositor_display )) return;
    /* Xlib may have read a reply into XCB while looking for events. Poll the
     * library buffer after that read, before waiting on the kernel fd. */
    if (process_client_surface_present_replies()) return;
    if (process_client_surface_copy_replies()) return;
    if (process_client_surface_handoffs()) return;
    process_client_surface_compositor_mailboxes();
    /* A synchronous copy may have buffered events and replies while handling
     * the sources above. Recheck after those requests as well, before poll. */
    if (client_surface_compositor_display && XPending( client_surface_compositor_display )) return;
    if (process_client_surface_present_replies()) return;
    if (process_client_surface_copy_replies()) return;
    pthread_mutex_lock( &client_surface_compositor_mutex );
    if (client_surface_compositor_head)
    {
        pthread_mutex_unlock( &client_surface_compositor_mutex );
        return;
    }
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    waiters[count++] = (struct pollfd){client_surface_compositor_notify[0], POLLIN, 0};
    if (client_surface_compositor_display)
        waiters[count++] = (struct pollfd){ConnectionNumber( client_surface_compositor_display ), POLLIN, 0};
    for (pool = client_surface_compositor_pools; pool; pool = pool->next)
    {
        assert( count < ARRAY_SIZE(waiters) );
        waiters[count++] = (struct pollfd){pool->ready_fd, POLLIN, 0};
    }
    for (job = client_surface_compositor_pending; job; job = job->next)
    {
        DWORD elapsed;
        int remaining;

        if (!job->present_started) continue;
        if (job->present_done) return;
        elapsed = NtGetTickCount() - job->present_start;
        remaining = elapsed >= 5000 ? 0 : 5000 - elapsed;
        if (timeout < 0 || timeout > remaining) timeout = remaining;
    }
    for (target = client_surface_compositor_targets; target; target = target->next)
    {
        DWORD elapsed;
        int remaining;

        if (!target->shrink_start || target->native_updates || target->assembly_pending) continue;
        elapsed = NtGetTickCount() - target->shrink_start;
        remaining = elapsed >= 2000 ? 0 : 2000 - elapsed;
        if (timeout < 0 || timeout > remaining) timeout = remaining;
    }
    do ret = poll( waiters, count, timeout ); while (ret < 0 && errno == EINTR);
    if (ret < 0) WARN( "client-surface compositor poll failed, error %d\n", errno );
}

static void quiesce_client_surface_compositor_target( struct client_surface_compositor_target *target )
{
    target->quiescing = TRUE;
    if (target->copy_frame) return;
    finish_client_surface_compositor_assembly( target, TRUE );
    if (target->mailbox_pending && target->mailbox_publish_generation)
        publish_client_surface_handoff_generation( target->toplevel,
            target->mailbox_publish_generation, target->mailbox_publish_epoch, FALSE );
    target->mailbox_pending = FALSE;
    target->mailbox_publish_generation = target->mailbox_publish_epoch = 0;
}

static struct client_surface_compositor_target *client_surface_compositor_job_target(
    const struct client_surface_compositor_job *job );

static BOOL client_surface_compositor_update_ready( struct client_surface_compositor_target *target,
                                                    const struct client_surface_compositor_job *until )
{
    const struct client_surface_compositor_job *job;
    unsigned int i;

    if (target->copy_frame || target->native_updates) return FALSE;
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        if (target->frames[i].serial) return FALSE;
    for (job = client_surface_compositor_pending; job && job != until; job = job->next)
        if (client_surface_compositor_job_target( job ) == target) return FALSE;
    return TRUE;
}

static BOOL execute_client_surface_compositor_job( struct client_surface_compositor_job *job )
{
    if (job->op == CLIENT_SURFACE_COMPOSITOR_BEGIN_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_TRY_BEGIN_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_END_UPDATE)
    {
        struct client_surface_compositor_target *target =
            find_client_surface_compositor_target( job->handoff_toplevel );

        if (!target) return FALSE;
        if (job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE ||
            job->op == CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE)
        {
            if (target->deferred_update != job->mark || !target->update_notified) return FALSE;
            if (job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE)
            {
                job->update_types = target->deferred_update_types | X11DRV_CLIENT_SURFACE_UPDATE_STATE;
                target->deferred_update_types = 0;
            }
            else
            {
                if (!target->deferred_update_types) target->deferred_update = 0;
                target->update_notified = FALSE;
                target->quiescing = target->native_updates || target->deferred_update;
            }
            return TRUE;
        }
        if (job->op == CLIENT_SURFACE_COMPOSITOR_TRY_BEGIN_UPDATE)
        {
            quiesce_client_surface_compositor_target( target );
            if (!client_surface_compositor_update_ready( target, job ))
            {
                if (!target->deferred_update)
                {
                    if (!(++client_surface_native_update_serial)) ++client_surface_native_update_serial;
                    target->deferred_update = client_surface_native_update_serial;
                }
                target->deferred_update_types |= job->update_types;
                job->update_deferred = TRUE;
                TRACE( "deferring native state update %s for %p\n",
                       wine_dbgstr_longlong( target->deferred_update ), target->toplevel );
                return FALSE;
            }
        }
        if (job->op != CLIENT_SURFACE_COMPOSITOR_END_UPDATE)
        {
            target->deferred_update_types &= ~job->update_types;
            ++target->native_updates;
            target->quiescing = TRUE;
            if (job->invalidate_scene) target->scene.valid = FALSE;
        }
        else
        {
            if (target->native_updates) --target->native_updates;
            target->quiescing = target->native_updates || target->deferred_update;
        }
        return TRUE;
    }
    if (job->op == CLIENT_SURFACE_COMPOSITOR_REGISTER_HANDOFF)
        return register_client_surface_compositor_handoff( job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_REUSE_HANDOFFS)
        return reuse_client_surface_compositor_handoffs( job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_SCENE)
        return check_client_surface_compositor_scene( job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_REPAIR_OWNER)
        return repair_client_surface_compositor_owner( job->handoff_toplevel );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_SWEEP_HANDOFFS)
        return sweep_client_surface_compositor_handoffs( job->handoff_toplevel, job->mark,
                                                          job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_UPDATE_TARGET)
        return update_client_surface_compositor_target( job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_REMOVE_TARGET)
        return remove_client_surface_compositor_target( job->handoff_toplevel );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_RESTORE_TARGET)
        return restore_client_surface_compositor_target( job );
    if (!client_surface_compositor_open()) return FALSE;
    switch (job->op)
    {
    case CLIENT_SURFACE_COMPOSITOR_ALLOC_POOL:
        return client_surface_alloc_on_compositor( job->destination, job->width,
                                                   job->height, job->depth, job->pixmaps );
    case CLIENT_SURFACE_COMPOSITOR_FREE_POOL:
        return client_surface_free_on_compositor( job->pixmaps );
    case CLIENT_SURFACE_COMPOSITOR_PRESENT:
        return client_surface_present_on_compositor( job );
    case CLIENT_SURFACE_COMPOSITOR_COPY:
        break;
    default:
        return FALSE;
    }
    return client_surface_copy_on_compositor( job->source, job->destination,
                                              job->source_x, job->source_y,
                                              job->destination_x, job->destination_y,
                                              job->width, job->height );
}

static struct client_surface_compositor_target *client_surface_compositor_job_target(
    const struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_target *target;
    unsigned int i;

    if (job->handoff_toplevel) return find_client_surface_compositor_target( job->handoff_toplevel );
    for (target = client_surface_compositor_targets; target; target = target->next)
    {
        if (target->window && (target->window == job->destination || target->window == job->source))
            return target;
        for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        {
            Pixmap pixmap = target->frames[i].pixmap;

            if (pixmap && (pixmap == job->source || pixmap == job->destination ||
                           pixmap == job->pixmaps[0] || pixmap == job->pixmaps[1])) return target;
        }
    }
    return NULL;
}

static BOOL client_surface_compositor_job_ready( struct client_surface_compositor_job *job,
                                                struct client_surface_compositor_target *target )
{
    unsigned int i;
    BOOL drain = FALSE;

    if (!target) return TRUE;
    /* These probes never mutate native geometry or consume an output. They
     * must answer while that target's older Present work is still pending. */
    if (job->op == CLIENT_SURFACE_COMPOSITOR_TRY_BEGIN_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE) return TRUE;
    /* Expose restoration records its own deferred work if necessary. */
    if (job->op == CLIENT_SURFACE_COMPOSITOR_RESTORE_TARGET) return TRUE;
    /* Preserve the scene, binding and frame referenced by the request. Only
     * this target waits; jobs for independent targets remain eligible. */
    if (target->copy_frame) return FALSE;
    if (job->op == CLIENT_SURFACE_COMPOSITOR_REMOVE_TARGET ||
        job->op == CLIENT_SURFACE_COMPOSITOR_BEGIN_UPDATE) drain = TRUE;
    if (job->op == CLIENT_SURFACE_COMPOSITOR_UPDATE_TARGET)
        drain = target->window != job->destination ||
                target->window_width != job->window_width || target->window_height != job->window_height ||
                !((target->frames[0].pixmap == job->pixmaps[0] && target->frames[1].pixmap == job->pixmaps[1]) ||
                  (target->frames[1].pixmap == job->pixmaps[0] && target->frames[0].pixmap == job->pixmaps[1]));
    if (drain)
    {
        /* Stop producing work for this target while its previous native
         * scene drains. Other targets remain eligible in the same loop. */
        quiesce_client_surface_compositor_target( target );
    }
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
    {
        struct client_surface_compositor_frame *frame = &target->frames[i];

        if (!frame->serial) continue;
        if (drain ||
            (job->op == CLIENT_SURFACE_COMPOSITOR_COPY && frame->pixmap == job->destination) ||
            (job->op == CLIENT_SURFACE_COMPOSITOR_PRESENT && frame->pixmap == job->source) ||
            (job->op == CLIENT_SURFACE_COMPOSITOR_FREE_POOL &&
             (frame->pixmap == job->pixmaps[0] || frame->pixmap == job->pixmaps[1]))) return FALSE;
    }
    return TRUE;
}

static BOOL process_client_surface_compositor_jobs(void)
{
    struct client_surface_compositor_job **cursor, *job, *earlier;
    unsigned int budget = 64;
    BOOL progressed = FALSE;
    struct client_surface_compositor_target *target;

    for (target = client_surface_compositor_targets; target; target = target->next)
    {
        if (!target->shrink_start || target->native_updates || target->assembly_pending ||
            NtGetTickCount() - target->shrink_start < 2000) continue;
        target->shrink_start = 0;
        NtUserPostMessage( target->toplevel, WM_WINE_UPDATEWINDOWSTATE,
                           WINE_UPDATE_CLIENT_SURFACE_HANDOFFS, 0 );
    }

    for (cursor = &client_surface_compositor_pending; *cursor; cursor = &(*cursor)->next) ;
    pthread_mutex_lock( &client_surface_compositor_mutex );
    *cursor = client_surface_compositor_head;
    client_surface_compositor_head = NULL;
    client_surface_compositor_tail = &client_surface_compositor_head;
    pthread_mutex_unlock( &client_surface_compositor_mutex );

    for (cursor = &client_surface_compositor_pending; (job = *cursor) && budget; )
    {
        struct client_surface_compositor_target *target = client_surface_compositor_job_target( job );

        /* Preserve ordering within a target, including GUI publication ACKs,
         * while allowing later jobs for independent targets to run. */
        for (earlier = client_surface_compositor_pending; earlier != job; earlier = earlier->next)
            if ((target && target == client_surface_compositor_job_target( earlier )) ||
                (job->handoff_toplevel && job->handoff_toplevel == earlier->handoff_toplevel)) break;
        if (earlier != job && job->op != CLIENT_SURFACE_COMPOSITOR_TRY_BEGIN_UPDATE &&
            job->op != CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE &&
            job->op != CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE)
        {
            cursor = &job->next;
            continue;
        }
        if (job->present_started)
        {
            if (!job->present_done && NtGetTickCount() - job->present_start >= 5000)
            {
                unsigned int i;

                /* Retire the caller's wait, not the in-flight output. Its
                 * serial remains owned until real Complete and Idle events. */
                if (target)
                    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
                        if (target->frames[i].waiter == job) target->frames[i].waiter = NULL;
                job->present_done = TRUE;
                job->result = FALSE;
            }
            if (!job->present_done)
            {
                cursor = &job->next;
                continue;
            }
        }
        else
        {
            if (!client_surface_compositor_job_ready( job, target ))
            {
                cursor = &job->next;
                continue;
            }
            job->result = execute_client_surface_compositor_job( job );
            if (job->present_started)
            {
                --budget;
                progressed = TRUE;
                cursor = &job->next;
                continue;
            }
        }
        *cursor = job->next;
        --budget;
        progressed = TRUE;
        if (job->async)
        {
            free( job );
            continue;
        }
        pthread_mutex_lock( &client_surface_compositor_mutex );
        job->complete = TRUE;
        pthread_cond_broadcast( &client_surface_compositor_cond );
        pthread_mutex_unlock( &client_surface_compositor_mutex );
    }
    for (target = client_surface_compositor_targets; target; target = target->next)
    {
        if (!target->deferred_update || target->update_notified) continue;
        quiesce_client_surface_compositor_target( target );
        if (!client_surface_compositor_update_ready( target, NULL )) continue;
        /* Keep this target quiescent until the GUI applies the latest server
         * state. A token prevents a delayed notification from resuming a new
         * target or a state update already consumed by another native change. */
        target->update_notified = NtUserPostMessage( target->toplevel, WM_X11DRV_CLIENT_SURFACE_UPDATE,
                                                    (UINT)target->deferred_update,
                                                    (UINT)(target->deferred_update >> 32) );
        if (!target->update_notified)
            WARN( "failed to notify deferred native update for %p\n", target->toplevel );
    }
    return progressed;
}

static void client_surface_compositor_thread( void *context )
{
    (void)context;

    for (;;)
    {
        BOOL progressed;

        process_client_surface_present_events();
        progressed = process_client_surface_present_replies();
        progressed |= process_client_surface_copy_replies();
        progressed |= process_client_surface_compositor_restores();
        progressed |= process_client_surface_compositor_jobs();
        progressed |= process_client_surface_handoffs();
        progressed |= replay_client_surface_scene_sources();
        process_client_surface_compositor_mailboxes();
        if (!progressed) wait_client_surface_compositor_work();
    }
}

static BOOL init_client_surface_compositor_notification(void)
{
    int fds[2];

    if (client_surface_compositor_notify[0] >= 0) return TRUE;
#ifdef __linux__
    if ((fds[0] = eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK )) < 0) return FALSE;
    if ((fds[1] = fcntl( fds[0], F_DUPFD_CLOEXEC, 0 )) < 0)
    {
        close( fds[0] );
        return FALSE;
    }
#else
    if (socketpair( AF_UNIX, SOCK_DGRAM, 0, fds )) return FALSE;
    if (fcntl( fds[0], F_SETFD, FD_CLOEXEC ) < 0 || fcntl( fds[1], F_SETFD, FD_CLOEXEC ) < 0 ||
        fcntl( fds[0], F_SETFL, O_NONBLOCK ) < 0 || fcntl( fds[1], F_SETFL, O_NONBLOCK ) < 0)
    {
        close( fds[0] );
        close( fds[1] );
        return FALSE;
    }
#endif
    client_surface_compositor_notify[0] = fds[0];
    client_surface_compositor_notify[1] = fds[1];
    return TRUE;
}

/* The caller holds client_surface_compositor_mutex. Ownership of asynchronous
 * jobs passes to the actor only when they have been queued successfully. */
static BOOL queue_client_surface_compositor_job( struct client_surface_compositor_job *job )
{
    HANDLE thread;
    NTSTATUS status;

    job->next = NULL;
    job->complete = FALSE;
    job->present_started = job->present_done = FALSE;
    job->update_deferred = FALSE;
    if (!client_surface_compositor_started)
    {
        if (!init_client_surface_compositor_notification()) return FALSE;
        status = PsCreateSystemThread( &thread, THREAD_ALL_ACCESS, NULL, 0, NULL,
                                       client_surface_compositor_thread, NULL );
        if (status)
        {
            WARN( "failed to create client-surface compositor, status %#lx\n",
                  (unsigned long)status );
            return FALSE;
        }
        NtClose( thread );
        client_surface_compositor_started = TRUE;
    }
    *client_surface_compositor_tail = job;
    client_surface_compositor_tail = &job->next;
    wake_client_surface_compositor();
    pthread_cond_broadcast( &client_surface_compositor_cond );
    return TRUE;
}

static BOOL submit_client_surface_compositor_job( struct client_surface_compositor_job *job )
{
    BOOL ret = FALSE;

    job->async = FALSE;
    pthread_mutex_lock( &client_surface_compositor_mutex );
    if (queue_client_surface_compositor_job( job ))
    {
        while (!job->complete)
            pthread_cond_wait( &client_surface_compositor_cond,
                               &client_surface_compositor_mutex );
        ret = job->result;
    }
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    return ret;
}

/* Release notifications carry only values; the caller does not consume their
 * result. Keep the synchronous path if ownership cannot pass to the actor. */
static void post_client_surface_compositor_job( struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_job *pending;

    assert( job->op == CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE ||
            job->op == CLIENT_SURFACE_COMPOSITOR_END_UPDATE );
    if ((pending = malloc( sizeof(*pending) )))
    {
        BOOL queued;

        *pending = *job;
        pending->async = TRUE;
        pthread_mutex_lock( &client_surface_compositor_mutex );
        queued = queue_client_surface_compositor_job( pending );
        pthread_mutex_unlock( &client_surface_compositor_mutex );
        if (queued) return;
        free( pending );
    }
    /* Dropping either notification would leave its target quiescent. */
    submit_client_surface_compositor_job( job );
}

static BOOL client_surface_backing_copy_area( Drawable source, Drawable destination,
                                              int source_x, int source_y,
                                              int destination_x, int destination_y,
                                              unsigned int width, unsigned int height )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_COPY,
        .source = source,
        .destination = destination,
        .source_x = source_x,
        .source_y = source_y,
        .destination_x = destination_x,
        .destination_y = destination_y,
        .width = width,
        .height = height,
    };

    return submit_client_surface_compositor_job( &job );
}

static BOOL client_surface_backing_alloc( Drawable drawable, unsigned int width,
                                          unsigned int height, unsigned int depth,
                                          Pixmap *first, Pixmap *second )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_ALLOC_POOL,
        .destination = drawable,
        .width = width,
        .height = height,
        .depth = depth,
    };

    if (!submit_client_surface_compositor_job( &job )) return FALSE;
    *first = job.pixmaps[0];
    *second = job.pixmaps[1];
    return TRUE;
}

static void client_surface_backing_free( Pixmap first, Pixmap second )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_FREE_POOL,
        .pixmaps = {first, second},
    };

    if (!submit_client_surface_compositor_job( &job ))
        WARN( "failed to release client-surface frame pool %#lx/%#lx\n", first, second );
}

static BOOL client_surface_backing_copy( Drawable source, Drawable destination,
                                         unsigned int width, unsigned int height )
{
    return client_surface_backing_copy_area( source, destination, 0, 0, 0, 0,
                                             width, height );
}

static BOOL client_surface_backing_present( Window window, Pixmap pixmap,
                                            unsigned int width, unsigned int height )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_PRESENT,
        .source = pixmap,
        .destination = window,
        .width = width,
        .height = height,
    };

    return submit_client_surface_compositor_job( &job );
}

static BOOL update_client_surface_backing_target( struct x11drv_win_data *data )
{
    struct client_surface_compositor_job job;
    unsigned int window_width, window_height;

    if (!get_client_surface_window_extent( data, &window_width, &window_height )) return FALSE;
    job = (struct client_surface_compositor_job)
    {
        .op = CLIENT_SURFACE_COMPOSITOR_UPDATE_TARGET,
        .pixmaps = {data->client_surface_backing, data->client_surface_backing_spare},
        .destination = data->whole_window,
        .width = data->client_surface_backing_width,
        .height = data->client_surface_backing_height,
        .window_width = window_width,
        .window_height = window_height,
        .valid_width = data->client_surface_backing_valid ?
                       data->client_surface_backing_valid_width : 0,
        .valid_height = data->client_surface_backing_valid ?
                        data->client_surface_backing_valid_height : 0,
        .depth = data->vis.depth,
        .handoff_toplevel = data->hwnd,
        .visual = data->vis.visualid,
        .shrink_start = data->client_surface_backing_shrink_start,
    };

    return submit_client_surface_compositor_job( &job );
}

static void remove_client_surface_backing_target( HWND toplevel )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_REMOVE_TARGET,
        .handoff_toplevel = toplevel,
    };

    submit_client_surface_compositor_job( &job );
}

BOOL X11DRV_client_surface_backing_begin_update( HWND hwnd, const struct window_rects *rects,
                                                UINT swp_flags, BOOL *deferred )
{
    const UINT no_geometry = SWP_NOSIZE | SWP_NOMOVE | SWP_NOCLIENTSIZE | SWP_NOCLIENTMOVE | SWP_NOZORDER;
    struct x11drv_win_data *data;
    BOOL backing, activation;
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_BEGIN_UPDATE,
        .handoff_toplevel = hwnd,
        .update_types = X11DRV_CLIENT_SURFACE_UPDATE_STATE |
            ((swp_flags & (WINE_SWP_CLIENT_SURFACE_BACKING_ENABLE | WINE_SWP_CLIENT_SURFACE_BACKING_DISABLE))
             ? X11DRV_CLIENT_SURFACE_UPDATE_BACKING : 0) |
            ((swp_flags & WINE_SWP_CLIENT_SURFACE_PREPARE) ? X11DRV_CLIENT_SURFACE_UPDATE_PREPARE : 0),
    };

    if (deferred) *deferred = FALSE;
    if (!(data = get_win_data( hwnd ))) return FALSE;
    backing = !!data->client_surface_backing;
    activation = (swp_flags & WINE_SWP_CLIENT_SURFACE_BACKING_ENABLE) &&
                 !data->client_surface_backing_enabled;
    /* A state-only refresh does not change the plan's placement or clip.
     * The server roster/epoch check continues
     * to invalidate topology and producer changes. Be conservative for
     * fullscreen mappings, shape, frame and actual native geometry changes. */
    job.invalidate_scene = !rects || (swp_flags & no_geometry) != no_geometry ||
        (swp_flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW | SWP_FRAMECHANGED | SWP_STATECHANGED)) ||
        data->is_fullscreen || (swp_flags & WINE_SWP_FULLSCREEN) ||
        memcmp( &data->rects, rects, sizeof(*rects) );
    release_win_data( data );
    if (!backing) return FALSE;

    /* Plain state notifications can be coalesced and reapplied from current
     * server state, including backing and preparation. A deferred prepare
     * returns FALSE to win32u, which must not acknowledge it before replay.
     * A new backing activation retains its synchronous native publication
     * boundary. Repeated enables can coalesce with a pending disable while
     * the native backing is still enabled. */
    if (deferred && !job.invalidate_scene && !activation &&
        !(swp_flags & WINE_SWP_CLIENT_SURFACE_PUBLISH))
    {
        BOOL ret;

        job.op = CLIENT_SURFACE_COMPOSITOR_TRY_BEGIN_UPDATE;
        ret = submit_client_surface_compositor_job( &job );
        *deferred = job.update_deferred;
        return ret;
    }

    /* A missing Present event can leave this target quiescing indefinitely.
     * Do not hold the process-wide window-data lock while it drains. Keep
     * only the handle across the wait; the caller must look up its data again.
     * Destroying the window also removes its compositor target. */
    return submit_client_surface_compositor_job( &job );
}

UINT X11DRV_client_surface_backing_resume_update( HWND hwnd, UINT64 serial )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE,
        .handoff_toplevel = hwnd,
        .mark = serial,
    };

    if (!submit_client_surface_compositor_job( &job )) return 0;
    return job.update_types;
}

void X11DRV_client_surface_backing_finish_deferred_update( HWND hwnd, UINT64 serial )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE,
        .handoff_toplevel = hwnd,
        .mark = serial,
    };

    /* Even if the server no longer needs a prepare or backing transition,
     * release this notification's hold after every state handler returned.
     * A handler which deferred again leaves its reasons for the next wake. */
    post_client_surface_compositor_job( &job );
}

void X11DRV_client_surface_backing_end_update( struct x11drv_win_data *data )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_END_UPDATE,
        .handoff_toplevel = data->hwnd,
    };

    /* The GUI connection owns native geometry, shape and staging. Its changes
     * finish while this target is quiescent, before the owner activates the
     * installed plan. No unrelated target participates in this barrier. */
    XSync( data->display, False );
    if (data->client_surface_backing) X11DRV_client_surface_backing_ensure( data );
    /* Later native update and destruction jobs for this target remain behind
     * its release in the actor queue. The GUI has no result to wait for. */
    post_client_surface_compositor_job( &job );
}

static BOOL register_client_surface_handoff( HWND toplevel,
                                             const struct client_surface_handoff_desc *desc,
                                             UINT64 mark )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_REGISTER_HANDOFF,
        .process = desc->process,
        .identity = desc->surface,
        .cookie = desc->cookie,
        .mark = mark,
        .handoff_window = wine_server_ptr_handle( desc->handle ),
        .handoff_toplevel = toplevel,
        .ready_fd = -1,
    };
    HANDLE event = NULL;
    BOOL ret = FALSE;
    NTSTATUS status;

    SERVER_START_REQ( get_client_surface_handoff )
    {
        req->handle = desc->handle;
        req->producer = desc->process;
        req->surface = desc->surface;
        req->owner = 1;
        status = wine_server_call( req );
        if (!status)
        {
            job.mapping = wine_server_ptr_handle( reply->mapping );
            job.view_size = reply->size;
            job.offset = reply->offset;
            job.mapping_id = reply->mapping_id;
            job.cookie = reply->cookie;
        }
    }
    SERVER_END_REQ;
    if (status) return FALSE;
    SERVER_START_REQ( get_client_surface_handoff_event )
    {
        req->handle = desc->handle;
        req->producer = desc->process;
        req->surface = desc->surface;
        req->cookie = job.cookie;
        req->owner = 1;
        status = wine_server_call( req );
        if (!status) event = wine_server_ptr_handle( reply->event );
    }
    SERVER_END_REQ;
    if (!status)
    {
        status = wine_server_handle_to_fd( event, FILE_READ_DATA, &job.ready_fd, NULL );
        NtClose( event );
    }
    if (status) goto release;
    ret = submit_client_surface_compositor_job( &job );
    if (!job.complete && job.ready_fd >= 0) close( job.ready_fd );

release:
    NtClose( job.mapping );
    if (ret) return TRUE;
    SERVER_START_REQ( release_client_surface_handoff )
    {
        req->handle = wine_server_user_handle( toplevel );
        req->producer = desc->process;
        req->surface = desc->surface;
        req->cookie = job.cookie;
        req->owner = 1;
        wine_server_call( req );
    }
    SERVER_END_REQ;
    return FALSE;
}

static BOOL refresh_client_surface_handoffs( HWND toplevel )
{
    struct client_surface_handoff_desc *descs = NULL;
    struct client_surface_scene_member *members = NULL;
    struct client_surface_scene_layout *layouts = NULL;
    BOOL *reused = NULL;
    UINT count = 0, i, index;
    unsigned int layout_count = 0;
    UINT64 scene_generation = 0;
    UINT64 mark = InterlockedIncrement64( (LONG64 *)&client_surface_compositor_mark );

    if (!mark) mark = InterlockedIncrement64( (LONG64 *)&client_surface_compositor_mark );
    if (!client_surface_get_scene_snapshot( toplevel, &scene_generation, &count, &members )) goto failed;
    if (count && !(descs = calloc( count, sizeof(*descs) ))) goto failed;
    for (i = 0; i < count; ++i)
    {
        descs[i].handle = wine_server_user_handle( members[i].hwnd );
        descs[i].process = members[i].process;
        descs[i].surface = members[i].identity;
        descs[i].cookie = members[i].cookie;
        descs[i].visible = members[i].visible;
    }
    if (count) qsort( descs, count, sizeof(*descs), compare_client_surface_handoff_descs );
    {
        struct client_surface_compositor_job job =
        {
            .op = CLIENT_SURFACE_COMPOSITOR_CHECK_SCENE,
            .handoff_toplevel = toplevel,
            .scene_epoch = scene_generation,
            .handoffs = descs,
            .handoff_count = count,
        };

        /* Backing ensure, snapshot and end-update can refresh the same plan
         * repeatedly. Its geometry and native clips are immutable at this
         * epoch. Reuse only a still-valid plan with the same live bindings;
         * native geometry and target changes keep their invalidation rules. */
        if (submit_client_surface_compositor_job( &job ))
        {
            if (!client_surface_scene_snapshot_current( toplevel, scene_generation )) goto failed;
            free( descs );
            client_surface_free_scene_snapshot( count, members );
            return TRUE;
        }
    }
    for (i = 0; i < count; ++i) layout_count += !!descs[i].visible;
    if (layout_count && !(layouts = calloc( layout_count, sizeof(*layouts) )))
    {
        layout_count = 0;
        goto failed;
    }
    /* Roster, selection, geometry and clips all come from the same reply.
     * The binding cache remains independent and includes hidden producers. */
    for (i = 0, index = 0; i < count; ++i)
    {
        if (!members[i].visible) continue;
        layouts[index].window = members[i].hwnd;
        layouts[index].process = members[i].process;
        layouts[index].identity = members[i].identity;
        layouts[index].geometry = members[i].target;
        if (!(layouts[index].clip = X11DRV_GetRegionData( members[i].region, 0 ))) goto failed;
        ++index;
    }
    if (count)
    {
        struct client_surface_compositor_job job =
        {
            .op = CLIENT_SURFACE_COMPOSITOR_REUSE_HANDOFFS,
            .handoff_toplevel = toplevel,
            .handoffs = descs,
            .handoff_count = count,
            .mark = mark,
        };

        if (!(reused = calloc( count, sizeof(*reused) ))) goto failed;
        job.handoff_reused = reused;
        if (!submit_client_surface_compositor_job( &job )) goto failed;
        for (i = 0; i < count; ++i)
            /* Do not provision channels for never-displayed hidden surfaces.
             * Existing authenticated bindings are marked above and retained. */
            if (descs[i].visible && !reused[i] &&
                !register_client_surface_handoff( toplevel, &descs[i], mark )) goto failed;
    }

    if (!client_surface_scene_snapshot_current( toplevel, scene_generation )) goto failed;
    {
        struct client_surface_compositor_job job =
        {
            .op = CLIENT_SURFACE_COMPOSITOR_SWEEP_HANDOFFS,
            .handoff_toplevel = toplevel,
            .mark = mark,
            .scene_epoch = scene_generation,
            .layouts = layouts,
            .layout_count = layout_count,
        };
        BOOL installed;

        installed = submit_client_surface_compositor_job( &job );
        layouts = job.layouts;
        layout_count = job.layout_count;
        if (!installed) goto failed;
    }
    free_client_surface_scene_layouts( layouts, layout_count );
    free( descs );
    free( reused );
    client_surface_free_scene_snapshot( count, members );
    return TRUE;

failed:
    free_client_surface_scene_layouts( layouts, layout_count );
    free( descs );
    free( reused );
    client_surface_free_scene_snapshot( count, members );
    return FALSE;
}

static unsigned int client_surface_backing_extent( int size )
{
    unsigned int requested = min( max( size, 1 ), 65535 );

    return min( (requested + 63) & ~63u, 65535 );
}

BOOL X11DRV_RepairClientSurfaceOwner( HWND hwnd )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_REPAIR_OWNER,
        .handoff_toplevel = hwnd,
    };

    /* The authoritative snapshot selects the exact bindings and layouts to
     * inspect. The actor owns their images and attestations; no channel state
     * is interpreted by the application thread as proof of a completed copy. */
    return refresh_client_surface_handoffs( hwnd ) && submit_client_surface_compositor_job( &job );
}

static BOOL get_client_surface_window_extent( struct x11drv_win_data *data,
                                              unsigned int *width, unsigned int *height )
{
    Window root;
    unsigned int border, depth;
    int x, y;

    return XGetGeometry( data->display, data->whole_window, &root, &x, &y,
                         width, height, &border, &depth );
}

void X11DRV_client_surface_backing_destroy( struct x11drv_win_data *data )
{
    if (data->client_surface_backing || data->client_surface_backing_spare)
    {
        remove_client_surface_backing_target( data->hwnd );
        client_surface_backing_free( data->client_surface_backing,
                                     data->client_surface_backing_spare );
    }
    data->client_surface_backing = 0;
    data->client_surface_backing_spare = 0;
    data->client_surface_backing_width = 0;
    data->client_surface_backing_height = 0;
    data->client_surface_backing_shrink_start = 0;
    data->client_surface_backing_valid_width = 0;
    data->client_surface_backing_valid_height = 0;
    data->client_surface_backing_valid = FALSE;
}

/* Align capacities to tiles, with modest growth slack and delayed shrinking.
 * Only the owner
 * compositor uses these Pixmaps, so replaced pools can be freed once its
 * target update has drained the old Present requests. */
BOOL X11DRV_client_surface_backing_ensure( struct x11drv_win_data *data )
{
    unsigned int width, height, window_width, window_height;
    unsigned int old_valid_width, old_valid_height;
    BOOL old_valid, valid, updated, shrink = FALSE;
    Pixmap pixmap, spare, old_pixmap, old_spare;

    if (!data->whole_window) return FALSE;
    if (!get_client_surface_window_extent( data, &window_width, &window_height )) return FALSE;
    width = client_surface_backing_extent( data->rects.visible.right - data->rects.visible.left );
    height = client_surface_backing_extent( data->rects.visible.bottom - data->rects.visible.top );
    if (data->client_surface_backing &&
        (UINT64)data->client_surface_backing_width * data->client_surface_backing_height >= (UINT64)width * height * 2)
    {
        SIZE size = {width, height};

        if (!data->client_surface_backing_shrink_start ||
            data->client_surface_backing_shrink_size.cx != width ||
            data->client_surface_backing_shrink_size.cy != height)
        {
            data->client_surface_backing_shrink_start = NtGetTickCount();
            if (!data->client_surface_backing_shrink_start) data->client_surface_backing_shrink_start = 1;
            data->client_surface_backing_shrink_size = size;
        }
        shrink = NtGetTickCount() - data->client_surface_backing_shrink_start >= 2000;
    }
    else data->client_surface_backing_shrink_start = 0;
    if (data->client_surface_backing && data->client_surface_backing_spare &&
        !shrink &&
        data->client_surface_backing_width >= width &&
        data->client_surface_backing_height >= height)
    {
        /* Allocation capacity is not content validity.  After a shrink, the
         * unused tail can contain an older scene (or allocation black).  A
         * later growth within the same geometric allocation must therefore
         * invalidate the backing and request a complete recomposition. */
        if (data->client_surface_backing_valid &&
            (window_width > data->client_surface_backing_valid_width ||
             window_height > data->client_surface_backing_valid_height))
        {
            data->client_surface_backing_valid = FALSE;
            data->client_surface_backing_valid_width = 0;
            data->client_surface_backing_valid_height = 0;
        }
        else if (data->client_surface_backing_valid)
        {
            data->client_surface_backing_valid_width =
                min( data->client_surface_backing_valid_width, window_width );
            data->client_surface_backing_valid_height =
                min( data->client_surface_backing_valid_height, window_height );
        }
        if (!update_client_surface_backing_target( data )) return FALSE;
        refresh_client_surface_handoffs( data->hwnd );
        return TRUE;
    }

    /* Grow the axis which needs space, without retaining the historical
     * maximum of the other axis across alternating wide/tall resizes. */
    if (!shrink && width > data->client_surface_backing_width)
        width = max( width, client_surface_backing_extent( data->client_surface_backing_width * 9 / 8 ) );
    if (!shrink && height > data->client_surface_backing_height)
        height = max( height, client_surface_backing_extent( data->client_surface_backing_height * 9 / 8 ) );
    old_valid = data->client_surface_backing_valid;
    old_valid_width = min( data->client_surface_backing_valid_width, width );
    old_valid_height = min( data->client_surface_backing_valid_height, height );

    /* Until a larger pool has been installed, the old Pixmap no longer
     * represents the complete host extent and must not satisfy Expose. */
    data->client_surface_backing_valid = FALSE;
    data->client_surface_backing_valid_width = 0;
    data->client_surface_backing_valid_height = 0;

    if (!client_surface_backing_alloc( data->whole_window, width, height,
                                       data->vis.depth, &pixmap, &spare ))
        return FALSE;

    /* Seed the replacement before changing the compositor target.  Producers
     * receive only source handoffs, never these owner-owned destination XIDs. */
    if (!client_surface_backing_copy( data->whole_window, pixmap,
                                      min( window_width, width ), min( window_height, height ) ) ||
        !client_surface_backing_copy( data->whole_window, spare,
                                      min( window_width, width ), min( window_height, height ) ) ||
        (data->client_surface_backing && old_valid &&
         (!client_surface_backing_copy( data->client_surface_backing, pixmap,
                                        old_valid_width, old_valid_height ) ||
          !client_surface_backing_copy( data->client_surface_backing, spare,
                                        old_valid_width, old_valid_height ))))
    {
        client_surface_backing_free( pixmap, spare );
        return FALSE;
    }

    old_pixmap = data->client_surface_backing;
    old_spare = data->client_surface_backing_spare;
    data->client_surface_backing = pixmap;
    data->client_surface_backing_spare = spare;
    data->client_surface_backing_width = width;
    data->client_surface_backing_height = height;
    data->client_surface_backing_shrink_start = 0;
    valid = old_valid && old_valid_width >= window_width && old_valid_height >= window_height;
    data->client_surface_backing_valid = valid;
    if (valid)
    {
        data->client_surface_backing_valid_width = window_width;
        data->client_surface_backing_valid_height = window_height;
    }
    updated = update_client_surface_backing_target( data );
    /* The synchronous target update drains the old pool before detaching it.
     * If installation failed, discard any partially updated target first;
     * the next ensure can install the new pool without dangling old XIDs. */
    if (!updated) remove_client_surface_backing_target( data->hwnd );
    if (old_pixmap || old_spare) client_surface_backing_free( old_pixmap, old_spare );
    if (!updated) return FALSE;
    refresh_client_surface_handoffs( data->hwnd );
    return TRUE;
}

BOOL X11DRV_client_surface_backing_snapshot( struct x11drv_win_data *data, BOOL invalidate )
{
    unsigned int width, height, window_width, window_height;
    Pixmap previous;

    if (!X11DRV_client_surface_backing_ensure( data )) return FALSE;
    if (!get_client_surface_window_extent( data, &window_width, &window_height )) return FALSE;
    width = min( data->client_surface_backing_width, window_width );
    height = min( data->client_surface_backing_height, window_height );
    /* The compositor uses another X connection.  Establish all preceding
     * owner-window drawing before it snapshots that drawable. */
    XSync( data->display, False );
    if (!client_surface_backing_copy( data->whole_window,
                                      data->client_surface_backing_spare, width, height ))
        return FALSE;
    if (width != window_width || height != window_height) return FALSE;
    previous = data->client_surface_backing;
    data->client_surface_backing = data->client_surface_backing_spare;
    data->client_surface_backing_spare = previous;
    TRACE( "rotated client-surface frame pool to %#lx (idle %#lx)\n",
           data->client_surface_backing, data->client_surface_backing_spare );
    data->client_surface_backing_valid = FALSE;
    data->client_surface_backing_valid_width = 0;
    data->client_surface_backing_valid_height = 0;
    if (!update_client_surface_backing_target( data )) return FALSE;
    refresh_client_surface_handoffs( data->hwnd );
    if (!invalidate)
    {
        data->client_surface_backing_valid = TRUE;
        data->client_surface_backing_valid_width = window_width;
        data->client_surface_backing_valid_height = window_height;
    }
    return TRUE;
}

BOOL X11DRV_client_surface_backing_publish( struct x11drv_win_data *data )
{
    unsigned int width, height, window_width, window_height;

    if (!data->whole_window || !data->client_surface_backing)
        return FALSE;
    if (!get_client_surface_window_extent( data, &window_width, &window_height )) return FALSE;
    width = min( data->client_surface_backing_width, window_width );
    height = min( data->client_surface_backing_height, window_height );
    if (width != window_width || height != window_height) return FALSE;
    if (!client_surface_backing_present( data->whole_window, data->client_surface_backing,
                                         width, height ))
    {
        TRACE( "falling back to XCopyArea publication for pixmap %#lx\n",
               data->client_surface_backing );
        if (!client_surface_backing_copy( data->client_surface_backing,
                                          data->whole_window, width, height ))
            return FALSE;
    }
    data->client_surface_backing_valid = TRUE;
    data->client_surface_backing_valid_width = window_width;
    data->client_surface_backing_valid_height = window_height;
    return TRUE;
}

BOOL X11DRV_client_surface_backing_restore( struct x11drv_win_data *data,
                                           Window window, const RECT *rect )
{
    struct client_surface_compositor_job job;
    unsigned int window_width, window_height;

    if (window != data->whole_window || !data->client_surface_backing ||
        IsRectEmpty( rect ))
        return FALSE;
    if (!get_client_surface_window_extent( data, &window_width, &window_height )) return FALSE;
    if (rect->left < 0 || rect->top < 0 ||
        (unsigned int)rect->right > window_width ||
        (unsigned int)rect->bottom > window_height)
        return FALSE;
    job = (struct client_surface_compositor_job)
    {
        .op = CLIENT_SURFACE_COMPOSITOR_RESTORE_TARGET,
        .destination = window,
        .source_x = rect->left,
        .source_y = rect->top,
        .destination_x = rect->left,
        .destination_y = rect->top,
        .width = rect->right - rect->left,
        .height = rect->bottom - rect->top,
        .window_width = window_width,
        .window_height = window_height,
        .handoff_toplevel = data->hwnd,
    };
    if (submit_client_surface_compositor_job( &job ))
    {
        data->client_surface_backing_valid = TRUE;
        data->client_surface_backing_valid_width = window_width;
        data->client_surface_backing_valid_height = window_height;
        return TRUE;
    }
    if (!data->client_surface_backing_valid ||
        window_width > data->client_surface_backing_valid_width ||
        window_height > data->client_surface_backing_valid_height)
        return FALSE;
    return client_surface_backing_copy_area( data->client_surface_backing,
                                             data->whole_window,
                                             rect->left, rect->top,
                                             rect->left, rect->top,
                                             rect->right - rect->left,
                                             rect->bottom - rect->top );
}
