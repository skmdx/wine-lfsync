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

BOOL client_surface_wait_present_completion( struct client_surface *surface,
                                             const struct client_surface_frame *present,
                                             DWORD timeout )
{
    struct client_surface_target target;
    DWORD elapsed, start = NtGetTickCount();
    BOOL completed;

    assert( present->completion.wait );
    pthread_mutex_lock( &surface->completion_wait_lock );
    /* Queue saturation and allocation failure execute the wait on the producer
     * thread.  Serialize that rare fallback with the surface worker: some host
     * APIs, notably vkWaitForPresentKHR, require external synchronization for
     * concurrent access to the same presentation object.  Recheck the target
     * after acquiring the lock so stale inline work does not wait five seconds
     * behind a completion which already observed the detach. */
    elapsed = NtGetTickCount() - start;
    timeout = elapsed < timeout ? timeout - elapsed : 0;
    client_surface_get_target( surface, &target );
    /* The owner may still be preparing a scene. That prevents publication,
     * but does not invalidate a completion for this unchanged native target. */
    completed = target.valid &&
                present->target_seq == target.seq &&
                present->completion.wait( present->completion.context, timeout );
    pthread_mutex_unlock( &surface->completion_wait_lock );
    return completed;
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

static void complete_deferred_present( struct client_surface *surface,
                                      struct client_surface_frame *present, const SIZE *expected_size )
{
    struct client_surface_completion completion = present->completion;
    struct client_surface_capture capture = present->capture;
    DWORD elapsed = NtGetTickCount() - present->submission_time;
    DWORD remaining = elapsed < CLIENT_SURFACE_PRESENT_TIMEOUT ? CLIENT_SURFACE_PRESENT_TIMEOUT - elapsed : 0;
    BOOL completed;

    /* A detached or retargeted surface cannot consume this completion. The
     * capture still owns its storage, and the native image keeps its fence
     * until GPU work retires even if we skip the wait or it fails. */
    completed = client_surface_wait_present_completion( surface, present, remaining );
    if (!client_surface_complete_present( surface, present, TRUE, completed, expected_size, 0 ) &&
        present->result == CLIENT_SURFACE_FRAME_PENDING)
        WARN( "deferred client-surface composition did not complete for %s\n",
              debugstr_client_surface( surface ) );
    /* Release outside the surface locks. Capture can hold the last reservation
     * on the native image; return the fence reference before that reservation. */
    completion.release( completion.context );
    if (capture.release) capture.release( capture.context );
}

static void client_surface_completion_worker( struct client_surface *surface, BOOL linger )
{
    struct client_surface_completion_job *job;

    for (;;)
    {
        pthread_mutex_lock( &surface->completion_lock );
        /* Backend completion callbacks may depend on per-thread state.  Reuse
         * this worker only for its own surface and only across short frame
         * gaps, preserving both affinity and the original concurrency bound. */
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

        complete_deferred_present( surface, &job->present, job->has_expected_size ? &job->expected_size : NULL );
        InterlockedDecrement( &client_surface_deferred_present_count );
        free( job );
    }
}

/* This entry point belongs to the Unix library and must run as host code.
 * PsCreateSystemThread invokes it directly on the Unix side, including when
 * the Windows process machine differs from the host machine. */
static void client_surface_completion_thread( void *context )
{
    client_surface_completion_worker( context, TRUE );
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
        complete_deferred_present( surface, present, expected_size );
        return;
    }
    if (!client_surface_reserve_completion_slot())
    {
        free( job );
        complete_deferred_present( surface, present, expected_size );
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
        complete_deferred_present( surface, present, expected_size );
        InterlockedDecrement( &client_surface_deferred_present_count );
        free( job );
        return;
    }
    list_add_tail( &surface->completion_queue, &job->entry );
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

    if (start_worker)
    {
        HANDLE thread;
        NTSTATUS status;

        status = PsCreateSystemThread( &thread, THREAD_ALL_ACCESS, NULL, 0, NULL,
                                       client_surface_completion_thread, surface );
        if (status)
        {
            /* Thread allocation failure is rare.  Drain this FIFO inline so
             * every external completion token still has exactly one owner. */
            WARN( "Failed to create client-surface completion worker, status %#lx\n",
                  (unsigned long)status );
            client_surface_completion_worker( surface, FALSE );
        }
        else NtClose( thread );
    }
    memset( present, 0, sizeof(*present) );
}
