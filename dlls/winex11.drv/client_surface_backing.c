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

#ifdef __linux__
#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "x11drv.h"
#include "xcomposite.h"
#include "xpresent.h"
#include "wine/server.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);

struct client_surface_compositor_pool
{
    struct client_surface_compositor_pool *next;
    struct client_surface_handoff_shared *shared;
    UINT64 id;
    SIZE_T size;
    unsigned int refs;
};

struct client_surface_compositor_binding
{
    struct client_surface_compositor_binding *next;
    struct client_surface_compositor_pool *pool;
    struct client_surface_handoff_slot *slot;
    Pixmap source;
    Drawable source_window;
    HWND toplevel;
    HWND window;
    process_id_t process;
    UINT64 identity;
    UINT64 cookie;
    UINT64 mark;
    UINT64 source_target_seq;
    VisualID source_visual;
    unsigned int source_width;
    unsigned int source_height;
    unsigned int source_depth;
    UINT64 held_control;
    UINT64 held_generation;
    UINT64 held_epoch;
    unsigned int held_frame;
};

#define CLIENT_SURFACE_COMPOSITOR_FRAME_COUNT 3
#define CLIENT_SURFACE_COMPOSITOR_MAX_INFLIGHT 2
#define CLIENT_SURFACE_COMPOSITOR_DAMAGE_HISTORY 64

struct client_surface_compositor_damage
{
    UINT64 revision;
    RECT rect;
};

struct client_surface_compositor_frame
{
    Pixmap pixmap;
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
};

struct client_surface_compositor_target
{
    struct client_surface_compositor_target *next;
    HWND toplevel;
    Window window;
    struct client_surface_compositor_frame frames[CLIENT_SURFACE_COMPOSITOR_FRAME_COUNT];
    Pixmap backing;
    Pixmap latest;
    Pixmap published;
    UINT64 revision;
    struct client_surface_compositor_damage damages[CLIENT_SURFACE_COMPOSITOR_DAMAGE_HISTORY];
    unsigned int published_width;
    unsigned int published_height;
    unsigned int next_frame;
    unsigned int mailbox_frame;
    unsigned int assembly_frame;
    UINT64 mailbox_publish_generation;
    UINT64 mailbox_publish_epoch;
    UINT64 assembly_generation;
    UINT64 assembly_epoch;
    UINT64 scene_epoch;
    BOOL mailbox_pending;
    BOOL assembly_pending;
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
static LONG client_surface_compositor_sequence;
#if defined(__linux__) && defined(SYS_futex_waitv)
static BOOL client_surface_compositor_waitv_available = TRUE;
#endif
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
    CLIENT_SURFACE_COMPOSITOR_SWEEP_HANDOFFS,
    CLIENT_SURFACE_COMPOSITOR_UPDATE_TARGET,
    CLIENT_SURFACE_COMPOSITOR_REMOVE_TARGET,
    CLIENT_SURFACE_COMPOSITOR_RESTORE_TARGET,
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
    void *view;
    SIZE_T view_size;
    SIZE_T offset;
    UINT64 mapping_id;
    UINT64 cookie;
    UINT64 identity;
    UINT64 mark;
    UINT64 scene_epoch;
    process_id_t process;
    HWND handoff_window;
    HWND handoff_toplevel;
    VisualID visual;
    BOOL result;
    BOOL complete;
};

static struct client_surface_compositor_job *client_surface_compositor_head;
static struct client_surface_compositor_job **client_surface_compositor_tail =
    &client_surface_compositor_head;

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

    if (!(++target->revision))
    {
        unsigned int i;

        target->revision = 1;
        memset( target->damages, 0, sizeof(target->damages) );
        for (i = 0; i < ARRAY_SIZE(target->frames); ++i) target->frames[i].revision = 0;
    }
    damage = &target->damages[target->revision % ARRAY_SIZE(target->damages)];
    damage->revision = target->revision;
    damage->rect = *rect;
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
static BOOL client_surface_present_on_compositor( Window window, Pixmap pixmap,
                                                  unsigned int width, unsigned int height );
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
    int error = 0;

    X11DRV_expect_error( display, client_surface_compositor_error, &error );
    pixmaps[0] = XCreatePixmap( display, drawable, width, height, depth );
    pixmaps[1] = XCreatePixmap( display, drawable, width, height, depth );
    XSync( display, False );
    X11DRV_check_error();
    if (!error) return TRUE;

    X11DRV_expect_error( display, client_surface_compositor_error, &error );
    XFreePixmap( display, pixmaps[0] );
    XFreePixmap( display, pixmaps[1] );
    XSync( display, False );
    X11DRV_check_error();
    pixmaps[0] = pixmaps[1] = 0;
    return FALSE;
}

static BOOL client_surface_free_on_compositor( const Pixmap pixmaps[2] )
{
    Display *display = client_surface_compositor_display;
    int error = 0;

    X11DRV_expect_error( display, client_surface_compositor_error, &error );
    if (pixmaps[0]) XFreePixmap( display, pixmaps[0] );
    if (pixmaps[1]) XFreePixmap( display, pixmaps[1] );
    XSync( display, False );
    X11DRV_check_error();
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
    if (!frame->serial || !frame->complete || !frame->idle) return;
    TRACE( "X Present serial %u pixmap %#lx completed and became idle\n",
           frame->serial, frame->pixmap );
    frame->serial = 0;
    frame->complete = frame->idle = FALSE;
    flush_client_surface_compositor_mailbox( target );
}

static void process_client_surface_present_events(void)
{
    Display *display = client_surface_compositor_display;

    if (!display || !usexpresent) return;
    while (XPending( display ))
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

                frame->complete = TRUE;
                frame->last_complete_serial = frame->serial;
                frame->last_complete_success = success;
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
        }
        else if (event.xcookie.evtype == PresentIdleNotify)
        {
            XPresentIdleNotifyEvent *notify = event.xcookie.data;

            if ((target = find_client_surface_compositor_window( notify->window )) &&
                (frame = find_client_surface_compositor_frame( target,
                    notify->serial_number, notify->pixmap ))) frame->idle = TRUE;
        }
        pXFreeEventData( display, &event );
        if (target && frame) finish_client_surface_compositor_frame( target, frame );
    }
}

static BOOL wait_client_surface_present_event( DWORD start )
{
    Display *display = client_surface_compositor_display;
    struct pollfd pfd = {.fd = ConnectionNumber( display ), .events = POLLIN};
    DWORD elapsed = NtGetTickCount() - start;
    int ret;

    if (elapsed >= 5000) return FALSE;
    do ret = poll( &pfd, 1, 5000 - elapsed ); while (ret < 0 && errno == EINTR);
    if (ret <= 0) return FALSE;
    process_client_surface_present_events();
    return TRUE;
}

static struct client_surface_compositor_frame *acquire_client_surface_compositor_frame(
    struct client_surface_compositor_target *target, Pixmap requested )
{
    DWORD start = NtGetTickCount();
    unsigned int i;

    for (;;)
    {
        process_client_surface_present_events();
        for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        {
            struct client_surface_compositor_frame *frame = &target->frames[i];

            if (frame->pixmap == requested && !frame->serial) return frame;
        }
        if (!wait_client_surface_present_event( start )) return NULL;
    }
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

        if (target->frames[index].serial) continue;
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
    X11DRV_expect_error( display, client_surface_compositor_error, &error );
    pXPresentPixmap( display, target->window, frame->pixmap, serial, None, None, 0, 0, None,
                     None, None, PresentOptionAsync | PresentOptionCopy,
                     0, 0, 0, NULL, 0 );
    XSync( display, False );
    X11DRV_check_error();
    if (error)
    {
        frame->serial = 0;
        frame->publish_pending = FALSE;
        return FALSE;
    }
    if (serial_ret) *serial_ret = serial;
    TRACE( "queued X Present serial %u pixmap %#lx generation %s\n", serial,
           frame->pixmap, wine_dbgstr_longlong( publish_generation ) );
    return TRUE;
}

static void flush_client_surface_compositor_mailbox(
    struct client_surface_compositor_target *target )
{
    struct client_surface_compositor_frame *frame;
    BOOL copied;

    if (!target->mailbox_pending ||
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

static void process_client_surface_present_events(void)
{
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
    __atomic_add_fetch( &client_surface_compositor_sequence, 1, __ATOMIC_RELEASE );
    client_surface_handoff_futex_wake( &client_surface_compositor_sequence );
}

static void client_surface_handoff_wake_release( struct client_surface_handoff_shared *shared )
{
    if (!__atomic_exchange_n( &shared->release_parked, 0, __ATOMIC_ACQ_REL )) return;
    __atomic_add_fetch( &shared->release_sequence, 1, __ATOMIC_RELEASE );
    client_surface_handoff_futex_wake( &shared->release_sequence );
}

static void release_client_surface_compositor_handoff(
    struct client_surface_compositor_binding *binding )
{
    UINT64 expected, released;

    if (!binding->held_control) return;
    expected = client_surface_handoff_control(
        client_surface_handoff_generation( binding->held_control ),
        CLIENT_SURFACE_HANDOFF_READING );
    released = client_surface_handoff_control(
        client_surface_handoff_generation( binding->held_control ),
        CLIENT_SURFACE_HANDOFF_RELEASED );
    __atomic_compare_exchange_n( &binding->slot->control, &expected, released, 0,
                                 __ATOMIC_RELEASE, __ATOMIC_ACQUIRE );
    binding->held_control = 0;
    binding->held_generation = 0;
    binding->held_epoch = 0;
    binding->held_frame = 0;
    client_surface_handoff_wake_release( binding->pool->shared );
}

static void finish_client_surface_compositor_assembly(
    struct client_surface_compositor_target *target, BOOL invalidate )
{
    struct client_surface_compositor_binding *binding;
    struct client_surface_compositor_frame *frame;

    if (!target->assembly_pending) return;
    frame = &target->frames[target->assembly_frame];
    for (binding = client_surface_compositor_bindings; binding; binding = binding->next)
        if (binding->toplevel == target->toplevel &&
            binding->held_generation == target->assembly_generation &&
            binding->held_epoch == target->assembly_epoch)
            release_client_surface_compositor_handoff( binding );
    if (invalidate)
    {
        /* A cancelled transaction may already have overwritten arbitrary
         * regions of its private frame.  Remove it from the damage lineage so
         * its next use starts with a full copy of the last complete frame. */
        assert( frame->pixmap != target->latest );
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
#ifdef SONAME_LIBXPRESENT
    DWORD start = NtGetTickCount();
#endif
    unsigned int i;

    for (;;)
    {
        process_client_surface_present_events();
        for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        {
            unsigned int index = (target->next_frame + i) % ARRAY_SIZE(target->frames);
            struct client_surface_compositor_frame *frame = &target->frames[index];

            if (frame->serial || frame->pixmap == target->latest ||
                (target->mailbox_pending && index == target->mailbox_frame))
                continue;
            target->next_frame = (index + 1) % ARRAY_SIZE(target->frames);
            return frame;
        }
#ifdef SONAME_LIBXPRESENT
        if (!usexpresent || !wait_client_surface_present_event( start )) return NULL;
#else
        return NULL;
#endif
    }
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

static BOOL client_surface_compositor_has_present_work(void)
{
    struct client_surface_compositor_target *target;
    unsigned int i;

    for (target = client_surface_compositor_targets; target; target = target->next)
    {
        if (target->mailbox_pending) return TRUE;
        for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
            if (target->frames[i].serial) return TRUE;
    }
    return FALSE;
}

static BOOL client_surface_present_on_compositor( Window window, Pixmap pixmap,
                                                  unsigned int width, unsigned int height )
{
#ifdef SONAME_LIBXPRESENT
    struct client_surface_compositor_target *target =
        find_client_surface_compositor_window( window );
    struct client_surface_compositor_frame *frame;
    uint32_t serial;
    DWORD start;

    if (!usexpresent || !target ||
        !(frame = acquire_client_surface_compositor_frame( target, pixmap )))
        return FALSE;
    frame->width = width;
    frame->height = height;
    /* This owner scene snapshot includes GDI pixels written outside the
     * compositor connection.  Record a complete checkpoint for later partial
     * handoffs into other pool entries. */
    note_client_surface_compositor_snapshot( target, pixmap );
    if (!submit_client_surface_present( target, frame, 0, 0, &serial ))
        return FALSE;
    start = NtGetTickCount();
    while (frame->last_complete_serial != serial)
        if (!wait_client_surface_present_event( start )) return FALSE;
    return frame->last_complete_success;
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
        free( pool );
        return;
    }
    assert( 0 );
}

static void remove_client_surface_compositor_binding(
    struct client_surface_compositor_binding **cursor )
{
    struct client_surface_compositor_binding *binding = *cursor;

    *cursor = binding->next;
    release_client_surface_compositor_handoff( binding );
    if (binding->source)
    {
        XFreePixmap( client_surface_compositor_display, binding->source );
        XSync( client_surface_compositor_display, False );
    }
    release_client_surface_compositor_binding_server( binding );
    release_client_surface_compositor_pool( binding->pool );
    free( binding );
}

static BOOL register_client_surface_compositor_handoff(
    struct client_surface_compositor_job *job )
{
    struct client_surface_handoff_shared *shared = job->view;
    struct client_surface_compositor_binding **cursor, *binding;
    struct client_surface_compositor_pool *pool;

    if (job->view_size < sizeof(*shared) ||
        job->offset > job->view_size - sizeof(struct client_surface_handoff_slot) ||
        __atomic_load_n( &shared->magic, __ATOMIC_ACQUIRE ) != CLIENT_SURFACE_HANDOFF_MAGIC ||
        shared->version != CLIENT_SURFACE_HANDOFF_VERSION ||
        shared->slot_count != CLIENT_SURFACE_HANDOFF_SLOTS ||
        shared->mapping_id != job->mapping_id)
        goto failed;

    for (cursor = &client_surface_compositor_bindings; *cursor; cursor = &(*cursor)->next)
    {
        binding = *cursor;
        if (binding->toplevel != job->handoff_toplevel ||
            binding->process != job->process || binding->identity != job->identity)
            continue;
        if (binding->cookie == job->cookie)
        {
            binding->mark = job->mark;
            NtUnmapViewOfSection( NtCurrentProcess(), job->view );
            return TRUE;
        }
        remove_client_surface_compositor_binding( cursor );
        break;
    }

    if (!(pool = find_client_surface_compositor_pool( job->mapping_id )))
    {
        if (!(pool = malloc( sizeof(*pool) ))) goto failed;
        pool->next = client_surface_compositor_pools;
        pool->shared = shared;
        pool->id = job->mapping_id;
        pool->size = job->view_size;
        pool->refs = 0;
        client_surface_compositor_pools = pool;
    }
    else
    {
        NtUnmapViewOfSection( NtCurrentProcess(), job->view );
        if (job->offset > pool->size - sizeof(struct client_surface_handoff_slot)) return FALSE;
        shared = pool->shared;
    }
    if (!(binding = calloc( 1, sizeof(*binding) )))
    {
        if (!pool->refs)
        {
            client_surface_compositor_pools = pool->next;
            NtUnmapViewOfSection( NtCurrentProcess(), pool->shared );
            free( pool );
        }
        return FALSE;
    }
    binding->next = client_surface_compositor_bindings;
    binding->pool = pool;
    binding->slot = (struct client_surface_handoff_slot *)((char *)shared + job->offset);
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
    return TRUE;

failed:
    NtUnmapViewOfSection( NtCurrentProcess(), job->view );
    return FALSE;
}

static void update_client_surface_compositor_scene(
    struct client_surface_compositor_target *target, UINT64 scene_epoch )
{
#ifdef SONAME_LIBXPRESENT
    unsigned int i;

    if (target->scene_epoch == scene_epoch) return;
    finish_client_surface_compositor_assembly( target, TRUE );
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
    target->scene_epoch = scene_epoch;
}

static BOOL sweep_client_surface_compositor_handoffs( HWND toplevel, UINT64 mark,
                                                       UINT64 scene_epoch )
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
    if ((target = find_client_surface_compositor_target( toplevel )))
        update_client_surface_compositor_scene( target, scene_epoch );
    return TRUE;
}

static void drain_client_surface_compositor_target(
    struct client_surface_compositor_target *target )
{
#ifdef SONAME_LIBXPRESENT
    DWORD start = NtGetTickCount();
    unsigned int i;

    for (;;)
    {
        BOOL pending = FALSE;

        process_client_surface_present_events();
        for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
            pending |= !!target->frames[i].serial;
        if (!pending) break;
        if (wait_client_surface_present_event( start )) continue;

        WARN( "timed out draining X Present frames for hwnd %p\n", target->toplevel );
        for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        {
            struct client_surface_compositor_frame *frame = &target->frames[i];
            Pixmap pixmap = frame->pixmap;

            if (frame->publish_pending)
                publish_client_surface_handoff_generation( target->toplevel,
                    frame->publish_generation, frame->publish_epoch, FALSE );
            memset( frame, 0, sizeof(*frame) );
            frame->pixmap = pixmap;
        }
        break;
    }
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
    BOOL same_pool;

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

        if (old_mailbox) XFreePixmap( client_surface_compositor_display, old_mailbox );
        memset( target->frames, 0, sizeof(target->frames) );
        target->frames[0].pixmap = job->pixmaps[0];
        target->frames[1].pixmap = job->pixmaps[1];
        X11DRV_expect_error( client_surface_compositor_display,
                             client_surface_compositor_error, &error );
        target->frames[2].pixmap = XCreatePixmap( client_surface_compositor_display,
            target->window, job->width, job->height, job->depth );
        XSync( client_surface_compositor_display, False );
        X11DRV_check_error();
        if (error || !target->frames[2].pixmap)
        {
            target->frames[2].pixmap = 0;
            return FALSE;
        }
        target->published = job->pixmaps[0];
        target->published_width = job->valid_width;
        target->published_height = job->valid_height;
        target->next_frame = 0;
        target->mailbox_pending = FALSE;
    }
    target->backing = job->pixmaps[0];
    target->width = job->width;
    target->height = job->height;
    target->window_width = job->window_width;
    target->window_height = job->window_height;
    target->depth = job->depth;
    target->visual = job->visual;
    TRACE( "updated compositor target hwnd %p window %#lx size %ux%u depth %u visual %#lx\n",
           target->toplevel, target->window, target->window_width, target->window_height,
           target->depth, target->visual );
    note_client_surface_compositor_snapshot( target, target->backing );
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

        if (target->toplevel != toplevel) continue;
        drain_client_surface_compositor_target( target );
        if (target->frames[2].pixmap)
        {
            XFreePixmap( client_surface_compositor_display, target->frames[2].pixmap );
            XSync( client_surface_compositor_display, False );
        }
        *cursor = target->next;
        free( target );
        return TRUE;
    }
    return TRUE;
}

static BOOL restore_client_surface_compositor_target(
    struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_target *target =
        find_client_surface_compositor_target( job->handoff_toplevel );

    process_client_surface_present_events();
    if (!target || !target->published || !target->window ||
        target->window_width != job->window_width ||
        target->window_height != job->window_height ||
        target->published_width < job->window_width ||
        target->published_height < job->window_height)
        return FALSE;
    job->valid_width = target->published_width;
    job->valid_height = target->published_height;
    return client_surface_copy_on_compositor( target->published, target->window,
                                              job->source_x, job->source_y,
                                              job->destination_x, job->destination_y,
                                              job->width, job->height );
}

static BOOL import_client_surface_pixmap( Drawable source, Pixmap *pixmap )
{
#ifdef SONAME_LIBXCOMPOSITE
    int error = 0;

    if (!usexcomposite) return FALSE;
    X11DRV_expect_error( client_surface_compositor_display,
                         client_surface_compositor_error, &error );
    *pixmap = pXCompositeNameWindowPixmap( client_surface_compositor_display, source );
    XSync( client_surface_compositor_display, False );
    X11DRV_check_error();
    return *pixmap && !error;
#else
    return FALSE;
#endif
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

static BOOL get_client_surface_compositor_source(
    struct client_surface_compositor_binding *binding,
    const struct client_surface_handoff_slot *slot, Pixmap *source,
    unsigned int *source_depth )
{
    Pixmap imported = 0;

    if (binding->source && binding->source_window == slot->source &&
        binding->source_target_seq == slot->target_seq &&
        binding->source_width == slot->width && binding->source_height == slot->height &&
        binding->source_visual == slot->source_visual)
    {
        *source = binding->source;
        *source_depth = binding->source_depth;
        return TRUE;
    }

    /* A snapshot remains producer-owned until our copy completes. Never cache
     * its XID: producer death before the copy executes is LOST, while a
     * completed copy leaves independently owned pixels in the owner frame. */
    if ((slot->flags & CLIENT_SURFACE_HANDOFF_COPY_SOURCE) || !usexcomposite)
    {
        *source = slot->source;
        TRACE( "copying transferred source %#lx without XComposite\n", *source );
        return validate_client_surface_pixmap( *source, slot->width, slot->height, source_depth );
    }

    if (!import_client_surface_pixmap( slot->source, &imported ) ||
        !validate_client_surface_pixmap( imported, slot->width, slot->height, source_depth ))
    {
        if (imported)
        {
            XFreePixmap( client_surface_compositor_display, imported );
            XSync( client_surface_compositor_display, False );
        }
        return FALSE;
    }
    if (binding->source) XFreePixmap( client_surface_compositor_display, binding->source );
    binding->source = imported;
    binding->source_window = slot->source;
    binding->source_target_seq = slot->target_seq;
    binding->source_visual = slot->source_visual;
    binding->source_width = slot->width;
    binding->source_height = slot->height;
    binding->source_depth = *source_depth;
    *source = imported;
    TRACE( "retained source pixmap %#lx for hwnd %p identity %s target %s depth %u visual %#lx\n",
           imported, binding->window, wine_dbgstr_longlong( binding->identity ),
           wine_dbgstr_longlong( slot->target_seq ), binding->source_depth,
           binding->source_visual );
    return TRUE;
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
    XImage *input = NULL, *output = NULL;
    unsigned int width = rect->right - rect->left, height = rect->bottom - rect->top;
    unsigned int x, y, source_y;
    unsigned long source_alpha, destination_alpha;
    int count;
    BOOL ret = FALSE;

    /* Keep the exceptional conversion path on the owner connection too.
     * The caller holds READING until its final XSync, covering both readback
     * and upload. The destination GC carries the exact scene clip. */
    if (!(source_visual = XGetVisualInfo( display, VisualIDMask, &source_template, &count )) ||
        !count || (source_visual->class != TrueColor && source_visual->class != DirectColor))
        goto done;
    if (!(destination_visual = XGetVisualInfo( display, VisualIDMask, &destination_template, &count )) ||
        !count || (destination_visual->class != TrueColor && destination_visual->class != DirectColor))
        goto done;
    if (!(input = XGetImage( display, source, 0, 0, source_width, source_height,
                             AllPlanes, ZPixmap )))
        goto done;
    if (!(output = XCreateImage( display, destination_visual->visual, destination_visual->depth,
                                ZPixmap, 0, NULL, width, height, 32, 0 )))
        goto done;
    if (output->bytes_per_line <= 0 || height > ~(SIZE_T)0 / output->bytes_per_line ||
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
    if (destination_visual) XFree( destination_visual );
    if (source_visual) XFree( source_visual );
    return ret;
}

static BOOL copy_client_surface_handoff_to_frame(
    struct client_surface_compositor_target *target,
    struct client_surface_compositor_frame *frame, Pixmap source, unsigned int source_depth,
    const struct client_surface_handoff_slot *slot, const RECT *damage )
{
    Display *display = client_surface_compositor_display;
    XRectangle clips[CLIENT_SURFACE_HANDOFF_MAX_CLIP_RECTS];
    RECT catchup = {0};
    BOOL clipped = !!(slot->flags & CLIENT_SURFACE_HANDOFF_CLIPPED);
    BOOL xfixes_clip = !!(slot->flags & CLIENT_SURFACE_HANDOFF_XFIXES_CLIP);
    BOOL pixmap_clip = !!(slot->flags & CLIENT_SURFACE_HANDOFF_PIXMAP_CLIP);
    BOOL copy_visible = !clipped || xfixes_clip || pixmap_clip || slot->clip_count;
    BOOL incoming_full, needs_catchup, native, overlay_copied = TRUE;
    unsigned int destination_width = slot->destination.right - slot->destination.left;
    unsigned int destination_height = slot->destination.bottom - slot->destination.top;
    unsigned int i;
    int error = 0;
    GC gc;

    if (!target->latest || !get_client_surface_compositor_catchup( target, frame, &catchup ))
        return FALSE;
    incoming_full = !clipped && damage->left == 0 && damage->top == 0 &&
                    (unsigned int)damage->right >= target->window_width &&
                    (unsigned int)damage->bottom >= target->window_height;
    needs_catchup = !incoming_full && frame->pixmap != target->latest &&
                    frame->revision != target->revision && !IsRectEmpty( &catchup );
    native = source_depth == target->depth && slot->source_visual == target->visual &&
             slot->width == destination_width && slot->height == destination_height;

    if (clipped && !xfixes_clip && !pixmap_clip)
        for (i = 0; i < slot->clip_count; ++i)
        {
            clips[i].x = slot->clips[i].x;
            clips[i].y = slot->clips[i].y;
            clips[i].width = slot->clips[i].width;
            clips[i].height = slot->clips[i].height;
        }

    X11DRV_expect_error( display, client_surface_compositor_error, &error );
    gc = XCreateGC( display, frame->pixmap, 0, NULL );
    if (gc)
    {
        if (needs_catchup)
            XCopyArea( display, target->latest, frame->pixmap, gc,
                       catchup.left, catchup.top,
                       catchup.right - catchup.left, catchup.bottom - catchup.top,
                       catchup.left, catchup.top );
        if (copy_visible)
        {
            if (xfixes_clip)
                overlay_copied = X11DRV_XFixes_SetClientSurfaceGCClip(
                    display, gc, slot->destination.left,
                    slot->destination.top, slot->clip_region );
            else if (pixmap_clip)
            {
                XSetClipOrigin( display, gc, slot->destination.left, slot->destination.top );
                XSetClipMask( display, gc, slot->clip_region );
            }
            else if (clipped)
                XSetClipRectangles( display, gc, slot->destination.left,
                                    slot->destination.top, clips, slot->clip_count, YXBanded );
            if (overlay_copied && native)
                XCopyArea( display, source, frame->pixmap, gc,
                           slot->damage.left, slot->damage.top,
                           slot->damage.right - slot->damage.left,
                           slot->damage.bottom - slot->damage.top,
                           slot->destination.left, slot->destination.top );
            else if (overlay_copied)
            {
                overlay_copied = X11DRV_XRender_CopyClientSurface(
                    display, source, slot->source_visual, frame->pixmap, target->visual,
                    slot->width, slot->height, &slot->destination,
                    clipped && !xfixes_clip && !pixmap_clip ? clips : NULL,
                    clipped && !xfixes_clip && !pixmap_clip ? slot->clip_count : 0,
                    xfixes_clip ? slot->clip_region : 0, pixmap_clip ? slot->clip_region : 0 );
                if (!overlay_copied)
                    overlay_copied = copy_client_surface_image(
                        display, source, frame->pixmap, gc, slot->source_visual, target->visual,
                        slot->width, slot->height, &slot->destination );
            }
        }
        XFreeGC( display, gc );
    }
    XSync( display, False );
    X11DRV_check_error();
    if (!gc || error || !overlay_copied) return FALSE;
    if (needs_catchup) frame->revision = target->revision;
    return TRUE;
}

static BOOL complete_client_surface_handoff_generation( HWND toplevel, UINT64 generation,
                                                        UINT64 scene_generation )
{
    BOOL accepted = FALSE;
    NTSTATUS status;

    SERVER_START_REQ( complete_client_surface_handoffs )
    {
        req->handle = wine_server_user_handle( toplevel );
        req->generation = generation;
        req->scene_generation = scene_generation;
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
    struct client_surface_compositor_binding *binding;
    unsigned int count = 0, frame_index = frame - target->frames;

    if (!target->assembly_pending || target->assembly_generation != generation ||
        target->assembly_epoch != epoch || target->assembly_frame != frame_index)
        return FALSE;
    for (binding = client_surface_compositor_bindings; binding; binding = binding->next)
    {
        UINT64 control;

        if (binding->toplevel != target->toplevel) continue;
        count++;
        if (!binding->held_control || binding->held_generation != generation ||
            binding->held_epoch != epoch || binding->held_frame != frame_index)
            return FALSE;
        control = __atomic_load_n( &binding->slot->control, __ATOMIC_ACQUIRE );
        if (control != client_surface_handoff_control(
                           client_surface_handoff_generation( binding->held_control ),
                           CLIENT_SURFACE_HANDOFF_READING ))
            return FALSE;
    }
    return !!count;
}

static BOOL publish_client_surface_handoff_assembly(
    struct client_surface_compositor_target *target,
    struct client_surface_compositor_frame *frame, UINT64 generation, UINT64 epoch )
{
    BOOL visible = FALSE, queued = FALSE, deferred = FALSE;
    RECT full = {0, 0, target->window_width, target->window_height};

    if (!complete_client_surface_handoff_generation( target->toplevel, generation, epoch ))
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
    /* XSync in the copy path made every imported source reusable. Keep all
     * transactional slots in READING until the server has atomically accepted
     * their complete set and the owner has reserved its single publication. */
    finish_client_surface_compositor_assembly( target, !visible );
    return visible;
}

static BOOL compose_client_surface_handoff(
    struct client_surface_compositor_binding *binding, UINT64 control )
{
    struct client_surface_handoff_slot *slot = binding->slot;
    struct client_surface_compositor_target *target;
    struct client_surface_compositor_frame *frame = NULL, *previous_publish = NULL;
    UINT64 expected = control, final;
    enum client_surface_handoff_state state;
    Pixmap source = 0;
    RECT damage;
    unsigned int destination_width, destination_height, i, source_depth = 0;
    BOOL xfixes_clip, pixmap_clip;
    BOOL composed = FALSE, copied = FALSE, dropped = FALSE;

    if (!__atomic_compare_exchange_n( &slot->control, &expected,
                                      client_surface_handoff_control(
                                          client_surface_handoff_generation( control ),
                                          CLIENT_SURFACE_HANDOFF_READING ),
                                      0, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE ))
        return FALSE;
    /* Clear this generation's ready bit while READING still prevents the
     * producer from reusing the slot.  Clearing after RELEASED could erase a
     * newer READY generation published by a racing producer. */
    __atomic_fetch_and( &binding->pool->shared->ready_bitmap[
                            (slot - binding->pool->shared->slots) / 64],
                        ~((LONG64)1 << ((slot - binding->pool->shared->slots) % 64)),
                        __ATOMIC_ACQ_REL );
    TRACE( "reading handoff hwnd %p identity %s generation %s\n", binding->window,
           wine_dbgstr_longlong( binding->identity ),
           wine_dbgstr_longlong( client_surface_handoff_generation( control ) ) );
    target = find_client_surface_compositor_target( binding->toplevel );
    if (!target || target->scene_epoch != slot->scene_epoch)
    {
        dropped = TRUE;
        goto release;
    }
    destination_width = slot->destination.right > slot->destination.left ?
                        slot->destination.right - slot->destination.left : 0;
    destination_height = slot->destination.bottom > slot->destination.top ?
                         slot->destination.bottom - slot->destination.top : 0;
    xfixes_clip = !!(slot->flags & CLIENT_SURFACE_HANDOFF_XFIXES_CLIP);
    pixmap_clip = !!(slot->flags & CLIENT_SURFACE_HANDOFF_PIXMAP_CLIP);
    if (slot->cookie == binding->cookie && slot->identity == binding->identity &&
        slot->producer_process == binding->process &&
        slot->window == wine_server_user_handle( binding->window ) &&
        slot->toplevel == wine_server_user_handle( binding->toplevel ) &&
        (slot->flags & (CLIENT_SURFACE_HANDOFF_NATIVE_X11 |
                        CLIENT_SURFACE_HANDOFF_FULL_DAMAGE)) ==
                       (CLIENT_SURFACE_HANDOFF_NATIVE_X11 |
                        CLIENT_SURFACE_HANDOFF_FULL_DAMAGE) &&
        target->backing && target->frames[0].pixmap && target->frames[1].pixmap &&
        target->window &&
        slot->destination.left >= 0 && slot->destination.top >= 0 &&
        destination_width && destination_height &&
        (unsigned int)slot->destination.right <= target->window_width &&
        (unsigned int)slot->destination.bottom <= target->window_height &&
        slot->width && slot->height && slot->source_visual &&
        !slot->damage.left && !slot->damage.top &&
        (unsigned int)slot->damage.right == slot->width &&
        (unsigned int)slot->damage.bottom == slot->height &&
        slot->clip_count <= CLIENT_SURFACE_HANDOFF_MAX_CLIP_RECTS &&
        (!!(slot->flags & CLIENT_SURFACE_HANDOFF_CLIPPED) || !slot->clip_count) &&
        ((!xfixes_clip && !pixmap_clip && !slot->clip_region) ||
         (xfixes_clip != pixmap_clip && (slot->flags & CLIENT_SURFACE_HANDOFF_CLIPPED) &&
          !slot->clip_count && slot->clip_region &&
          (pixmap_clip || X11DRV_XFixes_ClientSurfaceAvailable()))))
    {
        damage = slot->destination;
        if ((slot->flags & CLIENT_SURFACE_HANDOFF_CLIPPED) && !xfixes_clip && !pixmap_clip)
        {
            SetRectEmpty( &damage );
            for (i = 0; i < slot->clip_count; ++i)
            {
                const struct client_surface_handoff_clip_rect *clip = &slot->clips[i];
                RECT rect;

                if (!clip->width || !clip->height || clip->x < 0 || clip->y < 0 ||
                    (unsigned int)clip->x + clip->width > destination_width ||
                    (unsigned int)clip->y + clip->height > destination_height)
                    break;
                rect = (RECT){slot->destination.left + clip->x,
                              slot->destination.top + clip->y,
                              slot->destination.left + clip->x + clip->width,
                              slot->destination.top + clip->y + clip->height};
                if (!i) damage = rect;
                else
                {
                    damage.left = min( damage.left, rect.left );
                    damage.top = min( damage.top, rect.top );
                    damage.right = max( damage.right, rect.right );
                    damage.bottom = max( damage.bottom, rect.bottom );
                }
            }
            if (i != slot->clip_count) goto release;
            /* An empty explicit clip changes no visible pixels. Keep a
             * conservative journal entry so a rotated frame still catches up
             * before it can become the next owner snapshot. */
            if (!slot->clip_count) damage = slot->destination;
        }
        if (slot->scene_generation)
            previous_publish = find_client_surface_pending_publication(
                target, slot->scene_generation, slot->scene_epoch );
        if (slot->scene_generation && !previous_publish && target->assembly_pending &&
            (target->assembly_generation != slot->scene_generation ||
             target->assembly_epoch != slot->scene_epoch))
            finish_client_surface_compositor_assembly( target, TRUE );
        if (!slot->scene_generation || previous_publish)
            frame = get_client_surface_compositor_frame( target );
        else if (target->assembly_pending &&
                 target->assembly_generation == slot->scene_generation &&
                 target->assembly_epoch == slot->scene_epoch)
            frame = &target->frames[target->assembly_frame];
        else
            frame = acquire_client_surface_compositor_assembly_frame( target );

        if (!frame || !get_client_surface_compositor_source(
                          binding, slot, &source, &source_depth ))
            goto release;

        if (slot->scene_generation && !previous_publish && !target->assembly_pending)
        {
            target->assembly_pending = TRUE;
            target->assembly_generation = slot->scene_generation;
            target->assembly_epoch = slot->scene_epoch;
            target->assembly_frame = frame - target->frames;
        }

        /* Every producer in a transaction is copied into one owner-selected
         * frame. The owner retains their READING slots until the complete set
         * can be accepted and exposed as one scene. Full steady frames may
         * still rotate through the coalescing Present pool. */
        copied = copy_client_surface_handoff_to_frame( target, frame, source, source_depth,
                                                        slot, &damage );
        if (copied)
        {
            frame->width = target->window_width;
            frame->height = target->window_height;
        }
        if (copied && slot->scene_generation && !previous_publish)
        {
            assert( !binding->held_control );
            binding->held_control = control;
            binding->held_generation = slot->scene_generation;
            binding->held_epoch = slot->scene_epoch;
            binding->held_frame = frame - target->frames;
            composed = TRUE;
            if (client_surface_handoff_generation_assembled(
                    target, frame, slot->scene_generation, slot->scene_epoch ))
                composed = publish_client_surface_handoff_assembly(
                    target, frame, slot->scene_generation, slot->scene_epoch );
        }
        else if (copied)
        {
            note_client_surface_compositor_damage( target, frame, &damage );
#ifdef SONAME_LIBXPRESENT
            if (usexpresent && !target->mailbox_pending &&
                count_client_surface_compositor_frames( target ) <
                    CLIENT_SURFACE_COMPOSITOR_MAX_INFLIGHT)
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
                                                              0, 0, 0, 0,
                                                              target->width, target->height );
            if (composed && !frame->serial && !target->mailbox_pending)
            {
                target->published = frame->pixmap;
                target->published_width = frame->width;
                target->published_height = frame->height;
            }
        }
    }
release:
    if (!copied && slot->scene_generation && !previous_publish && target &&
        target->assembly_pending && target->assembly_generation == slot->scene_generation &&
        target->assembly_epoch == slot->scene_epoch)
        finish_client_surface_compositor_assembly( target, TRUE );
    if (binding->held_control)
    {
        TRACE( "held handoff hwnd %p identity %s generation %s scene %s for owner assembly\n",
               binding->window, wine_dbgstr_longlong( binding->identity ),
               wine_dbgstr_longlong( client_surface_handoff_generation( control ) ),
               wine_dbgstr_longlong( slot->scene_generation ) );
        return composed;
    }
    /* Once the compositor connection has copied the source into its backing,
     * or rejected it against a newer owner epoch before import, source storage
     * is reusable. A native import/copy failure instead retires the binding. */
    state = copied || dropped ? CLIENT_SURFACE_HANDOFF_RELEASED : CLIENT_SURFACE_HANDOFF_LOST;
    expected = client_surface_handoff_control( client_surface_handoff_generation( control ),
                                               CLIENT_SURFACE_HANDOFF_READING );
    final = client_surface_handoff_control( client_surface_handoff_generation( control ), state );
    __atomic_compare_exchange_n( &slot->control, &expected, final, 0,
                                 __ATOMIC_RELEASE, __ATOMIC_RELAXED );
    client_surface_handoff_wake_release( binding->pool->shared );
    TRACE( "%s handoff hwnd %p identity %s generation %s scene %s composed %u\n",
           state == CLIENT_SURFACE_HANDOFF_RELEASED ? "released" : "lost", binding->window,
           wine_dbgstr_longlong( binding->identity ),
           wine_dbgstr_longlong( client_surface_handoff_generation( control ) ),
           wine_dbgstr_longlong( slot->scene_generation ), composed );
    return composed;
}

static void process_client_surface_handoffs(void)
{
    struct client_surface_compositor_binding **cursor = &client_surface_compositor_bindings;

    while (*cursor)
    {
        struct client_surface_compositor_binding *binding = *cursor;
        ptrdiff_t index = binding->slot - binding->pool->shared->slots;
        UINT64 bitmap = __atomic_load_n( &binding->pool->shared->ready_bitmap[index / 64],
                                         __ATOMIC_ACQUIRE );
        UINT64 control;

        if (!(bitmap & ((UINT64)1 << (index % 64))))
        {
            cursor = &binding->next;
            continue;
        }
        control = __atomic_load_n( &binding->slot->control, __ATOMIC_ACQUIRE );
        if (client_surface_handoff_state( control ) == CLIENT_SURFACE_HANDOFF_READY)
        {
            compose_client_surface_handoff( binding, control );
            control = __atomic_load_n( &binding->slot->control, __ATOMIC_ACQUIRE );
        }
        if (client_surface_handoff_state( control ) == CLIENT_SURFACE_HANDOFF_LOST)
        {
            __atomic_fetch_and( &binding->pool->shared->ready_bitmap[index / 64],
                                ~((LONG64)1 << (index % 64)), __ATOMIC_ACQ_REL );
            remove_client_surface_compositor_binding( cursor );
        }
        else cursor = &binding->next;
    }
}

static void wait_client_surface_compositor_work(void)
{
#if defined(__linux__) && defined(SYS_futex_waitv)
    struct futex_waitv waiters[CLIENT_SURFACE_HANDOFF_MAX_POOLS_PER_CONSUMER + 1] = {0};
    struct timespec present_timeout, *timeout = NULL;
    struct client_surface_compositor_pool *pool;
    unsigned int count = 1;
    int ret;

    if (client_surface_compositor_waitv_available)
    {
        waiters[0].uaddr = (uintptr_t)&client_surface_compositor_sequence;
        waiters[0].val = __atomic_load_n( &client_surface_compositor_sequence, __ATOMIC_ACQUIRE );
        waiters[0].flags = FUTEX_32;
        for (pool = client_surface_compositor_pools; pool; pool = pool->next)
        {
            assert( count < ARRAY_SIZE(waiters) );
            __atomic_store_n( &pool->shared->ready_parked, 1, __ATOMIC_RELEASE );
            waiters[count].uaddr = (uintptr_t)&pool->shared->ready_sequence;
            waiters[count].val = __atomic_load_n( &pool->shared->ready_sequence, __ATOMIC_ACQUIRE );
            waiters[count++].flags = FUTEX_32;
        }

        /* Publish every parked flag before the final state and queue checks.  A
         * producer racing either check changes its sequence and makes waitv return
         * immediately; a producer preceding publication is found by this scan. */
        process_client_surface_handoffs();
        process_client_surface_present_events();
        pthread_mutex_lock( &client_surface_compositor_mutex );
        if (client_surface_compositor_head)
        {
            pthread_mutex_unlock( &client_surface_compositor_mutex );
            return;
        }
        pthread_mutex_unlock( &client_surface_compositor_mutex );

        /* X Present events and futex readiness cannot be waited by one kernel
         * primitive.  Poll the X connection only while a submitted frame is
         * outstanding; an idle compositor still sleeps indefinitely on the
         * lfsync-style producer/job sequences. */
        if (client_surface_compositor_has_present_work())
        {
            clock_gettime( CLOCK_MONOTONIC, &present_timeout );
            present_timeout.tv_nsec += 10000000;
            if (present_timeout.tv_nsec >= 1000000000)
            {
                present_timeout.tv_sec++;
                present_timeout.tv_nsec -= 1000000000;
            }
            timeout = &present_timeout;
        }
        do ret = syscall( SYS_futex_waitv, waiters, count, 0, timeout, CLOCK_MONOTONIC );
        while (ret < 0 && errno == EINTR);
        if (ret >= 0 || errno == EAGAIN || errno == ETIMEDOUT) return;
        client_surface_compositor_waitv_available = FALSE;
        if (errno != ENOSYS) WARN( "futex_waitv failed, error %d\n", errno );
    }
#endif

    /* Non-Linux and pre-futex_waitv kernels retain a bounded compatibility
     * wait.  Normal Linux operation has no periodic compositor wakeup. */
    pthread_mutex_lock( &client_surface_compositor_mutex );
    if (!client_surface_compositor_head)
    {
        struct timespec timeout;

        clock_gettime( CLOCK_REALTIME, &timeout );
        timeout.tv_nsec += 10000000;
        if (timeout.tv_nsec >= 1000000000)
        {
            timeout.tv_sec++;
            timeout.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait( &client_surface_compositor_cond,
                                &client_surface_compositor_mutex, &timeout );
    }
    pthread_mutex_unlock( &client_surface_compositor_mutex );
}

static BOOL execute_client_surface_compositor_job( struct client_surface_compositor_job *job )
{
    if (job->op == CLIENT_SURFACE_COMPOSITOR_REGISTER_HANDOFF)
        return register_client_surface_compositor_handoff( job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_SWEEP_HANDOFFS)
        return sweep_client_surface_compositor_handoffs( job->handoff_toplevel, job->mark,
                                                          job->scene_epoch );
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
        return client_surface_present_on_compositor( job->destination, job->source,
                                                      job->width, job->height );
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

static void client_surface_compositor_thread( void *context )
{
    (void)context;

    for (;;)
    {
        struct client_surface_compositor_job *job;

        pthread_mutex_lock( &client_surface_compositor_mutex );
        job = client_surface_compositor_head;
        if (!job)
        {
            pthread_mutex_unlock( &client_surface_compositor_mutex );
            process_client_surface_present_events();
            process_client_surface_handoffs();
            wait_client_surface_compositor_work();
            continue;
        }
        client_surface_compositor_head = job->next;
        if (!client_surface_compositor_head)
            client_surface_compositor_tail = &client_surface_compositor_head;
        pthread_mutex_unlock( &client_surface_compositor_mutex );

        job->result = execute_client_surface_compositor_job( job );

        pthread_mutex_lock( &client_surface_compositor_mutex );
        job->complete = TRUE;
        pthread_cond_broadcast( &client_surface_compositor_cond );
        pthread_mutex_unlock( &client_surface_compositor_mutex );
    }
}

static BOOL submit_client_surface_compositor_job( struct client_surface_compositor_job *job )
{
    HANDLE thread;
    NTSTATUS status;

    job->next = NULL;
    job->complete = FALSE;
    pthread_mutex_lock( &client_surface_compositor_mutex );
    if (!client_surface_compositor_started)
    {
        status = PsCreateSystemThread( &thread, THREAD_ALL_ACCESS, NULL, 0, NULL,
                                       client_surface_compositor_thread, NULL );
        if (status)
        {
            pthread_mutex_unlock( &client_surface_compositor_mutex );
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
    while (!job->complete)
        pthread_cond_wait( &client_surface_compositor_cond,
                           &client_surface_compositor_mutex );
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    return job->result;
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

static BOOL register_client_surface_handoff( HWND toplevel,
                                             const struct client_surface_handoff_desc *desc,
                                             UINT64 mark )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_REGISTER_HANDOFF,
        .process = desc->process,
        .identity = desc->surface,
        .mark = mark,
        .handoff_window = wine_server_ptr_handle( desc->handle ),
        .handoff_toplevel = toplevel,
    };
    HANDLE mapping = NULL;
    SIZE_T size;
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
            mapping = wine_server_ptr_handle( reply->mapping );
            job.view_size = reply->size;
            job.offset = reply->offset;
            job.mapping_id = reply->mapping_id;
            job.cookie = reply->cookie;
        }
    }
    SERVER_END_REQ;
    if (status) return FALSE;
    size = job.view_size;
    status = NtMapViewOfSection( mapping, NtCurrentProcess(), &job.view, 0, 0, NULL,
                                 &size, ViewShare, 0, PAGE_READWRITE );
    NtClose( mapping );
    if (status) goto release;
    job.view_size = size;
    if (submit_client_surface_compositor_job( &job )) return TRUE;
    if (!job.complete) NtUnmapViewOfSection( NtCurrentProcess(), job.view );

release:
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
    UINT size = 8, count = 0, i;
    UINT64 scene_generation = 0, current_generation = 0;
    UINT64 mark = InterlockedIncrement64( (LONG64 *)&client_surface_compositor_mark );
    NTSTATUS status;

    if (!mark) mark = InterlockedIncrement64( (LONG64 *)&client_surface_compositor_mark );
    for (;;)
    {
        struct client_surface_handoff_desc *next;
        data_size_t reply_size = 0;

        if (!(next = realloc( descs, size * sizeof(*descs) ))) goto failed;
        descs = next;
        SERVER_START_REQ( get_client_surface_handoffs )
        {
            req->handle = wine_server_user_handle( toplevel );
            wine_server_set_reply( req, descs, size * sizeof(*descs) );
            status = wine_server_call( req );
            if (!status)
            {
                count = reply->count;
                scene_generation = reply->scene_generation;
                reply_size = wine_server_reply_size( reply );
            }
        }
        SERVER_END_REQ;
        if (status) goto failed;
        if (count > size)
        {
            size = count;
            continue;
        }
        if (reply_size != count * sizeof(*descs) || (scene_generation & 1)) goto failed;
        break;
    }
    for (i = 0; i < count; ++i)
        if (!register_client_surface_handoff( toplevel, &descs[i], mark )) goto failed;

    SERVER_START_REQ( get_client_surface_handoffs )
    {
        req->handle = wine_server_user_handle( toplevel );
        status = wine_server_call( req );
        if (!status) current_generation = reply->scene_generation;
    }
    SERVER_END_REQ;
    if (status || current_generation != scene_generation) goto failed;
    {
        struct client_surface_compositor_job job =
        {
            .op = CLIENT_SURFACE_COMPOSITOR_SWEEP_HANDOFFS,
            .handoff_toplevel = toplevel,
            .mark = mark,
            .scene_epoch = scene_generation,
        };

        if (!submit_client_surface_compositor_job( &job )) goto failed;
    }
    free( descs );
    return TRUE;

failed:
    free( descs );
    return FALSE;
}

static unsigned int client_surface_backing_extent( int size )
{
    unsigned int extent = 64, requested = min( max( size, 1 ), 65535 );

    while (extent < requested)
    {
        if (extent >= 32768) return 65535;
        extent <<= 1;
    }
    return extent;
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
    data->client_surface_backing_valid_width = 0;
    data->client_surface_backing_valid_height = 0;
    data->client_surface_backing_valid = FALSE;
}

/* Grow geometrically to amortize allocation during resize.  Only the owner
 * compositor uses these Pixmaps, so replaced pools can be freed once its
 * target update has drained the old Present requests. */
BOOL X11DRV_client_surface_backing_ensure( struct x11drv_win_data *data )
{
    unsigned int width, height, window_width, window_height;
    unsigned int old_valid_width, old_valid_height;
    BOOL old_valid, valid, updated;
    Pixmap pixmap, spare, old_pixmap, old_spare;

    if (!data->whole_window) return FALSE;
    if (!get_client_surface_window_extent( data, &window_width, &window_height )) return FALSE;
    width = client_surface_backing_extent( data->rects.visible.right - data->rects.visible.left );
    height = client_surface_backing_extent( data->rects.visible.bottom - data->rects.visible.top );
    if (data->client_surface_backing && data->client_surface_backing_spare &&
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

    /* Keep capacity on both axes to avoid reallocating on every alternating
     * wide/tall resize. */
    width = max( width, data->client_surface_backing_width );
    height = max( height, data->client_surface_backing_height );
    old_valid = data->client_surface_backing_valid;
    old_valid_width = data->client_surface_backing_valid_width;
    old_valid_height = data->client_surface_backing_valid_height;

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
