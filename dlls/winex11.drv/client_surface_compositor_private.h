/*
 * X11 compositor actor internals
 *
 * Copyright 2026 Wine contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_X11DRV_CLIENT_SURFACE_COMPOSITOR_PRIVATE_H
#define __WINE_X11DRV_CLIENT_SURFACE_COMPOSITOR_PRIVATE_H

#include <poll.h>

#include "client_surface_compositor.h"
#include "client_surface_query.h"
#include "wine/rbtree.h"
#include "wine/server_protocol.h"
#define CLIENT_SURFACE_COPY_BATCH_SIZE CLIENT_SURFACE_CACHE_TRANSFORM_LIMIT
#define CLIENT_SURFACE_COMPOSITOR_FRAME_COUNT 3
#define CLIENT_SURFACE_COMPOSITOR_MAX_INFLIGHT 2
#define CLIENT_SURFACE_COMPOSITOR_DAMAGE_HISTORY 64

extern pthread_mutex_t client_surface_compositor_mutex;
extern void wake_client_surface_compositor(void);

/* Only compositor, composition and output include this header. The actor owns
 * targets, source bindings and scene state. Registries and copy batches remain
 * private to their owner; the mutex protects cross-thread admission and native
 * completion, not rendering or blocking native operations. */

struct client_surface_compositor_pool
{
    struct client_surface_compositor_pool *next, **prev;
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
    unsigned int width, height, depth;
};

struct client_surface_cache_copy
{
    struct client_surface_geometry_query query;
    BOOL query_pending;
    BOOL native_pending;
    struct client_surface_handoff_slot frame;
    UINT64 control, started;
    unsigned int index;
};

struct client_surface_compositor_binding
{
    struct rb_entry registry_entry;
    struct client_surface_memory_scope memory;
    struct client_surface_compositor_pool *pool;
    struct client_surface_compositor_queue *queue;
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
    UINT64 replay_generation;
    unsigned int latest_index;
    struct client_surface_cache_copy cache_copy;
    BOOL retired;
};

struct client_surface_scene_plan
{
    enum { OWNER_COMPOSITE, DIRECT_ATTACH } strategy;
    UINT64 direct_identity;
    Window direct_drawable;
    struct x11drv_native_window *direct_owner;
    struct client_surface_compositor_binding **members;
    struct client_surface_scene_layout *layouts;
    unsigned int count;
    UINT64 epoch;
    BOOL valid;
};

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
    struct rb_entry pixmap_entry;
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
    struct client_surface_native_present native_present;
};

enum client_surface_update_phase
{
    CLIENT_SURFACE_UPDATE_UNPOSTED,
    CLIENT_SURFACE_UPDATE_POSTED,
    CLIENT_SURFACE_UPDATE_CONSUMED,
};

struct client_surface_compositor_target
{
    struct client_surface_compositor_target *next, **prev;
    struct rb_entry registry_entry;
    struct client_surface_memory_scope memory;
    struct client_surface_owner_notifications *notifications;
    HWND toplevel;
    Window window;
    Window present_window;
    BOOL content_redirected;
    UINT64 content_epoch;
    struct x11drv_native_window *window_owner;
    struct client_surface_native_present_queue native_presents;
    struct client_surface_compositor_frame frames[CLIENT_SURFACE_COMPOSITOR_FRAME_COUNT];
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
    UINT64 binding_generation;
    UINT64 cache_generation, inventory_generation, replay_generation;
    struct client_surface_owner_repair repair;
    struct client_surface_handoff_receipt *receipts;
    unsigned int received;
    unsigned int replay_member;
    BOOL mailbox_pending;
    BOOL quiescing;
    unsigned int native_updates;
    UINT64 seed_serial;
    UINT64 deferred_update;
    enum client_surface_update_phase update_phase;
    BOOL preserve_content;
    UINT deferred_update_types;
    struct client_surface_compositor_mailbox *mailbox;
    DWORD shrink_start;
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
    UINT64 generation, publication_generation, frame_revision;
    unsigned int buffer_index;
    unsigned int scene_index, width, height;
    BOOL replay;
    RECT damage;
    XRectangle clips[];
};

struct client_surface_binding_key
{
    HWND toplevel;
    process_id_t process;
    UINT64 identity;
};

struct client_surface_compositor_scan
{
    UINT64 generation, wake_serial, pool_generation;
    unsigned int remaining, handoff_remaining;
    unsigned int target_count, pool_count;
    DWORD timeout_start;
    int timeout;
    BOOL progressed;
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
    /* One barrier per admitted queue bypasses ordinary request pressure.
     * Its synchronous caller owns the storage until the actor completes it. */
    pthread_cond_t barrier_available;
    BOOL barrier_in_use;
    /* Removal is admitted with the queue, including before a target exists.
     * Coalesced removals retire intervening admissions after the FIFO marker
     * completes; later incarnations must not revive those old requests. */
    struct client_surface_compositor_job remove;
    UINT64 remove_through, retired_through;
    BOOL remove_queued;
};

enum client_surface_compositor_capacity_kind
{
    CLIENT_SURFACE_COMPOSITOR_REQUEST_CAPACITY,
    CLIENT_SURFACE_COMPOSITOR_QUEUE_CAPACITY,
    CLIENT_SURFACE_COMPOSITOR_RELEASE_CAPACITY,
    CLIENT_SURFACE_COMPOSITOR_CAPACITY_COUNT,
};

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
    struct client_surface_native_work native_end;
    struct x11drv_native_window_read end_read;
    Display *end_display;
    unsigned int native_ends, native_batch;
    unsigned int pending_ends;
    BOOL finish_queued, direct_queued, direct_pending;
    UINT64 finish_serial;
    struct client_surface_direct_completion direct_plan, direct_proof;
};

extern void reset_client_surface_owner_repair( struct client_surface_owner_repair *repair );
extern UINT64 client_surface_pixmap_bytes( unsigned int width, unsigned int height, unsigned int depth );
extern void set_client_surface_compositor_pixmap( struct client_surface_compositor_frame *frame, Pixmap pixmap,
                                                 struct client_surface_cache_image *image );
extern BOOL client_surface_compositor_frame_writable( const struct client_surface_compositor_frame *frame );
extern struct client_surface_compositor_binding *next_client_surface_compositor_binding(
    struct client_surface_compositor_binding *binding );
extern BOOL client_surface_frame_copy_pending( const struct client_surface_compositor_frame *frame );
extern struct client_surface_compositor_frame *get_client_surface_compositor_pixmap(
    struct client_surface_compositor_target *target, Pixmap pixmap );
extern void detach_client_surface_output_transform( struct client_surface_compositor_target *target );
extern void note_client_surface_compositor_damage(
    struct client_surface_compositor_target *target,
    struct client_surface_compositor_frame *frame, const RECT *rect );
extern void wake_client_surface_compositor_queues(void);
extern void release_client_surface_compositor_capacity( enum client_surface_compositor_capacity_kind kind,
                                                        unsigned int count, SIZE_T bytes,
                                                        struct client_surface_compositor_queue *queue );
extern void free_client_surface_compositor_release( struct client_surface_memory_scope *memory, void *data,
                                                    unsigned int count, SIZE_T bytes );
extern void *alloc_client_surface_compositor_metadata( HWND toplevel, SIZE_T size,
                                                        struct client_surface_memory_scope *memory );
extern void update_client_surface_notification_plan( struct client_surface_compositor_target *target );
extern void client_surface_output_allocation_complete( void *context, BOOL success );
extern void start_client_surface_window_query( struct client_surface_window_query *query,
                                               struct x11drv_native_window *window,
                                               void (*finished)( struct client_surface_window_query *query ) );
extern BOOL create_client_surface_window_query( struct client_surface_compositor_job *job );
extern BOOL create_client_surface_output_allocation( struct client_surface_compositor_job *job );
extern BOOL release_client_surface_output_allocation( const Pixmap pixmaps[2] );
extern unsigned int count_client_surface_compositor_frames(
    const struct client_surface_compositor_target *target );
extern BOOL process_client_surface_native_present( struct client_surface_compositor_target *target );
extern BOOL submit_client_surface_present( struct client_surface_compositor_target *target,
                                           struct client_surface_compositor_frame *frame,
                                           UINT64 publish_generation, UINT64 publish_epoch,
                                           const RECT *copy_rect, uint32_t *serial_ret,
                                           const struct client_surface_compositor_binding *binding,
                                           const RECT *damage );
extern void retry_client_surface_compositor_mailbox( struct client_surface_compositor_target *target );
extern void free_client_surface_compositor_mailbox( struct client_surface_compositor_target *target );
extern struct client_surface_compositor_frame *get_client_surface_compositor_frame(
    struct client_surface_compositor_target *target, BOOL preserve_backing );
extern void client_surface_handoff_wake_release( struct client_surface_handoff_shared *shared );
extern void finish_client_surface_compositor_assembly(
    struct client_surface_compositor_target *target, BOOL invalidate );
extern void invalidate_client_surface_compositor_assembly( struct client_surface_compositor_target *target );
extern void abort_client_surface_output_transform_assembly( struct client_surface_compositor_target *target,
                                                            struct client_surface_output_transform *transform );
extern struct client_surface_compositor_frame *acquire_client_surface_compositor_assembly_frame(
    struct client_surface_compositor_target *target );
extern struct client_surface_compositor_target *find_client_surface_compositor_target( HWND toplevel );
extern BOOL client_surface_present_on_compositor( struct client_surface_compositor_job *job );
extern BOOL register_client_surface_compositor_handoff(
    struct client_surface_compositor_job *job );
extern void update_client_surface_compositor_scene(
    struct client_surface_compositor_target *target, UINT64 scene_epoch );
extern struct client_surface_compositor_binding *next_client_surface_job_binding(
    const struct client_surface_compositor_job *job );
extern void advance_client_surface_job_binding( struct client_surface_compositor_job *job,
                                                const struct client_surface_compositor_binding *binding );
extern BOOL reuse_client_surface_compositor_handoffs( struct client_surface_compositor_job *job,
                                                      unsigned int *budget );
extern void free_client_surface_scene_plan( struct client_surface_compositor_target *target );
extern BOOL check_client_surface_compositor_scene( struct client_surface_compositor_job *job,
                                                   unsigned int *budget );
extern BOOL install_client_surface_scene_plan( struct client_surface_compositor_target *target,
                                               struct client_surface_compositor_job *job,
                                               unsigned int *budget );
extern BOOL sweep_client_surface_compositor_handoffs( struct client_surface_compositor_job *job,
                                                       UINT64 mark, unsigned int *budget );
extern void drain_client_surface_compositor_target(
    struct client_surface_compositor_target *target );
extern BOOL update_client_surface_compositor_target( struct client_surface_compositor_job *job );
extern void release_client_surface_seed( struct client_surface_output_allocation *allocation );
extern BOOL copy_client_surface_compositor_pool( struct client_surface_compositor_job *job );
extern BOOL process_client_surface_output_seeds( struct client_surface_compositor_scan *scan );
extern BOOL replace_client_surface_compositor_pool( struct client_surface_compositor_job *job );
extern BOOL remove_client_surface_compositor_target( HWND toplevel );
extern BOOL client_surface_compositor_pool_retirable( const struct client_surface_compositor_target *target );
extern BOOL retire_client_surface_compositor_pool( HWND toplevel );
extern BOOL client_surface_direct_plan_current( const struct client_surface_compositor_job *job,
                                                const struct client_surface_compositor_target *target );
extern BOOL install_client_surface_direct_plan( struct client_surface_compositor_job *job,
                                                unsigned int *budget, BOOL *done );
extern BOOL renew_client_surface_direct_plan( const struct client_surface_compositor_job *job );
extern BOOL complete_client_surface_direct_plan( const struct client_surface_compositor_job *job );
extern BOOL publish_client_surface_handoff_generation( HWND toplevel, UINT64 generation,
                                                       UINT64 scene_generation, BOOL success );
extern BOOL repair_client_surface_compositor_owner( HWND toplevel, BOOL resolve,
                                                     struct client_surface_owner_repair *repair );
extern BOOL process_client_surface_handoffs( struct client_surface_compositor_scan *scan );
extern BOOL replay_client_surface_scene_sources( struct client_surface_compositor_target *target,
                                                 unsigned int *budget );
extern void quiesce_client_surface_compositor_target( struct client_surface_compositor_target *target );
extern NTSTATUS client_surface_compositor_update_ready( struct client_surface_compositor_target *target,
                                                        const struct client_surface_compositor_job *until );
extern void ready_client_surface_compositor_queue( struct client_surface_compositor_queue *queue );
extern BOOL notify_client_surface_output_allocations( struct client_surface_compositor_scan *scan );
extern BOOL process_client_surface_compositor_targets( struct client_surface_compositor_scan *scan );
extern void enqueue_client_surface_compositor_job( struct client_surface_compositor_job *job );

extern BOOL admit_client_surface_publication( struct client_surface_compositor_job *job );

/* A scan owns only cursors/counts, never registry nodes. Each owner initializes
 * and validates its traversal before exposing the descriptors to the scheduler. */
extern void drain_client_surface_notification( int fd );
extern void arm_client_surface_sources(void);
extern void init_client_surface_source_scan( struct client_surface_compositor_scan *scan );
extern void init_client_surface_output_scan( struct client_surface_compositor_scan *scan );
extern unsigned int get_client_surface_source_waiters( const struct client_surface_compositor_scan *scan,
                                                       struct pollfd *waiters, unsigned int capacity );
extern unsigned int get_client_surface_output_waiters( const struct client_surface_compositor_scan *scan,
                                                       struct pollfd *waiters, unsigned int capacity );
extern BOOL begin_client_surface_composition(void);
extern BOOL finish_client_surface_composition(void);

extern BOOL init_client_surface_target_notifications( struct client_surface_compositor_target *target );
extern void destroy_client_surface_target_notifications( struct client_surface_compositor_target *target );

#endif
