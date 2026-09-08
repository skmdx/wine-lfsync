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
#ifdef __linux__
#include <unistd.h>
#include <sys/syscall.h>
#endif

#include "ntstatus.h"
#include "client_surface.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(win);
WINE_DECLARE_DEBUG_CHANNEL(csperf);

static unsigned long long client_surface_perf_time(void)
{
    LARGE_INTEGER counter;

    NtQueryPerformanceCounter( &counter, NULL );
    return counter.QuadPart;
}

static void trace_client_surface_worker( const char *event, unsigned int slot, BOOL retire )
{
#ifdef __linux__
    TRACE_(csperf)( "ticks=%llu event=%s slot=%u retire=%u native_pid=%lu native_tid=%lu\n",
                   client_surface_perf_time(), event, slot, retire,
                   (unsigned long)getpid(), (unsigned long)syscall( SYS_gettid ) );
#else
    TRACE_(csperf)( "ticks=%llu event=%s slot=%u retire=%u\n",
                   client_surface_perf_time(), event, slot, retire );
#endif
}

struct client_surface_completion_job
{
    struct list entry;
    struct client_surface_frame present;
    struct client_surface_completion_result result;
    SIZE expected_size;
    DWORD wait_started, wait_timeout, poll_due, poll_delay;
    BOOL has_expected_size, deferred, allocated, pending, done;
};

#define CLIENT_SURFACE_MAX_DEFERRED_PRESENTS 64
#define CLIENT_SURFACE_MAX_PROCESS_DEFERRED_PRESENTS 1024
#define CLIENT_SURFACE_COMPLETION_WORKER_IDLE_TIMEOUT_MS 100
#define CLIENT_SURFACE_COMPLETION_POLL_TIMEOUT_MS 10
#define CLIENT_SURFACE_COMPLETION_MAX_WORKERS 4
static LONG client_surface_deferred_present_count;

static pthread_mutex_t completion_executor_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t completion_executor_cond = PTHREAD_COND_INITIALIZER;
static struct list completion_ready_surfaces = LIST_INIT(completion_ready_surfaces);
static unsigned int completion_busy_workers;

struct client_surface_completion_worker
{
    HANDLE thread;
    enum
    {
        COMPLETION_WORKER_FREE,
        COMPLETION_WORKER_STARTING,
        COMPLETION_WORKER_RUNNING,
        COMPLETION_WORKER_EXITING,
        COMPLETION_WORKER_REAPING,
    } state;
};
static struct client_surface_completion_worker completion_workers[CLIENT_SURFACE_COMPLETION_MAX_WORKERS];

static BOOL client_surface_reserve_completion_slot(void)
{
    LONG count = ReadAcquire( &client_surface_deferred_present_count );

    while (count < CLIENT_SURFACE_MAX_PROCESS_DEFERRED_PRESENTS)
    {
        LONG previous = InterlockedCompareExchange( &client_surface_deferred_present_count, count + 1, count );
        if (previous == count) return TRUE;
        count = previous;
    }
    /* Rejected reservations never consume capacity, including while their
     * callers wait in the FIFO with stack-owned synchronous jobs. */
    return FALSE;
}

static struct client_surface_completion_result client_surface_poll_present_completion(
    struct client_surface *surface, const struct client_surface_frame *present, DWORD timeout )
{
    struct client_surface_completion_result result = client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
    struct client_surface_target target;
    unsigned long long begin = TRACE_ON(csperf) ? client_surface_perf_time() : 0;
    unsigned long long native = 0, end;

    client_surface_get_target( surface, &target );
    /* The owner may still be preparing a scene. That prevents publication,
     * but does not invalidate a completion for this unchanged native target. */
    if (target.valid && present->target_epoch == target.epoch)
    {
        native = begin ? client_surface_perf_time() : 0;
        if (present->completion.wait)
            result = present->completion.wait( present->completion.context, timeout );
        else
        {
            assert( present->completion.kind == CLIENT_SURFACE_COMPLETION_SHARED );
            assert( surface->backend->completion );
            result = surface->backend->completion->wait( surface, timeout );
        }
    }
    end = begin ? client_surface_perf_time() : 0;
    assert( result.worker != CLIENT_SURFACE_COMPLETION_WORKER_RETIRE ||
            result.status == CLIENT_SURFACE_COMPLETION_FAILED );
    TRACE( "event=completion_poll surface=%p serial=%s target=%s completion=%p capture=%p "
           "control=%s submission=%u pending=%u status=%u worker=%u timeout=%u\n",
           surface, wine_dbgstr_longlong( present->serial ), wine_dbgstr_longlong( present->target_epoch ),
           present->completion.context, present->capture.context, wine_dbgstr_longlong( present->handoff_control ),
           present->submission_time, (unsigned int)InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ),
           result.status, result.worker, timeout );
    TRACE_(csperf)( "ticks=%llu event=completion_poll identity=%s serial=%s control=%s target_epoch=%s "
                   "begin=%llu native=%llu status=%u worker=%u timeout=%u\n", end,
                   wine_dbgstr_longlong( client_surface_get_identity( surface ) ),
                   wine_dbgstr_longlong( present->serial ), wine_dbgstr_longlong( present->handoff_control ),
                   wine_dbgstr_longlong( present->target_epoch ), begin, native, result.status, result.worker, timeout );
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

static struct client_surface_completion_job *completion_head( struct client_surface *surface )
{
    return LIST_ENTRY( list_head( &surface->completion_queue ), struct client_surface_completion_job, entry );
}

static void queue_ready_surface_locked( struct client_surface *surface )
{
    assert( !surface->completion_in_progress );
    assert( list_empty( &surface->completion_ready_entry ) );
    if (!list_empty( &surface->completion_queue ))
        list_add_tail( &completion_ready_surfaces, &surface->completion_ready_entry );
    pthread_cond_broadcast( &completion_executor_cond );
}

static void wait_completion_executor_locked( DWORD timeout )
{
    struct timespec abstime;

    clock_gettime( CLOCK_REALTIME, &abstime );
    abstime.tv_nsec += timeout * 1000000;
    abstime.tv_sec += abstime.tv_nsec / 1000000000;
    abstime.tv_nsec %= 1000000000;
    pthread_cond_timedwait( &completion_executor_cond, &completion_executor_lock, &abstime );
}

static unsigned int active_completion_workers_locked(void)
{
    unsigned int i, count = 0;

    for (i = 0; i < ARRAY_SIZE(completion_workers); ++i)
        if (completion_workers[i].state == COMPLETION_WORKER_RUNNING) ++count;
    return count;
}

static unsigned int starting_completion_workers_locked(void)
{
    unsigned int i, count = 0;

    for (i = 0; i < ARRAY_SIZE(completion_workers); ++i)
        if (completion_workers[i].state == COMPLETION_WORKER_STARTING) ++count;
    return count;
}

static void reap_completion_workers(void)
{
    LARGE_INTEGER timeout = {0};
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(completion_workers); ++i)
    {
        struct client_surface_completion_worker *worker = &completion_workers[i];
        HANDLE thread = NULL;
        NTSTATUS status;

        pthread_mutex_lock( &completion_executor_lock );
        if (worker->state == COMPLETION_WORKER_EXITING && worker->thread)
        {
            worker->state = COMPLETION_WORKER_REAPING;
            thread = worker->thread;
        }
        pthread_mutex_unlock( &completion_executor_lock );
        if (!thread) continue;
        status = NtWaitForSingleObject( thread, FALSE, &timeout );
        pthread_mutex_lock( &completion_executor_lock );
        if (!status)
        {
            worker->thread = NULL;
            worker->state = COMPLETION_WORKER_FREE;
        }
        else worker->state = COMPLETION_WORKER_EXITING;
        pthread_cond_broadcast( &completion_executor_cond );
        pthread_mutex_unlock( &completion_executor_lock );
        if (!status) NtClose( thread );
    }
}

static void client_surface_completion_thread( void *context );

/* Include creation reservations and still-exiting threads in the four slots.
 * A callback's RETIRE is not evidence that its native thread has exited. No
 * replacement can reuse that slot until its thread handle is signalled. */
static BOOL start_client_surface_completion_thread( struct client_surface *prospective )
{
    struct client_surface_completion_worker *worker = NULL;
    struct list *entry;
    unsigned int i, active, demand;
    NTSTATUS status;
    HANDLE thread;

    reap_completion_workers();
    pthread_mutex_lock( &completion_executor_lock );
    active = active_completion_workers_locked();
    demand = completion_busy_workers;
    LIST_FOR_EACH( entry, &completion_ready_surfaces ) ++demand;
    if (prospective && list_empty( &prospective->completion_queue )) ++demand;
    if (active + starting_completion_workers_locked() <
        min( demand, (unsigned int)ARRAY_SIZE(completion_workers) ))
        for (i = 0; i < ARRAY_SIZE(completion_workers); ++i)
            if (completion_workers[i].state == COMPLETION_WORKER_FREE)
            {
                worker = &completion_workers[i];
                worker->state = COMPLETION_WORKER_STARTING;
                break;
            }
    pthread_mutex_unlock( &completion_executor_lock );
    if (!worker) return !!active;

    /* PsCreateSystemThread invokes this Unix entry directly, including for a
     * Windows process whose machine differs from the host. Creation itself
     * must not hold the executor lock or a native presentation lock. */
    status = PsCreateSystemThread( &thread, THREAD_ALL_ACCESS, NULL, 0, NULL,
                                  client_surface_completion_thread, worker );
    pthread_mutex_lock( &completion_executor_lock );
    worker->thread = status ? NULL : thread;
    worker->state = status ? COMPLETION_WORKER_FREE : COMPLETION_WORKER_RUNNING;
    pthread_cond_broadcast( &completion_executor_cond );
    pthread_mutex_unlock( &completion_executor_lock );
    if (status) WARN( "Failed to create client-surface completion worker, status %#lx\n", (unsigned long)status );
    return !status;
}

static void finish_deferred_present( struct client_surface *surface, struct client_surface_frame *present,
                                     const SIZE *expected_size, struct client_surface_completion_result result,
                                     BOOL polled )
{
    struct client_surface_completion completion = present->completion;
    struct client_surface_capture capture = present->capture;
    UINT64 control = present->handoff_control;
    unsigned long long begin = TRACE_ON(csperf) ? client_surface_perf_time() : 0;
    unsigned long long completed, released;

    TRACE( "event=completion_finish surface=%p serial=%s status=%u worker=%u polled=%u\n",
           surface, wine_dbgstr_longlong( present->serial ), result.status, result.worker, polled );
    if (!client_surface_complete_present( surface, present, TRUE,
             result.status == CLIENT_SURFACE_COMPLETION_SIGNALED, expected_size, 0 ) &&
        present->result == CLIENT_SURFACE_FRAME_PENDING)
        WARN( "deferred client-surface composition did not complete for %s\n",
              debugstr_client_surface( surface ) );
    completed = begin ? client_surface_perf_time() : 0;
    /* The FIFO execution lease includes capture, publication and both final
     * releases. Return the fence reference before the image reservation. A
     * failed or cancelled frame cannot capture, but still retires both. */
    completion.release( completion.context );
    if (capture.release) capture.release( capture.context );
    released = begin ? client_surface_perf_time() : 0;
    TRACE( "event=completion_release surface=%p serial=%s completion=%p capture=%p\n",
           surface, wine_dbgstr_longlong( present->serial ), completion.context, capture.context );
    TRACE_(csperf)( "ticks=%llu event=completion_finish identity=%s serial=%s control=%s "
                   "begin=%llu completed=%llu status=%u worker=%u polled=%u\n", released,
                   wine_dbgstr_longlong( client_surface_get_identity( surface ) ),
                   wine_dbgstr_longlong( present->serial ), wine_dbgstr_longlong( control ),
                   begin, completed, result.status, result.worker, polled );
}

static struct client_surface_completion_result poll_completion_job( struct client_surface *surface,
                                                                     struct client_surface_completion_job *job )
{
    struct client_surface_completion_result result;
    DWORD now = NtGetTickCount(), elapsed = now - job->wait_started;
    DWORD remaining = elapsed < job->wait_timeout ? job->wait_timeout - elapsed : 0;
    DWORD poll_start = now;

    /* One native poll per turn. Queueing, another worker, or an inline helper
     * cannot renew the original submission deadline. Even a zero budget gets
     * the same single readiness probe as the synchronous wait contract. */
    result = client_surface_poll_present_completion( surface, &job->present,
                min( remaining, (DWORD)CLIENT_SURFACE_COMPLETION_POLL_TIMEOUT_MS ) );
    if (result.status != CLIENT_SURFACE_COMPLETION_PENDING) return result;
    now = NtGetTickCount();
    elapsed = now - job->wait_started;
    if (elapsed >= job->wait_timeout)
    {
        WARN( "timed out waiting for presentation completion for %s serial %s\n",
              debugstr_client_surface( surface ), wine_dbgstr_longlong( job->present.serial ) );
        return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
    }
    job->poll_due = now;
    if (now == poll_start)
    {
        /* Immediate GLX/EGL queries back off without occupying a worker. The
         * native query and subsequent capture themselves have no hard bound. */
        job->poll_due += min( job->poll_delay, job->wait_timeout - elapsed );
        job->poll_delay = min( job->poll_delay * 2, (DWORD)4 );
    }
    return result;
}

/* Called with a head execution lease, and without any scheduler lock. A job
 * stays at the head through PENDING; only its executing thread owns its fields
 * until it returns the lease under completion_executor_lock. */
static enum client_surface_completion_worker_disposition execute_completion_job(
    struct client_surface *surface, struct client_surface_completion_job *job, BOOL poll, BOOL worker )
{
    struct client_surface_completion_result result = client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
    BOOL allocated = job->allocated;

    if (poll) result = poll_completion_job( surface, job );
    else TRACE( "cancelling completion without polling %s serial %s\n",
                debugstr_client_surface( surface ), wine_dbgstr_longlong( job->present.serial ) );
    if (result.status != CLIENT_SURFACE_COMPLETION_PENDING && job->deferred)
        finish_deferred_present( surface, &job->present,
                                 job->has_expected_size ? &job->expected_size : NULL, result, poll );

    pthread_mutex_lock( &completion_executor_lock );
    assert( surface->completion_in_progress && completion_head( surface ) == job );
    if (worker) --completion_busy_workers;
    surface->completion_in_progress = FALSE;
    job->result = result;
    job->pending = result.status == CLIENT_SURFACE_COMPLETION_PENDING;
    if (result.status != CLIENT_SURFACE_COMPLETION_PENDING)
    {
        list_remove( &job->entry );
        if (allocated) InterlockedDecrement( &client_surface_deferred_present_count );
        job->done = TRUE;
    }
    queue_ready_surface_locked( surface );
    pthread_mutex_unlock( &completion_executor_lock );
    /* A stack owner can return as soon as done is published. Never access a
     * stack job after unlocking, including in traces. Each queued job pins
     * its surface independently of the worker and the native callback refs. */
    if (result.status != CLIENT_SURFACE_COMPLETION_PENDING)
    {
        if (allocated) free( job );
        client_surface_release( surface );
    }
    return result.worker;
}

static struct client_surface_completion_job *claim_completion_head_locked( struct client_surface *surface )
{
    assert( !surface->completion_in_progress );
    assert( !list_empty( &surface->completion_queue ) );
    assert( !list_empty( &surface->completion_ready_entry ) );
    list_remove( &surface->completion_ready_entry );
    list_init( &surface->completion_ready_entry );
    surface->completion_in_progress = TRUE;
    return completion_head( surface );
}

static struct client_surface *next_completion_surface_locked( DWORD now, DWORD *delay, BOOL cancel )
{
    struct client_surface *surface;

    LIST_FOR_EACH_ENTRY( surface, &completion_ready_surfaces, struct client_surface, completion_ready_entry )
    {
        struct client_surface_completion_job *job = completion_head( surface );
        if (cancel || !job->pending || (INT)(now - job->poll_due) >= 0) return surface;
        *delay = min( *delay, job->poll_due - now );
    }
    return NULL;
}

static void cancel_ready_completion_jobs(void)
{
    struct client_surface_completion_job *job;
    struct client_surface *surface;
    DWORD delay = 0;

    /* With no clean worker (including a failed replacement), no queued heap
     * token may depend on a future producer. A concurrently executing inline
     * head remains owned by that caller, which also hands off or drains its
     * own tail before returning. Cancellation never polls or captures on this
     * thread; only the existing terminal abandonment/release path runs. */
    for (;;)
    {
        pthread_mutex_lock( &completion_executor_lock );
        /* Creation reservations cap resources but are not successful owners.
         * If the final clean worker retired during an unrelated creation,
         * that creation's failure must not strand the older ready queues. */
        while (!active_completion_workers_locked() && starting_completion_workers_locked())
            pthread_cond_wait( &completion_executor_cond, &completion_executor_lock );
        if (active_completion_workers_locked() ||
            !(surface = next_completion_surface_locked( 0, &delay, TRUE )))
        {
            pthread_mutex_unlock( &completion_executor_lock );
            return;
        }
        job = claim_completion_head_locked( surface );
        pthread_mutex_unlock( &completion_executor_lock );
        execute_completion_job( surface, job, FALSE, FALSE );
    }
}

static void client_surface_completion_thread( void *context )
{
    struct client_surface_completion_worker *worker = context;
    struct client_surface_completion_job *job;
    struct client_surface *surface;
    DWORD idle_started = NtGetTickCount(), now, delay;
    enum client_surface_completion_worker_disposition disposition;

    pthread_mutex_lock( &completion_executor_lock );
    while (worker->state == COMPLETION_WORKER_STARTING)
        pthread_cond_wait( &completion_executor_cond, &completion_executor_lock );
    pthread_mutex_unlock( &completion_executor_lock );
    TRACE( "event=completion_executor_start slot=%u\n", (unsigned int)(worker - completion_workers) );
    trace_client_surface_worker( "executor_start", worker - completion_workers, FALSE );
    for (;;)
    {
        start_client_surface_completion_thread( NULL );
        pthread_mutex_lock( &completion_executor_lock );
        for (;;)
        {
            now = NtGetTickCount();
            delay = CLIENT_SURFACE_COMPLETION_WORKER_IDLE_TIMEOUT_MS;
            if ((surface = next_completion_surface_locked( now, &delay, FALSE ))) break;
            if (!list_empty( &completion_ready_surfaces )) idle_started = now;
            else if (now - idle_started >= CLIENT_SURFACE_COMPLETION_WORKER_IDLE_TIMEOUT_MS)
            {
                worker->state = COMPLETION_WORKER_EXITING;
                pthread_cond_broadcast( &completion_executor_cond );
                pthread_mutex_unlock( &completion_executor_lock );
                TRACE( "event=completion_executor_exit slot=%u retire=0\n",
                       (unsigned int)(worker - completion_workers) );
                trace_client_surface_worker( "executor_exit", worker - completion_workers, FALSE );
                return;
            }
            else delay = min( delay, CLIENT_SURFACE_COMPLETION_WORKER_IDLE_TIMEOUT_MS - (now - idle_started) );
            wait_completion_executor_locked( delay );
        }
        job = claim_completion_head_locked( surface );
        ++completion_busy_workers;
        pthread_mutex_unlock( &completion_executor_lock );
        disposition = execute_completion_job( surface, job, TRUE, TRUE );
        idle_started = NtGetTickCount();
        if (disposition == CLIENT_SURFACE_COMPLETION_WORKER_RETIRE)
        {
            pthread_mutex_lock( &completion_executor_lock );
            worker->state = COMPLETION_WORKER_EXITING;
            pthread_cond_broadcast( &completion_executor_cond );
            pthread_mutex_unlock( &completion_executor_lock );
            TRACE( "event=completion_retire surface=%p slot=%u\n",
                   surface, (unsigned int)(worker - completion_workers) );
            /* A clean spare may start in another free slot. The current slot
             * remains counted until actual thread exit; if all four are still
             * retiring we fail closed instead of creating a fifth thread. */
            if (!start_client_surface_completion_thread( NULL )) cancel_ready_completion_jobs();
            TRACE( "event=completion_executor_exit slot=%u retire=1\n",
                   (unsigned int)(worker - completion_workers) );
            trace_client_surface_worker( "executor_exit", worker - completion_workers, TRUE );
            return;
        }
    }
}

static void queue_completion_job_locked( struct client_surface *surface,
                                         struct client_surface_completion_job *job )
{
    BOOL empty = list_empty( &surface->completion_queue );

    list_add_tail( &surface->completion_queue, &job->entry );
    if (empty) queue_ready_surface_locked( surface );
    TRACE_(csperf)( "ticks=%llu event=completion_queue identity=%s serial=%s control=%s target_epoch=%s "
                   "deferred=%u inline=%u head=%u\n", client_surface_perf_time(),
                   wine_dbgstr_longlong( client_surface_get_identity( surface ) ),
                   wine_dbgstr_longlong( job->present.serial ), wine_dbgstr_longlong( job->present.handoff_control ),
                   wine_dbgstr_longlong( job->present.target_epoch ), job->deferred, !job->allocated, empty );
    TRACE( "event=completion_enqueue surface=%p serial=%s completion=%p capture=%p deferred=%u inline=%u\n",
           surface, wine_dbgstr_longlong( job->present.serial ), job->present.completion.context,
           job->present.capture.context, job->deferred, !job->allocated );
    pthread_cond_broadcast( &completion_executor_cond );
}

/* Allocation/admission/worker failure does not create a second ordering path.
 * A stack owner can help earlier jobs of its own surface, but cannot skip an
 * executing or PENDING head, nor wait for unrelated surfaces to free a worker.
 * Its stack storage and an extra surface reference live until terminal done. */
static void complete_inline_job( struct client_surface *surface, struct client_surface_completion_job *own )
{
    struct client_surface_completion_job *job;
    BOOL reusable = TRUE;
    DWORD now, delay;

    for (;;)
    {
        pthread_mutex_lock( &completion_executor_lock );
        if (own->done)
        {
            pthread_mutex_unlock( &completion_executor_lock );
            break;
        }
        job = completion_head( surface );
        now = NtGetTickCount();
        if (!surface->completion_in_progress &&
            (!reusable || !job->pending || (INT)(now - job->poll_due) >= 0))
        {
            job = claim_completion_head_locked( surface );
            pthread_mutex_unlock( &completion_executor_lock );
            if (execute_completion_job( surface, job, reusable, FALSE ) == CLIENT_SURFACE_COMPLETION_WORKER_RETIRE)
                reusable = FALSE;
        }
        else
        {
            delay = CLIENT_SURFACE_COMPLETION_POLL_TIMEOUT_MS;
            if (!surface->completion_in_progress && job->pending) delay = min( delay, job->poll_due - now );
            wait_completion_executor_locked( delay );
            pthread_mutex_unlock( &completion_executor_lock );
        }
    }
    /* The public wait-only API leaves capture/finish to its original caller.
     * It orders native waits, not those later caller operations. In particular
     * a caller contaminated while helping an earlier job must not capture on
     * return, even if a clean executor completed this wait-only node. */
    if (!reusable && !own->deferred)
    {
        own->result = client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
        own->result.worker = CLIENT_SURFACE_COMPLETION_WORKER_RETIRE;
    }
    /* The legacy synchronous SHARED probe runs under completion_lock. Its
     * exclusive monitor admission drained all earlier host tokens and still
     * bars new submissions, so it cannot leave a later FIFO job here. Do not
     * create a worker or finish a different frame under that native lock. */
    if (!own->deferred && own->present.completion.kind == CLIENT_SURFACE_COMPLETION_SHARED)
    {
        pthread_mutex_lock( &completion_executor_lock );
        assert( list_empty( &surface->completion_queue ) );
        pthread_mutex_unlock( &completion_executor_lock );
    }
    else if (!start_client_surface_completion_thread( NULL ))
    {
        for (;;)
        {
            pthread_mutex_lock( &completion_executor_lock );
            if (active_completion_workers_locked() || surface->completion_in_progress ||
                list_empty( &surface->completion_queue ))
            {
                pthread_mutex_unlock( &completion_executor_lock );
                break;
            }
            job = claim_completion_head_locked( surface );
            pthread_mutex_unlock( &completion_executor_lock );
            /* Our own result is already complete. If no clean worker can own
             * the tail, retire it without native waits or capture. Another
             * inline owner will receive its terminal notification normally. */
            execute_completion_job( surface, job, FALSE, FALSE );
        }
    }
    client_surface_release( surface );
}

struct client_surface_completion_result client_surface_wait_present_completion(
    struct client_surface *surface, const struct client_surface_frame *present, DWORD timeout )
{
    DWORD start = NtGetTickCount(), elapsed = start - present->submission_time;
    struct client_surface_completion_job job = { .present = *present, .poll_delay = 1, .wait_started = start };

    job.wait_timeout = min( timeout, elapsed < CLIENT_SURFACE_PRESENT_TIMEOUT ?
                                    CLIENT_SURFACE_PRESENT_TIMEOUT - elapsed : 0 );
    /* One reference for the queue and one for the caller, which may outlive
     * terminal removal by a worker. Acquire both before exposing the node. */
    client_surface_add_ref( surface );
    client_surface_add_ref( surface );
    pthread_mutex_lock( &completion_executor_lock );
    queue_completion_job_locked( surface, &job );
    pthread_mutex_unlock( &completion_executor_lock );
    complete_inline_job( surface, &job );
    return job.result;
}

static BOOL completion_queue_has_room_locked( struct client_surface *surface )
{
    struct list *entry;
    unsigned int count = 0;

    /* Include the executing head through its final releases. Synchronous
     * stack nodes cannot allocate more retained heap work under pressure. */
    LIST_FOR_EACH( entry, &surface->completion_queue )
        if (++count >= CLIENT_SURFACE_MAX_DEFERRED_PRESENTS) return FALSE;
    return TRUE;
}

void client_surface_defer_present( struct client_surface *surface,
                                   struct client_surface_frame *present,
                                   const SIZE *expected_size )
{
    struct client_surface_completion completion = present->completion;
    struct client_surface_completion_job local = {0}, *job;
    BOOL reserved = FALSE, asynchronous = FALSE;

    assert( completion.kind != CLIENT_SURFACE_COMPLETION_NONE );
    assert( completion.external_result );
    assert( present->serial );
    assert( InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ) > 0 );
    assert( completion.wait && completion.release );

    if ((job = malloc( sizeof(*job) )))
    {
        reserved = client_surface_reserve_completion_slot();
        if (reserved && InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ) <
                        CLIENT_SURFACE_MAX_DEFERRED_PRESENTS)
            asynchronous = start_client_surface_completion_thread( surface );
    }
    /* Creation is outside the queue lock. Recheck the active workers while
     * enqueueing: the last idle worker could have exited in the meantime. */
    client_surface_add_ref( surface );
    client_surface_add_ref( surface );
    pthread_mutex_lock( &completion_executor_lock );
    asynchronous = asynchronous && active_completion_workers_locked() && completion_queue_has_room_locked( surface );
    if (asynchronous) memset( job, 0, sizeof(*job) );
    else
    {
        free( job );
        if (reserved) InterlockedDecrement( &client_surface_deferred_present_count );
        job = &local;
    }
    job->present = *present;
    job->allocated = asynchronous;
    job->deferred = TRUE;
    job->has_expected_size = !!expected_size;
    if (expected_size) job->expected_size = *expected_size;
    job->wait_started = present->submission_time;
    job->wait_timeout = CLIENT_SURFACE_PRESENT_TIMEOUT;
    job->poll_delay = 1;
    queue_completion_job_locked( surface, job );
    pthread_mutex_unlock( &completion_executor_lock );
    if (asynchronous)
    {
        memset( present, 0, sizeof(*present) );
        client_surface_release( surface );
    }
    else
    {
        complete_inline_job( surface, job );
        *present = local.present;
    }
}
