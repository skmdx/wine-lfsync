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
#include <errno.h>
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
    struct client_surface *surface;
    struct client_surface_completion_worker *worker;
    struct client_surface_frame present;
    struct client_surface_completion_result result;
    SIZE expected_size;
    DWORD wait_started, wait_timeout, poll_due, poll_delay;
    BOOL has_expected_size, deferred, allocated, reserved, pending, done;
};

/* The scheduler owns all transitions under completion_executor_lock. Queued
 * jobs and unsubmitted reservations pin surface; the queue's pointer is weak. */
struct client_surface_completion_queue
{
    struct client_surface *surface;
    struct list jobs;
    struct list ready_entry;
    unsigned int reservations;
    BOOL in_progress;
};

#define CLIENT_SURFACE_MAX_DEFERRED_PRESENTS 64
#define CLIENT_SURFACE_MAX_PROCESS_DEFERRED_PRESENTS 1024
#define CLIENT_SURFACE_COMPLETION_WORKER_IDLE_TIMEOUT_MS 100
#define CLIENT_SURFACE_COMPLETION_POLL_TIMEOUT_MS 10
#define CLIENT_SURFACE_COMPLETION_MAX_WORKERS 4
static LONG client_surface_deferred_present_count;

static pthread_mutex_t completion_executor_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t completion_executor_once = PTHREAD_ONCE_INIT;
static pthread_cond_t completion_executor_cond;
static int completion_executor_init_status;
static struct list completion_ready_surfaces = LIST_INIT(completion_ready_surfaces);
static unsigned int completion_reservation_count;
static LONGLONG completion_domain_counter;

struct client_surface_completion_worker
{
    HANDLE thread;
    /* An admitted native domain owns this slot through the last callback and
     * release, including unsubmitted tickets. A stopped native call cannot
     * consume another domain's worker. PENDING yields within this domain. */
    UINT64 domain;
    unsigned int references;
    BOOL executing;
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

static void release_completion_worker_locked( struct client_surface_completion_worker *worker )
{
    assert( worker->references );
    --worker->references;
    TRACE( "event=completion_domain_release domain=%s slot=%u references=%u\n", wine_dbgstr_longlong( worker->domain ),
           (unsigned int)(worker - completion_workers), worker->references );
}

/* Native timed waits and the caller's remaining budget must both ignore wall
 * clock changes. Darwin provides a relative wait instead of a clock attribute. */
int client_surface_cond_init( pthread_cond_t *cond )
{
#ifdef __APPLE__
    return pthread_cond_init( cond, NULL );
#else
    pthread_condattr_t attr;
    int ret;

    if ((ret = pthread_condattr_init( &attr ))) return ret;
    if (!(ret = pthread_condattr_setclock( &attr, CLOCK_MONOTONIC )))
        ret = pthread_cond_init( cond, &attr );
    pthread_condattr_destroy( &attr );
    return ret;
#endif
}

int client_surface_cond_timedwait( pthread_cond_t *cond, pthread_mutex_t *mutex, DWORD timeout )
{
    struct timespec time;

#ifdef __APPLE__
    time.tv_sec = timeout / 1000;
    time.tv_nsec = (timeout % 1000) * 1000000;
    return pthread_cond_timedwait_relative_np( cond, mutex, &time );
#else
    if (clock_gettime( CLOCK_MONOTONIC, &time )) return errno;
    time.tv_sec += timeout / 1000;
    time.tv_nsec += (timeout % 1000) * 1000000;
    time.tv_sec += time.tv_nsec / 1000000000;
    time.tv_nsec %= 1000000000;
    return pthread_cond_timedwait( cond, mutex, &time );
#endif
}

static void init_completion_executor(void)
{
    completion_executor_init_status = client_surface_cond_init( &completion_executor_cond );
}

BOOL client_surface_completion_init( struct client_surface *surface )
{
    struct client_surface_completion_queue *queue;

    if (pthread_once( &completion_executor_once, init_completion_executor ) || completion_executor_init_status)
        return FALSE;
    if (!(queue = calloc( 1, sizeof(*queue) ))) return FALSE;
    queue->surface = surface;
    list_init( &queue->jobs );
    list_init( &queue->ready_entry );
    surface->completion_queue = queue;
    return TRUE;
}

void client_surface_completion_destroy( struct client_surface *surface )
{
    struct client_surface_completion_queue *queue = surface->completion_queue;

    assert( list_empty( &queue->jobs ) );
    assert( list_empty( &queue->ready_entry ) );
    assert( !queue->in_progress && !queue->reservations );
    free( queue );
    surface->completion_queue = NULL;
}

static BOOL client_surface_reserve_completion_slot(void)
{
    LONG count = ReadAcquire( &client_surface_deferred_present_count );

    while (count < CLIENT_SURFACE_MAX_PROCESS_DEFERRED_PRESENTS)
    {
        LONG previous = InterlockedCompareExchange( &client_surface_deferred_present_count, count + 1, count );
        if (previous == count) return TRUE;
        count = previous;
    }
    /* Rejected reservations never consume capacity or reach native submit. */
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
    return LIST_ENTRY( list_head( &surface->completion_queue->jobs ), struct client_surface_completion_job, entry );
}

static void queue_ready_surface_locked( struct client_surface *surface )
{
    assert( !surface->completion_queue->in_progress );
    assert( list_empty( &surface->completion_queue->ready_entry ) );
    if (!list_empty( &surface->completion_queue->jobs ))
        list_add_tail( &completion_ready_surfaces, &surface->completion_queue->ready_entry );
    pthread_cond_broadcast( &completion_executor_cond );
}

static void wait_completion_executor_locked( DWORD timeout )
{
    client_surface_cond_timedwait( &completion_executor_cond, &completion_executor_lock, timeout );
}

UINT64 client_surface_allocate_completion_domains( unsigned int count )
{
    LONGLONG previous, current = InterlockedCompareExchange64( &completion_domain_counter, 0, 0 );

    /* Zero names the process graphics backend. Queue domains are unique even
     * if device teardown frees the queue's address before the executor's last
     * surface release. Exhaustion fails admission instead of reusing a key. */
    for (;;)
    {
        if (!count || current > MAXLONGLONG - count) return 0;
        previous = InterlockedCompareExchange64( &completion_domain_counter, current + count, current );
        if (previous == current) return current + 1;
        current = previous;
    }
}

static struct client_surface_completion_worker *find_completion_worker_locked( UINT64 domain )
{
    struct client_surface_completion_worker *idle = NULL;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(completion_workers); ++i)
    {
        struct client_surface_completion_worker *worker = &completion_workers[i];

        /* A retiring domain still owns its accepted tickets. Do not admit a
         * second worker for that domain while its native thread is exiting. */
        if (worker->domain == domain && worker->references)
            return worker;
        if (worker->state == COMPLETION_WORKER_RUNNING && !worker->references && !worker->executing)
            idle = worker;
    }
    return idle;
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
static BOOL completion_queue_has_room_locked( struct client_surface *surface );

static BOOL reserve_completion_job_locked( struct client_surface *surface, UINT64 domain,
                                           struct client_surface_completion_job *job )
{
    struct client_surface_completion_worker *worker = find_completion_worker_locked( domain );

    if (!worker || worker->state != COMPLETION_WORKER_RUNNING || !completion_queue_has_room_locked( surface ))
        return FALSE;
    worker->domain = domain;
    ++worker->references;
    ++surface->completion_queue->reservations;
    ++completion_reservation_count;
    job->surface = surface;
    job->worker = worker;
    job->allocated = job->reserved = TRUE;
    TRACE( "event=completion_reserve surface=%p domain=%s slot=%u references=%u\n", surface, wine_dbgstr_longlong( domain ),
           (unsigned int)(worker - completion_workers), worker->references );
    pthread_cond_broadcast( &completion_executor_cond );
    return TRUE;
}

/* Include creation reservations and still-exiting threads in the four slots.
 * A callback's RETIRE is not evidence that its native thread has exited. No
 * replacement can reuse that slot until its thread handle is signalled. */
static BOOL start_client_surface_completion_thread( struct client_surface *surface, UINT64 domain,
                                                     struct client_surface_completion_job *job )
{
    struct client_surface_completion_worker *worker = NULL;
    unsigned int i;
    NTSTATUS status;
    HANDLE thread;
    BOOL admitted;

    reap_completion_workers();
    pthread_mutex_lock( &completion_executor_lock );
    if ((admitted = reserve_completion_job_locked( surface, domain, job ))) goto unlock;
    /* Creation never reserves a domain. Concurrent cold callers can use free
     * slots without depending on a still-STARTING thread; successful callers
     * coalesce onto the same domain when reserving under this lock. */
    if (!find_completion_worker_locked( domain ) && completion_queue_has_room_locked( surface ))
        for (i = 0; i < ARRAY_SIZE(completion_workers); ++i)
            if (completion_workers[i].state == COMPLETION_WORKER_FREE)
            {
                worker = &completion_workers[i];
                worker->state = COMPLETION_WORKER_STARTING;
                break;
            }
unlock:
    pthread_mutex_unlock( &completion_executor_lock );
    if (!worker) return admitted;

    /* PsCreateSystemThread invokes this Unix entry directly, including for a
     * Windows process whose machine differs from the host. Creation itself
     * must not hold the executor lock or a native presentation lock. */
    status = PsCreateSystemThread( &thread, THREAD_ALL_ACCESS, NULL, 0, NULL,
                                  client_surface_completion_thread, worker );
    pthread_mutex_lock( &completion_executor_lock );
    worker->thread = status ? NULL : thread;
    worker->state = status ? COMPLETION_WORKER_FREE : COMPLETION_WORKER_RUNNING;
    /* Publishing the clean worker and assigning the ticket are atomic. An
     * unrelated caller cannot take the sole idle worker between our capacity
     * check and admission while other free thread slots remain available. */
    admitted = reserve_completion_job_locked( surface, domain, job );
    pthread_cond_broadcast( &completion_executor_cond );
    pthread_mutex_unlock( &completion_executor_lock );
    if (status) WARN( "Failed to create client-surface completion worker, status %#lx\n", (unsigned long)status );
    return admitted;
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
    struct client_surface *surface, struct client_surface_completion_job *job, BOOL poll, BOOL owner_thread )
{
    struct client_surface_completion_result result = client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
    struct client_surface_completion_worker *worker = job->worker;
    BOOL allocated = job->allocated;

    if (poll) result = poll_completion_job( surface, job );
    else TRACE( "cancelling completion without polling %s serial %s\n",
                debugstr_client_surface( surface ), wine_dbgstr_longlong( job->present.serial ) );
    if (result.status != CLIENT_SURFACE_COMPLETION_PENDING && job->deferred)
        finish_deferred_present( surface, &job->present,
                                 job->has_expected_size ? &job->expected_size : NULL, result, poll );

    pthread_mutex_lock( &completion_executor_lock );
    assert( surface->completion_queue->in_progress && completion_head( surface ) == job );
    surface->completion_queue->in_progress = FALSE;
    job->result = result;
    job->pending = result.status == CLIENT_SURFACE_COMPLETION_PENDING;
    if (result.status != CLIENT_SURFACE_COMPLETION_PENDING)
    {
        list_remove( &job->entry );
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
    /* Native destruction in the last surface release is part of the lease,
     * too. Only now can another FIFO head or a newly admitted domain use this
     * slot. The worker array outlives both the job and the surface. */
    pthread_mutex_lock( &completion_executor_lock );
    if (worker)
    {
        assert( worker->executing && worker->references );
        if (owner_thread && result.worker == CLIENT_SURFACE_COMPLETION_WORKER_RETIRE)
            worker->state = COMPLETION_WORKER_EXITING;
        worker->executing = FALSE;
        if (result.status != CLIENT_SURFACE_COMPLETION_PENDING) release_completion_worker_locked( worker );
    }
    if (allocated && result.status != CLIENT_SURFACE_COMPLETION_PENDING)
        InterlockedDecrement( &client_surface_deferred_present_count );
    pthread_cond_broadcast( &completion_executor_cond );
    pthread_mutex_unlock( &completion_executor_lock );
    return result.worker;
}

static struct client_surface_completion_job *claim_completion_head_locked( struct client_surface *surface )
{
    struct client_surface_completion_job *job = completion_head( surface );

    assert( !surface->completion_queue->in_progress );
    assert( !list_empty( &surface->completion_queue->jobs ) );
    assert( !list_empty( &surface->completion_queue->ready_entry ) );
    list_remove( &surface->completion_queue->ready_entry );
    list_init( &surface->completion_queue->ready_entry );
    surface->completion_queue->in_progress = TRUE;
    if (job->worker)
    {
        assert( !job->worker->executing && job->worker->references );
        job->worker->executing = TRUE;
    }
    return job;
}

static struct client_surface *next_completion_surface_locked(
    struct client_surface_completion_worker *worker, DWORD now, DWORD *delay, BOOL cancel )
{
    struct client_surface_completion_queue *queue;

    LIST_FOR_EACH_ENTRY( queue, &completion_ready_surfaces, struct client_surface_completion_queue, ready_entry )
    {
        struct client_surface *surface = queue->surface;
        struct client_surface_completion_job *job = completion_head( surface );
        if (job->worker != worker || worker->executing) continue;
        if (cancel || !job->pending || (INT)(now - job->poll_due) >= 0) return surface;
        *delay = min( *delay, job->poll_due - now );
    }
    return NULL;
}

static void cancel_worker_completion_jobs( struct client_surface_completion_worker *worker )
{
    struct client_surface_completion_job *job;
    struct client_surface *surface;
    DWORD delay = 0;

    /* A contaminated thread remains this domain's cancellation owner until
     * all accepted tickets are queued or cancelled. Other domains keep their
     * own workers. No native poll or capture runs on this thread again. */
    for (;;)
    {
        pthread_mutex_lock( &completion_executor_lock );
        while (worker->references &&
               !(surface = next_completion_surface_locked( worker, 0, &delay, TRUE )))
            pthread_cond_wait( &completion_executor_cond, &completion_executor_lock );
        if (!worker->references)
        {
            pthread_mutex_unlock( &completion_executor_lock );
            return;
        }
        job = claim_completion_head_locked( surface );
        pthread_mutex_unlock( &completion_executor_lock );
        execute_completion_job( surface, job, FALSE, TRUE );
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
        pthread_mutex_lock( &completion_executor_lock );
        for (;;)
        {
            now = NtGetTickCount();
            delay = CLIENT_SURFACE_COMPLETION_WORKER_IDLE_TIMEOUT_MS;
            if ((surface = next_completion_surface_locked( worker, now, &delay, FALSE ))) break;
            if (worker->references) idle_started = now;
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
            cancel_worker_completion_jobs( worker );
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
    BOOL empty = list_empty( &surface->completion_queue->jobs );

    list_add_tail( &surface->completion_queue->jobs, &job->entry );
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

/* Explicit synchronous waits use the same FIFO. A stack owner can help earlier
 * jobs of its own surface, but cannot skip an executing or PENDING head. An
 * admitted head retains its native-domain lease even while an inline caller
 * helps it, so another surface in that domain cannot execute concurrently.
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
        if (!surface->completion_queue->in_progress && (!job->worker || !job->worker->executing) &&
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
            if (!surface->completion_queue->in_progress && job->pending) delay = min( delay, job->poll_due - now );
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
        assert( list_empty( &surface->completion_queue->jobs ) );
        pthread_mutex_unlock( &completion_executor_lock );
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
    unsigned int count = surface->completion_queue->reservations;

    /* Include unsubmitted reservations and the head through final releases.
     * Synchronous stack nodes cannot allocate more retained heap work under pressure. */
    if (count >= CLIENT_SURFACE_MAX_DEFERRED_PRESENTS) return FALSE;
    LIST_FOR_EACH( entry, &surface->completion_queue->jobs )
        if (++count >= CLIENT_SURFACE_MAX_DEFERRED_PRESENTS) return FALSE;
    return TRUE;
}

/* Reserve outside native and surface locks. Once admitted, this reference and
 * capacity belong to the ticket until cancellation or FIFO terminal release.
 * Outstanding tickets keep their domain's worker alive before submission.
 * Vulkan uses a unique queue key; other backends share domain zero for the
 * process graphics connection. A key never dereferences native owner memory. */
struct client_surface_completion_job *client_surface_reserve_completion_domain(
    struct client_surface *surface, UINT64 domain )
{
    struct client_surface_completion_job *job;

    if (!(job = calloc( 1, sizeof(*job) ))) return NULL;
    if (!client_surface_reserve_completion_slot())
    {
        free( job );
        return NULL;
    }
    client_surface_add_ref( surface );
    if (start_client_surface_completion_thread( surface, domain, job )) return job;
    TRACE( "event=completion_admission_failed surface=%p domain=%s\n", surface, wine_dbgstr_longlong( domain ) );
    InterlockedDecrement( &client_surface_deferred_present_count );
    client_surface_release( surface );
    free( job );
    return NULL;
}

struct client_surface_completion_job *client_surface_reserve_completion( struct client_surface *surface )
{
    return client_surface_reserve_completion_domain( surface, 0 );
}

void client_surface_cancel_completion( struct client_surface_completion_job *job )
{
    struct client_surface *surface;
    struct client_surface_completion_worker *worker;

    if (!job) return;
    surface = job->surface;
    worker = job->worker;
    pthread_mutex_lock( &completion_executor_lock );
    assert( job->reserved && surface->completion_queue->reservations && worker->references && completion_reservation_count );
    --surface->completion_queue->reservations;
    --completion_reservation_count;
    pthread_mutex_unlock( &completion_executor_lock );
    client_surface_release( surface );
    free( job );
    pthread_mutex_lock( &completion_executor_lock );
    release_completion_worker_locked( worker );
    InterlockedDecrement( &client_surface_deferred_present_count );
    pthread_cond_broadcast( &completion_executor_cond );
    pthread_mutex_unlock( &completion_executor_lock );
}

void client_surface_defer_reserved_present( struct client_surface_completion_job *job,
                                            struct client_surface_frame *present, const SIZE *expected_size )
{
    struct client_surface *surface = job->surface;

    assert( job->reserved && present->serial );
    assert( !present->completion_job );
    assert( present->completion.kind != CLIENT_SURFACE_COMPLETION_NONE );
    assert( present->completion.external_result && present->completion.wait && present->completion.release );
    assert( InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ) > 0 );
    job->present = *present;
    job->deferred = TRUE;
    job->has_expected_size = !!expected_size;
    if (expected_size) job->expected_size = *expected_size;
    job->wait_started = present->submission_time;
    job->wait_timeout = CLIENT_SURFACE_PRESENT_TIMEOUT;
    job->poll_delay = 1;
    memset( present, 0, sizeof(*present) );
    pthread_mutex_lock( &completion_executor_lock );
    assert( surface->completion_queue->reservations && job->worker->references && completion_reservation_count );
    job->reserved = FALSE;
    --surface->completion_queue->reservations;
    --completion_reservation_count;
    /* Publishing the job and consuming its reservation are atomic to worker
     * exit. A retiring last worker remains the cancellation owner until all
     * accepted tickets have either entered this FIFO or been cancelled. */
    queue_completion_job_locked( surface, job );
    pthread_mutex_unlock( &completion_executor_lock );
}

void client_surface_defer_present( struct client_surface *surface,
                                   struct client_surface_frame *present,
                                   const SIZE *expected_size )
{
    struct client_surface_completion_job *job = present->completion_job;

    assert( job && job->surface == surface );
    present->completion_job = NULL;
    client_surface_defer_reserved_present( job, present, expected_size );
}
