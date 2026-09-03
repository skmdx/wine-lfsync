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
#include "wine/server.h"
#include "wine/test.h"
#include "wine/wgl.h"

#define RACE_THREADS 4
#define RACE_ROUNDS 2000
#define OWNER_SURFACES 128
#define STATUS_WINE_INVALID_WINDOW_HANDLE (0xc0010000u | ERROR_INVALID_WINDOW_HANDLE)

struct surface_state
{
    HWND toplevel;
    UINT64 generation;
    UINT pending;
    UINT staged;
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

static unsigned int set_surface_state( HWND hwnd, UINT_PTR surface, UINT flags,
                                       UINT64 generation, struct surface_state *state )
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
    status = p_wine_server_call( &info );
    if (!status && state)
    {
        state->toplevel = wine_server_ptr_handle( reply->toplevel );
        state->generation = reply->generation;
        state->pending = reply->pending;
        state->staged = reply->staged;
        state->active = reply->active;
        state->cached = reply->cached;
        state->wake = reply->wake;
    }
    return status;
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
    ok( staged.staged && staged.generation && staged.pending == 2,
        "unexpected staged state: staged %u generation %s pending %u\n",
        staged.staged, wine_dbgstr_longlong( staged.generation ), staged.pending );

    status = set_surface_state( hwnd, first_surface, CLIENT_SURFACE_STATE_PRESENT_COMMIT,
                                staged.generation, &committed );
    ok( !status, "membership commit failed, status %#x\n", status );
    ok( !committed.staged && !committed.pending && committed.wake,
        "same-window commit did not complete all surfaces: staged %u pending %u wake %u\n",
        committed.staged, committed.pending, committed.wake );

    status = set_surface_state( hwnd, first_surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, &state );
    ok( !status, "first unregister failed, status %#x\n", status );
    ok( state.active == 1 && state.cached == 1,
        "cached surface was not retained: active %u cached %u\n", state.active, state.cached );

    ShowWindow( hwnd, SW_HIDE );
    ShowWindow( hwnd, SW_SHOW );
    status = set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_STAGED, 0, &staged );
    ok( !status, "cached stage failed, status %#x\n", status );
    ok( staged.staged && staged.pending == 2,
        "active and cached surfaces not included once: staged %u pending %u\n",
        staged.staged, staged.pending );
    status = set_surface_state( hwnd, first_surface, CLIENT_SURFACE_STATE_UNCACHE, 0, &state );
    ok( !status, "uncache failed, status %#x\n", status );
    ok( state.active == 1 && !state.cached && state.pending == 1 && state.staged && !state.wake,
        "uncache did not retire one surface: active %u cached %u pending %u staged %u wake %u\n",
        state.active, state.cached, state.pending, state.staged, state.wake );
    status = set_surface_state( hwnd, first_surface,
                                CLIENT_SURFACE_STATE_UNREGISTER | CLIENT_SURFACE_STATE_UNCACHE,
                                0, &state );
    ok( !status, "duplicate removal failed, status %#x\n", status );
    ok( state.active == 1 && !state.cached && state.pending == 1 && state.staged,
        "duplicate removal underflowed state: active %u cached %u pending %u staged %u\n",
        state.active, state.cached, state.pending, state.staged );
    status = set_surface_state( hwnd, second_surface, CLIENT_SURFACE_STATE_PRESENT_COMMIT,
                                staged.generation, &state );
    ok( !status, "remaining commit failed, status %#x\n", status );
    ok( !state.pending && !state.staged && state.wake,
        "remaining commit did not publish: pending %u staged %u wake %u\n",
        state.pending, state.staged, state.wake );
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
    set_surface_state( second, second_surface, CLIENT_SURFACE_STATE_REGISTER, 0, NULL );
    set_surface_state( descendant, descendant_surface, CLIENT_SURFACE_STATE_REGISTER, 0, NULL );
    set_surface_state( second, duplicate_surface, CLIENT_SURFACE_STATE_CACHE, 0, NULL );

    status = get_clip_state( first, &before );
    ok( !status, "clip snapshot failed, status %#x\n", status );
    ok( before.toplevel == parent, "clip top %p, expected %p\n", before.toplevel, parent );
    ok( !(before.scene_generation & 1), "unstable scene generation %s\n",
        wine_dbgstr_longlong( before.scene_generation ) );
    ok( before.count == 2, "clip count %u, expected two distinct windows\n", before.count );
    ok( clip_state_contains( &before, second ), "upper sibling missing from clip snapshot\n" );
    ok( clip_state_contains( &before, descendant ), "descendant missing from clip snapshot\n" );

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
    status = set_surface_state( second, second_surface, CLIENT_SURFACE_STATE_REGISTER, 0, &state );
    ok( !status, "second child register failed, status %#x\n", status );
    ok( state.toplevel == parent && state.active == 1,
        "second child top %p active %u\n", state.toplevel, state.active );

    ShowWindow( parent, SW_SHOW );
    status = set_surface_state( parent, 0, CLIENT_SURFACE_STATE_STAGED, 0, &staged );
    ok( !status, "subtree stage failed, status %#x\n", status );
    ok( staged.staged && staged.pending == 2,
        "visible subtree pending count %u staged %u\n", staged.pending, staged.staged );
    status = set_surface_state( first, first_surface, CLIENT_SURFACE_STATE_PRESENT_COMMIT,
                                staged.generation, &partial );
    ok( !status, "first child commit failed, status %#x\n", status );
    ok( partial.staged && partial.pending == 1 && !partial.wake,
        "partial commit state: staged %u pending %u wake %u\n",
        partial.staged, partial.pending, partial.wake );
    status = set_surface_state( first, first_surface, CLIENT_SURFACE_STATE_PRESENT_COMMIT,
                                staged.generation, &partial );
    ok( !status, "duplicate child commit failed, status %#x\n", status );
    ok( partial.staged && partial.pending == 1 && !partial.wake,
        "duplicate commit changed pending state: staged %u pending %u wake %u\n",
        partial.staged, partial.pending, partial.wake );

    ShowWindow( second, SW_HIDE );
    status = set_surface_state( parent, 0, 0, 0, &state );
    ok( !status, "state query after child hide failed, status %#x\n", status );
    ok( !state.staged && !state.pending,
        "hidden subtree was not retired: staged %u pending %u\n", state.staged, state.pending );

    ShowWindow( parent, SW_HIDE );
    ShowWindow( parent, SW_SHOW );
    status = set_surface_state( parent, 0, CLIENT_SURFACE_STATE_STAGED, 0, &staged );
    ok( !status, "second subtree stage failed, status %#x\n", status );
    ok( staged.staged && staged.pending == 1,
        "hidden child joined generation: staged %u pending %u\n", staged.staged, staged.pending );
    ShowWindow( second, SW_SHOW );
    status = set_surface_state( second, second_surface, CLIENT_SURFACE_STATE_PRESENT_COMMIT,
                                staged.generation, &state );
    ok( !status, "late child commit failed, status %#x\n", status );
    ok( state.staged && state.pending == 1 && !state.wake,
        "late child changed existing wait: staged %u pending %u wake %u\n",
        state.staged, state.pending, state.wake );
    status = set_surface_state( first, first_surface, CLIENT_SURFACE_STATE_PRESENT_COMMIT,
                                staged.generation, &state );
    ok( !status, "remaining child commit failed, status %#x\n", status );
    ok( !state.staged && !state.pending && state.wake,
        "remaining child did not publish: staged %u pending %u wake %u\n",
        state.staged, state.pending, state.wake );

    ShowWindow( parent, SW_HIDE );
    ShowWindow( parent, SW_SHOW );
    status = set_surface_state( parent, 0, CLIENT_SURFACE_STATE_STAGED, 0, &staged );
    ok( !status, "destroy subtree stage failed, status %#x\n", status );
    ok( staged.staged && staged.pending == 2,
        "destroy subtree pending count %u staged %u\n", staged.pending, staged.staged );
    status = set_surface_state( first, first_surface, CLIENT_SURFACE_STATE_PRESENT_COMMIT,
                                staged.generation, &state );
    ok( !status, "pre-destroy child commit failed, status %#x\n", status );
    ok( state.staged && state.pending == 1 && !state.wake,
        "pre-destroy state: staged %u pending %u wake %u\n",
        state.staged, state.pending, state.wake );
    ok( DestroyWindow( second ), "failed to destroy pending child, error %lu\n", GetLastError() );
    status = set_surface_state( parent, 0, 0, 0, &state );
    ok( !status, "state query after child destroy failed, status %#x\n", status );
    ok( !state.staged && !state.pending,
        "destroyed subtree was not retired: staged %u pending %u\n", state.staged, state.pending );
    set_surface_state( first, first_surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, NULL );
    DestroyWindow( parent );
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
    ShowWindow( hwnd, SW_SHOW );

    status = set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_STAGED, 0, &first );
    ok( !status, "first stage failed, status %#x\n", status );
    ok( first.staged, "first generation was not staged\n" );
    ok( first.generation != 0, "first generation is zero\n" );
    ok( first.pending == 1, "first pending count %u\n", first.pending );

    status = set_surface_state( hwnd, 0, CLIENT_SURFACE_STATE_STAGED, 0, &second );
    ok( !status, "second stage failed, status %#x\n", status );
    ok( second.staged, "second generation was not staged\n" );
    ok( second.generation && second.generation != first.generation,
        "generation was reused, first %s second %s\n",
        wine_dbgstr_longlong( first.generation ), wine_dbgstr_longlong( second.generation ) );
    ok( second.pending == 1, "second pending count %u\n", second.pending );

    status = set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_PRESENT_COMMIT,
                                first.generation, &stale );
    ok( !status, "stale commit failed, status %#x\n", status );
    ok( stale.staged && stale.generation == second.generation && stale.pending == 1,
        "stale commit changed generation state: staged %u generation %s pending %u\n",
        stale.staged, wine_dbgstr_longlong( stale.generation ), stale.pending );
    ok( !stale.wake, "stale commit woke the top-level window\n" );

    status = set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_PRESENT_COMMIT,
                                second.generation, &current );
    ok( !status, "current commit failed, status %#x\n", status );
    ok( !current.staged && !current.pending,
        "current commit did not complete: staged %u pending %u\n",
        current.staged, current.pending );
    ok( current.wake, "current commit did not wake the top-level window\n" );

    status = set_surface_state( hwnd, surface, CLIENT_SURFACE_STATE_UNREGISTER, 0, &empty );
    ok( !status, "unregister failed, status %#x\n", status );
    ok( !empty.active && !empty.cached && !empty.pending,
        "surface state leaked: active %u cached %u pending %u\n",
        empty.active, empty.cached, empty.pending );
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
        UINT_PTR id = context->base + (i & 7);

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
        contexts[i].base = 0x20000000 + i * 0x100;
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
            ok( state.staged && state.pending == OWNER_SURFACES,
                "owner generation staged %u pending %u\n", state.staged, state.pending );
        else
            ok( !state.staged && !state.pending && state.wake,
                "queue-less owner blocked publication: staged %u pending %u wake %u\n",
                state.staged, state.pending, state.wake );
        if (!strcmp( mode, "owner_stalled" ))
        {
            Sleep( 6500 );
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
    pump_messages( 500 );
    set_surface_state( hwnd, 0, 0, 0, &state );
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
    pump_messages( 500 );
    set_surface_state( hwnd, 0, 0, 0, &state );
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
    WNDCLASSA class = {0};
    unsigned int i;

    class.style = CS_OWNDC;
    class.lpfnWndProc = client_surface_proc;
    class.hInstance = GetModuleHandleA( NULL );
    class.lpszClassName = "client_surface_present_race";
    if (!RegisterClassA( &class ) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        win_skip( "failed to register OpenGL race window, error %lu\n", GetLastError() );
        return;
    }

    for (i = 0; i < 10; ++i)
    {
        struct present_race_context context = {0};
        HANDLE thread;
        unsigned int j;

        context.hwnd = CreateWindowA( class.lpszClassName, "present race", WS_POPUP | WS_VISIBLE,
                                      20, 20, 160, 120, NULL, NULL, class.hInstance, NULL );
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

START_TEST(client_surface)
{
    HMODULE ntdll = GetModuleHandleA( "ntdll.dll" );
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

    GetDesktopWindow();
    trace( "testing client surface generations\n" );
    test_generation_aba();
    trace( "testing client surface generation membership\n" );
    test_generation_membership();
    trace( "testing client surface clip scene snapshots\n" );
    test_clip_scene_snapshot();
    trace( "testing client surface subtree retirement\n" );
    test_subtree_generation_retirement();
    trace( "testing concurrent client surface state changes\n" );
    test_concurrent_state_changes();
    trace( "testing owner exit and window destruction\n" );
    test_owner_exit_and_destroy( argv );
    trace( "testing present and window destruction race\n" );
    test_present_destroy_race();
    trace( "testing hidden present and resize\n" );
    test_hidden_present_resize();
}
