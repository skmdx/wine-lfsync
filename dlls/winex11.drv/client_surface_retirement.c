/*
 * X11 client source retirement
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
#include <unistd.h>
#include <sys/socket.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

#include "client_surface.h"
#include "wine/server.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);
WINE_DECLARE_DEBUG_CHANNEL(csperf);

struct x11drv_client_surface_retirement
{
    struct list entry;
    void *view;
    struct client_surface_handoff_channel *channel;
    UINT64 identity, cookie;
    int ready_fd;
    struct x11drv_client_source_frame sources[CLIENT_SURFACE_SOURCE_FRAME_COUNT];
};

static void trace_source_retirement( const char *event, HWND hwnd,
                                     const struct x11drv_client_surface_retirement *retirement )
{
    LARGE_INTEGER ticks;
    unsigned long tid = 0;

    if (!TRACE_ON(csperf) || !retirement->identity || !retirement->cookie) return;
#ifdef __linux__
    tid = syscall( SYS_gettid );
#endif
    NtQueryPerformanceCounter( &ticks, NULL );
    TRACE_(csperf)( "ticks=%llu event=%s hwnd=%p retirement=%p identity=%s cookie=%s "
                   "native_pid=%lu native_tid=%lu\n", (unsigned long long)ticks.QuadPart,
                   event, hwnd, retirement, wine_dbgstr_longlong( retirement->identity ),
                   wine_dbgstr_longlong( retirement->cookie ), (unsigned long)getpid(), tid );
}

#define MAX_SOURCE_RETIREMENTS 1024
static pthread_mutex_t retirement_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t retirement_cond;
static pthread_once_t retirement_cond_once = PTHREAD_ONCE_INIT;
static int retirement_cond_status;
static struct list retirements = LIST_INIT( retirements );
static struct list retired_resources = LIST_INIT( retired_resources );
static unsigned int retirement_count;
static BOOL retirement_started;

static void init_retirement_cond(void)
{
    retirement_cond_status = client_surface_cond_init( &retirement_cond );
}

static void release_source_retirement( struct x11drv_client_surface_retirement *retirement )
{
    unsigned int i;

    assert( !retirement->view );
    for (i = 0; i < ARRAY_SIZE(retirement->sources); ++i)
    {
        struct x11drv_client_source_frame *frame = retirement->sources + i;
        Pixmap pixmap = frame->pixmap;

        x11drv_client_surface_release_source_frame( frame );
        if (pixmap)
            TRACE( "released retired source reference %#lx identity %s cookie %s\n", pixmap,
                   wine_dbgstr_longlong( retirement->identity ), wine_dbgstr_longlong( retirement->cookie ) );
    }
    /* These source references are released. A shared snapshot's last release
     * queues its independently charged storage until native destruction; this
     * mapping receipt does not assert native or physical image destruction. */
    trace_source_retirement( "source_retire_release", 0, retirement );
    pthread_mutex_lock( &retirement_lock );
    --retirement_count;
    pthread_mutex_unlock( &retirement_lock );
    client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING, sizeof(*retirement) );
    free( retirement );
}

static void wake_retiring_source_owner( struct x11drv_client_surface_retirement *retirement )
{
    struct client_surface_handoff_shared *shared = retirement->view;
    UINT64 value = 1;
    int ret;

    if (!__atomic_exchange_n( &shared->ready_parked, 0, __ATOMIC_ACQ_REL )) return;
    __atomic_add_fetch( &shared->ready_sequence, 1, __ATOMIC_RELEASE );
    do
#ifdef __linux__
        ret = write( retirement->ready_fd, &value, sizeof(value) );
#else
        ret = send( retirement->ready_fd, &value, sizeof(value), 0 );
#endif
    while (ret < 0 && errno == EINTR);
}

static BOOL retire_source_mapping( struct x11drv_client_surface_retirement *retirement )
{
    struct client_surface_handoff_shared *shared = retirement->view;
    struct client_surface_handoff_channel *channel = retirement->channel;
    unsigned int index = channel - shared->channels;
    BOOL ready;

    /* Closing prevents new reads without pretending that a checked X11 copy
     * has finished. Only the consumer advances its sequence or releases its
     * endpoint after all native reads have completed. */
    if (!__atomic_exchange_n( &channel->closed, 1, __ATOMIC_ACQ_REL ))
    {
        __atomic_fetch_or( &shared->ready_bitmap[index / 64], (UINT64)1 << (index % 64), __ATOMIC_RELEASE );
        wake_retiring_source_owner( retirement );
    }
    ready = !(__atomic_load_n( &channel->endpoints, __ATOMIC_ACQUIRE ) &
              CLIENT_SURFACE_HANDOFF_ENDPOINT_CONSUMER) ||
            __atomic_load_n( &channel->consumer_sequence, __ATOMIC_ACQUIRE ) ==
            __atomic_load_n( &channel->producer_sequence, __ATOMIC_ACQUIRE );
    if (!ready) return FALSE;

    TRACE( "releasing source mapping identity %s cookie %s after readers completed, view %p fd %d\n",
           wine_dbgstr_longlong( retirement->identity ), wine_dbgstr_longlong( retirement->cookie ),
           retirement->view, retirement->ready_fd );
    SERVER_START_REQ( release_client_surface_handoff )
    {
        req->handle = 0;
        req->producer = 0;
        req->surface = retirement->identity;
        req->cookie = retirement->cookie;
        req->owner = 0;
        wine_server_call( req );
    }
    SERVER_END_REQ;
    NtUnmapViewOfSection( NtCurrentProcess(), retirement->view );
    close( retirement->ready_fd );
    retirement->view = NULL;
    return TRUE;
}

static void source_retirement_thread( void *context )
{
    LARGE_INTEGER timeout = {0};
    struct list pending = LIST_INIT( pending );
    struct list resources = LIST_INIT( resources );
    struct x11drv_client_surface_retirement *retirement, *next;
    struct x11drv_client_surface_retired_resource *resource, *next_resource;

    for (;;)
    {
        pthread_mutex_lock( &retirement_lock );
        while (list_empty( &retirements ) && list_empty( &retired_resources ))
            pthread_cond_wait( &retirement_cond, &retirement_lock );
        list_move_tail( &pending, &retirements );
        list_move_tail( &resources, &retired_resources );
        pthread_mutex_unlock( &retirement_lock );

        LIST_FOR_EACH_ENTRY_SAFE( resource, next_resource, &resources, struct x11drv_client_surface_retired_resource, entry )
        {
            /* Native image destruction belongs to its connection's admitted
             * worker. Its handle, capacity and metadata outlive actual exit. */
            if (NtWaitForSingleObject( resource->thread, FALSE, &timeout )) continue;
            NtClose( resource->thread );
            list_remove( &resource->entry );
            resource->release( resource );
        }
        LIST_FOR_EACH_ENTRY_SAFE( retirement, next, &pending, struct x11drv_client_surface_retirement, entry )
        {
            if (!retire_source_mapping( retirement )) continue;
            list_remove( &retirement->entry );
            release_source_retirement( retirement );
        }
        pthread_mutex_lock( &retirement_lock );
        if (!list_empty( &pending ) || !list_empty( &resources ))
        {
            BOOL wait = list_empty( &retirements ) && list_empty( &retired_resources );

            list_move_tail( &retirements, &pending );
            list_move_tail( &retired_resources, &resources );
            if (wait) client_surface_cond_timedwait( &retirement_cond, &retirement_lock, 10 );
        }
        pthread_mutex_unlock( &retirement_lock );
    }
}

void x11drv_client_surface_retire_resource( struct x11drv_client_surface_retired_resource *resource )
{
    pthread_mutex_lock( &retirement_lock );
    assert( retirement_started && resource->thread );
    list_add_tail( &retired_resources, &resource->entry );
    pthread_cond_signal( &retirement_cond );
    pthread_mutex_unlock( &retirement_lock );
}

BOOL x11drv_client_surface_prepare_resource_retirement(void)
{
    HANDLE thread;
    NTSTATUS status = 0;

    pthread_once( &retirement_cond_once, init_retirement_cond );
    if (retirement_cond_status) return FALSE;
    pthread_mutex_lock( &retirement_lock );
    if (!retirement_started)
    {
        status = PsCreateSystemThread( &thread, THREAD_ALL_ACCESS, NULL, 0, NULL, source_retirement_thread, NULL );
        if (!status)
        {
            NtClose( thread );
            retirement_started = TRUE;
        }
    }
    pthread_mutex_unlock( &retirement_lock );
    return !status;
}

BOOL x11drv_client_surface_prepare_retirement( struct x11drv_client_surface *surface )
{
    struct x11drv_client_surface_retirement *retirement;

    if (surface->handoff_retirement) return TRUE;
    if (!x11drv_client_surface_prepare_resource_retirement()) return FALSE;
    if (!client_surface_reserve_memory( CLIENT_SURFACE_MEMORY_STAGING, sizeof(*retirement) )) return FALSE;
    if (!(retirement = calloc( 1, sizeof(*retirement) ))) goto failed;
    pthread_mutex_lock( &retirement_lock );
    if (retirement_count == MAX_SOURCE_RETIREMENTS) goto failed_locked;
    ++retirement_count;
    pthread_mutex_unlock( &retirement_lock );
    surface->handoff_retirement = retirement;
    return TRUE;

failed_locked:
    pthread_mutex_unlock( &retirement_lock );
    free( retirement );
failed:
    client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING, sizeof(*retirement) );
    return FALSE;
}

void x11drv_client_surface_retire_handoff( struct client_surface *client,
                                         const struct client_surface_handoff_lease *lease )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    struct x11drv_client_surface_retirement *retirement = surface->handoff_retirement;
    unsigned int i;

    if (!retirement)
    {
        /* Preparation failed before any source could be submitted. */
        struct x11drv_client_surface_retirement empty =
        {
            .view = lease->view, .channel = lease->channel,
            .identity = lease->channel->identity, .cookie = lease->cookie,
            .ready_fd = lease->ready_fd,
        };
        BOOL ready = retire_source_mapping( &empty );
        assert( ready );
        return;
    }
    retirement->view = lease->view;
    retirement->channel = lease->channel;
    retirement->identity = lease->channel->identity;
    retirement->cookie = lease->cookie;
    retirement->ready_fd = lease->ready_fd;
    memcpy( retirement->sources, surface->sources, sizeof(surface->sources) );
    memset( surface->sources, 0, sizeof(surface->sources) );
    surface->handoff_retirement = NULL;
    TRACE( "retiring source mapping identity %s cookie %s view %p fd %d\n",
           wine_dbgstr_longlong( retirement->identity ), wine_dbgstr_longlong( retirement->cookie ),
           retirement->view, retirement->ready_fd );
    trace_source_retirement( "source_retire_begin", client->hwnd, retirement );
    for (i = 0; i < ARRAY_SIZE(retirement->sources); ++i)
    {
        if (retirement->sources[i].pixmap)
            TRACE( "retaining source pixmap %#lx identity %s cookie %s\n", retirement->sources[i].pixmap,
                   wine_dbgstr_longlong( retirement->identity ), wine_dbgstr_longlong( retirement->cookie ) );
    }
    pthread_mutex_lock( &retirement_lock );
    list_add_tail( &retirements, &retirement->entry );
    pthread_cond_signal( &retirement_cond );
    pthread_mutex_unlock( &retirement_lock );
}

void x11drv_client_surface_destroy_retirement( struct x11drv_client_surface *surface )
{
    x11drv_client_surface_set_gpu_snapshot( surface, 0 );
    if (surface->handoff_retirement) release_source_retirement( surface->handoff_retirement );
}
