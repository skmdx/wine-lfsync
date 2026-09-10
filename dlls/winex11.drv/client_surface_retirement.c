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
#include <time.h>
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
    LONG refs;
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
static pthread_cond_t retirement_cond = PTHREAD_COND_INITIALIZER;
static struct list retirements = LIST_INIT( retirements );
static unsigned int retirement_count;
static BOOL retirement_started;

static void release_source_retirement( struct x11drv_client_surface_retirement *retirement )
{
    unsigned int i;

    if (InterlockedDecrement( &retirement->refs )) return;
    assert( !retirement->view );
    for (i = 0; i < ARRAY_SIZE(retirement->sources); ++i)
    {
        struct x11drv_client_source_frame *frame = retirement->sources + i;

        if (frame->image) frame->release_image( frame->image );
        if (frame->gc) XFreeGC( gdi_display, frame->gc );
        if (frame->pixmap) XFreePixmap( gdi_display, frame->pixmap );
        x11drv_client_surface_trace_image( "free", "producer_slot", gdi_display, frame->pixmap, frame->bytes );
        if (frame->pixmap)
            TRACE( "released retired source pixmap %#lx identity %s cookie %s\n", frame->pixmap,
                   wine_dbgstr_longlong( retirement->identity ), wine_dbgstr_longlong( retirement->cookie ) );
        client_surface_release_memory( CLIENT_SURFACE_MEMORY_SOURCE, frame->bytes );
    }
    XFlush( gdi_display );
    /* The native image release callbacks and XFreePixmap calls returned;
     * this does not assert that the X server or GPU freed physical memory. */
    trace_source_retirement( "source_retire_release", 0, retirement );
    pthread_mutex_lock( &retirement_lock );
    --retirement_count;
    pthread_mutex_unlock( &retirement_lock );
    client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING, sizeof(*retirement) );
    free( retirement );
}

void x11drv_client_surface_set_gpu_snapshot( struct x11drv_client_surface *surface, Pixmap pixmap )
{
    struct x11drv_client_surface_retirement *retirement = surface->snapshot_retirement;

    if (surface->gpu_snapshot == pixmap) return;
    surface->gpu_snapshot = pixmap;
    surface->snapshot_retirement = NULL;
    if (retirement) release_source_retirement( retirement );
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
    unsigned int i, index = channel - shared->channels;
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
    for (i = 0; i < ARRAY_SIZE(retirement->sources); ++i)
    {
        struct x11drv_client_source_frame *frame = retirement->sources + i;

        if (frame->image && frame->image_ready && !frame->image_ready( frame->image )) ready = FALSE;
    }
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
    struct list pending = LIST_INIT( pending );
    struct x11drv_client_surface_retirement *retirement, *next;

    for (;;)
    {
        pthread_mutex_lock( &retirement_lock );
        while (list_empty( &retirements )) pthread_cond_wait( &retirement_cond, &retirement_lock );
        list_move_tail( &pending, &retirements );
        pthread_mutex_unlock( &retirement_lock );

        LIST_FOR_EACH_ENTRY_SAFE( retirement, next, &pending, struct x11drv_client_surface_retirement, entry )
        {
            if (!retire_source_mapping( retirement )) continue;
            list_remove( &retirement->entry );
            release_source_retirement( retirement );
        }
        pthread_mutex_lock( &retirement_lock );
        if (!list_empty( &pending ))
        {
            struct timespec deadline;

            list_move_tail( &retirements, &pending );
            clock_gettime( CLOCK_REALTIME, &deadline );
            deadline.tv_nsec += 10000000;
            deadline.tv_sec += deadline.tv_nsec / 1000000000;
            deadline.tv_nsec %= 1000000000;
            pthread_cond_timedwait( &retirement_cond, &retirement_lock, &deadline );
        }
        pthread_mutex_unlock( &retirement_lock );
    }
}

BOOL x11drv_client_surface_prepare_retirement( struct x11drv_client_surface *surface )
{
    struct x11drv_client_surface_retirement *retirement;
    HANDLE thread;
    NTSTATUS status;

    if (surface->handoff_retirement) return TRUE;
    if (!client_surface_reserve_memory( CLIENT_SURFACE_MEMORY_STAGING, sizeof(*retirement) )) return FALSE;
    if (!(retirement = calloc( 1, sizeof(*retirement) ))) goto failed;
    pthread_mutex_lock( &retirement_lock );
    if (retirement_count == MAX_SOURCE_RETIREMENTS) goto failed_locked;
    /* Reserve the record and sole process worker before publishing any source.
     * Detach then needs no allocation and never frees a live reader on OOM. */
    if (!retirement_started)
    {
        status = PsCreateSystemThread( &thread, THREAD_ALL_ACCESS, NULL, 0, NULL, source_retirement_thread, NULL );
        if (status) goto failed_locked;
        NtClose( thread );
        retirement_started = TRUE;
    }
    ++retirement_count;
    pthread_mutex_unlock( &retirement_lock );
    retirement->refs = 1;
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
    for (i = 0; i < ARRAY_SIZE(retirement->sources); ++i)
        if (surface->gpu_snapshot && surface->gpu_snapshot == retirement->sources[i].pixmap)
        {
            assert( !surface->snapshot_retirement );
            InterlockedIncrement( &retirement->refs );
            surface->snapshot_retirement = retirement;
            break;
        }
    TRACE( "retiring source mapping identity %s cookie %s view %p fd %d\n",
           wine_dbgstr_longlong( retirement->identity ), wine_dbgstr_longlong( retirement->cookie ),
           retirement->view, retirement->ready_fd );
    trace_source_retirement( "source_retire_begin", client->hwnd, retirement );
    for (i = 0; i < ARRAY_SIZE(retirement->sources); ++i)
    {
        x11drv_client_surface_trace_image( "retire", "producer_slot", gdi_display,
                                          retirement->sources[i].pixmap, retirement->sources[i].bytes );
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
