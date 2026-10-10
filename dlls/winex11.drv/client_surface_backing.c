/*
 * GUI backing updates and compositor requests
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

#include "ntstatus.h"
#include "x11drv.h"
#include "client_surface.h"
#include "wine/server.h"
#include "client_surface_compositor.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);
WINE_DECLARE_DEBUG_CHANNEL(csperf);

static UINT64 client_surface_compositor_mark;

static NTSTATUS query_client_surface_extent( struct x11drv_win_data *data, UINT update,
                                             unsigned int *width, unsigned int *height );
static NTSTATUS ensure_client_surface_backing( struct x11drv_win_data *data, BOOL snapshot,
                                               BOOL invalidate, BOOL force, UINT update );

BOOL X11DRV_client_surface_prepare_direct( struct client_surface *surface,
                                          const struct client_surface_scene *scene )
{
    struct x11drv_win_data *data;
    BOOL accepted;
    struct client_surface_compositor_request request =
    { .job = {
        .op = CLIENT_SURFACE_COMPOSITOR_DIRECT_PLAN,
        .toplevel = scene->toplevel,
        .u.direct_plan =
        {
            .scene_epoch = scene->epoch,
            .identity = ReadAcquire64( (LONG64 *)&surface->identity ),
            .source = impl_from_client_surface( surface )->window,
            .source_owner = impl_from_client_surface( surface )->native_window,
        },
    } };

    if ((scene->generation && scene->generation != scene->epoch) ||
        !(data = get_win_data( scene->toplevel ))) return FALSE;
    request.job.u.direct_plan.destination = data->whole_window;
    request.job.u.direct_plan.window_owner = x11drv_native_window_acquire( data->native_window );
    release_win_data( data );
    if (!request.job.u.direct_plan.window_owner) return FALSE;
    /* The producer retains its drawable while this scalar plan is admitted.
     * Native attach follows on this thread inside the existing target update. */
    accepted = submit_client_surface_compositor_request( &request );
    x11drv_native_window_release( request.job.u.direct_plan.window_owner );
    TRACE( "DIRECT plan request hwnd %p scene %s identity %s accepted %u\n",
           scene->toplevel, wine_dbgstr_longlong( scene->epoch ), wine_dbgstr_longlong( request.job.u.direct_plan.identity ), accepted );
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

void X11DRV_client_surface_backing_cancel_allocation( struct x11drv_win_data *data )
{
    struct client_surface_output_allocation *allocation = data->client_surface_pending_allocation;

    data->client_surface_pending_allocation = NULL;
    data->client_surface_allocation_serial = 0;
    cancel_client_surface_output_request( allocation );
}

static void cancel_client_surface_geometry_request( struct client_surface_output_allocation **slot, UINT64 serial )
{
    struct client_surface_output_allocation *allocation = *slot;

    if (!allocation || (serial && allocation->serial != serial)) return;
    *slot = NULL;
    cancel_client_surface_output_request( allocation );
}

void X11DRV_client_surface_backing_cancel_geometry( struct x11drv_win_data *data, UINT64 serial )
{
    cancel_client_surface_geometry_request( &data->client_surface_pending_geometry, serial );
}

void X11DRV_client_surface_backing_cancel_requests( struct x11drv_win_data *data )
{
    X11DRV_client_surface_backing_cancel_allocation( data );
    X11DRV_client_surface_backing_cancel_geometry( data, 0 );
}

UINT64 X11DRV_client_surface_geometry_begin( struct x11drv_win_data *data )
{
    UINT64 previous = data->client_surface_geometry_scope;

    if (!++data->client_surface_geometry_next) ++data->client_surface_geometry_next;
    data->client_surface_geometry_scope = data->client_surface_geometry_next;
    return previous;
}

void X11DRV_client_surface_geometry_end( struct x11drv_win_data *data, UINT64 previous, NTSTATUS status )
{
    struct client_surface_output_allocation *allocation = data->client_surface_pending_geometry;

    /* A callback which did not use this observation cannot cancel another
     * operation's pending continuation. Nested callbacks have distinct scopes. */
    if (allocation && allocation->geometry_scope == data->client_surface_geometry_scope && status != STATUS_PENDING)
        X11DRV_client_surface_backing_cancel_geometry( data, allocation->serial );
    data->client_surface_geometry_scope = previous;
}

NTSTATUS X11DRV_client_surface_backing_staged( struct x11drv_win_data *data )
{
    struct client_surface_output_allocation *allocation = data->client_surface_pending_geometry;

    /* STAGED changes the authoritative server epoch, not native geometry.
     * Only this callback's completed observation can cross that boundary. */
    if (allocation && allocation->geometry_scope == data->client_surface_geometry_scope &&
        !allocation->pending && !client_surface_output_waits_scene( allocation ) && !allocation->failed)
        client_surface_capture_scene_state( data->hwnd, &allocation->scene );
    return X11DRV_client_surface_backing_ensure( data );
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
        ready = !allocation || client_surface_output_status( allocation ) != STATUS_PENDING;
        if (ready)
        {
            update = data->client_surface_allocation_update;
        }
    }
    if ((allocation = data->client_surface_pending_geometry) && allocation->serial == serial)
    {
        if (client_surface_output_status( allocation ) != STATUS_PENDING)
            update = allocation->geometry_update;
    }
    /* PREPARE can retain a pool for a DIRECT candidate while the server's
     * backing flag is clear. Its ordinary geometry refresh must continue
     * that update, not reinterpret completion as a request to destroy it. */
    if (update == WINE_UPDATE_CLIENT_SURFACE_BACKING &&
        !data->client_surface_backing_enabled && data->client_surface_backing)
        update = WINE_UPDATE_CLIENT_SURFACE_HANDOFFS;
    release_win_data( data );
    return update;
}

static NTSTATUS prepare_client_surface_output_allocation( struct x11drv_win_data *data,
                                                          unsigned int width, unsigned int height, BOOL force,
                                                          struct client_surface_output_allocation **result )
{
    struct client_surface_output_allocation *allocation = data->client_surface_pending_allocation;
    struct client_surface_compositor_request request =
    { .job = {
        .op = CLIENT_SURFACE_COMPOSITOR_CREATE_POOL,
        .toplevel = data->hwnd,
        .u.pool = {.width = width, .height = height, .depth = data->vis.depth},
    } };
    NTSTATUS status;

    if (allocation && (allocation->width != width || allocation->height != height || allocation->depth != data->vis.depth))
    {
        X11DRV_client_surface_backing_cancel_allocation( data );
        allocation = NULL;
    }
    if (!allocation)
    {
        if (!submit_client_surface_compositor_request( &request )) return STATUS_UNSUCCESSFUL;
        data->client_surface_pending_allocation = request.job.u.pool.allocation;
        request.job.u.pool.allocation->force = force;
        data->client_surface_allocation_serial = request.job.u.pool.allocation->serial;
        data->client_surface_allocation_update = WINE_UPDATE_CLIENT_SURFACE_BACKING;
        return STATUS_PENDING;
    }
    status = client_surface_output_status( allocation );
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
    struct client_surface_compositor_request request =
    { .job = {
        .op = CLIENT_SURFACE_COMPOSITOR_REPLACE_POOL,
        .toplevel = data->hwnd,
        .u.pool =
        {
            .allocation = allocation,
            .source = data->client_surface_backing,
            .destination = data->whole_window,
            .window_owner = data->native_window,
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
    } };

    if (snapshot && (width < window_width || height < window_height))
    {
        free_client_surface_pending_allocation( allocation );
        return STATUS_UNSUCCESSFUL;
    }
    if (allocation->phase != CLIENT_SURFACE_OUTPUT_PAIR_CHECKPOINT)
    {
        if (!data->native_window)
        {
            free_client_surface_pending_allocation( allocation );
            return STATUS_UNSUCCESSFUL;
        }
        if (!client_surface_capture_scene_state( data->hwnd, &request.job.u.pool.scene ))
        {
            /* The GUI must finish its current scene transaction before an
             * input can be identified. Reuse this storage and notification. */
            data->client_surface_pending_allocation = allocation;
            data->client_surface_allocation_update = WINE_UPDATE_CLIENT_SURFACE_BACKING;
            defer_client_surface_output_scene( allocation );
            return STATUS_PENDING;
        }
        allocation->phase = CLIENT_SURFACE_OUTPUT_PAIR_CREATE;
        /* Both replacement and snapshot own a canonical content seed. */
        release_client_surface_output_checkpoint( allocation );
        allocation->window_owner = x11drv_native_window_acquire( data->native_window );
        request.job.op = CLIENT_SURFACE_COMPOSITOR_COPY_POOL;
        if (submit_client_surface_compositor_request( &request ))
        {
            data->client_surface_pending_allocation = allocation;
            data->client_surface_allocation_update = WINE_UPDATE_CLIENT_SURFACE_BACKING;
            return STATUS_PENDING;
        }
        if (request.job.u.pool.invalid_source)
        {
            data->client_surface_backing_valid = FALSE;
            data->client_surface_backing_valid_width = data->client_surface_backing_valid_height = 0;
        }
        free_client_surface_pending_allocation( allocation );
        return request.job.u.pool.stale ? STATUS_RETRY : STATUS_UNSUCCESSFUL;
    }
    if (!submit_client_surface_compositor_request( &request ))
    {
        /* A refusal before execution leaves the completed allocation here.
         * The actor clears this pointer when it consumes that ownership. */
        if (request.job.u.pool.allocation) free_client_surface_pending_allocation( request.job.u.pool.allocation );
        if (request.job.u.pool.invalid_source)
        {
            data->client_surface_backing_valid = FALSE;
            data->client_surface_backing_valid_width = data->client_surface_backing_valid_height = 0;
        }
        return request.job.u.pool.stale ? STATUS_RETRY : STATUS_UNSUCCESSFUL;
    }
    *first = request.job.u.pool.pixmaps[0];
    *second = request.job.u.pool.pixmaps[1];
    return STATUS_SUCCESS;
}

static BOOL client_surface_backing_present( HWND toplevel, Window window, Pixmap pixmap,
                                            unsigned int width, unsigned int height,
                                            struct client_surface_output_allocation *allocation )
{
    struct client_surface_compositor_request request =
    { .job = {
        .op = CLIENT_SURFACE_COMPOSITOR_ADMIT_PRESENT,
        .toplevel = toplevel,
        .u.present =
        {
            .allocation = allocation,
            .source = pixmap,
            .destination = window,
            .width = width,
            .height = height,
        },
    } };

    return submit_client_surface_compositor_request( &request );
}

static BOOL update_client_surface_backing_target( struct x11drv_win_data *data,
                                                   unsigned int window_width, unsigned int window_height )
{
    struct client_surface_compositor_request request =
    { .job = {
        .op = CLIENT_SURFACE_COMPOSITOR_UPDATE_TARGET,
        .toplevel = data->hwnd,
        .u.pool =
        {
            .pixmaps = {data->client_surface_backing, data->client_surface_backing_spare},
            .destination = data->whole_window,
            .window_owner = data->native_window,
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
    } };

    return submit_client_surface_compositor_request( &request );
}

struct client_surface_owner_notifications *X11DRV_client_surface_backing_begin_update(
    HWND hwnd, const struct window_rects *rects, UINT swp_flags, BOOL managed, NTSTATUS *status )
{
    const UINT no_geometry = SWP_NOSIZE | SWP_NOMOVE | SWP_NOCLIENTSIZE | SWP_NOCLIENTMOVE | SWP_NOZORDER;
    struct x11drv_win_data *data;
    BOOL backing;
    struct client_surface_compositor_request request =
    { .job = {
        .op = CLIENT_SURFACE_COMPOSITOR_TRY_BEGIN_UPDATE,
        .toplevel = hwnd,
        .u.update =
        {
            .types = X11DRV_CLIENT_SURFACE_UPDATE_STATE |
                ((swp_flags & (WINE_SWP_CLIENT_SURFACE_BACKING_ENABLE | WINE_SWP_CLIENT_SURFACE_BACKING_DISABLE))
                 ? X11DRV_CLIENT_SURFACE_UPDATE_BACKING : 0) |
                ((swp_flags & WINE_SWP_CLIENT_SURFACE_PREPARE) ? X11DRV_CLIENT_SURFACE_UPDATE_PREPARE : 0) |
                ((swp_flags & WINE_SWP_CLIENT_SURFACE_PUBLISH) ? X11DRV_CLIENT_SURFACE_UPDATE_PUBLISH : 0) |
                (!rects ? X11DRV_CLIENT_SURFACE_UPDATE_REGION : 0),
        },
    } };

    *status = STATUS_SUCCESS;
    if (!(data = get_win_data( hwnd ))) return NULL;
    backing = !!data->client_surface_backing;
    request.job.u.update.preserve_content = managed && !data->managed &&
        data->desired_state.wm_state != WithdrawnState;
    /* A state-only refresh does not change the plan's placement or clip.
     * The server roster/epoch check continues
     * to invalidate topology and producer changes. Be conservative for
     * fullscreen mappings, shape, frame and actual native geometry changes. */
    request.job.u.update.invalidate_scene = !rects || (swp_flags & no_geometry) != no_geometry ||
        (swp_flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW | SWP_FRAMECHANGED | SWP_STATECHANGED)) ||
        data->is_fullscreen || (swp_flags & WINE_SWP_FULLSCREEN) ||
        memcmp( &data->rects, rects, sizeof(*rects) );
    release_win_data( data );
    if (!backing) return NULL;

    /* Every caller can replay from current Win32 state. The actor owns the
     * wake and update reasons until the native receipt arrives; publication
     * is acknowledged only by the normal generation-checked replay. */
    backing = submit_client_surface_compositor_request( &request );
    *status = request.job.u.update.status;
    return backing ? request.job.u.update.notifications : NULL;
}

UINT X11DRV_client_surface_backing_resume_update( HWND hwnd, UINT64 serial,
                                                 struct client_surface_owner_notifications **notifications )
{
    struct client_surface_compositor_request request =
    { .job = {
        .op = CLIENT_SURFACE_COMPOSITOR_CHECK_UPDATE,
        .toplevel = hwnd,
        .u.update =
        {
            .mark = serial,
        },
    } };

    *notifications = NULL;
    if (!submit_client_surface_compositor_request( &request )) return 0;
    *notifications = request.job.u.update.notifications;
    return request.job.u.update.types;
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

void X11DRV_client_surface_backing_end_update( struct x11drv_win_data *data,
                                               struct client_surface_owner_notifications *notifications )
{
    /* Keep this exact target quiescent until its creator stream completes.
     * Cancellation or target removal does not return BEGIN's reference early. */
    end_client_surface_native_update( notifications, data ? data->display : NULL,
                                      data ? data->native_window : NULL );
}

static BOOL register_client_surface_handoff( HWND toplevel,
                                             const struct client_surface_handoff_desc *desc,
                                             UINT64 mark )
{
    struct client_surface_compositor_request request =
    { .job = {
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
    } };
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
            request.job.u.registration.mapping = wine_server_ptr_handle( reply->mapping );
            request.job.u.registration.view_size = reply->size;
            request.job.u.registration.offset = reply->offset;
            request.job.u.registration.mapping_id = reply->mapping_id;
            request.job.u.registration.cookie = reply->cookie;
        }
    }
    SERVER_END_REQ;
    if (status) return FALSE;
    SERVER_START_REQ( get_client_surface_handoff_event )
    {
        req->handle = desc->handle;
        req->producer = desc->process;
        req->surface = desc->surface;
        req->cookie = request.job.u.registration.cookie;
        req->owner = 1;
        status = wine_server_call( req );
        if (!status) event = wine_server_ptr_handle( reply->event );
    }
    SERVER_END_REQ;
    if (!status)
    {
        status = wine_server_handle_to_fd( event, FILE_READ_DATA, &request.job.u.registration.ready_fd, NULL );
        NtClose( event );
    }
    if (status) goto release;
    ret = submit_client_surface_compositor_request( &request );

release:
    NtClose( request.job.u.registration.mapping );
    if (ret) return TRUE;
    SERVER_START_REQ( release_client_surface_handoff )
    {
        req->handle = wine_server_user_handle( toplevel );
        req->producer = desc->process;
        req->surface = desc->surface;
        req->cookie = request.job.u.registration.cookie;
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
    struct client_surface_compositor_request request =
    { .job = {
        .op = CLIENT_SURFACE_COMPOSITOR_REUSE_HANDOFFS,
        .toplevel = toplevel,
        .u.reuse =
        {
            .handoffs = descs,
            .count = count,
            .mark = mark,
        },
    } };
    BOOL *reused, ret = FALSE;
    UINT i;

    if (!count) return TRUE;
    if (!(reused = client_surface_alloc_owned_array( memory, count, sizeof(*reused) ))) return FALSE;
    request.job.u.reuse.reused = reused;
    if (!submit_client_surface_compositor_request( &request )) goto done;
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
    const struct client_surface_memory_scope *memory;
    struct client_surface_compositor_queue *queue;
    struct client_surface_scene_member *members = NULL;
    struct client_surface_handoff_desc *descs = NULL;
    UINT count = 0, live = 0, i;
    UINT64 scene = 0, mark = InterlockedIncrement64( (LONG64 *)&client_surface_compositor_mark );
    BOOL ret = FALSE;

    if (!mark) mark = InterlockedIncrement64( (LONG64 *)&client_surface_compositor_mark );
    if (!(queue = get_client_surface_compositor_queue( toplevel ))) return FALSE;
    memory = client_surface_compositor_queue_memory( queue );
    if (!client_surface_get_scene_snapshot( toplevel, memory, FALSE, &scene, &count, &members )) goto done;
    if (count && !(descs = client_surface_alloc_owned_array( memory, count, sizeof(*descs) ))) goto done;
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
    ret = bind_client_surface_handoffs( toplevel, memory, descs, live, mark );
done:
    client_surface_free_owned_array( descs );
    client_surface_free_scene_snapshot( count, members );
    release_client_surface_compositor_queue( queue );
    return ret;
}

BOOL X11DRV_client_surface_refresh_handoffs( HWND toplevel )
{
    const struct client_surface_memory_scope *memory;
    struct client_surface_compositor_queue *queue;
    struct client_surface_handoff_desc *descs = NULL;
    struct client_surface_scene_member *members = NULL;
    struct client_surface_scene_layout *layouts = NULL;
    struct client_surface_scene scene;
    UINT count = 0, i, index;
    unsigned int layout_count = 0;
    SIZE_T layout_bytes = 0;
    char *clip;
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
    memory = client_surface_compositor_queue_memory( queue );
    if (!client_surface_get_scene_snapshot( toplevel, memory, TRUE, &scene_generation, &count, &members )) goto failed;
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
    if (count && !(descs = client_surface_alloc_owned_array( memory, count, sizeof(*descs) ))) goto failed;
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
        struct client_surface_compositor_request request =
        { .job = {
            .op = CLIENT_SURFACE_COMPOSITOR_CHECK_SCENE,
            .toplevel = toplevel,
            .u.scene_check =
            {
                .epoch = scene_generation,
                .handoffs = descs,
                .count = count,
            },
        } };

        /* Backing ensure, snapshot and end-update can refresh the same plan
         * repeatedly. Its geometry and native clips are immutable at this
         * epoch. Reuse only a still-valid plan with the same live bindings;
         * native geometry and target changes keep their invalidation rules. */
        if (submit_client_surface_compositor_request( &request ))
        {
            if (!client_surface_scene_snapshot_current( toplevel, scene_generation )) goto failed;
            client_surface_free_owned_array( descs );
            client_surface_free_scene_snapshot( count, members );
            release_client_surface_compositor_queue( queue );
            return TRUE;
        }
    }
    for (i = 0; i < count; ++i)
    {
        DWORD size;

        if (!members[i].visible) continue;
        if (!(size = X11DRV_GetRegionDataSize( members[i].region )) ||
            layout_bytes > ~(SIZE_T)0 - sizeof(*layouts) - size) goto failed;
        layout_bytes += sizeof(*layouts) + size;
        ++layout_count;
    }
    if (layout_count && !(layouts = client_surface_alloc_owned_array( memory, layout_bytes, 1 )))
    {
        layout_count = 0;
        goto failed;
    }
    /* Roster, selection, geometry and clips all come from the same reply.
     * The binding cache remains independent and includes hidden producers. */
    clip = (char *)(layouts ? layouts + layout_count : NULL);
    for (i = 0, index = 0; i < count; ++i)
    {
        DWORD size;

        if (!members[i].visible) continue;
        layouts[index].window = members[i].hwnd;
        layouts[index].process = members[i].process;
        layouts[index].identity = members[i].identity;
        layouts[index].geometry = members[i].target;
        size = X11DRV_GetRegionDataSize( members[i].region );
        layouts[index].clip = (RGNDATA *)clip;
        if (!X11DRV_FillRegionData( members[i].region, 0, layouts[index].clip, size )) goto failed;
        clip += size;
        ++index;
    }
    if (layout_count) qsort( layouts, layout_count, sizeof(*layouts), compare_client_surface_scene_layouts );
    if (!bind_client_surface_handoffs( toplevel, memory, descs, count, mark )) goto failed;

    if (!client_surface_scene_snapshot_current( toplevel, scene_generation )) goto failed;
    {
        struct client_surface_compositor_request request =
        { .job = {
            .op = CLIENT_SURFACE_COMPOSITOR_SWEEP_HANDOFFS,
            .toplevel = toplevel,
            .u.scene_install =
            {
                .mark = mark,
                .epoch = scene_generation,
                .layouts = layouts,
                .count = layout_count,
            },
        } };
        BOOL installed;

        installed = submit_client_surface_compositor_request( &request );
        layouts = request.job.u.scene_install.layouts;
        layout_count = request.job.u.scene_install.count;
        if (!installed) goto failed;
    }
    free_client_surface_scene_layouts( layouts );
    client_surface_free_owned_array( descs );
    client_surface_free_scene_snapshot( count, members );
    release_client_surface_compositor_queue( queue );
    return TRUE;

failed:
    free_client_surface_scene_layouts( layouts );
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
    struct client_surface_compositor_request request =
    { .job = {
        .op = resolve ? CLIENT_SURFACE_COMPOSITOR_RESOLVE_SOURCES : CLIENT_SURFACE_COMPOSITOR_REPAIR_OWNER,
        .toplevel = hwnd,
    } };

    if (!resolve)
    {
        /* A cold cache needs the common producer-recovery path. Do not
         * build a scene or provision channels merely to discover that no
         * completed owner image exists. Source-resolution must still bind
         * and inspect channels, including newly completed producer images. */
        request.job.op = CLIENT_SURFACE_COMPOSITOR_CHECK_CACHE;
        if (!submit_client_surface_compositor_request( &request )) return FALSE;
        request.job.op = CLIENT_SURFACE_COMPOSITOR_REPAIR_OWNER;
    }
    /* The authoritative snapshot selects the exact bindings and layouts to
     * inspect. The actor owns their images and attestations; no channel state
     * is interpreted by the application thread as proof of a completed copy. */
    return X11DRV_client_surface_refresh_handoffs( hwnd ) && submit_client_surface_compositor_request( &request );
}

void X11DRV_client_surface_backing_destroy( struct x11drv_win_data *data )
{
    X11DRV_client_surface_backing_cancel_requests( data );
    data->client_surface_map_update = 0;
    remove_client_surface_backing_target( data->hwnd );
    if (data->client_surface_backing || data->client_surface_backing_spare)
    {
        retire_client_surface_output( data->client_surface_backing,
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
    struct client_surface_compositor_request request =
    { .job = {
        .op = CLIENT_SURFACE_COMPOSITOR_RETIRE_POOL,
        .toplevel = data->hwnd,
    } };

    X11DRV_client_surface_backing_cancel_requests( data );
    if (!submit_client_surface_compositor_request( &request )) return FALSE;
    /* The old pair is now detached after Complete/Idle and the scene ACK.
     * Freeing it neither changes the native target nor starts another scene. */
    retire_client_surface_output( data->client_surface_backing, data->client_surface_backing_spare );
    data->client_surface_backing = data->client_surface_backing_spare = 0;
    data->client_surface_backing_width = data->client_surface_backing_height = 0;
    data->client_surface_backing_valid_width = data->client_surface_backing_valid_height = 0;
    data->client_surface_backing_shrink_start = 0;
    data->client_surface_backing_valid = FALSE;
    return TRUE;
}

static NTSTATUS renew_client_surface_owner( struct x11drv_win_data *data )
{
    struct client_surface_scene scene;
    NTSTATUS status;
    unsigned int width, height;

    /* A sole retained native child can continue DIRECT after the owner and
     * producer applied the new geometry. Any Win32 child requires the normal
     * clipping/checkpoint path, even before its producer is registered. */
    client_surface_get_toplevel_scene( data->hwnd, &scene );
    if (!data->client_surface_backing && !data->client_surface_backing_spare &&
        !scene.valid && scene.direct_candidate && scene.epoch && !(scene.epoch & 1) &&
        !NtUserGetWindowRelative( data->hwnd, GW_CHILD ))
    {
        struct client_surface_compositor_request request =
        { .job = {
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
        } };

        status = query_client_surface_extent( data, WINE_PREPARE_CLIENT_SURFACES, &width, &height );
        if (status) return status;
        request.job.u.direct_renew.query = &data->client_surface_pending_geometry->geometry_query;
        if (submit_client_surface_compositor_request( &request ))
        {
            X11DRV_client_surface_backing_cancel_allocation( data );
            X11DRV_client_surface_backing_cancel_geometry( data, 0 );
            return STATUS_SUCCESS;
        }
        /* This operation attempted renewal and now owns the checkpoint
         * fallback. Do not turn it into a fresh preparation on a GUI wake. */
        return STATUS_MORE_PROCESSING_REQUIRED;
    }
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS X11DRV_client_surface_prepare_owner( struct x11drv_win_data *data )
{
    NTSTATUS status;

    /* A pending allocation owns the checkpoint chosen after a rejected
     * renewal. Resume it rather than starting another renewal attempt. */
    if (!data->client_surface_pending_allocation)
    {
        status = renew_client_surface_owner( data );
        if (status != STATUS_NOT_SUPPORTED && status != STATUS_MORE_PROCESSING_REQUIRED)
            return status;
    }
    /* Without an authenticated retained attachment, preserve GDI/background
     * pixels before the next producer can choose or fall back to composition. */
    status = ensure_client_surface_backing( data, TRUE, TRUE, FALSE, WINE_PREPARE_CLIENT_SURFACES );
    if (status == STATUS_PENDING)
    {
        data->client_surface_allocation_update = WINE_PREPARE_CLIENT_SURFACES;
        if (data->client_surface_pending_geometry)
            data->client_surface_pending_geometry->geometry_update = WINE_PREPARE_CLIENT_SURFACES;
    }
    return status;
}

static BOOL client_surface_output_checkpoint_matches( struct x11drv_win_data *data,
                                                       struct client_surface_output_allocation *allocation,
                                                       unsigned int window_width, unsigned int window_height )
{
    if (allocation->phase == CLIENT_SURFACE_OUTPUT_PAIR_CHECKPOINT)
        return allocation->source == data->client_surface_backing && allocation->window_owner == data->native_window &&
               allocation->window == data->whole_window && allocation->window_width == window_width &&
               allocation->window_height == window_height &&
               client_surface_output_checkpoint_scene_current( &allocation->scene );
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
static NTSTATUS ensure_client_surface_backing_extent( struct x11drv_win_data *data, BOOL snapshot,
                                                      BOOL invalidate, BOOL *force,
                                                      unsigned int window_width, unsigned int window_height )
{
    struct client_surface_output_allocation *allocation;
    unsigned int width, height;
    unsigned int old_valid_width, old_valid_height;
    BOOL old_valid, valid, shrink;
    Pixmap pixmap, spare, old_pixmap, old_spare;
    NTSTATUS status;

    shrink = FALSE;
    /* Native ConfigureNotify can precede the Win32 geometry update. The
     * checkpoint must cover the actual Window even during that interval. */
    width = client_surface_backing_extent( max( data->rects.visible.right - data->rects.visible.left, window_width ) );
    height = client_surface_backing_extent( max( data->rects.visible.bottom - data->rects.visible.top, window_height ) );
    TRACE_(csperf)( "event=backing_extent hwnd=%p native=%ux%u capacity=%ux%u desired=%s\n",
                   data->hwnd, window_width, window_height, width, height, wine_dbgstr_rect( &data->rects.visible ) );
    allocation = data->client_surface_pending_allocation;
    if (allocation && (allocation->phase == CLIENT_SURFACE_OUTPUT_PAIR_CHECKPOINT ||
                       allocation->phase == CLIENT_SURFACE_OUTPUT_WINDOW_COPY_WAIT) &&
        !client_surface_output_checkpoint_matches( data, allocation, window_width, window_height ))
    {
        X11DRV_client_surface_backing_cancel_allocation( data );
        allocation = NULL;
    }
    if (allocation && allocation->force) *force = TRUE;
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
        !snapshot && !shrink && !*force &&
        data->client_surface_backing_width >= width &&
        data->client_surface_backing_height >= height)
    {
        if (data->client_surface_pending_allocation)
            X11DRV_client_surface_backing_cancel_allocation( data );
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
        if (!update_client_surface_backing_target( data, window_width, window_height )) return STATUS_UNSUCCESSFUL;
        X11DRV_client_surface_refresh_handoffs( data->hwnd );
        return STATUS_SUCCESS;
    }

    /* A snapshot owns its new pair until the checked seed is complete.
     * Never replace the live pool and then overwrite its spare with another
     * Window read: native publication must continue to use the old images
     * throughout preparation, including cancellation and delayed completion. */
    /* Grow the axis which needs space, without retaining the historical
     * maximum of the other axis across alternating wide/tall resizes. */
    if (!shrink && width > data->client_surface_backing_width)
        width = max( width, client_surface_backing_extent( data->client_surface_backing_width * 9 / 8 ) );
    if (!shrink && height > data->client_surface_backing_height)
        height = max( height, client_surface_backing_extent( data->client_surface_backing_height * 9 / 8 ) );
    if (!shrink && width <= data->client_surface_backing_width && height <= data->client_surface_backing_height)
    {
        /* A new checkpoint still obeys the capacity shrink delay. */
        width = data->client_surface_backing_width;
        height = data->client_surface_backing_height;
    }
    /* CREATE has no content identity. A resumed COPY instead retains its
     * exact source and scene; the matching check above precedes consumption. */
    if ((status = prepare_client_surface_output_allocation( data, width, height, *force, &allocation )) != STATUS_SUCCESS)
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
    if (old_pixmap || old_spare) retire_client_surface_output( old_pixmap, old_spare );
    X11DRV_client_surface_refresh_handoffs( data->hwnd );
    if (snapshot && !invalidate)
    {
        data->client_surface_backing_valid = TRUE;
        data->client_surface_backing_valid_width = window_width;
        data->client_surface_backing_valid_height = window_height;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS query_client_surface_extent( struct x11drv_win_data *data, UINT update,
                                             unsigned int *width, unsigned int *height )
{
    struct client_surface_output_allocation **slot = &data->client_surface_pending_geometry;
    struct client_surface_output_allocation *allocation = *slot;
    struct client_surface_compositor_request request =
    { .job = {
        .op = CLIENT_SURFACE_COMPOSITOR_QUERY_WINDOW,
        .toplevel = data->hwnd,
        .u.pool = {.window_owner = data->native_window, .source = data->client_surface_backing,
                   .destination = data->whole_window, .geometry_update = update,
                   .window_width = data->rects.visible.right - data->rects.visible.left,
                   .window_height = data->rects.visible.bottom - data->rects.visible.top,
                   .geometry_rect = {data->rects.client.left - data->rects.visible.left,
                                     data->rects.client.top - data->rects.visible.top,
                                     data->rects.client.right - data->rects.visible.left,
                                     data->rects.client.bottom - data->rects.visible.top}},
    } };
    NTSTATUS status;

    /* A generic refresh cannot consume the only wake or observation of
     * a reserved PREPARE/PUBLISH, including one invalidated by native changes. */
    if (allocation && update == WINE_UPDATE_CLIENT_SURFACE_BACKING && allocation->geometry_update != update &&
        allocation->geometry_scope != data->client_surface_geometry_scope)
        return STATUS_PENDING;
    if (allocation && (allocation->window_owner != data->native_window || allocation->window != data->whole_window ||
        allocation->source != data->client_surface_backing ||
        allocation->geometry_revision != data->client_surface_native_revision ||
        (!client_surface_output_waits_scene( allocation ) && !client_surface_output_checkpoint_scene_current( &allocation->scene ))))
    {
        cancel_client_surface_geometry_request( slot, 0 );
        allocation = NULL;
    }
    if (allocation)
    {
        if (update != WINE_UPDATE_CLIENT_SURFACE_BACKING) allocation->geometry_update = update;
        allocation->geometry_scope = data->client_surface_geometry_scope;
        status = client_surface_output_status( allocation );
        if (status) return status;
    }
    if (!allocation || allocation->phase == CLIENT_SURFACE_OUTPUT_WINDOW_SCENE_WAIT)
    {
        if (!data->native_window) return STATUS_UNSUCCESSFUL;
        client_surface_capture_scene_state( data->hwnd, &request.job.u.pool.scene );
        request.job.u.pool.allocation = allocation;
        if (!submit_client_surface_compositor_request( &request )) return STATUS_UNSUCCESSFUL;
        *slot = request.job.u.pool.allocation;
        request.job.u.pool.allocation->geometry_scope = data->client_surface_geometry_scope;
        request.job.u.pool.allocation->geometry_revision = data->client_surface_native_revision;
        return STATUS_PENDING;
    }
    TRACE_(csperf)( "ticks=%llu event=geometry_query_adopt request=%p serial=%llu status=0 epoch=%llu\n",
                   client_surface_perf_time(), allocation, (unsigned long long)allocation->serial,
                   (unsigned long long)allocation->scene.epoch );
    *width = allocation->window_width;
    *height = allocation->window_height;
    return STATUS_SUCCESS;
}

/* One native observation supplies capacity, snapshot and target installation
 * for this attempt. A stale scene retries at the observation boundary, never
 * by mixing a new native size into an already checked capacity decision. */
static NTSTATUS ensure_client_surface_backing( struct x11drv_win_data *data, BOOL snapshot,
                                               BOOL invalidate, BOOL force, UINT update )
{
    unsigned int window_width, window_height;
    NTSTATUS status;

    if (!data->whole_window) return STATUS_UNSUCCESSFUL;
    status = query_client_surface_extent( data, update, &window_width, &window_height );
    if (status) return status;
    if (snapshot)
    {
        data->client_surface_map_update =
            data->client_surface_pending_geometry->geometry_query.map_state != IsViewable ? update : 0;
        if (data->client_surface_map_update)
        {
            /* MapNotify must start a fresh observation, not reuse an
             * unviewable result as permission to read Window pixels. Keep the
             * admitted operation: a generic backing update need not prepare
             * a DIRECT candidate whose server backing flag is still clear. */
            TRACE( "waiting for client-surface map hwnd %p update %#x\n", data->hwnd, update );
            X11DRV_client_surface_backing_cancel_geometry( data, 0 );
            return STATUS_PENDING;
        }
    }
    status = ensure_client_surface_backing_extent( data, snapshot, invalidate, &force, window_width, window_height );
    if (status == STATUS_RETRY)
    {
        X11DRV_client_surface_backing_cancel_geometry( data, 0 );
        return query_client_surface_extent( data, update, &window_width, &window_height );
    }
    /* This operation may install/rotate its own backing while retaining the
     * same native observation. An unrelated replacement still fails matching. */
    if (status == STATUS_SUCCESS && data->client_surface_pending_geometry)
        data->client_surface_pending_geometry->source = data->client_surface_backing;
    return status;
}

NTSTATUS X11DRV_client_surface_backing_ensure( struct x11drv_win_data *data )
{
    return ensure_client_surface_backing( data, FALSE, FALSE, FALSE, WINE_UPDATE_CLIENT_SURFACE_BACKING );
}

NTSTATUS X11DRV_client_surface_backing_snapshot( struct x11drv_win_data *data, BOOL invalidate )
{
    return ensure_client_surface_backing( data, TRUE, invalidate, FALSE, WINE_UPDATE_CLIENT_SURFACE_BACKING );
}

NTSTATUS X11DRV_client_surface_backing_publish( struct x11drv_win_data *data )
{
    struct client_surface_output_allocation *allocation;
    unsigned int window_width, window_height;
    BOOL force = FALSE;
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    if (!data->whole_window || !data->client_surface_backing) goto done;
    status = query_client_surface_extent( data, WINE_PUBLISH_CLIENT_SURFACES, &window_width, &window_height );
    if (status) goto done;
    allocation = data->client_surface_pending_geometry;
    if (allocation->phase == CLIENT_SURFACE_OUTPUT_WINDOW_PRESENT) goto published;
    /* This reservation owns one observation through capacity changes and
     * target installation. A deferred allocation resumes PUBLISH, not an
     * unrelated backing update which would consume another observation. */
    status = ensure_client_surface_backing_extent( data, FALSE, FALSE, &force, window_width, window_height );
    if (status == STATUS_RETRY)
    {
        X11DRV_client_surface_backing_cancel_geometry( data, 0 );
        status = query_client_surface_extent( data, WINE_PUBLISH_CLIENT_SURFACES, &window_width, &window_height );
    }
    if (status == STATUS_PENDING)
        data->client_surface_allocation_update = WINE_PUBLISH_CLIENT_SURFACES;
    if (status) goto done;
    if (!client_surface_backing_present( data->hwnd, data->whole_window, data->client_surface_backing,
                                         window_width, window_height, allocation ))
    {
        /* Refused admission must not bypass that Window's checked-copy
         * publication order. */
        status = STATUS_UNSUCCESSFUL;
        goto done;
    }
    status = STATUS_PENDING;
    goto done;
published:
    data->client_surface_backing_valid = TRUE;
    data->client_surface_backing_valid_width = window_width;
    data->client_surface_backing_valid_height = window_height;
done:
    return status;
}
