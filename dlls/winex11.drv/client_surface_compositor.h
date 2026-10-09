/*
 * GUI / compositor request and output ownership contract
 *
 * Copyright 2026 Wine contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_X11DRV_CLIENT_SURFACE_COMPOSITOR_H
#define __WINE_X11DRV_CLIENT_SURFACE_COMPOSITOR_H

#include "client_surface.h"
#include "client_surface_cache.h"
#include "wine/server_protocol.h"

/* GUI callers own request storage through synchronous return. Async releases
 * transfer already admitted storage. Queue and notification objects are opaque;
 * callers never take the actor mutex or manipulate its registries. */

struct client_surface_scene_layout
{
    HWND window;
    process_id_t process;
    UINT64 identity;
    struct client_surface_target geometry;
    RGNDATA *clip;
};

struct client_surface_owner_repair
{
    struct client_surface_handoff_receipt *receipts;
    UINT64 epoch, bindings, inventory, caches;
    enum { OWNER_REPAIR_NEW, OWNER_REPAIR_COLLECTING, OWNER_REPAIR_COMPLETE } phase;
    unsigned int index, count;
    BOOL resolve, result;
};

enum client_surface_compositor_op
{
    CLIENT_SURFACE_COMPOSITOR_REPLACE_POOL,
    CLIENT_SURFACE_COMPOSITOR_FREE_POOL,
    CLIENT_SURFACE_COMPOSITOR_PRESENT,
    CLIENT_SURFACE_COMPOSITOR_ADMIT_PRESENT,
    CLIENT_SURFACE_COMPOSITOR_REGISTER_HANDOFF,
    CLIENT_SURFACE_COMPOSITOR_REUSE_HANDOFFS,
    CLIENT_SURFACE_COMPOSITOR_CHECK_SCENE,
    CLIENT_SURFACE_COMPOSITOR_CHECK_CACHE,
    CLIENT_SURFACE_COMPOSITOR_REPAIR_OWNER,
    CLIENT_SURFACE_COMPOSITOR_RESOLVE_SOURCES,
    CLIENT_SURFACE_COMPOSITOR_SWEEP_HANDOFFS,
    CLIENT_SURFACE_COMPOSITOR_UPDATE_TARGET,
    CLIENT_SURFACE_COMPOSITOR_REMOVE_TARGET,
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
    CLIENT_SURFACE_COMPOSITOR_QUERY_WINDOW,
    CLIENT_SURFACE_COMPOSITOR_DROP_SEED,
    CLIENT_SURFACE_COMPOSITOR_COPY_POOL,
};

struct client_surface_direct_completion
{
    Drawable source;
    UINT64 identity, scene_epoch, native_epoch;
};

/* Embedded in already admitted request storage; the native callback owns
 * that storage until complete. The creator receipt and Window lease travel
 * with the observation, independently of the actor target. */
struct client_surface_window_query
{
    struct client_surface_native_work work;
    struct x11drv_native_window_read read;
    struct x11drv_native_window_read child_read;
    struct x11drv_native_window *child_owner;
    Window child;
    UINT64 direct_epoch, direct_identity;
    RECT direct_rect;
    unsigned int direct_width, direct_height;
    BOOL direct_checked;
    void (*finished)( struct client_surface_window_query *query );
    Window window;
    unsigned int width, height;
    int map_state, error;
    BOOL started, waiting, success;
    LONG complete;
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
    /* Actor-only continuation. Cursors are scalar keys: a handoff can close
     * and release its binding between dispatcher slices. */
    struct
    {
        unsigned int index, count, needed, found, bound;
        BOOL cursor_set;
        process_id_t process;
        UINT64 identity, generation;
        struct client_surface_compositor_binding **members;
        struct client_surface_handoff_receipt *receipts;
    } scan;
    union
    {
        struct client_surface_owner_repair repair;
        /* CREATE_POOL returns independently owned pending storage.
         * COPY_POOL admits an owned read of a completed OUTPUT rectangle.
         * REPLACE_POOL installs the checked owned copy.
         * UPDATE_TARGET borrows the registered pair and uses the same
         * installation payload as the replacement transaction. */
        struct
        {
            struct client_surface_output_allocation *allocation;
            struct x11drv_native_window *window_owner;
            Drawable source, destination;
            unsigned int width, height, window_width, window_height;
            unsigned int copy_count, preserve_width, preserve_height;
            unsigned int valid_width, valid_height, depth;
            UINT geometry_update;
            RECT geometry_rect;
            Pixmap pixmaps[2];
            VisualID visual;
            DWORD shrink_start;
            struct client_surface_scene scene;
            BOOL stale, invalid_source;
        } pool;
        /* FREE_POOL transfers its detached pair and accounting on enqueue. */
        Pixmap retired_pixmaps[2];
        /* PRESENT owns its GUI continuation; the target retains native
         * resources independently after cancellation or a caller deadline. */
        struct
        {
            Drawable source, destination;
            unsigned int width, height;
            struct client_surface_output_allocation *allocation;
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
            BOOL started;
        } scene_check;
        /* SWEEP_HANDOFFS adopts layouts only when installing the plan, then
         * clears this pointer/count. Failure, stale and unchanged plans leave
         * them with the synchronous caller, including all clip allocations. */
        struct
        {
            struct client_surface_scene_layout *layouts;
            unsigned int count;
            UINT64 epoch, mark;
            enum { SCENE_INSTALL_SWEEP, SCENE_INSTALL_ALLOCATE, SCENE_INSTALL_MEMBERS } phase;
        } scene_install;
        /* The producer retains source; destination has an owned native lease. */
        struct
        {
            Drawable source, destination;
            UINT64 identity, scene_epoch;
            struct x11drv_native_window *window_owner, *source_owner;
            struct client_surface_window_query query;
            enum { DIRECT_PLAN_ADMIT, DIRECT_PLAN_RETIRE_BINDINGS, DIRECT_PLAN_WAIT_CONTENT } phase;
        } direct_plan;
        /* RENEW_DIRECT consumes a completed, caller-owned observation. */
        struct
        {
            Drawable destination;
            UINT64 scene_epoch;
            const struct client_surface_window_query *query;
            int source_x, source_y;
            unsigned int width, height, window_width, window_height;
        } direct_renew;
        /* DIRECT_COMPLETE carries scalar attestations, no native lease. */
        struct client_surface_direct_completion direct_complete;
        /* Native-update operations carry scalar barriers and return values. */
        struct
        {
            struct client_surface_owner_notifications *notifications;
            UINT64 mark;
            unsigned int count;
            BOOL invalidate_scene, preserve_content;
            NTSTATUS status;
            UINT types;
        } update;
    } u;
};

struct client_surface_compositor_request
{
    struct client_surface_compositor_job job;
    pthread_cond_t completed;
};

/* The release node is part of the admitted output storage. The GUI can pass
 * ownership back without allocating or waiting for the actor. The registry
 * links are protected by compositor_mutex. The actor drops its independently
 * owned native images after draining their users; each image keeps its own
 * work node and accounting until native destruction actually completes. */
enum client_surface_output_phase
{
    CLIENT_SURFACE_OUTPUT_PAIR_CREATE,
    CLIENT_SURFACE_OUTPUT_PAIR_SCENE_WAIT,
    CLIENT_SURFACE_OUTPUT_PAIR_CHECKPOINT,
    CLIENT_SURFACE_OUTPUT_WINDOW_SCENE_WAIT,
    CLIENT_SURFACE_OUTPUT_WINDOW_QUERY,
    CLIENT_SURFACE_OUTPUT_WINDOW_COPY_WAIT,
    CLIENT_SURFACE_OUTPUT_WINDOW_PRESENT,
};

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
    /* Publication retains its completed OUTPUT through native completion. */
    struct client_surface_cache_image *source_image;
    struct x11drv_native_window *window_owner;
    struct x11drv_native_window_read seed_read;
    BOOL content_owned;
    UINT64 content_epoch;
    struct client_surface_window_query geometry_query;
    enum client_surface_output_phase phase;
    UINT geometry_update;
    UINT64 geometry_scope, geometry_revision;
    BOOL seed_target;
    struct list seed_entry;
    struct client_surface_scene scene;
    Pixmap source, published;
    Window window;
    UINT64 source_revision;
    unsigned int window_width, window_height, copy_width, copy_height;
    BOOL force;
    /* The GUI owns the pending pointer. The callbacks retain this object
     * until both native operations finish, even after the GUI abandons it.
     * These three fields use compositor_mutex; no callback touches win_data. */
    unsigned int pending;
    BOOL failed, abandoned;
    struct list notification_entry;
};

extern BOOL client_surface_output_waits_scene( const struct client_surface_output_allocation *allocation );
extern struct client_surface_compositor_queue *get_client_surface_compositor_queue( HWND toplevel );
extern void release_client_surface_compositor_queue( struct client_surface_compositor_queue *queue );
extern void release_client_surface_output_checkpoint( struct client_surface_output_allocation *allocation );
extern void free_client_surface_pending_allocation( struct client_surface_output_allocation *allocation );
extern int compare_client_surface_scene_layouts( const void *a, const void *b );
extern int compare_client_surface_handoff_descs( const void *a, const void *b );
extern void free_client_surface_scene_layouts( struct client_surface_scene_layout *layouts );
extern BOOL client_surface_output_checkpoint_scene_current( const struct client_surface_scene *expected );
extern BOOL submit_client_surface_compositor_request( struct client_surface_compositor_request *request );
extern void post_client_surface_compositor_job( struct client_surface_compositor_job *job );
extern void cancel_client_surface_output_request( struct client_surface_output_allocation *allocation );
extern void remove_client_surface_backing_target( HWND toplevel );
/* The returned scope is borrowed until the caller releases its queue reference. */
extern const struct client_surface_memory_scope *client_surface_compositor_queue_memory(
    const struct client_surface_compositor_queue *queue );
extern void end_client_surface_native_update( struct client_surface_owner_notifications *notifications,
                                              Display *display, struct x11drv_native_window *window );
/* Completion includes removal of the queued GUI notification. Only then may
 * the GUI consume the result or return the pending allocation's ownership. */
extern NTSTATUS client_surface_output_status( const struct client_surface_output_allocation *allocation );
extern void defer_client_surface_output_scene( struct client_surface_output_allocation *allocation );

extern void retire_client_surface_output( Pixmap first, Pixmap second );

#endif
