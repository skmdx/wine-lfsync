/*
 * Client surface lifetime and publication tests
 *
 * Copyright 2026 Wine contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windows.h"
#include "winternl.h"
#include "wine/server.h"
#include "wine/test.h"
#include "wine/wgl.h"
#include "wine/client_surface.h"

#define RACE_THREADS 4
#define RACE_ROUNDS 2000
#define OWNER_SURFACES 128
#define STATUS_WINE_INVALID_WINDOW_HANDLE (0xc0010000u | ERROR_INVALID_WINDOW_HANDLE)

struct surface_state
{
    HWND toplevel;
    UINT64 generation;
    UINT64 scene_generation;
    UINT pending;
    UINT staged;
    UINT ready;
    UINT publish;
    UINT compose;
    UINT mode;
    UINT active;
    UINT cached;
    BOOL wake;
};

struct clip_state
{
    HWND toplevel;
    UINT64 scene_generation;
    UINT count;
    struct client_surface_clip_window windows[8];
};

static unsigned int (CDECL *p_wine_server_call)(void *);
static void pump_messages( DWORD timeout );

static unsigned int set_surface_state_scene( HWND hwnd, UINT_PTR surface, UINT flags,
                                             UINT64 generation, UINT64 scene_generation,
                                             struct surface_state *state )
{
    struct __server_request_info info;
    struct set_client_surface_state_request *req = &info.u.req.set_client_surface_state_request;
    const struct set_client_surface_state_reply *reply = &info.u.reply.set_client_surface_state_reply;
    unsigned int status;

    memset( &info, 0, sizeof(info) );
    req->__header.req = REQ_set_client_surface_state;
    req->handle = wine_server_user_handle( hwnd );
    req->surface = surface;
    req->flags = flags;
    req->generation = generation;
    req->scene_generation = scene_generation;
    status = p_wine_server_call( &info );
    if (!status && state)
    {
        state->toplevel = wine_server_ptr_handle( reply->toplevel );
        state->generation = reply->generation;
        state->scene_generation = reply->scene_generation;
        state->pending = reply->pending;
        state->staged = reply->staged;
        state->ready = reply->ready;
        state->publish = reply->publish;
        state->compose = reply->compose;
        state->mode = reply->mode;
        state->active = reply->active;
        state->cached = reply->cached;
        state->wake = reply->wake;
    }
    return status;
}

static unsigned int set_surface_state( HWND hwnd, UINT_PTR surface, UINT flags,
                                       UINT64 generation, struct surface_state *state )
{
    return set_surface_state_scene( hwnd, surface, flags, generation, 0, state );
}

static unsigned int set_server_parent( HWND hwnd, HWND parent )
{
    struct __server_request_info info = {0};

    info.u.req.set_parent_request.__header.req = REQ_set_parent;
    info.u.req.set_parent_request.handle = wine_server_user_handle( hwnd );
    info.u.req.set_parent_request.parent = wine_server_user_handle( parent );
    return p_wine_server_call( &info );
}

static unsigned int commit_surface_state( HWND hwnd, UINT_PTR surface,
                                          const struct surface_state *generation,
                                          struct surface_state *state )
{
    return set_surface_state_scene( hwnd, surface, CLIENT_SURFACE_STATE_PRESENT_COMMIT,
                                    generation->generation, generation->scene_generation, state );
}

static unsigned int claim_surface_state( HWND hwnd, UINT_PTR surface,
                                         struct surface_state *state )
{
    return set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_CLAIM, 0, state );
}

static unsigned int begin_surface_state( HWND hwnd, UINT_PTR surface,
                                         const struct surface_state *generation,
                                         struct surface_state *state )
{
    return set_surface_state_scene( hwnd, surface, CLIENT_SURFACE_STATE_PRESENT_BEGIN,
                                    generation->generation, generation->scene_generation, state );
}

static unsigned int publish_surface_state( HWND hwnd, struct surface_state *state )
{
    struct surface_state begin;
    unsigned int status;

    memset( &begin, 0, sizeof(begin) );
    status = set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_PUBLISH_BEGIN, 0, &begin );
    if (status || !begin.publish)
    {
        if (state) *state = begin;
        return status;
    }
    return set_surface_state_scene( hwnd, 0, CLIENT_SURFACE_STATE_PUBLISH_COMMIT,
                                    begin.generation, begin.scene_generation, state );
}

static unsigned int prepare_surface_state( HWND hwnd, struct surface_state *state )
{
    struct surface_state begin;
    unsigned int status;

    memset( &begin, 0, sizeof(begin) );
    status = set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_PREPARE_BEGIN, 0, &begin );
    if (status || !begin.publish)
    {
        if (state) *state = begin;
        return status;
    }
    return set_surface_state_scene( hwnd, 0, CLIENT_SURFACE_STATE_PREPARE_COMMIT,
                                    0, begin.scene_generation, state );
}

static unsigned int get_clip_state_in_bounds( HWND hwnd, UINT dpi, const RECT *bounds,
                                               struct clip_state *state )
{
    struct __server_request_info info;
    struct get_client_surface_clip_windows_request *req =
        &info.u.req.get_client_surface_clip_windows_request;
    const struct get_client_surface_clip_windows_reply *reply =
        &info.u.reply.get_client_surface_clip_windows_reply;
    struct rectangle rect;
    unsigned int status;

    memset( &info, 0, sizeof(info) );
    memset( state, 0, sizeof(*state) );
    req->__header.req = REQ_get_client_surface_clip_windows;
    req->handle = wine_server_user_handle( hwnd );
    req->dpi.num = dpi;
    req->dpi.den = 1;
    if (bounds)
    {
        rect = wine_server_rectangle( *bounds );
        wine_server_add_data( &info, &rect, sizeof(rect) );
    }
    wine_server_set_reply( &info, state->windows, sizeof(state->windows) );
    status = p_wine_server_call( &info );
    if (!status)
    {
        state->toplevel = wine_server_ptr_handle( reply->toplevel );
        state->scene_generation = reply->scene_generation;
        state->count = reply->count;
    }
    return status;
}

static unsigned int get_clip_state( HWND hwnd, struct clip_state *state )
{
    return get_clip_state_in_bounds( hwnd, 96, NULL, state );
}

static void check_clip_bounds( HWND hwnd, UINT dpi, const RECT *bounds )
{
    struct clip_state full, clipped;
    unsigned int status, count = 0, i;

    status = get_clip_state_in_bounds( hwnd, dpi, NULL, &full );
    ok( !status && full.count <= ARRAY_SIZE(full.windows),
        "full clip status %#x count %u\n", status, full.count );
    if (status || full.count > ARRAY_SIZE(full.windows)) return;
    status = get_clip_state_in_bounds( hwnd, dpi, bounds, &clipped );
    ok( !status, "bounded clip status %#x\n", status );
    if (status) return;
    ok( clipped.toplevel == full.toplevel && clipped.scene_generation == full.scene_generation,
        "bounds changed the clip scene\n" );
    for (i = 0; i < full.count; ++i)
    {
        RECT rect = wine_server_get_rect( full.windows[i].rect ), expected, actual;

        if (!IntersectRect( &expected, &rect, bounds )) continue;
        if (count < clipped.count && count < ARRAY_SIZE(clipped.windows))
        {
            actual = wine_server_get_rect( clipped.windows[count].rect );
            ok( clipped.windows[count].handle == full.windows[i].handle && EqualRect( &actual, &expected ),
                "bounded clip %u at DPI %u has window %#x rect %s, expected %#x %s\n",
                count, dpi, clipped.windows[count].handle, wine_dbgstr_rect( &actual ),
                full.windows[i].handle, wine_dbgstr_rect( &expected ) );
        }
        ++count;
    }
    ok( clipped.count == count, "bounded clip count %u, expected %u at DPI %u for %s\n",
        clipped.count, count, dpi, wine_dbgstr_rect( bounds ) );
}

static BOOL clip_state_contains( const struct clip_state *state, HWND hwnd )
{
    UINT i;

    for (i = 0; i < state->count && i < ARRAY_SIZE(state->windows); ++i)
        if (wine_server_ptr_handle( state->windows[i].handle ) == hwnd) return TRUE;
    return FALSE;
}

static void test_completion_result_provenance(void)
{
    struct client_surface_completion completion = {0};

    completion.kind = CLIENT_SURFACE_COMPLETION_SHARED;
    ok( !client_surface_completion_result_is_external( &completion ),
        "unresolved shared completion has an external result\n" );
    completion.external_result = TRUE;
    ok( client_surface_completion_result_is_external( &completion ),
        "queued shared completion lost its supplied result\n" );

    completion.kind = CLIENT_SURFACE_COMPLETION_EXACT;
    ok( client_surface_completion_result_is_external( &completion ),
        "exact completion lost its supplied result\n" );
    completion.external_result = FALSE;
    ok( !client_surface_completion_result_is_external( &completion ),
        "unresolved exact completion has an external result\n" );
}

static UINT clip_state_count( const struct clip_state *state, HWND hwnd )
{
    UINT count = 0, i;

    for (i = 0; i < state->count && i < ARRAY_SIZE(state->windows); ++i)
        if (wine_server_ptr_handle( state->windows[i].handle ) == hwnd) count++;
    return count;
}

static LRESULT CALLBACK client_surface_proc( HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam )
{
    return DefWindowProcA( hwnd, message, wparam, lparam );
}

static HWND create_test_window( BOOL visible )
{
    WNDCLASSA class = {0};

    class.lpfnWndProc = client_surface_proc;
    class.hInstance = GetModuleHandleA( NULL );
    class.lpszClassName = "client_surface_test";
    if (!RegisterClassA( &class ) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return NULL;
    return CreateWindowExA( 0, class.lpszClassName, "client surface test",
                            WS_POPUP | (visible ? WS_VISIBLE : 0),
                            10, 10, 160, 120, NULL, NULL, class.hInstance, NULL );
}

static HWND create_test_child( HWND parent, int x )
{
    return CreateWindowExA( 0, "client_surface_test", "client surface child",
                            WS_CHILD | WS_VISIBLE, x, 10, 50, 40, parent, NULL,
                            GetModuleHandleA( NULL ), NULL );
}

static void complete_single_surface_generation( HWND hwnd, UINT_PTR surface,
                                                struct surface_state *state )
{
    struct surface_state generation;
    unsigned int status;

    if (!state->pending && state->mode == CLIENT_SURFACE_PRESENTATION_COMPOSITED)
    {
        status = prepare_surface_state( hwnd, state );
        ok( !status, "surface prepare status %#x\n", status );
    }
    generation = *state;
    if (!generation.pending) return;
    ok( generation.pending == 1, "unexpected generation pending count %u\n", generation.pending );
    status = begin_surface_state( hwnd, surface, &generation, state );
    ok( !status && state->compose, "generation begin status %#x compose %u\n",
        status, state->compose );
    status = commit_surface_state( hwnd, surface, &generation, state );
    ok( !status, "generation commit status %#x\n", status );
    if (state->ready)
    {
        status = publish_surface_state( hwnd, state );
        ok( !status, "generation publish status %#x\n", status );
    }
}

static void test_presentation_modes(void)
{
    const UINT_PTR surface = 0x1234d000, child_surface = 0x1234d001;
    const UINT direct_flags = CLIENT_SURFACE_STATE_REGISTER |
                              CLIENT_SURFACE_STATE_SCENE_PUBLICATION |
                              CLIENT_SURFACE_STATE_DIRECT_PRESENTATION;
    struct surface_state state;
    HWND hwnd, child = NULL;
    unsigned int status;

    hwnd = create_test_window( FALSE );
    ok( !!hwnd, "failed to create presentation mode window, error %lu\n", GetLastError() );
    if (!hwnd) return;
    ShowWindow( hwnd, SW_SHOW );
    pump_messages( 100 );

    status = set_surface_state( hwnd, surface, direct_flags, 0, &state );
    ok( !status, "direct surface register failed, status %#x\n", status );
    status = claim_surface_state( hwnd, surface, &state );
    ok( !status, "direct surface claim failed, status %#x\n", status );
    complete_single_surface_generation( hwnd, surface, &state );
    status = set_surface_state( hwnd, surface, 0, 0, &state );
    ok( !status && state.mode == CLIENT_SURFACE_PRESENTATION_DIRECT,
        "simple top-level mode %u, expected DIRECT, status %#x\n", state.mode, status );

    child = create_test_child( hwnd, 10 );
    ok( !!child, "failed to create presentation mode child, error %lu\n", GetLastError() );
    if (child)
    {
        status = set_surface_state( child, child_surface, direct_flags, 0, &state );
        ok( !status, "child surface register failed, status %#x\n", status );
        status = claim_surface_state( child, child_surface, &state );
        ok( !status && state.mode == CLIENT_SURFACE_PRESENTATION_COMPOSITED,
            "multi-window mode %u, expected COMPOSITED, status %#x\n", state.mode, status );
        status = set_surface_state( child, child_surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, &state );
        ok( !status, "child surface unregister failed, status %#x\n", status );
        DestroyWindow( child );
        child = NULL;
        status = set_surface_state( hwnd, surface, 0, 0, &state );
        ok( !status, "post-child mode query failed, status %#x\n", status );
        complete_single_surface_generation( hwnd, surface, &state );
        status = set_surface_state( hwnd, surface, 0, 0, &state );
        ok( !status && state.mode == CLIENT_SURFACE_PRESENTATION_DIRECT,
            "post-child mode %u, expected DIRECT, status %#x\n", state.mode, status );
    }

    ShowWindow( hwnd, SW_HIDE );
    pump_messages( 100 );
    status = set_surface_state( hwnd, surface, 0, 0, &state );
    ok( !status && state.mode == CLIENT_SURFACE_PRESENTATION_COMPOSITED,
        "hidden idle mode %u, expected COMPOSITED, status %#x\n", state.mode, status );
    ShowWindow( hwnd, SW_SHOW );
    status = set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_STAGED, 0, &state );
    ok( !status && state.mode == CLIENT_SURFACE_PRESENTATION_STAGED,
        "show transition mode %u, expected STAGED, status %#x\n", state.mode, status );
    complete_single_surface_generation( hwnd, surface, &state );
    status = set_surface_state( hwnd, surface, 0, 0, &state );
    ok( !status && state.mode == CLIENT_SURFACE_PRESENTATION_DIRECT,
        "published show mode %u, expected DIRECT, status %#x\n", state.mode, status );

    status = set_surface_state( hwnd, surface,
                                CLIENT_SURFACE_STATE_REGISTER |
                                CLIENT_SURFACE_STATE_SCENE_PUBLICATION, 0, &state );
    ok( !status && state.mode == CLIENT_SURFACE_PRESENTATION_COMPOSITED,
        "backend without direct capability mode %u, expected COMPOSITED, status %#x\n",
        state.mode, status );
    set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    DestroyWindow( hwnd );
    pump_messages( 200 );
}

static void test_generation_membership(void)
{
    const UINT_PTR first_surface = 0x12350000, second_surface = 0x12350001;
    struct surface_state state, staged, committed;
    HWND hwnd;
    unsigned int status;

    hwnd = create_test_window( FALSE );
    ok( !!hwnd, "failed to create membership window, error %lu\n", GetLastError() );
    if (!hwnd) return;

    status = set_surface_state( hwnd, first_surface,
                                CLIENT_SURFACE_STATE_REGISTER | CLIENT_SURFACE_STATE_CACHE,
                                0, &state );
    ok( !status, "first register failed, status %#x\n", status );
    ok( state.active == 1 && state.cached == 1,
        "first counts active %u cached %u\n", state.active, state.cached );
    status = set_surface_state( hwnd, first_surface,
                                CLIENT_SURFACE_STATE_REGISTER | CLIENT_SURFACE_STATE_CACHE,
                                0, &state );
    ok( !status, "duplicate register failed, status %#x\n", status );
    ok( state.active == 1 && state.cached == 1,
        "duplicate register changed counts: active %u cached %u\n", state.active, state.cached );
    status = set_surface_state( hwnd, second_surface, CLIENT_SURFACE_STATE_REGISTER, 0, &state );
    ok( !status, "second register failed, status %#x\n", status );
    ok( state.active == 2 && state.cached == 1,
        "second counts active %u cached %u\n", state.active, state.cached );

    ShowWindow( hwnd, SW_SHOW );
    status = set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_STAGED, 0, &staged );
    ok( !status, "membership stage failed, status %#x\n", status );
    ok( staged.staged && staged.generation && staged.pending == 1,
        "unexpected staged state: staged %u generation %s pending %u\n",
        staged.staged, wine_dbgstr_longlong( staged.generation ), staged.pending );

    status = begin_surface_state( hwnd, first_surface, &staged, &committed );
    ok( !status && committed.compose,
        "cached producer was displaced by dormant active surface, compose %u status %#x\n",
        committed.compose, status );

    status = claim_surface_state( hwnd, second_surface, &committed );
    ok( !status && committed.generation != staged.generation && committed.pending == 1,
        "active producer claim did not restart generation: old %s new %s pending %u status %#x\n",
        wine_dbgstr_longlong( staged.generation ),
        wine_dbgstr_longlong( committed.generation ), committed.pending, status );
    staged = committed;
    status = begin_surface_state( hwnd, first_surface, &staged, &committed );
    ok( !status && !committed.compose,
        "displaced cached producer remained selected, compose %u status %#x\n",
        committed.compose, status );
    status = begin_surface_state( hwnd, second_surface, &staged, &committed );
    ok( !status && committed.compose,
        "newest active producer was not selected, compose %u status %#x\n",
        committed.compose, status );
    status = commit_surface_state( hwnd, second_surface, &staged, &committed );
    ok( !status, "selected membership commit failed, status %#x\n", status );
    ok( committed.staged && committed.ready && !committed.pending && !committed.wake,
        "same-window commit was exposed before owner publish: staged %u ready %u pending %u wake %u\n",
        committed.staged, committed.ready, committed.pending, committed.wake );
    status = publish_surface_state( hwnd, &committed );
    ok( !status && !committed.staged && !committed.ready && committed.wake,
        "owner publish failed: staged %u ready %u wake %u status %#x\n",
        committed.staged, committed.ready, committed.wake, status );

    status = set_surface_state( hwnd, first_surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, &state );
    ok( !status, "first unregister failed, status %#x\n", status );
    ok( state.active == 1 && state.cached == 1,
        "cached surface was not retained: active %u cached %u\n", state.active, state.cached );

    ShowWindow( hwnd, SW_HIDE );
    ShowWindow( hwnd, SW_SHOW );
    status = set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_STAGED, 0, &staged );
    ok( !status, "cached stage failed, status %#x\n", status );
    ok( staged.staged && staged.pending == 1,
        "window producer was not normalized: staged %u pending %u\n",
        staged.staged, staged.pending );
    status = set_surface_state( hwnd, first_surface, CLIENT_SURFACE_STATE_UNCACHE, 0, &state );
    ok( !status, "uncache failed, status %#x\n", status );
    ok( state.active == 1 && !state.cached && state.pending == 1 && state.staged && !state.wake,
        "uncache did not retire one surface: active %u cached %u pending %u staged %u wake %u\n",
        state.active, state.cached, state.pending, state.staged, state.wake );
    ok( state.generation == staged.generation,
        "unselected cache removal restarted generation %s\n",
        wine_dbgstr_longlong( state.generation ) );
    status = set_surface_state( hwnd, first_surface,
                                CLIENT_SURFACE_STATE_UNREGISTER | CLIENT_SURFACE_STATE_UNCACHE,
                                0, &state );
    ok( !status, "duplicate removal failed, status %#x\n", status );
    ok( state.active == 1 && !state.cached && state.pending == 1 && state.staged,
        "duplicate removal underflowed state: active %u cached %u pending %u staged %u\n",
        state.active, state.cached, state.pending, state.staged );
    status = commit_surface_state( hwnd, second_surface, &state, &state );
    ok( !status, "remaining commit failed, status %#x\n", status );
    ok( !state.pending && state.staged && state.ready && !state.wake,
        "remaining commit did not become ready: pending %u staged %u ready %u wake %u\n",
        state.pending, state.staged, state.ready, state.wake );
    status = publish_surface_state( hwnd, &state );
    ok( !status && !state.pending && !state.staged && state.wake,
        "remaining generation did not publish: pending %u staged %u wake %u status %#x\n",
        state.pending, state.staged, state.wake, status );
    status = set_surface_state( hwnd, second_surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, &state );
    ok( !status, "second unregister failed, status %#x\n", status );
    ok( !state.active && !state.cached && !state.pending,
        "unexpected final counts: active %u cached %u pending %u\n",
        state.active, state.cached, state.pending );
    DestroyWindow( hwnd );
}

static void test_clip_scene_snapshot(void)
{
    const UINT_PTR first_surface = 0x12370000, second_surface = 0x12370001;
    const UINT_PTR descendant_surface = 0x12370002, duplicate_surface = 0x12370003;
    struct clip_state before, after;
    static const RECT bounds[] =
    {
        {23, 13, 34, 21}, {42, 12, 49, 25}, {10, 10, 60, 50},
        {65, 35, 70, 50}, {20, 20, 20, 30}, {200, 200, 210, 210}, {-5, -5, 20, 20},
    };
    HRGN shape, shape_part;
    HWND parent, first, second, descendant, hidden;
    unsigned int status, i;

    parent = create_test_window( TRUE );
    ok( !!parent, "failed to create clip parent, error %lu\n", GetLastError() );
    if (!parent) return;
    first = create_test_child( parent, 10 );
    second = create_test_child( parent, 20 );
    descendant = first ? create_test_child( first, 5 ) : NULL;
    ok( !!first && !!second && !!descendant, "failed to create clip hierarchy, error %lu\n",
        GetLastError() );
    if (!first || !second || !descendant) goto done;

    SetWindowPos( first, HWND_BOTTOM, 10, 10, 50, 40, SWP_NOACTIVATE );
    SetWindowPos( second, HWND_TOP, 20, 10, 50, 40, SWP_NOACTIVATE );
    set_surface_state( first, first_surface, CLIENT_SURFACE_STATE_REGISTER, 0, NULL );
    claim_surface_state( first, first_surface, NULL );
    status = get_clip_state( first, &before );
    ok( !status && !before.count, "initial clip status %#x count %u\n", status, before.count );

    set_surface_state( second, second_surface, CLIENT_SURFACE_STATE_REGISTER, 0, NULL );
    set_surface_state( descendant, descendant_surface, CLIENT_SURFACE_STATE_REGISTER, 0, NULL );
    status = get_clip_state( first, &after );
    ok( !status, "dormant clip snapshot failed, status %#x\n", status );
    ok( after.scene_generation == before.scene_generation,
        "dormant registration changed scene epoch from %s to %s\n",
        wine_dbgstr_longlong( before.scene_generation ), wine_dbgstr_longlong( after.scene_generation ) );
    ok( !after.count, "dormant surfaces changed the clip in the same scene, count %u\n", after.count );

    status = claim_surface_state( descendant, descendant_surface, NULL );
    ok( !status, "descendant claim failed, status %#x\n", status );
    status = get_clip_state( first, &after );
    ok( !status && after.count == 1 && clip_state_contains( &after, descendant ),
        "claimed descendant clip status %#x count %u\n", status, after.count );
    ok( after.scene_generation != before.scene_generation,
        "first completed descendant frame did not invalidate the clip scene\n" );

    /* A completed cache also occludes, even when a dormant active surface
     * exists for the same HWND.  The active surface takes over after claim. */
    set_surface_state( second, duplicate_surface, CLIENT_SURFACE_STATE_CACHE, 0, NULL );

    status = get_clip_state( first, &before );
    ok( !status, "clip snapshot failed, status %#x\n", status );
    ok( before.toplevel == parent, "clip top %p, expected %p\n", before.toplevel, parent );
    ok( !(before.scene_generation & 1), "unstable scene generation %s\n",
        wine_dbgstr_longlong( before.scene_generation ) );
    ok( before.count == 2, "clip count %u, expected two distinct windows\n", before.count );
    ok( clip_state_contains( &before, second ), "upper sibling missing from clip snapshot\n" );
    ok( clip_state_contains( &before, descendant ), "descendant missing from clip snapshot\n" );

    hidden = CreateWindowExA( 0, "client_surface_test", "hidden non-producer", WS_CHILD,
                              3, 4, 12, 13, parent, NULL, NULL, NULL );
    ok( !!hidden, "failed to create hidden non-producer\n" );
    if (hidden)
    {
        get_clip_state( first, &before );
        ok( DestroyWindow( hidden ), "failed to destroy hidden non-producer\n" );
        status = get_clip_state( first, &after );
        ok( !status && after.scene_generation == before.scene_generation && after.count == before.count,
            "hidden non-producer destruction changed scene %s/%u to %s/%u, status %#x\n",
            wine_dbgstr_longlong( before.scene_generation ), before.count,
            wine_dbgstr_longlong( after.scene_generation ), after.count, status );
    }

    claim_surface_state( second, second_surface, NULL );
    status = get_clip_state( first, &after );
    ok( !status && after.count == before.count,
        "claiming a cached window duplicated its clip, status %#x count %u\n", status, after.count );

    shape = CreateRectRgn( 0, 0, 20, 15 );
    shape_part = CreateRectRgn( 30, 25, 50, 40 );
    ok( !!shape && !!shape_part, "failed to create shaped clip regions\n" );
    if (shape && shape_part)
    {
        CombineRgn( shape, shape, shape_part, RGN_OR );
        DeleteObject( shape_part );
        if (!SetWindowRgn( second, shape, TRUE )) DeleteObject( shape );
        status = get_clip_state( first, &after );
        ok( !status, "shaped clip snapshot failed, status %#x\n", status );
        ok( after.count == 3 && clip_state_count( &after, second ) == 2 &&
            clip_state_contains( &after, descendant ),
            "shaped clip was reduced to its bounds: count %u sibling rects %u descendant %u\n",
            after.count, clip_state_count( &after, second ),
            clip_state_contains( &after, descendant ) );
    }
    else
    {
        if (shape) DeleteObject( shape );
        if (shape_part) DeleteObject( shape_part );
    }

    /* Compare exact tagged rectangles with the unrestricted snapshot, also
     * beyond the target's HWND rectangle: present coverage can differ from
     * Win32 client bounds. Keep shaped and ancestor clipping at each DPI. */
    for (i = 0; i < ARRAY_SIZE(bounds); ++i)
    {
        check_clip_bounds( first, 96, &bounds[i] );
        check_clip_bounds( first, 144, &bounds[i] );
    }
    SetWindowPos( first, NULL, -10, -10, 50, 40, SWP_NOACTIVATE | SWP_NOZORDER );
    check_clip_bounds( first, 96, &bounds[ARRAY_SIZE(bounds) - 1] );
    check_clip_bounds( first, 144, &bounds[ARRAY_SIZE(bounds) - 1] );

    SetWindowPos( first, HWND_TOP, 10, 10, 50, 40, SWP_NOACTIVATE );
    status = get_clip_state( first, &after );
    ok( !status, "post-zorder clip snapshot failed, status %#x\n", status );
    ok( after.scene_generation != before.scene_generation,
        "z-order change did not advance scene generation %s\n",
        wine_dbgstr_longlong( after.scene_generation ) );
    ok( after.count == 1 && clip_state_contains( &after, descendant ),
        "post-zorder clip count %u, descendant %u, sibling %u\n", after.count,
        clip_state_contains( &after, descendant ), clip_state_contains( &after, second ) );

    ShowWindow( descendant, SW_HIDE );
    status = get_clip_state( first, &after );
    ok( !status, "post-hide clip snapshot failed, status %#x\n", status );
    ok( !after.count, "hidden descendant remained in clip snapshot, count %u\n", after.count );

    set_surface_state( descendant, descendant_surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    set_surface_state( second, duplicate_surface, CLIENT_SURFACE_STATE_UNCACHE, 0, NULL );
    set_surface_state( second, second_surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    set_surface_state( first, first_surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
done:
    DestroyWindow( parent );
}

static unsigned int get_scene_regions( HWND top, UINT64 epoch, const void *members, UINT member_bytes,
                                        void *data, UINT size, UINT *required, UINT *returned )
{
    struct __server_request_info info = {0};
    struct get_client_surface_scene_regions_request *req = &info.u.req.get_client_surface_scene_regions_request;
    unsigned int status;

    req->__header.req = REQ_get_client_surface_scene_regions;
    req->handle = wine_server_user_handle( top );
    req->scene_generation = epoch;
    if (member_bytes) wine_server_add_data( &info, members, member_bytes );
    wine_server_set_reply( &info, data, size );
    status = p_wine_server_call( &info );
    *required = info.u.reply.get_client_surface_scene_regions_reply.total_size;
    *returned = wine_server_reply_size( &info.u.reply.get_client_surface_scene_regions_reply );
    return status;
}

static void check_scene_region_batch( HWND top, UINT64 epoch,
                                      const struct client_surface_scene_region_request *members, UINT count )
{
    unsigned char data[16384], *cursor = data;
    UINT status, required, returned, remaining, i, j;

    status = get_scene_regions( top, epoch, members, count * sizeof(*members), data, sizeof(data),
                                 &required, &returned );
    ok( !status && returned == required, "batch status %#x, size %u/%u\n", status, returned, required );
    if (status) return;
    remaining = returned;
    for (i = 0; i < count; ++i)
    {
        struct client_surface_scene_region header;
        struct __server_request_info info = {0};
        struct rectangle visible[64];
        struct get_visible_region_reply *reply = &info.u.reply.get_visible_region_reply;
        struct clip_state clips;
        RECT bounds = wine_server_get_rect( members[i].bounds );
        HWND hwnd = wine_server_ptr_handle( members[i].handle );
        UINT bytes;

        ok( remaining >= sizeof(header), "truncated header %u\n", i );
        if (remaining < sizeof(header)) return;
        memcpy( &header, cursor, sizeof(header) );
        cursor += sizeof(header);
        remaining -= sizeof(header);
        ok( header.handle == members[i].handle && header.window_dpi.num && header.window_dpi.den,
            "member %u handle %#x/%#x dpi %u/%u\n", i, header.handle, members[i].handle,
            header.window_dpi.num, header.window_dpi.den );
        ok( header.visible_count <= remaining / sizeof(RECT), "truncated visible region\n" );
        if (header.visible_count > remaining / sizeof(RECT)) return;
        bytes = header.visible_count * sizeof(RECT);
        if (!(members[i].flags & CLIENT_SURFACE_SCENE_PRESENT_RECT))
        {
            info.u.req.get_visible_region_request.__header.req = REQ_get_visible_region;
            info.u.req.get_visible_region_request.window = members[i].handle;
            info.u.req.get_visible_region_request.flags = members[i].flags;
            if (members[i].flags & DCX_PARENTCLIP)
                info.u.req.get_visible_region_request.flags &= ~DCX_CLIPSIBLINGS;
            wine_server_set_reply( &info, visible, sizeof(visible) );
            status = p_wine_server_call( &info );
            ok( !status && wine_server_reply_size( reply ) == bytes,
                "visible %u status %#x size %u/%u\n", i, status, wine_server_reply_size( reply ), bytes );
            if (status || wine_server_reply_size( reply ) != bytes) return;
            for (j = 0; j < header.visible_count; ++j)
            {
                visible[j].left -= reply->win_rect.left;
                visible[j].right -= reply->win_rect.left;
                visible[j].top -= reply->win_rect.top;
                visible[j].bottom -= reply->win_rect.top;
            }
            ok( !memcmp( visible, cursor, bytes ), "visible region differs for member %u\n", i );
        }
        else ok( !header.visible_count, "present rectangle has DC clip\n" );
        cursor += bytes;
        remaining -= bytes;
        status = get_clip_state_in_bounds( hwnd, members[i].dpi.num, &bounds, &clips );
        ok( !status && clips.scene_generation == epoch && clips.count == header.clip_count,
            "occlusion %u status %#x, count %u/%u\n", i, status, clips.count, header.clip_count );
        if (status || clips.count != header.clip_count || clips.count > ARRAY_SIZE(clips.windows)) return;
        bytes = header.clip_count * sizeof(*clips.windows);
        ok( remaining >= bytes, "truncated occlusion\n" );
        if (remaining < bytes) return;
        ok( !memcmp( cursor, clips.windows, bytes ), "occlusion differs for member %u\n", i );
        cursor += bytes;
        remaining -= bytes;
    }
    ok( !remaining, "unexpected trailing bytes %u\n", remaining );
}

static void test_scene_region_batch(void)
{
    struct client_surface_scene_region_request members[CLIENT_SURFACE_SCENE_BATCH_MAX + 1];
    static const UINT flags[] = {0, DCX_CLIPCHILDREN, DCX_CLIPSIBLINGS,
                                 DCX_PARENTCLIP, DCX_PARENTCLIP | DCX_CLIPSIBLINGS};
    const RECT bounds[] = {{0, 0, 160, 120}, {28, 11, 37, 28}, {0, 0, 0, 0}, {-5, -5, 15, 15}};
    const UINT_PTR identity = 0x12470000;
    unsigned char data[16384];
    struct clip_state state;
    HWND top, first, second, other;
    UINT status, required, returned, i, j;
    UINT64 epoch;
    HRGN shape;

    top = create_test_window( TRUE );
    first = create_test_child( top, 10 );
    second = create_test_child( top, 20 );
    other = create_test_window( TRUE );
    ok( top && first && second && other, "could not create batch clip windows\n" );
    if (!top || !first || !second || !other) goto done;
    SetWindowPos( first, HWND_BOTTOM, 10, 10, 80, 60, SWP_NOACTIVATE );
    SetWindowPos( second, HWND_TOP, 25, 5, 50, 40, SWP_NOACTIVATE );
    shape = CreateRectRgn( 4, 2, 21, 23 );
    SetWindowRgn( second, shape, FALSE );
    set_surface_state( second, identity, CLIENT_SURFACE_STATE_REGISTER, 0, NULL );
    claim_surface_state( second, identity, NULL );
    get_clip_state( first, &state );
    epoch = state.scene_generation;
    memset( members, 0, sizeof(members) );
    for (i = 0; i < ARRAY_SIZE(members); ++i)
    {
        members[i].handle = wine_server_user_handle( first );
        members[i].dpi = (struct ratio){96, 1};
        members[i].bounds = wine_server_rectangle( bounds[0] );
    }
    for (i = 0; i < ARRAY_SIZE(flags); ++i)
        for (j = 0; j < ARRAY_SIZE(bounds); ++j)
        {
            members[0].flags = flags[i];
            members[0].dpi.num = j & 1 ? 144 : 96;
            members[0].bounds = wine_server_rectangle( bounds[j] );
            check_scene_region_batch( top, epoch, members, 2 );
        }
    members[0] = members[1];
    check_scene_region_batch( top, epoch, members, CLIENT_SURFACE_SCENE_BATCH_MAX );
    status = get_scene_regions( top, epoch, members, sizeof(members), data, sizeof(data), &required, &returned );
    ok( status == STATUS_INVALID_PARAMETER && !returned, "oversized batch status %#x size %u\n", status, returned );
    status = get_scene_regions( top, epoch, members, sizeof(*members) - 1, data, sizeof(data), &required, &returned );
    ok( status == STATUS_INVALID_PARAMETER && !returned, "partial member status %#x size %u\n", status, returned );
    status = get_scene_regions( top, epoch, NULL, 0, data, sizeof(data), &required, &returned );
    ok( !status && !returned && !required, "empty batch status %#x size %u/%u\n", status, returned, required );
    status = get_scene_regions( top, epoch, members, 2 * sizeof(*members), data, 1, &required, &returned );
    ok( status == STATUS_BUFFER_OVERFLOW && required > 1 && !returned,
        "short reply status %#x size %u/%u\n", status, returned, required );
    ok( required && required <= sizeof(data), "unexpected required size %u\n", required );
    if (!required || required > sizeof(data)) goto done;
    status = get_scene_regions( top, epoch, members, 2 * sizeof(*members), data, required, &required, &returned );
    ok( !status && returned == required, "exact reply status %#x size %u/%u\n", status, returned, required );
    members[1].handle = wine_server_user_handle( other );
    status = get_scene_regions( top, epoch, members, 2 * sizeof(*members), data, sizeof(data), &required, &returned );
    ok( status == STATUS_INVALID_PARAMETER && !returned, "foreign scene status %#x size %u\n", status, returned );
    members[1] = members[0];
    members[1].dpi.den = 0;
    status = get_scene_regions( top, epoch, members, 2 * sizeof(*members), data, sizeof(data), &required, &returned );
    ok( status == STATUS_INVALID_PARAMETER && !returned, "invalid dpi status %#x size %u\n", status, returned );
    members[1] = members[0];
    members[1].flags = CLIENT_SURFACE_SCENE_PRESENT_RECT;
    status = get_scene_regions( top, epoch, members, 2 * sizeof(*members), data, sizeof(data), &required, &returned );
    ok( status == STATUS_INVALID_PARAMETER && !returned, "child present rect status %#x size %u\n", status, returned );
    members[1].handle = wine_server_user_handle( top );
    check_scene_region_batch( top, epoch, members, 2 );
    status = get_scene_regions( top, epoch | 1, members, sizeof(*members), data, sizeof(data), &required, &returned );
    ok( status == STATUS_RETRY && !returned, "odd epoch status %#x size %u\n", status, returned );
    ShowWindow( second, SW_HIDE );
    status = get_scene_regions( top, epoch, members, sizeof(*members), data, sizeof(data), &required, &returned );
    ok( status == STATUS_RETRY && !returned, "stale epoch status %#x size %u\n", status, returned );
    get_clip_state( first, &state );
    check_scene_region_batch( top, state.scene_generation, members, 2 );
    set_surface_state( second, identity, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
done:
    if (other) DestroyWindow( other );
    if (top) DestroyWindow( top );
}

static void test_complex_clip_snapshot(void)
{
    const UINT_PTR surface = 0x1235a000;
    const UINT side = 256, count = side * side;
    struct __server_request_info info;
    struct client_surface_clip_window *clips = NULL;
    struct rectangle *rects = NULL;
    HWND parent, child = NULL;
    unsigned int status;
    UINT x, y, i;

    parent = create_test_window( TRUE );
    ok( !!parent, "failed to create complex clip parent\n" );
    if (!parent) return;
    SetWindowPos( parent, NULL, 10, 10, 2 * side, 2 * side, SWP_NOACTIVATE | SWP_NOZORDER );
    child = create_test_child( parent, 0 );
    ok( !!child, "failed to create complex clip child\n" );
    if (!child) goto done;
    SetWindowPos( child, NULL, 0, 0, 2 * side, 2 * side, SWP_NOACTIVATE | SWP_NOZORDER );
    rects = malloc( count * sizeof(*rects) );
    clips = malloc( count * sizeof(*clips) );
    ok( !!rects && !!clips, "failed to allocate complex clip data\n" );
    if (!rects || !clips) goto done;
    for (y = i = 0; y < side; ++y)
        for (x = 0; x < side; ++x, ++i)
            rects[i] = (struct rectangle){2 * x, 2 * y, 2 * x + 1, 2 * y + 1};

    /* Install canonical disjoint rectangles at the server boundary; this
     * test concerns the clip protocol, not host XShape request sizing. */
    memset( &info, 0, sizeof(info) );
    info.u.req.set_window_region_request.__header.req = REQ_set_window_region;
    info.u.req.set_window_region_request.window = wine_server_user_handle( child );
    wine_server_add_data( &info, rects, count * sizeof(*rects) );
    status = p_wine_server_call( &info );
    ok( !status, "complex region setup failed, status %#x\n", status );
    if (status) goto done;
    set_surface_state( child, surface, CLIENT_SURFACE_STATE_REGISTER, 0, NULL );
    claim_surface_state( child, surface, NULL );

    memset( &info, 0, sizeof(info) );
    info.u.req.get_client_surface_clip_windows_request.__header.req = REQ_get_client_surface_clip_windows;
    info.u.req.get_client_surface_clip_windows_request.handle = wine_server_user_handle( parent );
    info.u.req.get_client_surface_clip_windows_request.dpi.num = 1;
    info.u.req.get_client_surface_clip_windows_request.dpi.den = 1;
    wine_server_set_reply( &info, clips, count * sizeof(*clips) );
    status = p_wine_server_call( &info );
    ok( !status, "complex clip snapshot failed, status %#x\n", status );
    ok( info.u.reply.get_client_surface_clip_windows_reply.count == count,
        "complex clip count %u, expected %u\n", info.u.reply.get_client_surface_clip_windows_reply.count, count );
    ok( wine_server_reply_size( &info.u.reply ) == count * sizeof(*clips),
        "rectangle reply truncated at handle count: %u bytes, expected %u\n",
        wine_server_reply_size( &info.u.reply ), count * (UINT)sizeof(*clips) );
    set_surface_state( child, surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
done:
    free( clips );
    free( rects );
    if (child) SetWindowRgn( child, NULL, FALSE );
    DestroyWindow( parent );
}

static void test_subtree_generation_retirement(void)
{
    const UINT_PTR first_surface = 0x12360000, second_surface = 0x12360001;
    struct surface_state staged, partial, state;
    HWND parent, first, second;
    unsigned int status;

    parent = create_test_window( FALSE );
    ok( !!parent, "failed to create subtree parent, error %lu\n", GetLastError() );
    if (!parent) return;
    first = create_test_child( parent, 10 );
    second = create_test_child( parent, 70 );
    ok( !!first && !!second, "failed to create subtree children, error %lu\n", GetLastError() );
    if (!first || !second)
    {
        DestroyWindow( parent );
        return;
    }

    status = set_surface_state( first, first_surface, CLIENT_SURFACE_STATE_REGISTER, 0, &state );
    ok( !status, "first child register failed, status %#x\n", status );
    ok( state.toplevel == parent && state.active == 1,
        "first child top %p active %u\n", state.toplevel, state.active );
    status = claim_surface_state( first, first_surface, &state );
    ok( !status, "first child claim failed, status %#x\n", status );
    status = set_surface_state( second, second_surface, CLIENT_SURFACE_STATE_REGISTER, 0, &state );
    ok( !status, "second child register failed, status %#x\n", status );
    ok( state.toplevel == parent && state.active == 1,
        "second child top %p active %u\n", state.toplevel, state.active );
    status = claim_surface_state( second, second_surface, &state );
    ok( !status, "second child claim failed, status %#x\n", status );

    ShowWindow( parent, SW_SHOW );
    status = set_surface_state( parent, 0, CLIENT_SURFACE_STATE_STAGED, 0, &staged );
    ok( !status, "subtree stage failed, status %#x\n", status );
    ok( staged.staged && staged.pending == 2,
        "visible subtree pending count %u staged %u\n", staged.pending, staged.staged );
    status = commit_surface_state( first, first_surface, &staged, &partial );
    ok( !status, "first child commit failed, status %#x\n", status );
    ok( partial.staged && partial.pending == 1 && !partial.wake,
        "partial commit state: staged %u pending %u wake %u\n",
        partial.staged, partial.pending, partial.wake );
    status = commit_surface_state( first, first_surface, &staged, &partial );
    ok( !status, "duplicate child commit failed, status %#x\n", status );
    ok( partial.staged && partial.pending == 1 && !partial.wake,
        "duplicate commit changed pending state: staged %u pending %u wake %u\n",
        partial.staged, partial.pending, partial.wake );

    ShowWindow( second, SW_HIDE );
    status = set_surface_state( parent, 0, 0, 0, &state );
    ok( !status, "state query after child hide failed, status %#x\n", status );
    ok( state.staged && state.pending == 1 && state.generation != staged.generation,
        "scene change did not restart visible subtree: staged %u pending %u generation %s\n",
        state.staged, state.pending, wine_dbgstr_longlong( state.generation ) );
    status = commit_surface_state( first, first_surface, &state, &partial );
    ok( !status && partial.ready && !partial.pending,
        "restarted subtree did not become ready: ready %u pending %u status %#x\n",
        partial.ready, partial.pending, status );
    status = publish_surface_state( parent, &partial );
    ok( !status && !partial.staged && partial.wake,
        "restarted subtree did not publish: staged %u wake %u status %#x\n",
        partial.staged, partial.wake, status );

    ShowWindow( parent, SW_HIDE );
    ShowWindow( parent, SW_SHOW );
    status = set_surface_state( parent, 0, CLIENT_SURFACE_STATE_STAGED, 0, &staged );
    ok( !status, "second subtree stage failed, status %#x\n", status );
    ok( staged.staged && staged.pending == 1,
        "hidden child joined generation: staged %u pending %u\n", staged.staged, staged.pending );
    ShowWindow( second, SW_SHOW );
    status = set_surface_state( parent, 0, 0, 0, &state );
    ok( !status && state.generation != staged.generation && state.pending == 2,
        "shown child did not restart full generation: generation %s pending %u status %#x\n",
        wine_dbgstr_longlong( state.generation ), state.pending, status );
    status = commit_surface_state( second, second_surface, &state, &partial );
    ok( !status, "late child commit failed, status %#x\n", status );
    ok( partial.staged && partial.pending == 1 && !partial.wake,
        "late child changed existing wait: staged %u pending %u wake %u\n",
        partial.staged, partial.pending, partial.wake );
    status = commit_surface_state( first, first_surface, &state, &state );
    ok( !status, "remaining child commit failed, status %#x\n", status );
    ok( state.staged && state.ready && !state.pending && !state.wake,
        "remaining child did not complete: staged %u ready %u pending %u wake %u\n",
        state.staged, state.ready, state.pending, state.wake );
    status = publish_surface_state( parent, &state );
    ok( !status && !state.staged && state.wake,
        "remaining child generation did not publish: staged %u wake %u status %#x\n",
        state.staged, state.wake, status );

    ShowWindow( parent, SW_HIDE );
    ShowWindow( parent, SW_SHOW );
    status = set_surface_state( parent, 0, CLIENT_SURFACE_STATE_STAGED, 0, &staged );
    ok( !status, "destroy subtree stage failed, status %#x\n", status );
    ok( staged.staged && staged.pending == 2,
        "destroy subtree pending count %u staged %u\n", staged.pending, staged.staged );
    status = commit_surface_state( first, first_surface, &staged, &state );
    ok( !status, "pre-destroy child commit failed, status %#x\n", status );
    ok( state.staged && state.pending == 1 && !state.wake,
        "pre-destroy state: staged %u pending %u wake %u\n",
        state.staged, state.pending, state.wake );
    ok( DestroyWindow( second ), "failed to destroy pending child, error %lu\n", GetLastError() );
    status = set_surface_state( parent, 0, 0, 0, &state );
    ok( !status, "state query after child destroy failed, status %#x\n", status );
    ok( state.staged && state.pending == 1 && state.generation != staged.generation,
        "destroyed subtree did not restart remaining scene: staged %u pending %u generation %s\n",
        state.staged, state.pending, wine_dbgstr_longlong( state.generation ) );
    status = commit_surface_state( first, first_surface, &state, &state );
    ok( !status && state.ready && !state.pending,
        "remaining subtree did not become ready: ready %u pending %u status %#x\n",
        state.ready, state.pending, status );
    status = publish_surface_state( parent, &state );
    ok( !status && !state.staged && state.wake,
        "remaining subtree did not publish: staged %u wake %u status %#x\n",
        state.staged, state.wake, status );
    set_surface_state( first, first_surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    DestroyWindow( parent );
}

struct handoff_binding
{
    HANDLE mapping;
    UINT size, offset;
    UINT64 mapping_id, cookie;
};

static unsigned int get_surface_handoff( HWND hwnd, DWORD producer, UINT_PTR surface,
                                         BOOL owner, struct handoff_binding *binding )
{
    struct __server_request_info info = {0};
    const struct get_client_surface_handoff_reply *reply =
        &info.u.reply.get_client_surface_handoff_reply;
    unsigned int status;

    memset( binding, 0, sizeof(*binding) );
    info.u.req.get_client_surface_handoff_request.__header.req = REQ_get_client_surface_handoff;
    info.u.req.get_client_surface_handoff_request.handle = wine_server_user_handle( hwnd );
    info.u.req.get_client_surface_handoff_request.producer = producer;
    info.u.req.get_client_surface_handoff_request.surface = surface;
    info.u.req.get_client_surface_handoff_request.owner = owner;
    status = p_wine_server_call( &info );
    if (!status)
    {
        binding->mapping = wine_server_ptr_handle( reply->mapping );
        binding->size = reply->size;
        binding->offset = reply->offset;
        binding->mapping_id = reply->mapping_id;
        binding->cookie = reply->cookie;
    }
    return status;
}

static unsigned int release_surface_handoff( HWND hwnd, DWORD producer, UINT_PTR surface,
                                              UINT64 cookie, BOOL owner )
{
    struct __server_request_info info = {0};

    info.u.req.release_client_surface_handoff_request.__header.req =
        REQ_release_client_surface_handoff;
    info.u.req.release_client_surface_handoff_request.handle = wine_server_user_handle( hwnd );
    info.u.req.release_client_surface_handoff_request.producer = producer;
    info.u.req.release_client_surface_handoff_request.surface = surface;
    info.u.req.release_client_surface_handoff_request.cookie = cookie;
    info.u.req.release_client_surface_handoff_request.owner = owner;
    return p_wine_server_call( &info );
}

static void check_surface_handoff_cookie( HWND hwnd, UINT_PTR surface, UINT64 cookie )
{
    struct __server_request_info info = {0};
    struct client_surface_handoff_desc desc = {0};
    unsigned int status;

    info.u.req.get_client_surface_handoffs_request.__header.req = REQ_get_client_surface_handoffs;
    info.u.req.get_client_surface_handoffs_request.handle = wine_server_user_handle( hwnd );
    wine_server_set_reply( &info, &desc, sizeof(desc) );
    status = p_wine_server_call( &info );
    ok( !status, "handoff roster status %#x\n", status );
    ok( info.u.reply.get_client_surface_handoffs_reply.count == 1 &&
        wine_server_reply_size( &info.u.reply ) == sizeof(desc), "unexpected handoff roster size\n" );
    ok( desc.handle == wine_server_user_handle( hwnd ) && desc.process == GetCurrentProcessId() &&
        desc.surface == surface, "unexpected handoff roster member\n" );
    ok( desc.cookie == cookie, "handoff roster cookie %s, expected %s\n",
        wine_dbgstr_longlong( desc.cookie ), wine_dbgstr_longlong( cookie ) );
    ok( desc.visible == !!IsWindowVisible( hwnd ), "handoff roster visibility %u, expected %u\n",
        desc.visible, !!IsWindowVisible( hwnd ) );
}

static unsigned int complete_surface_handoffs( HWND hwnd, UINT64 generation, UINT64 epoch,
                                               const struct client_surface_handoff_receipt *receipts,
                                               unsigned int count,
                                               BOOL *accepted )
{
    struct __server_request_info info = {0};
    const struct complete_client_surface_handoffs_reply *reply =
        &info.u.reply.complete_client_surface_handoffs_reply;
    unsigned int status;

    info.u.req.complete_client_surface_handoffs_request.__header.req =
        REQ_complete_client_surface_handoffs;
    info.u.req.complete_client_surface_handoffs_request.handle = wine_server_user_handle( hwnd );
    info.u.req.complete_client_surface_handoffs_request.generation = generation;
    info.u.req.complete_client_surface_handoffs_request.scene_generation = epoch;
    if (count) wine_server_add_data( &info, receipts, count * sizeof(*receipts) );
    status = p_wine_server_call( &info );
    if (!status && accepted) *accepted = reply->accepted;
    return status;
}

static void test_handoff_receipts(void)
{
    const UINT_PTR identity = 0x79580000;
    struct handoff_binding producer = {0}, owner = {0};
    struct client_surface_handoff_receipt receipt;
    struct client_surface_handoff_channel *channel;
    struct surface_state state, before;
    struct __server_request_info info = {0};
    HWND hwnd = create_test_window( FALSE );
    unsigned int status;
    void *view = NULL;
    BOOL accepted;

    ok( !!hwnd, "failed to create receipt window\n" );
    if (!hwnd) return;
    status = set_surface_state( hwnd, identity,
        CLIENT_SURFACE_STATE_REGISTER | CLIENT_SURFACE_STATE_SCENE_PUBLICATION, 0, NULL );
    ok( !status, "receipt registration status %#x\n", status );
    status = claim_surface_state( hwnd, identity, NULL );
    ok( !status, "receipt claim status %#x\n", status );
    status = get_surface_handoff( hwnd, 0, identity, FALSE, &producer );
    ok( !status, "receipt producer bind status %#x\n", status );
    if (status) goto done;
    view = MapViewOfFile( producer.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, producer.size );
    ok( !!view, "receipt source mapping failed %lu\n", GetLastError() );
    if (!view) goto done;
    channel = (void *)((char *)view + producer.offset);
    ok( producer.offset + sizeof(*channel) <= producer.size, "channel exceeds its mapping\n" );
    ok( channel->cookie == producer.cookie && channel->identity == identity,
        "source channel has an unrelated identity\n" );
    status = get_surface_handoff( hwnd, GetCurrentProcessId(), identity, TRUE, &owner );
    ok( !status, "receipt owner bind status %#x\n", status );
    if (status) goto done;
    ShowWindow( hwnd, SW_SHOW );
    status = set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_STAGED, 0, &state );
    ok( !status && state.staged && state.pending == 1,
        "receipt scene status %#x staged %u pending %u\n", status, state.staged, state.pending );

    /* The owner has finished reading image 1. Its receipt must remain usable
     * even after that image has been returned and reserved for newer work. */
    receipt = (struct client_surface_handoff_receipt){
        .handle = wine_server_user_handle( hwnd ), .process = GetCurrentProcessId(),
        .surface = identity, .cookie = producer.cookie, .source_generation = 7, .buffer_index = 1,
    };
    channel->slots[1].source_sequence = 8;
    __atomic_store_n( &channel->producer_sequence, 2, __ATOMIC_RELEASE );
    __atomic_store_n( &channel->consumer_sequence, 2, __ATOMIC_RELEASE );
    status = complete_surface_handoffs( hwnd, state.generation, state.scene_generation, NULL, 0, &accepted );
    ok( !status && !accepted, "missing receipt accepted, status %#x\n", status );
    ++receipt.cookie;
    status = complete_surface_handoffs( hwnd, state.generation, state.scene_generation, &receipt, 1, &accepted );
    ok( !status && !accepted, "stale binding receipt accepted, status %#x\n", status );
    --receipt.cookie;
    receipt.buffer_index = CLIENT_SURFACE_HANDOFF_RING_SIZE;
    status = complete_surface_handoffs( hwnd, state.generation, state.scene_generation, &receipt, 1, &accepted );
    ok( !status && !accepted, "out-of-range source receipt accepted, status %#x\n", status );
    receipt.buffer_index = 1;

    before = state;
    info.u.req.cancel_client_surface_handoffs_request.__header.req = REQ_cancel_client_surface_handoffs;
    info.u.req.cancel_client_surface_handoffs_request.handle = wine_server_user_handle( hwnd );
    info.u.req.cancel_client_surface_handoffs_request.generation = state.generation;
    info.u.req.cancel_client_surface_handoffs_request.scene_generation = state.scene_generation;
    status = p_wine_server_call( &info );
    ok( !status, "assembly cancellation status %#x\n", status );
    status = set_surface_state( hwnd, 0, 0, 0, &state );
    ok( !status && state.scene_generation != before.scene_generation && state.pending == 1,
        "cancel did not request a complete replay, status %#x pending %u\n", status, state.pending );
    status = complete_surface_handoffs( hwnd, before.generation, before.scene_generation, &receipt, 1, &accepted );
    ok( !status && !accepted, "cancelled scene accepted a late receipt, status %#x\n", status );
    status = complete_surface_handoffs( hwnd, state.generation, state.scene_generation, &receipt, 1, &accepted );
    ok( !status && accepted, "returned source receipt rejected, status %#x accepted %u\n", status, accepted );
    status = complete_surface_handoffs( hwnd, state.generation, state.scene_generation, &receipt, 1, &accepted );
    ok( !status && !accepted, "publication ticket reserved twice, status %#x\n", status );
    before = state;
    status = set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_FAILED, 0, NULL );
    ok( !status, "failed native publication status %#x\n", status );
    status = set_surface_state_scene( hwnd, 0, CLIENT_SURFACE_STATE_PUBLISH_COMMIT,
        before.generation, before.scene_generation, &state );
    ok( !status && state.staged && state.scene_generation != before.scene_generation,
        "late ACK exposed failed publication, status %#x staged %u\n", status, state.staged );
    status = set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_GEOMETRY_READY, 0, &state );
    ok( !status && state.staged && state.pending == 1,
        "failed publication did not restart on owner activity, status %#x pending %u\n", status, state.pending );
    status = complete_surface_handoffs( hwnd, state.generation, state.scene_generation, &receipt, 1, &accepted );
    ok( !status && accepted, "replacement publication receipt rejected, status %#x\n", status );
    status = set_surface_state_scene( hwnd, 0, CLIENT_SURFACE_STATE_PUBLISH_COMMIT,
        state.generation, state.scene_generation, &state );
    ok( !status && !state.staged, "receipt publication failed, status %#x staged %u\n", status, state.staged );
done:
    set_surface_state( hwnd, identity, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    if (producer.cookie) release_surface_handoff( hwnd, 0, identity, producer.cookie, FALSE );
    if (owner.cookie) release_surface_handoff( hwnd, GetCurrentProcessId(), identity, owner.cookie, TRUE );
    if (view) UnmapViewOfFile( view );
    if (producer.mapping) CloseHandle( producer.mapping );
    if (owner.mapping) CloseHandle( owner.mapping );
    DestroyWindow( hwnd );
}

static void test_handoff_storage(void)
{
    const UINT_PTR identity = 0x79400000;
    const UINT flags = CLIENT_SURFACE_STATE_REGISTER | CLIENT_SURFACE_STATE_SCENE_PUBLICATION;
    struct handoff_binding producer = {0}, owner = {0}, replacement = {0}, adjacent = {0}, denied;
    struct client_surface_handoff_shared *producer_shared = NULL, *owner_shared = NULL;
    struct client_surface_handoff_channel *producer_slot = NULL, *owner_slot = NULL, *other_channel = NULL;
    HWND hwnd = create_test_window( FALSE ), other = create_test_window( FALSE );
    UINT64 produced, consumed, old_cookie = 0;
    unsigned int round, i;
    BOOL producer_bound = FALSE, owner_bound = FALSE;
    BOOL accepted = TRUE;
    unsigned int status, index;
    void *producer_view = NULL, *owner_view = NULL;

    ok( hwnd && other, "failed to create handoff storage windows\n" );
    if (!hwnd || !other) goto done;
    status = set_surface_state( hwnd, identity, flags, 0, NULL );
    ok( !status, "handoff registration status %#x\n", status );
    if (status) goto done;
    status = claim_surface_state( hwnd, identity, NULL );
    ok( !status, "handoff claim status %#x\n", status );
    if (status) goto unregister;
    status = get_surface_handoff( hwnd, GetCurrentProcessId() + 1, identity, FALSE, &denied );
    ok( status == STATUS_ACCESS_DENIED, "foreign producer endpoint status %#x\n", status );
    if (!status) CloseHandle( denied.mapping );

    status = get_surface_handoff( hwnd, 0, identity, FALSE, &producer );
    ok( !status, "producer handoff bind status %#x\n", status );
    if (status) goto unregister;
    producer_bound = TRUE;
    producer_view = MapViewOfFile( producer.mapping, FILE_MAP_READ | FILE_MAP_WRITE,
                                   0, 0, producer.size );
    CloseHandle( producer.mapping );
    producer.mapping = NULL;
    ok( !!producer_view, "producer handoff map error %lu\n", GetLastError() );
    if (!producer_view) goto unregister;

    status = get_surface_handoff( hwnd, GetCurrentProcessId(), identity, TRUE, &owner );
    ok( !status, "owner handoff bind status %#x\n", status );
    if (status) goto unregister;
    owner_bound = TRUE;
    owner_view = MapViewOfFile( owner.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, owner.size );
    CloseHandle( owner.mapping );
    owner.mapping = NULL;
    ok( !!owner_view, "owner handoff map error %lu\n", GetLastError() );
    if (!owner_view) goto unregister;

    ok( producer.size == sizeof(*producer_shared) && producer.size == owner.size &&
        producer.offset == owner.offset && producer.mapping_id == owner.mapping_id &&
        producer.cookie == owner.cookie && producer.cookie && producer.mapping_id,
        "producer/owner handoff bindings differ\n" );
    ok( producer.offset >= offsetof(struct client_surface_handoff_shared, channels) &&
        producer.offset + sizeof(*producer_slot) <= producer.size && !(producer.offset % 64),
        "invalid handoff slot layout size %u offset %u\n", producer.size, producer.offset );
    producer_shared = producer_view;
    owner_shared = owner_view;
    producer_slot = (void *)((char *)producer_view + producer.offset);
    owner_slot = (void *)((char *)owner_view + owner.offset);
    index = producer_slot - producer_shared->channels;
    ok( producer_shared->magic == CLIENT_SURFACE_HANDOFF_MAGIC &&
        producer_shared->version == CLIENT_SURFACE_HANDOFF_VERSION &&
        producer_shared->channel_count == CLIENT_SURFACE_HANDOFF_CHANNELS &&
        producer_shared->mapping_id == producer.mapping_id,
        "invalid producer handoff header\n" );
    ok( owner_shared->magic == CLIENT_SURFACE_HANDOFF_MAGIC &&
        owner_shared->mapping_id == producer.mapping_id,
        "owner mapped a different handoff pool\n" );
    ok( producer_slot->cookie == producer.cookie && producer_slot->identity == identity &&
        producer_slot->producer_process == GetCurrentProcessId() &&
        producer_slot->window == HandleToUlong( hwnd ) && producer_slot->toplevel == HandleToUlong( hwnd ),
        "invalid initialized handoff identity\n" );
    ok( producer_slot->endpoints == (CLIENT_SURFACE_HANDOFF_ENDPOINT_PRODUCER |
                                     CLIENT_SURFACE_HANDOFF_ENDPOINT_CONSUMER),
        "handoff endpoints %#lx\n", producer_slot->endpoints );
    ok( !producer_slot->producer_sequence && !producer_slot->consumer_sequence && !producer_slot->closed,
        "new channel is not empty and open\n" );
    status = set_surface_state( other, identity + 1, flags, 0, NULL );
    ok( !status, "adjacent surface registration status %#x\n", status );
    status = get_surface_handoff( other, 0, identity + 1, FALSE, &adjacent );
    ok( !status, "adjacent channel bind status %#x\n", status );
    if (!status)
    {
        ok( adjacent.mapping_id == producer.mapping_id && adjacent.offset != producer.offset,
            "surfaces did not receive separate channels in the process-pair mapping\n" );
        if (adjacent.mapping_id == producer.mapping_id && adjacent.offset != producer.offset)
        {
            other_channel = (void *)((char *)producer_view + adjacent.offset);
            __atomic_store_n( &other_channel->producer_sequence, 1, __ATOMIC_RELEASE );
        }
        CloseHandle( adjacent.mapping );
        adjacent.mapping = NULL;
    }
    /* Use both mappings, fill the whole ring before returning any descriptor,
     * and cross UINT64 wrap. Adjacent surfaces keep independent sequences. */
    for (round = 0; round < 3; ++round)
    {
        UINT64 start = round == 2 ? ~(UINT64)0 - 1 : round * CLIENT_SURFACE_HANDOFF_RING_SIZE;

        __atomic_store_n( &producer_slot->producer_sequence, start, __ATOMIC_RELEASE );
        __atomic_store_n( &owner_slot->consumer_sequence, start, __ATOMIC_RELEASE );
        for (i = 0; i < CLIENT_SURFACE_HANDOFF_RING_SIZE; ++i)
        {
            struct client_surface_handoff_slot *frame =
                &producer_slot->slots[(start + i) & (CLIENT_SURFACE_HANDOFF_RING_SIZE - 1)];

            frame->source = 0x12345678 + i;
            frame->source_sequence = i + 1;
            __atomic_store_n( &producer_slot->producer_sequence, start + i + 1, __ATOMIC_RELEASE );
            ok( !client_surface_handoff_consumed( producer_slot, start + i + 1 ),
                "unread descriptor %u was returned at round %u\n", i, round );
        }
        __atomic_fetch_or( &producer_shared->ready_bitmap[index / 64],
                           (UINT64)1 << (index % 64), __ATOMIC_RELEASE );
        __atomic_fetch_and( &owner_shared->ready_bitmap[index / 64],
                            ~((UINT64)1 << (index % 64)), __ATOMIC_ACQ_REL );
        produced = __atomic_load_n( &owner_slot->producer_sequence, __ATOMIC_ACQUIRE );
        consumed = __atomic_load_n( &owner_slot->consumer_sequence, __ATOMIC_RELAXED );
        ok( produced - consumed == CLIENT_SURFACE_HANDOFF_RING_SIZE, "ring was not full\n" );
        for (i = 0; i < CLIENT_SURFACE_HANDOFF_RING_SIZE; ++i)
        {
            const struct client_surface_handoff_slot *frame =
                &owner_slot->slots[consumed & (CLIENT_SURFACE_HANDOFF_RING_SIZE - 1)];

            ok( frame->source == 0x12345678 + i && frame->source_sequence == i + 1,
                "descriptor %u at round %u was overwritten\n", i, round );
            __atomic_store_n( &owner_slot->consumer_sequence, ++consumed, __ATOMIC_RELEASE );
            ok( client_surface_handoff_consumed( producer_slot, consumed ),
                "producer did not observe returned descriptor %u at round %u\n", i, round );
        }
        ok( produced == consumed, "ring did not drain\n" );
        if (other_channel)
            ok( other_channel->producer_sequence == 1 && !other_channel->consumer_sequence &&
                !other_channel->closed, "ring operation changed an adjacent channel\n" );
    }
    status = complete_surface_handoffs( hwnd, 0, 0, NULL, 0, &accepted );
    ok( !status && !accepted, "idle handoff completion status %#x accepted %u\n", status, accepted );

    old_cookie = producer.cookie;
    status = release_surface_handoff( hwnd, 0, identity, old_cookie + 1, FALSE );
    ok( status == STATUS_INVALID_PARAMETER, "wrong producer release status %#x\n", status );
    status = set_surface_state( hwnd, identity, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    ok( !status, "handoff unregister status %#x\n", status );
    ok( __atomic_load_n( &producer_slot->closed, __ATOMIC_ACQUIRE ), "unregister did not close channel\n" );
    ok( producer_slot->producer_sequence == produced && producer_slot->consumer_sequence == consumed,
        "unregister changed channel sequences\n" );
    status = release_surface_handoff( hwnd, 0, identity, old_cookie, FALSE );
    ok( !status, "producer handoff release status %#x\n", status );
    producer_bound = FALSE;
    ok( owner_slot->endpoints == CLIENT_SURFACE_HANDOFF_ENDPOINT_CONSUMER,
        "producer endpoint release left %#lx\n", owner_slot->endpoints );
    status = release_surface_handoff( hwnd, GetCurrentProcessId(), identity, old_cookie, TRUE );
    ok( !status, "owner handoff release status %#x\n", status );
    owner_bound = FALSE;
    ok( !owner_slot->endpoints, "owner endpoint release left %#lx\n", owner_slot->endpoints );
    ok( !(__atomic_load_n( &owner_shared->ready_bitmap[index / 64], __ATOMIC_ACQUIRE ) &
          ((UINT64)1 << (index % 64))), "retired handoff left a ready bit set\n" );

    set_surface_state( other, identity + 1, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    if (adjacent.cookie)
    {
        release_surface_handoff( other, 0, identity + 1, adjacent.cookie, FALSE );
        adjacent.cookie = 0;
    }
    status = set_surface_state( other, identity, flags, 0, NULL );
    ok( !status, "handoff identity reuse registration status %#x\n", status );
    status = get_surface_handoff( other, 0, identity, FALSE, &replacement );
    ok( !status, "replacement handoff bind status %#x\n", status );
    if (!status)
    {
        CloseHandle( replacement.mapping );
        replacement.mapping = NULL;
        ok( replacement.cookie != old_cookie,
            "replacement handoff reused cookie %s\n", wine_dbgstr_longlong( old_cookie ) );
        status = release_surface_handoff( other, 0, identity, old_cookie, FALSE );
        ok( status == STATUS_INVALID_PARAMETER, "stale cookie released replacement, status %#x\n", status );
        status = release_surface_handoff( other, 0, identity, replacement.cookie, FALSE );
        ok( !status, "replacement handoff release status %#x\n", status );
    }
    set_surface_state( other, identity, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    goto done;

unregister:
    set_surface_state( hwnd, identity, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
done:
    if (other)
    {
        set_surface_state( other, identity + 1, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
        if (adjacent.cookie) release_surface_handoff( other, 0, identity + 1, adjacent.cookie, FALSE );
    }
    if (producer.mapping) CloseHandle( producer.mapping );
    if (owner.mapping) CloseHandle( owner.mapping );
    if (replacement.mapping) CloseHandle( replacement.mapping );
    if (producer_bound)
        release_surface_handoff( hwnd, 0, identity, producer.cookie, FALSE );
    if (owner_bound)
        release_surface_handoff( hwnd, GetCurrentProcessId(), identity, owner.cookie, TRUE );
    if (producer_view) UnmapViewOfFile( producer_view );
    if (owner_view) UnmapViewOfFile( owner_view );
    if (hwnd) DestroyWindow( hwnd );
    if (other) DestroyWindow( other );
}

static void test_handoff_consumer_retirement( BOOL failed )
{
    const UINT_PTR identity = 0x79480000;
    const UINT flags = CLIENT_SURFACE_STATE_REGISTER | CLIENT_SURFACE_STATE_SCENE_PUBLICATION;
    struct handoff_binding producer = {0}, owner = {0}, replacement = {0}, pending = {0};
    struct client_surface_handoff_shared *shared;
    struct client_surface_handoff_channel *slot;
    struct __server_request_info info;
    void *producer_view = NULL, *owner_view = NULL, *replacement_view = NULL;
    UINT64 old_cookie = 0;
    LONG release_sequence;
    BOOL producer_bound = FALSE, owner_bound = FALSE, replacement_bound = FALSE;
    unsigned int status, i;
    HWND hwnd = create_test_window( TRUE );
    HWND other = NULL;

    ok( !!hwnd, "failed to create handoff recovery window\n" );
    if (!hwnd) return;
    status = set_surface_state( hwnd, identity, flags, 0, NULL );
    ok( !status, "handoff recovery registration status %#x\n", status );
    status = claim_surface_state( hwnd, identity, NULL );
    ok( !status, "handoff recovery claim status %#x\n", status );
    check_surface_handoff_cookie( hwnd, identity, 0 );
    status = get_surface_handoff( hwnd, 0, identity, FALSE, &producer );
    ok( !status, "handoff recovery producer bind status %#x\n", status );
    if (status) goto done;
    producer_bound = TRUE;
    producer_view = MapViewOfFile( producer.mapping, FILE_MAP_READ | FILE_MAP_WRITE,
                                   0, 0, producer.size );
    CloseHandle( producer.mapping );
    producer.mapping = NULL;
    ok( !!producer_view, "handoff recovery producer map error %lu\n", GetLastError() );
    if (!producer_view) goto done;
    check_surface_handoff_cookie( hwnd, identity, 0 );

    status = get_surface_handoff( hwnd, GetCurrentProcessId(), identity, TRUE, &owner );
    ok( !status, "handoff recovery consumer bind status %#x\n", status );
    if (status) goto done;
    owner_bound = TRUE;
    owner_view = MapViewOfFile( owner.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, owner.size );
    CloseHandle( owner.mapping );
    owner.mapping = NULL;
    ok( !!owner_view, "handoff recovery consumer map error %lu\n", GetLastError() );
    if (!owner_view) goto done;
    check_surface_handoff_cookie( hwnd, identity, owner.cookie );

    /* Hiding removes a layer from composition, not its authenticated channel
     * or cached frame. The hidden roster must keep the selected producer and
     * reusable owner binding. */
    ShowWindow( hwnd, SW_HIDE );
    check_surface_handoff_cookie( hwnd, identity, owner.cookie );
    ShowWindow( hwnd, SW_SHOW );
    check_surface_handoff_cookie( hwnd, identity, owner.cookie );

    slot = (void *)((char *)producer_view + producer.offset);
    /* Leave the ring full while changing its consumer lifetime. Retirement
     * must close the channel without acknowledging these unread descriptors. */
    __atomic_store_n( &slot->producer_sequence, CLIENT_SURFACE_HANDOFF_RING_SIZE, __ATOMIC_RELEASE );
    if (!failed)
    {
        /* A hidden owner can release and reacquire its unchanged binding.
         * Keep the only completed image for the subsequent show. */
        status = release_surface_handoff( hwnd, GetCurrentProcessId(), identity, owner.cookie, TRUE );
        ok( !status, "temporary consumer release status %#x\n", status );
        owner_bound = FALSE;
        check_surface_handoff_cookie( hwnd, identity, 0 );
        ok( !slot->closed && !slot->consumer_sequence &&
            slot->producer_sequence == CLIENT_SURFACE_HANDOFF_RING_SIZE,
            "temporary consumer release discarded unread descriptors\n" );
        status = get_surface_handoff( hwnd, GetCurrentProcessId(), identity, TRUE, &owner );
        ok( !status, "temporary consumer rebind status %#x\n", status );
        if (status) goto done;
        owner_bound = TRUE;
        ok( owner.cookie == producer.cookie, "temporary consumer rebind replaced the cookie\n" );
        CloseHandle( owner.mapping );
        owner.mapping = NULL;
        check_surface_handoff_cookie( hwnd, identity, owner.cookie );

        other = create_test_window( TRUE );
        ok( !!other, "failed to create replacement owner\n" );
        if (!other) goto done;
        /* Reparent at the server boundary while the old owner is reading.
         * Returning to the original root must not revive this old binding. */
        for (i = 0; i < 2; ++i)
        {
            memset( &info, 0, sizeof(info) );
            info.u.req.set_parent_request.__header.req = REQ_set_parent;
            info.u.req.set_parent_request.handle = wine_server_user_handle( hwnd );
            info.u.req.set_parent_request.parent = wine_server_user_handle( i ? GetDesktopWindow() : other );
            status = p_wine_server_call( &info );
            ok( !status, "reparent %u status %#x\n", i, status );
            check_surface_handoff_cookie( hwnd, identity, 0 );
            status = get_surface_handoff( hwnd, 0, identity, FALSE, &pending );
            ok( status == STATUS_DEVICE_BUSY, "reparent %u reacquired retired binding, status %#x\n", i, status );
            if (!status) CloseHandle( pending.mapping );
            ok( !__atomic_load_n( &slot->consumer_sequence, __ATOMIC_ACQUIRE ),
                "reparent %u acknowledged an unfinished owner read\n", i );
        }
    }
    if (failed) __atomic_store_n( &slot->closed, 1, __ATOMIC_RELEASE );
    check_surface_handoff_cookie( hwnd, identity, 0 );
    shared = producer_view;
    __atomic_store_n( &shared->release_parked, 1, __ATOMIC_RELEASE );
    release_sequence = __atomic_load_n( &shared->release_sequence, __ATOMIC_ACQUIRE );
    old_cookie = producer.cookie;
    status = release_surface_handoff( hwnd, GetCurrentProcessId(), identity,
                                      owner.cookie, TRUE );
    ok( !status, "handoff recovery consumer release status %#x\n", status );
    owner_bound = FALSE;
    ok( __atomic_load_n( &shared->release_sequence, __ATOMIC_ACQUIRE ) != release_sequence,
        "retired consumer did not wake source waiters\n" );
    ok( __atomic_load_n( &slot->closed, __ATOMIC_ACQUIRE ), "retired consumer left channel open\n" );
    ok( !slot->consumer_sequence && slot->producer_sequence == CLIENT_SURFACE_HANDOFF_RING_SIZE,
        "retirement changed channel sequences\n" );
    status = get_surface_handoff( hwnd, GetCurrentProcessId(), identity, TRUE, &pending );
    ok( status == STATUS_DEVICE_BUSY, "reacquired lost binding while producer still mapped, status %#x\n", status );
    if (!status)
    {
        CloseHandle( pending.mapping );
        release_surface_handoff( hwnd, GetCurrentProcessId(), identity, pending.cookie, TRUE );
    }
    status = release_surface_handoff( hwnd, 0, identity, producer.cookie, FALSE );
    ok( !status, "handoff recovery producer release status %#x\n", status );
    producer_bound = FALSE;
    UnmapViewOfFile( owner_view );
    owner_view = NULL;
    UnmapViewOfFile( producer_view );
    producer_view = NULL;

    status = get_surface_handoff( hwnd, 0, identity, FALSE, &replacement );
    ok( !status, "handoff recovery replacement bind status %#x\n", status );
    if (status) goto done;
    replacement_bound = TRUE;
    check_surface_handoff_cookie( hwnd, identity, 0 );
    replacement_view = MapViewOfFile( replacement.mapping, FILE_MAP_READ | FILE_MAP_WRITE,
                                      0, 0, replacement.size );
    CloseHandle( replacement.mapping );
    replacement.mapping = NULL;
    ok( !!replacement_view, "handoff recovery replacement map error %lu\n", GetLastError() );
    if (replacement_view)
    {
        shared = replacement_view;
        slot = (void *)((char *)replacement_view + replacement.offset);
        ok( replacement.cookie != old_cookie && slot->cookie == replacement.cookie,
            "handoff recovery reused stale cookie %s\n", wine_dbgstr_longlong( old_cookie ) );
        ok( !slot->producer_sequence && !slot->consumer_sequence && !slot->closed,
            "replacement channel was not reset\n" );
        ok( slot->endpoints == CLIENT_SURFACE_HANDOFF_ENDPOINT_PRODUCER,
            "handoff recovery replacement endpoints %#lx\n", slot->endpoints );
        ok( !(__atomic_load_n( &shared->ready_bitmap[
                                  (slot - shared->channels) / 64], __ATOMIC_ACQUIRE ) &
              ((UINT64)1 << ((slot - shared->channels) % 64))),
            "handoff recovery replacement retained a ready bit\n" );
    }
done:
    if (producer.mapping) CloseHandle( producer.mapping );
    if (owner.mapping) CloseHandle( owner.mapping );
    if (replacement.mapping) CloseHandle( replacement.mapping );
    if (producer_bound) release_surface_handoff( hwnd, 0, identity, producer.cookie, FALSE );
    if (owner_bound)
        release_surface_handoff( hwnd, GetCurrentProcessId(), identity, owner.cookie, TRUE );
    if (replacement_bound)
        release_surface_handoff( hwnd, 0, identity, replacement.cookie, FALSE );
    if (producer_view) UnmapViewOfFile( producer_view );
    if (owner_view) UnmapViewOfFile( owner_view );
    if (replacement_view) UnmapViewOfFile( replacement_view );
    set_surface_state( hwnd, identity, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    DestroyWindow( hwnd );
    if (other) DestroyWindow( other );
}

static void test_handoff_lost_recovery(void)
{
    unsigned int failed;

    for (failed = 0; failed < 2; ++failed)
    {
        winetest_push_context( "failed copy %u", failed );
        test_handoff_consumer_retirement( failed );
        winetest_pop_context();
    }
}

#define HANDOFF_EXIT_ID 0x79500000

static void handoff_storage_exit_child( HWND hwnd, HANDLE ready, HANDLE release,
                                        BOOL completed )
{
    struct handoff_binding binding;
    struct client_surface_handoff_shared *shared;
    struct client_surface_handoff_channel *slot;
    unsigned int status, index;
    void *view = NULL;

    status = set_surface_state( hwnd, HANDOFF_EXIT_ID,
                                CLIENT_SURFACE_STATE_REGISTER |
                                CLIENT_SURFACE_STATE_SCENE_PUBLICATION, 0, NULL );
    ok( !status, "exit child handoff registration status %#x\n", status );
    status = claim_surface_state( hwnd, HANDOFF_EXIT_ID, NULL );
    ok( !status, "exit child handoff claim status %#x\n", status );
    status = get_surface_handoff( hwnd, 0, HANDOFF_EXIT_ID, FALSE, &binding );
    ok( !status, "exit child producer bind status %#x\n", status );
    if (status) goto done;
    view = MapViewOfFile( binding.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, binding.size );
    CloseHandle( binding.mapping );
    ok( !!view, "exit child producer map error %lu\n", GetLastError() );
    if (!view) goto done;
    shared = view;
    slot = (void *)((char *)view + binding.offset);
    index = slot - shared->channels;
    ok( !slot->producer_sequence && !slot->consumer_sequence && !slot->closed,
        "exit test channel was not empty\n" );
    if (completed)
    {
        slot->slots[0].source_sequence = 1;
        __atomic_store_n( &slot->producer_sequence, 1, __ATOMIC_RELEASE );
        __atomic_fetch_or( &shared->ready_bitmap[index / 64],
                           (UINT64)1 << (index % 64), __ATOMIC_RELEASE );
    }
done:
    SetEvent( ready );
    ok( WaitForSingleObject( release, 10000 ) == WAIT_OBJECT_0,
        "exit child release timed out\n" );
    /* Deliberately omit endpoint release, unmap and unregister. */
}

static void test_handoff_storage_process_exit( char **argv, BOOL completed )
{
    SECURITY_ATTRIBUTES attr = {sizeof(attr), NULL, TRUE};
    STARTUPINFOA startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process = {0};
    struct handoff_binding owner = {0};
    struct client_surface_handoff_shared *shared;
    struct client_surface_handoff_channel *slot = NULL;
    struct surface_state state;
    HWND hwnd = create_test_window( FALSE );
    HANDLE ready = NULL, release = NULL;
    unsigned int status, index = 0;
    char command[MAX_PATH * 2];
    void *view = NULL;

    ok( !!hwnd, "failed to create handoff process-exit window\n" );
    if (!hwnd) return;
    ready = CreateEventA( &attr, TRUE, FALSE, NULL );
    release = CreateEventA( &attr, TRUE, FALSE, NULL );
    ok( ready && release, "failed to create handoff process-exit events\n" );
    if (!ready || !release) goto done;
    sprintf( command, "\"%s\" %s handoff_storage_exit_child %p %p %p %u",
             argv[0], argv[1], hwnd, ready, release, completed );
    if (!CreateProcessA( NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process ))
    {
        ok( 0, "handoff exit child CreateProcess error %lu\n", GetLastError() );
        goto done;
    }
    ok( WaitForSingleObject( ready, 10000 ) == WAIT_OBJECT_0,
        "handoff exit child did not become ready\n" );
    status = get_surface_handoff( hwnd, process.dwProcessId, HANDOFF_EXIT_ID, TRUE, &owner );
    ok( !status, "exit child owner bind status %#x\n", status );
    if (!status)
    {
        view = MapViewOfFile( owner.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, owner.size );
        CloseHandle( owner.mapping );
        owner.mapping = NULL;
        ok( !!view, "exit child owner map error %lu\n", GetLastError() );
    }
    if (view)
    {
        shared = view;
        slot = (void *)((char *)view + owner.offset);
        index = slot - shared->channels;
        ok( __atomic_load_n( &slot->producer_sequence, __ATOMIC_ACQUIRE ) == !!completed &&
            !slot->consumer_sequence && !slot->closed, "unexpected pre-exit channel state\n" );
        ok( slot->endpoints == (CLIENT_SURFACE_HANDOFF_ENDPOINT_PRODUCER |
                                CLIENT_SURFACE_HANDOFF_ENDPOINT_CONSUMER),
            "exit child endpoints before exit %#lx\n", slot->endpoints );
    }
    SetEvent( release );
    wait_child_process( &process );
    CloseHandle( process.hThread );
    CloseHandle( process.hProcess );
    process.hProcess = NULL;
    if (slot)
    {
        ok( __atomic_load_n( &slot->closed, __ATOMIC_ACQUIRE ), "process exit left channel open\n" );
        ok( slot->producer_sequence == !!completed && !slot->consumer_sequence,
            "process exit forged a descriptor acknowledgement\n" );
        ok( !slot->endpoints, "producer exit left endpoints %#lx\n", slot->endpoints );
        ok( !(__atomic_load_n( &shared->ready_bitmap[index / 64], __ATOMIC_ACQUIRE ) &
              ((UINT64)1 << (index % 64))), "producer exit left a ready bit set\n" );
    }
    status = set_surface_state( hwnd, 0, 0, 0, &state );
    ok( !status && !state.active && !state.cached,
        "producer exit left memberships: status %#x active %u cached %u\n",
        status, state.active, state.cached );
done:
    if (process.hProcess)
    {
        SetEvent( release );
        wait_child_process( &process );
        CloseHandle( process.hThread );
        CloseHandle( process.hProcess );
    }
    if (owner.mapping) CloseHandle( owner.mapping );
    if (view) UnmapViewOfFile( view );
    if (ready) CloseHandle( ready );
    if (release) CloseHandle( release );
    DestroyWindow( hwnd );
}

#define HANDOFF_OWNER_EXIT_ID 0x79600000

struct handoff_owner_exit_shared
{
    HWND hwnd;
};

static void handoff_storage_owner_exit_child( HANDLE mapping, HANDLE window_ready,
                                               HANDLE binding_ready, HANDLE owner_ready,
                                               HANDLE release, DWORD producer, UINT_PTR identity )
{
    struct handoff_owner_exit_shared *state;
    struct handoff_binding binding = {0};
    HWND hwnd = NULL;
    void *view = NULL;
    unsigned int status;

    state = MapViewOfFile( mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(*state) );
    ok( !!state, "owner-exit child shared map error %lu\n", GetLastError() );
    if (!state)
    {
        SetEvent( window_ready );
        SetEvent( owner_ready );
        return;
    }
    hwnd = create_test_window( FALSE );
    ok( !!hwnd, "owner-exit child failed to create window, error %lu\n", GetLastError() );
    state->hwnd = hwnd;
    SetEvent( window_ready );
    ok( WaitForSingleObject( binding_ready, 10000 ) == WAIT_OBJECT_0,
        "owner-exit child binding wait timed out\n" );
    if (!hwnd) goto done;

    status = get_surface_handoff( hwnd, producer, identity, TRUE, &binding );
    ok( !status, "owner-exit child consumer bind status %#x\n", status );
    if (status) goto done;
    view = MapViewOfFile( binding.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, binding.size );
    CloseHandle( binding.mapping );
    binding.mapping = NULL;
    ok( !!view, "owner-exit child consumer map error %lu\n", GetLastError() );
done:
    SetEvent( owner_ready );
    ok( WaitForSingleObject( release, 10000 ) == WAIT_OBJECT_0,
        "owner-exit child release timed out\n" );
    /* Deliberately omit endpoint release, unmap, and window destruction. */
}

static void test_handoff_storage_owner_exit( char **argv, BOOL completed )
{
    SECURITY_ATTRIBUTES attr = {sizeof(attr), NULL, TRUE};
    STARTUPINFOA startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process = {0};
    struct handoff_owner_exit_shared *state = NULL;
    struct handoff_binding producer = {0};
    struct client_surface_handoff_shared *shared;
    struct client_surface_handoff_channel *slot = NULL;
    HANDLE mapping = NULL, window_ready = NULL, binding_ready = NULL;
    HANDLE owner_ready = NULL, release = NULL;
    UINT_PTR identity = HANDOFF_OWNER_EXIT_ID + completed;
    unsigned int status, index = 0;
    char command[MAX_PATH * 2];
    void *view = NULL;
    HWND hwnd = NULL;

    mapping = CreateFileMappingA( INVALID_HANDLE_VALUE, &attr, PAGE_READWRITE, 0,
                                  sizeof(*state), NULL );
    window_ready = CreateEventA( &attr, TRUE, FALSE, NULL );
    binding_ready = CreateEventA( &attr, TRUE, FALSE, NULL );
    owner_ready = CreateEventA( &attr, TRUE, FALSE, NULL );
    release = CreateEventA( &attr, TRUE, FALSE, NULL );
    ok( mapping && window_ready && binding_ready && owner_ready && release,
        "failed to create owner-exit handoff synchronization objects\n" );
    if (!mapping || !window_ready || !binding_ready || !owner_ready || !release) goto done;
    state = MapViewOfFile( mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(*state) );
    ok( !!state, "owner-exit parent shared map error %lu\n", GetLastError() );
    if (!state) goto done;

    sprintf( command, "\"%s\" %s handoff_storage_owner_exit_child %p %p %p %p %p %lu %Ix",
             argv[0], argv[1], mapping, window_ready, binding_ready, owner_ready, release,
             GetCurrentProcessId(), identity );
    if (!CreateProcessA( NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process ))
    {
        ok( 0, "handoff owner-exit child CreateProcess error %lu\n", GetLastError() );
        goto done;
    }
    ok( WaitForSingleObject( window_ready, 10000 ) == WAIT_OBJECT_0,
        "handoff owner-exit child did not create its window\n" );
    hwnd = state->hwnd;
    ok( hwnd && IsWindow( hwnd ), "owner-exit child returned invalid window %p\n", hwnd );
    if (!hwnd || !IsWindow( hwnd )) goto release_child;

    status = set_surface_state( hwnd, identity,
                                CLIENT_SURFACE_STATE_REGISTER |
                                CLIENT_SURFACE_STATE_SCENE_PUBLICATION, 0, NULL );
    ok( !status, "owner-exit producer registration status %#x\n", status );
    status = claim_surface_state( hwnd, identity, NULL );
    ok( !status, "owner-exit producer claim status %#x\n", status );
    status = get_surface_handoff( hwnd, 0, identity, FALSE, &producer );
    ok( !status, "owner-exit producer bind status %#x\n", status );
    if (status) goto release_child;
    view = MapViewOfFile( producer.mapping, FILE_MAP_READ | FILE_MAP_WRITE,
                          0, 0, producer.size );
    CloseHandle( producer.mapping );
    producer.mapping = NULL;
    ok( !!view, "owner-exit producer map error %lu\n", GetLastError() );
    if (!view) goto release_child;

    SetEvent( binding_ready );
    ok( WaitForSingleObject( owner_ready, 10000 ) == WAIT_OBJECT_0,
        "handoff owner-exit consumer did not become ready\n" );
    shared = view;
    slot = (void *)((char *)view + producer.offset);
    index = slot - shared->channels;
    ok( slot->endpoints == (CLIENT_SURFACE_HANDOFF_ENDPOINT_PRODUCER |
                            CLIENT_SURFACE_HANDOFF_ENDPOINT_CONSUMER),
        "owner-exit endpoints before exit %#lx\n", slot->endpoints );
    ok( !slot->producer_sequence && !slot->consumer_sequence && !slot->closed,
        "exit test channel was not empty\n" );
    if (completed)
    {
        slot->slots[0].source_sequence = 1;
        __atomic_store_n( &slot->producer_sequence, 1, __ATOMIC_RELEASE );
        __atomic_fetch_or( &shared->ready_bitmap[index / 64],
                           (UINT64)1 << (index % 64), __ATOMIC_RELEASE );
    }
release_child:
    SetEvent( binding_ready );
    SetEvent( release );
    wait_child_process( &process );
    CloseHandle( process.hThread );
    CloseHandle( process.hProcess );
    process.hProcess = NULL;
    if (slot)
    {
        ok( __atomic_load_n( &slot->closed, __ATOMIC_ACQUIRE ), "process exit left channel open\n" );
        ok( slot->producer_sequence == !!completed && !slot->consumer_sequence,
            "process exit forged a descriptor acknowledgement\n" );
        ok( !slot->endpoints, "owner exit left endpoints %#lx\n", slot->endpoints );
        ok( !(__atomic_load_n( &shared->ready_bitmap[index / 64], __ATOMIC_ACQUIRE ) &
              ((UINT64)1 << (index % 64))), "owner exit left a ready bit set\n" );
    }
done:
    if (process.hProcess)
    {
        SetEvent( binding_ready );
        SetEvent( release );
        wait_child_process( &process );
        CloseHandle( process.hThread );
        CloseHandle( process.hProcess );
    }
    if (producer.mapping) CloseHandle( producer.mapping );
    if (view) UnmapViewOfFile( view );
    if (state) UnmapViewOfFile( state );
    if (mapping) CloseHandle( mapping );
    if (window_ready) CloseHandle( window_ready );
    if (binding_ready) CloseHandle( binding_ready );
    if (owner_ready) CloseHandle( owner_ready );
    if (release) CloseHandle( release );
}


static void test_generation_aba(void)
{
    const UINT_PTR surface = 0x12340000;
    struct surface_state first, second, stale, current, empty;
    HWND hwnd;
    unsigned int status;

    hwnd = create_test_window( FALSE );
    ok( !!hwnd, "failed to create window, error %lu\n", GetLastError() );
    if (!hwnd) return;

    status = set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_REGISTER, 0, NULL );
    ok( !status, "register failed, status %#x\n", status );
    status = claim_surface_state( hwnd, surface, NULL );
    ok( !status, "claim failed, status %#x\n", status );
    ShowWindow( hwnd, SW_SHOW );

    status = set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_STAGED, 0, &first );
    ok( !status, "first stage failed, status %#x\n", status );
    ok( first.staged, "first generation was not staged\n" );
    ok( first.generation != 0, "first generation is zero\n" );
    ok( first.generation == first.scene_generation,
        "first transaction %s does not match scene epoch %s\n",
        wine_dbgstr_longlong( first.generation ),
        wine_dbgstr_longlong( first.scene_generation ) );
    ok( first.pending == 1, "first pending count %u\n", first.pending );

    status = set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_STAGED, 0, &second );
    ok( !status, "second stage failed, status %#x\n", status );
    ok( second.staged, "second generation was not staged\n" );
    ok( second.generation && second.generation != first.generation,
        "generation was reused, first %s second %s\n",
        wine_dbgstr_longlong( first.generation ), wine_dbgstr_longlong( second.generation ) );
    ok( second.generation == second.scene_generation &&
        second.scene_generation != first.scene_generation,
        "second transaction %s did not advance scene epoch %s from %s\n",
        wine_dbgstr_longlong( second.generation ),
        wine_dbgstr_longlong( second.scene_generation ),
        wine_dbgstr_longlong( first.scene_generation ) );
    ok( second.pending == 1, "second pending count %u\n", second.pending );

    status = commit_surface_state( hwnd, surface, &first, &stale );
    ok( !status, "stale commit failed, status %#x\n", status );
    ok( stale.staged && stale.generation == second.generation && stale.pending == 1,
        "stale commit changed generation state: staged %u generation %s pending %u\n",
        stale.staged, wine_dbgstr_longlong( stale.generation ), stale.pending );
    ok( !stale.wake, "stale commit woke the top-level window\n" );

    status = commit_surface_state( hwnd, surface, &second, &current );
    ok( !status, "current commit failed, status %#x\n", status );
    ok( current.staged && current.ready && !current.pending,
        "current commit did not become ready: staged %u ready %u pending %u\n",
        current.staged, current.ready, current.pending );
    ok( !current.wake, "current commit exposed the top-level before publish\n" );

    SetWindowPos( hwnd, NULL, 20, 20, 160, 120, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE );
    status = set_surface_state( hwnd, 0, 0, 0, &stale );
    ok( !status && stale.staged && !stale.ready && stale.pending == 1 &&
        stale.generation != current.generation &&
        stale.scene_generation != current.scene_generation,
        "ready scene was not invalidated: staged %u ready %u pending %u generation %s scene %s\n",
        stale.staged, stale.ready, stale.pending, wine_dbgstr_longlong( stale.generation ),
        wine_dbgstr_longlong( stale.scene_generation ) );
    status = publish_surface_state( hwnd, &current );
    ok( !status && current.staged && !current.ready && current.pending == 1 && !current.wake,
        "stale publish exposed restarted scene: staged %u ready %u pending %u wake %u status %#x\n",
        current.staged, current.ready, current.pending, current.wake, status );
    status = commit_surface_state( hwnd, surface, &stale, &current );
    ok( !status && current.staged && current.ready && !current.pending,
        "restarted generation did not become ready: staged %u ready %u pending %u status %#x\n",
        current.staged, current.ready, current.pending, status );
    status = publish_surface_state( hwnd, &current );
    ok( !status && !current.staged && current.wake,
        "restarted generation did not publish: staged %u wake %u status %#x\n",
        current.staged, current.wake, status );

    status = set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, &empty );
    ok( !status, "unregister failed, status %#x\n", status );
    ok( !empty.active && !empty.cached && !empty.pending,
        "surface state leaked: active %u cached %u pending %u\n",
        empty.active, empty.cached, empty.pending );
    DestroyWindow( hwnd );
}

static void test_publish_transaction(void)
{
    const UINT_PTR surface = 0x12380000;
    struct surface_state staged, ready, publishing, changed, committed, repaired;
    HWND hwnd;
    unsigned int status;

    hwnd = create_test_window( FALSE );
    ok( !!hwnd, "failed to create publish transaction window, error %lu\n", GetLastError() );
    if (!hwnd) return;

    set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_REGISTER |
                       CLIENT_SURFACE_STATE_SCENE_PUBLICATION, 0, NULL );
    claim_surface_state( hwnd, surface, NULL );
    ShowWindow( hwnd, SW_SHOW );
    status = set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_STAGED, 0, &staged );
    ok( !status && staged.staged && staged.pending == 1,
        "failed to stage publish transaction: status %#x staged %u pending %u\n",
        status, staged.staged, staged.pending );
    status = commit_surface_state( hwnd, surface, &staged, &ready );
    ok( !status && ready.ready && !ready.pending,
        "publish transaction did not become ready: status %#x ready %u pending %u\n",
        status, ready.ready, ready.pending );

    status = set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_PUBLISH_BEGIN, 0, &publishing );
    ok( !status && publishing.publish && publishing.staged && publishing.ready,
        "publish begin was rejected: status %#x publish %u staged %u ready %u\n",
        status, publishing.publish, publishing.staged, publishing.ready );

    SetWindowPos( hwnd, NULL, 30, 30, 0, 0,
                  SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
    status = set_surface_state( hwnd, 0, 0, 0, &changed );
    ok( !status && changed.staged && changed.ready &&
        changed.generation == publishing.generation &&
        changed.scene_generation != publishing.scene_generation,
        "publish token was restarted before host ACK: status %#x staged %u ready %u generation %s scene %s\n",
        status, changed.staged, changed.ready, wine_dbgstr_longlong( changed.generation ),
        wine_dbgstr_longlong( changed.scene_generation ) );

    status = set_surface_state_scene( hwnd, 0, CLIENT_SURFACE_STATE_PUBLISH_COMMIT,
                                      publishing.generation, publishing.scene_generation, &committed );
    ok( !status && !committed.staged && committed.wake && !committed.pending &&
        !committed.generation,
        "publish ACK did not wait for owner snapshot: status %#x staged %u wake %u pending %u generation %s\n",
        status, committed.staged, committed.wake, committed.pending,
        wine_dbgstr_longlong( committed.generation ) );
    status = prepare_surface_state( hwnd, &committed );
    ok( !status && committed.pending == 1 && committed.generation != publishing.generation,
        "owner snapshot did not start live repair: status %#x pending %u generation %s\n",
        status, committed.pending, wine_dbgstr_longlong( committed.generation ) );

    status = commit_surface_state( hwnd, surface, &committed, &repaired );
    ok( !status && !repaired.staged && repaired.ready && !repaired.pending && repaired.generation,
        "live repair did not reach publication: status %#x staged %u ready %u pending %u generation %s\n",
        status, repaired.staged, repaired.ready, repaired.pending,
        wine_dbgstr_longlong( repaired.generation ) );
    status = publish_surface_state( hwnd, &repaired );
    ok( !status && !repaired.staged && !repaired.pending && !repaired.generation,
        "live repair publication did not retire its epoch: status %#x staged %u pending %u generation %s\n",
        status, repaired.staged, repaired.pending, wine_dbgstr_longlong( repaired.generation ) );

    set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    DestroyWindow( hwnd );
}

static void test_live_prepare_transaction(void)
{
    const UINT_PTR surface = 0x123a0000;
    struct surface_state preparing, stale, current, ready;
    HWND hwnd;
    unsigned int status;

    hwnd = create_test_window( TRUE );
    ok( !!hwnd, "failed to create live prepare window, error %lu\n", GetLastError() );
    if (!hwnd) return;

    status = set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_REGISTER |
                                CLIENT_SURFACE_STATE_SCENE_PUBLICATION, 0, NULL );
    ok( !status, "live prepare register failed, status %#x\n", status );
    status = claim_surface_state( hwnd, surface, &preparing );
    ok( !status && !preparing.generation && !preparing.pending,
        "live scene started before owner snapshot: status %#x generation %s pending %u\n",
        status, wine_dbgstr_longlong( preparing.generation ), preparing.pending );

    status = set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_PREPARE_BEGIN, 0, &preparing );
    ok( !status && preparing.publish && !preparing.generation,
        "live prepare begin failed: status %#x prepare %u generation %s\n",
        status, preparing.publish, wine_dbgstr_longlong( preparing.generation ) );

    SetWindowPos( hwnd, NULL, 11, 10, 0, 0,
                  SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
    status = set_surface_state_scene( hwnd, 0, CLIENT_SURFACE_STATE_PREPARE_COMMIT,
                                      0, preparing.scene_generation, &stale );
    ok( !status && !stale.generation && stale.scene_generation != preparing.scene_generation,
        "stale prepare started a live scene: status %#x generation %s old scene %s new scene %s\n",
        status, wine_dbgstr_longlong( stale.generation ),
        wine_dbgstr_longlong( preparing.scene_generation ),
        wine_dbgstr_longlong( stale.scene_generation ) );

    status = set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_PREPARE_BEGIN, 0, &current );
    ok( !status && current.publish,
        "restarted live prepare begin failed: status %#x prepare %u\n", status, current.publish );
    status = set_surface_state_scene( hwnd, 0, CLIENT_SURFACE_STATE_PREPARE_COMMIT,
                                      0, current.scene_generation, &current );
    ok( !status && current.generation && current.pending == 1,
        "live prepare commit did not start composition: status %#x generation %s pending %u\n",
        status, wine_dbgstr_longlong( current.generation ), current.pending );

    status = commit_surface_state( hwnd, surface, &current, &ready );
    ok( !status && ready.ready && !ready.pending && ready.generation,
        "live composition did not become ready: status %#x ready %u pending %u generation %s\n",
        status, ready.ready, ready.pending, wine_dbgstr_longlong( ready.generation ) );
    status = publish_surface_state( hwnd, &ready );
    ok( !status && !ready.ready && !ready.generation,
        "live composition did not publish: status %#x ready %u generation %s\n",
        status, ready.ready, wine_dbgstr_longlong( ready.generation ) );

    set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    DestroyWindow( hwnd );
}

static void test_unbacked_live_generation(void)
{
    const UINT_PTR surface = 0x123b0000;
    struct surface_state composing, completed, prepare;
    HWND hwnd;
    unsigned int status;

    hwnd = create_test_window( TRUE );
    ok( !!hwnd, "failed to create unbacked live window, error %lu\n", GetLastError() );
    if (!hwnd) return;

    status = set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_REGISTER, 0, NULL );
    ok( !status, "unbacked live register failed, status %#x\n", status );
    status = claim_surface_state( hwnd, surface, &composing );
    ok( !status && composing.generation && composing.pending == 1 && !composing.ready,
        "unbacked live scene did not start directly: status %#x generation %s pending %u ready %u\n",
        status, wine_dbgstr_longlong( composing.generation ), composing.pending, composing.ready );

    status = set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_PREPARE_BEGIN, 0, &prepare );
    ok( !status && !prepare.publish,
        "unbacked live scene requested an owner snapshot: status %#x prepare %u\n",
        status, prepare.publish );
    status = commit_surface_state( hwnd, surface, &composing, &completed );
    ok( !status && !completed.generation && !completed.pending && !completed.ready,
        "unbacked live scene waited for owner publication: status %#x generation %s pending %u ready %u\n",
        status, wine_dbgstr_longlong( completed.generation ), completed.pending, completed.ready );

    set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    DestroyWindow( hwnd );
}

static void test_native_backing_barrier(void)
{
    const UINT_PTR surface = 0x123d0000, barrier = 0x45670000;
    struct surface_state state, composing, ready, sealed, blocked;
    HWND hwnd;
    unsigned int status;

    hwnd = create_test_window( TRUE );
    ok( !!hwnd, "failed to create native barrier window, error %lu\n", GetLastError() );
    if (!hwnd) return;

    status = set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_REGISTER |
                                CLIENT_SURFACE_STATE_SCENE_PUBLICATION, 0, NULL );
    ok( !status, "native barrier register failed, status %#x\n", status );
    claim_surface_state( hwnd, surface, &state );
    status = prepare_surface_state( hwnd, &composing );
    ok( !status && composing.generation && composing.pending == 1,
        "native barrier prepare failed: status %#x generation %s pending %u\n",
        status, wine_dbgstr_longlong( composing.generation ), composing.pending );
    status = commit_surface_state( hwnd, surface, &composing, &ready );
    ok( !status && ready.ready, "native barrier composition failed, status %#x ready %u\n",
        status, ready.ready );
    status = publish_surface_state( hwnd, &state );
    ok( !status && !state.generation, "native barrier publication failed, status %#x\n", status );

    status = set_surface_state( hwnd, barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN,
                                0, &sealed );
    ok( !status && !sealed.pending && (sealed.scene_generation & 1),
        "native barrier did not seal the scene: status %#x pending %u scene %s\n",
        status, sealed.pending, wine_dbgstr_longlong( sealed.scene_generation ) );
    status = set_surface_state( hwnd, barrier + 1, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN,
                                0, NULL );
    ok( status == STATUS_DEVICE_BUSY, "competing native barrier returned %#x\n", status );
    status = set_surface_state( hwnd, barrier + 1, CLIENT_SURFACE_STATE_NATIVE_BARRIER_END, 0, NULL );
    ok( status == STATUS_INVALID_PARAMETER, "wrong barrier token ended the scene, status %#x\n", status );

    status = begin_surface_state( hwnd, surface, &state, &blocked );
    ok( !status && !blocked.compose,
        "old composition crossed native barrier: status %#x compose %u\n", status, blocked.compose );
    status = commit_surface_state( hwnd, surface, &state, &sealed );
    ok( !status, "stale composition commit failed, status %#x\n", status );
    status = set_surface_state( hwnd, barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN,
                                0, &sealed );
    ok( !status && !sealed.pending && (sealed.scene_generation & 1),
        "native barrier lost its scene seal: status %#x pending %u scene %s\n",
        status, sealed.pending, wine_dbgstr_longlong( sealed.scene_generation ) );
    status = set_surface_state( hwnd, barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_END,
                                0, &sealed );
    ok( !status && !(sealed.scene_generation & 1),
        "native barrier did not reopen scene: status %#x scene %s\n",
        status, wine_dbgstr_longlong( sealed.scene_generation ) );

    set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    DestroyWindow( hwnd );
}

static void test_demoted_native_barrier(void)
{
    const UINT_PTR surface = 0x123d6000, barrier = 0x45676000;
    struct surface_state sealed;
    HWND first, second;
    unsigned int status;

    first = create_test_window( TRUE );
    second = create_test_window( TRUE );
    ok( !!first && !!second, "failed to create demotion parents, error %lu\n", GetLastError() );
    if (!first || !second) goto done;
    set_surface_state( first, surface, CLIENT_SURFACE_STATE_REGISTER, 0, NULL );
    claim_surface_state( first, surface, NULL );

    /* Native resource replacement must still seal the old owner after the
     * server hierarchy has changed, not the newly selected top-level. */
    status = set_server_parent( first, second );
    ok( !status, "server demotion failed, status %#x\n", status );
    status = set_surface_state( first, barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN, 0, &sealed );
    ok( !status && sealed.toplevel == first && !sealed.pending,
        "native barrier followed new hierarchy: status %#x target %p pending %u\n",
        status, sealed.toplevel, sealed.pending );
    status = set_surface_state( first, barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_END, 0, &sealed );
    ok( !status && !(sealed.scene_generation & 1) && !sealed.generation,
        "demoted target retained its own transaction: status %#x scene %s generation %s\n",
        status, wine_dbgstr_longlong( sealed.scene_generation ), wine_dbgstr_longlong( sealed.generation ) );
    status = set_server_parent( first, GetDesktopWindow() );
    ok( !status, "server promotion failed, status %#x\n", status );
    set_surface_state( first, surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
done:
    if (first) DestroyWindow( first );
    if (second) DestroyWindow( second );
}

static void test_notification_identity_aba(void)
{
    const UINT_PTR surface = 0x123d8000;
    MSG message;
    HWND first, second;
    unsigned int status;

    first = create_test_window( FALSE );
    second = create_test_window( FALSE );
    ok( !!first && !!second, "failed to create notification ABA windows, error %lu\n",
        GetLastError() );
    if (!first || !second)
    {
        if (first) DestroyWindow( first );
        if (second) DestroyWindow( second );
        return;
    }

    /* Ensure that window destruction can queue the process-local identity
     * notification, but leave it undispatched across the first registration
     * attempt.  The retired index entry is the ABA exclusion boundary. */
    PeekMessageA( &message, NULL, 0, 0, PM_NOREMOVE );
    status = set_surface_state( first, surface, CLIENT_SURFACE_STATE_REGISTER, 0, NULL );
    ok( !status, "notification ABA register failed, status %#x\n", status );
    ok( DestroyWindow( first ), "failed to destroy notification ABA window, error %lu\n",
        GetLastError() );

    status = set_surface_state( second, surface, CLIENT_SURFACE_STATE_REGISTER, 0, NULL );
    ok( status == STATUS_INVALID_PARAMETER,
        "retired identity was reused before notification removal, status %#x\n", status );

    /* Process-internal notifications are sent-message work and must bypass
     * application window and message-range filters.  A posted notification
     * would remain stranded behind this valid restrictive PeekMessage call. */
    PeekMessageA( &message, second, WM_USER, WM_USER, PM_NOREMOVE );
    status = set_surface_state( second, surface, CLIENT_SURFACE_STATE_REGISTER, 0, NULL );
    ok( !status, "filtered message pump retained notification identity tombstone, status %#x\n",
        status );
    set_surface_state( second, surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    DestroyWindow( second );
}

static void test_late_present_cutover(void)
{
    const UINT_PTR surface = 0x12390000;
    struct surface_state staged, ready, reopened, publishing, late, published, repaired;
    HWND hwnd;
    unsigned int status;

    hwnd = create_test_window( FALSE );
    ok( !!hwnd, "failed to create late-present window, error %lu\n", GetLastError() );
    if (!hwnd) return;

    set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_REGISTER |
                       CLIENT_SURFACE_STATE_SCENE_PUBLICATION, 0, NULL );
    claim_surface_state( hwnd, surface, NULL );
    ShowWindow( hwnd, SW_SHOW );
    status = set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_STAGED, 0, &staged );
    ok( !status && staged.staged && staged.pending == 1,
        "failed to stage late-present window: status %#x staged %u pending %u\n",
        status, staged.staged, staged.pending );
    status = commit_surface_state( hwnd, surface, &staged, &ready );
    ok( !status && ready.ready && !ready.pending,
        "initial frame did not become ready: status %#x ready %u pending %u\n",
        status, ready.ready, ready.pending );

    status = begin_surface_state( hwnd, surface, &ready, &reopened );
    ok( !status && reopened.compose && !reopened.ready && reopened.pending == 1 &&
        reopened.generation == ready.generation,
        "late frame did not reopen generation: status %#x compose %u ready %u pending %u generation %s\n",
        status, reopened.compose, reopened.ready, reopened.pending,
        wine_dbgstr_longlong( reopened.generation ) );
    status = commit_surface_state( hwnd, surface, &reopened, &ready );
    ok( !status && ready.ready && !ready.pending,
        "reopened frame did not become ready: status %#x ready %u pending %u\n",
        status, ready.ready, ready.pending );

    status = set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_PUBLISH_BEGIN, 0, &publishing );
    ok( !status && publishing.publish && publishing.ready,
        "publish begin failed: status %#x publish %u ready %u\n",
        status, publishing.publish, publishing.ready );
    status = begin_surface_state( hwnd, surface, &publishing, &late );
    ok( !status && !late.compose && late.ready && !late.pending,
        "present crossed an active host publication: status %#x compose %u ready %u pending %u\n",
        status, late.compose, late.ready, late.pending );
    status = set_surface_state_scene( hwnd, 0, CLIENT_SURFACE_STATE_PUBLISH_COMMIT,
                                      publishing.generation, publishing.scene_generation, &published );
    ok( !status && !published.staged && !published.pending && !published.generation,
        "late frame repair skipped owner snapshot: status %#x staged %u pending %u generation %s\n",
        status, published.staged, published.pending, wine_dbgstr_longlong( published.generation ) );
    status = prepare_surface_state( hwnd, &published );
    ok( !status && published.pending == 1 && published.generation != publishing.generation,
        "late frame snapshot did not start repair: status %#x pending %u generation %s\n",
        status, published.pending, wine_dbgstr_longlong( published.generation ) );
    status = commit_surface_state( hwnd, surface, &published, &repaired );
    ok( !status && repaired.ready && !repaired.pending && repaired.generation,
        "late-frame repair did not reach publication: status %#x ready %u pending %u generation %s\n",
        status, repaired.ready, repaired.pending, wine_dbgstr_longlong( repaired.generation ) );
    status = publish_surface_state( hwnd, &repaired );
    ok( !status && !repaired.pending && !repaired.generation,
        "late-frame publication did not finish: status %#x pending %u generation %s\n",
        status, repaired.pending, wine_dbgstr_longlong( repaired.generation ) );

    set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    DestroyWindow( hwnd );
}

struct race_context
{
    HWND hwnd;
    UINT_PTR base;
    LONG failures;
    LONG first_status;
};

static DWORD WINAPI surface_race_thread( void *arg )
{
    struct race_context *context = arg;
    unsigned int i, status;

    for (i = 0; i < RACE_ROUNDS; ++i)
    {
        UINT_PTR id = context->base + i;

        status = set_surface_state( context->hwnd, id, CLIENT_SURFACE_STATE_REGISTER, 0, NULL );
        if (status == STATUS_INVALID_HANDLE || status == STATUS_WINE_INVALID_WINDOW_HANDLE) break;
        if (status)
        {
            InterlockedCompareExchange( &context->first_status, status, 0 );
            InterlockedIncrement( &context->failures );
        }
        status = set_surface_state( context->hwnd, id,
                                    CLIENT_SURFACE_STATE_UNREGISTER | CLIENT_SURFACE_STATE_CACHE,
                                    0, NULL );
        if (status == STATUS_INVALID_HANDLE || status == STATUS_WINE_INVALID_WINDOW_HANDLE) break;
        if (status)
        {
            InterlockedCompareExchange( &context->first_status, status, 0 );
            InterlockedIncrement( &context->failures );
        }
        status = set_surface_state( context->hwnd, id,
                                    CLIENT_SURFACE_STATE_REGISTER | CLIENT_SURFACE_STATE_UNCACHE,
                                    0, NULL );
        if (status == STATUS_INVALID_HANDLE || status == STATUS_WINE_INVALID_WINDOW_HANDLE) break;
        if (status)
        {
            InterlockedCompareExchange( &context->first_status, status, 0 );
            InterlockedIncrement( &context->failures );
        }
        status = set_surface_state( context->hwnd, id, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
        if (status == STATUS_INVALID_HANDLE || status == STATUS_WINE_INVALID_WINDOW_HANDLE) break;
        if (status)
        {
            InterlockedCompareExchange( &context->first_status, status, 0 );
            InterlockedIncrement( &context->failures );
        }
    }
    return 0;
}

static void test_concurrent_state_changes(void)
{
    struct race_context contexts[RACE_THREADS];
    HANDLE threads[RACE_THREADS];
    struct surface_state state;
    HWND hwnd;
    unsigned int i, status, thread_count = 0;

    hwnd = create_test_window( FALSE );
    ok( !!hwnd, "failed to create race window, error %lu\n", GetLastError() );
    if (!hwnd) return;

    for (i = 0; i < RACE_THREADS; ++i)
    {
        contexts[i].hwnd = hwnd;
        contexts[i].base = 0x20000000 + i * 0x10000;
        contexts[i].failures = 0;
        contexts[i].first_status = 0;
        threads[i] = CreateThread( NULL, 0, surface_race_thread, &contexts[i], 0, NULL );
        ok( !!threads[i], "failed to create thread %u, error %lu\n", i, GetLastError() );
        if (threads[i]) thread_count++;
    }

    for (i = 0; i < 500; ++i)
    {
        ShowWindow( hwnd, SW_HIDE );
        ShowWindow( hwnd, SW_SHOW );
        set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_STAGED, 0, &state );
        set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_BYPASS, 0, NULL );
    }

    if (thread_count == RACE_THREADS)
        ok( WaitForMultipleObjects( RACE_THREADS, threads, TRUE, 30000 ) == WAIT_OBJECT_0,
            "surface state race timed out\n" );
    else
        for (i = 0; i < RACE_THREADS; ++i)
            if (threads[i]) ok( WaitForSingleObject( threads[i], 30000 ) == WAIT_OBJECT_0,
                                "surface state thread %u timed out\n", i );
    for (i = 0; i < RACE_THREADS; ++i)
    {
        ok( !contexts[i].failures, "thread %u had %ld request failures, first status %#lx\n",
            i, contexts[i].failures, contexts[i].first_status );
        if (threads[i]) CloseHandle( threads[i] );
    }

    status = set_surface_state( hwnd, 0, 0, 0, &state );
    ok( !status, "state query failed, status %#x\n", status );
    ok( !state.active && !state.cached && !state.pending,
        "concurrent state leaked: active %u cached %u pending %u\n",
        state.active, state.cached, state.pending );
    DestroyWindow( hwnd );
}

static void owner_exit_child( HWND hwnd, HANDLE ready, HANDLE release, BOOL create_queue,
                              BOOL scene_publication )
{
    MSG message;
    unsigned int i, status;

    for (i = 0; i < OWNER_SURFACES; ++i)
    {
        status = set_surface_state( hwnd, 0x30000000 + i,
                                    CLIENT_SURFACE_STATE_REGISTER | CLIENT_SURFACE_STATE_CACHE |
                                    (scene_publication ? CLIENT_SURFACE_STATE_SCENE_PUBLICATION : 0),
                                    0, NULL );
        ok( !status, "owner register %u failed, status %#x\n", i, status );
    }
    if (create_queue) PeekMessageA( &message, NULL, 0, 0, PM_NOREMOVE );
    SetEvent( ready );
    ok( WaitForSingleObject( release, 10000 ) == WAIT_OBJECT_0,
        "owner exit child release timed out\n" );
}

static void destroy_race_child( HWND hwnd )
{
    struct race_context context = {hwnd, 0x40000000};

    surface_race_thread( &context );
    ok( !context.failures, "destroy race had %ld request failures, first status %#lx\n",
        context.failures, context.first_status );
}

static BOOL register_present_test_class(void);

static BOOL run_child( char **argv, const char *mode, HWND hwnd, DWORD delay )
{
    SECURITY_ATTRIBUTES attr = {sizeof(attr), NULL, TRUE};
    STARTUPINFOA startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process;
    HANDLE ready = NULL, release = NULL;
    char command[MAX_PATH * 2];

    ready = CreateEventA( &attr, TRUE, FALSE, NULL );
    if (!strcmp( mode, "owner_exit" ) || !strcmp( mode, "owner_no_queue" ) ||
        !strcmp( mode, "owner_stalled" ))
        release = CreateEventA( &attr, TRUE, FALSE, NULL );
    sprintf( command, "%s %s %s %p %p %p", argv[0], argv[1], mode, hwnd, ready, release );
    if (!CreateProcessA( NULL, command, NULL, NULL, !!ready, 0, NULL, NULL, &startup, &process ))
    {
        ok( 0, "CreateProcess failed, error %lu\n", GetLastError() );
        if (ready) CloseHandle( ready );
        if (release) CloseHandle( release );
        return FALSE;
    }
    ok( WaitForSingleObject( ready, 10000 ) == WAIT_OBJECT_0,
        "%s child did not become ready\n", mode );
    if (!strcmp( mode, "owner_exit" ) || !strcmp( mode, "owner_no_queue" ) ||
        !strcmp( mode, "owner_stalled" ))
    {
        struct surface_state state;
        unsigned int status;

        ShowWindow( hwnd, SW_SHOW );
        status = set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_STAGED, 0, &state );
        ok( !status, "owner exit stage failed, status %#x\n", status );
        if (strcmp( mode, "owner_no_queue" ))
            ok( state.staged && state.pending == 1,
                "owner generation staged %u pending %u\n", state.staged, state.pending );
        else
        {
            if (state.pending)
                skip( "process startup created a renderer message queue; "
                      "queue-less notification path unavailable\n" );
            else
            {
                ok( state.staged && state.ready && !state.wake,
                    "queue-less renderer did not reach owner publish: "
                    "staged %u ready %u wake %u\n",
                    state.staged, state.ready, state.wake );
                status = publish_surface_state( hwnd, &state );
                ok( !status && !state.staged && state.wake,
                    "queue-less generation did not publish: staged %u wake %u status %#x\n",
                    state.staged, state.wake, status );
            }
        }
        if (!strcmp( mode, "owner_stalled" ))
        {
            DWORD start = GetTickCount();
            unsigned int move = 0;

            /* Scene churn must not rearm the six-second publication deadline
             * or enqueue one renderer notification per intermediate epoch. */
            while (GetTickCount() - start < 5500)
            {
                SetWindowPos( hwnd, NULL, 10 + (move++ & 1), 10, 0, 0,
                              SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
                Sleep( 50 );
            }
            Sleep( 1200 );
            status = set_surface_state( hwnd, 0, 0, 0, &state );
            ok( !status, "stalled owner state query failed, status %#x\n", status );
            ok( state.staged && !state.pending,
                "stalled owner exposed an incomplete scene: staged %u pending %u\n",
                state.staged, state.pending );
        }
        SetEvent( release );
    }
    else
    {
        if (delay) Sleep( delay );
        DestroyWindow( hwnd );
    }
    wait_child_process( &process );
    if (ready) CloseHandle( ready );
    if (release) CloseHandle( release );
    CloseHandle( process.hThread );
    CloseHandle( process.hProcess );
    return TRUE;
}

static void pixel_format_child( HWND hwnd, int expected )
{
    HDC hdc;
    int format;

    hdc = GetDC( hwnd );
    ok( !!hdc, "failed to get foreign window DC, error %lu\n", GetLastError() );
    if (!hdc) return;
    format = GetPixelFormat( hdc );
    ok( format == expected, "foreign window pixel format %d, expected %d\n", format, expected );
    ReleaseDC( hwnd, hdc );
}

static void test_cross_process_pixel_format( char **argv )
{
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
                                PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 24};
    STARTUPINFOA startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process;
    char command[MAX_PATH * 2];
    HDC hdc;
    HWND hwnd;
    int format;

    if (!register_present_test_class()) return;
    hwnd = CreateWindowA( "client_surface_present_race", "cross-process pixel format",
                          WS_POPUP, 20, 20, 160, 120, NULL, NULL,
                          GetModuleHandleA( NULL ), NULL );
    ok( !!hwnd, "failed to create pixel-format window, error %lu\n", GetLastError() );
    if (!hwnd) return;

    hdc = GetDC( hwnd );
    format = hdc ? ChoosePixelFormat( hdc, &pfd ) : 0;
    if (!hdc || !format || !SetPixelFormat( hdc, format, &pfd ))
    {
        win_skip( "pixel-format setup failed, error %lu\n", GetLastError() );
        if (hdc) ReleaseDC( hwnd, hdc );
        DestroyWindow( hwnd );
        return;
    }
    ok( GetPixelFormat( hdc ) == format, "owner window lost pixel format %d\n", format );

    sprintf( command, "%s %s pixel_format %p %d", argv[0], argv[1], hwnd, format );
    if (!CreateProcessA( NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process ))
        ok( 0, "CreateProcess failed, error %lu\n", GetLastError() );
    else
    {
        wait_child_process( &process );
        CloseHandle( process.hThread );
        CloseHandle( process.hProcess );
    }

    ReleaseDC( hwnd, hdc );
    DestroyWindow( hwnd );
}

struct present_race_context
{
    HWND hwnd;
    HANDLE ready;
    volatile LONG stop;
    LONG setup_error;
    LONG presents;
    BOOL unpaced;
    char renderer[128];
};

static void pump_messages( DWORD timeout )
{
    DWORD end = GetTickCount() + timeout;
    MSG message;

    do
    {
        while (PeekMessageA( &message, NULL, 0, 0, PM_REMOVE ))
        {
            TranslateMessage( &message );
            DispatchMessageA( &message );
            if ((LONG)(end - GetTickCount()) <= 0) break;
        }
        Sleep( 10 );
    } while ((LONG)(end - GetTickCount()) > 0);
}

static COLORREF get_gl_front_pixel( const RECT *rect )
{
    BYTE pixel[4] = {0};

    glReadBuffer( GL_FRONT );
    glReadPixels( (rect->left + rect->right) / 2, (rect->top + rect->bottom) / 2,
                  1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel );
    glFinish();
    return RGB( pixel[0], pixel[1], pixel[2] );
}

static BOOL wait_for_surface_idle( HWND hwnd, DWORD timeout, struct surface_state *state )
{
    DWORD start = GetTickCount();
    unsigned int status;

    do
    {
        pump_messages( 10 );
        status = set_surface_state( hwnd, 0, 0, 0, state );
        if (status || (!state->staged && !state->pending)) return !status;
    } while (GetTickCount() - start < timeout);
    return FALSE;
}

static BOOL color_matches( COLORREF color, BYTE red, BYTE green, BYTE blue )
{
    return color != CLR_INVALID && abs( (int)GetRValue( color ) - red ) <= 24 &&
           abs( (int)GetGValue( color ) - green ) <= 24 &&
           abs( (int)GetBValue( color ) - blue ) <= 24;
}

static void test_hidden_present_resize(void)
{
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
                                PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 24};
    struct surface_state state;
    HGLRC glrc = NULL;
    HDC hdc = NULL;
    HWND hwnd;
    COLORREF color;
    RECT rect;
    const char *renderer;
    int format;

    if (!register_present_test_class()) return;
    hwnd = CreateWindowExA( WS_EX_LAYERED | WS_EX_TOPMOST, "client_surface_present_race",
                            "hidden present", WS_POPUP, 320, 240, 160, 120,
                            NULL, NULL, GetModuleHandleA( NULL ), NULL );
    ok( !!hwnd, "failed to create hidden present window, error %lu\n", GetLastError() );
    if (!hwnd) return;
    ok( SetLayeredWindowAttributes( hwnd, 0, 255, LWA_ALPHA ),
        "failed to initialize layered attributes, error %lu\n", GetLastError() );
    ok( SetWindowPos( hwnd, HWND_TOPMOST, 320, 240, 224, 176, SWP_NOACTIVATE ),
        "failed to resize hidden window, error %lu\n", GetLastError() );

    hdc = GetDC( hwnd );
    format = hdc ? ChoosePixelFormat( hdc, &pfd ) : 0;
    if (!hdc || !format || !SetPixelFormat( hdc, format, &pfd ) ||
        !(glrc = wglCreateContext( hdc )) || !wglMakeCurrent( hdc, glrc ))
    {
        win_skip( "hidden OpenGL context setup failed, error %lu\n", GetLastError() );
        goto done;
    }
    renderer = (const char *)glGetString( GL_RENDERER );
    trace( "hidden-present OpenGL renderer: %s\n", renderer ? renderer : "(null)" );
    ok( !!renderer, "OpenGL renderer is unavailable\n" );

    GetClientRect( hwnd, &rect );
    ok( rect.right == 224 && rect.bottom == 176,
        "unexpected first client size %ldx%ld\n", rect.right, rect.bottom );
    glViewport( 0, 0, rect.right, rect.bottom );
    glClearColor( 0.8, 0.1, 0.6, 1.0 );
    glClear( GL_COLOR_BUFFER_BIT );
    ok( SwapBuffers( hdc ), "first hidden SwapBuffers failed, error %lu\n", GetLastError() );
    set_surface_state( hwnd, 0, 0, 0, &state );
    ok( state.active == 1, "hidden present active surface count %u\n", state.active );

    ShowWindow( hwnd, SW_SHOWNA );
    ok( wait_for_surface_idle( hwnd, 7000, &state ),
        "first hidden frame did not become idle\n" );
    ok( !state.staged && !state.pending,
        "first hidden frame not published: staged %u pending %u\n", state.staged, state.pending );
    color = get_gl_front_pixel( &rect );
    trace( "first hidden-present front pixel %#lx\n", color );
    ok( color_matches( color, 204, 26, 153 ), "unexpected first front pixel %#lx\n", color );

    ShowWindow( hwnd, SW_HIDE );
    ok( SetWindowPos( hwnd, HWND_TOPMOST, 320, 240, 240, 188, SWP_NOACTIVATE ),
        "failed to resize second hidden frame, error %lu\n", GetLastError() );
    GetClientRect( hwnd, &rect );
    ok( rect.right == 240 && rect.bottom == 188,
        "unexpected second client size %ldx%ld\n", rect.right, rect.bottom );
    glViewport( 0, 0, rect.right, rect.bottom );
    glClearColor( 0.1, 0.75, 0.2, 1.0 );
    glClear( GL_COLOR_BUFFER_BIT );
    ok( SwapBuffers( hdc ), "second hidden SwapBuffers failed, error %lu\n", GetLastError() );
    ShowWindow( hwnd, SW_SHOWNA );
    ok( wait_for_surface_idle( hwnd, 7000, &state ),
        "resized hidden frame did not become idle\n" );
    ok( !state.staged && !state.pending,
        "resized hidden frame not published: staged %u pending %u\n", state.staged, state.pending );
    color = get_gl_front_pixel( &rect );
    trace( "resized hidden-present front pixel %#lx\n", color );
    ok( color_matches( color, 26, 191, 51 ), "unexpected resized front pixel %#lx\n", color );

done:
    if (glrc) wglMakeCurrent( NULL, NULL );
    if (glrc) wglDeleteContext( glrc );
    if (hdc) ReleaseDC( hwnd, hdc );
    DestroyWindow( hwnd );
}

static void test_handoff_bitmap_boundary(void)
{
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
                                PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 24};
    HWND windows[65] = {0};
    HDC dcs[ARRAY_SIZE(windows)] = {0};
    HGLRC contexts[ARRAY_SIZE(windows)] = {0};
    BOOL (WINAPI *swap_interval)( int );
    struct surface_state state;
    COLORREF color;
    unsigned int i;
    int format;

    if (!register_present_test_class()) return;
    /* Keep real producers alive together so the shared pool reaches bit 63
     * and the next bitmap word instead of recycling the first slot. */
    for (i = 0; i < ARRAY_SIZE(windows); ++i)
    {
        windows[i] = CreateWindowExA( WS_EX_LAYERED | WS_EX_TOPMOST, "client_surface_present_race",
                                      "handoff bitmap boundary", WS_POPUP, 720, 240, 32, 32,
                                      NULL, NULL, GetModuleHandleA( NULL ), NULL );
        ok( !!windows[i], "window %u creation failed, error %lu\n", i, GetLastError() );
        if (!windows[i]) goto done;
        ok( SetLayeredWindowAttributes( windows[i], 0, 255, LWA_ALPHA ),
            "window %u alpha initialization failed, error %lu\n", i, GetLastError() );
        dcs[i] = GetDC( windows[i] );
        format = dcs[i] ? ChoosePixelFormat( dcs[i], &pfd ) : 0;
        if (!dcs[i] || !format || !SetPixelFormat( dcs[i], format, &pfd ) ||
            !(contexts[i] = wglCreateContext( dcs[i] )) || !wglMakeCurrent( dcs[i], contexts[i] ))
        {
            if (!i) win_skip( "OpenGL setup failed, error %lu\n", GetLastError() );
            else ok( FALSE, "OpenGL setup %u failed, error %lu\n", i, GetLastError() );
            goto done;
        }
        swap_interval = (void *)wglGetProcAddress( "wglSwapIntervalEXT" );
        if (swap_interval) swap_interval( 0 );
        glViewport( 0, 0, 32, 32 );
        glClearColor( (i + 1) * 3 / 255.0f, 0.5f, 0.25f, 1 );
        glClear( GL_COLOR_BUFFER_BIT );
        ok( SwapBuffers( dcs[i] ), "hidden present %u failed, error %lu\n", i, GetLastError() );
        pump_messages( 10 );
    }
    for (i = 0; i < ARRAY_SIZE(windows); ++i)
    {
        ShowWindow( windows[i], SW_SHOWNA );
        if (!wait_for_surface_idle( windows[i], 7000, &state ))
        {
            ok( FALSE, "surface %u did not become idle\n", i );
            goto done;
        }
        color = GetPixel( dcs[i], 16, 16 );
        ok( color_matches( color, (i + 1) * 3, 128, 64 ),
            "surface %u has unexpected destination pixel %#lx\n", i, color );
        if (!color_matches( color, (i + 1) * 3, 128, 64 )) goto done;
        ShowWindow( windows[i], SW_HIDE );
    }
done:
    wglMakeCurrent( NULL, NULL );
    for (i = 0; i < ARRAY_SIZE(windows); ++i)
    {
        if (contexts[i]) wglDeleteContext( contexts[i] );
        if (dcs[i]) ReleaseDC( windows[i], dcs[i] );
        if (windows[i]) DestroyWindow( windows[i] );
    }
    pump_messages( 20 );
}

static void test_grow64_present_completion(void)
{
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
                                PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 24};
    struct surface_state state = {0};
    HGLRC glrc = NULL;
    HDC hdc = NULL;
    COLORREF color;
    HWND hwnd;
    RECT rect;
    int format;

    if (!register_present_test_class()) return;
    hwnd = CreateWindowExA( WS_EX_LAYERED | WS_EX_TOPMOST, "client_surface_present_race",
                            "grow64 present completion", WS_POPUP, 720, 240, 64, 64,
                            NULL, NULL, GetModuleHandleA( NULL ), NULL );
    ok( !!hwnd, "failed to create grow64 window, error %lu\n", GetLastError() );
    if (!hwnd) return;
    ok( SetLayeredWindowAttributes( hwnd, 0, 255, LWA_ALPHA ),
        "failed to initialize grow64 layered attributes, error %lu\n", GetLastError() );

    hdc = GetDC( hwnd );
    format = hdc ? ChoosePixelFormat( hdc, &pfd ) : 0;
    if (!hdc || !format || !SetPixelFormat( hdc, format, &pfd ) ||
        !(glrc = wglCreateContext( hdc )) || !wglMakeCurrent( hdc, glrc ))
    {
        win_skip( "grow64 OpenGL context setup failed, error %lu\n", GetLastError() );
        goto done;
    }

    GetClientRect( hwnd, &rect );
    ok( rect.right == 64 && rect.bottom == 64,
        "unexpected grow64 initial size %ldx%ld\n", rect.right, rect.bottom );
    glViewport( 0, 0, rect.right, rect.bottom );
    glClearColor( 0.7, 0.2, 0.85, 1.0 );
    glClear( GL_COLOR_BUFFER_BIT );
    ok( SwapBuffers( hdc ), "grow64 reset SwapBuffers failed, error %lu\n", GetLastError() );

    ok( SetWindowPos( hwnd, HWND_TOPMOST, 720, 240, 196, 140,
                      SWP_NOACTIVATE | SWP_SHOWWINDOW ),
        "failed to grow and show window, error %lu\n", GetLastError() );
    GetClientRect( hwnd, &rect );
    ok( rect.right == 196 && rect.bottom == 140,
        "unexpected grown size %ldx%ld\n", rect.right, rect.bottom );
    glViewport( 0, 0, rect.right, rect.bottom );
    glClearColor( 0.15, 0.65, 0.35, 1.0 );
    glClear( GL_COLOR_BUFFER_BIT );
    ok( SwapBuffers( hdc ), "grown SwapBuffers failed, error %lu\n", GetLastError() );

    ok( wait_for_surface_idle( hwnd, 7000, &state ),
        "grown frame did not become idle: staged %u pending %u\n", state.staged, state.pending );
    color = get_gl_front_pixel( &rect );
    ok( color_matches( color, 38, 166, 89 ), "unexpected grown front pixel %#lx\n", color );

done:
    if (glrc) wglMakeCurrent( NULL, NULL );
    if (glrc) wglDeleteContext( glrc );
    if (hdc) ReleaseDC( hwnd, hdc );
    DestroyWindow( hwnd );
}

static BOOL register_present_test_class(void)
{
    WNDCLASSA class = {0};

    class.style = CS_OWNDC;
    class.lpfnWndProc = client_surface_proc;
    class.hInstance = GetModuleHandleA( NULL );
    class.lpszClassName = "client_surface_present_race";
    if (!RegisterClassA( &class ) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        win_skip( "failed to register OpenGL present window, error %lu\n", GetLastError() );
        return FALSE;
    }
    return TRUE;
}

static void test_paced_present_completion(void)
{
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
                                PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 24};
    static const float colors[][3] = {{0.75, 0.25, 0.125}, {0.125, 0.625, 0.875}};
    struct surface_state state;
    HGLRC glrc = NULL;
    HDC hdc = NULL;
    HWND hwnd;
    COLORREF color;
    RECT rect;
    unsigned int status;
    unsigned int i;
    int format;

    if (!register_present_test_class()) return;
    hwnd = CreateWindowExA( WS_EX_LAYERED | WS_EX_TOPMOST, "client_surface_present_race",
                            "paced presents", WS_POPUP, 520, 240, 160, 120,
                            NULL, NULL, GetModuleHandleA( NULL ), NULL );
    ok( !!hwnd, "failed to create paced-present window, error %lu\n", GetLastError() );
    if (!hwnd) return;
    ok( SetLayeredWindowAttributes( hwnd, 0, 255, LWA_ALPHA ),
        "failed to initialize paced-present layered attributes, error %lu\n", GetLastError() );

    hdc = GetDC( hwnd );
    format = hdc ? ChoosePixelFormat( hdc, &pfd ) : 0;
    if (!hdc || !format || !SetPixelFormat( hdc, format, &pfd ) ||
        !(glrc = wglCreateContext( hdc )) || !wglMakeCurrent( hdc, glrc ))
    {
        win_skip( "paced-present OpenGL context setup failed, error %lu\n", GetLastError() );
        goto done;
    }

    GetClientRect( hwnd, &rect );
    glViewport( 0, 0, rect.right, rect.bottom );
    for (i = 0; i < 16; ++i)
    {
        GLenum error;

        glClearColor( colors[i & 1][0], colors[i & 1][1], colors[i & 1][2], 1.0 );
        glClear( GL_COLOR_BUFFER_BIT );
        error = glGetError();
        ok( error == GL_NO_ERROR, "frame %u GL error %#x\n", i, error );
        ok( SwapBuffers( hdc ), "frame %u SwapBuffers failed, error %lu\n",
            i, GetLastError() );
        Sleep( 50 );
    }

    ShowWindow( hwnd, SW_SHOWNA );
    pump_messages( 500 );
    status = set_surface_state( hwnd, 0, 0, 0, &state );
    ok( !status, "paced-present state query failed, status %#x\n", status );
    ok( !state.staged && !state.pending,
        "paced presents not published: staged %u pending %u\n", state.staged, state.pending );
    color = get_gl_front_pixel( &rect );
    ok( color_matches( color, 32, 159, 223 ), "unexpected final paced-present pixel %#lx\n",
        color );

done:
    if (glrc) wglMakeCurrent( NULL, NULL );
    if (glrc) wglDeleteContext( glrc );
    if (hdc) ReleaseDC( hwnd, hdc );
    DestroyWindow( hwnd );
}

static DWORD WINAPI present_race_thread( void *arg )
{
    struct present_race_context *context = arg;
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
                                PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 24};
    HGLRC glrc = NULL;
    HDC hdc = NULL;
    int format;

    if (!(hdc = GetDC( context->hwnd )) || !(format = ChoosePixelFormat( hdc, &pfd )) ||
        !SetPixelFormat( hdc, format, &pfd ) || !(glrc = wglCreateContext( hdc )) ||
        !wglMakeCurrent( hdc, glrc ))
        context->setup_error = GetLastError() ? GetLastError() : ERROR_NOT_SUPPORTED;
    if (!context->setup_error)
    {
        const char *renderer = (const char *)glGetString( GL_RENDERER );

        if (renderer) lstrcpynA( context->renderer, renderer, ARRAY_SIZE(context->renderer) );
        if (context->unpaced)
        {
            BOOL (WINAPI *swap_interval)( int ) = (void *)wglGetProcAddress( "wglSwapIntervalEXT" );
            if (swap_interval) swap_interval( 0 );
        }
    }
    SetEvent( context->ready );

    if (!context->setup_error)
    {
        while (!context->stop)
        {
            glClearColor( 0.25, 0.5, 0.75, 1.0 );
            glClear( GL_COLOR_BUFFER_BIT );
            SwapBuffers( hdc );
            InterlockedIncrement( &context->presents );
        }
        wglMakeCurrent( NULL, NULL );
    }
    if (glrc) wglDeleteContext( glrc );
    if (hdc) ReleaseDC( context->hwnd, hdc );
    return 0;
}

static void test_present_destroy_race(void)
{
    unsigned int i;

    if (!register_present_test_class()) return;

    for (i = 0; i < 10; ++i)
    {
        struct present_race_context context = {0};
        HANDLE thread;
        unsigned int j;

        context.hwnd = CreateWindowA( "client_surface_present_race", "present race",
                                      WS_POPUP | WS_VISIBLE, 20, 20, 160, 120,
                                      NULL, NULL, GetModuleHandleA( NULL ), NULL );
        ok( !!context.hwnd, "failed to create OpenGL race window, error %lu\n", GetLastError() );
        if (!context.hwnd) break;
        context.ready = CreateEventA( NULL, TRUE, FALSE, NULL );
        thread = context.ready ? CreateThread( NULL, 0, present_race_thread, &context, 0, NULL ) : NULL;
        ok( !!context.ready && !!thread, "failed to create present race objects, error %lu\n",
            GetLastError() );
        if (!context.ready || !thread)
        {
            if (thread) CloseHandle( thread );
            if (context.ready) CloseHandle( context.ready );
            DestroyWindow( context.hwnd );
            break;
        }
        ok( WaitForSingleObject( context.ready, 10000 ) == WAIT_OBJECT_0,
            "OpenGL setup timed out\n" );
        if (context.setup_error)
        {
            win_skip( "OpenGL context setup failed, error %ld\n", context.setup_error );
            InterlockedExchange( &context.stop, 1 );
            WaitForSingleObject( thread, 10000 );
            CloseHandle( thread );
            CloseHandle( context.ready );
            DestroyWindow( context.hwnd );
            break;
        }
        if (!i) trace( "OpenGL renderer: %s\n", context.renderer );
        ok( context.renderer[0], "OpenGL renderer is unavailable\n" );
        for (j = 0; j < 200; ++j)
            SetWindowPos( context.hwnd, NULL, 20, 20, 160 + (j & 7), 120 + (j & 3),
                          SWP_NOACTIVATE | SWP_NOZORDER );
        ok( DestroyWindow( context.hwnd ), "failed to destroy OpenGL race window, error %lu\n",
            GetLastError() );
        Sleep( 20 );
        InterlockedExchange( &context.stop, 1 );
        ok( WaitForSingleObject( thread, 10000 ) == WAIT_OBJECT_0,
            "present race thread timed out\n" );
        ok( context.presents > 0, "present race did not execute any presents\n" );
        CloseHandle( thread );
        CloseHandle( context.ready );
    }
}

static void test_native_reparent_race(void)
{
    struct present_race_context context = {.unpaced = TRUE};
    HWND parents[2] = {0};
    HANDLE thread = NULL;
    DWORD start, status, longest = 0;
    unsigned int i;
    BOOL destroyed = FALSE;

    if (!register_present_test_class()) return;
    for (i = 0; i < ARRAY_SIZE(parents); ++i)
    {
        parents[i] = CreateWindowA( "client_surface_present_race", "handoff reparent owner",
                                    WS_POPUP | WS_VISIBLE, 20 + 350 * i, 20, 320, 240,
                                    NULL, NULL, GetModuleHandleA( NULL ), NULL );
        ok( !!parents[i], "failed to create owner %u, error %lu\n", i, GetLastError() );
        if (!parents[i]) goto done;
    }
    /* This same-process child exercises native WSI completion and the owner
     * compositor while the window thread reparents and destroys the HWND. */
    context.hwnd = CreateWindowA( "client_surface_present_race", "handoff reparent source",
                                  WS_CHILD | WS_VISIBLE, 0, 0, 3840, 2160, parents[0],
                                  NULL, GetModuleHandleA( NULL ), NULL );
    ok( !!context.hwnd, "failed to create handoff child, error %lu\n", GetLastError() );
    if (!context.hwnd) goto done;
    context.ready = CreateEventA( NULL, TRUE, FALSE, NULL );
    thread = context.ready ? CreateThread( NULL, 0, present_race_thread, &context, 0, NULL ) : NULL;
    ok( !!context.ready && !!thread, "failed to create handoff race objects, error %lu\n", GetLastError() );
    if (!thread) goto done;
    start = GetTickCount();
    while ((status = WaitForSingleObject( context.ready, 0 )) == WAIT_TIMEOUT &&
           GetTickCount() - start < 10000) pump_messages( 1 );
    ok( status == WAIT_OBJECT_0, "handoff context setup timed out\n" );
    if (status != WAIT_OBJECT_0) goto done;
    if (context.setup_error)
    {
        win_skip( "handoff OpenGL setup failed, error %ld\n", context.setup_error );
        goto done;
    }
    start = GetTickCount();
    while (InterlockedCompareExchange( &context.presents, 0, 0 ) < 8 &&
           GetTickCount() - start < 5000) pump_messages( 1 );
    ok( context.presents >= 8, "handoff producer did not start, presents %ld\n", context.presents );
    for (i = 0; i < 16; ++i)
    {
        HWND parent = parents[(i + 1) % ARRAY_SIZE(parents)];
        LONG before = InterlockedCompareExchange( &context.presents, 0, 0 );
        DWORD elapsed;

        start = GetTickCount();
        ok( !!SetParent( context.hwnd, parent ), "reparent %u failed, error %lu\n", i, GetLastError() );
        elapsed = GetTickCount() - start;
        longest = max( longest, elapsed );
        ok( elapsed < 5000, "reparent %u blocked for %lu ms\n", i, elapsed );
        ok( GetAncestor( context.hwnd, GA_PARENT ) == parent, "reparent %u left the old owner\n", i );
        start = GetTickCount();
        do pump_messages( 1 );
        while (InterlockedCompareExchange( &context.presents, 0, 0 ) <= before &&
               GetTickCount() - start < 5000);
        elapsed = GetTickCount() - start;
        ok( elapsed < 1000, "message processing after reparent %u blocked for %lu ms\n", i, elapsed );
        ok( InterlockedCompareExchange( &context.presents, 0, 0 ) > before,
            "producer stopped after reparent %u\n", i );
        if (InterlockedCompareExchange( &context.presents, 0, 0 ) <= before) break;
    }
    trace( "native reparent renderer %s, presents %ld, longest GUI mutation %lu ms\n",
           context.renderer, context.presents, longest );
    destroyed = DestroyWindow( context.hwnd );
    ok( destroyed, "failed to destroy handoff child, error %lu\n", GetLastError() );
done:
    InterlockedExchange( &context.stop, 1 );
    if (thread)
    {
        start = GetTickCount();
        while ((status = WaitForSingleObject( thread, 0 )) == WAIT_TIMEOUT &&
               GetTickCount() - start < 10000) pump_messages( 1 );
        ok( status == WAIT_OBJECT_0, "handoff reparent thread timed out\n" );
        /* A timed-out thread still owns the stack context; end the failed
         * test process instead of continuing with a dangling context. */
        if (status != WAIT_OBJECT_0) ExitProcess( 1 );
        CloseHandle( thread );
    }
    if (context.ready) CloseHandle( context.ready );
    if (context.hwnd && !destroyed) DestroyWindow( context.hwnd );
    for (i = 0; i < ARRAY_SIZE(parents); ++i)
        if (parents[i]) DestroyWindow( parents[i] );
}

static void test_owner_exit_and_destroy( char **argv )
{
    struct surface_state state;
    HWND hwnd;
    unsigned int status;

    hwnd = create_test_window( FALSE );
    ok( !!hwnd, "failed to create owner window, error %lu\n", GetLastError() );
    if (!hwnd) return;

    if (run_child( argv, "owner_no_queue", hwnd, 0 ))
    {
        status = set_surface_state( hwnd, 0, 0, 0, &state );
        ok( !status, "state query after queue-less owner exit failed, status %#x\n", status );
        ok( !state.active && !state.cached && !state.pending,
            "queue-less owner leaked state: active %u cached %u pending %u\n",
            state.active, state.cached, state.pending );
    }

    ShowWindow( hwnd, SW_HIDE );
    if (run_child( argv, "owner_stalled", hwnd, 0 ))
    {
        status = set_surface_state( hwnd, 0, 0, 0, &state );
        ok( !status, "state query after stalled owner exit failed, status %#x\n", status );
        ok( !state.active && !state.cached && !state.pending,
            "stalled owner leaked state: active %u cached %u pending %u\n",
            state.active, state.cached, state.pending );
    }

    ShowWindow( hwnd, SW_HIDE );
    if (run_child( argv, "owner_exit", hwnd, 0 ))
    {
        status = set_surface_state( hwnd, 0, 0, 0, &state );
        ok( !status, "state query after owner exit failed, status %#x\n", status );
        ok( !state.active && !state.cached && !state.pending,
            "dead owner leaked state: active %u cached %u pending %u\n",
            state.active, state.cached, state.pending );
    }

    run_child( argv, "destroy_race", hwnd, 10 );
}

struct focused_test_case
{
    const char *name;
    const char *description;
    void (*func)(void);
};

static BOOL run_focused_test_case( const char *name, char **argv )
{
    static const struct focused_test_case cases[] =
    {
        {"completion-provenance", "client surface completion result provenance",
         test_completion_result_provenance},
        {"presentation-modes", "client surface presentation modes", test_presentation_modes},
        {"generation-membership", "client surface generation membership",
         test_generation_membership},
        {"clip-scene-snapshot", "client surface clip scene snapshots",
         test_clip_scene_snapshot},
        {"complex-clip-snapshot", "complex client surface clip snapshot", test_complex_clip_snapshot},
        {"scene-region-batch", "client surface region batches", test_scene_region_batch},
        {"subtree-retirement", "client surface subtree retirement",
         test_subtree_generation_retirement},
        {"generation-aba", "client surface generation ABA exclusion", test_generation_aba},
        {"publish-transaction", "client surface host publication transaction",
         test_publish_transaction},
        {"live-prepare", "live client surface prepare transaction",
         test_live_prepare_transaction},
        {"unbacked-live", "unbacked live client surface publication",
         test_unbacked_live_generation},
        {"native-backing-barrier", "native backing destruction barrier",
         test_native_backing_barrier},
        {"demoted-native-barrier", "native backing barrier after demotion", test_demoted_native_barrier},
        {"handoff-storage", "client surface generation handoff storage", test_handoff_storage},
        {"handoff-receipts", "source-independent assembly receipts", test_handoff_receipts},
        {"notification-filter", "client surface notification filter bypass",
         test_notification_identity_aba},
        {"late-present-cutover", "late client surface publication cut-over",
         test_late_present_cutover},
        {"concurrent-state", "concurrent client surface state changes",
         test_concurrent_state_changes},
        {"hidden-present-resize", "hidden present and resize", test_hidden_present_resize},
        {"handoff-bitmap", "real WGL handoff bitmap boundary", test_handoff_bitmap_boundary},
        {"grow64-completion", "64x64 grow presentation completion",
         test_grow64_present_completion},
        {"paced-completion", "paced hidden client surface presents",
         test_paced_present_completion},
        {"present-destroy-race", "present and window destruction race",
         test_present_destroy_race},
        {"native-reparent-race", "native WSI and owner reparent race", test_native_reparent_race},
    };
    unsigned int i;

    if (!strcmp( name, "presentation-modes-destroy-race" ))
    {
        trace( "testing presentation mode teardown before present destruction race\n" );
        test_presentation_modes();
        test_present_destroy_race();
        return TRUE;
    }
    if (!strcmp( name, "owner-exit-destroy-race" ))
    {
        trace( "testing owner teardown before present destruction race\n" );
        test_owner_exit_and_destroy( argv );
        test_present_destroy_race();
        return TRUE;
    }
    for (i = 0; i < ARRAY_SIZE(cases); ++i)
    {
        if (strcmp( name, cases[i].name )) continue;
        trace( "testing %s\n", cases[i].description );
        cases[i].func();
        return TRUE;
    }
    if (!strcmp( name, "cross-process-pixel-format" ))
    {
        trace( "testing cross-process pixel format persistence\n" );
        test_cross_process_pixel_format( argv );
        return TRUE;
    }
    if (!strcmp( name, "handoff-submitted-process-exit" ))
    {
        test_handoff_storage_process_exit( argv, FALSE );
        return TRUE;
    }
    if (!strcmp( name, "handoff-lost-recovery" ))
    {
        test_handoff_lost_recovery();
        return TRUE;
    }
    if (!strcmp( name, "handoff-ready-process-exit" ))
    {
        test_handoff_storage_process_exit( argv, TRUE );
        return TRUE;
    }
    if (!strcmp( name, "handoff-owner-submitted-process-exit" ))
    {
        test_handoff_storage_owner_exit( argv, FALSE );
        return TRUE;
    }
    if (!strcmp( name, "handoff-owner-ready-process-exit" ))
    {
        test_handoff_storage_owner_exit( argv, TRUE );
        return TRUE;
    }
    if (!strcmp( name, "owner-exit-destroy" ))
    {
        trace( "testing owner exit and window destruction\n" );
        test_owner_exit_and_destroy( argv );
        return TRUE;
    }
    return FALSE;
}

START_TEST(client_surface)
{
    HMODULE ntdll = GetModuleHandleA( "ntdll.dll" );
    const char *test_case;
    char **argv;
    int argc;
    HWND hwnd;

    argc = winetest_get_mainargs( &argv );
    p_wine_server_call = (void *)GetProcAddress( ntdll, "wine_server_call" );
    if (!p_wine_server_call)
    {
        win_skip( "Wine server interface is unavailable\n" );
        return;
    }

    if (argc > 6 && !strcmp( argv[2], "handoff_storage_exit_child" ))
    {
        HANDLE ready, release;

        sscanf( argv[3], "%p", &hwnd );
        sscanf( argv[4], "%p", &ready );
        sscanf( argv[5], "%p", &release );
        handoff_storage_exit_child( hwnd, ready, release, atoi( argv[6] ) );
        return;
    }
    if (argc > 9 && !strcmp( argv[2], "handoff_storage_owner_exit_child" ))
    {
        HANDLE mapping, window_ready, binding_ready, owner_ready, release;

        sscanf( argv[3], "%p", &mapping );
        sscanf( argv[4], "%p", &window_ready );
        sscanf( argv[5], "%p", &binding_ready );
        sscanf( argv[6], "%p", &owner_ready );
        sscanf( argv[7], "%p", &release );
        handoff_storage_owner_exit_child( mapping, window_ready, binding_ready, owner_ready,
                                          release, strtoul( argv[8], NULL, 10 ),
                                          strtoull( argv[9], NULL, 16 ) );
        return;
    }

    if (argc > 5 && (!strcmp( argv[2], "owner_exit" ) ||
                     !strcmp( argv[2], "owner_no_queue" ) ||
                     !strcmp( argv[2], "owner_stalled" )))
    {
        HANDLE ready, release;

        sscanf( argv[3], "%p", &hwnd );
        sscanf( argv[4], "%p", &ready );
        sscanf( argv[5], "%p", &release );
        owner_exit_child( hwnd, ready, release, strcmp( argv[2], "owner_no_queue" ),
                          !strcmp( argv[2], "owner_stalled" ) );
        return;
    }
    if (argc > 4 && !strcmp( argv[2], "destroy_race" ))
    {
        HANDLE ready;

        sscanf( argv[3], "%p", &hwnd );
        sscanf( argv[4], "%p", &ready );
        SetEvent( ready );
        destroy_race_child( hwnd );
        return;
    }
    if (argc > 4 && !strcmp( argv[2], "pixel_format" ))
    {
        int format;

        sscanf( argv[3], "%p", &hwnd );
        format = atoi( argv[4] );
        pixel_format_child( hwnd, format );
        return;
    }

    test_case = getenv( "WINETEST_CLIENT_SURFACE_CASE" );
    if (test_case && *test_case)
    {
        GetDesktopWindow();
        if (!run_focused_test_case( test_case, argv ))
            ok( 0, "unknown WINETEST_CLIENT_SURFACE_CASE %s\n", test_case );
        return;
    }

    GetDesktopWindow();
    trace( "testing client surface completion result provenance\n" );
    test_completion_result_provenance();
    trace( "testing client surface generation handoff storage\n" );
    test_handoff_storage();
    test_handoff_receipts();
    test_handoff_lost_recovery();
    test_handoff_storage_process_exit( argv, FALSE );
    test_handoff_storage_process_exit( argv, TRUE );
    test_handoff_storage_owner_exit( argv, FALSE );
    test_handoff_storage_owner_exit( argv, TRUE );
    trace( "testing client surface generations\n" );
    test_generation_aba();
    trace( "testing client surface presentation modes\n" );
    test_presentation_modes();
    trace( "testing client surface host publication transaction\n" );
    test_publish_transaction();
    trace( "testing live client surface prepare transaction\n" );
    test_live_prepare_transaction();
    trace( "testing unbacked live client surface publication\n" );
    test_unbacked_live_generation();
    trace( "testing native backing destruction barrier\n" );
    test_native_backing_barrier();
    trace( "testing native backing barrier after demotion\n" );
    test_demoted_native_barrier();
    trace( "testing client surface notification identity ABA\n" );
    test_notification_identity_aba();
    trace( "testing late client surface publication cut-over\n" );
    test_late_present_cutover();
    trace( "testing client surface generation membership\n" );
    test_generation_membership();
    trace( "testing client surface clip scene snapshots\n" );
    test_clip_scene_snapshot();
    test_scene_region_batch();
    trace( "testing complex client surface clip snapshot\n" );
    test_complex_clip_snapshot();
    trace( "testing client surface subtree retirement\n" );
    test_subtree_generation_retirement();
    trace( "testing concurrent client surface state changes\n" );
    test_concurrent_state_changes();
    trace( "testing owner exit and window destruction\n" );
    test_owner_exit_and_destroy( argv );
    trace( "testing present and window destruction race\n" );
    test_present_destroy_race();
    trace( "testing paced present completion\n" );
    test_paced_present_completion();
    trace( "testing cross-process pixel format persistence\n" );
    test_cross_process_pixel_format( argv );
    trace( "testing hidden present and resize\n" );
    test_hidden_present_resize();
    trace( "testing real WGL handoff bitmap boundary\n" );
    test_handoff_bitmap_boundary();
    trace( "testing 64x64 grow presentation completion\n" );
    test_grow64_present_completion();
}
