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

#include "ntstatus.h"
#include "x11drv.h"
#include "client_surface.h"
#include "xcomposite.h"
#include "xpresent.h"
#include "client_surface_xcb.h"
#include "client_surface_query.h"
#include "client_surface_cache.h"
#include "wine/server.h"
#include "wine/rbtree.h"

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
    struct client_surface_memory_scope memory;
    struct client_surface_handoff_shared *shared;
    UINT64 id;
    SIZE_T size;
    unsigned int refs;
    unsigned int next_word;
    int ready_fd;
    UINT64 query_generation;
    UINT64 query_wait_bitmap[CLIENT_SURFACE_HANDOFF_BITMAP_WORDS];
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
    struct client_surface_cache_image *storage;
    Pixmap pixmap;
    unsigned int xcb_gc;
    unsigned int width, height, depth;
};

struct client_surface_compositor_reply
{
    struct list entry;
    struct client_surface_xcb_request *requests;
    unsigned int count;
    void (*complete)( struct client_surface_compositor_reply *reply, BOOL success );
};

struct client_surface_cache_copy
{
    struct client_surface_geometry_query query;
    BOOL query_pending;
    BOOL native_pending;
    struct client_surface_compositor_reply reply;
    struct client_surface_xcb_request request;
    struct client_surface_handoff_slot frame;
    UINT64 control, started;
    unsigned int index;
};

struct client_surface_compositor_binding
{
    struct client_surface_compositor_binding *next;
    struct client_surface_memory_scope memory;
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
    struct client_surface_cache_copy cache_copy;
    BOOL retired;
};

static BOOL client_surface_cache_read_pending( const struct client_surface_compositor_binding *binding )
{
    return binding->cache_copy.query_pending || binding->cache_copy.native_pending ||
           binding->cache_copy.reply.requests;
}

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

/* A queued native request is not a committed source checkpoint. Newer cache
 * reception may race an asynchronous copy, and must keep its replay dirty. */
static void note_client_surface_source_copy( struct client_surface_compositor_binding *binding,
                                             UINT64 epoch, UINT64 sequence )
{
    binding->source_epoch = epoch;
    binding->source_sequence = sequence;
    if (binding->latest_frame.source_sequence == sequence) binding->replay_epoch = epoch;
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
    enum { OWNER_COMPOSITE, DIRECT_ATTACH } strategy;
    UINT64 direct_identity;
    Window direct_drawable;
    struct client_surface_compositor_binding **members;
    struct client_surface_scene_layout *layouts;
    unsigned int count;
    UINT64 epoch;
    BOOL valid;
};

#define CLIENT_SURFACE_COMPOSITOR_FRAME_COUNT 3
#define CLIENT_SURFACE_COMPOSITOR_MAX_INFLIGHT 2
#define CLIENT_SURFACE_COMPOSITOR_DAMAGE_HISTORY 64

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
    struct client_surface_cache_image *image; /* borrowed from the installed pair/mailbox */
#ifdef SONAME_LIBXPRESENT
    struct rb_entry pixmap_entry;
#endif
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
    enum { OUTPUT_REQUEST_PRESENT, OUTPUT_REQUEST_COPY, OUTPUT_REQUEST_COPY_CLEANUP } request_op;
    struct client_surface_xcb_request request;
    unsigned int xcb_gc;
    struct client_surface_compositor_binding *copy_binding;
    struct client_surface_cache_image *copy_image;
    unsigned int copy_index;
    UINT64 copy_control;
    UINT64 copy_sequence;
    UINT64 copy_epoch;
    BOOL copy_replay;
    RECT copy_damage;
    struct client_surface_xcb_request copy_request;
    struct client_surface_compositor_reply reply;
    struct client_surface_compositor_target *reply_target;
    struct client_surface_copy_batch *reply_batch;
};

struct client_surface_compositor_target
{
    struct client_surface_compositor_target *next, **prev;
    struct rb_entry registry_entry;
#ifdef SONAME_LIBXPRESENT
    struct rb_entry present_entry;
#endif
    struct client_surface_memory_scope memory;
    struct client_surface_owner_notifications *notifications;
    HWND toplevel;
    Window window;
    struct client_surface_compositor_frame frames[CLIENT_SURFACE_COMPOSITOR_FRAME_COUNT];
    struct client_surface_compositor_frame *copy_frame;
    struct client_surface_output_transform *transform;
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
    BOOL update_resumed;
    UINT deferred_update_types;
    struct client_surface_compositor_mailbox *mailbox;
    DWORD shrink_start;
    XID present_event;
    unsigned int width;
    unsigned int height;
    unsigned int window_width;
    unsigned int window_height;
    unsigned int depth;
    VisualID visual;
};

struct client_surface_compositor_mailbox
{
    struct client_surface_memory_scope memory;
    struct client_surface_cache_image *image;
    HWND toplevel;
    Window window;
    unsigned int width, height, depth;
    BOOL pending;
};

/* Native completion owns no target, binding, mapping or Window pointer.
 * The live target's matching receipt is the authority to adopt this image. */
struct client_surface_output_transform
{
    struct client_surface_cache_transform native;
    struct client_surface_output_transform *next;
    struct client_surface_cache_image *image, *source, *catchup;
    HWND toplevel, window;
    Window destination;
    process_id_t process;
    UINT64 identity, cookie, epoch, sequence, control, revision;
    UINT64 generation, frame_revision;
    unsigned int buffer_index;
    unsigned int scene_index, width, height;
    BOOL replay;
    RECT damage;
    XRectangle clips[];
};

#ifdef SONAME_LIBXPRESENT
static int compare_client_surface_compositor_pixmap( const void *key, const struct rb_entry *entry )
{
    const struct client_surface_compositor_frame *frame =
        CONTAINING_RECORD( entry, const struct client_surface_compositor_frame, pixmap_entry );
    Pixmap a = *(const Pixmap *)key, b = frame->pixmap;

    return (a > b) - (a < b);
}

static struct rb_tree client_surface_compositor_pixmaps = {compare_client_surface_compositor_pixmap};
#endif

static void set_client_surface_compositor_pixmap( struct client_surface_compositor_frame *frame, Pixmap pixmap,
                                                 struct client_surface_cache_image *image )
{
#ifdef SONAME_LIBXPRESENT
    assert( !frame->reply_target && !frame->serial );
    if (frame->pixmap) rb_remove( &client_surface_compositor_pixmaps, &frame->pixmap_entry );
#endif
    frame->pixmap = pixmap;
    frame->image = image;
#ifdef SONAME_LIBXPRESENT
    if (pixmap)
    {
        assert( !rb_get( &client_surface_compositor_pixmaps, &pixmap ) );
        rb_put( &client_surface_compositor_pixmaps, &pixmap, &frame->pixmap_entry );
    }
#endif
}

/* Read leases are admitted only on the actor, after checking outstanding
 * writers. Selection and every explicit destination write run on that same
 * actor, so the shared check and native submission cannot race a new reader. */
static BOOL client_surface_compositor_frame_writable( const struct client_surface_compositor_frame *frame )
{
    return frame->image && !client_surface_cache_shared( frame->image );
}

static pthread_mutex_t client_surface_compositor_mutex = PTHREAD_MUTEX_INITIALIZER;
static Display *client_surface_compositor_display;
static BOOL client_surface_compositor_started;
static int client_surface_compositor_notify[2] = {-1, -1};
static struct client_surface_compositor_pool *client_surface_compositor_pools;
static struct client_surface_compositor_binding *client_surface_compositor_bindings;
static struct client_surface_compositor_target *client_surface_compositor_targets;
static struct client_surface_compositor_target *client_surface_compositor_next_target;
static unsigned int client_surface_compositor_target_count;
static UINT64 client_surface_compositor_target_generation;
static UINT64 client_surface_compositor_query_generation;
static UINT64 client_surface_compositor_mark;

struct client_surface_compositor_scan
{
    UINT64 generation, wake_serial;
    unsigned int remaining;
    DWORD timeout_start;
    int timeout;
    BOOL progressed;
};

static int compare_client_surface_compositor_target( const void *key, const struct rb_entry *entry )
{
    const struct client_surface_compositor_target *target =
        CONTAINING_RECORD( entry, const struct client_surface_compositor_target, registry_entry );
    ULONG_PTR a = (ULONG_PTR)key, b = (ULONG_PTR)target->toplevel;

    return (a > b) - (a < b);
}

/* The actor alone publishes and removes targets. Keep lookup independent of
 * maintenance traversal, without adding an allocation or a metadata lock to
 * each source/event. Both intrusive indices share the target's accounting. */
static struct rb_tree client_surface_compositor_target_registry = {compare_client_surface_compositor_target};

static void register_client_surface_compositor_target( struct client_surface_compositor_target *target )
{
    assert( !rb_get( &client_surface_compositor_target_registry, target->toplevel ) );
    rb_put( &client_surface_compositor_target_registry, target->toplevel, &target->registry_entry );
    target->next = client_surface_compositor_targets;
    target->prev = &client_surface_compositor_targets;
    if (target->next) target->next->prev = &target->next;
    client_surface_compositor_targets = target;
    if (!client_surface_compositor_next_target) client_surface_compositor_next_target = target;
    ++client_surface_compositor_target_count;
    ++client_surface_compositor_target_generation;
}

enum client_surface_compositor_op
{
    CLIENT_SURFACE_COMPOSITOR_REPLACE_POOL,
    CLIENT_SURFACE_COMPOSITOR_COPY,
    CLIENT_SURFACE_COMPOSITOR_FREE_POOL,
    CLIENT_SURFACE_COMPOSITOR_PRESENT,
    CLIENT_SURFACE_COMPOSITOR_REGISTER_HANDOFF,
    CLIENT_SURFACE_COMPOSITOR_REUSE_HANDOFFS,
    CLIENT_SURFACE_COMPOSITOR_CHECK_SCENE,
    CLIENT_SURFACE_COMPOSITOR_CHECK_CACHE,
    CLIENT_SURFACE_COMPOSITOR_REPAIR_OWNER,
    CLIENT_SURFACE_COMPOSITOR_RESOLVE_SOURCES,
    CLIENT_SURFACE_COMPOSITOR_SWEEP_HANDOFFS,
    CLIENT_SURFACE_COMPOSITOR_UPDATE_TARGET,
    CLIENT_SURFACE_COMPOSITOR_REMOVE_TARGET,
    CLIENT_SURFACE_COMPOSITOR_RESTORE_TARGET,
    CLIENT_SURFACE_COMPOSITOR_BEGIN_UPDATE,
    CLIENT_SURFACE_COMPOSITOR_TRY_BEGIN_UPDATE,
    CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE,
    CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE,
    CLIENT_SURFACE_COMPOSITOR_END_UPDATE,
    CLIENT_SURFACE_COMPOSITOR_DIRECT_PLAN,
    CLIENT_SURFACE_COMPOSITOR_DIRECT_COMPLETE,
    CLIENT_SURFACE_COMPOSITOR_RETIRE_POOL,
    CLIENT_SURFACE_COMPOSITOR_RENEW_DIRECT,
    CLIENT_SURFACE_COMPOSITOR_CREATE_POOL,
    CLIENT_SURFACE_COMPOSITOR_COPY_POOL,
};

struct client_surface_direct_completion
{
    Drawable source;
    UINT64 identity, scene_epoch, native_epoch;
};

struct client_surface_compositor_job
{
    struct client_surface_compositor_job *next;
    struct client_surface_compositor_queue *queue;
    UINT64 sequence;
    struct client_surface_owner_notifications *notifications;
    enum client_surface_compositor_op op;
    HWND toplevel; /* Scalar routing key, never a borrowed window-data pointer. */
    BOOL result;
    BOOL complete;
    BOOL async;
    union
    {
        /* COPY borrows the XIDs until synchronous completion. */
        struct
        {
            Drawable source, destination;
            int source_x, source_y, destination_x, destination_y;
            unsigned int width, height;
            BOOL shared;
        } copy;
        /* CREATE_POOL returns independently owned pending storage.
         * COPY_POOL admits an owned read of a completed OUTPUT rectangle.
         * REPLACE_POOL installs that checked copy, or synchronously reads
         * the borrowed Window checkpoint for the remaining legacy paths.
         * UPDATE_TARGET borrows the registered pair and uses the same
         * installation payload as the replacement transaction. */
        struct
        {
            struct client_surface_output_allocation *allocation;
            Drawable source, destination;
            unsigned int width, height, window_width, window_height;
            unsigned int copy_count, preserve_width, preserve_height;
            unsigned int valid_width, valid_height, depth;
            Pixmap pixmaps[2];
            VisualID visual;
            DWORD shrink_start;
            struct client_surface_scene scene;
            BOOL stale, invalid_source;
        } pool;
        /* FREE_POOL transfers its detached pair and accounting on enqueue. */
        Pixmap retired_pixmaps[2];
        /* PRESENT borrows registered output XIDs. The target keeps the native
         * serial after a caller timeout; only its waiter pointer is detached. */
        struct
        {
            Drawable source, destination;
            unsigned int width, height;
            BOOL started, done;
            DWORD start;
        } present;
        /* REGISTER_HANDOFF borrows mapping until synchronous return. ready_fd
         * is consumed by a new pool or closed at job completion/cancellation. */
        struct
        {
            HANDLE mapping;
            SIZE_T view_size, offset;
            UINT64 mapping_id, cookie, identity, mark;
            int ready_fd;
            process_id_t process;
            HWND window;
        } registration;
        /* REUSE_HANDOFFS borrows both input and output arrays until return. */
        struct
        {
            const struct client_surface_handoff_desc *handoffs;
            unsigned int count;
            BOOL *reused;
            UINT64 mark;
        } reuse;
        /* CHECK_SCENE only borrows the roster; it cannot adopt layouts. */
        struct
        {
            const struct client_surface_handoff_desc *handoffs;
            unsigned int count;
            UINT64 epoch;
        } scene_check;
        /* SWEEP_HANDOFFS adopts layouts only when installing the plan, then
         * clears this pointer/count. Failure, stale and unchanged plans leave
         * them with the synchronous caller, including all clip allocations. */
        struct
        {
            struct client_surface_scene_layout *layouts;
            unsigned int count;
            UINT64 epoch, mark;
        } scene_install;
        /* DIRECT_PLAN borrows drawable IDs for synchronous native checks. */
        struct
        {
            Drawable source, destination;
            UINT64 identity, scene_epoch;
        } direct_plan;
        /* RENEW_DIRECT borrows only the owner's destination and geometry. */
        struct
        {
            Drawable destination;
            UINT64 scene_epoch;
            int source_x, source_y;
            unsigned int width, height, window_width, window_height;
        } direct_renew;
        /* DIRECT_COMPLETE carries scalar attestations, no native lease. */
        struct client_surface_direct_completion direct_complete;
        /* RESTORE_TARGET borrows the destination; deferred restoration is
         * stored on the target, without retaining this stack job. */
        struct
        {
            Drawable destination;
            int destination_x, destination_y;
            unsigned int width, height, window_width, window_height;
            unsigned int valid_width, valid_height;
        } restore;
        /* Native-update operations carry scalar barriers and return values. */
        struct
        {
            struct client_surface_owner_notifications *notifications;
            UINT64 mark;
            unsigned int count;
            BOOL invalidate_scene, deferred;
            UINT types;
        } update;
    } u;
};

struct client_surface_compositor_request
{
    struct client_surface_compositor_job job;
    struct client_surface_memory_scope memory;
    pthread_cond_t completed;
};

/* The routing lifetime spans initial target preparation, replacement and the
 * last resource release. Only refs/registry/admission use compositor_mutex;
 * the actor alone owns both FIFOs and the ready/parked list entry. */
struct client_surface_compositor_queue
{
    struct rb_entry registry_entry;
    struct client_surface_memory_scope memory;
    struct client_surface_compositor_target *target;
    struct list entry;
    struct client_surface_compositor_job *head, **tail, *control_head, **control_tail;
    HWND toplevel;
    unsigned int refs, incoming, requests;
    /* Native update barriers and removal already have synchronous callers.
     * Serialize their storage within the admitted queue, independently of
     * ordinary request pressure. Release notifications never use this slot. */
    struct client_surface_compositor_request barrier;
    pthread_cond_t barrier_available;
    BOOL barrier_in_use;
};

enum client_surface_compositor_capacity_kind
{
    CLIENT_SURFACE_COMPOSITOR_REQUEST_CAPACITY,
    CLIENT_SURFACE_COMPOSITOR_QUEUE_CAPACITY,
    CLIENT_SURFACE_COMPOSITOR_RELEASE_CAPACITY,
    CLIENT_SURFACE_COMPOSITOR_CAPACITY_COUNT,
};

static const struct { unsigned int count; SIZE_T bytes; } client_surface_compositor_limits[] =
{
    [CLIENT_SURFACE_COMPOSITOR_REQUEST_CAPACITY] = {1024, 256 * 1024},
    [CLIENT_SURFACE_COMPOSITOR_QUEUE_CAPACITY] = {2048, 1024 * 1024},
    [CLIENT_SURFACE_COMPOSITOR_RELEASE_CAPACITY] = {4096, 1024 * 1024},
};
#define CLIENT_SURFACE_COMPOSITOR_REQUESTS_PER_TARGET 64
static struct { unsigned int count; SIZE_T bytes; } client_surface_compositor_capacity[CLIENT_SURFACE_COMPOSITOR_CAPACITY_COUNT];

static struct client_surface_compositor_job *client_surface_compositor_head;
static struct client_surface_compositor_job **client_surface_compositor_tail =
    &client_surface_compositor_head;
static int compare_client_surface_compositor_queue( const void *key, const struct rb_entry *entry )
{
    const struct client_surface_compositor_queue *queue =
        CONTAINING_RECORD( entry, const struct client_surface_compositor_queue, registry_entry );
    ULONG_PTR a = (ULONG_PTR)key, b = (ULONG_PTR)queue->toplevel;

    return (a > b) - (a < b);
}

static struct rb_tree client_surface_compositor_queues = {compare_client_surface_compositor_queue};
static struct list client_surface_compositor_ready = LIST_INIT( client_surface_compositor_ready );
static struct list client_surface_compositor_parked = LIST_INIT( client_surface_compositor_parked );
static UINT64 client_surface_compositor_wake_serial;
static UINT64 client_surface_compositor_sequence;
static LONGLONG client_surface_compositor_domain;
static UINT64 client_surface_native_update_serial;

/* One object owns all notifications for a target lifetime. BEGIN and resumed
 * GUI notifications retain references until END/FINISH dispatch, even after
 * the target is removed. DIRECT receipts hold a queue reference and coalesce
 * only within the authenticated plan. All fields here use compositor_mutex;
 * execution consumes a private job payload, never a concurrently written one. */
struct client_surface_owner_notifications
{
    struct client_surface_owner_notifications *next;
    struct client_surface_compositor_queue *queue;
    struct client_surface_memory_scope memory;
    HWND toplevel;
    UINT64 refs;
    struct client_surface_compositor_job end, finish, direct;
    unsigned int pending_ends;
    BOOL end_queued, finish_queued, direct_queued, direct_pending;
    UINT64 finish_serial;
    struct client_surface_direct_completion direct_plan, direct_proof;
};

static struct client_surface_owner_notifications *client_surface_owner_notifications;

/* The release node is part of the admitted output storage. The GUI can pass
 * ownership back without allocating or waiting for the actor. The registry
 * links are protected by compositor_mutex. The actor drops its independently
 * owned native images after draining their users; each image keeps its own
 * work node and accounting until native destruction actually completes. */
struct client_surface_output_allocation
{
    struct client_surface_compositor_job release;
    struct client_surface_output_allocation *next;
    struct client_surface_memory_scope memory;
    struct client_surface_cache_image *images[2];
    Pixmap pixmaps[2];
    UINT64 bytes;
    UINT64 serial;
    unsigned int width, height, depth;
    /* A capacity-only replacement reads a completed OUTPUT, never Window
     * pixels. This lease survives cancellation and target/pool removal. */
    struct client_surface_cache_image *source_image;
    struct x11drv_native_window *window_owner;
    struct client_surface_scene scene;
    Pixmap source, published;
    Window window;
    UINT64 source_revision;
    unsigned int window_width, window_height, copy_width, copy_height;
    BOOL checkpoint, copy_wait, force;
    /* The GUI owns the pending pointer. The callbacks retain this object
     * until both native operations finish, even after the GUI abandons it.
     * These three fields use compositor_mutex; no callback touches win_data. */
    unsigned int pending;
    BOOL failed, abandoned;
    struct list notification_entry;
};

static UINT64 client_surface_output_allocation_serial;
static struct list client_surface_output_notifications = LIST_INIT( client_surface_output_notifications );
static unsigned int client_surface_output_notification_count;

static struct client_surface_output_allocation *client_surface_output_allocations;

#define CLIENT_SURFACE_COPY_BATCH_SIZE CLIENT_SURFACE_CACHE_TRANSFORM_LIMIT
struct client_surface_owner_copy
{
    struct client_surface_compositor_binding *binding;
    struct client_surface_cache_image *image;
    unsigned int buffer_index;
    UINT64 control, generation, epoch;
    UINT64 sequence;
    BOOL replay;
};

struct client_surface_copy_batch
{
    struct client_surface_compositor_target *target;
    struct client_surface_compositor_frame *frame;
    struct client_surface_output_transform *transform, *tail;
    struct client_surface_owner_copy copies[CLIENT_SURFACE_COPY_BATCH_SIZE];
    unsigned int count;
    int error;
    BOOL asynchronous;
    UINT64 revision;
    struct client_surface_xcb_request requests[CLIENT_SURFACE_COPY_BATCH_SIZE];
};

static struct client_surface_copy_batch client_surface_copy_batch;
static struct client_surface_copy_batch client_surface_pending_batches[64];
static unsigned int client_surface_pending_batch_count;
static struct list client_surface_compositor_replies = LIST_INIT( client_surface_compositor_replies );

static void complete_client_surface_output_reply( struct client_surface_compositor_reply *reply, BOOL success );
static BOOL replay_client_surface_scene_sources( struct client_surface_compositor_target *target,
                                                 unsigned int *budget );

static void queue_client_surface_reply( struct client_surface_compositor_reply *reply,
                                        struct client_surface_xcb_request *requests, unsigned int count,
                                        void (*complete)( struct client_surface_compositor_reply *, BOOL ) )
{
    assert( !reply->requests && count );
    reply->requests = requests;
    reply->count = count;
    reply->complete = complete;
    list_add_tail( &client_surface_compositor_replies, &reply->entry );
}

/* Every checked group ends in a GetInputFocus barrier on this actor's sole
 * connection. Keep that submission order so a later poll cannot buffer an
 * earlier reply behind an already inspected target before the actor sleeps.
 * The frame's existing native-use lifetime owns this intrusive notification. */
static void queue_client_surface_compositor_reply( struct client_surface_compositor_target *target,
                                                   struct client_surface_compositor_frame *frame,
                                                   struct client_surface_copy_batch *batch )
{
    assert( !frame->reply_target );
    frame->reply_target = target;
    frame->reply_batch = batch;
    queue_client_surface_reply( &frame->reply,
        batch ? batch->requests : frame->request_pending ? &frame->request : &frame->copy_request,
        batch ? batch->count : 1, complete_client_surface_output_reply );
}

static void flush_client_surface_copy_batch(void);

static struct client_surface_compositor_frame *get_client_surface_compositor_pixmap(
    struct client_surface_compositor_target *target, Pixmap pixmap )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        if (target->frames[i].pixmap == pixmap) return &target->frames[i];
    return NULL;
}

static void abort_client_surface_output_transform_assembly( struct client_surface_compositor_target *target,
                                                            struct client_surface_output_transform *transform );

static void detach_client_surface_output_transform( struct client_surface_compositor_target *target )
{
    if (!target->transform) return;
    TRACE_(csperf)( "ticks=%llu event=output_transform_detach transform=%p hwnd=%p destination=%lx\n",
                   client_surface_perf_time(), target->transform, target->toplevel, target->window );
    abort_client_surface_output_transform_assembly( target, target->transform );
    target->transform = NULL;
    /* The worker owns the image write until its checked completion. Shared
     * storage cannot become a new destination even after logical removal. */
    target->replay_member = 0;
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
static void enqueue_client_surface_compositor_job( struct client_surface_compositor_job *job );

static void wake_client_surface_compositor_queues(void)
{
    ++client_surface_compositor_wake_serial;
    /* Native completion or a timer can make parked heads runnable. Move the
     * list in constant time; dispatch still inspects at most 64 queue heads. */
    list_move_tail( &client_surface_compositor_ready, &client_surface_compositor_parked );
}
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
        TRACE_(csperf)( "ticks=%llu event=xlib_copy_request source=%lx destination=%lx width=%u height=%u clipped=0 route=restore\n",
                       client_surface_perf_time(), source, destination, width, height );
        XFreeGC( display, gc );
    }
    XSync( display, False );
    X11DRV_check_error();
    TRACE_(csperf)( "ticks=%llu event=xlib_restore_checked source=%lx destination=%lx copied=%u error=%d sync_calls=1\n",
                   client_surface_perf_time(), source, destination, !!gc, error );
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

/* Capacity is reserved before allocation and returned after the real free.
 * Reserved release nodes compete only when their resources are constructed,
 * never when those resources need to retire. Caller holds compositor_mutex. */
static BOOL reserve_client_surface_compositor_capacity( enum client_surface_compositor_capacity_kind kind,
                                                        unsigned int count, SIZE_T bytes,
                                                        struct client_surface_compositor_queue *queue )
{
    BOOL accepted = count <= client_surface_compositor_limits[kind].count - client_surface_compositor_capacity[kind].count &&
                    bytes <= client_surface_compositor_limits[kind].bytes - client_surface_compositor_capacity[kind].bytes;

    if (queue) accepted = accepted && queue->requests < CLIENT_SURFACE_COMPOSITOR_REQUESTS_PER_TARGET;
    if (accepted)
    {
        client_surface_compositor_capacity[kind].count += count;
        client_surface_compositor_capacity[kind].bytes += bytes;
        if (queue) ++queue->requests;
    }
    TRACE_(csperf)( "ticks=%llu event=compositor_admission kind=%u count=%u bytes=%zu accepted=%u "
                   "used=%u used_bytes=%zu hwnd=%p target_requests=%u\n", client_surface_perf_time(), kind,
                   count, (size_t)bytes, accepted, client_surface_compositor_capacity[kind].count,
                   (size_t)client_surface_compositor_capacity[kind].bytes, queue ? queue->toplevel : NULL,
                   queue ? queue->requests : 0 );
    return accepted;
}

static void release_client_surface_compositor_capacity( enum client_surface_compositor_capacity_kind kind,
                                                        unsigned int count, SIZE_T bytes,
                                                        struct client_surface_compositor_queue *queue )
{
    pthread_mutex_lock( &client_surface_compositor_mutex );
    assert( client_surface_compositor_capacity[kind].count >= count && client_surface_compositor_capacity[kind].bytes >= bytes );
    client_surface_compositor_capacity[kind].count -= count;
    client_surface_compositor_capacity[kind].bytes -= bytes;
    if (queue)
    {
        assert( queue->requests );
        --queue->requests;
    }
    TRACE_(csperf)( "ticks=%llu event=compositor_capacity_return kind=%u count=%u bytes=%zu used=%u used_bytes=%zu "
                   "hwnd=%p target_requests=%u\n", client_surface_perf_time(), kind, count, (size_t)bytes,
                   client_surface_compositor_capacity[kind].count, (size_t)client_surface_compositor_capacity[kind].bytes,
                   queue ? queue->toplevel : NULL, queue ? queue->requests : 0 );
    pthread_mutex_unlock( &client_surface_compositor_mutex );
}

BOOL x11drv_reserve_release_capacity( unsigned int count, SIZE_T bytes )
{
    BOOL ret;

    pthread_mutex_lock( &client_surface_compositor_mutex );
    ret = reserve_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_RELEASE_CAPACITY, count, bytes, NULL );
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    return ret;
}

void x11drv_return_release_capacity( unsigned int count, SIZE_T bytes )
{
    release_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_RELEASE_CAPACITY, count, bytes, NULL );
}

static void free_client_surface_compositor_release( struct client_surface_memory_scope *memory, void *data,
                                                    unsigned int count, SIZE_T bytes )
{
    client_surface_free_owned_metadata( memory, data, bytes );
    x11drv_return_release_capacity( count, bytes );
}

/* Routing storage is admitted before the actor starts. Its account uses the
 * same stable native domain as the actor's images, never the GUI thread's
 * execution domain. Each allocation retains it through actual retirement. */
static void *alloc_client_surface_compositor_metadata( HWND toplevel, SIZE_T size,
                                                        struct client_surface_memory_scope *memory )
{
    UINT64 domain;
    void *data;

    pthread_mutex_lock( &client_surface_compositor_mutex );
    if (!client_surface_compositor_domain)
        client_surface_compositor_domain = client_surface_allocate_completion_domains( 1 );
    domain = client_surface_compositor_domain;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    if (!domain || !client_surface_memory_scope_init( memory, toplevel, domain )) return NULL;
    if (!(data = client_surface_alloc_scoped_metadata( memory, 1, size )))
        client_surface_memory_scope_destroy( memory );
    return data;
}

static void free_client_surface_compositor_queue( struct client_surface_compositor_queue *queue )
{
    pthread_cond_destroy( &queue->barrier_available );
    pthread_cond_destroy( &queue->barrier.completed );
    client_surface_free_owned_metadata( &queue->memory, queue, sizeof(*queue) );
    release_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_QUEUE_CAPACITY, 1, sizeof(*queue), NULL );
}

static struct client_surface_compositor_queue *get_client_surface_compositor_queue( HWND toplevel )
{
    struct client_surface_compositor_queue *queue, *created = NULL;
    struct client_surface_memory_scope memory = {0};
    struct rb_entry *entry;

    assert( toplevel );
    pthread_mutex_lock( &client_surface_compositor_mutex );
    for (;;)
    {
        if ((entry = rb_get( &client_surface_compositor_queues, toplevel )))
        {
            queue = CONTAINING_RECORD( entry, struct client_surface_compositor_queue, registry_entry );
            ++queue->refs;
            break;
        }
        if (created)
        {
            queue = created;
            created = NULL;
            rb_put( &client_surface_compositor_queues, toplevel, &queue->registry_entry );
            break;
        }
        if (!reserve_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_QUEUE_CAPACITY, 1, sizeof(*created), NULL ))
        {
            pthread_mutex_unlock( &client_surface_compositor_mutex );
            return NULL;
        }
        pthread_mutex_unlock( &client_surface_compositor_mutex );
        if (!(created = alloc_client_surface_compositor_metadata( toplevel, sizeof(*created), &memory )))
        {
            release_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_QUEUE_CAPACITY, 1, sizeof(*created), NULL );
            return NULL;
        }
        created->memory = memory;
        if (pthread_cond_init( &created->barrier.completed, NULL )) goto failed;
        if (pthread_cond_init( &created->barrier_available, NULL ))
        {
            pthread_cond_destroy( &created->barrier.completed );
            goto failed;
        }
        created->toplevel = toplevel;
        created->refs = 1;
        created->tail = &created->head;
        created->control_tail = &created->control_head;
        list_init( &created->entry );
        pthread_mutex_lock( &client_surface_compositor_mutex );
    }
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    if (created) free_client_surface_compositor_queue( created );
    return queue;

failed:
    client_surface_free_owned_metadata( &created->memory, created, sizeof(*created) );
    release_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_QUEUE_CAPACITY, 1, sizeof(*created), NULL );
    return NULL;
}

static void release_client_surface_compositor_queue( struct client_surface_compositor_queue *queue )
{
    BOOL unused;

    pthread_mutex_lock( &client_surface_compositor_mutex );
    assert( queue->refs );
    if ((unused = !--queue->refs))
    {
        assert( !queue->head && !queue->control_head && !queue->incoming && !queue->requests && !queue->barrier_in_use &&
                !queue->target && list_empty( &queue->entry ) );
        rb_remove( &client_surface_compositor_queues, &queue->registry_entry );
    }
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    if (unused) free_client_surface_compositor_queue( queue );
}

static struct client_surface_compositor_target *alloc_client_surface_compositor_target( HWND toplevel )
{
    struct client_surface_memory_scope memory = {0};
    struct client_surface_compositor_target *target;
    struct client_surface_owner_notifications *notifications;

    if (!(target = alloc_client_surface_compositor_metadata( toplevel, sizeof(*target), &memory ))) return NULL;
    target->memory = memory;
    target->toplevel = toplevel;
    if (!x11drv_reserve_release_capacity( 3, sizeof(*notifications) ))
    {
        client_surface_free_owned_metadata( &target->memory, target, sizeof(*target) );
        return NULL;
    }
    memset( &memory, 0, sizeof(memory) );
    client_surface_memory_scope_copy( &memory, &target->memory, TRUE );
    if (!(notifications = client_surface_alloc_scoped_metadata( &memory, 1, sizeof(*notifications) )))
    {
        client_surface_memory_scope_destroy( &memory );
        client_surface_free_owned_metadata( &target->memory, target, sizeof(*target) );
        release_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_RELEASE_CAPACITY, 3, sizeof(*notifications), NULL );
        return NULL;
    }
    notifications->memory = memory;
    /* The creating job already owns this queue, so this lookup cannot need
     * another allocation or depend on a second admission decision. */
    notifications->queue = get_client_surface_compositor_queue( toplevel );
    assert( notifications->queue );
    notifications->toplevel = toplevel;
    notifications->refs = 1;
    notifications->end = (struct client_surface_compositor_job){
        .op = CLIENT_SURFACE_COMPOSITOR_END_UPDATE, .toplevel = toplevel,
        .notifications = notifications, .queue = notifications->queue, .async = TRUE};
    notifications->finish = (struct client_surface_compositor_job){
        .op = CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE, .toplevel = toplevel,
        .notifications = notifications, .queue = notifications->queue, .async = TRUE};
    notifications->direct = (struct client_surface_compositor_job){
        .op = CLIENT_SURFACE_COMPOSITOR_DIRECT_COMPLETE, .toplevel = toplevel,
        .notifications = notifications, .queue = notifications->queue, .async = TRUE};
    target->notifications = notifications;
    notifications->queue->target = target;
    pthread_mutex_lock( &client_surface_compositor_mutex );
    notifications->next = client_surface_owner_notifications;
    client_surface_owner_notifications = notifications;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    return target;
}

static void free_client_surface_compositor_target( struct client_surface_compositor_target *target )
{
    struct client_surface_owner_notifications **cursor, *notifications = target->notifications;
    BOOL unused;

    assert( notifications->queue->target == target );
    notifications->queue->target = NULL;
    pthread_mutex_lock( &client_surface_compositor_mutex );
    for (cursor = &client_surface_owner_notifications; *cursor != notifications; cursor = &(*cursor)->next)
        assert( *cursor );
    *cursor = notifications->next;
    unused = !--notifications->refs;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    if (unused)
    {
        release_client_surface_compositor_queue( notifications->queue );
        free_client_surface_compositor_release( &notifications->memory, notifications, 3, sizeof(*notifications) );
    }
    client_surface_free_owned_metadata( &target->memory, target, sizeof(*target) );
}

static void update_client_surface_notification_plan( struct client_surface_compositor_target *target )
{
    struct client_surface_owner_notifications *notifications = target->notifications;

    pthread_mutex_lock( &client_surface_compositor_mutex );
    notifications->direct_plan = (struct client_surface_direct_completion){0};
    if (target->scene.valid && target->scene.strategy == DIRECT_ATTACH)
        notifications->direct_plan = (struct client_surface_direct_completion){
            .identity = target->scene.direct_identity, .scene_epoch = target->scene.epoch,
            .source = target->scene.direct_drawable};
    pthread_mutex_unlock( &client_surface_compositor_mutex );
}

static void wake_client_surface_compositor(void);

static void release_client_surface_output_checkpoint( struct client_surface_output_allocation *allocation )
{
    client_surface_cache_release( allocation->source_image );
    allocation->source_image = NULL;
    if (allocation->window_owner) x11drv_native_window_release( allocation->window_owner );
    allocation->window_owner = NULL;
}

static void free_client_surface_pending_allocation( struct client_surface_output_allocation *allocation )
{
    assert( !allocation->pending );
    release_client_surface_output_checkpoint( allocation );
    client_surface_cache_release( allocation->images[0] );
    client_surface_cache_release( allocation->images[1] );
    release_client_surface_compositor_queue( allocation->release.queue );
    free_client_surface_compositor_release( &allocation->memory, allocation, 1, sizeof(*allocation) );
}

static void client_surface_output_allocation_complete( void *context, BOOL success )
{
    struct client_surface_output_allocation *allocation = context;
    BOOL abandoned = FALSE;

    pthread_mutex_lock( &client_surface_compositor_mutex );
    allocation->failed |= !success;
    assert( allocation->pending );
    if (!--allocation->pending)
    {
        if (!allocation->failed)
        {
            allocation->pixmaps[0] = client_surface_cache_pixmap( allocation->images[0] );
            allocation->pixmaps[1] = client_surface_cache_pixmap( allocation->images[1] );
        }
        abandoned = allocation->abandoned;
        if (!abandoned)
        {
            list_add_tail( &client_surface_output_notifications, &allocation->notification_entry );
            ++client_surface_output_notification_count;
        }
    }
    TRACE_(csperf)( "ticks=%llu event=%s request=%p serial=%llu pending=%u failed=%u abandoned=%u\n",
                   client_surface_perf_time(), allocation->checkpoint ? "output_pair_copy_complete" : "output_pair_create_complete",
                   allocation, (unsigned long long)allocation->serial, allocation->pending,
                   allocation->failed, allocation->abandoned );
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    /* A cancelled GUI owner may release the object immediately after
     * unlocking. Only the abandoned callback retains final ownership here. */
    if (abandoned) free_client_surface_pending_allocation( allocation );
}

static BOOL create_client_surface_output_allocation( struct client_surface_compositor_job *job )
{
    struct client_surface_memory_scope memory = {0};
    struct client_surface_output_allocation *allocation;

    if (!x11drv_reserve_release_capacity( 1, sizeof(*allocation) )) return FALSE;
    if (!(allocation = alloc_client_surface_compositor_metadata( job->toplevel, sizeof(*allocation), &memory )))
    {
        release_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_RELEASE_CAPACITY, 1, sizeof(*allocation), NULL );
        return FALSE;
    }
    allocation->memory = memory;
    list_init( &allocation->notification_entry );
    allocation->release.queue = get_client_surface_compositor_queue( job->toplevel );
    assert( allocation->release.queue );
    allocation->release.toplevel = job->toplevel;
    allocation->bytes = 2 * client_surface_pixmap_bytes( job->u.pool.width, job->u.pool.height, job->u.pool.depth );
    if (!client_surface_cache_reserve_output_pair( &allocation->memory, allocation->bytes / 2,
                                                   wake_client_surface_compositor, allocation->images ))
    {
        release_client_surface_compositor_queue( allocation->release.queue );
        free_client_surface_compositor_release( &allocation->memory, allocation, 1, sizeof(*allocation) );
        return FALSE;
    }
    allocation->width = job->u.pool.width;
    allocation->height = job->u.pool.height;
    allocation->depth = job->u.pool.depth;
    if (!++client_surface_output_allocation_serial) ++client_surface_output_allocation_serial;
    allocation->serial = client_surface_output_allocation_serial;
    allocation->pending = 2;
    job->u.pool.allocation = allocation;
    client_surface_cache_create_output_pair( allocation->images, allocation->width, allocation->height,
                                              allocation->depth, client_surface_output_allocation_complete, allocation );
    return TRUE;
}

static void register_client_surface_output_allocation( struct client_surface_output_allocation *allocation )
{
    release_client_surface_output_checkpoint( allocation );
    pthread_mutex_lock( &client_surface_compositor_mutex );
    allocation->next = client_surface_output_allocations;
    client_surface_output_allocations = allocation;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
}

static BOOL client_surface_alloc_on_compositor( struct client_surface_compositor_job *job )
{
    Display *display = client_surface_compositor_display;
    struct client_surface_output_allocation *allocation = job->u.pool.allocation;
    unsigned int i, copy_width = min( job->u.pool.window_width, job->u.pool.width );
    unsigned int copy_height = min( job->u.pool.window_height, job->u.pool.height );
    GC gc;
    int error = 0;

    assert( allocation && !allocation->pending && !allocation->failed );
    assert( job->u.pool.copy_count && job->u.pool.copy_count <= ARRAY_SIZE(job->u.pool.pixmaps) );
    memcpy( job->u.pool.pixmaps, allocation->pixmaps, sizeof(job->u.pool.pixmaps) );
    X11DRV_expect_error( display, client_surface_compositor_error, &error );
    /* The owner completed its drawing before this job. All destinations are
     * private new images, so allocation and checkpoint errors can share one
     * reply boundary. Do not register or install any of them before it. */
    gc = XCreateGC( display, job->u.pool.pixmaps[0], 0, NULL );
    if (gc)
    {
        for (i = 0; i < job->u.pool.copy_count; ++i)
        {
            XCopyArea( display, job->u.pool.destination, job->u.pool.pixmaps[i], gc, 0, 0,
                       copy_width, copy_height, 0, 0 );
            TRACE_(csperf)( "ticks=%llu event=xlib_copy_request source=%lx destination=%lx width=%u height=%u clipped=0 route=restore\n",
                           client_surface_perf_time(), job->u.pool.destination, job->u.pool.pixmaps[i],
                           copy_width, copy_height );
            /* The replacement job drained the old target before entering.
             * Preserve its completed intersection over the GUI seed without
             * exposing either new image before the common error boundary. */
            if (job->u.pool.source && job->u.pool.preserve_width && job->u.pool.preserve_height)
            {
                XCopyArea( display, job->u.pool.source, job->u.pool.pixmaps[i], gc, 0, 0,
                           job->u.pool.preserve_width, job->u.pool.preserve_height, 0, 0 );
                TRACE_(csperf)( "ticks=%llu event=xlib_copy_request source=%lx destination=%lx width=%u height=%u clipped=0 route=restore\n",
                               client_surface_perf_time(), job->u.pool.source, job->u.pool.pixmaps[i],
                               job->u.pool.preserve_width, job->u.pool.preserve_height );
            }
        }
        XFreeGC( display, gc );
    }
    XSync( display, False );
    X11DRV_check_error();
    TRACE_(csperf)( "ticks=%llu event=output_pool_alloc first=%lx second=%lx "
                   "width=%u height=%u depth=%u checkpoint_copies=%u copy_width=%u copy_height=%u sync_calls=1 error=%d success=%u\n",
                   client_surface_perf_time(), job->u.pool.pixmaps[0], job->u.pool.pixmaps[1],
                   job->u.pool.width, job->u.pool.height, job->u.pool.depth, gc ? job->u.pool.copy_count : 0,
                   copy_width, copy_height, error, !!gc && !error );
    if (gc && !error)
    {
        memcpy( allocation->pixmaps, job->u.pool.pixmaps, sizeof(allocation->pixmaps) );
        register_client_surface_output_allocation( allocation );
        return TRUE;
    }

    job->u.pool.pixmaps[0] = job->u.pool.pixmaps[1] = 0;
    return FALSE;
}

static BOOL release_client_surface_output_allocation( const Pixmap pixmaps[2] )
{
    struct client_surface_output_allocation **cursor, *allocation;

    pthread_mutex_lock( &client_surface_compositor_mutex );
    for (cursor = &client_surface_output_allocations; (allocation = *cursor); cursor = &allocation->next)
        if ((allocation->pixmaps[0] == pixmaps[0] && allocation->pixmaps[1] == pixmaps[1]) ||
            (allocation->pixmaps[0] == pixmaps[1] && allocation->pixmaps[1] == pixmaps[0])) break;
    assert( allocation );
    *cursor = allocation->next;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    /* Every copy and Present user has drained before dispatch. The worker
     * receives no target or release-job pointer, and its native reply can
     * arrive after this allocation's GUI/actor bookkeeping is gone. */
    client_surface_cache_release( allocation->images[0] );
    client_surface_cache_release( allocation->images[1] );
    /* A rollback has no queued node. Otherwise the dispatcher still owns
     * release, including its result and queue link, until it retires it. */
    if (!allocation->release.async)
    {
        release_client_surface_compositor_queue( allocation->release.queue );
        free_client_surface_compositor_release( &allocation->memory, allocation, 1, sizeof(*allocation) );
    }
    return TRUE;
}

#ifdef SONAME_LIBXPRESENT

static int compare_client_surface_compositor_present( const void *key, const struct rb_entry *entry )
{
    const struct client_surface_compositor_target *target =
        CONTAINING_RECORD( entry, const struct client_surface_compositor_target, present_entry );
    XID a = *(const XID *)key, b = target->present_event;

    return (a > b) - (a < b);
}

static struct rb_tree client_surface_compositor_present_registry = {compare_client_surface_compositor_present};

static struct client_surface_compositor_target *find_client_surface_compositor_present( XID event, Window window )
{
    struct client_surface_compositor_target *target;
    struct rb_entry *entry;

    if (!(entry = rb_get( &client_surface_compositor_present_registry, &event ))) return NULL;
    target = CONTAINING_RECORD( entry, struct client_surface_compositor_target, present_entry );
    return target->window == window ? target : NULL;
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
    TRACE( "%s serial %u pixmap %#lx completed and became idle\n",
           frame->request_op ? "staged copy" : "X Present", frame->serial, frame->pixmap );
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
        assert( frame->waiter->op == CLIENT_SURFACE_COMPOSITOR_PRESENT );
        frame->waiter->u.present.done = TRUE;
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

static void complete_client_surface_present_request( struct client_surface_compositor_target *target,
                                                     struct client_surface_compositor_frame *frame, BOOL success )
{
    frame->request_pending = FALSE;
    TRACE( "validated %s request %u serial %u success %u\n",
           frame->request_op ? "staged copy" : "X Present", frame->request.cookies[0], frame->serial, success );
    if (frame->request_op != OUTPUT_REQUEST_PRESENT)
    {
        if (frame->request_op == OUTPUT_REQUEST_COPY)
        {
            TRACE_(csperf)( "ticks=%llu event=publish_copy_complete window=%lx pixmap=%lx serial=%u "
                           "generation=%llu epoch=%llu success=%u\n", client_surface_perf_time(), target->window,
                           frame->pixmap, frame->serial, frame->publish_generation, frame->publish_epoch, success );
            if (!success && frame->xcb_gc)
            {
                /* A failed CreateGC may leave only an allocated XID. Keep the
                 * frame owned while its checked free consumes either result. */
                frame->request_op = OUTPUT_REQUEST_COPY_CLEANUP;
                frame->request_pending = TRUE;
                client_surface_xcb_free_gc_async( client_surface_compositor_display,
                                                  frame->xcb_gc, &frame->request );
                queue_client_surface_compositor_reply( target, frame, NULL );
                return;
            }
        }
        else
        {
            TRACE_(csperf)( "ticks=%llu event=publish_copy_cleanup window=%lx pixmap=%lx serial=%u "
                           "gc=%u success=%u\n", client_surface_perf_time(), target->window,
                           frame->pixmap, frame->serial, frame->xcb_gc, success );
            frame->xcb_gc = 0;
            success = FALSE;
        }
        /* The checked destination copy owns its completion. Neither a Present
         * event nor another copy may turn a failed receipt into exposure. */
        frame->last_complete_success = success;
        frame->last_complete_serial = frame->serial;
        frame->complete = frame->idle = TRUE;
    }
    else if (!success)
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
                (target = find_client_surface_compositor_present( notify->eid, notify->window )) &&
                (frame = find_client_surface_compositor_frame( target,
                                                               notify->serial_number, 0 )))
            {
                BOOL success = notify->mode != PresentCompleteModeSkip;

                TRACE_(csperf)( "ticks=%llu event=complete window=%lx pixmap=%lx serial=%u "
                               "mode=%u ust=%s msc=%s event_id=%lx\n", client_surface_perf_time(), target->window,
                               frame->pixmap, frame->serial, notify->mode,
                               wine_dbgstr_longlong( notify->ust ), wine_dbgstr_longlong( notify->msc ), target->present_event );
                frame->complete = TRUE;
                frame->last_complete_serial = frame->serial;
                frame->last_complete_success = success;
                complete_client_surface_compositor_frame( target, frame );
            }
        }
        else if (event.xcookie.evtype == PresentIdleNotify)
        {
            XPresentIdleNotifyEvent *notify = event.xcookie.data;

            if ((target = find_client_surface_compositor_present( notify->eid, notify->window )) &&
                (frame = find_client_surface_compositor_frame( target,
                    notify->serial_number, notify->pixmap )))
            {
                TRACE_(csperf)( "ticks=%llu event=idle window=%lx pixmap=%lx serial=%u event_id=%lx\n",
                               client_surface_perf_time(), target->window, frame->pixmap, frame->serial, target->present_event );
                frame->idle = TRUE;
            }
        }
        pXFreeEventData( display, &event );
        if (target && frame)
        {
            finish_client_surface_compositor_frame( target, frame );
            wake_client_surface_compositor_queues();
        }
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

static BOOL wait_client_surface_compositor_pixmap_idle( Pixmap pixmap )
{
    struct client_surface_compositor_frame *frame;
    struct rb_entry *entry;

    if (!(entry = rb_get( &client_surface_compositor_pixmaps, &pixmap ))) return TRUE;
    frame = CONTAINING_RECORD( entry, struct client_surface_compositor_frame, pixmap_entry );
    return !frame->serial;
}

static BOOL submit_client_surface_present( struct client_surface_compositor_target *target,
                                           struct client_surface_compositor_frame *frame,
                                           UINT64 publish_generation, UINT64 publish_epoch,
                                           uint32_t *serial_ret )
{
    Display *display = client_surface_compositor_display;
    struct client_surface_scene scene;
    RECT full = {0, 0, target->width, target->height};
    BOOL copy;
    uint32_t serial;
    int error = 0;

    if (!usexpresent || !target->present_event || frame->serial) return FALSE;
    /* A staged window withholds pixels until this publication completes.
     * Waiting for its host frame callback can therefore wait on exposure
     * itself. Copy into the withheld drawable and check that native write;
     * visible scene transactions retain Present's normal completion. */
    copy = publish_generation && client_surface_get_toplevel_scene( target->toplevel, &scene ) &&
           scene.toplevel == target->toplevel && scene.epoch == publish_epoch &&
           scene.generation == publish_generation && scene.mode == CLIENT_SURFACE_PRESENTATION_STAGED;
    if (copy && !client_surface_xcb_available( display )) return FALSE;
    if (!(serial = ++client_surface_present_serial)) serial = ++client_surface_present_serial;

    frame->serial = serial;
    frame->last_complete_serial = 0;
    frame->last_complete_success = FALSE;
    frame->complete = frame->idle = FALSE;
    frame->publish_generation = publish_generation;
    frame->publish_epoch = publish_epoch;
    frame->publish_pending = !!publish_generation;
    frame->request_op = copy ? OUTPUT_REQUEST_COPY : OUTPUT_REQUEST_PRESENT;
    if (copy)
        frame->request_pending = client_surface_xcb_copy( display, frame->pixmap, target->window,
            &frame->xcb_gc, 0, &full, &full, &full, NULL, 1, FALSE, &frame->request, TRUE );
    else
        frame->request_pending = client_surface_xcb_present( display, target->window, frame->pixmap,
                                                            serial, &frame->request );
    if (copy && !frame->request_pending)
    {
        frame->serial = 0;
        frame->publish_pending = FALSE;
        return FALSE;
    }
    if (frame->request_pending) queue_client_surface_compositor_reply( target, frame, NULL );
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
    if (copy)
        TRACE_(csperf)( "ticks=%llu event=publish_copy_submit window=%lx pixmap=%lx serial=%u "
                       "generation=%llu epoch=%llu cookie=%u barrier=%u\n", client_surface_perf_time(),
                       target->window, frame->pixmap, serial, publish_generation, publish_epoch,
                       frame->request.cookies[frame->request.count - 1], frame->request.barrier );
    else
        TRACE_(csperf)( "ticks=%llu event=present window=%lx pixmap=%lx serial=%u\n",
                       client_surface_perf_time(), target->window, frame->pixmap, serial );
    TRACE( "queued %s serial %u pixmap %#lx generation %s\n", copy ? "staged copy" : "X Present",
           serial, frame->pixmap, wine_dbgstr_longlong( publish_generation ) );
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

static void process_client_surface_present_events(void)
{
    Display *display = client_surface_compositor_display;
    unsigned int budget = 128;
    XEvent event;

    if (!display) return;
    while (budget-- && XPending( display )) XNextEvent( display, &event );
}

#endif

static void release_client_surface_compositor_mailbox( struct client_surface_compositor_mailbox *mailbox )
{
    client_surface_cache_release( mailbox->image );
    client_surface_free_owned_metadata( &mailbox->memory, mailbox, sizeof(*mailbox) );
}

static void retry_client_surface_compositor_mailbox( struct client_surface_compositor_target *target )
{
    struct client_surface_compositor_mailbox *mailbox = target->mailbox;

    /* A failed native allocation is retried after new source/scene input,
     * not on its own completion wake or another target's maintenance. */
    if (!mailbox || mailbox->pending || mailbox->image) return;
    target->mailbox = NULL;
    release_client_surface_compositor_mailbox( mailbox );
}

static void complete_client_surface_compositor_mailbox( void *context, BOOL success )
{
    struct client_surface_compositor_mailbox *mailbox = context;
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( mailbox->toplevel );
    BOOL current = target && target->mailbox == mailbox;

    assert( mailbox->pending );
    mailbox->pending = FALSE;
    TRACE_(csperf)( "ticks=%llu event=output_mailbox_complete mailbox=%p image=%p window=%lx success=%u current=%u\n",
                   client_surface_perf_time(), mailbox, mailbox->image, mailbox->window, success, current );
    /* The allocation object survives target removal and pool replacement.
     * An HWND or target address can be reused, but this live object cannot. */
    if (!current)
    {
        release_client_surface_compositor_mailbox( mailbox );
        return;
    }
    if (!success)
    {
        client_surface_cache_release( mailbox->image );
        mailbox->image = NULL;
        return;
    }
    assert( target->window == mailbox->window && target->width == mailbox->width &&
            target->height == mailbox->height && target->depth == mailbox->depth );
    set_client_surface_compositor_pixmap( &target->frames[2], client_surface_cache_pixmap( mailbox->image ), mailbox->image );
    target->next_frame = 0;
    target->replay_member = 0;
}

static struct client_surface_compositor_frame *alloc_client_surface_compositor_mailbox(
    struct client_surface_compositor_target *target )
{
    struct client_surface_compositor_mailbox *mailbox;
    struct client_surface_memory_scope memory = {0};
    UINT64 bytes;

    /* Keep both completed checkpoints. The empty third slot owns a pending
     * allocation, not permission to delay the actor on native storage work. */
    if (target->mailbox || !target->frames[0].pixmap || !target->frames[1].pixmap) return NULL;
    client_surface_memory_scope_copy( &memory, &target->memory, TRUE );
    if (!(mailbox = client_surface_alloc_scoped_metadata( &memory, 1, sizeof(*mailbox) )))
    {
        client_surface_memory_scope_destroy( &memory );
        return NULL;
    }
    mailbox->memory = memory;
    mailbox->toplevel = target->toplevel;
    mailbox->window = target->window;
    mailbox->width = target->width;
    mailbox->height = target->height;
    mailbox->depth = target->depth;
    bytes = client_surface_pixmap_bytes( target->width, target->height, target->depth );
    mailbox->image = client_surface_cache_create_output( &mailbox->memory, target->window,
        target->width, target->height, target->depth, bytes,
        wake_client_surface_compositor, complete_client_surface_compositor_mailbox, mailbox );
    if (!mailbox->image)
    {
        release_client_surface_compositor_mailbox( mailbox );
        return NULL;
    }
    mailbox->pending = TRUE;
    target->mailbox = mailbox;
    TRACE_(csperf)( "ticks=%llu event=output_mailbox_pending mailbox=%p image=%p window=%lx width=%u height=%u depth=%u\n",
                   client_surface_perf_time(), mailbox, mailbox->image, mailbox->window,
                   mailbox->width, mailbox->height, mailbox->depth );
    return NULL;
}

static void free_client_surface_compositor_mailbox( struct client_surface_compositor_target *target )
{
    struct client_surface_compositor_frame *frame = &target->frames[2];
    struct client_surface_compositor_mailbox *mailbox = target->mailbox;

    if (!mailbox) return;
    assert( !frame->serial && !frame->copy_binding );
    target->mailbox = NULL;
    set_client_surface_compositor_pixmap( frame, 0, NULL );
    TRACE_(csperf)( "ticks=%llu event=output_mailbox_detach mailbox=%p image=%p window=%lx pending=%u\n",
                   client_surface_perf_time(), mailbox, mailbox->image, mailbox->window, mailbox->pending );
    if (!mailbox->pending) release_client_surface_compositor_mailbox( mailbox );
}

static struct client_surface_compositor_frame *get_client_surface_compositor_frame(
    struct client_surface_compositor_target *target )
{
    unsigned int i;

    process_client_surface_present_events();
    /* A reserved scene's complete image owns its publication ticket until
     * submission. New source images remain in the independent owner cache. */
    if (target->mailbox_pending && target->mailbox_publish_generation) return NULL;
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
    {
        unsigned int index = (target->next_frame + i) % ARRAY_SIZE(target->frames);

        /* Neither a failed copy nor a rejected publication may damage the
         * native published image or the complete catchup checkpoint. When
         * all three images are owned, keep coalescing in the source caches;
         * the existing two Present credits and mailbox still make progress. */
        if (!target->frames[index].pixmap || target->frames[index].serial ||
            target->frames[index].pixmap == target->latest ||
            target->frames[index].pixmap == target->published ||
            (target->mailbox_pending && index == target->mailbox_frame) ||
            (target->assembly_pending && index == target->assembly_frame) ||
            !client_surface_compositor_frame_writable( &target->frames[index] )) continue;
        target->next_frame = (index + 1) % ARRAY_SIZE(target->frames);
        return &target->frames[index];
    }
    return alloc_client_surface_compositor_mailbox( target );
}

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
    TRACE_(csperf)( "ticks=%llu event=actor_signal fd=%d result=%d\n",
                   client_surface_perf_time(), client_surface_compositor_notify[1], ret );
}

static void client_surface_handoff_wake_release( struct client_surface_handoff_shared *shared )
{
    if (!__atomic_exchange_n( &shared->release_parked, 0, __ATOMIC_ACQ_REL )) return;
    __atomic_add_fetch( &shared->release_sequence, 1, __ATOMIC_RELEASE );
    client_surface_handoff_futex_wake( &shared->release_sequence );
    TRACE_(csperf)( "ticks=%llu event=release_signal mapping=%s\n",
                   client_surface_perf_time(), wine_dbgstr_longlong( shared->mapping_id ) );
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

static void invalidate_client_surface_compositor_assembly( struct client_surface_compositor_target *target )
{
    unsigned int i;

    if (!target->assembly_pending) return;
    /* Earlier chunks only proved regions of this private image. Its next
     * catchup may replace them, so replay all members after detaching even
     * when this scene's epoch survives the cancelled transaction. */
    for (i = 0; i < target->scene.count; ++i)
    {
        struct client_surface_compositor_binding *binding = target->scene.members[i];

        if (binding->source_epoch == target->assembly_epoch)
            binding->source_epoch = binding->source_sequence = 0;
        binding->replay_epoch = 0;
    }
    target->replay_member = 0;
    finish_client_surface_compositor_assembly( target, TRUE );
}

static void abort_client_surface_output_transform_assembly( struct client_surface_compositor_target *target,
                                                            struct client_surface_output_transform *transform )
{
    if (!transform->generation || target->transform != transform || !target->assembly_pending ||
        target->assembly_generation != transform->generation || target->assembly_epoch != transform->epoch ||
        target->scene.epoch != transform->epoch ||
        target->frames[target->assembly_frame].image != transform->image) return;
    invalidate_client_surface_compositor_assembly( target );
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

        /* This nonzero-generation assembly must also preserve the GUI's
         * prepared backing until its publication has been acknowledged. */
        if (!frame->pixmap || frame->serial ||
            frame->pixmap == target->latest || frame->pixmap == target->published ||
            frame->pixmap == target->backing ||
            (target->mailbox_pending && index == target->mailbox_frame) ||
            !client_surface_compositor_frame_writable( frame ))
            continue;
        target->next_frame = (index + 1) % ARRAY_SIZE(target->frames);
        return frame;
    }
    return alloc_client_surface_compositor_mailbox( target );
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
    struct rb_entry *entry;

    if (!(entry = rb_get( &client_surface_compositor_target_registry, toplevel ))) return NULL;
    return CONTAINING_RECORD( entry, struct client_surface_compositor_target, registry_entry );
}

static BOOL client_surface_present_on_compositor( struct client_surface_compositor_job *job )
{
#ifdef SONAME_LIBXPRESENT
    struct client_surface_compositor_target *target =
        find_client_surface_compositor_target( job->toplevel );
    struct client_surface_compositor_frame *frame;

    if (!usexpresent || !target || target->window != job->u.present.destination ||
        !(frame = acquire_client_surface_compositor_frame( target, job->u.present.source )))
        return FALSE;
    frame->width = job->u.present.width;
    frame->height = job->u.present.height;
    /* This owner scene snapshot includes GDI pixels written outside the
     * compositor connection.  Record a complete checkpoint for later partial
     * handoffs into other pool entries. */
    note_client_surface_compositor_snapshot( target, job->u.present.source );
    if (!submit_client_surface_present( target, frame, 0, 0, NULL ))
        return FALSE;
    frame->waiter = job;
    job->u.present.started = TRUE;
    job->u.present.start = NtGetTickCount();
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
        client_surface_free_owned_metadata( &pool->memory, pool, sizeof(*pool) );
        return;
    }
    assert( 0 );
}

static void free_client_surface_cached_image( struct client_surface_cached_image *image )
{
    client_surface_cache_release( image->storage );
    memset( image, 0, sizeof(*image) );
}

static void free_client_surface_compositor_binding( struct client_surface_compositor_binding *binding )
{
    assert( binding->retired && !client_surface_cache_read_pending( binding ) );
    /* Releasing the consumer endpoint also permits the producer to retire
     * unacknowledged slots. Keep it, the mapping and both cache images until
     * our last native read and checked reply have completed. */
    release_client_surface_compositor_binding_server( binding );
    release_client_surface_compositor_pool( binding->pool );
    free_client_surface_cached_image( &binding->latest_image );
    free_client_surface_cached_image( &binding->spare_image );
    client_surface_free_owned_metadata( &binding->memory, binding, sizeof(*binding) );
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
    binding->pool->query_wait_bitmap[index / 64] &= ~((UINT64)1 << (index % 64));
    if (target)
    {
        detach_client_surface_output_transform( target );
        finish_client_surface_compositor_assembly( target, TRUE );
        target->scene.valid = FALSE;
    }
    /* Acknowledged descriptors refer to this binding's cached images. Once
     * the cache is discarded, a new consumer needs a new channel and frame;
     * it cannot resume at the old consumer sequence without those images. */
    __atomic_store_n( &binding->channel->closed, 1, __ATOMIC_RELEASE );
    binding->retired = TRUE;
    TRACE_(csperf)( "ticks=%llu event=cache_detach identity=%s cookie=%s pending=%u query_pending=%u native_pending=%u token=%s barrier=%u "
                   "consumed=%s produced=%s endpoints=%u\n", client_surface_perf_time(),
                   wine_dbgstr_longlong( binding->identity ), wine_dbgstr_longlong( binding->cookie ),
                   !!binding->cache_copy.reply.requests, binding->cache_copy.query_pending,
                   binding->cache_copy.native_pending,
                   wine_dbgstr_longlong( binding->cache_copy.control ),
                   binding->cache_copy.request.barrier,
                   wine_dbgstr_longlong( __atomic_load_n( &binding->channel->consumer_sequence, __ATOMIC_ACQUIRE ) ),
                   wine_dbgstr_longlong( __atomic_load_n( &binding->channel->producer_sequence, __ATOMIC_ACQUIRE ) ),
                   __atomic_load_n( &binding->channel->endpoints, __ATOMIC_ACQUIRE ) );
    if (!client_surface_cache_read_pending( binding )) free_client_surface_compositor_binding( binding );
}

static struct client_surface_compositor_pool *acquire_client_surface_compositor_pool(
    struct client_surface_compositor_job *job )
{
    struct client_surface_handoff_shared *shared;
    struct client_surface_compositor_pool *pool;
    struct client_surface_memory_scope memory = {0};
    SIZE_T size = job->u.registration.view_size;
    void *view = NULL;

    if ((pool = find_client_surface_compositor_pool( job->u.registration.mapping_id )))
    {
        size = pool->size;
        view = pool->shared;
    }
    else
    {
        /* The job's caller keeps its section handle until this synchronous job
         * returns. Only the first binding maps a pool; later registrations use
         * this connection's retained view, including across scene changes. */
        if (NtMapViewOfSection( job->u.registration.mapping, NtCurrentProcess(), &view, 0, 0, NULL,
                               &size, ViewShare, 0, PAGE_READWRITE )) return NULL;
    }
    shared = view;

    if (size < sizeof(*shared) ||
        __atomic_load_n( &shared->magic, __ATOMIC_ACQUIRE ) != CLIENT_SURFACE_HANDOFF_MAGIC ||
        shared->version != CLIENT_SURFACE_HANDOFF_VERSION ||
        shared->channel_count != CLIENT_SURFACE_HANDOFF_CHANNELS ||
        shared->mapping_id != job->u.registration.mapping_id)
        goto failed;
    if (pool)
    {
        ++pool->refs;
        return pool;
    }
    /* A shared mapping can serve several owners on this connection. */
    if (!(pool = alloc_client_surface_compositor_metadata( NULL, sizeof(*pool), &memory ))) goto failed;
    pool->memory = memory;
    pool->next = client_surface_compositor_pools;
    pool->shared = shared;
    pool->id = job->u.registration.mapping_id;
    pool->size = size;
    pool->refs = 1;
    pool->ready_fd = job->u.registration.ready_fd;
    job->u.registration.ready_fd = -1;
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
    struct client_surface_memory_scope memory = {0};
    struct client_surface_compositor_pool *pool;
    struct client_surface_handoff_channel *channel;
    BOOL ret = FALSE;

    if (!(pool = acquire_client_surface_compositor_pool( job ))) goto done;
    if (job->u.registration.offset < offsetof(struct client_surface_handoff_shared, channels) ||
        job->u.registration.offset > sizeof(*pool->shared) - sizeof(*channel) ||
        (job->u.registration.offset - offsetof(struct client_surface_handoff_shared, channels)) % sizeof(*channel)) goto done;
    channel = (struct client_surface_handoff_channel *)((char *)pool->shared + job->u.registration.offset);
    if (channel->cookie != job->u.registration.cookie || channel->identity != job->u.registration.identity ||
        channel->producer_process != job->u.registration.process ||
        channel->window != wine_server_user_handle( job->u.registration.window ) ||
        channel->toplevel != wine_server_user_handle( job->toplevel ) ||
        __atomic_load_n( &channel->closed, __ATOMIC_ACQUIRE )) goto done;

    for (cursor = &client_surface_compositor_bindings; *cursor; cursor = &(*cursor)->next)
    {
        binding = *cursor;
        if (binding->toplevel != job->toplevel ||
            binding->process != job->u.registration.process || binding->identity != job->u.registration.identity)
            continue;
        if (binding->cookie == job->u.registration.cookie)
        {
            binding->mark = job->u.registration.mark;
            ret = TRUE;
            goto done;
        }
        /* Keep the acquired pool alive if this was its last old binding. */
        remove_client_surface_compositor_binding( cursor );
        break;
    }

    if (!(binding = alloc_client_surface_compositor_metadata( job->toplevel, sizeof(*binding), &memory ))) goto done;
    binding->memory = memory;
    binding->next = client_surface_compositor_bindings;
    binding->pool = pool;
    binding->channel = channel;
    pool->bindings[channel - pool->shared->channels] = binding;
    binding->toplevel = job->toplevel;
    binding->window = job->u.registration.window;
    binding->process = job->u.registration.process;
    binding->identity = job->u.registration.identity;
    binding->cookie = job->u.registration.cookie;
    binding->mark = job->u.registration.mark;
    pool->refs++;
    client_surface_compositor_bindings = binding;
    TRACE( "registered handoff hwnd %p identity %s producer %04x pool %s cookie %s\n",
           binding->window, wine_dbgstr_longlong( binding->identity ), binding->process,
           wine_dbgstr_longlong( pool->id ), wine_dbgstr_longlong( binding->cookie ) );
    ret = TRUE;

done:
    if (pool) release_client_surface_compositor_pool( pool );
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

    for (i = 0; i < job->u.reuse.count; ++i) job->u.reuse.reused[i] = FALSE;
    for (binding = client_surface_compositor_bindings; binding; binding = binding->next)
        if (binding->toplevel == job->toplevel) ++count;
    if (!count) return TRUE;
    if (!(members = client_surface_alloc_owned_array( &job->queue->memory, count, sizeof(*members) ))) return FALSE;
    i = 0;
    for (binding = client_surface_compositor_bindings; binding; binding = binding->next)
        if (binding->toplevel == job->toplevel) members[i++] = binding;
    qsort( members, count, sizeof(*members), compare_client_surface_scene_members );

    for (i = 0; i < job->u.reuse.count; ++i)
    {
        const struct client_surface_handoff_desc *desc = &job->u.reuse.handoffs[i];
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
            if ((job->u.reuse.reused[i] = client_surface_compositor_binding_is_live( binding )))
                binding->mark = job->u.reuse.mark;
            break;
        }
    }
    client_surface_free_owned_array( members );
    return TRUE;
}

static void free_client_surface_scene_layouts( struct client_surface_scene_layout *layouts,
                                               unsigned int count )
{
    unsigned int i;

    for (i = 0; i < count; ++i) client_surface_free_owned_array( layouts[i].clip );
    client_surface_free_owned_array( layouts );
}

static void free_client_surface_scene_plan( struct client_surface_compositor_target *target )
{
    detach_client_surface_output_transform( target );
    client_surface_free_owned_array( target->receipts );
    client_surface_free_owned_array( target->scene.members );
    free_client_surface_scene_layouts( target->scene.layouts, target->scene.count );
    target->receipts = NULL;
    target->scene.members = NULL;
    target->scene.layouts = NULL;
    target->scene.count = 0;
    target->scene.valid = FALSE;
}

static BOOL check_client_surface_compositor_scene( const struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_target *target =
        find_client_surface_compositor_target( job->toplevel );
    struct client_surface_compositor_binding *binding;
    unsigned int i, count = 0, found = 0, needed = 0, bound = 0;

    for (i = 0; i < job->u.scene_check.count; ++i)
    {
        count += !!job->u.scene_check.handoffs[i].visible;
        needed += !!(job->u.scene_check.handoffs[i].visible || job->u.scene_check.handoffs[i].producer_mapped);
    }
    if (!target || !target->scene.valid || target->scene.strategy != OWNER_COMPOSITE ||
        target->scene.epoch != job->u.scene_check.epoch ||
        target->scene.count != count) return FALSE;
    for (binding = client_surface_compositor_bindings; binding; binding = binding->next)
    {
        const struct client_surface_handoff_desc *desc;
        unsigned int low = 0, high = job->u.scene_check.count;

        if (binding->toplevel != job->toplevel) continue;
        /* The caller sorted this authoritative roster. Validate retained
         * hidden bindings too; a changed producer or retired cookie must not
         * survive merely because the visible scene is unchanged. */
        while (low < high)
        {
            unsigned int mid = low + (high - low) / 2;

            if (job->u.scene_check.handoffs[mid].handle < wine_server_user_handle( binding->window )) low = mid + 1;
            else high = mid;
        }
        if (low == job->u.scene_check.count) return FALSE;
        desc = &job->u.scene_check.handoffs[low];
        if (wine_server_user_handle( binding->window ) != desc->handle ||
            binding->process != desc->process || binding->identity != desc->surface ||
            binding->cookie != desc->cookie || !client_surface_compositor_binding_is_live( binding )) return FALSE;
        bound += !!(desc->visible || desc->producer_mapped);
        if (!desc->visible) continue;
        if (binding->scene_index >= target->scene.count ||
            target->scene.members[binding->scene_index] != binding) return FALSE;
        ++found;
    }
    return found == count && bound == needed;
}

static BOOL install_client_surface_scene_plan( struct client_surface_compositor_target *target,
                                               struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_binding *binding, **members = NULL;
    struct client_surface_handoff_receipt *receipts = NULL;
    unsigned int count = job->u.scene_install.count, available = 0, i = 0, next = 0;

    for (binding = client_surface_compositor_bindings; binding; binding = binding->next)
        if (binding->toplevel == target->toplevel) ++available;
    if (count > available) return FALSE;
    if (count)
    {
        if (!(members = client_surface_alloc_owned_array( &target->memory, available, sizeof(*members) ))) return FALSE;
        if (!(receipts = client_surface_alloc_owned_array( &target->memory, count, sizeof(*receipts) )))
        {
            client_surface_free_owned_array( members );
            return FALSE;
        }
        for (binding = client_surface_compositor_bindings; binding; binding = binding->next)
            if (binding->toplevel == target->toplevel) members[i++] = binding;
        qsort( members, available, sizeof(*members), compare_client_surface_scene_members );
        qsort( job->u.scene_install.layouts, count, sizeof(*job->u.scene_install.layouts), compare_client_surface_scene_layouts );
        for (i = 0; i < count; ++i)
        {
            /* The binding cache includes hidden producers. Select only the
             * visible scene's layers from this sorted list; visibility does
             * not discard their last completed images. */
            while (next < available && wine_server_user_handle( members[next]->window ) <
                                       wine_server_user_handle( job->u.scene_install.layouts[i].window )) ++next;
            if (next == available || members[next]->window != job->u.scene_install.layouts[i].window ||
                members[next]->process != job->u.scene_install.layouts[i].process ||
                members[next]->identity != job->u.scene_install.layouts[i].identity)
            {
                client_surface_free_owned_array( members );
                client_surface_free_owned_array( receipts );
                return FALSE;
            }
            members[i] = members[next++];
        }
    }
    if (target->scene.valid && target->scene.epoch == job->u.scene_install.epoch && target->scene.count == count &&
        (!count || !memcmp( members, target->scene.members, count * sizeof(*members) )))
    {
        client_surface_free_owned_array( receipts );
        client_surface_free_owned_array( members );
        return TRUE;
    }
    finish_client_surface_compositor_assembly( target, TRUE );
    update_client_surface_compositor_scene( target, job->u.scene_install.epoch );
    free_client_surface_scene_plan( target );
    target->scene.members = members;
    target->scene.strategy = OWNER_COMPOSITE;
    target->scene.direct_identity = 0;
    target->scene.direct_drawable = None;
    target->scene.layouts = job->u.scene_install.layouts;
    job->u.scene_install.layouts = NULL;
    job->u.scene_install.count = 0;
    target->scene.count = count;
    target->scene.valid = TRUE;
    update_client_surface_notification_plan( target );
    target->quiescing = target->native_updates || target->deferred_update;
    target->receipts = receipts;
    target->replay_member = 0;
    retry_client_surface_compositor_mailbox( target );
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
#else
    (void)target;
#endif
}

static void free_client_surface_compositor_present_input( struct client_surface_compositor_target *target )
{
#ifdef SONAME_LIBXPRESENT
    if (target->present_event)
    {
        int error = 0;

        rb_remove( &client_surface_compositor_present_registry, &target->present_entry );
        /* The GUI can destroy the native window before its asynchronous
         * target removal reaches the actor. The server already discarded
         * that window's event selection; check the late unregistration too. */
        X11DRV_expect_error( client_surface_compositor_display, client_surface_compositor_error, &error );
        pXPresentFreeInput( client_surface_compositor_display, target->window,
                            target->present_event );
        XSync( client_surface_compositor_display, False );
        X11DRV_check_error();
        TRACE_(csperf)( "ticks=%llu event=present_input_free window=%lx event_id=%lx error=%d\n",
                       client_surface_perf_time(), target->window, target->present_event, error );
        if (error && error != BadWindow) WARN( "failed to release Present input for window %#lx, error %d\n",
                                              target->window, error );
        target->present_event = 0;
    }
#else
    (void)target;
#endif
}

static void quiesce_client_surface_compositor_target( struct client_surface_compositor_target *target );

static BOOL update_client_surface_compositor_target( struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_target *target;
    struct client_surface_output_allocation *allocation = NULL;
    BOOL same_pool, checkpoint, created = FALSE;

    if (!(target = find_client_surface_compositor_target( job->toplevel )))
    {
        if (!(target = alloc_client_surface_compositor_target( job->toplevel ))) return FALSE;
        created = TRUE;
    }
    same_pool = target->frames[0].pixmap && target->frames[1].pixmap &&
                ((target->frames[0].pixmap == job->u.pool.pixmaps[0] &&
                 target->frames[1].pixmap == job->u.pool.pixmaps[1]) ||
                 (target->frames[0].pixmap == job->u.pool.pixmaps[1] &&
                 target->frames[1].pixmap == job->u.pool.pixmaps[0]));
    if (!same_pool)
    {
        /* Only a checked checkpoint pair can replace the old pool. A failed
         * checkpoint or target allocation leaves it available for cleanup;
         * composition owns any third image separately after installation. */
        for (allocation = client_surface_output_allocations; allocation; allocation = allocation->next)
            if ((allocation->pixmaps[0] == job->u.pool.pixmaps[0] && allocation->pixmaps[1] == job->u.pool.pixmaps[1]) ||
                (allocation->pixmaps[0] == job->u.pool.pixmaps[1] && allocation->pixmaps[1] == job->u.pool.pixmaps[0])) break;
        if (!allocation) goto failed;
    }
    if (created) register_client_surface_compositor_target( target );
    if (target->window != job->u.pool.destination || target->window_width != job->u.pool.window_width ||
        target->window_height != job->u.pool.window_height || !same_pool)
        quiesce_client_surface_compositor_target( target );
    checkpoint = !same_pool || target->backing != job->u.pool.pixmaps[0];
    if (target->window != job->u.pool.destination || target->window_width != job->u.pool.window_width ||
        target->window_height != job->u.pool.window_height || target->depth != job->u.pool.depth || target->visual != job->u.pool.visual)
    {
        target->scene.valid = FALSE;
        SetRectEmpty( &target->restore_rect );
    }
    if (target->assembly_pending &&
        (target->window != job->u.pool.destination || target->backing != job->u.pool.pixmaps[0] ||
         target->window_width != job->u.pool.window_width || target->window_height != job->u.pool.window_height ||
         target->depth != job->u.pool.depth || target->visual != job->u.pool.visual))
        finish_client_surface_compositor_assembly( target, TRUE );
    if ((target->window && target->window != job->u.pool.destination) ||
        (target->frames[0].pixmap && !same_pool))
        drain_client_surface_compositor_target( target );
    /* Selection belongs to the native window, not its replaceable images.
     * Keep it across pool replacement and an acknowledged DIRECT plan. */
    if (target->window && target->window != job->u.pool.destination)
        free_client_surface_compositor_present_input( target );
    if (target->mailbox && target->mailbox->pending &&
        (target->window != job->u.pool.destination || target->width != job->u.pool.width ||
         target->height != job->u.pool.height || target->depth != job->u.pool.depth))
        free_client_surface_compositor_mailbox( target );
    retry_client_surface_compositor_mailbox( target );
    target->window = job->u.pool.destination;
    if (!same_pool)
    {
        unsigned int i;

        free_client_surface_compositor_mailbox( target );
        for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        {
            client_surface_xcb_free_gc( client_surface_compositor_display, &target->frames[i].xcb_gc );
            if (target->frames[i].gc) XFreeGC( client_surface_compositor_display, target->frames[i].gc );
            if (i < 2) set_client_surface_compositor_pixmap( &target->frames[i], 0, NULL );
        }
        memset( target->frames, 0, sizeof(target->frames) );
        for (i = 0; i < 2; ++i)
            set_client_surface_compositor_pixmap( &target->frames[i], job->u.pool.pixmaps[i],
                allocation->images[allocation->pixmaps[0] != job->u.pool.pixmaps[i]] );
        target->published = job->u.pool.pixmaps[0];
        target->published_width = job->u.pool.valid_width;
        target->published_height = job->u.pool.valid_height;
        target->next_frame = 0;
        target->mailbox_pending = FALSE;
    }
    target->backing = job->u.pool.pixmaps[0];
    target->shrink_start = job->u.pool.shrink_start;
    target->width = job->u.pool.width;
    target->height = job->u.pool.height;
    target->window_width = job->u.pool.window_width;
    target->window_height = job->u.pool.window_height;
    target->depth = job->u.pool.depth;
    target->visual = job->u.pool.visual;
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
        if (target->present_event)
        {
            assert( !rb_get( &client_surface_compositor_present_registry, &target->present_event ) );
            rb_put( &client_surface_compositor_present_registry, &target->present_event, &target->present_entry );
            TRACE_(csperf)( "ticks=%llu event=present_input_register window=%lx event_id=%lx\n",
                           client_surface_perf_time(), target->window, target->present_event );
        }
    }
#endif
    return TRUE;

failed:
    if (created) free_client_surface_compositor_target( target );
    else target->quiescing = target->native_updates || target->deferred_update;
    return FALSE;
}

static BOOL client_surface_output_checkpoint_scene_current( const struct client_surface_scene *expected )
{
    struct client_surface_scene current;

    return client_surface_capture_scene_state( expected->toplevel, &current ) &&
           current.toplevel == expected->toplevel && current.epoch == expected->epoch &&
           current.generation == expected->generation && current.mode == expected->mode;
}

static struct client_surface_compositor_frame *client_surface_output_checkpoint_frame(
    struct client_surface_compositor_target *target, Pixmap source, BOOL *busy )
{
    unsigned int i;

    *busy = FALSE;
    if (!target || target->backing != source) return NULL;
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
    {
        struct client_surface_compositor_frame *frame = &target->frames[i];

        if (frame->pixmap != source || !frame->image) continue;
        /* A partial assembly is not a completed checkpoint, even before its
         * next native request is queued. Admission never waits for it. */
        if (target->copy_frame == frame || client_surface_cache_write_pending( frame->image ) ||
            (target->assembly_pending && target->assembly_frame == i) ||
            (client_surface_copy_batch.count && client_surface_copy_batch.frame == frame))
        {
            *busy = TRUE;
            return NULL;
        }
        if (!frame->revision) return NULL;
        return frame;
    }
    return NULL;
}

static BOOL copy_client_surface_compositor_pool( struct client_surface_compositor_job *job )
{
    struct client_surface_output_allocation *allocation = job->u.pool.allocation;
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( job->toplevel );
    struct client_surface_compositor_frame *frame;
    BOOL busy;
    unsigned int i;

    assert( allocation && !allocation->checkpoint && !allocation->pending && !allocation->failed );
    frame = client_surface_output_checkpoint_frame( target, job->u.pool.source, &busy );
    if (!target || !allocation->window_owner || target->window != job->u.pool.destination ||
        target->backing != job->u.pool.source ||
        target->depth != job->u.pool.depth || target->visual != job->u.pool.visual ||
        !job->u.pool.window_width || !job->u.pool.window_height ||
        job->u.pool.preserve_width < job->u.pool.window_width ||
        job->u.pool.preserve_height < job->u.pool.window_height ||
        job->u.pool.preserve_width > min( target->width, allocation->width ) ||
        job->u.pool.preserve_height > min( target->height, allocation->height ) ||
        !client_surface_output_checkpoint_scene_current( &job->u.pool.scene ))
    {
        job->u.pool.stale = TRUE;
        return FALSE;
    }
    allocation->source = job->u.pool.source;
    allocation->source_revision = target->revision;
    allocation->published = target->published;
    allocation->scene = job->u.pool.scene;
    allocation->window = target->window;
    allocation->window_width = job->u.pool.window_width;
    allocation->window_height = job->u.pool.window_height;
    allocation->copy_width = job->u.pool.preserve_width;
    allocation->copy_height = job->u.pool.preserve_height;
    if (!frame && !busy)
    {
        /* Failed partial writes invalidate the GUI's old validity hint.
         * Require a fresh checkpoint, not an endless source-ready wait. */
        job->u.pool.stale = job->u.pool.invalid_source = TRUE;
        return FALSE;
    }
    if (!frame)
    {
        /* Admission owns this bounded notification, not a stack job parked
         * behind the source's writer. REMOVE can cancel it without a read. */
        pthread_mutex_lock( &client_surface_compositor_mutex );
        allocation->copy_wait = TRUE;
        list_add_tail( &client_surface_output_notifications, &allocation->notification_entry );
        ++client_surface_output_notification_count;
        pthread_mutex_unlock( &client_surface_compositor_mutex );
        TRACE_(csperf)( "ticks=%llu event=output_pair_copy_wait request=%p serial=%llu hwnd=%p source=%lx\n",
                       client_surface_perf_time(), allocation, (unsigned long long)allocation->serial,
                       job->toplevel, allocation->source );
        return TRUE;
    }
    allocation->source_image = client_surface_cache_acquire( frame->image );
    pthread_mutex_lock( &client_surface_compositor_mutex );
    allocation->copy_wait = FALSE;
    allocation->checkpoint = TRUE;
    allocation->pending = ARRAY_SIZE(allocation->images);
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    TRACE_(csperf)( "ticks=%llu event=output_pair_copy_submit request=%p serial=%llu hwnd=%p window=%lx "
                   "source_image=%p source=%lx first=%lx second=%lx width=%u height=%u preserve_width=%u preserve_height=%u "
                   "epoch=%llu generation=%llu revision=%llu\n", client_surface_perf_time(), allocation,
                   (unsigned long long)allocation->serial, job->toplevel, allocation->window,
                   allocation->source_image, allocation->source, allocation->pixmaps[0], allocation->pixmaps[1],
                   allocation->window_width, allocation->window_height, allocation->copy_width, allocation->copy_height,
                   (unsigned long long)allocation->scene.epoch, (unsigned long long)allocation->scene.generation,
                   (unsigned long long)allocation->source_revision );
    for (i = 0; i < ARRAY_SIZE(allocation->images); ++i)
        client_surface_cache_copy_output( allocation->images[i], allocation->source,
            allocation->copy_width, allocation->copy_height, client_surface_output_allocation_complete, allocation );
    return TRUE;
}

static BOOL install_client_surface_output_checkpoint( struct client_surface_compositor_job *job )
{
    struct client_surface_output_allocation *allocation = job->u.pool.allocation;
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( job->toplevel );
    struct client_surface_compositor_frame *frame;
    BOOL current, busy;

    assert( allocation->checkpoint && !allocation->pending && !allocation->failed );
    frame = client_surface_output_checkpoint_frame( target, allocation->source, &busy );
    current = frame && frame->image == allocation->source_image && target->revision == allocation->source_revision &&
              target->published == allocation->published && target->window == allocation->window &&
              target->depth == job->u.pool.depth && target->visual == job->u.pool.visual &&
              allocation->width == job->u.pool.width && allocation->height == job->u.pool.height &&
              job->u.pool.source == allocation->source && job->u.pool.destination == allocation->window &&
              job->u.pool.window_width == allocation->window_width && job->u.pool.window_height == allocation->window_height &&
              job->u.pool.preserve_width == allocation->copy_width && job->u.pool.preserve_height == allocation->copy_height &&
              client_surface_output_checkpoint_scene_current( &allocation->scene );
    TRACE_(csperf)( "ticks=%llu event=output_pair_copy_install request=%p serial=%llu hwnd=%p source=%lx current=%u\n",
                   client_surface_perf_time(), allocation, (unsigned long long)allocation->serial,
                   job->toplevel, allocation->source, current );
    if (!current)
    {
        job->u.pool.stale = TRUE;
        return FALSE;
    }
    memcpy( job->u.pool.pixmaps, allocation->pixmaps, sizeof(job->u.pool.pixmaps) );
    register_client_surface_output_allocation( allocation );
    return TRUE;
}

static BOOL replace_client_surface_compositor_pool( struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( job->toplevel );
    struct client_surface_compositor_frame *source;

    if (!job->u.pool.allocation->checkpoint && target && job->u.pool.source &&
        job->u.pool.preserve_width && job->u.pool.preserve_height &&
        (source = get_client_surface_compositor_pixmap( target, job->u.pool.source )) &&
        (!source->revision || client_surface_cache_write_pending( source->image )))
    {
        /* A detached transform may still be writing the former GUI backing.
         * Do not preserve those uncommitted pixels over a fresh Window seed.
         * Reuse the GUI's stale-hint retry without waiting for that writer. */
        job->u.pool.stale = job->u.pool.invalid_source = TRUE;
        return FALSE;
    }

    /* Completed empty storage becomes an output pool only after checked
     * checkpoint copies and installation. The GUI keeps the old pair until
     * this job succeeds; failure leaves its pending publication intact. */
    if (!client_surface_compositor_open() ||
        !(job->u.pool.allocation->checkpoint ? install_client_surface_output_checkpoint( job ) :
                                              client_surface_alloc_on_compositor( job )))
    {
        free_client_surface_pending_allocation( job->u.pool.allocation );
        job->u.pool.allocation = NULL;
        goto failed;
    }
    job->u.pool.allocation = NULL; /* Registered pool owns the completed storage. */
    if (update_client_surface_compositor_target( job )) return TRUE;
    release_client_surface_output_allocation( job->u.pool.pixmaps );
    job->u.pool.pixmaps[0] = job->u.pool.pixmaps[1] = 0;
failed:
    if ((target = find_client_surface_compositor_target( job->toplevel )))
        target->quiescing = target->native_updates || target->deferred_update;
    return FALSE;
}

static BOOL remove_client_surface_compositor_target( HWND toplevel )
{
    struct client_surface_compositor_target *target;
    unsigned int i;

    sweep_client_surface_compositor_handoffs( toplevel, 0, 0 );
    if (!(target = find_client_surface_compositor_target( toplevel ))) return TRUE;
    drain_client_surface_compositor_target( target );
    free_client_surface_compositor_present_input( target );
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
    {
        client_surface_xcb_free_gc( client_surface_compositor_display, &target->frames[i].xcb_gc );
        if (target->frames[i].gc) XFreeGC( client_surface_compositor_display, target->frames[i].gc );
        if (i < 2) set_client_surface_compositor_pixmap( &target->frames[i], 0, NULL );
    }
    free_client_surface_compositor_mailbox( target );
    rb_remove( &client_surface_compositor_target_registry, &target->registry_entry );
    *target->prev = target->next;
    if (target->next) target->next->prev = target->prev;
    if (client_surface_compositor_next_target == target)
        client_surface_compositor_next_target = target->next ? target->next : client_surface_compositor_targets;
    --client_surface_compositor_target_count;
    ++client_surface_compositor_target_generation;
    free_client_surface_scene_plan( target );
    free_client_surface_compositor_target( target );
    return TRUE;
}

static BOOL client_surface_compositor_pool_retirable( const struct client_surface_compositor_target *target )
{
    struct client_surface_scene scene;

    return target && target->scene.valid && target->scene.strategy == DIRECT_ATTACH &&
           client_surface_get_toplevel_scene( target->toplevel, &scene ) && !scene.generation &&
           scene.epoch == target->scene.epoch && scene.mode == CLIENT_SURFACE_PRESENTATION_DIRECT;
}

/* The logical owner and its immutable plan outlive a retired output pool.
 * Native attachment does not use any of these old copy/Present resources. */
static BOOL retire_client_surface_compositor_pool( HWND toplevel )
{
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( toplevel );
    unsigned int i;

    if (!client_surface_compositor_pool_retirable( target )) return FALSE;
    drain_client_surface_compositor_target( target );
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
    {
        client_surface_xcb_free_gc( client_surface_compositor_display, &target->frames[i].xcb_gc );
        if (target->frames[i].gc) XFreeGC( client_surface_compositor_display, target->frames[i].gc );
        if (i < 2) set_client_surface_compositor_pixmap( &target->frames[i], 0, NULL );
    }
    free_client_surface_compositor_mailbox( target );
    memset( target->frames, 0, sizeof(target->frames) );
    target->backing = target->latest = target->published = None;
    target->published_width = target->published_height = 0;
    SetRectEmpty( &target->restore_rect );
    return TRUE;
}

static BOOL get_client_surface_direct_scene( HWND toplevel, UINT64 epoch, struct client_surface_scene *scene )
{
    return client_surface_get_toplevel_scene( toplevel, scene ) && scene->direct_candidate &&
           scene->epoch == epoch && (!scene->generation || scene->generation == epoch);
}

static BOOL client_surface_direct_plan_current( const struct client_surface_compositor_job *job,
                                                const struct client_surface_compositor_target *target )
{
    struct client_surface_scene current;

    return job->u.direct_plan.source && job->u.direct_plan.destination && (!target || target->window == job->u.direct_plan.destination) &&
           get_client_surface_direct_scene( job->toplevel, job->u.direct_plan.scene_epoch, &current );
}

static BOOL install_client_surface_direct_plan( const struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( job->toplevel );
    struct client_surface_scene current;
    XWindowAttributes attributes;
    UINT64 scene_id = job->u.direct_plan.scene_epoch;
    BOOL accepted = FALSE, allocated = FALSE;
    Status queried;
    int error = 0;

    /* The shared candidate is only an early rejection hint. The prepare and
     * select requests authenticate the sole selected identity, owner process
     * and exact scene before changing the plan. No geometry or clip from a
     * roster snapshot is consumed by native attachment. */
    if (!job->u.direct_plan.source || !job->u.direct_plan.destination ||
        !get_client_surface_direct_scene( job->toplevel, scene_id, &current ))
        goto done;
    /* A managed top-level may still be waiting for the WM to map it. A
     * DIRECT image presented into that unmapped hierarchy can be discarded
     * by the later map. Keep the first frame in the owner cache instead. */
    X11DRV_expect_error( client_surface_compositor_display, client_surface_compositor_error, &error );
    queried = XGetWindowAttributes( client_surface_compositor_display, job->u.direct_plan.destination, &attributes );
    X11DRV_check_error();
    TRACE( "DIRECT native admission hwnd %p window %#lx queried %u map state %d error %d\n",
           job->toplevel, job->u.direct_plan.destination, queried, queried ? attributes.map_state : -1, error );
    if (!queried || error || attributes.map_state != IsViewable) goto done;
    if (!current.generation)
    {
        UINT64 next_scene = 0;

        /* A retained owner plan supplies the native target; only the server's
         * successful ACK checkpoint can authorize skipping geometry prepare.
         * The submitting producer keeps this exact drawable alive throughout. */
        if (!target || !target->scene.valid || target->scene.strategy != OWNER_COMPOSITE ||
            target->scene.epoch != scene_id || target->window != job->u.direct_plan.destination) goto done;
        SERVER_START_REQ( prepare_client_surface_direct_plan )
        {
            req->handle = wine_server_user_handle( job->toplevel );
            req->scene_id = scene_id;
            req->surface = job->u.direct_plan.identity;
            req->previous_scene = 0;
            if (!wine_server_call( req )) next_scene = reply->scene_id;
        }
        SERVER_END_REQ;
        if (!next_scene) goto done;
        TRACE( "owner DIRECT strategy scene hwnd %p old %s new %s identity %s\n",
               job->toplevel, wine_dbgstr_longlong( scene_id ), wine_dbgstr_longlong( next_scene ),
               wine_dbgstr_longlong( job->u.direct_plan.identity ) );
        /* Read the new immutable scene rather than retagging the old one.
         * Failed admission leaves the channels and old plan intact; the
         * restart's ordinary owner wake can compose the new scene. */
        scene_id = next_scene;
        if (!get_client_surface_direct_scene( job->toplevel, scene_id, &current ) ||
            current.generation != scene_id) goto done;
    }
    if (!target)
    {
        if (!(target = alloc_client_surface_compositor_target( job->toplevel ))) goto done;
        target->window = job->u.direct_plan.destination;
        allocated = TRUE;
    }
    if (target->window != job->u.direct_plan.destination) goto done;
    SERVER_START_REQ( select_client_surface_direct_plan )
    {
        req->handle = wine_server_user_handle( job->toplevel );
        req->scene_id = scene_id;
        req->surface = job->u.direct_plan.identity;
        if (!wine_server_call( req )) accepted = reply->accepted;
    }
    SERVER_END_REQ;
    if (!accepted) goto done;
    if (allocated) register_client_surface_compositor_target( target );
    /* Only authenticated admission may cancel the previous assembly. While
     * native reads and Present requests drained, the scheduler paused new
     * work without discarding a newer scene's assembly or mailbox. Keep its
     * output allocation until the host-success publication ACK. */
    quiesce_client_surface_compositor_target( target );
    sweep_client_surface_compositor_handoffs( target->toplevel, 0, NULL );
    update_client_surface_compositor_scene( target, scene_id );
    free_client_surface_scene_plan( target );
    target->scene = (struct client_surface_scene_plan){
        .strategy = DIRECT_ATTACH, .direct_identity = job->u.direct_plan.identity,
        .direct_drawable = job->u.direct_plan.source, .epoch = scene_id, .valid = TRUE,
    };
    update_client_surface_notification_plan( target );
    SetRectEmpty( &target->restore_rect );
    TRACE( "owner DIRECT_ATTACH hwnd %p scene %s identity %s drawable %#lx\n",
           target->toplevel, wine_dbgstr_longlong( scene_id ), wine_dbgstr_longlong( job->u.direct_plan.identity ), job->u.direct_plan.source );
done:
    if (allocated && !accepted) free_client_surface_compositor_target( target );
    return accepted;
}

static BOOL renew_client_surface_direct_plan( const struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( job->toplevel );
    struct client_surface_scene current;
    XWindowAttributes window, drawable;
    Window root, parent, *children = NULL;
    unsigned int count, i;
    UINT64 scene_id = 0;
    BOOL native;
    int error = 0;

    /* Only a previously admitted attachment with its output pool already
     * retired can renew without preserving a composition checkpoint. */
    if (!target || !target->scene.valid || target->scene.strategy != DIRECT_ATTACH ||
        !target->scene.direct_drawable || target->window != job->u.direct_renew.destination ||
        target->copy_frame || target->native_updates || target->deferred_update) return FALSE;
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        if (target->frames[i].pixmap) return FALSE;
    client_surface_get_toplevel_scene( job->toplevel, &current );
    if (current.valid || !current.direct_candidate || current.generation ||
        current.epoch != job->u.direct_renew.scene_epoch || (current.epoch & 1)) return FALSE;

    /* The GUI has completed its native changes. Check the retained child
     * itself, including its parent and exact client extent, rather than
     * treating the old scene's geometry or a DIRECT candidate as that proof. */
    if (client_surface_xcb_available( client_surface_compositor_display ))
    {
        RECT rect = {job->u.direct_renew.source_x, job->u.direct_renew.source_y, job->u.direct_renew.source_x + job->u.direct_renew.width, job->u.direct_renew.source_y + job->u.direct_renew.height};

        if (!client_surface_xcb_check_direct( client_surface_compositor_display, job->u.direct_renew.destination,
                                              target->scene.direct_drawable, job->u.direct_renew.window_width,
                                              job->u.direct_renew.window_height, &rect )) return FALSE;
    }
    else
    {
        X11DRV_expect_error( client_surface_compositor_display, client_surface_compositor_error, &error );
        native = XGetWindowAttributes( client_surface_compositor_display, job->u.direct_renew.destination, &window ) &&
                 XGetWindowAttributes( client_surface_compositor_display, target->scene.direct_drawable, &drawable ) &&
                 XQueryTree( client_surface_compositor_display, target->scene.direct_drawable,
                             &root, &parent, &children, &count );
        if (children) XFree( children );
        X11DRV_check_error();
        if (!native || error || window.map_state != IsViewable || drawable.map_state != IsViewable ||
            parent != job->u.direct_renew.destination || drawable.border_width ||
            window.width != job->u.direct_renew.window_width || window.height != job->u.direct_renew.window_height ||
            drawable.x != job->u.direct_renew.source_x || drawable.y != job->u.direct_renew.source_y ||
            drawable.width != job->u.direct_renew.width || drawable.height != job->u.direct_renew.height) return FALSE;
    }

    SERVER_START_REQ( prepare_client_surface_direct_plan )
    {
        req->handle = wine_server_user_handle( job->toplevel );
        req->scene_id = job->u.direct_renew.scene_epoch;
        req->surface = target->scene.direct_identity;
        req->previous_scene = target->scene.epoch;
        if (!wine_server_call( req )) scene_id = reply->scene_id;
    }
    SERVER_END_REQ;
    if (!scene_id) return FALSE;
    target->scene.epoch = scene_id;
    update_client_surface_notification_plan( target );
    target->window_width = job->u.direct_renew.window_width;
    target->window_height = job->u.direct_renew.window_height;
    SetRectEmpty( &target->restore_rect );
    TRACE( "owner DIRECT_ATTACH hwnd %p scene %s identity %s drawable %#lx renewed=1 size=%ux%u\n",
           target->toplevel, wine_dbgstr_longlong( scene_id ),
           wine_dbgstr_longlong( target->scene.direct_identity ), target->scene.direct_drawable,
           target->window_width, target->window_height );
    return TRUE;
}

static BOOL complete_client_surface_direct_plan( const struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( job->toplevel );
    BOOL accepted = FALSE;

    if (!target || target->notifications != job->notifications || !target->scene.valid || target->scene.strategy != DIRECT_ATTACH ||
        target->scene.epoch != job->u.direct_complete.scene_epoch || target->scene.direct_identity != job->u.direct_complete.identity ||
        target->scene.direct_drawable != job->u.direct_complete.source) return FALSE;
    /* No native pointer is retained or dereferenced by this queued proof.
     * The producer validated its native epoch before enqueue; destruction or
     * a new scene invalidates server admission for that unique surface ID. */
    SERVER_START_REQ( complete_client_surface_direct_plan )
    {
        req->handle = wine_server_user_handle( job->toplevel );
        req->scene_id = job->u.direct_complete.scene_epoch;
        req->surface = job->u.direct_complete.identity;
        if (!wine_server_call( req )) accepted = reply->accepted;
    }
    SERVER_END_REQ;
    TRACE( "owner DIRECT_ATTACH host complete hwnd %p scene %s identity %s target %s accepted %u\n",
           job->toplevel, wine_dbgstr_longlong( job->u.direct_complete.scene_epoch ), wine_dbgstr_longlong( job->u.direct_complete.identity ),
           wine_dbgstr_longlong( job->u.direct_complete.native_epoch ), accepted );
    return accepted && publish_client_surface_handoff_generation( job->toplevel,
                                                                 job->u.direct_complete.scene_epoch, job->u.direct_complete.scene_epoch, TRUE );
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

static BOOL process_client_surface_compositor_restore( struct client_surface_compositor_target *target )
{
    if (IsRectEmpty( &target->restore_rect ) || !client_surface_compositor_restore_ready( target )) return FALSE;
    if (!restore_client_surface_compositor_pixels( target ))
        NtUserPostMessage( target->toplevel, WM_WINE_UPDATEWINDOWSTATE,
                           WINE_UPDATE_CLIENT_SURFACE_HANDOFFS, 0 );
    return TRUE;
}

static BOOL restore_client_surface_compositor_target(
    struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_target *target =
        find_client_surface_compositor_target( job->toplevel );
    RECT rect = {job->u.restore.destination_x, job->u.restore.destination_y,
                 job->u.restore.destination_x + job->u.restore.width, job->u.restore.destination_y + job->u.restore.height};

    process_client_surface_present_events();
    if (!target || !target->window || target->scene.strategy == DIRECT_ATTACH ||
        target->window != job->u.restore.destination ||
        target->window_width != job->u.restore.window_width ||
        target->window_height != job->u.restore.window_height)
        return FALSE;
    job->u.restore.valid_width = target->published_width;
    job->u.restore.valid_height = target->published_height;
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

static BOOL client_surface_source_cache_matches( const struct client_surface_source_cache *cache,
                                                 const struct client_surface_handoff_slot *slot )
{
    return cache->pixmap == slot->source && cache->target_epoch == slot->target_epoch &&
           cache->width == slot->width && cache->height == slot->height && cache->visual == slot->source_visual;
}

static BOOL get_client_surface_compositor_source(
    struct client_surface_compositor_binding *binding,
    const struct client_surface_handoff_slot *slot, Pixmap *source,
    unsigned int *source_depth )
{
    struct client_surface_source_cache *cache;
    unsigned int i;

    if (!(slot->flags & CLIENT_SURFACE_HANDOFF_COPY_SOURCE)) return FALSE;
    /* Cache validated metadata, without owning or retaining the producer's
     * XID. The slot pins this independent image through copy completion, and
     * every actual read still participates in the X error validation boundary. */
    /* Transport slots and native images rotate independently. In particular,
     * three source images through a four-slot ring never match the previous
     * image at the same ring position. Reuse metadata by its exact native
     * tuple within this binding; the ring index only selects a miss victim. */
    for (i = 0; i < ARRAY_SIZE(binding->sources); ++i)
        if (client_surface_source_cache_matches( &binding->sources[i], slot )) break;
    if (i == ARRAY_SIZE(binding->sources)) return FALSE;
    cache = &binding->sources[i];
    *source = cache->pixmap;
    *source_depth = cache->depth;
    return TRUE;
}

static void release_client_surface_cached_source( struct client_surface_compositor_binding *binding,
                                                  BOOL success )
{
    const struct client_surface_cache_copy *copy = &binding->cache_copy;
    struct client_surface_handoff_channel *channel = binding->channel;
    unsigned int index = channel - binding->pool->shared->channels;

    trace_client_surface_source( "cache_copy", binding, copy->control, copy->frame.source_sequence,
                                 0, binding->latest_image.pixmap, success );
    /* Descriptor publication pins the independent producer image. Sending a
     * request is not permission to return it; this runs after the checked
     * read, or after rejecting a descriptor without submitting any read. */
    __atomic_store_n( &channel->consumer_sequence, copy->control, __ATOMIC_RELEASE );
    if (!success && !binding->latest_image.pixmap)
        __atomic_store_n( &channel->closed, 1, __ATOMIC_RELEASE );
    trace_client_surface_source( "cache_release", binding, copy->control, copy->frame.source_sequence,
                                 0, binding->latest_image.pixmap, success );
    TRACE( "%s handoff hwnd %p identity %s sequence %s after owner cache copy\n",
           success || binding->latest_image.pixmap ? "released" : "lost", binding->window,
           wine_dbgstr_longlong( binding->identity ), wine_dbgstr_longlong( copy->control ) );
    client_surface_handoff_wake_release( binding->pool->shared );
    if (binding->retired)
    {
        free_client_surface_compositor_binding( binding );
        return;
    }
    if (success)
    {
        struct client_surface_compositor_target *target = find_client_surface_compositor_target( binding->toplevel );

        binding->replay_epoch = 0;
        /* A new completed source also retries preceding dirty scene members.
         * No target pointer is borrowed across this native copy. */
        if (target)
        {
            target->replay_member = 0;
            retry_client_surface_compositor_mailbox( target );
        }
    }
    /* A producer may have filled the ring while the read was pending. Its
     * hint was consumed then; recheck the authoritative sequence after the
     * completion without requiring another producer notification. */
    __atomic_fetch_or( &binding->pool->shared->ready_bitmap[index / 64],
                       (UINT64)1 << (index % 64), __ATOMIC_RELEASE );
}

static void finish_client_surface_cache_copy( struct client_surface_compositor_binding *binding, BOOL success )
{
    struct client_surface_cache_copy *copy = &binding->cache_copy;

    if (success && !binding->retired)
    {
        struct client_surface_cached_image previous = binding->latest_image;

        binding->latest_image = binding->spare_image;
        binding->spare_image = previous;
        binding->latest_frame = copy->frame;
        binding->latest_frame.source = binding->latest_image.pixmap;
        binding->latest_control = copy->control;
        binding->latest_index = copy->index;
    }
    release_client_surface_cached_source( binding, success && !binding->retired );
}

static void complete_client_surface_cache_read( struct client_surface_compositor_binding *binding,
                                                BOOL success, BOOL async )
{
    struct client_surface_cache_copy *copy = &binding->cache_copy;
    struct client_surface_cached_image *image = &binding->spare_image;
    struct client_surface_compositor_target *target;
    BOOL retired = binding->retired;
    unsigned int budget = 1;

    TRACE_(csperf)( "ticks=%llu event=cache_native_copy identity=%s cookie=%s token=%s sequence=%s "
                   "source=%lx destination=%lx width=%u height=%u depth=%u pixel_bits=%u copied=1 error=%u "
                   "sync_calls=%u async=%u elapsed=%s retired=%u\n",
                   client_surface_perf_time(), wine_dbgstr_longlong( binding->identity ),
                   wine_dbgstr_longlong( binding->cookie ), wine_dbgstr_longlong( copy->control ),
                   wine_dbgstr_longlong( copy->frame.source_sequence ), (Pixmap)copy->frame.source, image->pixmap,
                   image->width, image->height, image->depth,
                   pixmap_formats[image->depth] ? pixmap_formats[image->depth]->bits_per_pixel : 0, !success, !async, async,
                   wine_dbgstr_longlong( client_surface_perf_time() - copy->started ), binding->retired );
    if (!success) free_client_surface_cached_image( image );
    finish_client_surface_cache_copy( binding, success );
    /* Give an accepted cache one bounded output opportunity before another
     * READY can start replacing it. Waiting for a newer read below must not
     * starve output under a continuous producer. Use the scene's replay order,
     * including its assembly obligations, rather than composing out of order. */
    if (success && !retired && (target = find_client_surface_compositor_target( binding->toplevel )))
        replay_client_surface_scene_sources( target, &budget );
}

static void complete_client_surface_cache_reply( struct client_surface_compositor_reply *reply, BOOL success )
{
    struct client_surface_compositor_binding *binding =
        CONTAINING_RECORD( reply, struct client_surface_compositor_binding, cache_copy.reply );

    complete_client_surface_cache_read( binding, success, TRUE );
}

static void start_client_surface_cache_copy( struct client_surface_compositor_binding *binding,
                                             unsigned int depth );

static void complete_client_surface_cache_creation( void *context, BOOL success )
{
    struct client_surface_compositor_binding *binding = context;
    struct client_surface_cached_image *image = &binding->spare_image;

    assert( binding->cache_copy.native_pending );
    binding->cache_copy.native_pending = FALSE;
    TRACE_(csperf)( "ticks=%llu event=cache_image_complete identity=%s cookie=%s token=%s image=%p "
                   "success=%u retired=%u\n", client_surface_perf_time(),
                   wine_dbgstr_longlong( binding->identity ), wine_dbgstr_longlong( binding->cookie ),
                   wine_dbgstr_longlong( binding->cache_copy.control ), image->storage, success, binding->retired );
    if (!success || binding->retired)
    {
        free_client_surface_cached_image( image );
        release_client_surface_cached_source( binding, FALSE );
        return;
    }
    image->pixmap = client_surface_cache_pixmap( image->storage );
    image->xcb_gc = client_surface_cache_gc( image->storage );
    start_client_surface_cache_copy( binding, image->depth );
}

static void complete_client_surface_cache_fallback( void *context, BOOL success )
{
    struct client_surface_compositor_binding *binding = context;
    struct client_surface_cache_copy *copy = &binding->cache_copy;

    assert( copy->native_pending );
    copy->native_pending = FALSE;
    complete_client_surface_cache_read( binding, success, FALSE );
}

static void start_client_surface_cache_copy( struct client_surface_compositor_binding *binding,
                                             unsigned int depth )
{
    struct client_surface_cached_image *image = &binding->spare_image;
    struct client_surface_cache_copy *copy = &binding->cache_copy;
    struct client_surface_handoff_slot frame = copy->frame;
    Display *display = client_surface_compositor_display;
    Pixmap source = frame.source;
    UINT64 control = copy->control;

    assert( !binding->retired && !client_surface_cache_read_pending( binding ) );
    TRACE( "reading handoff hwnd %p identity %s sequence %s into owner cache\n", binding->window,
           wine_dbgstr_longlong( binding->identity ), wine_dbgstr_longlong( control ) );
    /* An old latest image can still be a scene output's source after swapping
     * into the spare. Both cross-connection reclaim and fallback writes must
     * wait for that read; retain its reference and admit a separate candidate. */
    if (!image->storage || client_surface_cache_shared( image->storage ) ||
        image->width != frame.width || image->height != frame.height || image->depth != depth)
    {
        UINT64 bytes = client_surface_pixmap_bytes( frame.width, frame.height, depth );

        free_client_surface_cached_image( image );
        image->storage = client_surface_cache_create( &binding->memory, frame.width, frame.height, depth, bytes,
            wake_client_surface_compositor, complete_client_surface_cache_creation, binding );
        if (!image->storage) goto done;
        image->width = frame.width;
        image->height = frame.height;
        image->depth = depth;
        copy->native_pending = TRUE;
        TRACE_(csperf)( "ticks=%llu event=cache_image_pending identity=%s cookie=%s token=%s image=%p operation=1\n",
                       client_surface_perf_time(), wine_dbgstr_longlong( binding->identity ),
                       wine_dbgstr_longlong( binding->cookie ), wine_dbgstr_longlong( control ), image->storage );
        return;
    }
    /* Keep the last complete cache intact until the new full image and its
     * error check succeed. Neither buffer borrows a producer XID. */
    if (client_surface_xcb_available( display ))
    {
        RECT rect = {0, 0, frame.width, frame.height};
        XRectangle clip = {0, 0, frame.width, frame.height};

        if (client_surface_xcb_copy( display, source, image->pixmap, &image->xcb_gc,
                                    0, NULL, &rect, &rect, &clip, 1, FALSE, &copy->request, TRUE ))
        {
            queue_client_surface_reply( &copy->reply, &copy->request, 1, complete_client_surface_cache_reply );
            TRACE_(csperf)( "ticks=%llu event=cache_pending identity=%s cookie=%s token=%s sequence=%s "
                           "window=0 pixmap=%lx success=1 source=%lx display=%p request=%u barrier=%u\n",
                           client_surface_perf_time(), wine_dbgstr_longlong( binding->identity ),
                           wine_dbgstr_longlong( binding->cookie ), wine_dbgstr_longlong( control ),
                           wine_dbgstr_longlong( frame.source_sequence ), image->pixmap, source, display,
                           copy->request.cookies[copy->request.count - 1], copy->request.barrier );
            return;
        }
    }
    copy->native_pending = TRUE;
    client_surface_cache_copy( image->storage, source, complete_client_surface_cache_fallback, binding );
    TRACE_(csperf)( "ticks=%llu event=cache_image_pending identity=%s cookie=%s token=%s image=%p operation=2\n",
                   client_surface_perf_time(), wine_dbgstr_longlong( binding->identity ),
                   wine_dbgstr_longlong( binding->cookie ), wine_dbgstr_longlong( control ), image->storage );
    return;
done:
    release_client_surface_cached_source( binding, FALSE );
}

static void complete_client_surface_source_query( struct client_surface_geometry_query *query )
{
    struct client_surface_compositor_binding *binding =
        CONTAINING_RECORD( query, struct client_surface_compositor_binding, cache_copy.query );
    struct client_surface_cache_copy *copy = &binding->cache_copy;
    const struct client_surface_handoff_slot *frame = &copy->frame;

    assert( copy->query_pending );
    copy->query_pending = FALSE;
    TRACE_(csperf)( "ticks=%llu event=cache_query_complete identity=%s cookie=%s token=%s query=%p "
                   "success=%u retired=%u\n", client_surface_perf_time(),
                   wine_dbgstr_longlong( binding->identity ), wine_dbgstr_longlong( binding->cookie ),
                   wine_dbgstr_longlong( copy->control ), query, query->success, binding->retired );
    if (binding->retired || !query->success)
    {
        release_client_surface_cached_source( binding, FALSE );
        return;
    }
    binding->sources[copy->index] = (struct client_surface_source_cache){frame->source,
        frame->target_epoch, frame->source_visual, frame->width, frame->height, query->depth};
    start_client_surface_cache_copy( binding, query->depth );
}

static BOOL cache_client_surface_handoff( struct client_surface_compositor_binding *binding,
                                          unsigned int index, UINT64 control )
{
    struct client_surface_handoff_slot frame = binding->channel->slots[index];
    struct client_surface_cache_copy *copy = &binding->cache_copy;
    enum client_surface_query_status status;
    unsigned int depth;
    Pixmap source;
    BOOL success = FALSE;

    assert( !binding->retired && !client_surface_cache_read_pending( binding ) );
    copy->frame = frame;
    copy->index = index;
    copy->control = control;
    copy->started = client_surface_perf_time();
    if (frame.cookie != binding->cookie || frame.identity != binding->identity ||
        frame.producer_process != binding->process ||
        frame.window != wine_server_user_handle( binding->window ) ||
        frame.toplevel != wine_server_user_handle( binding->toplevel ) ||
        !(frame.flags & CLIENT_SURFACE_HANDOFF_NATIVE_X11) ||
        !(frame.flags & CLIENT_SURFACE_HANDOFF_COPY_SOURCE) || !frame.width || !frame.height ||
        !frame.source_visual) goto rejected;
    if (binding->latest_image.pixmap && frame.source_sequence < binding->latest_frame.source_sequence)
    {
        success = TRUE;
        goto rejected;
    }
    if (get_client_surface_compositor_source( binding, &frame, &source, &depth ))
    {
        trace_client_surface_source( "claim", binding, control, frame.source_sequence, 0, frame.source, TRUE );
        start_client_surface_cache_copy( binding, depth );
        return TRUE;
    }
    copy->query = (struct client_surface_geometry_query){.pixmap = frame.source,
        .min_width = frame.width, .min_height = frame.height, .complete = complete_client_surface_source_query};
    status = client_surface_query_geometry( &copy->query, wake_client_surface_compositor );
    if (status == CLIENT_SURFACE_QUERY_FULL) return FALSE;
    if (status == CLIENT_SURFACE_QUERY_FAILED) goto rejected;
    copy->query_pending = TRUE;
    trace_client_surface_source( "claim", binding, control, frame.source_sequence, 0, frame.source, TRUE );
    TRACE_(csperf)( "ticks=%llu event=cache_query_pending identity=%s cookie=%s token=%s sequence=%s "
                   "query=%p source=%lx\n", client_surface_perf_time(),
                   wine_dbgstr_longlong( binding->identity ), wine_dbgstr_longlong( binding->cookie ),
                   wine_dbgstr_longlong( control ), wine_dbgstr_longlong( frame.source_sequence ),
                   &copy->query, (Pixmap)frame.source );
    return TRUE;
rejected:
    trace_client_surface_source( "claim", binding, control, frame.source_sequence, 0, frame.source, TRUE );
    release_client_surface_cached_source( binding, success );
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
    BOOL steady;
    RECT destination;
    RECT source_damage;
    const RGNDATA *clip;
};

static void complete_client_surface_output_transform( void *context, BOOL success );
static void complete_client_surface_output_transform_batch( void *context, BOOL success );

static struct client_surface_output_transform *alloc_client_surface_output_transform(
    struct client_surface_compositor_target *target,
    struct client_surface_compositor_binding *binding, struct client_surface_compositor_frame *frame,
    const struct client_surface_composition_plan *plan, const RECT *damage,
    const RECT *catchup, BOOL needs_catchup, BOOL clipped, BOOL native )
{
    struct client_surface_compositor_frame *latest =
        get_client_surface_compositor_pixmap( target, target->latest );
    struct client_surface_output_transform *transform;
    const struct client_surface_handoff_slot *slot = &binding->latest_frame;
    SIZE_T count = plan->clip->rdh.nCount;

    assert( !target->transform && !target->copy_frame );
    if (count > (~(SIZE_T)0 - sizeof(*transform)) / sizeof(XRectangle) ||
        (needs_catchup && (!latest || !latest->image || !latest->revision ||
                          client_surface_cache_write_pending( latest->image )))) return NULL;
    if (!(transform = client_surface_alloc_owned_array( &target->memory, 1,
                         sizeof(*transform) + count * sizeof(XRectangle) ))) return NULL;
    transform->image = frame->image;
    transform->source = client_surface_cache_acquire( binding->latest_image.storage );
    if (needs_catchup) transform->catchup = client_surface_cache_acquire( latest->image );
    transform->native = (struct client_surface_cache_transform){
        .source = binding->latest_image.pixmap,
        .catchup = needs_catchup ? target->latest : 0,
        .source_visual = slot->source_visual, .destination_visual = target->visual,
        .source_width = slot->width, .source_height = slot->height,
        .destination = plan->destination, .catchup_rect = *catchup,
        .source_damage = plan->source_damage,
        .clips = transform->clips, .clip_count = count, .clipped = clipped, .native = native,
    };
    memcpy( transform->clips, plan->clip->Buffer, count * sizeof(XRectangle) );
    transform->toplevel = target->toplevel;
    transform->window = binding->window;
    transform->destination = target->window;
    transform->process = binding->process;
    transform->identity = binding->identity;
    transform->cookie = binding->cookie;
    transform->epoch = plan->epoch;
    transform->sequence = slot->source_sequence;
    transform->control = binding->latest_control;
    transform->revision = target->revision;
    transform->generation = plan->generation;
    transform->buffer_index = binding->latest_index;
    transform->scene_index = binding->scene_index;
    transform->width = target->window_width;
    transform->height = target->window_height;
    transform->replay = binding->source_sequence == slot->source_sequence;
    transform->damage = *damage;
    return transform;
}

static void free_client_surface_output_transforms( struct client_surface_output_transform *transform )
{
    struct client_surface_output_transform *next;

    for (; transform; transform = next)
    {
        next = transform->next;
        client_surface_cache_release( transform->source );
        client_surface_cache_release( transform->catchup );
        client_surface_free_owned_array( transform );
    }
}

static BOOL submit_client_surface_output_transform( struct client_surface_compositor_target *target,
    struct client_surface_compositor_binding *binding, struct client_surface_compositor_frame *frame,
    const struct client_surface_composition_plan *plan, const RECT *damage,
    const RECT *catchup, BOOL needs_catchup, BOOL clipped )
{
    struct client_surface_output_transform *transform;

    assert( plan->steady && !plan->generation );
    if (!(transform = alloc_client_surface_output_transform( target, binding, frame, plan, damage,
                                                             catchup, needs_catchup, clipped, FALSE ))) return FALSE;
    if (!client_surface_cache_transform_output( frame->image, &transform->native, 1,
                                                complete_client_surface_output_transform, transform ))
    {
        free_client_surface_output_transforms( transform );
        return FALSE;
    }
    target->transform = transform;
    /* Cancellation must not leave a partially written old checkpoint with
     * a valid journal revision, even when its image remains in this pool. */
    frame->revision = 0;
    TRACE_(csperf)( "ticks=%llu event=output_transform_submit transform=%p hwnd=%p window=%lx "
                   "image=%p source=%p catchup=%p destination=%lx epoch=%llu generation=0 sequence=%llu revision=%llu\n",
                   client_surface_perf_time(), transform, target->toplevel, target->window,
                   transform->image, transform->source, transform->catchup, frame->pixmap,
                   (unsigned long long)transform->epoch, (unsigned long long)transform->sequence,
                   (unsigned long long)transform->revision );
    return TRUE;
}

static BOOL copy_client_surface_handoff_to_frame(
    struct client_surface_compositor_target *target,
    struct client_surface_compositor_binding *binding,
    struct client_surface_compositor_frame *frame, Pixmap source, unsigned int source_depth,
    const struct client_surface_handoff_slot *slot,
    const struct client_surface_composition_plan *plan, const RECT *damage,
    BOOL batch, BOOL *pending )
{
    Display *display = client_surface_compositor_display;
    const XRectangle *clips = (const XRectangle *)plan->clip->Buffer;
    unsigned int clip_count = plan->clip->rdh.nCount;
    RECT catchup = {0};
    UINT64 revision = batch && !client_surface_copy_batch.asynchronous ?
                      client_surface_copy_batch.revision : frame->revision;
    BOOL clipped, incoming_full, needs_catchup, native, overlay_copied = TRUE;
    unsigned int destination_width = plan->destination.right - plan->destination.left;
    unsigned int destination_height = plan->destination.bottom - plan->destination.top;
    int error = 0;
    GC gc;

    if (!client_surface_compositor_frame_writable( frame )) return FALSE;
    if (!target->latest || !get_client_surface_compositor_catchup( target, frame, &catchup ))
        return FALSE;
    clipped = clip_count != 1 || clips[0].x || clips[0].y ||
              clips[0].width != destination_width || clips[0].height != destination_height;
    incoming_full = !clipped && damage->left == 0 && damage->top == 0 &&
                    (unsigned int)damage->right >= target->window_width &&
                    (unsigned int)damage->bottom >= target->window_height;
    needs_catchup = !incoming_full && frame->pixmap != target->latest &&
                    revision != target->revision && !IsRectEmpty( &catchup );
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
         * A full source replaces the checkpoint just as catchup does.
         * The revision is invalidated if any request in the batch fails. */
        if (copied && (needs_catchup || incoming_full)) frame->revision = target->revision;
        return copied;
    }

    TRACE_(csperf)( "ticks=%llu event=copy_route native=%u full=%u transaction=%u steady=%u assembly=%u "
                   "mailbox=%u ticket=%u latest=%u published=%u inflight=%u\n",
                   client_surface_perf_time(), native, incoming_full, !!plan->generation,
                   plan->steady, target->assembly_pending, target->mailbox_pending,
                   !!target->mailbox_publish_generation, frame->pixmap == target->latest,
                   frame->pixmap == target->published, !!frame->serial );
    if (batch)
    {
        struct client_surface_output_transform *transform;

        assert( plan->generation && frame->pixmap != target->backing );
        if (!(transform = alloc_client_surface_output_transform( target, binding, frame, plan, damage,
                                                                 &catchup, needs_catchup, clipped, native ))) return FALSE;
        if (client_surface_copy_batch.tail)
        {
            client_surface_copy_batch.tail->next = transform;
            client_surface_copy_batch.tail->native.next = &transform->native;
        }
        else client_surface_copy_batch.transform = transform;
        client_surface_copy_batch.tail = transform;
        /* Only the checked completion may make this revision real. Following
         * members use the planned revision to avoid overwriting earlier ones. */
        if (needs_catchup || incoming_full) client_surface_copy_batch.revision = target->revision;
        return TRUE;
    }
    if (plan->steady && !native)
    {
        *pending = submit_client_surface_output_transform( target, binding, frame, plan, damage,
                                                           &catchup, needs_catchup, clipped );
        return *pending;
    }
    if (!plan->generation && native &&
        !target->assembly_pending && !target->mailbox_pending &&
        frame->pixmap != target->latest && frame->pixmap != target->published &&
        client_surface_xcb_copy( display, source, frame->pixmap, &frame->xcb_gc,
                                 needs_catchup ? target->latest : 0, &catchup,
                                 &plan->source_damage, &plan->destination, clips, clip_count, clipped,
                                 &frame->copy_request, TRUE ))
    {
        queue_client_surface_compositor_reply( target, frame, NULL );
        *pending = TRUE;
        return TRUE;
    }

    X11DRV_expect_error( display, client_surface_compositor_error, &error );
    if (!(gc = frame->gc)) gc = frame->gc = XCreateGC( display, frame->pixmap, 0, NULL );
    if (gc)
    {
        /* Clip belongs to this copy's immutable scene member. Reset it before
         * copying the owner checkpoint into a reused output frame. */
        XSetClipMask( display, gc, None );
        XSetClipOrigin( display, gc, 0, 0 );
        if (needs_catchup)
        {
            XCopyArea( display, target->latest, frame->pixmap, gc,
                       catchup.left, catchup.top,
                       catchup.right - catchup.left, catchup.bottom - catchup.top,
                       catchup.left, catchup.top );
            TRACE_(csperf)( "ticks=%llu event=xlib_copy_request source=%lx destination=%lx width=%u height=%u clipped=0 route=checkpoint\n",
                           client_surface_perf_time(), target->latest, frame->pixmap,
                           catchup.right - catchup.left, catchup.bottom - catchup.top );
        }
        if (clip_count)
        {
            if (clipped)
                XSetClipRectangles( display, gc, plan->destination.left,
                                    plan->destination.top, (XRectangle *)clips, clip_count, YXBanded );
            if (overlay_copied && native)
            {
                XCopyArea( display, source, frame->pixmap, gc,
                           plan->source_damage.left, plan->source_damage.top,
                           plan->source_damage.right - plan->source_damage.left,
                           plan->source_damage.bottom - plan->source_damage.top,
                           plan->destination.left + plan->source_damage.left,
                           plan->destination.top + plan->source_damage.top );
                TRACE_(csperf)( "ticks=%llu event=xlib_copy_request source=%lx destination=%lx width=%u height=%u clipped=%u route=overlay\n",
                               client_surface_perf_time(), source, frame->pixmap,
                               plan->source_damage.right - plan->source_damage.left,
                               plan->source_damage.bottom - plan->source_damage.top, clipped );
            }
            else if (overlay_copied)
            {
                overlay_copied = X11DRV_XRender_CopyClientSurface(
                    display, source, slot->source_visual, frame->pixmap, target->visual,
                    slot->width, slot->height, &plan->destination,
                    clipped ? clips : NULL, clipped ? clip_count : 0, 0, 0 );
                if (!overlay_copied)
                    overlay_copied = client_surface_copy_image(
                        display, &target->memory, source, frame->pixmap, gc, slot->source_visual, target->visual,
                        slot->width, slot->height, &plan->destination );
            }
        }
    }
    XSync( display, False );
    X11DRV_check_error();
    TRACE_(csperf)( "ticks=%llu event=xlib_output_checked destination=%lx copied=%u error=%d sync_calls=1\n",
                   client_surface_perf_time(), frame->pixmap, !!gc && overlay_copied, error );
    if (!gc || error || !overlay_copied)
    {
        /* A native sequence may have copied only part of this private
         * image. Its old journal revision cannot describe those bytes. */
        frame->revision = 0;
        discard_client_surface_compositor_gc( frame );
        return FALSE;
    }
    /* A successful full source also consumes the checkpoint. Later partial
     * or empty members must preserve its pixels in this private assembly. */
    if (needs_catchup || incoming_full) frame->revision = target->revision;
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

static void apply_client_surface_owner_copies( struct client_surface_compositor_target *target,
    struct client_surface_compositor_frame *frame, const struct client_surface_owner_copy *copies,
    unsigned int count, BOOL success, BOOL asynchronous )
{
    unsigned int i;
    UINT64 generation, epoch;
    BOOL assembly_valid;

    if (!count) return;
    generation = copies[0].generation;
    epoch = copies[0].epoch;
    assembly_valid = target->assembly_pending && target->scene.valid &&
        target->assembly_generation == generation && target->assembly_epoch == epoch &&
        target->scene.epoch == epoch && target->assembly_frame == frame - target->frames;
    for (i = 0; i < count; ++i)
    {
        const struct client_surface_owner_copy *copy = &copies[i];
        struct client_surface_compositor_binding *binding = copy->binding;

        trace_client_surface_source( copy->replay ?
                                     (asynchronous ? "replay_copy_async" : "replay_copy_sync") :
                                     (asynchronous ? "copy_async" : "copy_sync"),
                                     binding, copy->control, copy->sequence,
                                     target->window, frame->pixmap, success && assembly_valid );
        if (success && assembly_valid)
        {
            struct client_surface_handoff_receipt *receipt = &target->receipts[binding->scene_index];

            note_client_surface_source_copy( binding, copy->epoch, copy->sequence );
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
           frame->pixmap, asynchronous );
    if (!success)
    {
        invalidate_client_surface_compositor_assembly( target );
        wake_client_surface_compositor();
    }
    else if (client_surface_handoff_generation_assembled( target, frame, generation, epoch ))
        publish_client_surface_handoff_assembly( target, frame, generation, epoch );
}

static void complete_client_surface_copy_batch( struct client_surface_copy_batch *batch, BOOL success )
{
    unsigned int i, count = batch->count;

    batch->count = 0;
    for (i = 0; i < count; ++i) client_surface_cache_release( batch->copies[i].image );
    if (!success) discard_client_surface_compositor_gc( batch->frame );
    apply_client_surface_owner_copies( batch->target, batch->frame, batch->copies, count,
                                       success, batch->asynchronous );
}

static void flush_client_surface_copy_batch(void)
{
    struct client_surface_copy_batch *batch = &client_surface_copy_batch;
    struct client_surface_output_transform *transform;
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
        queue_client_surface_compositor_reply( client_surface_copy_batch.target,
            client_surface_copy_batch.frame, &client_surface_pending_batches[i] );
        client_surface_copy_batch.count = 0;
        return;
    }
    transform = batch->transform;
    if (!batch->error)
    {
        assert( transform && !batch->target->transform && !batch->target->copy_frame );
        transform->frame_revision = batch->revision;
        if (client_surface_cache_transform_output( batch->frame->image, &transform->native, batch->count,
                                                   complete_client_surface_output_transform_batch, transform ))
        {
            struct client_surface_output_transform *member;

            batch->target->transform = transform;
            batch->frame->revision = 0;
            TRACE_(csperf)( "ticks=%llu event=output_transform_batch_submit transform=%p hwnd=%p window=%lx "
                           "image=%p destination=%lx epoch=%llu generation=%llu count=%u revision=%llu\n",
                           client_surface_perf_time(), transform, transform->toplevel, transform->destination,
                           transform->image, batch->frame->pixmap, (unsigned long long)transform->epoch,
                           (unsigned long long)transform->generation, batch->count,
                           (unsigned long long)transform->frame_revision );
            for (member = transform, i = 0; member; member = member->next, ++i)
                TRACE_(csperf)( "ticks=%llu event=output_transform_batch_member transform=%p member=%p "
                               "image=%p source=%p catchup=%p index=%u scene_index=%u identity=%llu cookie=%llu "
                               "epoch=%llu generation=%llu sequence=%llu control=%llu\n",
                               client_surface_perf_time(), transform, member, member->image, member->source,
                               member->catchup, i, member->scene_index, (unsigned long long)member->identity,
                               (unsigned long long)member->cookie, (unsigned long long)member->epoch,
                               (unsigned long long)member->generation, (unsigned long long)member->sequence,
                               (unsigned long long)member->control );
            batch->count = 0;
        }
    }
    if (batch->count)
    {
        /* Nothing was submitted. The current actor-local builder may fail
         * its assembly, but has no native GC to retire on this connection. */
        free_client_surface_output_transforms( transform );
        i = batch->count;
        batch->count = 0;
        apply_client_surface_owner_copies( batch->target, batch->frame, batch->copies, i, FALSE, TRUE );
    }
    batch->transform = batch->tail = NULL;
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

static void complete_client_surface_frame_copy( struct client_surface_compositor_target *target,
    struct client_surface_compositor_frame *frame, struct client_surface_compositor_binding *binding,
    UINT64 epoch, UINT64 sequence, const RECT *damage, BOOL success )
{
    if (!success)
    {
        binding->source_epoch = binding->source_sequence = 0;
        binding->replay_epoch = 0;
        frame->revision = 0;
        return;
    }
    note_client_surface_source_copy( binding, epoch, sequence );
    note_client_surface_compositor_damage( target, frame, damage );
    publish_client_surface_handoff_frame( target, frame, NULL );
}

static void complete_client_surface_output_transform( void *context, BOOL success )
{
    struct client_surface_output_transform *transform = context;
    struct client_surface_compositor_target *target =
        find_client_surface_compositor_target( transform->toplevel );
    struct client_surface_compositor_frame *frame = NULL;
    struct client_surface_compositor_binding *binding = NULL;
    struct client_surface_scene scene;
    BOOL current = target && target->transform == transform;

    if (current)
    {
        target->transform = NULL;
        frame = get_client_surface_compositor_pixmap( target, client_surface_cache_pixmap( transform->image ) );
        current = frame && frame->image == transform->image && !frame->serial &&
                  target->window == transform->destination && !target->quiescing &&
                  target->revision == transform->revision && target->window_width == transform->width &&
                  target->window_height == transform->height && target->visual == transform->native.destination_visual &&
                  target->scene.valid &&
                  target->scene.epoch == transform->epoch && transform->scene_index < target->scene.count &&
                  client_surface_get_toplevel_scene( target->toplevel, &scene ) &&
                  scene.epoch == transform->epoch && scene.mode != CLIENT_SURFACE_PRESENTATION_DIRECT &&
                  !scene.generation;
        if (current)
        {
            binding = target->scene.members[transform->scene_index];
            current = binding->window == transform->window && binding->process == transform->process &&
                      binding->identity == transform->identity && binding->cookie == transform->cookie &&
                      client_surface_compositor_binding_is_live( binding ) &&
                      (binding->source_epoch != transform->epoch || binding->source_sequence <= transform->sequence);
        }
        if (!current) target->replay_member = 0;
    }
    TRACE_(csperf)( "ticks=%llu event=output_transform_complete transform=%p hwnd=%p window=%lx "
                   "image=%p source=%p destination=%lx epoch=%llu sequence=%llu success=%u current=%u\n",
                   client_surface_perf_time(), transform, transform->toplevel, transform->destination,
                   transform->image, transform->source, client_surface_cache_pixmap( transform->image ),
                   (unsigned long long)transform->epoch, (unsigned long long)transform->sequence, success, current );
    /* Completion removed CACHE_TRANSFORM before invoking us. Drop all native
     * leases even when REMOVE has already released the target and binding. */
    client_surface_cache_release( transform->source );
    client_surface_cache_release( transform->catchup );
    client_surface_cache_release( transform->image );
    if (current)
    {
        trace_client_surface_source( transform->replay ? "replay_transform" : "transform",
            binding, transform->control, transform->sequence, target->window, frame->pixmap, success );
        complete_client_surface_frame_copy( target, frame, binding, transform->epoch,
                                            transform->sequence, &transform->damage, success );
    }
    client_surface_free_owned_array( transform );
}

static void complete_client_surface_output_transform_batch( void *context, BOOL success )
{
    struct client_surface_output_transform *transform = context, *member;
    struct client_surface_cache_image *image = transform->image;
    struct client_surface_compositor_target *target =
        find_client_surface_compositor_target( transform->toplevel );
    struct client_surface_compositor_frame *frame = NULL;
    struct client_surface_owner_copy copies[CLIENT_SURFACE_COPY_BATCH_SIZE];
    struct client_surface_scene scene;
    unsigned int count = 0;
    BOOL attached = target && target->transform == transform, current = attached;

    if (current)
    {
        frame = get_client_surface_compositor_pixmap( target, client_surface_cache_pixmap( transform->image ) );
        current = frame && frame->image == transform->image && !frame->serial &&
                  frame->pixmap != target->backing && target->window == transform->destination &&
                  !target->quiescing && target->revision == transform->revision &&
                  target->window_width == transform->width && target->window_height == transform->height &&
                  target->visual == transform->native.destination_visual && target->scene.valid &&
                  target->scene.epoch == transform->epoch && target->assembly_pending &&
                  target->assembly_generation == transform->generation &&
                  target->assembly_epoch == transform->epoch && target->assembly_frame == frame - target->frames &&
                  client_surface_get_toplevel_scene( target->toplevel, &scene ) &&
                  scene.epoch == transform->epoch && scene.generation == transform->generation &&
                  scene.mode != CLIENT_SURFACE_PRESENTATION_DIRECT && !scene.publication_pending;
    }
    /* Resolve all saved identities before changing any receipt or source
     * epoch. A stale result must never abort a replacement assembly. */
    for (member = transform; member; member = member->next, ++count)
    {
        struct client_surface_compositor_binding *binding;

        assert( count < ARRAY_SIZE(copies) );
        if (!current) continue;
        current = member->scene_index < target->scene.count;
        if (!current) continue;
        binding = target->scene.members[member->scene_index];
        current = binding->window == member->window && binding->process == member->process &&
                  binding->identity == member->identity && binding->cookie == member->cookie &&
                  client_surface_compositor_binding_is_live( binding ) &&
                  (binding->source_epoch != member->epoch || binding->source_sequence <= member->sequence);
        copies[count] = (struct client_surface_owner_copy){binding, NULL, member->buffer_index,
            member->control, member->generation, member->epoch, member->sequence, member->replay};
    }
    TRACE_(csperf)( "ticks=%llu event=output_transform_batch_complete transform=%p hwnd=%p window=%lx "
                   "image=%p destination=%lx epoch=%llu generation=%llu count=%u success=%u current=%u\n",
                   client_surface_perf_time(), transform, transform->toplevel, transform->destination,
                   transform->image, client_surface_cache_pixmap( transform->image ),
                   (unsigned long long)transform->epoch, (unsigned long long)transform->generation,
                   count, success, current );
    /* The destination's sole native write reference is shared by the whole
     * group. Return it before the existing assembly commit can publish. */
    if (current && success) frame->revision = transform->frame_revision;
    if (attached)
    {
        if (!current) abort_client_surface_output_transform_assembly( target, transform );
        target->transform = NULL;
    }
    free_client_surface_output_transforms( transform );
    client_surface_cache_release( image );
    if (current) apply_client_surface_owner_copies( target, frame, copies, count, success, TRUE );
}

static void complete_client_surface_output_reply( struct client_surface_compositor_reply *reply, BOOL success )
{
    struct client_surface_compositor_frame *frame =
        CONTAINING_RECORD( reply, struct client_surface_compositor_frame, reply );
    struct client_surface_compositor_target *target = frame->reply_target;
    struct client_surface_copy_batch *batch = frame->reply_batch;
    struct client_surface_compositor_binding *binding;
    UINT64 control;

    frame->reply_target = NULL;
    frame->reply_batch = NULL;
    if (frame->request_pending)
    {
#ifdef SONAME_LIBXPRESENT
        complete_client_surface_present_request( target, frame, success );
#endif
        return;
    }
    assert( target->copy_frame == frame );
    target->copy_frame = NULL;
    if (batch)
    {
        --client_surface_pending_batch_count;
        complete_client_surface_copy_batch( batch, success && !batch->error );
        return;
    }
    binding = frame->copy_binding;
    assert( binding );
    control = frame->copy_control;
    frame->copy_binding = NULL;
    client_surface_cache_release( frame->copy_image );
    frame->copy_image = NULL;
    TRACE( "validated owner copy request %u pixmap %#lx success %u\n",
           frame->copy_request.cookies[0], frame->pixmap, success );
    trace_client_surface_source( frame->copy_replay ? "replay_copy_async" : "copy_async",
                                 binding, control, frame->copy_sequence,
                                 target->window, frame->pixmap, success );
    if (!success) discard_client_surface_compositor_gc( frame );
    complete_client_surface_frame_copy( target, frame, binding, frame->copy_epoch,
                                        frame->copy_sequence, &frame->copy_damage, success );
}

static BOOL process_client_surface_compositor_replies(void)
{
    struct client_surface_compositor_reply *reply;
    unsigned int inspected = 0, completed = 0;
    BOOL success;

    while (inspected < 64 && !list_empty( &client_surface_compositor_replies ))
    {
        reply = LIST_ENTRY( list_head( &client_surface_compositor_replies ),
                            struct client_surface_compositor_reply, entry );
        ++inspected;
        if (!client_surface_xcb_poll_batch( client_surface_compositor_display, reply->requests,
                                            reply->count, &success )) break;
        ++completed;
        /* Completion may requeue the same node or release its owning object. */
        list_remove( &reply->entry );
        reply->requests = NULL;
        reply->complete( reply, success );
    }
    TRACE_(csperf)( "ticks=%llu event=compositor_reply_scan inspected=%u completed=%u pending=%u\n",
                   client_surface_perf_time(), inspected, completed, !list_empty( &client_surface_compositor_replies ) );
    return !!completed;
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

static BOOL repair_client_surface_compositor_owner( HWND toplevel, BOOL resolve )
{
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( toplevel );
    struct client_surface_handoff_receipt *receipts;
    struct client_surface_scene current;
    unsigned int i, count = 0;
    BOOL accepted = FALSE;

    if (resolve && (!client_surface_get_toplevel_scene( toplevel, &current ) || !current.source_pending))
        return TRUE;
    if (!target || !target->scene.valid || !target->scene.count ||
        !client_surface_scene_snapshot_current( toplevel, target->scene.epoch )) return FALSE;
    if (!(receipts = client_surface_alloc_owned_array( &target->memory, target->scene.count, sizeof(*receipts) ))) return FALSE;
    for (i = 0; i < target->scene.count; ++i)
    {
        struct client_surface_compositor_binding *binding = target->scene.members[i];

        if (!client_surface_compositor_binding_is_live( binding ) ||
            !client_surface_cached_frame_matches_layout( binding, &target->scene.layouts[i] ))
        {
            if (resolve) continue;
            goto done;
        }
        receipts[count++] = (struct client_surface_handoff_receipt){
            .handle = wine_server_user_handle( binding->window ), .process = binding->process,
            .surface = binding->identity, .cookie = binding->cookie,
            .source_generation = binding->latest_frame.source_sequence, .buffer_index = binding->latest_index,
        };
    }
    if (resolve)
    {
        SERVER_START_REQ( resolve_client_surface_scene_sources )
        {
            req->handle = wine_server_user_handle( toplevel );
            req->scene_id = target->scene.epoch;
            wine_server_add_data( req, receipts, count * sizeof(*receipts) );
            if (!wine_server_call( req )) accepted = reply->accepted;
        }
        SERVER_END_REQ;
        TRACE( "owner scene sources hwnd %p scene %s images %u/%u accepted %u\n", toplevel,
               wine_dbgstr_longlong( target->scene.epoch ), count, target->scene.count, accepted );
    }
    else
    {
        SERVER_START_REQ( request_client_surface_owner_repair )
        {
            req->handle = wine_server_user_handle( toplevel );
            req->scene_id = target->scene.epoch;
            wine_server_add_data( req, receipts, count * sizeof(*receipts) );
            if (!wine_server_call( req )) accepted = reply->accepted;
        }
        SERVER_END_REQ;
        TRACE( "owner cache repair hwnd %p scene %s images %u accepted %u\n", toplevel,
               wine_dbgstr_longlong( target->scene.epoch ), count, accepted );
    }
    if (accepted)
    {
        /* A repair of an existing assembly can keep its scene ID. Backing
         * damage must nevertheless replay every retained image in that scene. */
        target->replay_member = 0;
        for (i = 0; i < target->scene.count; ++i) target->scene.members[i]->replay_epoch = 0;
    }
done:
    client_surface_free_owned_array( receipts );
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
    if (!target || target->copy_frame || target->transform || target->quiescing || !target->scene.valid) return FALSE;
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
    /* A returned output credit also starts reading the newest retained
     * producer image. Do not spend that credit on an older dirty cache while
     * its replacement is being checked. Scene obligations still use their
     * complete owner images independently of this steady-frame preference. */
    if (!current.generation && !current.source_pending && !current.publication_pending &&
        client_surface_cache_read_pending( binding )) return FALSE;
    /* Inventory resolution precedes the first copy, so the full assembly
     * cannot race its own pending decision or publish a stale cache proof. */
    if (current.source_pending &&
        (!repair_client_surface_compositor_owner( binding->toplevel, TRUE ) ||
         !client_surface_get_toplevel_scene( binding->toplevel, &current ) || current.source_pending ||
         current.epoch != target->scene.epoch || current.mode == CLIENT_SURFACE_PRESENTATION_DIRECT))
        return FALSE;
    if (replay) trace_client_surface_source( "replay_cache", binding, control,
                                             slot->source_sequence, target->window, source, TRUE );
    /* PUBLISHING still carries the transaction generation while the GUI
     * exposes its native output, even after its Present ticket is released.
     * Its assembly was already accepted: newer images use the steady path
     * in the same scene, never another completion of that transaction. */
    plan.generation = current.publication_pending ? 0 : current.generation;
    /* PREPARE and STAGED install a fresh GUI checkpoint, using pair COW if
     * an older transform still owns its spare. Keep that checkpoint intact
     * through the subsequent nonzero generation, including PUBLISHING where
     * plan.generation is zero but the GUI still owes its native publication.
     * Only a truly steady result may detach without a publication receipt. */
    plan.steady = !current.generation;
    TRACE( "owner source window %#lx generation %s epoch %s publication %u output %s sequence %s\n",
           target->window, wine_dbgstr_longlong( current.generation ), wine_dbgstr_longlong( current.epoch ),
           current.publication_pending, wine_dbgstr_longlong( plan.generation ),
           wine_dbgstr_longlong( slot->source_sequence ) );
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
    if (target->copy_frame || target->transform ||
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
#ifdef SONAME_LIBXPRESENT
        /* A single steady layer can retain newer complete images in its
         * owner cache until a native output can be submitted. Rewriting the
         * mailbox while both Present credits are owned only replaces work
         * that cannot become visible. Keep source_sequence at the last copy;
         * a skipped damage base then takes the existing full-image recovery.
         * Transactions and multi-layer replay keep their assembly ordering. */
        if (!plan.generation && target->scene.count == 1 && usexpresent && target->present_event &&
            count_client_surface_compositor_frames( target ) >= CLIENT_SURFACE_COMPOSITOR_MAX_INFLIGHT)
            goto retry;
#endif
        if (!plan.generation || previous_publish)
            frame = get_client_surface_compositor_frame( target );
        else if (target->assembly_pending &&
                 target->assembly_generation == plan.generation &&
                 target->assembly_epoch == plan.epoch)
            frame = &target->frames[target->assembly_frame];
        else
            frame = acquire_client_surface_compositor_assembly_frame( target );

        if (!frame || !client_surface_compositor_frame_writable( frame ))
        {
retry:
            /* Output backpressure retains the owner cache, never a producer
             * slot. Retry this image when output work makes progress. */
            binding->replay_epoch = 0;
            target->replay_member = min( target->replay_member, binding->scene_index );
            return FALSE;
        }
        /* A failed publication can leave staging without a generation.
         * Once both retained source and output storage are usable, rebuild
         * that publication before copying: a steady Present cannot expose
         * the staged window. Keep these owned resources on rejection and
         * wait for a current plan after repair changes the scene epoch. */
        if (current.mode == CLIENT_SURFACE_PRESENTATION_STAGED && !current.generation)
        {
            repair_client_surface_compositor_owner( binding->toplevel, FALSE );
            goto retry;
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
                if (target->copy_frame || target->transform) goto retry;
            }
            if (!client_surface_copy_batch.count)
            {
                client_surface_copy_batch.target = target;
                client_surface_copy_batch.frame = frame;
                client_surface_copy_batch.error = 0;
                client_surface_copy_batch.asynchronous = asynchronous;
                client_surface_copy_batch.revision = frame->revision;
                client_surface_copy_batch.transform = client_surface_copy_batch.tail = NULL;
            }
            assert( client_surface_copy_batch.frame == frame );
            client_surface_copy_batch.copies[client_surface_copy_batch.count++] =
                (struct client_surface_owner_copy){binding, asynchronous ?
                                                   client_surface_cache_acquire( binding->latest_image.storage ) : NULL,
                                                   buffer_index, control, plan.generation, plan.epoch,
                                                   slot->source_sequence, replay};
            client_surface_copy_batch.requests[client_surface_copy_batch.count - 1] =
                (struct client_surface_xcb_request){0};
        }
        copied = copy_client_surface_handoff_to_frame( target, binding, frame, source, source_depth,
                                                       slot, &plan, &damage, batch, &pending );
        if (copied)
        {
            if (!pending && !batch)
                note_client_surface_source_copy( binding, plan.epoch, slot->source_sequence );
            frame->width = target->window_width;
            frame->height = target->window_height;
        }
        if (pending)
        {
            if (target->transform) return TRUE;
            target->copy_frame = frame;
            frame->copy_binding = binding;
            frame->copy_image = client_surface_cache_acquire( binding->latest_image.storage );
            frame->copy_index = buffer_index;
            frame->copy_control = control;
            frame->copy_sequence = slot->source_sequence;
            frame->copy_epoch = plan.epoch;
            frame->copy_replay = replay;
            frame->copy_damage = damage;
            return TRUE;
        }
        if (batch)
        {
            if (!copied) client_surface_copy_batch.error = 1;
            if (!copied || client_surface_copy_batch.count == CLIENT_SURFACE_COPY_BATCH_SIZE)
                flush_client_surface_copy_batch();
            return copied;
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
    if (replay && (copied || dropped)) binding->replay_epoch = target->scene.epoch;
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

static BOOL client_surface_handoff_matches_cache( const struct client_surface_compositor_binding *binding,
                                                 const struct client_surface_handoff_slot *slot )
{
    const struct client_surface_handoff_slot *cached = &binding->latest_frame;

    return slot->cookie == cached->cookie && slot->identity == cached->identity &&
           slot->producer_process == cached->producer_process && slot->window == cached->window &&
           slot->toplevel == cached->toplevel && slot->target_epoch == cached->target_epoch &&
           slot->source && slot->source_visual == cached->source_visual &&
           slot->width == cached->width && slot->height == cached->height &&
           (slot->flags & (CLIENT_SURFACE_HANDOFF_NATIVE_X11 | CLIENT_SURFACE_HANDOFF_COPY_SOURCE)) ==
                         (CLIENT_SURFACE_HANDOFF_NATIVE_X11 | CLIENT_SURFACE_HANDOFF_COPY_SOURCE);
}

/* Only unread steady images can be superseded. Keep the last complete owner
 * image and the newest published producer image; a scene transaction or a
 * native read must never depend on a slot returned by this path. */
static BOOL coalesce_client_surface_handoffs( struct client_surface_compositor_binding *binding,
                                             UINT64 *consumed, UINT64 produced, unsigned int limit )
{
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( binding->toplevel );
    struct client_surface_handoff_channel *channel = binding->channel;
    const struct client_surface_handoff_slot *newest;
    struct client_surface_scene scene;
    UINT64 previous = *consumed;

    assert( !client_surface_cache_read_pending( binding ) && produced != *consumed );
    if (!target || target->quiescing || target->assembly_pending || !target->scene.valid ||
        binding->scene_index >= target->scene.count || target->scene.members[binding->scene_index] != binding ||
        !client_surface_cached_frame_matches_layout( binding, &target->scene.layouts[binding->scene_index] ) ||
        !client_surface_get_toplevel_scene( binding->toplevel, &scene ) || scene.epoch != target->scene.epoch ||
        scene.mode != CLIENT_SURFACE_PRESENTATION_COMPOSITED || scene.generation ||
        scene.source_pending || scene.publication_pending) return FALSE;
    newest = &channel->slots[(produced - 1) & (CLIENT_SURFACE_HANDOFF_RING_SIZE - 1)];
    if (!client_surface_handoff_matches_cache( binding, newest ) ||
        newest->source_sequence < binding->latest_frame.source_sequence) return FALSE;
    while (produced - *consumed > 1 && limit--)
    {
        const struct client_surface_handoff_slot slot =
            channel->slots[*consumed & (CLIENT_SURFACE_HANDOFF_RING_SIZE - 1)];

        if (!client_surface_handoff_matches_cache( binding, &slot ) ||
            slot.source_sequence >= newest->source_sequence) break;
        ++*consumed;
        /* No native request has read this slot. The producer can now reuse
         * it, but cannot replace newest before its separate read receipt. */
        __atomic_store_n( &channel->consumer_sequence, *consumed, __ATOMIC_RELEASE );
        TRACE_(csperf)( "ticks=%llu event=cache_skip identity=%s cookie=%s token=%s sequence=%s "
                       "pixmap=%s retained_token=%s retained_sequence=%s cached_sequence=%s scene=%s\n",
                       client_surface_perf_time(), wine_dbgstr_longlong( binding->identity ),
                       wine_dbgstr_longlong( binding->cookie ), wine_dbgstr_longlong( *consumed ),
                       wine_dbgstr_longlong( slot.source_sequence ), wine_dbgstr_longlong( slot.source ),
                       wine_dbgstr_longlong( produced ), wine_dbgstr_longlong( newest->source_sequence ),
                       wine_dbgstr_longlong( binding->latest_frame.source_sequence ), wine_dbgstr_longlong( scene.epoch ) );
    }
    if (*consumed != previous) client_surface_handoff_wake_release( binding->pool->shared );
    if (produced - *consumed != 1) return FALSE;
#ifdef SONAME_LIBXPRESENT
    /* Native Complete/Idle or a new READY wakes the actor. Preserve the hint
     * across its armed rescan without reporting a parked image as progress.
     * A newer READY may replace this one; the final image waits for credit. */
    if (usexpresent && target->present_event &&
        count_client_surface_compositor_frames( target ) >= CLIENT_SURFACE_COMPOSITOR_MAX_INFLIGHT)
    {
        TRACE_(csperf)( "ticks=%llu event=cache_defer identity=%s cookie=%s token=%s sequence=%s scene=%s\n",
                       client_surface_perf_time(), wine_dbgstr_longlong( binding->identity ),
                       wine_dbgstr_longlong( binding->cookie ), wine_dbgstr_longlong( produced ),
                       wine_dbgstr_longlong( newest->source_sequence ), wine_dbgstr_longlong( scene.epoch ) );
        return TRUE;
    }
#endif
    return FALSE;
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
        if (pool->query_generation != client_surface_compositor_query_generation)
        {
            unsigned int restored = 0;

            /* Native return alone does not free query capacity: the actor
             * must consume the result. Rearm parked hints once per actual
             * capacity change, without repeatedly reporting FULL as work.
             * New producer READYs can still coalesce or hit warm metadata. */
            for (n = 0; n < CLIENT_SURFACE_HANDOFF_BITMAP_WORDS; ++n)
            {
                restored += __builtin_popcountll( pool->query_wait_bitmap[n] );
                __atomic_fetch_or( &pool->shared->ready_bitmap[n], pool->query_wait_bitmap[n], __ATOMIC_RELEASE );
                pool->query_wait_bitmap[n] = 0;
            }
            pool->query_generation = client_surface_compositor_query_generation;
            if (restored)
                TRACE_(csperf)( "ticks=%llu event=source_query_retry pool=%s generation=%s hints=%u\n",
                               client_surface_perf_time(), wine_dbgstr_longlong( pool->id ),
                               wine_dbgstr_longlong( pool->query_generation ), restored );
        }
        for (n = 0; n < CLIENT_SURFACE_HANDOFF_BITMAP_WORDS; ++n)
        {
            unsigned int word = (start + n) % CLIENT_SURFACE_HANDOFF_BITMAP_WORDS;
            UINT64 bits = __atomic_load_n( &pool->shared->ready_bitmap[word], __ATOMIC_ACQUIRE );

            while (bits)
            {
                unsigned int bit = __builtin_ctzll( bits ), index = word * 64 + bit;
                struct client_surface_compositor_binding *binding = pool->bindings[index], **cursor;
                struct client_surface_handoff_channel *channel = &pool->shared->channels[index];
                UINT64 consumed, produced, previous;
                unsigned int frames = 0;

                bits &= bits - 1;
                if (!binding) continue;
                pool->query_wait_bitmap[word] &= ~((UINT64)1 << bit);
                /* Clear the hint before acquiring the sequence. A concurrent
                 * publisher either appears in that load or leaves its bit set. */
                __atomic_fetch_and( &pool->shared->ready_bitmap[word], ~((UINT64)1 << bit), __ATOMIC_ACQ_REL );
                while (!client_surface_cache_read_pending( binding ) &&
                       !__atomic_load_n( &channel->closed, __ATOMIC_ACQUIRE ))
                {
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
                    flush_client_surface_copy_batch();
                    previous = consumed;
                    if (coalesce_client_surface_handoffs( binding, &consumed, produced,
                                                         CLIENT_SURFACE_HANDOFF_RING_SIZE - frames ))
                    {
                        progressed |= consumed != previous;
                        budget -= consumed - previous;
                        __atomic_fetch_or( &pool->shared->ready_bitmap[word], (UINT64)1 << bit, __ATOMIC_RELEASE );
                        break;
                    }
                    budget -= consumed - previous;
                    frames += consumed - previous;
                    /* A full native queue still consumes an admission attempt. */
                    --budget;
                    if (!cache_client_surface_handoff( binding, consumed & (CLIENT_SURFACE_HANDOFF_RING_SIZE - 1),
                                                        consumed + 1 ))
                    {
                        progressed |= consumed != previous;
                        pool->query_wait_bitmap[word] |= (UINT64)1 << bit;
                        TRACE_(csperf)( "ticks=%llu event=source_query_defer identity=%s cookie=%s token=%s pool=%s\n",
                                       client_surface_perf_time(), wine_dbgstr_longlong( binding->identity ),
                                       wine_dbgstr_longlong( binding->cookie ), wine_dbgstr_longlong( consumed + 1 ),
                                       wine_dbgstr_longlong( pool->id ) );
                        break;
                    }
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

static BOOL replay_client_surface_scene_sources( struct client_surface_compositor_target *target,
                                                 unsigned int *budget )
{
    BOOL progressed = FALSE;

    /* Scene replay reads owner-local images. It never claims a returned
     * producer slot or depends on the producer retaining its previous XID. */
    if (!target->scene.valid || target->quiescing || target->copy_frame || target->transform) return FALSE;
    while (target->replay_member < target->scene.count && *budget)
    {
        struct client_surface_compositor_binding *binding = target->scene.members[target->replay_member];

        --*budget;
        if (binding->latest_image.pixmap && binding->replay_epoch != target->scene.epoch)
        {
            BOOL queued = compose_client_surface_cached_frame( binding );

            /* Accepted asynchronous copies advance this bounded scan,
             * but commit their source checkpoint only with their real
             * reply. A newer source resets the scan above. */
            if (!queued && binding->latest_image.pixmap &&
                binding->replay_epoch != target->scene.epoch) break;
        }
        ++target->replay_member;
        progressed = TRUE;
        if (target->copy_frame || target->transform) break;
    }
    flush_client_surface_copy_batch();
    return progressed;
}

static void drain_client_surface_notification( int fd )
{
    UINT64 value;
    int ret;

    do ret = read( fd, &value, sizeof(value) ); while (ret > 0 || (ret < 0 && errno == EINTR));
}

static void arm_client_surface_compositor_work(void)
{
    struct client_surface_compositor_pool *pool;

    drain_client_surface_notification( client_surface_compositor_notify[0] );
    for (pool = client_surface_compositor_pools; pool; pool = pool->next)
    {
        drain_client_surface_notification( pool->ready_fd );
        __atomic_store_n( &pool->shared->ready_parked, 1, __ATOMIC_RELEASE );
    }
}

static void wait_client_surface_compositor_work( const struct client_surface_compositor_scan *scan )
{
    struct pollfd waiters[CLIENT_SURFACE_HANDOFF_MAX_POOLS_PER_CONSUMER + 2];
    struct client_surface_compositor_pool *pool;
    unsigned int count = 0;
    DWORD elapsed;
    int ret, timeout = scan->timeout;

    /* A complete quiet traversal follows arming. A later publisher leaves
     * its notification unread. Xlib may also have buffered replies while
     * handling sources or events: check the oldest barrier after its final
     * read, without scanning every target or draining any notification. */
    assert( !scan->remaining && scan->generation == client_surface_compositor_target_generation );
    if (client_surface_compositor_display && XPending( client_surface_compositor_display )) return;
    if (process_client_surface_compositor_replies())
    {
        wake_client_surface_compositor_queues();
        return;
    }
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
    /* The earliest deadline was collected during the traversal. Time spent
     * in later slices must not extend it, including across tick wraparound. */
    do
    {
        if ((timeout = scan->timeout) >= 0)
        {
            elapsed = NtGetTickCount() - scan->timeout_start;
            timeout = elapsed >= timeout ? 0 : timeout - elapsed;
        }
        TRACE_(csperf)( "ticks=%llu event=compositor_wait targets=%u remaining=%u timeout=%d generation=%s\n",
                       client_surface_perf_time(), client_surface_compositor_target_count, scan->remaining, timeout,
                       wine_dbgstr_longlong( scan->generation ) );
        ret = poll( waiters, count, timeout );
    } while (ret < 0 && errno == EINTR);
    wake_client_surface_compositor_queues();
    if (ret < 0) WARN( "client-surface compositor poll failed, error %d\n", errno );
}

static void quiesce_client_surface_compositor_target( struct client_surface_compositor_target *target )
{
    target->quiescing = TRUE;
    detach_client_surface_output_transform( target );
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
    const struct client_surface_compositor_queue *queue = target->notifications->queue;
    unsigned int i;

    if (target->copy_frame || target->native_updates) return FALSE;
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        if (target->frames[i].serial) return FALSE;
    if (!until)
    {
        BOOL incoming;

        pthread_mutex_lock( &client_surface_compositor_mutex );
        incoming = !!queue->incoming;
        pthread_mutex_unlock( &client_surface_compositor_mutex );
        if (incoming) return FALSE;
    }
    return (!queue->head || (until && queue->head->sequence >= until->sequence)) &&
           (!queue->control_head || (until && queue->control_head->sequence >= until->sequence));
}

static BOOL execute_client_surface_compositor_job( struct client_surface_compositor_job *job )
{
    if (job->op == CLIENT_SURFACE_COMPOSITOR_CREATE_POOL)
        return create_client_surface_output_allocation( job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_BEGIN_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_TRY_BEGIN_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_END_UPDATE)
    {
        struct client_surface_compositor_target *target =
            find_client_surface_compositor_target( job->toplevel );

        if (!target || (job->notifications && target->notifications != job->notifications)) return FALSE;
        if (job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE ||
            job->op == CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE)
        {
            if (target->deferred_update != job->u.update.mark || !target->update_notified) return FALSE;
            if (job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE)
            {
                if (target->update_resumed) return FALSE;
                target->update_resumed = TRUE;
                job->u.update.types = target->deferred_update_types | X11DRV_CLIENT_SURFACE_UPDATE_STATE;
                target->deferred_update_types = 0;
                job->u.update.notifications = target->notifications;
                pthread_mutex_lock( &client_surface_compositor_mutex );
                assert( !target->notifications->finish_serial && !target->notifications->finish_queued );
                ++target->notifications->refs;
                target->notifications->finish_serial = job->u.update.mark;
                target->notifications->finish.u.update.mark = job->u.update.mark;
                pthread_mutex_unlock( &client_surface_compositor_mutex );
            }
            else
            {
                if (!target->deferred_update_types) target->deferred_update = 0;
                target->update_notified = FALSE;
                target->update_resumed = FALSE;
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
                target->deferred_update_types |= job->u.update.types;
                job->u.update.deferred = TRUE;
                TRACE( "deferring native state update %s for %p\n",
                       wine_dbgstr_longlong( target->deferred_update ), target->toplevel );
                return FALSE;
            }
        }
        if (job->op != CLIENT_SURFACE_COMPOSITOR_END_UPDATE)
        {
            target->deferred_update_types &= ~job->u.update.types;
            ++target->native_updates;
            job->u.update.notifications = target->notifications;
            pthread_mutex_lock( &client_surface_compositor_mutex );
            ++target->notifications->refs;
            pthread_mutex_unlock( &client_surface_compositor_mutex );
            target->quiescing = TRUE;
            if (job->u.update.invalidate_scene) target->scene.valid = FALSE;
        }
        else
        {
            assert( job->u.update.count && target->native_updates >= job->u.update.count );
            target->native_updates -= job->u.update.count;
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
    if (job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_CACHE)
    {
        struct client_surface_compositor_binding *binding;
        BOOL cached = FALSE;

        /* Only completed owner images can support a repair. Keep this
         * inventory on the actor, independent of the current layout
         * and native output ownership. A positive hint still needs the
         * complete scene, binding and image checks in the actual repair. */
        for (binding = client_surface_compositor_bindings; binding; binding = binding->next)
            if (binding->toplevel == job->toplevel && binding->latest_image.pixmap)
            {
                cached = TRUE;
                break;
            }
        TRACE( "owner cache probe hwnd %p cached %u\n", job->toplevel, cached );
        return cached;
    }
    if (job->op == CLIENT_SURFACE_COMPOSITOR_REPAIR_OWNER)
        return repair_client_surface_compositor_owner( job->toplevel, FALSE );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_RESOLVE_SOURCES)
        return repair_client_surface_compositor_owner( job->toplevel, TRUE );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_SWEEP_HANDOFFS)
        return sweep_client_surface_compositor_handoffs( job->toplevel, job->u.scene_install.mark,
                                                          job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_UPDATE_TARGET)
        return update_client_surface_compositor_target( job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_COPY_POOL)
        return copy_client_surface_compositor_pool( job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_REPLACE_POOL)
        return replace_client_surface_compositor_pool( job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_REMOVE_TARGET)
        return remove_client_surface_compositor_target( job->toplevel );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_RETIRE_POOL)
        return retire_client_surface_compositor_pool( job->toplevel );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_DIRECT_PLAN)
        return install_client_surface_direct_plan( job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_RENEW_DIRECT)
        return renew_client_surface_direct_plan( job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_DIRECT_COMPLETE)
        return complete_client_surface_direct_plan( job );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_RESTORE_TARGET)
        return restore_client_surface_compositor_target( job );
    if (!client_surface_compositor_open()) return FALSE;
    switch (job->op)
    {
    case CLIENT_SURFACE_COMPOSITOR_FREE_POOL:
        return release_client_surface_output_allocation( job->u.retired_pixmaps );
    case CLIENT_SURFACE_COMPOSITOR_PRESENT:
        return client_surface_present_on_compositor( job );
    case CLIENT_SURFACE_COMPOSITOR_COPY:
        break;
    default:
        return FALSE;
    }
    return client_surface_copy_on_compositor( job->u.copy.source, job->u.copy.destination,
                                              job->u.copy.source_x, job->u.copy.source_y,
                                              job->u.copy.destination_x, job->u.copy.destination_y,
                                              job->u.copy.width, job->u.copy.height );
}

static struct client_surface_compositor_target *client_surface_compositor_job_target(
    const struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_target *target = job->queue->target;
    unsigned int i;

    if (target && job->notifications && target->notifications != job->notifications) return NULL;
    /* A detached old pool shares routing order with its HWND, but it need
     * not wait for native reads of a replacement pool on the new target. */
    if (target && job->op == CLIENT_SURFACE_COMPOSITOR_FREE_POOL)
    {
        for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
            if (target->frames[i].pixmap && (target->frames[i].pixmap == job->u.retired_pixmaps[0] ||
                                           target->frames[i].pixmap == job->u.retired_pixmaps[1])) return target;
        return NULL;
    }
    return target;
}

static BOOL client_surface_compositor_job_ready( struct client_surface_compositor_job *job,
                                                struct client_surface_compositor_target *target,
                                                BOOL *rejected )
{
    unsigned int i;
    BOOL drain = FALSE;

    *rejected = FALSE;
    if (job->op == CLIENT_SURFACE_COMPOSITOR_CREATE_POOL || job->op == CLIENT_SURFACE_COMPOSITOR_COPY_POOL) return TRUE;
    if (!target) return TRUE;
    if (job->op == CLIENT_SURFACE_COMPOSITOR_DIRECT_PLAN ||
        job->op == CLIENT_SURFACE_COMPOSITOR_RETIRE_POOL)
    {
        BOOL current = job->op == CLIENT_SURFACE_COMPOSITOR_DIRECT_PLAN ?
            client_surface_direct_plan_current( job, target ) :
            client_surface_compositor_pool_retirable( target );

        if (!current)
        {
            target->quiescing = target->native_updates || target->deferred_update;
            *rejected = TRUE;
            return TRUE;
        }
        /* Admission may fail after native completion or a server mutation.
         * Pause new work, but preserve assembly/mailbox ownership until the
         * exact scene RPC accepts. Other native barriers remain independent. */
        target->quiescing = TRUE;
        detach_client_surface_output_transform( target );
        if (target->copy_frame) return FALSE;
        for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
            if (target->frames[i].serial) return FALSE;
        return TRUE;
    }
    /* These probes never mutate native geometry or consume an output. They
     * must answer while that target's older Present work is still pending. */
    if (job->op == CLIENT_SURFACE_COMPOSITOR_TRY_BEGIN_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_END_UPDATE) return TRUE;
    /* The copy uses immutable scene/binding references. Inspecting those
     * references, or resolving source availability from completed owner
     * caches, neither mutates nor releases its output. A queued resolve can
     * arrive after replay resolved the inventory and started an assembly;
     * making the GUI wait for that assembly would block its next scene
     * change behind native completion. The resolver keeps the exact scene
     * and receipt checks, including its already-resolved no-op. */
    if (job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_SCENE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_CACHE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_RESOLVE_SOURCES) return TRUE;
    /* Expose restoration records its own deferred work if necessary. */
    if (job->op == CLIENT_SURFACE_COMPOSITOR_RESTORE_TARGET) return TRUE;
    /* Unlike checked XCB copies, transforms own every native input and their
     * completion record. Mutation/removal detaches adoption, not native work. */
    detach_client_surface_output_transform( target );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_COPY || job->op == CLIENT_SURFACE_COMPOSITOR_PRESENT)
    {
        Pixmap source = job->op == CLIENT_SURFACE_COMPOSITOR_COPY ? job->u.copy.source : job->u.present.source;
        struct client_surface_compositor_frame *frame = get_client_surface_compositor_pixmap( target, source );

        if (frame && (!frame->revision || client_surface_cache_write_pending( frame->image )))
        {
            /* PREPARE/snapshot and its nonzero generation protect GUI
             * publication. Expose fallback can still encounter a cancelled
             * old backing: refuse that read and let its normal repaint run. */
            TRACE_(csperf)( "ticks=%llu event=output_read_reject hwnd=%p op=%u source=%lx revision=%llu\n",
                           client_surface_perf_time(), target->toplevel, job->op, source,
                           (unsigned long long)frame->revision );
            *rejected = TRUE;
            return TRUE;
        }
    }
    if (job->op == CLIENT_SURFACE_COMPOSITOR_COPY)
        for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
            if (target->frames[i].pixmap == job->u.copy.destination &&
                !client_surface_compositor_frame_writable( &target->frames[i] ))
            {
                /* The GUI must COW a shared spare, not wait behind another
                 * actor request before learning that this image is leased. */
                job->u.copy.shared = TRUE;
                *rejected = TRUE;
                return TRUE;
            }
    /* Preserve the scene, binding and frame referenced by the request. Only
     * this target waits; jobs for independent targets remain eligible. */
    if (target->copy_frame) return FALSE;
    if (job->op == CLIENT_SURFACE_COMPOSITOR_REMOVE_TARGET ||
        job->op == CLIENT_SURFACE_COMPOSITOR_BEGIN_UPDATE ||
        job->op == CLIENT_SURFACE_COMPOSITOR_REPLACE_POOL) drain = TRUE;
    if (job->op == CLIENT_SURFACE_COMPOSITOR_UPDATE_TARGET)
        drain = target->window != job->u.pool.destination ||
                target->window_width != job->u.pool.window_width || target->window_height != job->u.pool.window_height ||
                !((target->frames[0].pixmap == job->u.pool.pixmaps[0] && target->frames[1].pixmap == job->u.pool.pixmaps[1]) ||
                  (target->frames[1].pixmap == job->u.pool.pixmaps[0] && target->frames[0].pixmap == job->u.pool.pixmaps[1]));
    if (drain)
    {
        /* Stop producing work for this target while its previous native
         * scene drains. Other targets remain eligible in the same loop. */
        if (job->op == CLIENT_SURFACE_COMPOSITOR_UPDATE_TARGET ||
            job->op == CLIENT_SURFACE_COMPOSITOR_REPLACE_POOL)
            /* Allocation may still fail. Preserve the pending publication
             * until the replacement pool has been prepared successfully. */
            target->quiescing = TRUE;
        else
            quiesce_client_surface_compositor_target( target );
    }
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
    {
        struct client_surface_compositor_frame *frame = &target->frames[i];

        if (!frame->serial) continue;
        if (drain ||
            (job->op == CLIENT_SURFACE_COMPOSITOR_COPY && frame->pixmap == job->u.copy.destination) ||
            (job->op == CLIENT_SURFACE_COMPOSITOR_PRESENT && frame->pixmap == job->u.present.source) ||
            (job->op == CLIENT_SURFACE_COMPOSITOR_FREE_POOL &&
             (frame->pixmap == job->u.retired_pixmaps[0] || frame->pixmap == job->u.retired_pixmaps[1]))) return FALSE;
    }
    return TRUE;
}

static void release_client_surface_compositor_job_resources( struct client_surface_compositor_job *job )
{
    /* Registration may fail validation, be cancelled before execution, or
     * reuse an existing pool. Only a newly created pool adopts the fd. The
     * section handle remains borrowed from the synchronous caller. */
    if (job->op == CLIENT_SURFACE_COMPOSITOR_REGISTER_HANDOFF && job->u.registration.ready_fd >= 0)
    {
        close( job->u.registration.ready_fd );
        job->u.registration.ready_fd = -1;
    }
}

static void prepare_client_surface_notification( struct client_surface_compositor_job *job )
{
    struct client_surface_owner_notifications *notifications = job->notifications;

    if (!notifications) return;
    pthread_mutex_lock( &client_surface_compositor_mutex );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_END_UPDATE)
    {
        job->u.update.count = notifications->pending_ends;
        notifications->pending_ends = 0;
        assert( job->u.update.count );
    }
    else if (job->op == CLIENT_SURFACE_COMPOSITOR_DIRECT_COMPLETE)
    {
        assert( notifications->direct_pending );
        job->u.direct_complete = notifications->direct_proof;
        notifications->direct_pending = FALSE;
    }
    pthread_mutex_unlock( &client_surface_compositor_mutex );
}

/* The dispatcher has unlinked the node. Producers only change the pending
 * fields while it executes, so a concurrent notification can now requeue it
 * without overwriting the live job or losing its retained reference. */
static void finish_client_surface_notification( struct client_surface_compositor_job *job )
{
    struct client_surface_owner_notifications *notifications = job->notifications;
    BOOL unused;

    pthread_mutex_lock( &client_surface_compositor_mutex );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_END_UPDATE)
    {
        assert( notifications->refs >= job->u.update.count );
        notifications->refs -= job->u.update.count;
        if (notifications->pending_ends) enqueue_client_surface_compositor_job( job );
        else notifications->end_queued = FALSE;
    }
    else if (job->op == CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE)
    {
        notifications->finish_queued = FALSE;
        notifications->finish_serial = 0;
        --notifications->refs;
    }
    else
    {
        assert( job->op == CLIENT_SURFACE_COMPOSITOR_DIRECT_COMPLETE );
        if (notifications->direct_pending) enqueue_client_surface_compositor_job( job );
        else
        {
            notifications->direct_queued = FALSE;
            --notifications->refs;
        }
    }
    unused = !notifications->refs;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    if (unused)
    {
        release_client_surface_compositor_queue( notifications->queue );
        free_client_surface_compositor_release( &notifications->memory, notifications, 3, sizeof(*notifications) );
    }
}

static BOOL client_surface_compositor_control_job( const struct client_surface_compositor_job *job )
{
    return job->op == CLIENT_SURFACE_COMPOSITOR_CREATE_POOL ||
           job->op == CLIENT_SURFACE_COMPOSITOR_COPY_POOL ||
           job->op == CLIENT_SURFACE_COMPOSITOR_TRY_BEGIN_UPDATE ||
           job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE ||
           job->op == CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE ||
           job->op == CLIENT_SURFACE_COMPOSITOR_END_UPDATE ||
           job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_CACHE;
}

static void ready_client_surface_compositor_queue( struct client_surface_compositor_queue *queue )
{
    list_remove( &queue->entry );
    list_add_tail( &client_surface_compositor_ready, &queue->entry );
}

/* Inspect only a FIFO head. A started Present keeps this exact job until its
 * completion or caller deadline, independently of the output's native lease. */
static BOOL step_client_surface_compositor_job( struct client_surface_compositor_job *job,
                                               BOOL *progressed )
{
    struct client_surface_compositor_target *target = client_surface_compositor_job_target( job );
    BOOL rejected;

    if (job->op == CLIENT_SURFACE_COMPOSITOR_PRESENT && job->u.present.started)
    {
        if (!job->u.present.done && NtGetTickCount() - job->u.present.start >= 5000)
        {
            unsigned int i;

            if (target)
                for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
                    if (target->frames[i].waiter == job) target->frames[i].waiter = NULL;
            job->u.present.done = TRUE;
            job->result = FALSE;
        }
        return job->u.present.done;
    }
    if (!client_surface_compositor_job_ready( job, target, &rejected )) return FALSE;
    prepare_client_surface_notification( job );
    job->result = !rejected && execute_client_surface_compositor_job( job );
    *progressed = TRUE;
    if (job->op == CLIENT_SURFACE_COMPOSITOR_DIRECT_PLAN ||
        job->op == CLIENT_SURFACE_COMPOSITOR_RETIRE_POOL)
    {
        target = client_surface_compositor_job_target( job );
        if (target) target->quiescing = target->native_updates || target->deferred_update;
    }
    return job->op != CLIENT_SURFACE_COMPOSITOR_PRESENT || !job->u.present.started || job->u.present.done;
}

static BOOL process_client_surface_compositor_jobs(void)
{
    struct client_surface_compositor_queue *queue;
    struct client_surface_compositor_job *incoming, **tail = &incoming, *job;
    unsigned int admitted = 0, scanned = 0, completed = 0, budget = 64;
    BOOL progressed = FALSE, more;

    /* Bound ingestion as well as execution. The next iteration resumes from
     * the same incoming head, without traversing any parked queue's jobs. */
    pthread_mutex_lock( &client_surface_compositor_mutex );
    while (admitted < 64 && (job = client_surface_compositor_head))
    {
        client_surface_compositor_head = job->next;
        *tail = job;
        tail = &job->next;
        --job->queue->incoming;
        ++admitted;
    }
    if (!client_surface_compositor_head) client_surface_compositor_tail = &client_surface_compositor_head;
    more = !!client_surface_compositor_head;
    *tail = NULL;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    while ((job = incoming))
    {
        incoming = job->next;
        job->next = NULL;
        queue = job->queue;
        if (client_surface_compositor_control_job( job ))
        {
            *queue->control_tail = job;
            queue->control_tail = &job->next;
        }
        else
        {
            *queue->tail = job;
            queue->tail = &job->next;
        }
        ready_client_surface_compositor_queue( queue );
    }

    while (budget && !list_empty( &client_surface_compositor_ready ))
    {
        BOOL done, control;

        queue = LIST_ENTRY( list_head( &client_surface_compositor_ready ), struct client_surface_compositor_queue, entry );
        list_remove( &queue->entry );
        list_init( &queue->entry );
        control = queue->control_head && (!queue->head || queue->control_head->sequence < queue->head->sequence);
        job = control ? queue->control_head : queue->head;
        assert( job );
        --budget;
        ++scanned;
        done = step_client_surface_compositor_job( job, &progressed );
        /* Preserve the old bypass contract: only these control operations
         * may pass a blocked normal head. Their own FIFO remains ordered. */
        if (!done && !control && queue->control_head)
        {
            if (!budget)
            {
                ready_client_surface_compositor_queue( queue );
                break;
            }
            control = TRUE;
            job = queue->control_head;
            --budget;
            ++scanned;
            done = step_client_surface_compositor_job( job, &progressed );
        }
        if (!done)
        {
            list_add_tail( &client_surface_compositor_parked, &queue->entry );
            continue;
        }
        if (control)
        {
            if (!(queue->control_head = job->next)) queue->control_tail = &queue->control_head;
        }
        else if (!(queue->head = job->next)) queue->tail = &queue->head;
        if (queue->head || queue->control_head) ready_client_surface_compositor_queue( queue );
        ++completed;
        progressed = TRUE;
        release_client_surface_compositor_job_resources( job );
        if (job->async)
        {
            if (job->notifications) finish_client_surface_notification( job );
            else
            {
                struct client_surface_output_allocation *allocation =
                    CONTAINING_RECORD( job, struct client_surface_output_allocation, release );

                assert( job->op == CLIENT_SURFACE_COMPOSITOR_FREE_POOL );
                release_client_surface_compositor_queue( allocation->release.queue );
                free_client_surface_compositor_release( &allocation->memory, allocation, 1, sizeof(*allocation) );
            }
        }
        else
        {
            struct client_surface_compositor_request *request =
                CONTAINING_RECORD( job, struct client_surface_compositor_request, job );

            pthread_mutex_lock( &client_surface_compositor_mutex );
            job->complete = TRUE;
            pthread_cond_signal( &request->completed );
            pthread_mutex_unlock( &client_surface_compositor_mutex );
        }
        /* The enqueue reference also covers dispatcher bookkeeping after a
         * synchronous caller returns or the final notification frees itself. */
        release_client_surface_compositor_queue( queue );
    }
    TRACE_(csperf)( "ticks=%llu event=compositor_queue_scan ingested=%u inspected=%u completed=%u more=%u runnable=%u\n",
                   client_surface_perf_time(), admitted, scanned, completed, more,
                   !list_empty( &client_surface_compositor_ready ) );
    return progressed || more || !list_empty( &client_surface_compositor_ready );
}

static void update_client_surface_compositor_timeout( struct client_surface_compositor_scan *scan,
                                                      DWORD now, unsigned int remaining )
{
    DWORD elapsed = now - scan->timeout_start;

    if (scan->timeout >= 0)
        scan->timeout = elapsed >= scan->timeout ? 0 : scan->timeout - elapsed;
    scan->timeout_start = now;
    if (scan->timeout < 0 || scan->timeout > remaining) scan->timeout = remaining;
}

static BOOL notify_client_surface_output_allocations( struct client_surface_compositor_scan *scan )
{
    struct client_surface_output_allocation *allocation;
    unsigned int budget;
    BOOL posted, pending, progressed = FALSE;

    pthread_mutex_lock( &client_surface_compositor_mutex );
    budget = min( client_surface_output_notification_count, 64 );
    while (budget-- && !list_empty( &client_surface_output_notifications ))
    {
        allocation = LIST_ENTRY( list_head( &client_surface_output_notifications ),
                                 struct client_surface_output_allocation, notification_entry );
        /* Queue the scalar server message and publish that fact atomically
         * with respect to its GUI receiver and cancellation. No native call
         * or window-data lookup runs while this metadata lock is held. */
        posted = TRUE;
        if (allocation->copy_wait)
        {
            struct client_surface_compositor_target *target =
                find_client_surface_compositor_target( allocation->release.toplevel );

            /* A gone/replaced source also wakes the GUI to discard the old
             * request. A live writer does not generate periodic GUI retries. */
            if (target && target->window == allocation->window && target->backing == allocation->source)
            {
                BOOL busy;

                client_surface_output_checkpoint_frame( target, allocation->source, &busy );
                posted = !busy;
            }
        }
        if (posted)
            posted = NtUserPostMessage( allocation->release.toplevel, WM_X11DRV_CLIENT_SURFACE_POOL,
                                        (UINT)allocation->serial, (UINT)(allocation->serial >> 32) );
        list_remove( &allocation->notification_entry );
        if (posted)
        {
            list_init( &allocation->notification_entry );
            --client_surface_output_notification_count;
        }
        else list_add_tail( &client_surface_output_notifications, &allocation->notification_entry );
        progressed |= posted;
    }
    pending = !!client_surface_output_notification_count;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    if (pending) update_client_surface_compositor_timeout( scan, NtGetTickCount(), 100 );
    return progressed;
}

static BOOL process_client_surface_compositor_targets( struct client_surface_compositor_scan *scan )
{
    struct client_surface_compositor_target *target;
    unsigned int inspected = 0, budget = CLIENT_SURFACE_COPY_BATCH_SIZE;
    BOOL progressed = FALSE;

    /* Insertion/removal changes the set to recheck, but never resets fair
     * traversal to the list head. Removal repairs the retained cursor before
     * freeing a target. A complete stable round is required before poll(). */
    if (scan->generation != client_surface_compositor_target_generation)
    {
        scan->generation = client_surface_compositor_target_generation;
        scan->remaining = client_surface_compositor_target_count;
        scan->timeout = -1;
    }
    while (scan->remaining && inspected < 64 && budget)
    {
        struct client_surface_compositor_job *job;
        DWORD now, elapsed;

        target = client_surface_compositor_next_target;
        assert( target );
        client_surface_compositor_next_target = target->next ? target->next : client_surface_compositor_targets;
        --scan->remaining;
        ++inspected;
        if (target->deferred_update && !target->update_notified)
        {
            quiesce_client_surface_compositor_target( target );
            if (client_surface_compositor_update_ready( target, NULL ))
            {
                /* Keep this exact target quiescent until the GUI consumes
                 * the token; a delayed notification cannot resume another. */
                target->update_notified = NtUserPostMessage( target->toplevel, WM_X11DRV_CLIENT_SURFACE_UPDATE,
                                                            (UINT)target->deferred_update,
                                                            (UINT)(target->deferred_update >> 32) );
                if (!target->update_notified)
                    WARN( "failed to notify deferred native update for %p\n", target->toplevel );
                progressed |= target->update_notified;
            }
        }
        progressed |= process_client_surface_compositor_restore( target );
        progressed |= replay_client_surface_scene_sources( target, &budget );
#ifdef SONAME_LIBXPRESENT
        flush_client_surface_compositor_mailbox( target );
#endif
        now = NtGetTickCount();
        if (target->shrink_start && !target->native_updates && !target->assembly_pending)
        {
            elapsed = now - target->shrink_start;
            if (elapsed >= 2000)
            {
                target->shrink_start = 0;
                NtUserPostMessage( target->toplevel, WM_WINE_UPDATEWINDOWSTATE,
                                   WINE_UPDATE_CLIENT_SURFACE_HANDOFFS, 0 );
                progressed = TRUE;
            }
            else update_client_surface_compositor_timeout( scan, now, 2000 - elapsed );
        }
        /* An in-flight Present owns its target until its FIFO head retires.
         * Collect its deadline here instead of traversing all parked queues. */
        job = target->notifications->queue->head;
        if (job && job->op == CLIENT_SURFACE_COMPOSITOR_PRESENT && job->u.present.started)
        {
            elapsed = now - job->u.present.start;
            if (job->u.present.done || elapsed >= 5000)
            {
                ready_client_surface_compositor_queue( target->notifications->queue );
                progressed = TRUE;
            }
            else update_client_surface_compositor_timeout( scan, now, 5000 - elapsed );
        }
    }
    TRACE_(csperf)( "ticks=%llu event=compositor_target_scan targets=%u inspected=%u replayed=%u remaining=%u progressed=%u generation=%s\n",
                   client_surface_perf_time(), client_surface_compositor_target_count, inspected,
                   CLIENT_SURFACE_COPY_BATCH_SIZE - budget, scan->remaining, progressed,
                   wine_dbgstr_longlong( scan->generation ) );
    return progressed;
}

static void client_surface_compositor_thread( void *context )
{
    BOOL armed = FALSE;

    (void)context;
    for (;;)
    {
        struct client_surface_compositor_scan scan =
        {
            .generation = client_surface_compositor_target_generation,
            .wake_serial = client_surface_compositor_wake_serial,
            .remaining = client_surface_compositor_target_count,
            .timeout = -1,
        };

        TRACE_(csperf)( "ticks=%llu event=compositor_scan_begin targets=%u armed=%u generation=%s\n",
                       client_surface_perf_time(), client_surface_compositor_target_count, armed,
                       wine_dbgstr_longlong( scan.generation ) );
        do
        {
            BOOL progressed;

            process_client_surface_present_events();
            progressed = client_surface_complete_queries( CLIENT_SURFACE_COPY_BATCH_SIZE );
            if (progressed) ++client_surface_compositor_query_generation;
            progressed |= client_surface_complete_cache( CLIENT_SURFACE_COPY_BATCH_SIZE );
            progressed |= process_client_surface_compositor_replies();
            if (progressed) wake_client_surface_compositor_queues();
            progressed |= process_client_surface_compositor_jobs();
            progressed |= process_client_surface_handoffs();
            progressed |= process_client_surface_compositor_targets( &scan );
            progressed |= notify_client_surface_output_allocations( &scan );
            scan.progressed |= progressed;
        } while (scan.remaining);
        /* Helpers acquiring output credit can consume events too. Count
         * those wakes even when their caller could not submit any work. */
        if (scan.progressed || scan.wake_serial != client_surface_compositor_wake_serial)
        {
            armed = FALSE;
            continue;
        }
        if (!armed)
        {
            arm_client_surface_compositor_work();
            armed = TRUE;
            continue;
        }
        wait_client_surface_compositor_work( &scan );
        armed = FALSE;
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

/* An admitted resource's release is infallible. This only links its existing
 * node under the metadata mutex; no native operation runs while it is held. */
static void enqueue_client_surface_compositor_job( struct client_surface_compositor_job *job )
{
    assert( client_surface_compositor_started );
    assert( job->queue );
    ++job->queue->refs;
    ++job->queue->incoming;
    job->sequence = ++client_surface_compositor_sequence;
    job->next = NULL;
    job->complete = FALSE;
    if (job->op == CLIENT_SURFACE_COMPOSITOR_PRESENT)
        job->u.present.started = job->u.present.done = FALSE;
    if (job->op == CLIENT_SURFACE_COMPOSITOR_TRY_BEGIN_UPDATE)
        job->u.update.deferred = FALSE;
    *client_surface_compositor_tail = job;
    client_surface_compositor_tail = &job->next;
    wake_client_surface_compositor();
}

/* The caller holds client_surface_compositor_mutex. Ownership of asynchronous
 * jobs passes to the actor only when they have been queued successfully. */
static BOOL queue_client_surface_compositor_job( struct client_surface_compositor_job *job )
{
    HANDLE thread;
    NTSTATUS status;

    job->complete = FALSE;
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
    enqueue_client_surface_compositor_job( job );
    return TRUE;
}

static struct client_surface_compositor_request *alloc_client_surface_compositor_request(
    const struct client_surface_compositor_job *job, struct client_surface_compositor_queue *queue )
{
    struct client_surface_compositor_request *request;
    struct client_surface_memory_scope memory = {0};
    BOOL barrier = job->op == CLIENT_SURFACE_COMPOSITOR_BEGIN_UPDATE ||
                   job->op == CLIENT_SURFACE_COMPOSITOR_TRY_BEGIN_UPDATE ||
                   job->op == CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE ||
                   job->op == CLIENT_SURFACE_COMPOSITOR_REMOVE_TARGET;

    pthread_mutex_lock( &client_surface_compositor_mutex );
    if (barrier)
    {
        /* These callers already synchronously protect native geometry or
         * drain a target. They cannot interpret capacity refusal as "there
         * was no target to protect". Their admitted queue owns one slot;
         * concurrent barriers serialize here instead of allocating jobs.
         * No release notification waits for this slot or uses its storage. */
        while (queue->barrier_in_use)
            pthread_cond_wait( &queue->barrier_available, &client_surface_compositor_mutex );
        queue->barrier_in_use = TRUE;
        request = &queue->barrier;
        pthread_mutex_unlock( &client_surface_compositor_mutex );
    }
    else
    {
        BOOL accepted = reserve_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_REQUEST_CAPACITY,
                                                                     1, sizeof(*request), queue );

        pthread_mutex_unlock( &client_surface_compositor_mutex );
        if (!accepted) return NULL;
        client_surface_memory_scope_copy( &memory, &queue->memory, TRUE );
        if (!(request = client_surface_alloc_scoped_metadata( &memory, 1, sizeof(*request) )))
        {
            client_surface_memory_scope_destroy( &memory );
            release_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_REQUEST_CAPACITY,
                                                        1, sizeof(*request), queue );
            return NULL;
        }
        request->memory = memory;
        if (pthread_cond_init( &request->completed, NULL ))
        {
            client_surface_free_owned_metadata( &request->memory, request, sizeof(*request) );
            release_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_REQUEST_CAPACITY,
                                                        1, sizeof(*request), queue );
            return NULL;
        }
    }
    request->job = *job;
    request->job.queue = queue;
    request->job.async = FALSE;
    return request;
}

static void free_client_surface_compositor_request( struct client_surface_compositor_request *request,
                                                    struct client_surface_compositor_queue *queue )
{
    if (request == &queue->barrier)
    {
        pthread_mutex_lock( &client_surface_compositor_mutex );
        assert( queue->barrier_in_use );
        queue->barrier_in_use = FALSE;
        pthread_cond_signal( &queue->barrier_available );
        pthread_mutex_unlock( &client_surface_compositor_mutex );
    }
    else
    {
        pthread_cond_destroy( &request->completed );
        client_surface_free_owned_metadata( &request->memory, request, sizeof(*request) );
        release_client_surface_compositor_capacity( CLIENT_SURFACE_COMPOSITOR_REQUEST_CAPACITY,
                                                    1, sizeof(*request), queue );
    }
}

static BOOL submit_client_surface_compositor_job( struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_queue *queue;
    struct client_surface_compositor_request *request;
    BOOL ret = FALSE;

    if (!(queue = get_client_surface_compositor_queue( job->toplevel ))) goto failed;
    if (!(request = alloc_client_surface_compositor_request( job, queue )))
    {
        release_client_surface_compositor_queue( queue );
        goto failed;
    }
    pthread_mutex_lock( &client_surface_compositor_mutex );
    if (queue_client_surface_compositor_job( &request->job ))
    {
        while (!request->job.complete)
            pthread_cond_wait( &request->completed,
                               &client_surface_compositor_mutex );
        ret = request->job.result;
    }
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    if (!request->job.complete) release_client_surface_compositor_job_resources( &request->job );
    job->u = request->job.u;
    job->result = request->job.result;
    job->complete = request->job.complete;
    free_client_surface_compositor_request( request, queue );
    release_client_surface_compositor_queue( queue );
    return ret;

failed:
    release_client_surface_compositor_job_resources( job );
    return FALSE;
}

/* Every notification already owns storage: an output allocation or a target
 * notification object. This boundary never allocates or waits for dispatch. */
static void post_client_surface_compositor_job( struct client_surface_compositor_job *job )
{
    struct client_surface_owner_notifications *notifications = job->notifications;

    if (job->op == CLIENT_SURFACE_COMPOSITOR_FREE_POOL)
    {
        struct client_surface_output_allocation *allocation;

        if (!job->u.retired_pixmaps[0] && !job->u.retired_pixmaps[1]) return;
        pthread_mutex_lock( &client_surface_compositor_mutex );
        for (allocation = client_surface_output_allocations; allocation; allocation = allocation->next)
            if ((allocation->pixmaps[0] == job->u.retired_pixmaps[0] && allocation->pixmaps[1] == job->u.retired_pixmaps[1]) ||
                (allocation->pixmaps[0] == job->u.retired_pixmaps[1] && allocation->pixmaps[1] == job->u.retired_pixmaps[0])) break;
        assert( allocation && !allocation->release.async );
        allocation->release.op = job->op;
        allocation->release.u.retired_pixmaps[0] = job->u.retired_pixmaps[0];
        allocation->release.u.retired_pixmaps[1] = job->u.retired_pixmaps[1];
        allocation->release.async = TRUE;
        enqueue_client_surface_compositor_job( &allocation->release );
        pthread_mutex_unlock( &client_surface_compositor_mutex );
        return;
    }
    assert( job->op == CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE ||
            job->op == CLIENT_SURFACE_COMPOSITOR_END_UPDATE ||
            job->op == CLIENT_SURFACE_COMPOSITOR_DIRECT_COMPLETE );
    pthread_mutex_lock( &client_surface_compositor_mutex );
    if (job->op == CLIENT_SURFACE_COMPOSITOR_END_UPDATE)
    {
        assert( notifications && notifications->refs && notifications->pending_ends < ~0u );
        ++notifications->pending_ends;
        if (!notifications->end_queued)
        {
            notifications->end_queued = TRUE;
            enqueue_client_surface_compositor_job( &notifications->end );
        }
    }
    else if (job->op == CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE)
    {
        if (notifications && notifications->finish_serial == job->u.update.mark && !notifications->finish_queued)
        {
            notifications->finish_queued = TRUE;
            enqueue_client_surface_compositor_job( &notifications->finish );
        }
    }
    else
    {
        for (notifications = client_surface_owner_notifications; notifications; notifications = notifications->next)
            if (notifications->toplevel == job->toplevel) break;
        if (notifications && notifications->direct_plan.identity == job->u.direct_complete.identity &&
            notifications->direct_plan.scene_epoch == job->u.direct_complete.scene_epoch &&
            notifications->direct_plan.source == job->u.direct_complete.source)
        {
            notifications->direct_proof = job->u.direct_complete;
            notifications->direct_pending = TRUE;
            if (!notifications->direct_queued)
            {
                notifications->direct_queued = TRUE;
                ++notifications->refs;
                enqueue_client_surface_compositor_job( &notifications->direct );
            }
        }
    }
    pthread_mutex_unlock( &client_surface_compositor_mutex );
}

BOOL X11DRV_client_surface_prepare_direct( struct client_surface *surface,
                                          const struct client_surface_scene *scene )
{
    struct x11drv_win_data *data;
    BOOL accepted;
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_DIRECT_PLAN,
        .toplevel = scene->toplevel,
        .u.direct_plan =
        {
            .scene_epoch = scene->epoch,
            .identity = ReadAcquire64( (LONG64 *)&surface->identity ),
            .source = impl_from_client_surface( surface )->window,
        },
    };

    if ((scene->generation && scene->generation != scene->epoch) ||
        !(data = get_win_data( scene->toplevel ))) return FALSE;
    job.u.direct_plan.destination = data->whole_window;
    release_win_data( data );
    /* The producer retains its drawable while this scalar plan is admitted.
     * Native attach follows on this thread inside the existing target update. */
    accepted = submit_client_surface_compositor_job( &job );
    TRACE( "DIRECT plan request hwnd %p scene %s identity %s accepted %u\n",
           scene->toplevel, wine_dbgstr_longlong( scene->epoch ), wine_dbgstr_longlong( job.u.direct_plan.identity ), accepted );
    return accepted;
}

void X11DRV_client_surface_complete_direct( struct client_surface *surface,
                                           const struct client_surface_frame *frame )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_DIRECT_COMPLETE,
        .toplevel = frame->scene.toplevel,
        .u.direct_complete =
        {
            .scene_epoch = frame->scene.epoch,
            .native_epoch = frame->target_epoch,
            .identity = ReadAcquire64( (LONG64 *)&surface->identity ),
            .source = impl_from_client_surface( surface )->window,
        },
    };

    /* A delayed scene ACK is not a native Present failure. There is no caller
     * result or native object access after the scalar proof is enqueued. */
    post_client_surface_compositor_job( &job );
}

static NTSTATUS client_surface_backing_copy_area( HWND toplevel, Drawable source, Drawable destination,
                                                  int source_x, int source_y,
                                                  int destination_x, int destination_y,
                                                  unsigned int width, unsigned int height )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_COPY,
        .toplevel = toplevel,
        .u.copy =
        {
            .source = source,
            .destination = destination,
            .source_x = source_x,
            .source_y = source_y,
            .destination_x = destination_x,
            .destination_y = destination_y,
            .width = width,
            .height = height,
        },
    };

    if (submit_client_surface_compositor_job( &job )) return STATUS_SUCCESS;
    return job.u.copy.shared ? STATUS_SHARING_VIOLATION : STATUS_UNSUCCESSFUL;
}

void X11DRV_client_surface_backing_cancel_allocation( struct x11drv_win_data *data )
{
    struct client_surface_output_allocation *allocation = data->client_surface_pending_allocation;
    BOOL pending;

    data->client_surface_pending_allocation = NULL;
    data->client_surface_allocation_serial = 0;
    if (!allocation) return;
    pthread_mutex_lock( &client_surface_compositor_mutex );
    allocation->abandoned = TRUE;
    pending = !!allocation->pending;
    if (!list_empty( &allocation->notification_entry ))
    {
        list_remove( &allocation->notification_entry );
        list_init( &allocation->notification_entry );
        --client_surface_output_notification_count;
    }
    TRACE_(csperf)( "ticks=%llu event=output_pair_create_cancel request=%p serial=%llu pending=%u checkpoint=%u waiting=%u\n",
                   client_surface_perf_time(), allocation, (unsigned long long)allocation->serial, allocation->pending,
                   allocation->checkpoint, allocation->copy_wait );
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    if (!pending) free_client_surface_pending_allocation( allocation );
}

UINT X11DRV_client_surface_backing_pool_ready( HWND hwnd, UINT64 serial )
{
    struct client_surface_output_allocation *allocation;
    struct x11drv_win_data *data;
    BOOL ready = FALSE;
    UINT update = 0;

    if (!(data = get_win_data( hwnd ))) return FALSE;
    if (serial && data->client_surface_allocation_serial == serial)
    {
        allocation = data->client_surface_pending_allocation;
        pthread_mutex_lock( &client_surface_compositor_mutex );
        ready = !allocation || (!allocation->pending && list_empty( &allocation->notification_entry ));
        pthread_mutex_unlock( &client_surface_compositor_mutex );
        if (ready)
        {
            update = data->client_surface_allocation_update;
        }
    }
    release_win_data( data );
    return update;
}

static NTSTATUS prepare_client_surface_output_allocation( struct x11drv_win_data *data,
                                                          unsigned int width, unsigned int height, BOOL force,
                                                          struct client_surface_output_allocation **result )
{
    struct client_surface_output_allocation *allocation = data->client_surface_pending_allocation;
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_CREATE_POOL,
        .toplevel = data->hwnd,
        .u.pool = {.width = width, .height = height, .depth = data->vis.depth},
    };
    NTSTATUS status;

    if (allocation && (allocation->width != width || allocation->height != height || allocation->depth != data->vis.depth))
    {
        X11DRV_client_surface_backing_cancel_allocation( data );
        allocation = NULL;
    }
    if (!allocation)
    {
        if (!submit_client_surface_compositor_job( &job )) return STATUS_UNSUCCESSFUL;
        data->client_surface_pending_allocation = job.u.pool.allocation;
        job.u.pool.allocation->force = force;
        data->client_surface_allocation_serial = job.u.pool.allocation->serial;
        data->client_surface_allocation_update = WINE_UPDATE_CLIENT_SURFACE_BACKING;
        return STATUS_PENDING;
    }
    pthread_mutex_lock( &client_surface_compositor_mutex );
    status = allocation->pending || !list_empty( &allocation->notification_entry ) ? STATUS_PENDING :
             allocation->failed ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    if (status == STATUS_PENDING) return status;
    data->client_surface_pending_allocation = NULL;
    if (status == STATUS_SUCCESS) *result = allocation;
    else free_client_surface_pending_allocation( allocation );
    return status;
}

static NTSTATUS replace_client_surface_backing( struct x11drv_win_data *data,
                                            struct client_surface_output_allocation *allocation, unsigned int width,
                                            unsigned int height, unsigned int window_width,
                                            unsigned int window_height, BOOL snapshot,
                                            unsigned int preserve_width, unsigned int preserve_height,
                                            Pixmap *first, Pixmap *second )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_REPLACE_POOL,
        .toplevel = data->hwnd,
        .u.pool =
        {
            .allocation = allocation,
            .source = snapshot ? 0 : data->client_surface_backing,
            .destination = data->whole_window,
            .width = width,
            .height = height,
            .depth = data->vis.depth,
            .window_width = window_width,
            .window_height = window_height,
            .copy_count = snapshot ? 1 : 2,
            .preserve_width = preserve_width,
            .preserve_height = preserve_height,
            .valid_width = preserve_width >= window_width && preserve_height >= window_height ? window_width : 0,
            .valid_height = preserve_width >= window_width && preserve_height >= window_height ? window_height : 0,
            .visual = data->vis.visualid,
        },
    };

    if (snapshot && (width < window_width || height < window_height))
    {
        free_client_surface_pending_allocation( allocation );
        return STATUS_UNSUCCESSFUL;
    }
    if (!snapshot && !allocation->checkpoint && preserve_width >= window_width &&
        preserve_height >= window_height && data->native_window &&
        client_surface_capture_scene_state( data->hwnd, &job.u.pool.scene ))
    {
        /* The legacy Window seed is completely overwritten by this valid
         * OUTPUT rectangle. Admit that owned read without touching Window. */
        release_client_surface_output_checkpoint( allocation );
        allocation->window_owner = x11drv_native_window_acquire( data->native_window );
        job.op = CLIENT_SURFACE_COMPOSITOR_COPY_POOL;
        if (submit_client_surface_compositor_job( &job ))
        {
            data->client_surface_pending_allocation = allocation;
            data->client_surface_allocation_update = WINE_UPDATE_CLIENT_SURFACE_BACKING;
            return STATUS_PENDING;
        }
        if (job.u.pool.invalid_source)
        {
            data->client_surface_backing_valid = FALSE;
            data->client_surface_backing_valid_width = data->client_surface_backing_valid_height = 0;
        }
        free_client_surface_pending_allocation( allocation );
        return job.u.pool.stale ? STATUS_RETRY : STATUS_UNSUCCESSFUL;
    }
    /* Complete the GUI connection's drawing before the actor reads it. */
    if (!allocation->checkpoint) XSync( data->display, False );
    if (!submit_client_surface_compositor_job( &job ))
    {
        /* A refusal before execution leaves the completed allocation here.
         * The actor clears this pointer when it consumes that ownership. */
        if (job.u.pool.allocation) free_client_surface_pending_allocation( job.u.pool.allocation );
        if (job.u.pool.invalid_source)
        {
            data->client_surface_backing_valid = FALSE;
            data->client_surface_backing_valid_width = data->client_surface_backing_valid_height = 0;
        }
        return job.u.pool.stale ? STATUS_RETRY : STATUS_UNSUCCESSFUL;
    }
    *first = job.u.pool.pixmaps[0];
    *second = job.u.pool.pixmaps[1];
    return STATUS_SUCCESS;
}

static void client_surface_backing_free( Pixmap first, Pixmap second )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_FREE_POOL,
        .u.retired_pixmaps = {first, second},
    };

    /* These are either unregistered allocations, or the prior synchronous
     * target removal/replacement drained and detached them. The actor owns
     * both XIDs and their accounting after enqueue; no GUI data is retained. */
    post_client_surface_compositor_job( &job );
}

static BOOL client_surface_backing_copy( HWND toplevel, Drawable source, Drawable destination,
                                         unsigned int width, unsigned int height )
{
    return client_surface_backing_copy_area( toplevel, source, destination, 0, 0, 0, 0,
                                             width, height ) == STATUS_SUCCESS;
}

static BOOL client_surface_backing_present( HWND toplevel, Window window, Pixmap pixmap,
                                            unsigned int width, unsigned int height )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_PRESENT,
        .toplevel = toplevel,
        .u.present =
        {
            .source = pixmap,
            .destination = window,
            .width = width,
            .height = height,
        },
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
        .toplevel = data->hwnd,
        .u.pool =
        {
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
            .visual = data->vis.visualid,
            .shrink_start = data->client_surface_backing_shrink_start,
        },
    };

    return submit_client_surface_compositor_job( &job );
}

static void remove_client_surface_backing_target( HWND toplevel )
{
    BOOL started;
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_REMOVE_TARGET,
        .toplevel = toplevel,
    };

    /* A plan can exist without output pixmaps, but an ordinary window's
     * destruction must not start a previously unused compositor. */
    pthread_mutex_lock( &client_surface_compositor_mutex );
    started = client_surface_compositor_started;
    pthread_mutex_unlock( &client_surface_compositor_mutex );
    if (started) submit_client_surface_compositor_job( &job );
}

struct client_surface_owner_notifications *X11DRV_client_surface_backing_begin_update(
    HWND hwnd, const struct window_rects *rects, UINT swp_flags, BOOL *deferred )
{
    const UINT no_geometry = SWP_NOSIZE | SWP_NOMOVE | SWP_NOCLIENTSIZE | SWP_NOCLIENTMOVE | SWP_NOZORDER;
    struct x11drv_win_data *data;
    BOOL backing, activation;
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_BEGIN_UPDATE,
        .toplevel = hwnd,
        .u.update =
        {
            .types = X11DRV_CLIENT_SURFACE_UPDATE_STATE |
                ((swp_flags & (WINE_SWP_CLIENT_SURFACE_BACKING_ENABLE | WINE_SWP_CLIENT_SURFACE_BACKING_DISABLE))
                 ? X11DRV_CLIENT_SURFACE_UPDATE_BACKING : 0) |
                ((swp_flags & WINE_SWP_CLIENT_SURFACE_PREPARE) ? X11DRV_CLIENT_SURFACE_UPDATE_PREPARE : 0),
        },
    };

    if (deferred) *deferred = FALSE;
    if (!(data = get_win_data( hwnd ))) return NULL;
    backing = !!data->client_surface_backing;
    activation = (swp_flags & WINE_SWP_CLIENT_SURFACE_BACKING_ENABLE) &&
                 !data->client_surface_backing_enabled;
    /* A state-only refresh does not change the plan's placement or clip.
     * The server roster/epoch check continues
     * to invalidate topology and producer changes. Be conservative for
     * fullscreen mappings, shape, frame and actual native geometry changes. */
    job.u.update.invalidate_scene = !rects || (swp_flags & no_geometry) != no_geometry ||
        (swp_flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW | SWP_FRAMECHANGED | SWP_STATECHANGED)) ||
        data->is_fullscreen || (swp_flags & WINE_SWP_FULLSCREEN) ||
        memcmp( &data->rects, rects, sizeof(*rects) );
    release_win_data( data );
    if (!backing) return NULL;

    /* Plain state notifications can be coalesced and reapplied from current
     * server state, including backing and preparation. A deferred prepare
     * returns STATUS_PENDING to win32u, which must not acknowledge it before
     * replay. A new backing activation retains its synchronous native publication
     * boundary. Repeated enables can coalesce with a pending disable while
     * the native backing is still enabled. */
    if (deferred && !job.u.update.invalidate_scene && !activation &&
        !(swp_flags & WINE_SWP_CLIENT_SURFACE_PUBLISH))
    {
        BOOL ret;

        job.op = CLIENT_SURFACE_COMPOSITOR_TRY_BEGIN_UPDATE;
        ret = submit_client_surface_compositor_job( &job );
        *deferred = job.u.update.deferred;
        return ret ? job.u.update.notifications : NULL;
    }

    /* A missing Present event can leave this target quiescing indefinitely.
     * Do not hold the process-wide window-data lock while it drains. Keep
     * only the handle across the wait; the caller must look up its data again.
     * Destroying the window also removes its compositor target. */
    return submit_client_surface_compositor_job( &job ) ? job.u.update.notifications : NULL;
}

UINT X11DRV_client_surface_backing_resume_update( HWND hwnd, UINT64 serial,
                                                 struct client_surface_owner_notifications **notifications )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE,
        .toplevel = hwnd,
        .u.update =
        {
            .mark = serial,
        },
    };

    *notifications = NULL;
    if (!submit_client_surface_compositor_job( &job )) return 0;
    *notifications = job.u.update.notifications;
    return job.u.update.types;
}

void X11DRV_client_surface_backing_finish_deferred_update( HWND hwnd, UINT64 serial,
                                                          struct client_surface_owner_notifications *notifications )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_FINISH_UPDATE,
        .notifications = notifications,
        .toplevel = hwnd,
        .u.update =
        {
            .mark = serial,
        },
    };

    /* Even if the server no longer needs a prepare or backing transition,
     * release this notification's hold after every state handler returned.
     * A handler which deferred again leaves its reasons for the next wake. */
    post_client_surface_compositor_job( &job );
}

NTSTATUS X11DRV_client_surface_backing_end_update( struct x11drv_win_data *data,
                                                   struct client_surface_owner_notifications *notifications,
                                                   NTSTATUS status )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_END_UPDATE,
        .notifications = notifications,
        .toplevel = notifications->toplevel,
    };

    /* The GUI connection owns native geometry, shape and staging. Its changes
     * finish while this target is quiescent, before the owner activates the
     * installed plan. No unrelated target participates in this barrier. */
    if (data)
    {
        X11DRV_sync_window_changes( data->display );
        if (status == STATUS_SUCCESS && data->client_surface_backing)
            status = X11DRV_client_surface_backing_ensure( data );
    }
    /* Return this exact target lifetime even if window data disappeared. An
     * obsolete release cannot resume a replacement target on the same HWND. */
    post_client_surface_compositor_job( &job );
    return status;
}

static BOOL register_client_surface_handoff( HWND toplevel,
                                             const struct client_surface_handoff_desc *desc,
                                             UINT64 mark )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_REGISTER_HANDOFF,
        .toplevel = toplevel,
        .u.registration =
        {
            .process = desc->process,
            .identity = desc->surface,
            .cookie = desc->cookie,
            .mark = mark,
            .window = wine_server_ptr_handle( desc->handle ),
            .ready_fd = -1,
        },
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
        req->require_producer = !desc->visible;
        status = wine_server_call( req );
        if (!status)
        {
            job.u.registration.mapping = wine_server_ptr_handle( reply->mapping );
            job.u.registration.view_size = reply->size;
            job.u.registration.offset = reply->offset;
            job.u.registration.mapping_id = reply->mapping_id;
            job.u.registration.cookie = reply->cookie;
        }
    }
    SERVER_END_REQ;
    if (status) return FALSE;
    SERVER_START_REQ( get_client_surface_handoff_event )
    {
        req->handle = desc->handle;
        req->producer = desc->process;
        req->surface = desc->surface;
        req->cookie = job.u.registration.cookie;
        req->owner = 1;
        status = wine_server_call( req );
        if (!status) event = wine_server_ptr_handle( reply->event );
    }
    SERVER_END_REQ;
    if (!status)
    {
        status = wine_server_handle_to_fd( event, FILE_READ_DATA, &job.u.registration.ready_fd, NULL );
        NtClose( event );
    }
    if (status) goto release;
    ret = submit_client_surface_compositor_job( &job );

release:
    NtClose( job.u.registration.mapping );
    if (ret) return TRUE;
    SERVER_START_REQ( release_client_surface_handoff )
    {
        req->handle = wine_server_user_handle( toplevel );
        req->producer = desc->process;
        req->surface = desc->surface;
        req->cookie = job.u.registration.cookie;
        req->owner = 1;
        wine_server_call( req );
    }
    SERVER_END_REQ;
    return FALSE;
}

static BOOL bind_client_surface_handoffs( HWND toplevel, const struct client_surface_memory_scope *memory,
                                          struct client_surface_handoff_desc *descs,
                                          UINT count, UINT64 mark )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_REUSE_HANDOFFS,
        .toplevel = toplevel,
        .u.reuse =
        {
            .handoffs = descs,
            .count = count,
            .mark = mark,
        },
    };
    BOOL *reused, ret = FALSE;
    UINT i;

    if (!count) return TRUE;
    if (!(reused = client_surface_alloc_owned_array( memory, count, sizeof(*reused) ))) return FALSE;
    job.u.reuse.reused = reused;
    if (!submit_client_surface_compositor_job( &job )) goto done;
    for (i = 0; i < count; ++i)
        /* A live producer can have unread frames or a capture already
         * waiting for storage. Hiding it does not cancel that obligation.
         * Unused hidden registrations still need no channel. */
        if ((descs[i].visible || descs[i].producer_mapped) && !reused[i] &&
            !register_client_surface_handoff( toplevel, &descs[i], mark )) goto done;
    ret = TRUE;
done:
    client_surface_free_owned_array( reused );
    return ret;
}

BOOL X11DRV_client_surface_bind_producers( HWND toplevel )
{
    struct client_surface_compositor_queue *queue;
    struct client_surface_scene_member *members = NULL;
    struct client_surface_handoff_desc *descs = NULL;
    UINT count = 0, live = 0, i;
    UINT64 scene = 0, mark = InterlockedIncrement64( (LONG64 *)&client_surface_compositor_mark );
    BOOL ret = FALSE;

    if (!mark) mark = InterlockedIncrement64( (LONG64 *)&client_surface_compositor_mark );
    if (!(queue = get_client_surface_compositor_queue( toplevel ))) return FALSE;
    if (!client_surface_get_scene_snapshot( toplevel, &queue->memory, &scene, &count, &members )) goto done;
    if (count && !(descs = client_surface_alloc_owned_array( &queue->memory, count, sizeof(*descs) ))) goto done;
    for (i = 0; i < count; ++i)
    {
        if (!members[i].producer_mapped) continue;
        descs[live++] = (struct client_surface_handoff_desc)
        {
            .handle = wine_server_user_handle( members[i].hwnd ),
            .process = members[i].process,
            .surface = members[i].identity,
            .cookie = members[i].cookie,
            .visible = members[i].visible,
            .producer_mapped = TRUE,
        };
    }
    /* Bind only transport here. Installing or sweeping a scene before the
     * native update barrier could alter an in-flight assembly. Registration
     * authenticates the current selected producer and channel lifetime; the
     * actor scans READY even without a visible layout or backing target. */
    if (live) qsort( descs, live, sizeof(*descs), compare_client_surface_handoff_descs );
    ret = bind_client_surface_handoffs( toplevel, &queue->memory, descs, live, mark );
done:
    client_surface_free_owned_array( descs );
    client_surface_free_scene_snapshot( count, members );
    release_client_surface_compositor_queue( queue );
    return ret;
}

static BOOL refresh_client_surface_handoffs( HWND toplevel )
{
    struct client_surface_compositor_queue *queue;
    struct client_surface_handoff_desc *descs = NULL;
    struct client_surface_scene_member *members = NULL;
    struct client_surface_scene_layout *layouts = NULL;
    struct client_surface_scene scene;
    UINT count = 0, i, index;
    unsigned int layout_count = 0;
    UINT64 scene_generation = 0;
    UINT64 mark;

    /* DIRECT admission already proves a sole selected producer. No roster
     * or clip is consumed below for this strategy; avoid building that
     * snapshot after every native update. Candidates and changed scenes
     * still follow the complete owner-plan path. */
    if (client_surface_get_toplevel_scene( toplevel, &scene ) &&
        scene.mode == CLIENT_SURFACE_PRESENTATION_DIRECT && scene.direct_candidate)
        return client_surface_scene_snapshot_current( toplevel, scene.epoch );

    mark = InterlockedIncrement64( (LONG64 *)&client_surface_compositor_mark );
    if (!mark) mark = InterlockedIncrement64( (LONG64 *)&client_surface_compositor_mark );
    if (!(queue = get_client_surface_compositor_queue( toplevel ))) return FALSE;
    if (!client_surface_get_scene_snapshot( toplevel, &queue->memory, &scene_generation, &count, &members )) goto failed;
    if (count == 1 && members[0].direct_candidate &&
        client_surface_get_toplevel_scene( toplevel, &scene ) &&
        scene.epoch == scene_generation && scene.mode == CLIENT_SURFACE_PRESENTATION_DIRECT)
    {
        /* Only an admitted attachment can replace the composition path.
         * A candidate still needs channels and an owner plan if the native
         * producer presents before preparation or admission can finish. */
        client_surface_free_scene_snapshot( count, members );
        release_client_surface_compositor_queue( queue );
        return client_surface_scene_snapshot_current( toplevel, scene_generation );
    }
    if (count && !(descs = client_surface_alloc_owned_array( &queue->memory, count, sizeof(*descs) ))) goto failed;
    for (i = 0; i < count; ++i)
    {
        descs[i].handle = wine_server_user_handle( members[i].hwnd );
        descs[i].process = members[i].process;
        descs[i].surface = members[i].identity;
        descs[i].cookie = members[i].cookie;
        descs[i].visible = members[i].visible;
        descs[i].producer_mapped = members[i].producer_mapped;
    }
    if (count) qsort( descs, count, sizeof(*descs), compare_client_surface_handoff_descs );
    {
        struct client_surface_compositor_job job =
        {
            .op = CLIENT_SURFACE_COMPOSITOR_CHECK_SCENE,
            .toplevel = toplevel,
            .u.scene_check =
            {
                .epoch = scene_generation,
                .handoffs = descs,
                .count = count,
            },
        };

        /* Backing ensure, snapshot and end-update can refresh the same plan
         * repeatedly. Its geometry and native clips are immutable at this
         * epoch. Reuse only a still-valid plan with the same live bindings;
         * native geometry and target changes keep their invalidation rules. */
        if (submit_client_surface_compositor_job( &job ))
        {
            if (!client_surface_scene_snapshot_current( toplevel, scene_generation )) goto failed;
            client_surface_free_owned_array( descs );
            client_surface_free_scene_snapshot( count, members );
            release_client_surface_compositor_queue( queue );
            return TRUE;
        }
    }
    for (i = 0; i < count; ++i) layout_count += !!descs[i].visible;
    if (layout_count && !(layouts = client_surface_alloc_owned_array( &queue->memory, layout_count, sizeof(*layouts) )))
    {
        layout_count = 0;
        goto failed;
    }
    /* Roster, selection, geometry and clips all come from the same reply.
     * The binding cache remains independent and includes hidden producers. */
    for (i = 0, index = 0; i < count; ++i)
    {
        DWORD size;

        if (!members[i].visible) continue;
        layouts[index].window = members[i].hwnd;
        layouts[index].process = members[i].process;
        layouts[index].identity = members[i].identity;
        layouts[index].geometry = members[i].target;
        if (!(size = X11DRV_GetRegionDataSize( members[i].region )) ||
            !(layouts[index].clip = client_surface_alloc_owned_array( &queue->memory, size, 1 )) ||
            !X11DRV_FillRegionData( members[i].region, 0, layouts[index].clip, size )) goto failed;
        ++index;
    }
    if (!bind_client_surface_handoffs( toplevel, &queue->memory, descs, count, mark )) goto failed;

    if (!client_surface_scene_snapshot_current( toplevel, scene_generation )) goto failed;
    {
        struct client_surface_compositor_job job =
        {
            .op = CLIENT_SURFACE_COMPOSITOR_SWEEP_HANDOFFS,
            .toplevel = toplevel,
            .u.scene_install =
            {
                .mark = mark,
                .epoch = scene_generation,
                .layouts = layouts,
                .count = layout_count,
            },
        };
        BOOL installed;

        installed = submit_client_surface_compositor_job( &job );
        layouts = job.u.scene_install.layouts;
        layout_count = job.u.scene_install.count;
        if (!installed) goto failed;
    }
    free_client_surface_scene_layouts( layouts, layout_count );
    client_surface_free_owned_array( descs );
    client_surface_free_scene_snapshot( count, members );
    release_client_surface_compositor_queue( queue );
    return TRUE;

failed:
    free_client_surface_scene_layouts( layouts, layout_count );
    client_surface_free_owned_array( descs );
    client_surface_free_scene_snapshot( count, members );
    release_client_surface_compositor_queue( queue );
    return FALSE;
}

static unsigned int client_surface_backing_extent( int size )
{
    unsigned int requested = min( max( size, 1 ), 65535 );

    return min( (requested + 63) & ~63u, 65535 );
}

BOOL X11DRV_RepairClientSurfaceOwner( HWND hwnd, BOOL resolve )
{
    struct client_surface_compositor_job job =
    {
        .op = resolve ? CLIENT_SURFACE_COMPOSITOR_RESOLVE_SOURCES : CLIENT_SURFACE_COMPOSITOR_REPAIR_OWNER,
        .toplevel = hwnd,
    };

    if (!resolve)
    {
        /* A cold cache needs the common producer-recovery path. Do not
         * build a scene or provision channels merely to discover that no
         * completed owner image exists. Source-resolution must still bind
         * and inspect channels, including newly completed producer images. */
        job.op = CLIENT_SURFACE_COMPOSITOR_CHECK_CACHE;
        if (!submit_client_surface_compositor_job( &job )) return FALSE;
        job.op = CLIENT_SURFACE_COMPOSITOR_REPAIR_OWNER;
    }
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
    X11DRV_client_surface_backing_cancel_allocation( data );
    remove_client_surface_backing_target( data->hwnd );
    if (data->client_surface_backing || data->client_surface_backing_spare)
    {
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

BOOL X11DRV_client_surface_backing_retire( struct x11drv_win_data *data )
{
    struct client_surface_compositor_job job =
    {
        .op = CLIENT_SURFACE_COMPOSITOR_RETIRE_POOL,
        .toplevel = data->hwnd,
    };

    X11DRV_client_surface_backing_cancel_allocation( data );
    if (!submit_client_surface_compositor_job( &job )) return FALSE;
    /* The old pair is now detached after Complete/Idle and the scene ACK.
     * Freeing it neither changes the native target nor starts another scene. */
    client_surface_backing_free( data->client_surface_backing, data->client_surface_backing_spare );
    data->client_surface_backing = data->client_surface_backing_spare = 0;
    data->client_surface_backing_width = data->client_surface_backing_height = 0;
    data->client_surface_backing_valid_width = data->client_surface_backing_valid_height = 0;
    data->client_surface_backing_shrink_start = 0;
    data->client_surface_backing_valid = FALSE;
    return TRUE;
}

NTSTATUS X11DRV_client_surface_prepare_owner( struct x11drv_win_data *data )
{
    struct client_surface_scene scene;
    NTSTATUS status;

    /* A sole retained native child can continue DIRECT after the owner and
     * producer applied the new geometry. Any Win32 child requires the normal
     * clipping/checkpoint path, even before its producer is registered. */
    client_surface_get_toplevel_scene( data->hwnd, &scene );
    if (!data->client_surface_backing && !data->client_surface_backing_spare &&
        !scene.valid && scene.direct_candidate && scene.epoch && !(scene.epoch & 1) &&
        !NtUserGetWindowRelative( data->hwnd, GW_CHILD ))
    {
        struct client_surface_compositor_job job =
        {
            .op = CLIENT_SURFACE_COMPOSITOR_RENEW_DIRECT,
            .toplevel = data->hwnd,
            .u.direct_renew =
            {
                .scene_epoch = scene.epoch,
                .destination = data->whole_window,
                .source_x = data->rects.client.left - data->rects.visible.left,
                .source_y = data->rects.client.top - data->rects.visible.top,
                .width = data->rects.client.right - data->rects.client.left,
                .height = data->rects.client.bottom - data->rects.client.top,
                .window_width = data->rects.visible.right - data->rects.visible.left,
                .window_height = data->rects.visible.bottom - data->rects.visible.top,
            },
        };

        X11DRV_sync_window_changes( data->display );
        if (submit_client_surface_compositor_job( &job ))
        {
            X11DRV_client_surface_backing_cancel_allocation( data );
            return STATUS_SUCCESS;
        }
    }
    /* Without an authenticated retained attachment, preserve GDI/background
     * pixels before the next producer can choose or fall back to composition. */
    status = X11DRV_client_surface_backing_snapshot( data, TRUE );
    if (status == STATUS_PENDING) data->client_surface_allocation_update = WINE_PREPARE_CLIENT_SURFACES;
    return status;
}

static NTSTATUS copy_client_surface_backing_snapshot( struct x11drv_win_data *data, Pixmap pixmap,
                                                  unsigned int width, unsigned int height,
                                                  unsigned int window_width, unsigned int window_height )
{
    if (width < window_width || height < window_height) return STATUS_UNSUCCESSFUL;
    /* The compositor uses another connection. Complete the owner's drawing
     * before copying the complete checkpoint, including its GDI pixels. */
    XSync( data->display, False );
    return client_surface_backing_copy_area( data->hwnd, data->whole_window, pixmap,
                                            0, 0, 0, 0, window_width, window_height );
}

static NTSTATUS snapshot_client_surface_backing( struct x11drv_win_data *data, BOOL invalidate, BOOL ensure_on_failure,
                                             unsigned int window_width, unsigned int window_height )
{
    Pixmap previous;
    NTSTATUS status;

    if ((status = copy_client_surface_backing_snapshot( data, data->client_surface_backing_spare,
            data->client_surface_backing_width, data->client_surface_backing_height,
            window_width, window_height )) != STATUS_SUCCESS)
    {
        /* A failed snapshot must still apply the capacity check's validity
         * and native extent to the old target. Successful snapshots combine
         * that update with their checkpoint rotation below. */
        if (status != STATUS_SHARING_VIOLATION && ensure_on_failure && update_client_surface_backing_target( data ))
            refresh_client_surface_handoffs( data->hwnd );
        return status;
    }
    previous = data->client_surface_backing;
    data->client_surface_backing = data->client_surface_backing_spare;
    data->client_surface_backing_spare = previous;
    TRACE( "rotated client-surface frame pool to %#lx (idle %#lx)\n",
           data->client_surface_backing, data->client_surface_backing_spare );
    data->client_surface_backing_valid = FALSE;
    data->client_surface_backing_valid_width = 0;
    data->client_surface_backing_valid_height = 0;
    if (!update_client_surface_backing_target( data )) return STATUS_UNSUCCESSFUL;
    refresh_client_surface_handoffs( data->hwnd );
    if (!invalidate)
    {
        data->client_surface_backing_valid = TRUE;
        data->client_surface_backing_valid_width = window_width;
        data->client_surface_backing_valid_height = window_height;
    }
    return STATUS_SUCCESS;
}

static BOOL client_surface_output_checkpoint_matches( struct x11drv_win_data *data,
                                                       struct client_surface_output_allocation *allocation,
                                                       unsigned int window_width, unsigned int window_height )
{
    return allocation->window_owner == data->native_window && allocation->window == data->whole_window &&
           allocation->source == data->client_surface_backing && data->client_surface_backing_valid &&
           allocation->window_width == window_width && allocation->window_height == window_height &&
           allocation->copy_width == min( data->client_surface_backing_valid_width, allocation->width ) &&
           allocation->copy_height == min( data->client_surface_backing_valid_height, allocation->height ) &&
           client_surface_output_checkpoint_scene_current( &allocation->scene );
}

/* Align capacities to tiles, with modest growth slack and delayed shrinking.
 * Only the owner
 * compositor uses these Pixmaps, so replaced pools can be freed once its
 * target update has drained the old Present requests. */
static NTSTATUS ensure_client_surface_backing( struct x11drv_win_data *data, BOOL snapshot, BOOL invalidate, BOOL force )
{
    struct client_surface_output_allocation *allocation;
    unsigned int width, height, window_width, window_height;
    unsigned int old_valid_width, old_valid_height;
    BOOL old_valid, valid, shrink;
    Pixmap pixmap, spare, old_pixmap, old_spare;
    NTSTATUS status;

retry:
    shrink = FALSE;
    if (!data->whole_window) return STATUS_UNSUCCESSFUL;
    if (!get_client_surface_window_extent( data, &window_width, &window_height )) return STATUS_UNSUCCESSFUL;
    width = client_surface_backing_extent( data->rects.visible.right - data->rects.visible.left );
    height = client_surface_backing_extent( data->rects.visible.bottom - data->rects.visible.top );
    allocation = data->client_surface_pending_allocation;
    if (allocation && (allocation->checkpoint || allocation->copy_wait) &&
        !client_surface_output_checkpoint_matches( data, allocation, window_width, window_height ))
    {
        X11DRV_client_surface_backing_cancel_allocation( data );
        allocation = NULL;
    }
    if (allocation && allocation->force) force = TRUE;
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
        !shrink && !force &&
        data->client_surface_backing_width >= width &&
        data->client_surface_backing_height >= height)
    {
        if (data->client_surface_pending_allocation) X11DRV_client_surface_backing_cancel_allocation( data );
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
        if (snapshot)
        {
            status = snapshot_client_surface_backing( data, invalidate, TRUE, window_width, window_height );
            if (status != STATUS_SHARING_VIOLATION) return status;
            /* The old request may already be cancelled, but its native read
             * still owns this spare. Replace the pair through normal bounded
             * admission; no wait or failed scene is needed for shared storage. */
            force = TRUE;
        }
        else
        {
            if (!update_client_surface_backing_target( data )) return STATUS_UNSUCCESSFUL;
            refresh_client_surface_handoffs( data->hwnd );
            return STATUS_SUCCESS;
        }
    }

    if (snapshot && (data->client_surface_backing || data->client_surface_backing_spare))
    {
        /* Replacing a live pool must retain its old valid intersection as
         * the published checkpoint, separate from the new GUI snapshot.
         * Keep that capacity-only replacement before rotating the snapshot;
         * the one-checkpoint shortcut below is only for an empty pool. */
        if ((status = ensure_client_surface_backing( data, FALSE, FALSE, force )) != STATUS_SUCCESS) return status;
        if (!get_client_surface_window_extent( data, &window_width, &window_height )) return STATUS_UNSUCCESSFUL;
        return snapshot_client_surface_backing( data, invalidate, FALSE, window_width, window_height );
    }

    /* Grow the axis which needs space, without retaining the historical
     * maximum of the other axis across alternating wide/tall resizes. */
    if (!shrink && width > data->client_surface_backing_width)
        width = max( width, client_surface_backing_extent( data->client_surface_backing_width * 9 / 8 ) );
    if (!shrink && height > data->client_surface_backing_height)
        height = max( height, client_surface_backing_extent( data->client_surface_backing_height * 9 / 8 ) );
    /* CREATE has no content identity. A resumed COPY instead retains its
     * exact source and scene; the matching check above precedes consumption. */
    if ((status = prepare_client_surface_output_allocation( data, width, height, force, &allocation )) != STATUS_SUCCESS)
        return status;
    old_valid = data->client_surface_backing_valid;
    old_valid_width = min( data->client_surface_backing_valid_width, width );
    old_valid_height = min( data->client_surface_backing_valid_height, height );

    /* Until a larger pool has been installed, the old Pixmap no longer
     * represents the complete host extent and must not satisfy Expose. */
    if (snapshot || !old_valid || old_valid_width < window_width || old_valid_height < window_height)
    {
        data->client_surface_backing_valid = FALSE;
        data->client_surface_backing_valid_width = 0;
        data->client_surface_backing_valid_height = 0;
    }

    status = replace_client_surface_backing( data, allocation, width, height, window_width, window_height,
                                             snapshot, !snapshot && old_valid ? old_valid_width : 0,
                                             !snapshot && old_valid ? old_valid_height : 0, &pixmap, &spare );
    if (status == STATUS_RETRY) goto retry;
    if (status != STATUS_SUCCESS) return status;

    /* The actor installed the checked checkpoint pair. Publish GUI ownership
     * only after success, so failure never requires restoring these handles. */
    old_pixmap = data->client_surface_backing;
    old_spare = data->client_surface_backing_spare;
    data->client_surface_backing = pixmap;
    data->client_surface_backing_spare = spare;
    data->client_surface_backing_width = width;
    data->client_surface_backing_height = height;
    data->client_surface_backing_shrink_start = 0;
    valid = !snapshot && old_valid && old_valid_width >= window_width && old_valid_height >= window_height;
    data->client_surface_backing_valid = valid;
    if (valid)
    {
        data->client_surface_backing_valid_width = window_width;
        data->client_surface_backing_valid_height = window_height;
    }
    if (snapshot)
        TRACE( "rotated client-surface frame pool to %#lx (idle %#lx)\n",
               data->client_surface_backing, data->client_surface_backing_spare );
    /* Successful installation drained and detached every old native user. */
    if (old_pixmap || old_spare) client_surface_backing_free( old_pixmap, old_spare );
    refresh_client_surface_handoffs( data->hwnd );
    if (snapshot && !invalidate)
    {
        data->client_surface_backing_valid = TRUE;
        data->client_surface_backing_valid_width = window_width;
        data->client_surface_backing_valid_height = window_height;
    }
    return STATUS_SUCCESS;
}

NTSTATUS X11DRV_client_surface_backing_ensure( struct x11drv_win_data *data )
{
    return ensure_client_surface_backing( data, FALSE, FALSE, FALSE );
}

NTSTATUS X11DRV_client_surface_backing_snapshot( struct x11drv_win_data *data, BOOL invalidate )
{
    return ensure_client_surface_backing( data, TRUE, invalidate, FALSE );
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
    if (!client_surface_backing_present( data->hwnd, data->whole_window, data->client_surface_backing,
                                         width, height ))
    {
        TRACE( "falling back to XCopyArea publication for pixmap %#lx\n",
               data->client_surface_backing );
        if (!client_surface_backing_copy( data->hwnd, data->client_surface_backing,
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
        .toplevel = data->hwnd,
        .u.restore =
        {
            .destination = window,
            .destination_x = rect->left,
            .destination_y = rect->top,
            .width = rect->right - rect->left,
            .height = rect->bottom - rect->top,
            .window_width = window_width,
            .window_height = window_height,
        },
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
    return client_surface_backing_copy_area( data->hwnd, data->client_surface_backing,
                                             data->whole_window,
                                             rect->left, rect->top,
                                             rect->left, rect->top,
                                             rect->right - rect->left,
                                             rect->bottom - rect->top ) == STATUS_SUCCESS;
}
