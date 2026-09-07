/*
 * Client surface presentation completion
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

#include <assert.h>
#include <time.h>

#include "ntstatus.h"
#include "client_surface.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(win);

struct client_surface_completion_job
{
    struct list entry;
    struct client_surface_frame present;
    SIZE expected_size;
    BOOL has_expected_size;
};

#define CLIENT_SURFACE_MAX_DEFERRED_PRESENTS 64
#define CLIENT_SURFACE_MAX_PROCESS_DEFERRED_PRESENTS 1024
#define CLIENT_SURFACE_COMPLETION_WORKER_IDLE_TIMEOUT_MS 100
#define CLIENT_SURFACE_COMPLETION_POLL_TIMEOUT_MS 10
static LONG client_surface_deferred_present_count;

static BOOL client_surface_reserve_completion_slot(void)
{
    LONG count = ReadAcquire( &client_surface_deferred_present_count );

    while (count < CLIENT_SURFACE_MAX_PROCESS_DEFERRED_PRESENTS)
    {
        LONG previous = InterlockedCompareExchange( &client_surface_deferred_present_count, count + 1, count );
        if (previous == count) return TRUE;
        count = previous;
    }
    /* A rejected reservation must not temporarily consume a slot: its
     * rollback could otherwise make another caller wait inline even after
     * a real completion has freed capacity. */
    return FALSE;
}

static struct client_surface_completion_result client_surface_poll_present_completion(
    struct client_surface *surface, const struct client_surface_frame *present, DWORD timeout )
{
    struct client_surface_completion_result result = client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
    struct client_surface_target target;

    client_surface_get_target( surface, &target );
    /* The owner may still be preparing a scene. That prevents publication,
     * but does not invalidate a completion for this unchanged native target. */
    if (target.valid && present->target_epoch == target.epoch)
    {
        if (present->completion.wait)
            result = present->completion.wait( present->completion.context, timeout );
        else
        {
            assert( present->completion.kind == CLIENT_SURFACE_COMPLETION_SHARED );
            assert( surface->backend->completion );
            result = surface->backend->completion->wait( surface, timeout );
        }
    }
    assert( result.worker != CLIENT_SURFACE_COMPLETION_WORKER_RETIRE ||
            result.status == CLIENT_SURFACE_COMPLETION_FAILED );
    TRACE( "event=completion_poll surface=%p serial=%s target=%s completion=%p capture=%p "
           "control=%s submission=%u pending=%u status=%u worker=%u timeout=%u\n",
           surface, wine_dbgstr_longlong( present->serial ), wine_dbgstr_longlong( present->target_epoch ),
           present->completion.context, present->capture.context, wine_dbgstr_longlong( present->handoff_control ),
           present->submission_time, (unsigned int)InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ),
           result.status, result.worker, timeout );
    return result;
}

struct client_surface_completion_result client_surface_wait_present_completion(
    struct client_surface *surface, const struct client_surface_frame *present, DWORD timeout )
{
    struct client_surface_completion_result result;
    DWORD start = NtGetTickCount(), elapsed = start - present->submission_time;
    DWORD remaining, poll_start, delay_ms = 1;
    LARGE_INTEGER delay;

    /* The caller's budget cannot extend the original submission deadline,
     * including when allocation or queue admission fell back to this stack. */
    timeout = min( timeout, elapsed < CLIENT_SURFACE_PRESENT_TIMEOUT ?
                           CLIENT_SURFACE_PRESENT_TIMEOUT - elapsed : 0 );
    /* Keep this frame's turn through PENDING. Allocation and queue-admission
     * fallbacks must not overtake it on a producer thread between short native
     * polls. The per-surface mutex also preserves native wait serialization;
     * no process-wide lock spans a wait. Recheck target and budget after it. */
    pthread_mutex_lock( &surface->completion_wait_lock );
    for (;;)
    {
        elapsed = NtGetTickCount() - start;
        remaining = elapsed < timeout ? timeout - elapsed : 0;
        poll_start = NtGetTickCount();
        result = client_surface_poll_present_completion( surface, present,
                    min( remaining, (DWORD)CLIENT_SURFACE_COMPLETION_POLL_TIMEOUT_MS ) );
        if (result.status != CLIENT_SURFACE_COMPLETION_PENDING) break;
        elapsed = NtGetTickCount() - start;
        if (elapsed >= timeout)
        {
            WARN( "timed out waiting for presentation completion for %s serial %s\n",
                  debugstr_client_surface( surface ), wine_dbgstr_longlong( present->serial ) );
            result = client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
            break;
        }
        /* GLX and EGL timestamp queries have no native timeout. Back off if
         * a poll returned immediately, without resetting the frame deadline.
         * A native fence wait already slept, so it needs no additional delay. */
        if (NtGetTickCount() == poll_start)
        {
            delay.QuadPart = -(LONGLONG)min( delay_ms, timeout - elapsed ) * 10000;
            NtDelayExecution( FALSE, &delay );
            delay_ms = min( delay_ms * 2, (DWORD)4 );
        }
    }
    pthread_mutex_unlock( &surface->completion_wait_lock );
    return result;
}

void client_surface_set_present_completion( struct client_surface_frame *present,
                                            client_surface_completion_wait_func wait,
                                            client_surface_completion_release_func release,
                                            void *context )
{
    assert( present->completion.kind != CLIENT_SURFACE_COMPLETION_NONE );
    assert( wait );
    present->completion.external_result = TRUE;
    present->completion.wait = wait;
    present->completion.release = release;
    present->completion.context = context;
}

static BOOL wait_for_completion_job_locked( struct client_surface *surface )
{
    struct timespec abstime;
    int ret = 0;

    if (clock_gettime( CLOCK_REALTIME, &abstime )) return FALSE;
    abstime.tv_nsec += CLIENT_SURFACE_COMPLETION_WORKER_IDLE_TIMEOUT_MS * 1000000;
    abstime.tv_sec += abstime.tv_nsec / 1000000000;
    abstime.tv_nsec %= 1000000000;

    while (list_empty( &surface->completion_queue ) && !ret)
        ret = pthread_cond_timedwait( &surface->completion_queue_cond,
                                     &surface->completion_lock, &abstime );
    return !list_empty( &surface->completion_queue );
}

static enum client_surface_completion_worker_disposition complete_deferred_present(
    struct client_surface *surface, struct client_surface_frame *present,
    const SIZE *expected_size, BOOL poll )
{
    struct client_surface_completion_result result = client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
    struct client_surface_completion completion = present->completion;
    struct client_surface_capture capture = present->capture;
    DWORD elapsed = NtGetTickCount() - present->submission_time;
    DWORD remaining = elapsed < CLIENT_SURFACE_PRESENT_TIMEOUT ? CLIENT_SURFACE_PRESENT_TIMEOUT - elapsed : 0;

    /* A detached or retargeted surface cannot consume this completion. The
     * capture still owns its storage, and the native image keeps its fence
     * until GPU work retires even if we skip the wait or it fails. */
    if (poll) result = client_surface_wait_present_completion( surface, present, remaining );
    else TRACE( "cancelling completion without polling %s serial %s\n",
                debugstr_client_surface( surface ), wine_dbgstr_longlong( present->serial ) );
    TRACE( "event=completion_finish surface=%p serial=%s status=%u worker=%u polled=%u\n",
           surface, wine_dbgstr_longlong( present->serial ), result.status, result.worker, poll );
    if (!client_surface_complete_present( surface, present, TRUE,
             result.status == CLIENT_SURFACE_COMPLETION_SIGNALED, expected_size, 0 ) &&
        present->result == CLIENT_SURFACE_FRAME_PENDING)
        WARN( "deferred client-surface composition did not complete for %s\n",
              debugstr_client_surface( surface ) );
    /* Release outside the surface locks. Capture can hold the last reservation
     * on the native image; return the fence reference before that reservation. */
    completion.release( completion.context );
    if (capture.release) capture.release( capture.context );
    TRACE( "event=completion_release surface=%p serial=%s completion=%p capture=%p\n",
           surface, wine_dbgstr_longlong( present->serial ), completion.context, capture.context );
    return result.worker;
}

static BOOL start_client_surface_completion_thread( struct client_surface *surface );

static void client_surface_completion_worker( struct client_surface *surface, BOOL linger )
{
    enum client_surface_completion_worker_disposition disposition;
    struct client_surface_completion_job *job;
    BOOL poll = TRUE;

    for (;;)
    {
        pthread_mutex_lock( &surface->completion_lock );
        /* Keep the surface-local FIFO and its original concurrency bound.
         * A callback can separately disqualify this thread from reuse. */
        if (list_empty( &surface->completion_queue ) &&
            (!linger || !wait_for_completion_job_locked( surface )))
        {
            surface->completion_worker_active = FALSE;
            pthread_mutex_unlock( &surface->completion_lock );
            client_surface_release( surface );
            return;
        }
        job = LIST_ENTRY( list_head( &surface->completion_queue ),
                          struct client_surface_completion_job, entry );
        list_remove( &job->entry );
        pthread_mutex_unlock( &surface->completion_lock );

        disposition = complete_deferred_present( surface, &job->present,
                          job->has_expected_size ? &job->expected_size : NULL, poll );
        InterlockedDecrement( &client_surface_deferred_present_count );
        free( job );

        if (disposition == CLIENT_SURFACE_COMPLETION_WORKER_RETIRE)
        {
            TRACE( "event=completion_retire surface=%p\n", surface );
            /* Transfer the active FIFO and its surface reference intact. An
             * inline caller returns normally; only our system thread exits.
             * Never use the usual inline wait fallback on an unclean worker. */
            if (start_client_surface_completion_thread( surface )) return;
            WARN( "cancelling remaining completion FIFO for %s after worker retirement\n",
                  debugstr_client_surface( surface ) );
            /* A failed replacement must not strand jobs waiting for another
             * producer. Drain failures with no wait or capture, preserving
             * the fence-reference then capture-reservation release order. */
            poll = FALSE;
            linger = FALSE;
        }
    }
}

/* This entry point belongs to the Unix library and must run as host code.
 * PsCreateSystemThread invokes it directly on the Unix side, including when
 * the Windows process machine differs from the host machine. */
static void client_surface_completion_thread( void *context )
{
    client_surface_completion_worker( context, TRUE );
}

static BOOL start_client_surface_completion_thread( struct client_surface *surface )
{
    HANDLE thread;
    NTSTATUS status;

    status = PsCreateSystemThread( &thread, THREAD_ALL_ACCESS, NULL, 0, NULL,
                                  client_surface_completion_thread, surface );
    if (status)
    {
        WARN( "Failed to create client-surface completion worker, status %#lx\n", (unsigned long)status );
        return FALSE;
    }
    NtClose( thread );
    return TRUE;
}

void client_surface_defer_present( struct client_surface *surface,
                                   struct client_surface_frame *present,
                                   const SIZE *expected_size )
{
    struct client_surface_completion completion = present->completion;
    struct client_surface_completion_job *job;
    BOOL start_worker = FALSE;

    assert( completion.kind != CLIENT_SURFACE_COMPLETION_NONE );
    assert( completion.external_result );
    assert( present->serial );
    assert( InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ) > 0 );
    assert( completion.wait && completion.release );

    if (!(job = malloc( sizeof(*job) )))
    {
        complete_deferred_present( surface, present, expected_size, TRUE );
        return;
    }
    if (!client_surface_reserve_completion_slot())
    {
        free( job );
        complete_deferred_present( surface, present, expected_size, TRUE );
        return;
    }
    job->has_expected_size = !!expected_size;
    if (expected_size) job->expected_size = *expected_size;

    job->present = *present;
    pthread_mutex_lock( &surface->completion_lock );
    if (InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ) >=
        CLIENT_SURFACE_MAX_DEFERRED_PRESENTS)
    {
        pthread_mutex_unlock( &surface->completion_lock );
        complete_deferred_present( surface, present, expected_size, TRUE );
        InterlockedDecrement( &client_surface_deferred_present_count );
        free( job );
        return;
    }
    list_add_tail( &surface->completion_queue, &job->entry );
    TRACE( "event=completion_enqueue surface=%p serial=%s completion=%p capture=%p\n",
           surface, wine_dbgstr_longlong( present->serial ), completion.context, present->capture.context );
    if (!surface->completion_worker_active)
    {
        surface->completion_worker_active = TRUE;
        client_surface_add_ref( surface );
        start_worker = TRUE;
    }
    /* Only this surface's worker waits on the queue condition.  Submission
     * readiness notifications must neither steal its enqueue signal nor
     * wake it repeatedly before there is a job to consume. */
    else pthread_cond_signal( &surface->completion_queue_cond );
    pthread_mutex_unlock( &surface->completion_lock );

    if (start_worker && !start_client_surface_completion_thread( surface ))
    {
        /* Thread allocation failure is rare. Drain this FIFO inline so every
         * external completion token still has exactly one owner. */
        client_surface_completion_worker( surface, FALSE );
    }
    memset( present, 0, sizeof(*present) );
}
