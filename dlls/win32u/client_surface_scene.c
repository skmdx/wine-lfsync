/*
 * Client surface scene state and GUI publication transactions
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

#include "ntstatus.h"
#include "client_surface.h"
#include "ntuser_private.h"
#include "wine/server.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(win);
WINE_DECLARE_DEBUG_CHANNEL(csperf);

/* This module owns server state transitions and the GUI's asynchronous
 * PREPARE/PUBLISH continuation. Surface registry, placement snapshots and
 * producer lifetime remain with client_surface.c. */

HWND client_surface_set_server_state( HWND hwnd, const struct client_surface *surface,
                                      UINT flags, UINT64 generation,
                                      UINT64 scene_generation, BOOL *wake )
{
    HWND toplevel = 0;

    if (wake) *wake = FALSE;
    SERVER_START_REQ( set_client_surface_state )
    {
        req->handle = wine_server_user_handle( hwnd );
        req->surface = surface ? client_surface_get_identity( surface ) : 0;
        req->flags = flags;
        req->generation = (flags & CLIENT_SURFACE_STATE_NATIVE_CANDIDATE) && surface ?
                          surface->present_serial + 1 : generation;
        req->scene_generation = scene_generation;
        if (!wine_server_call( req ))
        {
            toplevel = wine_server_ptr_handle( reply->toplevel );
            if (wake) *wake = reply->wake;
        }
    }
    SERVER_END_REQ;
    return toplevel;
}

void client_surface_geometry_ready( HWND hwnd )
{
    HWND toplevel;
    BOOL wake;

    toplevel = client_surface_set_server_state( hwnd, NULL,
                                                CLIENT_SURFACE_STATE_GEOMETRY_READY, 0, 0, &wake );
    if (wake && toplevel) NtUserPostMessage( toplevel, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
}

void client_surface_repair_owner( HWND hwnd )
{
    /* Only the native owner can know whether its immutable images cover the
     * current scene. Legacy and cold caches still need producer source recovery. */
    if (!user_driver->pRepairClientSurfaceOwner( hwnd, FALSE )) client_surface_geometry_ready( hwnd );
}

void client_surface_resolve_sources( HWND hwnd )
{
    struct client_surface_scene scene;

    if (!client_surface_get_toplevel_scene( hwnd, &scene ) || !scene.source_pending) return;
    if (user_driver->pRepairClientSurfaceOwner( hwnd, TRUE )) return;
    /* Unsupported backends and failed cache inspections report an empty
     * inventory for the captured scene. A late owner message cannot reopen a
     * finished repair or supersede a newer source recovery request. */
    SERVER_START_REQ( resolve_client_surface_scene_sources )
    {
        req->handle = wine_server_user_handle( hwnd );
        req->scene_id = scene.epoch;
        wine_server_call( req );
    }
    SERVER_END_REQ;
}

static BOOL client_surface_set_scene_result( const struct client_surface_scene *scene, UINT flags )
{
    BOOL accepted = FALSE, staged = FALSE;

    if (!scene->toplevel || !scene->epoch) return FALSE;
    SERVER_START_REQ( set_client_surface_state )
    {
        req->handle = req->scene_toplevel = wine_server_user_handle( scene->toplevel );
        req->flags = flags;
        req->generation = scene->generation;
        req->scene_generation = scene->epoch;
        req->producer_sequence = scene->paint_serial;
        if (!wine_server_call( req ) && reply->toplevel == wine_server_user_handle( scene->toplevel ))
        {
            accepted = TRUE;
            staged = reply->staged;
            if (reply->wake && flags != CLIENT_SURFACE_STATE_PREPARE_COMMIT)
                NtUserPostMessage( scene->toplevel, WM_WINE_UPDATEWINDOWSTATE, 0, 0 );
        }
    }
    SERVER_END_REQ;
    TRACE( "client surface result %#x for %p generation %s scene %s accepted %u staged %u\n", flags,
           scene->toplevel, wine_dbgstr_longlong( scene->generation ),
           wine_dbgstr_longlong( scene->epoch ), accepted, staged );
    return accepted && (flags != CLIENT_SURFACE_STATE_STAGED || staged);
}

BOOL client_surface_set_staged( const struct client_surface_scene *scene )
{
    return client_surface_set_scene_result( scene, CLIENT_SURFACE_STATE_STAGED );
}

void client_surface_bypass_staging( HWND hwnd )
{
    client_surface_set_server_state( hwnd, NULL, CLIENT_SURFACE_STATE_BYPASS, 0, 0, NULL );
}

void client_surface_fail_scene( const struct client_surface_scene *scene )
{
    client_surface_set_scene_result( scene, CLIENT_SURFACE_STATE_FAILED );
}

static BOOL set_client_surface_native_barrier( HWND hwnd, UINT_PTR token, BOOL begin )
{
    BOOL ret = FALSE;

    SERVER_START_REQ( set_client_surface_native_barrier )
    {
        req->handle = wine_server_user_handle( hwnd );
        req->token = token;
        req->begin = begin;
        ret = !wine_server_call( req );
    }
    SERVER_END_REQ;
    return ret;
}

BOOL client_surface_begin_native_barrier( HWND hwnd, UINT_PTR token )
{
    return set_client_surface_native_barrier( hwnd, token, TRUE );
}

BOOL client_surface_end_native_barrier( HWND hwnd, UINT_PTR token )
{
    return set_client_surface_native_barrier( hwnd, token, FALSE );
}

UINT client_surface_begin_publish( HWND hwnd, UINT64 *generation, UINT64 *scene_generation )
{
    UINT publish = 0;

    *generation = 0;
    *scene_generation = 0;
    SERVER_START_REQ( set_client_surface_state )
    {
        req->handle = wine_server_user_handle( hwnd );
        req->surface = 0;
        req->flags = CLIENT_SURFACE_STATE_PUBLISH_BEGIN;
        req->generation = 0;
        req->scene_generation = 0;
        if (!wine_server_call( req ) && reply->publish)
        {
            *generation = reply->generation;
            *scene_generation = reply->scene_generation;
            publish = reply->publish;
        }
    }
    SERVER_END_REQ;
    return publish;
}

NTSTATUS client_surface_publish_window( HWND hwnd )
{
    UINT64 generation, scene;
    NTSTATUS status = STATUS_NOT_FOUND;
    UINT publish = 0;
    BOOL again;
    WND *win;

    if (!is_current_thread_window( hwnd ) || !(win = get_win_ptr( hwnd ))) return STATUS_NOT_FOUND;
    if (win == WND_DESKTOP || win == WND_OTHER_PROCESS) return STATUS_NOT_FOUND;
    if (win->publish_busy)
    {
        win->publish_again = TRUE;
        release_win_ptr( win );
        return STATUS_PENDING;
    }
    win->publish_busy = TRUE;
    generation = win->publish_generation;
    scene = win->publish_scene;
    release_win_ptr( win );

    if (generation)
    {
        SERVER_START_REQ( set_client_surface_state )
        {
            req->handle = wine_server_user_handle( hwnd );
            req->flags = CLIENT_SURFACE_STATE_PUBLISH_RESUME;
            req->surface = 0;
            req->generation = generation;
            req->scene_generation = scene;
            if (!wine_server_call( req )) publish = reply->publish;
        }
        SERVER_END_REQ;
        /* Retirement authenticates the old reservation independently. A
         * stale continuation must never fail or commit a newer publication. */
        if (!publish)
        {
            client_surface_end_publish( hwnd, generation, scene, FALSE );
            /* This wake may already belong to the next READY transaction. */
            publish = client_surface_begin_publish( hwnd, &generation, &scene );
        }
    }
    else publish = client_surface_begin_publish( hwnd, &generation, &scene );
    if (publish)
    {
        if (publish == CLIENT_SURFACE_PUBLISH_EXPOSE)
            status = user_driver->pExposeClientSurface( hwnd, scene ) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
        else status = publish_window_state( hwnd );
        if (status != STATUS_PENDING)
            client_surface_end_publish( hwnd, generation, scene, status == STATUS_SUCCESS );
    }
    if (!(win = get_win_ptr( hwnd ))) return STATUS_NOT_FOUND;
    if (win == WND_DESKTOP || win == WND_OTHER_PROCESS) return STATUS_NOT_FOUND;
    win->publish_generation = status == STATUS_PENDING ? generation : 0;
    win->publish_scene = status == STATUS_PENDING ? scene : 0;
    win->publish_busy = FALSE;
    again = win->publish_again;
    win->publish_again = FALSE;
    release_win_ptr( win );
    if (again) NtUserPostMessage( hwnd, WM_WINE_UPDATEWINDOWSTATE, WINE_PUBLISH_CLIENT_SURFACES, 0 );
    return status;
}

BOOL client_surface_end_publish( HWND hwnd, UINT64 generation, UINT64 scene_generation, BOOL success )
{
    BOOL accepted = FALSE;
    NTSTATUS status;

    if (success)
    {
        SERVER_START_REQ( set_client_surface_state )
        {
            req->handle = wine_server_user_handle( hwnd );
            req->flags = CLIENT_SURFACE_STATE_PUBLISH_COMMIT;
            req->generation = generation;
            req->scene_generation = scene_generation;
            status = wine_server_call( req );
            if (!status) accepted = reply->publish;
        }
        SERVER_END_REQ;
    }
    else
    {
        /* A failed native exposure retires only the publication this GUI
         * reserved, never a newer scene reached while it was running. */
        SERVER_START_REQ( publish_client_surface_handoff )
        {
            req->handle = wine_server_user_handle( hwnd );
            req->generation = generation;
            req->scene_generation = scene_generation;
            req->success = FALSE;
            status = wine_server_call( req );
            if (!status) accepted = reply->accepted;
        }
        SERVER_END_REQ;
    }
    TRACE( "published GUI generation %s epoch %s status %#lx accepted %u success %u\n",
           wine_dbgstr_longlong( generation ), wine_dbgstr_longlong( scene_generation ),
           (unsigned long)status, accepted, success );
    return !status && accepted && success;
}

NTSTATUS client_surface_begin_prepare( HWND hwnd, struct client_surface_scene *scene )
{
    NTSTATUS status;

    memset( scene, 0, sizeof(*scene) );
    SERVER_START_REQ( set_client_surface_state )
    {
        req->handle = wine_server_user_handle( hwnd );
        req->surface = 0;
        req->flags = CLIENT_SURFACE_STATE_PREPARE_BEGIN;
        req->generation = 0;
        req->scene_generation = 0;
        wine_server_set_reply( req, &scene->paint_serial, sizeof(scene->paint_serial) );
        status = wine_server_call( req );
        if (status == STATUS_SUCCESS && wine_server_reply_size( reply ) == sizeof(scene->paint_serial) && reply->publish &&
            reply->toplevel == wine_server_user_handle( hwnd ) &&
            reply->scene_generation && !(reply->scene_generation & 1))
        {
            /* Keep the exact admission reply, including PREPARING generation
             * zero. A later shared-state read could name another preparation. */
            scene->toplevel = wine_server_ptr_handle( reply->toplevel );
            scene->epoch = reply->scene_generation;
            scene->generation = reply->generation;
        }
        else if (status == STATUS_SUCCESS) status = STATUS_NOT_FOUND;
    }
    SERVER_END_REQ;
    TRACE_(csperf)( "event=client_surface_prepare hwnd=%p status=%#x epoch=%llu generation=%llu paint_serial=%llu\n",
                   hwnd, (unsigned int)status, (unsigned long long)scene->epoch,
                   (unsigned long long)scene->generation, (unsigned long long)scene->paint_serial );
    return status;
}

BOOL client_surface_end_prepare( const struct client_surface_scene *scene )
{
    return client_surface_set_scene_result( scene, CLIENT_SURFACE_STATE_PREPARE_COMMIT );
}
