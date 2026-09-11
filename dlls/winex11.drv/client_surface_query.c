/*
 * Independent native queries for X11 client surface owners
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
#include <fcntl.h>

#include "x11drv.h"
#include "client_surface_query.h"

WINE_DECLARE_DEBUG_CHANNEL(csperf);

/* Query storage is embedded in admitted bindings. Bound queued, running and
 * undelivered results together; a stopped native call never owns this mutex.
 * Workers and their private connections last for the process, like the owner
 * actor. Any free worker can query any target: these are not domain leases.
 * Four stopped calls exhaust native execution, but not owner control. */
#define CLIENT_SURFACE_QUERY_LIMIT 1024
static pthread_mutex_t query_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t query_cond = PTHREAD_COND_INITIALIZER;
static struct client_surface_geometry_query *query_head, **query_tail = &query_head;
static struct client_surface_geometry_query *completed_head, **completed_tail = &completed_head;
static unsigned int query_count, queued_count, running_count, worker_count;
static void (*query_wake)(void);

struct client_surface_query_worker
{
    Display *display;
    struct x11drv_error_handler errors;
    int error;
};

static struct client_surface_query_worker query_workers[4];

static unsigned long long query_time(void)
{
    LARGE_INTEGER counter;

    NtQueryPerformanceCounter( &counter, NULL );
    return counter.QuadPart;
}

static int query_error( Display *display, XErrorEvent *event, void *arg )
{
    struct client_surface_query_worker *worker = arg;

    worker->error = event->error_code;
    return TRUE;
}

static BOOL open_query_display( struct client_surface_query_worker *worker )
{
    if (worker->display) return TRUE;
    if (!(worker->display = XOpenDisplay( DisplayString( gdi_display ) ))) return FALSE;
    worker->errors.display = worker->display;
    worker->errors.callback = query_error;
    worker->errors.arg = worker;
    X11DRV_register_error_handler( &worker->errors );
    if (fcntl( ConnectionNumber( worker->display ), F_SETFD, FD_CLOEXEC ) == -1)
    {
        XCloseDisplay( worker->display );
        X11DRV_unregister_error_handler( &worker->errors );
        worker->display = NULL;
        return FALSE;
    }
    return TRUE;
}

static void execute_geometry_query( struct client_surface_query_worker *worker,
                                    struct client_surface_geometry_query *query )
{
    Window root;
    unsigned int border;
    int x, y;
    BOOL ret;

    query->success = FALSE;
    if (!open_query_display( worker )) return;
    /* The private error sink avoids X11DRV_expect_error's process-wide lock.
     * Only this worker uses the connection, including its final error drain. */
    XLockDisplay( worker->display );
    worker->error = 0;
    ret = XGetGeometry( worker->display, query->pixmap, &root, &x, &y,
                        &query->width, &query->height, &border, &query->depth );
    XSync( worker->display, False );
    query->success = ret && !worker->error && query->width >= query->min_width &&
                     query->height >= query->min_height;
    TRACE_(csperf)( "ticks=%llu event=source_geometry query=%p pixmap=%lx display=%p result=%u error=%d "
                   "geometry_calls=1 sync_calls=1 worker=%u\n", query_time(), query, query->pixmap,
                   worker->display, ret, worker->error, (unsigned int)(worker - query_workers) );
    XUnlockDisplay( worker->display );
}

static void client_surface_query_thread( void *context )
{
    struct client_surface_query_worker *worker = context;
    struct client_surface_geometry_query *query;
    void (*wake)(void);

    for (;;)
    {
        pthread_mutex_lock( &query_mutex );
        while (!(query = query_head)) pthread_cond_wait( &query_cond, &query_mutex );
        if (!(query_head = query->next)) query_tail = &query_head;
        --queued_count;
        ++running_count;
        pthread_mutex_unlock( &query_mutex );

        execute_geometry_query( worker, query );

        pthread_mutex_lock( &query_mutex );
        --running_count;
        query->next = NULL;
        *completed_tail = query;
        completed_tail = &query->next;
        wake = query_wake;
        pthread_mutex_unlock( &query_mutex );
        /* The actor can free the enclosing binding as soon as we unlock. */
        wake();
    }
}

enum client_surface_query_status client_surface_query_geometry(
    struct client_surface_geometry_query *query, void (*wake)(void) )
{
    enum client_surface_query_status result = CLIENT_SURFACE_QUERY_FULL;
    HANDLE thread;
    NTSTATUS status;

    pthread_mutex_lock( &query_mutex );
    assert( !query_wake || query_wake == wake );
    query_wake = wake;
    if (query_count == CLIENT_SURFACE_QUERY_LIMIT) goto done;
    if (queued_count + running_count >= worker_count && worker_count < ARRAY_SIZE(query_workers))
    {
        status = PsCreateSystemThread( &thread, THREAD_ALL_ACCESS, NULL, 0, NULL,
                                       client_surface_query_thread, &query_workers[worker_count] );
        if (!status)
        {
            ++worker_count;
            NtClose( thread );
        }
        else if (!worker_count)
        {
            result = CLIENT_SURFACE_QUERY_FAILED;
            goto done;
        }
    }
    query->next = NULL;
    *query_tail = query;
    query_tail = &query->next;
    ++query_count;
    ++queued_count;
    result = CLIENT_SURFACE_QUERY_ACCEPTED;
    pthread_cond_signal( &query_cond );
done:
    TRACE_(csperf)( "ticks=%llu event=source_query_admission query=%p pixmap=%lx status=%u count=%u queued=%u "
                   "running=%u workers=%u\n", query_time(), query, query->pixmap, result, query_count,
                   queued_count, running_count, worker_count );
    pthread_mutex_unlock( &query_mutex );
    return result;
}

BOOL client_surface_complete_queries( unsigned int budget )
{
    struct client_surface_geometry_query *query;
    BOOL progressed = FALSE;

    while (budget--)
    {
        pthread_mutex_lock( &query_mutex );
        if (!(query = completed_head))
        {
            pthread_mutex_unlock( &query_mutex );
            break;
        }
        if (!(completed_head = query->next)) completed_tail = &completed_head;
        assert( query_count );
        --query_count;
        TRACE_(csperf)( "ticks=%llu event=source_query_return query=%p pixmap=%lx count=%u queued=%u running=%u workers=%u\n",
                       query_time(), query, query->pixmap, query_count, queued_count, running_count, worker_count );
        pthread_mutex_unlock( &query_mutex );
        query->complete( query );
        progressed = TRUE;
    }
    return progressed;
}
