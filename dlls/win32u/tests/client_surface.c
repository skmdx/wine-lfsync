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
    return set_surface_state_scene( hwnd, surface, CLIENT_SURFACE_STATE_PRESENT_COMMIT |
                                    CLIENT_SURFACE_STATE_PRESENT_END,
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

static unsigned int begin_surface_write_state( HWND hwnd, UINT_PTR surface,
                                               const struct surface_state *generation,
                                               struct surface_state *state )
{
    return set_surface_state_scene( hwnd, surface, CLIENT_SURFACE_STATE_PRESENT_BEGIN |
                                    CLIENT_SURFACE_STATE_PRESENT_WRITE_LEASE,
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

static unsigned int get_clip_state( HWND hwnd, struct clip_state *state )
{
    struct __server_request_info info;
    struct get_client_surface_clip_windows_request *req =
        &info.u.req.get_client_surface_clip_windows_request;
    const struct get_client_surface_clip_windows_reply *reply =
        &info.u.reply.get_client_surface_clip_windows_reply;
    unsigned int status;

    memset( &info, 0, sizeof(info) );
    memset( state, 0, sizeof(*state) );
    req->__header.req = REQ_get_client_surface_clip_windows;
    req->handle = wine_server_user_handle( hwnd );
    req->dpi.num = 96;
    req->dpi.den = 1;
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
    HRGN shape, shape_part;
    HWND parent, first, second, descendant;
    unsigned int status;

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

struct lease_binding
{
    HANDLE mapping;
    UINT size, offset;
    UINT64 mapping_id, cookie;
};

static unsigned int get_surface_lease( HWND hwnd, UINT_PTR surface, struct lease_binding *binding )
{
    struct __server_request_info info = {0};
    const struct get_client_surface_lease_reply *reply = &info.u.reply.get_client_surface_lease_reply;
    unsigned int status;

    memset( binding, 0, sizeof(*binding) );
    info.u.req.get_client_surface_lease_request.__header.req = REQ_get_client_surface_lease;
    info.u.req.get_client_surface_lease_request.handle = wine_server_user_handle( hwnd );
    info.u.req.get_client_surface_lease_request.surface = surface;
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

static unsigned int release_surface_lease( UINT_PTR surface, UINT64 cookie )
{
    struct __server_request_info info = {0};

    info.u.req.release_client_surface_lease_request.__header.req = REQ_release_client_surface_lease;
    info.u.req.release_client_surface_lease_request.surface = surface;
    info.u.req.release_client_surface_lease_request.cookie = cookie;
    return p_wine_server_call( &info );
}

static unsigned int complete_surface_lease( UINT_PTR surface, UINT64 cookie )
{
    struct __server_request_info info = {0};

    info.u.req.complete_client_surface_lease_request.__header.req = REQ_complete_client_surface_lease;
    info.u.req.complete_client_surface_lease_request.surface = surface;
    info.u.req.complete_client_surface_lease_request.cookie = cookie;
    return p_wine_server_call( &info );
}

static struct client_surface_lease *map_test_surface_lease( HWND hwnd, UINT_PTR identity,
                                                           struct lease_binding *binding, void **view )
{
    unsigned int status;

    *view = NULL;
    status = set_surface_state( hwnd, identity, CLIENT_SURFACE_STATE_REGISTER |
                                CLIENT_SURFACE_STATE_NATIVE_WRITE_LEASE, 0, NULL );
    ok( !status, "lease register status %#x\n", status );
    if (status) return NULL;
    status = get_surface_lease( hwnd, identity, binding );
    ok( !status, "lease bind status %#x\n", status );
    if (status) return NULL;
    *view = MapViewOfFile( binding->mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, binding->size );
    CloseHandle( binding->mapping );
    ok( !!*view, "lease map error %lu\n", GetLastError() );
    if (!*view)
    {
        release_surface_lease( identity, binding->cookie );
        return NULL;
    }
    return (struct client_surface_lease *)((char *)*view + binding->offset);
}

static void test_lease_root_barriers(void)
{
    const UINT_PTR first_id = 0x79200000, second_id = 0x79200001;
    const UINT_PTR first_barrier = 0x79210000, second_barrier = 0x79210001;
    struct client_surface_lease *slots[2] = {0};
    struct lease_binding bindings[2];
    struct surface_state state;
    HWND first = create_test_window( FALSE ), second = create_test_window( FALSE ), child = NULL;
    void *views[2] = {0};
    unsigned int status;
    LONG64 expected;

    ok( first && second, "failed to create shared barrier roots\n" );
    if (!first || !second) goto done;
    child = create_test_child( first, 10 );
    ok( !!child, "failed to create shared barrier child\n" );
    if (!child) goto done;
    slots[0] = map_test_surface_lease( child, first_id, &bindings[0], &views[0] );
    if (!slots[0]) goto done;
    /* No production admission exists yet. Explicitly open this synthetic
     * control word, claim it by CAS, and test the actual server drain protocol.
     * No native copy is submitted by these protocol-only writers. */
    __atomic_store_n( &slots[0]->control, 0, __ATOMIC_SEQ_CST );
    expected = 0;
    ok( __atomic_compare_exchange_n( &slots[0]->control, &expected, GetCurrentThreadId(),
                                     0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ), "first claim failed\n" );
    status = set_server_parent( child, second );
    ok( !status, "shared writer reparent status %#x\n", status );
    ok( slots[0]->control == (CLIENT_SURFACE_LEASE_CLOSED | GetCurrentThreadId()),
        "reparent did not seal old-root writer\n" );
    slots[1] = map_test_surface_lease( child, second_id, &bindings[1], &views[1] );
    if (!slots[1]) goto done;
    __atomic_store_n( &slots[1]->control, GetCurrentThreadId(), __ATOMIC_SEQ_CST );
    status = set_server_parent( first, second );
    ok( !status, "native root demotion status %#x\n", status );

    status = set_surface_state( first, first_barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN, 0, &state );
    ok( !status && state.pending == 1, "demoted root missed its writer: status %#x pending %u\n",
        status, state.pending );
    status = set_surface_state( second, second_barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN, 0, &state );
    ok( !status && state.pending == 1, "new root count includes wrong writers: status %#x pending %u\n",
        status, state.pending );
    status = set_surface_state( first, first_barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_END, 0, NULL );
    ok( status == STATUS_DEVICE_BUSY, "busy old root reopened, status %#x\n", status );
    status = complete_surface_lease( first_id, bindings[0].cookie );
    ok( status == STATUS_DEVICE_BUSY, "nonempty shared writer ACK accepted, status %#x\n", status );
    __atomic_fetch_and( &slots[0]->control, CLIENT_SURFACE_LEASE_CLOSED, __ATOMIC_SEQ_CST );
    status = set_surface_state( first, first_barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN, 0, &state );
    ok( !status && state.pending == 1, "local clear alone drained server barrier: status %#x pending %u\n",
        status, state.pending );
    status = complete_surface_lease( first_id, bindings[0].cookie + 1 );
    ok( status == STATUS_INVALID_PARAMETER, "wrong cookie completed writer, status %#x\n", status );
    status = complete_surface_lease( first_id, bindings[0].cookie );
    ok( !status, "old-root completion status %#x\n", status );
    status = complete_surface_lease( first_id, bindings[0].cookie );
    ok( !status, "duplicate completion status %#x\n", status );
    status = set_surface_state( first, first_barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN, 0, &state );
    ok( !status && !state.pending, "ACK did not drain old root: status %#x pending %u\n", status, state.pending );
    status = set_surface_state( second, second_barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN, 0, &state );
    ok( !status && state.pending == 1, "old-root ACK drained new-root writer: status %#x pending %u\n",
        status, state.pending );
    status = set_surface_state( child, second_id, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    ok( !status, "retire claimed membership status %#x\n", status );
    status = set_surface_state( second, second_barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN, 0, &state );
    ok( !status && state.pending == 1, "retirement dropped shared writer: status %#x pending %u\n",
        status, state.pending );
    status = release_surface_lease( second_id, bindings[1].cookie );
    ok( status == STATUS_DEVICE_BUSY, "retired busy binding released, status %#x\n", status );
    __atomic_fetch_and( &slots[1]->control, CLIENT_SURFACE_LEASE_CLOSED, __ATOMIC_SEQ_CST );
    status = release_surface_lease( second_id, bindings[1].cookie );
    ok( !status, "retired binding ACK status %#x\n", status );
    slots[1] = NULL;
    status = set_surface_state( second, second_barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN, 0, &state );
    ok( !status && !state.pending, "binding ACK did not drain new root: status %#x pending %u\n", status, state.pending );
    status = set_surface_state( first, first_barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_END, 0, NULL );
    ok( !status, "old native barrier end status %#x\n", status );
    status = set_surface_state( second, second_barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_END, 0, NULL );
    ok( !status, "new native barrier end status %#x\n", status );
done:
    if (slots[0])
    {
        __atomic_store_n( &slots[0]->control, CLIENT_SURFACE_LEASE_CLOSED, __ATOMIC_SEQ_CST );
        release_surface_lease( first_id, bindings[0].cookie );
    }
    if (slots[1])
    {
        __atomic_store_n( &slots[1]->control, CLIENT_SURFACE_LEASE_CLOSED, __ATOMIC_SEQ_CST );
        release_surface_lease( second_id, bindings[1].cookie );
    }
    if (first) set_surface_state( first, first_barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_END, 0, NULL );
    if (second) set_surface_state( second, second_barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_END, 0, NULL );
    if (views[0]) UnmapViewOfFile( views[0] );
    if (views[1]) UnmapViewOfFile( views[1] );
    if (child) DestroyWindow( child );
    if (first) DestroyWindow( first );
    if (second) DestroyWindow( second );
}

static DWORD WINAPI lease_claim_and_exit( void *arg )
{
    struct client_surface_lease *slot = arg;
    LONG64 expected = 0;

    return !__atomic_compare_exchange_n( &slot->control, &expected, GetCurrentThreadId(),
                                         0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST );
}

static void test_lease_writer_thread_exit(void)
{
    const UINT_PTR identity = 0x79300000, barrier = 0x79310000;
    struct lease_binding binding;
    struct client_surface_lease *slot;
    struct surface_state state;
    HWND hwnd = create_test_window( FALSE );
    void *view = NULL;
    HANDLE thread;
    DWORD tid, code;
    unsigned int status;

    ok( !!hwnd, "failed to create thread-exit root\n" );
    if (!hwnd) return;
    slot = map_test_surface_lease( hwnd, identity, &binding, &view );
    if (!slot) goto done;
    __atomic_store_n( &slot->control, 0, __ATOMIC_SEQ_CST );
    thread = CreateThread( NULL, 0, lease_claim_and_exit, slot, 0, &tid );
    ok( !!thread, "claim thread creation error %lu\n", GetLastError() );
    if (!thread) goto release;
    ok( WaitForSingleObject( thread, 10000 ) == WAIT_OBJECT_0, "claim thread exit timed out\n" );
    GetExitCodeThread( thread, &code );
    ok( !code, "claim thread CAS failed, code %lu\n", code );
    CloseHandle( thread );
    ok( slot->control == (CLIENT_SURFACE_LEASE_CLOSED | tid), "thread exit failed to seal claim\n" );
    status = set_surface_state( hwnd, barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN, 0, &state );
    ok( !status && state.pending == 1, "thread death acknowledged native completion: status %#x pending %u\n",
        status, state.pending );
    status = complete_surface_lease( identity, binding.cookie );
    ok( status == STATUS_DEVICE_BUSY, "dead nonempty claim completed, status %#x\n", status );
    /* The synthetic worker issued no native work. A real renderer may clear
     * this word from another thread only after proving that work completed. */
    __atomic_fetch_and( &slot->control, CLIENT_SURFACE_LEASE_CLOSED, __ATOMIC_SEQ_CST );
    status = complete_surface_lease( identity, binding.cookie );
    ok( !status, "surviving thread completion status %#x\n", status );
    status = set_surface_state( hwnd, barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN, 0, &state );
    ok( !status && !state.pending, "surviving ACK did not drain: status %#x pending %u\n", status, state.pending );
release:
    __atomic_store_n( &slot->control, CLIENT_SURFACE_LEASE_CLOSED, __ATOMIC_SEQ_CST );
    release_surface_lease( identity, binding.cookie );
    set_surface_state( hwnd, barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_END, 0, NULL );
done:
    if (view) UnmapViewOfFile( view );
    set_surface_state( hwnd, identity, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    DestroyWindow( hwnd );
}

static void test_lease_storage(void)
{
    NTSTATUS (WINAPI *query_process)( HANDLE, PROCESSINFOCLASS, void *, ULONG, ULONG * ) =
        (void *)GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "NtQueryInformationProcess" );
    const UINT_PTR identity = 0x79000000;
    const UINT flags = CLIENT_SURFACE_STATE_REGISTER | CLIENT_SURFACE_STATE_NATIVE_WRITE_LEASE |
                       CLIENT_SURFACE_STATE_SCENE_PUBLICATION;
    struct lease_binding first, again, replacement, *bindings;
    struct client_surface_lease *slot;
    HWND hwnd = create_test_window( FALSE ), other = create_test_window( FALSE );
    char *view = NULL;
    unsigned int status, i, count = 0;
    DWORD handles_before = 0, handles_after = 0;

    ok( hwnd && other, "failed to create lease storage windows\n" );
    if (!hwnd || !other) goto done;
    status = get_surface_lease( hwnd, identity, &first );
    ok( status == STATUS_INVALID_PARAMETER, "unregistered lease status %#x\n", status );
    status = set_surface_state( hwnd, identity, CLIENT_SURFACE_STATE_REGISTER, 0, NULL );
    ok( !status, "register unsupported producer status %#x\n", status );
    status = get_surface_lease( hwnd, identity, &first );
    ok( status == STATUS_INVALID_PARAMETER, "unsupported producer lease status %#x\n", status );
    status = set_surface_state( hwnd, identity, flags, 0, NULL );
    ok( !status, "register native writer status %#x\n", status );
    status = get_surface_lease( hwnd, identity, &first );
    ok( !status, "get native writer storage status %#x\n", status );
    if (status) goto unregister;
    ok( first.size == CLIENT_SURFACE_LEASE_SLOTS * sizeof(*slot) &&
        first.offset + sizeof(*slot) <= first.size && !(first.offset % sizeof(*slot)) &&
        first.cookie && first.mapping_id, "invalid lease layout or identity\n" );
    view = MapViewOfFile( first.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, first.size );
    ok( !!view, "map lease section failed, error %lu\n", GetLastError() );
    CloseHandle( first.mapping );
    if (!view)
    {
        release_surface_lease( identity, first.cookie );
        goto unregister;
    }
    slot = (struct client_surface_lease *)(view + first.offset);
    ok( slot->control == CLIENT_SURFACE_LEASE_CLOSED && slot->cookie == first.cookie &&
        slot->identity == identity, "new capability is not closed or has wrong identity\n" );
    status = get_surface_lease( other, identity, &again );
    ok( status == STATUS_INVALID_PARAMETER, "wrong HWND admitted storage, status %#x\n", status );
    status = get_surface_lease( hwnd, identity, &again );
    ok( !status, "repeat query status %#x\n", status );
    if (!status)
    {
        ok( again.mapping_id == first.mapping_id && again.offset == first.offset &&
            again.cookie == first.cookie, "repeat query replaced the binding\n" );
        CloseHandle( again.mapping );
    }
    status = release_surface_lease( identity, first.cookie + 1 );
    ok( status == STATUS_INVALID_PARAMETER, "wrong cookie retired binding, status %#x\n", status );
    /* This deliberately mutates the writable view. The server must observe the
     * same word and refuse reuse while a native writer could still own it. */
    __atomic_store_n( &slot->control, GetCurrentThreadId(), __ATOMIC_SEQ_CST );
    status = release_surface_lease( identity, first.cookie );
    ok( status == STATUS_DEVICE_BUSY, "busy binding released, status %#x\n", status );
    ok( slot->control == (CLIENT_SURFACE_LEASE_CLOSED | GetCurrentThreadId()),
        "server did not preserve and seal the shared writer word\n" );
    __atomic_store_n( &slot->control, CLIENT_SURFACE_LEASE_CLOSED, __ATOMIC_SEQ_CST );
    slot->cookie = 0; /* validation must use the server-owned cookie */
    status = release_surface_lease( identity, first.cookie );
    ok( !status, "release of mapped binding status %#x\n", status );
    status = get_surface_lease( hwnd, identity, &replacement );
    ok( !status, "replacement binding status %#x\n", status );
    if (status) goto unregister;
    CloseHandle( replacement.mapping );
    ok( replacement.mapping_id == first.mapping_id && replacement.offset == first.offset &&
        replacement.cookie != first.cookie, "slot was not safely reused with a new cookie\n" );
    ok( slot->control == CLIENT_SURFACE_LEASE_CLOSED && slot->cookie == replacement.cookie,
        "existing mapped view does not observe the replacement binding\n" );
    status = release_surface_lease( identity, first.cookie );
    ok( status == STATUS_INVALID_PARAMETER, "stale ACK retired new binding, status %#x\n", status );
    status = set_surface_state( hwnd, identity, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    ok( !status, "unregister while mapped status %#x\n", status );
    status = set_surface_state( other, identity, flags, 0, NULL );
    ok( status == STATUS_INVALID_PARAMETER, "unacknowledged identity was reused, status %#x\n", status );
    status = release_surface_lease( identity, replacement.cookie );
    ok( !status, "release retired identity without HWND status %#x\n", status );
    status = set_surface_state( other, identity, flags, 0, NULL );
    ok( !status, "released identity cannot be reused, status %#x\n", status );
    set_surface_state( other, identity, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    UnmapViewOfFile( view );
    view = NULL;

    /* Cross a pool boundary, release everything, and verify all slots are
     * reused without unbounded section handles or new mapping identities. */
    bindings = calloc( CLIENT_SURFACE_LEASE_SLOTS + 1, sizeof(*bindings) );
    ok( !!bindings, "failed to allocate binding test data\n" );
    if (!bindings) goto done;
    status = query_process( GetCurrentProcess(), ProcessHandleCount, &handles_before,
                            sizeof(handles_before), NULL );
    ok( !status, "query initial handle count status %#x\n", status );
    for (i = 0; i <= CLIENT_SURFACE_LEASE_SLOTS; ++i)
    {
        status = set_surface_state( hwnd, identity + i, flags, 0, NULL );
        ok( !status, "pool register %u status %#x\n", i, status );
        if (status) break;
        status = get_surface_lease( hwnd, identity + i, &bindings[i] );
        ok( !status, "pool binding %u status %#x\n", i, status );
        if (status)
        {
            set_surface_state( hwnd, identity + i, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
            break;
        }
        CloseHandle( bindings[i].mapping );
        ++count;
    }
    if (count > CLIENT_SURFACE_LEASE_SLOTS)
        ok( bindings[0].mapping_id != bindings[CLIENT_SURFACE_LEASE_SLOTS].mapping_id,
            "pool overflow reused a live slot\n" );
    for (i = 0; i < count; ++i)
    {
        set_surface_state( hwnd, identity + i, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
        status = release_surface_lease( identity + i, bindings[i].cookie );
        ok( !status, "pool binding release %u status %#x\n", i, status );
    }
    if (count)
    {
        set_surface_state( hwnd, identity, flags, 0, NULL );
        status = get_surface_lease( hwnd, identity, &again );
        ok( !status, "reuse drained pool status %#x\n", status );
        if (!status)
        {
            ok( again.mapping_id == bindings[0].mapping_id && again.offset == bindings[0].offset &&
                again.cookie != bindings[0].cookie, "drained pool not reused safely\n" );
            CloseHandle( again.mapping );
            release_surface_lease( identity, again.cookie );
        }
        set_surface_state( hwnd, identity, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    }
    free( bindings );
    set_surface_state( other, identity, flags, 0, NULL );
    status = get_surface_lease( other, identity, &again );
    ok( !status, "bind before HWND destruction status %#x\n", status );
    if (!status)
    {
        char *retained = MapViewOfFile( again.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, again.size );
        ok( !!retained, "map retained destroy view error %lu\n", GetLastError() );
        CloseHandle( again.mapping );
        DestroyWindow( other );
        other = NULL;
        if (retained)
        {
            slot = (struct client_surface_lease *)(retained + again.offset);
            ok( slot->control == CLIENT_SURFACE_LEASE_CLOSED, "destroyed HWND left capability open\n" );
            UnmapViewOfFile( retained );
        }
        status = release_surface_lease( identity, again.cookie );
        ok( !status, "release binding after HWND destruction status %#x\n", status );
    }
    status = query_process( GetCurrentProcess(), ProcessHandleCount, &handles_after,
                            sizeof(handles_after), NULL );
    ok( !status, "query final handle count status %#x\n", status );
    ok( handles_after <= handles_before + 4, "lease handles grew from %lu to %lu\n",
        handles_before, handles_after );
    goto done;
unregister:
    set_surface_state( hwnd, identity, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
done:
    if (view) UnmapViewOfFile( view );
    if (hwnd) DestroyWindow( hwnd );
    if (other) DestroyWindow( other );
}

#define LEASE_PARENT_ID 0x79100000
#define LEASE_CHILD_ID  0x79100001

static void lease_storage_child( HWND hwnd, HANDLE ready, HANDLE release, BOOL claimed )
{
    struct lease_binding binding;
    unsigned int status;
    void *view = NULL;

    status = get_surface_lease( hwnd, LEASE_PARENT_ID, &binding );
    ok( status == STATUS_INVALID_PARAMETER, "child mapped parent capability, status %#x\n", status );
    if (!status) CloseHandle( binding.mapping );
    status = set_surface_state( hwnd, LEASE_CHILD_ID, CLIENT_SURFACE_STATE_REGISTER |
                                CLIENT_SURFACE_STATE_NATIVE_WRITE_LEASE, 0, NULL );
    ok( !status, "child register status %#x\n", status );
    status = get_surface_lease( hwnd, LEASE_CHILD_ID, &binding );
    ok( !status, "child lease binding status %#x\n", status );
    if (!status)
    {
        view = MapViewOfFile( binding.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, binding.size );
        ok( !!view, "child map lease section error %lu\n", GetLastError() );
        CloseHandle( binding.mapping );
        if (view && claimed)
        {
            struct client_surface_lease *slot = (void *)((char *)view + binding.offset);
            /* Synthetic accounting test only; no native target write occurs. */
            __atomic_store_n( &slot->control, GetCurrentThreadId(), __ATOMIC_SEQ_CST );
        }
    }
    SetEvent( ready );
    ok( WaitForSingleObject( release, 10000 ) == WAIT_OBJECT_0, "child release timed out\n" );
    /* Deliberately omit unregister, binding ACK and UnmapViewOfFile. The final
     * thread/process cleanup must retire the foreign-HWND membership and pool. */
}

static void test_lease_storage_process_exit( char **argv, BOOL claimed )
{
    SECURITY_ATTRIBUTES attr = {sizeof(attr), NULL, TRUE};
    STARTUPINFOA startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process;
    struct lease_binding own, foreign, again;
    struct surface_state state;
    HWND hwnd = create_test_window( FALSE );
    HANDLE ready = NULL, release = NULL;
    unsigned int status;
    char command[MAX_PATH * 2];

    ok( !!hwnd, "failed to create lease process window\n" );
    if (!hwnd) return;
    status = set_surface_state( hwnd, LEASE_PARENT_ID, CLIENT_SURFACE_STATE_REGISTER |
                                CLIENT_SURFACE_STATE_NATIVE_WRITE_LEASE, 0, NULL );
    ok( !status, "parent registration status %#x\n", status );
    status = get_surface_lease( hwnd, LEASE_PARENT_ID, &own );
    ok( !status, "parent binding status %#x\n", status );
    if (status) goto done;
    CloseHandle( own.mapping );
    ready = CreateEventA( &attr, TRUE, FALSE, NULL );
    release = CreateEventA( &attr, TRUE, FALSE, NULL );
    ok( ready && release, "failed to create lease process events\n" );
    if (!ready || !release) goto release_binding;
    sprintf( command, "\"%s\" %s lease_storage_child %p %p %p %u", argv[0], argv[1], hwnd, ready, release, claimed );
    if (!CreateProcessA( NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process ))
    {
        ok( 0, "lease child CreateProcess error %lu\n", GetLastError() );
        goto release_binding;
    }
    ok( WaitForSingleObject( ready, 10000 ) == WAIT_OBJECT_0, "lease child did not become ready\n" );
    status = get_surface_lease( hwnd, LEASE_CHILD_ID, &foreign );
    ok( status == STATUS_INVALID_PARAMETER, "parent mapped child capability, status %#x\n", status );
    if (!status) CloseHandle( foreign.mapping );
    status = set_surface_state( hwnd, 0, 0, 0, &state );
    ok( !status && state.active == 2, "parent/child memberships status %#x active %u\n", status, state.active );
    if (claimed)
    {
        status = set_surface_state( hwnd, LEASE_PARENT_ID, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN, 0, &state );
        ok( !status && state.pending == 1, "child claim was not counted: status %#x pending %u\n",
            status, state.pending );
    }
    SetEvent( release );
    wait_child_process( &process );
    CloseHandle( process.hThread );
    CloseHandle( process.hProcess );
    status = set_surface_state( hwnd, 0, 0, 0, &state );
    ok( !status && state.active == 1, "child leaked membership status %#x active %u\n", status, state.active );
    if (claimed)
    {
        status = set_surface_state( hwnd, LEASE_PARENT_ID, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN, 0, &state );
        ok( !status && !state.pending, "dead synthetic child leaked writer accounting: status %#x pending %u\n",
            status, state.pending );
        status = set_surface_state( hwnd, LEASE_PARENT_ID, CLIENT_SURFACE_STATE_NATIVE_BARRIER_END, 0, NULL );
        ok( !status, "parent barrier did not end after child cleanup, status %#x\n", status );
    }
    status = get_surface_lease( hwnd, LEASE_PARENT_ID, &again );
    ok( !status, "parent binding lost after child exit, status %#x\n", status );
    if (!status)
    {
        ok( again.mapping_id == own.mapping_id && again.cookie == own.cookie,
            "child cleanup changed parent capability\n" );
        CloseHandle( again.mapping );
    }
release_binding:
    release_surface_lease( LEASE_PARENT_ID, own.cookie );
done:
    if (ready) CloseHandle( ready );
    if (release) CloseHandle( release );
    set_surface_state( hwnd, LEASE_PARENT_ID, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    DestroyWindow( hwnd );
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

static void test_scene_writer_barrier(void)
{
    const UINT_PTR surface = 0x123c0000;
    struct surface_state state, composing, ready, steady, blocked, repaired;
    HWND hwnd;
    unsigned int status;

    hwnd = create_test_window( TRUE );
    ok( !!hwnd, "failed to create writer barrier window, error %lu\n", GetLastError() );
    if (!hwnd) return;

    status = set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_REGISTER |
                                CLIENT_SURFACE_STATE_SCENE_PUBLICATION |
                                CLIENT_SURFACE_STATE_NATIVE_WRITE_LEASE, 0, NULL );
    ok( !status, "writer barrier register failed, status %#x\n", status );
    status = claim_surface_state( hwnd, surface, &state );
    ok( !status && !state.generation, "writer barrier claim started before prepare, status %#x\n", status );
    status = prepare_surface_state( hwnd, &composing );
    ok( !status && composing.generation && composing.pending == 1,
        "writer barrier initial prepare failed: status %#x generation %s pending %u\n",
        status, wine_dbgstr_longlong( composing.generation ), composing.pending );
    status = commit_surface_state( hwnd, surface, &composing, &ready );
    ok( !status && ready.ready && !ready.pending,
        "writer barrier initial composition failed: status %#x ready %u pending %u\n",
        status, ready.ready, ready.pending );
    status = publish_surface_state( hwnd, &state );
    ok( !status && !state.generation && !state.ready,
        "writer barrier initial publication failed: status %#x generation %s ready %u\n",
        status, wine_dbgstr_longlong( state.generation ), state.ready );

    status = begin_surface_write_state( hwnd, surface, &state, &steady );
    ok( !status && steady.compose,
        "steady backing writer was not admitted: status %#x compose %u\n", status, steady.compose );
    SetWindowPos( hwnd, NULL, 17, 10, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
    status = set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_PREPARE_BEGIN, 0, &blocked );
    ok( !status && !blocked.publish && !blocked.generation,
        "scene prepare crossed an active backing writer: status %#x prepare %u generation %s\n",
        status, blocked.publish, wine_dbgstr_longlong( blocked.generation ) );

    status = commit_surface_state( hwnd, surface, &state, &blocked );
    ok( !status && !blocked.generation,
        "stale writer completion entered the new scene: status %#x generation %s\n",
        status, wine_dbgstr_longlong( blocked.generation ) );
    status = prepare_surface_state( hwnd, &composing );
    ok( !status && composing.generation && composing.pending == 1,
        "writer release did not start scene repair: status %#x generation %s pending %u\n",
        status, wine_dbgstr_longlong( composing.generation ), composing.pending );
    status = commit_surface_state( hwnd, surface, &composing, &repaired );
    ok( !status && repaired.ready && !repaired.pending,
        "writer barrier repair did not become ready: status %#x ready %u pending %u\n",
        status, repaired.ready, repaired.pending );
    status = publish_surface_state( hwnd, &repaired );
    ok( !status && !repaired.generation && !repaired.pending,
        "writer barrier repair did not publish: status %#x generation %s pending %u\n",
        status, wine_dbgstr_longlong( repaired.generation ), repaired.pending );

    set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    DestroyWindow( hwnd );
}

static void test_backend_capability_isolation(void)
{
    const UINT_PTR surface = 0x123b8000, barrier = 0x45668000;
    struct surface_state state, composing, ready, steady, denied, sealed;
    HWND hwnd;
    unsigned int status;

    hwnd = create_test_window( TRUE );
    ok( !!hwnd, "failed to create capability isolation window, error %lu\n", GetLastError() );
    if (!hwnd) return;

    status = set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_REGISTER |
                                CLIENT_SURFACE_STATE_SCENE_PUBLICATION, 0, NULL );
    ok( !status, "capability isolation register failed, status %#x\n", status );
    claim_surface_state( hwnd, surface, &state );
    status = prepare_surface_state( hwnd, &composing );
    ok( !status && composing.generation && composing.pending == 1,
        "capability isolation prepare failed: status %#x generation %s pending %u\n",
        status, wine_dbgstr_longlong( composing.generation ), composing.pending );
    status = commit_surface_state( hwnd, surface, &composing, &ready );
    ok( !status && ready.ready, "capability isolation composition failed, status %#x ready %u\n",
        status, ready.ready );
    status = publish_surface_state( hwnd, &state );
    ok( !status && !state.generation,
        "capability isolation publication failed, status %#x generation %s\n",
        status, wine_dbgstr_longlong( state.generation ) );

    status = begin_surface_state( hwnd, surface, &state, &steady );
    ok( !status && steady.compose,
        "scene publication backend was not admitted, status %#x compose %u\n",
        status, steady.compose );
    status = begin_surface_write_state( hwnd, surface, &state, &denied );
    ok( !status && !denied.compose,
        "publication-only backend was granted a write lease, status %#x compose %u\n",
        status, denied.compose );
    status = set_surface_state( hwnd, barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN,
                                0, &sealed );
    ok( !status && !sealed.pending && (sealed.scene_generation & 1),
        "publication-only backend acquired a write lease: status %#x pending %u scene %s\n",
        status, sealed.pending, wine_dbgstr_longlong( sealed.scene_generation ) );
    status = set_surface_state( hwnd, barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_END,
                                0, &sealed );
    ok( !status && !(sealed.scene_generation & 1),
        "capability isolation barrier did not reopen: status %#x scene %s\n",
        status, wine_dbgstr_longlong( sealed.scene_generation ) );
    set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_PRESENT_END, 0, NULL );
    set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    DestroyWindow( hwnd );
}

static void test_native_backing_barrier(void)
{
    const UINT_PTR surface = 0x123d0000, barrier = 0x45670000;
    struct surface_state state, composing, ready, steady, sealed, blocked;
    HWND hwnd;
    unsigned int status;

    hwnd = create_test_window( TRUE );
    ok( !!hwnd, "failed to create native barrier window, error %lu\n", GetLastError() );
    if (!hwnd) return;

    status = set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_REGISTER |
                                CLIENT_SURFACE_STATE_SCENE_PUBLICATION |
                                CLIENT_SURFACE_STATE_NATIVE_WRITE_LEASE, 0, NULL );
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

    status = begin_surface_write_state( hwnd, surface, &state, &steady );
    ok( !status && steady.compose, "native barrier writer was not admitted, status %#x\n", status );
    status = set_surface_state( hwnd, barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN,
                                0, &sealed );
    ok( !status && sealed.pending == 1 && (sealed.scene_generation & 1),
        "native barrier did not seal one writer: status %#x pending %u scene %s\n",
        status, sealed.pending, wine_dbgstr_longlong( sealed.scene_generation ) );
    status = set_surface_state( hwnd, barrier + 1, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN,
                                0, NULL );
    ok( status == STATUS_DEVICE_BUSY, "competing native barrier returned %#x\n", status );
    status = set_surface_state( hwnd, barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_END, 0, NULL );
    ok( status == STATUS_DEVICE_BUSY, "active native barrier ended with status %#x\n", status );

    status = begin_surface_write_state( hwnd, surface, &state, &blocked );
    ok( !status && !blocked.compose,
        "new writer crossed native barrier: status %#x compose %u\n", status, blocked.compose );
    status = commit_surface_state( hwnd, surface, &state, &sealed );
    ok( !status, "native barrier writer release failed, status %#x\n", status );
    status = set_surface_state( hwnd, barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN,
                                0, &sealed );
    ok( !status && !sealed.pending && (sealed.scene_generation & 1),
        "native barrier did not drain: status %#x pending %u scene %s\n",
        status, sealed.pending, wine_dbgstr_longlong( sealed.scene_generation ) );
    status = set_surface_state( hwnd, barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_END,
                                0, &sealed );
    ok( !status && !(sealed.scene_generation & 1),
        "native barrier did not reopen scene: status %#x scene %s\n",
        status, wine_dbgstr_longlong( sealed.scene_generation ) );

    set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    DestroyWindow( hwnd );
}

static void test_reparent_writer_leases(void)
{
    const UINT_PTR first_surface = 0x123d4000, second_surface = 0x123d4001;
    const UINT_PTR first_barrier = 0x45674000, second_barrier = 0x45674001;
    struct surface_state state, sealed;
    HWND first, second, child = NULL;
    unsigned int status;

    first = create_test_window( TRUE );
    second = create_test_window( TRUE );
    ok( !!first && !!second, "failed to create reparent lease parents, error %lu\n", GetLastError() );
    if (!first || !second) goto done;
    child = create_test_child( first, 10 );
    ok( !!child, "failed to create reparent lease child, error %lu\n", GetLastError() );
    if (!child) goto done;

    status = set_surface_state( child, first_surface, CLIENT_SURFACE_STATE_REGISTER |
                                CLIENT_SURFACE_STATE_NATIVE_WRITE_LEASE, 0, NULL );
    ok( !status, "first lease registration failed, status %#x\n", status );
    claim_surface_state( child, first_surface, &state );
    status = begin_surface_write_state( child, first_surface, &state, &state );
    ok( !status && state.compose, "first writer was not admitted, status %#x compose %u\n",
        status, state.compose );

    ok( SetParent( child, second ) == first, "failed to reparent lease child, error %lu\n", GetLastError() );
    status = set_surface_state( child, second_surface, CLIENT_SURFACE_STATE_REGISTER |
                                CLIENT_SURFACE_STATE_NATIVE_WRITE_LEASE, 0, NULL );
    ok( !status, "second lease registration failed, status %#x\n", status );
    claim_surface_state( child, second_surface, &state );
    status = begin_surface_write_state( child, second_surface, &state, &state );
    ok( !status && state.compose, "second writer was not admitted, status %#x compose %u\n",
        status, state.compose );

    status = set_surface_state( first, first_barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN, 0, &sealed );
    ok( !status && sealed.pending == 1, "old parent lost its writer: status %#x pending %u\n",
        status, sealed.pending );
    status = set_surface_state( second, second_barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN, 0, &sealed );
    ok( !status && sealed.pending == 1, "new parent lost its writer: status %#x pending %u\n",
        status, sealed.pending );

    ok( DestroyWindow( child ), "failed to destroy lease child, error %lu\n", GetLastError() );
    child = NULL;
    status = set_surface_state( first, first_barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN, 0, &sealed );
    ok( !status && !sealed.pending, "old parent retained a destroyed writer: status %#x pending %u\n",
        status, sealed.pending );
    status = set_surface_state( first, first_barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_END, 0, &sealed );
    ok( !status && !(sealed.scene_generation & 1), "old parent barrier did not end, status %#x\n", status );
    status = set_surface_state( second, second_barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN, 0, &sealed );
    ok( !status && !sealed.pending, "new parent retained a destroyed writer: status %#x pending %u\n",
        status, sealed.pending );
    status = set_surface_state( second, second_barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_END, 0, &sealed );
    ok( !status && !(sealed.scene_generation & 1), "new parent barrier did not end, status %#x\n", status );
done:
    if (child) DestroyWindow( child );
    if (second) DestroyWindow( second );
    if (first) DestroyWindow( first );
}

static void test_demoted_native_barrier(void)
{
    const UINT_PTR surface = 0x123d6000, barrier = 0x45676000;
    struct surface_state state, sealed;
    HWND first, second;
    unsigned int status;

    first = create_test_window( TRUE );
    second = create_test_window( TRUE );
    ok( !!first && !!second, "failed to create demotion parents, error %lu\n", GetLastError() );
    if (!first || !second) goto done;
    set_surface_state( first, surface, CLIENT_SURFACE_STATE_REGISTER |
                       CLIENT_SURFACE_STATE_NATIVE_WRITE_LEASE, 0, NULL );
    claim_surface_state( first, surface, &state );
    status = begin_surface_write_state( first, surface, &state, &state );
    ok( !status && state.compose, "demotion writer not admitted: status %#x compose %u\n",
        status, state.compose );

    /* SetParent updates the server hierarchy before destroying the former
     * top-level's native window.  Stop at this server boundary so the test
     * can finish the writer while the native teardown barrier is sealed. */
    status = set_server_parent( first, second );
    ok( !status, "server demotion failed, status %#x\n", status );
    status = set_surface_state( first, barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN, 0, &sealed );
    ok( !status && sealed.toplevel == first && sealed.pending == 1,
        "native barrier followed new hierarchy: status %#x target %p pending %u\n",
        status, sealed.toplevel, sealed.pending );
    status = set_surface_state( first, barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_END, 0, NULL );
    ok( status == STATUS_DEVICE_BUSY, "demoted barrier ignored old writer, status %#x\n", status );
    set_surface_state( first, surface, CLIENT_SURFACE_STATE_PRESENT_END, 0, NULL );
    /* Reopen even after a failed expectation so the baseline can clean up. */
    set_surface_state( first, barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN, 0, NULL );
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

struct writer_exit_context
{
    HWND hwnd;
    UINT_PTR surface;
    struct surface_state scene;
    HANDLE ready;
    HANDLE release;
    unsigned int status;
    BOOL compose;
};

static DWORD WINAPI writer_exit_thread( void *arg )
{
    struct writer_exit_context *context = arg;
    struct surface_state state = {0};

    context->status = begin_surface_write_state( context->hwnd, context->surface,
                                                 &context->scene, &state );
    context->compose = state.compose;
    SetEvent( context->ready );
    WaitForSingleObject( context->release, 10000 );
    return 0; /* deliberately omit PRESENT_END */
}

static void test_writer_thread_exit(void)
{
    const UINT_PTR surface = 0x123e0000, barrier = 0x45678000;
    struct writer_exit_context context = {0};
    struct surface_state state = {0}, composing = {0}, ready = {0}, sealed = {0};
    HANDLE thread = NULL;
    HWND hwnd;
    unsigned int status;

    hwnd = create_test_window( TRUE );
    ok( !!hwnd, "failed to create writer-exit window, error %lu\n", GetLastError() );
    if (!hwnd) return;

    status = set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_REGISTER |
                                CLIENT_SURFACE_STATE_SCENE_PUBLICATION |
                                CLIENT_SURFACE_STATE_NATIVE_WRITE_LEASE, 0, NULL );
    ok( !status, "writer-exit register failed, status %#x\n", status );
    claim_surface_state( hwnd, surface, &state );
    status = prepare_surface_state( hwnd, &composing );
    ok( !status && composing.generation && composing.pending == 1,
        "writer-exit prepare failed: status %#x generation %s pending %u\n",
        status, wine_dbgstr_longlong( composing.generation ), composing.pending );
    status = commit_surface_state( hwnd, surface, &composing, &ready );
    ok( !status && ready.ready, "writer-exit composition failed, status %#x ready %u\n",
        status, ready.ready );
    status = publish_surface_state( hwnd, &state );
    ok( !status && !state.generation, "writer-exit publication failed, status %#x\n", status );

    context.hwnd = hwnd;
    context.surface = surface;
    context.scene = state;
    context.ready = CreateEventA( NULL, TRUE, FALSE, NULL );
    context.release = CreateEventA( NULL, TRUE, FALSE, NULL );
    if (context.ready && context.release)
        thread = CreateThread( NULL, 0, writer_exit_thread, &context, 0, NULL );
    ok( !!context.ready && !!context.release && !!thread,
        "failed to create writer-exit synchronization, error %lu\n", GetLastError() );
    if (thread && WaitForSingleObject( context.ready, 10000 ) == WAIT_OBJECT_0)
    {
        ok( !context.status && context.compose,
            "writer-exit lease failed: status %#x compose %u\n",
            context.status, context.compose );
        status = set_surface_state( hwnd, barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN,
                                    0, &sealed );
        ok( !status && sealed.pending == 1 && (sealed.scene_generation & 1),
            "writer-exit barrier missed lease: status %#x pending %u scene %s\n",
            status, sealed.pending, wine_dbgstr_longlong( sealed.scene_generation ) );
        SetEvent( context.release );
        ok( WaitForSingleObject( thread, 10000 ) == WAIT_OBJECT_0,
            "writer-exit thread did not terminate\n" );
        status = set_surface_state( hwnd, barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_BEGIN,
                                    0, &sealed );
        ok( !status && !sealed.pending && (sealed.scene_generation & 1),
            "dead writer retained lease: status %#x pending %u scene %s\n",
            status, sealed.pending, wine_dbgstr_longlong( sealed.scene_generation ) );
        status = set_surface_state( hwnd, barrier, CLIENT_SURFACE_STATE_NATIVE_BARRIER_END,
                                    0, &sealed );
        ok( !status && !(sealed.scene_generation & 1),
            "writer-exit barrier did not reopen scene: status %#x scene %s\n",
            status, wine_dbgstr_longlong( sealed.scene_generation ) );
    }
    else
    {
        ok( 0, "writer-exit thread did not acquire lease\n" );
        if (context.release) SetEvent( context.release );
        if (thread) WaitForSingleObject( thread, 10000 );
    }
    if (thread) CloseHandle( thread );
    if (context.release) CloseHandle( context.release );
    if (context.ready) CloseHandle( context.ready );

    set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    DestroyWindow( hwnd );
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

static void owner_exit_child( HWND hwnd, HANDLE ready, HANDLE release, BOOL create_queue )
{
    MSG message;
    unsigned int i, status;

    for (i = 0; i < OWNER_SURFACES; ++i)
    {
        status = set_surface_state( hwnd, 0x30000000 + i,
                                    CLIENT_SURFACE_STATE_REGISTER | CLIENT_SURFACE_STATE_CACHE,
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
            ok( !state.staged && !state.pending,
                "stalled owner blocked publication deadline: staged %u pending %u\n",
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
        {"subtree-retirement", "client surface subtree retirement",
         test_subtree_generation_retirement},
        {"generation-aba", "client surface generation ABA exclusion", test_generation_aba},
        {"publish-transaction", "client surface host publication transaction",
         test_publish_transaction},
        {"live-prepare", "live client surface prepare transaction",
         test_live_prepare_transaction},
        {"unbacked-live", "unbacked live client surface publication",
         test_unbacked_live_generation},
        {"scene-writer-barrier", "client surface scene writer barrier",
         test_scene_writer_barrier},
        {"backend-capability", "client surface backend capability isolation",
         test_backend_capability_isolation},
        {"native-backing-barrier", "native backing destruction barrier",
         test_native_backing_barrier},
        {"reparent-writer-leases", "writer lease cleanup across reparenting", test_reparent_writer_leases},
        {"demoted-native-barrier", "native backing barrier after demotion", test_demoted_native_barrier},
        {"lease-storage", "scoped client surface lease storage", test_lease_storage},
        {"lease-root-barriers", "shared writer barriers on exact native roots", test_lease_root_barriers},
        {"lease-writer-thread-exit", "shared writer sealing on thread exit", test_lease_writer_thread_exit},
        {"notification-filter", "client surface notification filter bypass",
         test_notification_identity_aba},
        {"writer-thread-exit", "writer thread exit lease cleanup", test_writer_thread_exit},
        {"late-present-cutover", "late client surface publication cut-over",
         test_late_present_cutover},
        {"concurrent-state", "concurrent client surface state changes",
         test_concurrent_state_changes},
        {"hidden-present-resize", "hidden present and resize", test_hidden_present_resize},
        {"grow64-completion", "64x64 grow presentation completion",
         test_grow64_present_completion},
        {"paced-completion", "paced hidden client surface presents",
         test_paced_present_completion},
        {"present-destroy-race", "present and window destruction race",
         test_present_destroy_race},
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
    if (!strcmp( name, "lease-storage-process-exit" ))
    {
        test_lease_storage_process_exit( argv, FALSE );
        return TRUE;
    }
    if (!strcmp( name, "lease-storage-claimed-process-exit" ))
    {
        test_lease_storage_process_exit( argv, TRUE );
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

    if (argc > 5 && !strcmp( argv[2], "lease_storage_child" ))
    {
        HANDLE ready, release;
        sscanf( argv[3], "%p", &hwnd );
        sscanf( argv[4], "%p", &ready );
        sscanf( argv[5], "%p", &release );
        lease_storage_child( hwnd, ready, release, argc > 6 && atoi( argv[6] ) );
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
        owner_exit_child( hwnd, ready, release, strcmp( argv[2], "owner_no_queue" ) );
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
    trace( "testing scoped client surface lease storage\n" );
    test_lease_storage();
    test_lease_storage_process_exit( argv, FALSE );
    test_lease_storage_process_exit( argv, TRUE );
    test_lease_root_barriers();
    test_lease_writer_thread_exit();
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
    trace( "testing client surface backend capability isolation\n" );
    test_backend_capability_isolation();
    trace( "testing client surface scene writer barrier\n" );
    test_scene_writer_barrier();
    trace( "testing native backing destruction barrier\n" );
    test_native_backing_barrier();
    trace( "testing writer lease cleanup across reparenting\n" );
    test_reparent_writer_leases();
    trace( "testing native backing barrier after demotion\n" );
    test_demoted_native_barrier();
    trace( "testing client surface notification identity ABA\n" );
    test_notification_identity_aba();
    trace( "testing writer thread exit lease cleanup\n" );
    test_writer_thread_exit();
    trace( "testing late client surface publication cut-over\n" );
    test_late_present_cutover();
    trace( "testing client surface generation membership\n" );
    test_generation_membership();
    trace( "testing client surface clip scene snapshots\n" );
    test_clip_scene_snapshot();
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
    trace( "testing 64x64 grow presentation completion\n" );
    test_grow64_present_completion();
}
