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
    struct client_surface_present present;
    client_surface_completion_wait_func wait;
    client_surface_completion_release_func release;
    void *context;
    SIZE expected_size;
    DWORD submission_time;
    BOOL has_expected_size;
    BOOL submitted;
};

#define CLIENT_SURFACE_MAX_DEFERRED_PRESENTS 64
#define CLIENT_SURFACE_MAX_PROCESS_DEFERRED_PRESENTS 1024
#define CLIENT_SURFACE_COMPLETION_WORKER_IDLE_TIMEOUT_MS 100
static LONG client_surface_deferred_present_count;

void client_surface_begin_inline_completion( struct client_surface *surface,
                                             struct client_surface_present *present )
{
    /* Inline fallback still owns a causal completion boundary.  Publish that
     * ownership under completion_lock so cached replay and a shared driver
     * monitor cannot pass it merely because no FIFO node was allocated. */
    assert( present->external_completion );
    if (!present->completion_locked)
    {
        client_surface_lock_present( surface );
        present->completion_locked = TRUE;
    }
    client_surface_register_external_completion_locked( surface, present );
    present->completion_locked = FALSE;
    client_surface_unlock_present( surface );
}

BOOL client_surface_wait_present_completion( struct client_surface *surface,
                                             const struct client_surface_present *present,
                                             BOOL submitted,
                                             client_surface_completion_wait_func wait,
                                             void *context, DWORD timeout )
{
    DWORD elapsed, start = NtGetTickCount();
    BOOL completed;

    assert( wait );
    pthread_mutex_lock( &surface->completion_wait_lock );
    /* Queue saturation and allocation failure execute the wait on the producer
     * thread.  Serialize that rare fallback with the surface worker: some host
     * APIs, notably vkWaitForPresentKHR, require external synchronization for
     * concurrent access to the same presentation object.  Recheck the target
     * after acquiring the lock so stale inline work does not wait five seconds
     * behind a completion which already observed the detach. */
    elapsed = NtGetTickCount() - start;
    timeout = elapsed < timeout ? timeout - elapsed : 0;
    completed = submitted && present->target_ready &&
                present->target_epoch == ReadAcquire64( &surface->target_epoch ) &&
                present->lifecycle_seq == ReadAcquire( &surface->lifecycle_seq ) &&
                wait( context, timeout );
    pthread_mutex_unlock( &surface->completion_wait_lock );
    return completed;
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
                                     &surface->completion_queue_lock, &abstime );
    return !list_empty( &surface->completion_queue );
}

static void client_surface_completion_worker( struct client_surface *surface, BOOL linger )
{
    struct client_surface_completion_job *job;

    for (;;)
    {
        DWORD elapsed, remaining;
        BOOL completed;

        pthread_mutex_lock( &surface->completion_queue_lock );
        /* Backend completion callbacks may depend on per-thread state.  Reuse
         * this worker only for its own surface and only across short frame
         * gaps, preserving both affinity and the original concurrency bound. */
        if (list_empty( &surface->completion_queue ) &&
            (!linger || !wait_for_completion_job_locked( surface )))
        {
            surface->completion_worker_active = FALSE;
            pthread_mutex_unlock( &surface->completion_queue_lock );
            client_surface_release( surface );
            return;
        }
        job = LIST_ENTRY( list_head( &surface->completion_queue ),
                          struct client_surface_completion_job, entry );
        list_remove( &job->entry );
        pthread_mutex_unlock( &surface->completion_queue_lock );

        elapsed = NtGetTickCount() - job->submission_time;
        remaining = elapsed < CLIENT_SURFACE_PRESENT_TIMEOUT ?
                    CLIENT_SURFACE_PRESENT_TIMEOUT - elapsed : 0;
        /* A detached or retargeted surface cannot consume this completion.
         * Its job-owned references still protect the native source while the
         * queued request retires, but waiting for an unmapped drawable may
         * otherwise run to the full timeout for every queued frame. */
        completed = client_surface_wait_present_completion( surface, &job->present,
                                                            job->submitted, job->wait,
                                                            job->context, remaining );
        if (!client_surface_complete_present( surface, &job->present, job->submitted,
                                              completed,
                                              job->has_expected_size ? &job->expected_size : NULL,
                                              0 ) && job->submitted &&
            !job->present.superseded && !job->present.completion_failed)
            WARN( "deferred client-surface composition did not complete for %s\n",
                  debugstr_client_surface( surface ) );
        job->release( job->context );
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
                                   struct client_surface_present *present,
                                   BOOL submitted, const SIZE *expected_size,
                                   client_surface_completion_wait_func wait,
                                   client_surface_completion_release_func release,
                                   void *context )
{
    struct client_surface_completion_job *job;
    BOOL completed, start_worker = FALSE;
    DWORD elapsed, remaining;

    assert( present->external_completion );
    assert( wait && release );

    if (!(job = malloc( sizeof(*job) )))
    {
        client_surface_begin_inline_completion( surface, present );
        elapsed = NtGetTickCount() - present->submission_time;
        remaining = elapsed < CLIENT_SURFACE_PRESENT_TIMEOUT ?
                    CLIENT_SURFACE_PRESENT_TIMEOUT - elapsed : 0;
        completed = client_surface_wait_present_completion( surface, present, submitted,
                                                            wait, context, remaining );
        client_surface_complete_present( surface, present, submitted, completed,
                                         expected_size, 0 );
        release( context );
        return;
    }
    if (InterlockedIncrement( &client_surface_deferred_present_count ) >
        CLIENT_SURFACE_MAX_PROCESS_DEFERRED_PRESENTS)
    {
        InterlockedDecrement( &client_surface_deferred_present_count );
        free( job );
        client_surface_begin_inline_completion( surface, present );
        elapsed = NtGetTickCount() - present->submission_time;
        remaining = elapsed < CLIENT_SURFACE_PRESENT_TIMEOUT ?
                    CLIENT_SURFACE_PRESENT_TIMEOUT - elapsed : 0;
        completed = client_surface_wait_present_completion( surface, present, submitted,
                                                            wait, context, remaining );
        client_surface_complete_present( surface, present, submitted, completed,
                                         expected_size, 0 );
        release( context );
        return;
    }
    job->wait = wait;
    job->release = release;
    job->context = context;
    job->submission_time = present->submission_time;
    job->submitted = submitted;
    job->has_expected_size = !!expected_size;
    if (expected_size) job->expected_size = *expected_size;

    /* Register queue ownership while excluding cached replay.  A native
     * driver monitor may transfer an already-held completion lock; explicit
     * completion IDs acquire it only for this ownership hand-off. */
    if (!present->completion_locked)
    {
        client_surface_lock_present( surface );
        present->completion_locked = TRUE;
    }
    client_surface_register_external_completion_locked( surface, present );
    job->present = *present;
    pthread_mutex_lock( &surface->completion_queue_lock );
    if (InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ) >=
        CLIENT_SURFACE_MAX_DEFERRED_PRESENTS)
    {
        pthread_mutex_unlock( &surface->completion_queue_lock );
        client_surface_begin_inline_completion( surface, present );
        elapsed = NtGetTickCount() - present->submission_time;
        remaining = elapsed < CLIENT_SURFACE_PRESENT_TIMEOUT ?
                    CLIENT_SURFACE_PRESENT_TIMEOUT - elapsed : 0;
        completed = client_surface_wait_present_completion( surface, present, submitted,
                                                            wait, context, remaining );
        client_surface_complete_present( surface, present, submitted, completed,
                                         expected_size, 0 );
        release( context );
        InterlockedDecrement( &client_surface_deferred_present_count );
        free( job );
        return;
    }
    job->present.completion_locked = FALSE;
    list_add_tail( &surface->completion_queue, &job->entry );
    if (!surface->completion_worker_active)
    {
        surface->completion_worker_active = TRUE;
        client_surface_add_ref( surface );
        start_worker = TRUE;
    }
    else pthread_cond_signal( &surface->completion_queue_cond );
    pthread_mutex_unlock( &surface->completion_queue_lock );
    present->completion_locked = FALSE;
    client_surface_unlock_present( surface );

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
