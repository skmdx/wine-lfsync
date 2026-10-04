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
#include "ntuser_private.h"
#include "wine/debug.h"
#include "wine/rbtree.h"
#include "wine/unixlib.h"

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
    struct client_surface_completion_domain *execution_domain;
    struct client_surface_frame present;
    enum client_surface_completion_status native_status;
    SIZE expected_size;
    DWORD poll_due, poll_delay;
    BOOL has_expected_size, pending;
};

/* The scheduler owns all transitions under completion_executor_lock. Queued
 * jobs and unsubmitted reservations pin surface; the queue's pointer is weak. */
struct client_surface_completion_queue
{
    struct client_surface *surface;
    struct list jobs;
    struct list ready_entry;
    unsigned int reservations;
    struct client_surface_completion_job retirement;
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
static LONGLONG completion_domain_counter;

struct client_surface_completion_domain
{
    struct rb_entry entry;
    UINT64 id;
    unsigned int references;
    unsigned int retirements; /* dormant cleanup owners, not native execution leases */
};

struct client_surface_completion_worker
{
    HANDLE thread;
    struct ntdll_thread *native_thread;
    /* A callback owns the slot through capture and final release. PENDING
     * returns the slot, but not the job's domain or surface FIFO position.
     * A contaminated thread retains its domain through native thread teardown. */
    struct client_surface_completion_domain *domain;
    BOOL finishing;
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

static int completion_domain_compare( const void *key, const struct rb_entry *entry )
{
    const struct client_surface_completion_domain *domain =
        RB_ENTRY_VALUE( entry, const struct client_surface_completion_domain, entry );
    UINT64 id = *(const UINT64 *)key;

    return id < domain->id ? -1 : id > domain->id;
}

static struct rb_tree completion_domains = {completion_domain_compare};

static struct client_surface_completion_worker *completion_domain_owner_locked(
    const struct client_surface_completion_domain *domain )
{
    unsigned int i;

    assert( domain );
    for (i = 0; i < ARRAY_SIZE(completion_workers); ++i)
        if (completion_workers[i].domain == domain) return &completion_workers[i];
    return NULL;
}

/* Empty domains only survive while pinned by one of the four workers. Stop
 * after enough demand is found instead of counting the entire backlog. */
static unsigned int completion_demand_locked( unsigned int limit )
{
    struct client_surface_completion_domain *domain;
    unsigned int count = 0;

    RB_FOR_EACH_ENTRY( domain, &completion_domains, struct client_surface_completion_domain, entry )
    {
        if (domain->references || domain->retirements)
        {
            if (++count == limit) break;
        }
        else assert( completion_domain_owner_locked( domain ) );
    }
    return count;
}

static void free_completion_domain_locked( struct client_surface_completion_domain *domain )
{
    assert( !domain->references && !completion_domain_owner_locked( domain ) );
    if (domain->retirements) return;
    rb_remove( &completion_domains, &domain->entry );
    client_surface_free_metadata( domain, sizeof(*domain) );
}

static void release_completion_domain_locked( struct client_surface_completion_domain *domain )
{
    assert( domain->references );
    --domain->references;
    TRACE( "event=completion_domain_release domain=%s references=%u\n",
           wine_dbgstr_longlong( domain->id ), domain->references );
    if (!domain->references && !completion_domain_owner_locked( domain )) free_completion_domain_locked( domain );
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
    assert( !queue->reservations );
    if (queue->retirement.execution_domain)
    {
        struct client_surface_completion_domain *domain = queue->retirement.execution_domain;
        pthread_mutex_lock( &completion_executor_lock );
        assert( domain->retirements );
        --domain->retirements;
        if (!domain->references && !completion_domain_owner_locked( domain ))
            free_completion_domain_locked( domain );
        pthread_cond_broadcast( &completion_executor_cond );
        pthread_mutex_unlock( &completion_executor_lock );
    }
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
    client_surface_get_target( surface, &target );
    if (ReadAcquire( &surface->closing ) || !target.valid || present->target_epoch != target.epoch)
        result.status = CLIENT_SURFACE_COMPLETION_FAILED;
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

BOOL client_surface_get_execution_domain( UINT64 *domain )
{
    struct user_thread_info *info = get_user_thread_info();

    if (info->completion_domain)
    {
        *domain = info->completion_domain->id;
        return TRUE;
    }
    if (!info->client_surface_domain &&
        !(info->client_surface_domain = client_surface_allocate_completion_domains( 1 ))) return FALSE;
    *domain = info->client_surface_domain;
    return TRUE;
}

static struct client_surface_completion_domain *find_completion_domain_locked( UINT64 id )
{
    struct rb_entry *entry = rb_get( &completion_domains, &id );

    return entry ? RB_ENTRY_VALUE( entry, struct client_surface_completion_domain, entry ) : NULL;
}

static BOOL completion_domain_retiring_locked( UINT64 id )
{
    struct client_surface_completion_domain *domain = find_completion_domain_locked( id );
    struct client_surface_completion_worker *worker = domain ? completion_domain_owner_locked( domain ) : NULL;

    return worker && worker->state != COMPLETION_WORKER_RUNNING;
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
        /* The Wine handle can signal before native TLS destructors finish.
         * REAPING excludes other joiners; neither a new producer nor another
         * worker may reuse this slot or domain until the native join returns. */
        if (!status) status = ntdll_join_thread( worker->native_thread );
        pthread_mutex_lock( &completion_executor_lock );
        if (!status)
        {
            struct client_surface_completion_domain *domain = worker->domain;

            if (domain)
            {
                assert( completion_domain_owner_locked( domain ) == worker && !domain->references );
                worker->domain = NULL;
                free_completion_domain_locked( domain );
            }
            worker->thread = NULL;
            worker->native_thread = NULL;
            worker->state = COMPLETION_WORKER_FREE;
        }
        else worker->state = COMPLETION_WORKER_EXITING;
        pthread_cond_broadcast( &completion_executor_cond );
        pthread_mutex_unlock( &completion_executor_lock );
        if (!status) NtClose( thread );
    }
}

static void client_surface_completion_thread( void *context );
static enum client_surface_admission_reason acquire_completion_domain_locked( struct client_surface_completion_job *job,
                                                                            UINT64 id )
{
    struct client_surface_completion_domain *domain = find_completion_domain_locked( id );
    struct client_surface_completion_worker *owner = domain ? completion_domain_owner_locked( domain ) : NULL;
    unsigned int i, clean = 0;
    BOOL available = FALSE;

    if (ReadAcquire( &job->surface->closing )) return CLIENT_SURFACE_STALE_OR_CLOSED;
    if (owner && owner->state != COMPLETION_WORKER_RUNNING) return CLIENT_SURFACE_EXECUTOR_UNAVAILABLE;
    /* GLX can contaminate its domain-zero worker. Keep another clean slot
     * alive to observe its actual exit and retire the native surface later. */
    if (!id && domain && domain->retirements)
    {
        for (i = 0; i < ARRAY_SIZE(completion_workers); ++i)
            clean += completion_workers[i].state == COMPLETION_WORKER_RUNNING;
        if (clean < 2) return CLIENT_SURFACE_EXECUTOR_UNAVAILABLE;
    }
    /* Readiness polls share the bounded job backlog. Capture and release
     * keep their execution slot; when every slot is in that phase, refuse
     * new independent domains before they consume application sync. Existing
     * work may still queue behind its own domain's active call. */
    if (owner && owner->state == COMPLETION_WORKER_RUNNING)
        available = TRUE;
    for (i = 0; !available && i < ARRAY_SIZE(completion_workers); ++i)
        available = completion_workers[i].state == COMPLETION_WORKER_RUNNING && !completion_workers[i].finishing;
    if (!available) return CLIENT_SURFACE_EXECUTOR_UNAVAILABLE;
    if (!domain)
    {
        if (!(domain = client_surface_alloc_metadata( 1, sizeof(*domain) ))) return CLIENT_SURFACE_OUT_OF_MEMORY;
        domain->id = id;
        rb_put( &completion_domains, &domain->id, &domain->entry );
    }
    ++domain->references;
    job->execution_domain = domain;
    TRACE( "event=completion_reserve surface=%p domain=%s references=%u\n",
           job->surface, wine_dbgstr_longlong( domain->id ), domain->references );
    pthread_cond_broadcast( &completion_executor_cond );
    return CLIENT_SURFACE_ACCEPTED;
}

/* Include creation reservations and still-exiting threads in the four slots.
 * A callback's RETIRE is not evidence that its native thread has exited. No
 * replacement can reuse that slot until the native thread has been joined. */
enum client_surface_admission_reason client_surface_activate_completion( struct client_surface_completion_job *job,
                                                                        UINT64 id )
{
    struct client_surface_completion_worker *worker = NULL;
    struct client_surface_completion_domain *domain;
    unsigned int i, running = 0, domains;
    NTSTATUS status;
    HANDLE thread;
    struct ntdll_thread *native_thread;
    enum client_surface_admission_reason reason;

    if (ReadAcquire( &job->surface->closing )) return CLIENT_SURFACE_STALE_OR_CLOSED;
    if (job->execution_domain)
    {
        assert( job->execution_domain->id == id );
        return CLIENT_SURFACE_ACCEPTED;
    }
    reap_completion_workers();
    pthread_mutex_lock( &completion_executor_lock );
    domain = find_completion_domain_locked( id );
    domains = completion_demand_locked( CLIENT_SURFACE_COMPLETION_MAX_WORKERS );
    for (i = 0; i < ARRAY_SIZE(completion_workers); ++i)
    {
        running += completion_workers[i].state == COMPLETION_WORKER_RUNNING ||
                   completion_workers[i].state == COMPLETION_WORKER_STARTING;
    }
    reason = CLIENT_SURFACE_EXECUTOR_UNAVAILABLE;
    if (completion_domain_retiring_locked( id )) goto unlock;
    /* Grow up to the execution bound as independent domains arrive. Beyond
     * it, runnable domains share clean workers between completed polls. */
    if (running >= min( domains + !domain, CLIENT_SURFACE_COMPLETION_MAX_WORKERS ))
    {
        reason = acquire_completion_domain_locked( job, id );
        if (reason != CLIENT_SURFACE_EXECUTOR_UNAVAILABLE) goto unlock;
    }
    /* Creation never reserves a domain. Concurrent cold callers can use free
     * slots without depending on a still-STARTING thread; successful callers
     * coalesce onto the same domain when reserving under this lock. */
    for (i = 0; i < ARRAY_SIZE(completion_workers); ++i)
        if (completion_workers[i].state == COMPLETION_WORKER_FREE)
        {
            worker = &completion_workers[i];
            worker->state = COMPLETION_WORKER_STARTING;
            break;
        }
    if (!worker) reason = acquire_completion_domain_locked( job, id );
unlock:
    pthread_mutex_unlock( &completion_executor_lock );
    if (!worker) goto done;

    /* The system thread invokes this Unix entry directly, including for a
     * Windows process whose machine differs from the host. Creation itself
     * must not hold the executor lock or a native presentation lock. */
    status = ntdll_create_joinable_thread( &thread, &native_thread, client_surface_completion_thread, worker );
    pthread_mutex_lock( &completion_executor_lock );
    worker->thread = status ? NULL : thread;
    worker->native_thread = native_thread;
    worker->state = status ? COMPLETION_WORKER_FREE : COMPLETION_WORKER_RUNNING;
    /* Publishing the clean worker and assigning the ticket are atomic. An
     * unrelated caller cannot take the sole idle worker between our capacity
     * check and admission while other free thread slots remain available. */
    reason = acquire_completion_domain_locked( job, id );
    pthread_cond_broadcast( &completion_executor_cond );
    pthread_mutex_unlock( &completion_executor_lock );
    if (status) WARN( "Failed to create client-surface completion worker, status %#lx\n", (unsigned long)status );
done:
    if (reason != CLIENT_SURFACE_ACCEPTED)
        TRACE( "event=completion_admission_failed surface=%p domain=%s reason=%u scope=%u\n",
               job->surface, wine_dbgstr_longlong( id ), reason, CLIENT_SURFACE_CAPACITY_NONE );
    return reason;
}

BOOL client_surface_prepare_retirement( struct client_surface *surface )
{
    struct client_surface_completion_job *job = &surface->completion_queue->retirement;
    struct client_surface_completion_domain *domain;
    struct client_surface_completion_worker *worker;
    unsigned int i, running, starting;
    NTSTATUS status;
    HANDLE thread;
    struct ntdll_thread *native_thread;

    job->surface = surface;
    /* Cleanup ownership is dormant metadata, not a native execution ticket.
     * Concurrent surface creation must not consume the four execution slots
     * or fail because those slots are currently finishing other surfaces. */
    reap_completion_workers();
    pthread_mutex_lock( &completion_executor_lock );
    for (;;)
    {
        running = starting = 0;
        for (i = 0; i < ARRAY_SIZE(completion_workers); ++i)
        {
            running += completion_workers[i].state == COMPLETION_WORKER_RUNNING;
            starting += completion_workers[i].state == COMPLETION_WORKER_STARTING;
        }
        if (running >= 2) break;
        if (starting && running + starting == ARRAY_SIZE(completion_workers))
        {
            pthread_cond_wait( &completion_executor_cond, &completion_executor_lock );
            continue;
        }
        worker = NULL;
        for (i = 0; i < ARRAY_SIZE(completion_workers); ++i)
            if (completion_workers[i].state == COMPLETION_WORKER_FREE)
            {
                worker = &completion_workers[i];
                worker->state = COMPLETION_WORKER_STARTING;
                break;
            }
        pthread_mutex_unlock( &completion_executor_lock );
        if (!worker) return FALSE;
        status = ntdll_create_joinable_thread( &thread, &native_thread, client_surface_completion_thread, worker );
        pthread_mutex_lock( &completion_executor_lock );
        worker->thread = status ? NULL : thread;
        worker->native_thread = native_thread;
        worker->state = status ? COMPLETION_WORKER_FREE : COMPLETION_WORKER_RUNNING;
        pthread_cond_broadcast( &completion_executor_cond );
        if (status)
        {
            pthread_mutex_unlock( &completion_executor_lock );
            return FALSE;
        }
    }
    if (!(domain = find_completion_domain_locked( 0 )))
    {
        if ((domain = client_surface_alloc_metadata( 1, sizeof(*domain) )))
        {
            domain->id = 0;
            rb_put( &completion_domains, &domain->id, &domain->entry );
        }
    }
    if (domain)
    {
        job->execution_domain = domain;
        ++domain->retirements;
    }
    pthread_mutex_unlock( &completion_executor_lock );
    return !!domain;
}

static BOOL finish_deferred_present( struct client_surface *surface, struct client_surface_frame *present,
                                     const SIZE *expected_size, struct client_surface_completion_result result,
                                     BOOL polled )
{
    struct client_surface_completion completion = present->completion;
    struct client_surface_capture capture = present->capture;
    UINT64 control = present->handoff_control;
    unsigned long long begin = TRACE_ON(csperf) ? client_surface_perf_time() : 0;
    unsigned long long completed, released;
    struct client_surface_present_result outcome;

    if (!client_surface_try_complete_present( surface, present,
          result.status == CLIENT_SURFACE_COMPLETION_SIGNALED, expected_size, &outcome )) return FALSE;
    TRACE( "event=completion_finish surface=%p serial=%s status=%u worker=%u polled=%u\n",
           surface, wine_dbgstr_longlong( present->serial ), result.status, result.worker, polled );
    TRACE( "event=present_adoption surface=%p serial=%s owner=%u completion=%u image=%u handoff=%u result=%u elapsed=%u\n",
           surface, wine_dbgstr_longlong( present->serial ), outcome.owner, outcome.completion,
           outcome.image_complete, outcome.handoff, present->result,
           NtGetTickCount() - present->submission_time );
    if (!outcome.image_complete && present->result == CLIENT_SURFACE_FRAME_PENDING)
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
    return TRUE;
}

static struct client_surface_completion_result poll_completion_job( struct client_surface *surface,
                                                                     struct client_surface_completion_job *job )
{
    struct client_surface_completion_result result;
    DWORD now = NtGetTickCount(), elapsed = now - job->present.submission_time;
    DWORD remaining = elapsed < CLIENT_SURFACE_PRESENT_TIMEOUT ? CLIENT_SURFACE_PRESENT_TIMEOUT - elapsed : 0;
    DWORD poll_start = now;

    if (job == &surface->completion_queue->retirement)
    {
        client_surface_retire_resources( surface );
        return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_SIGNALED );
    }
    /* One native poll per turn, without renewing the submission deadline. */
    result = client_surface_poll_present_completion( surface, &job->present,
                min( remaining, (DWORD)CLIENT_SURFACE_COMPLETION_POLL_TIMEOUT_MS ) );
    if (result.status == CLIENT_SURFACE_COMPLETION_FAILED) return result;
    now = NtGetTickCount();
    elapsed = now - job->present.submission_time;
    if (client_surface_present_expired( &job->present ) ||
        elapsed >= CLIENT_SURFACE_PRESENT_TIMEOUT)
    {
        TRACE_(csperf)( "ticks=%llu event=completion_timeout identity=%s serial=%s elapsed=%u timeout=%u poll_elapsed=%u\n",
                       client_surface_perf_time(), wine_dbgstr_longlong( client_surface_get_identity( surface ) ),
                       wine_dbgstr_longlong( job->present.serial ), elapsed, CLIENT_SURFACE_PRESENT_TIMEOUT, now - poll_start );
        WARN( "timed out waiting for presentation completion for %s serial %s\n",
              debugstr_client_surface( surface ), wine_dbgstr_longlong( job->present.serial ) );
        result.status = CLIENT_SURFACE_COMPLETION_FAILED;
        return result;
    }
    if (result.status != CLIENT_SURFACE_COMPLETION_PENDING) return result;
    job->poll_due = now;
    if (now == poll_start)
    {
        /* Immediate GLX/EGL queries back off without occupying a worker. The
         * native query and subsequent capture themselves have no hard bound. */
        job->poll_due += min( job->poll_delay, CLIENT_SURFACE_PRESENT_TIMEOUT - elapsed );
        job->poll_delay = min( job->poll_delay * 2, (DWORD)4 );
    }
    return result;
}

/* Called with a head execution lease, and without any scheduler lock. A job
 * stays at the head through PENDING; only its executing thread owns its fields
 * until it returns the lease under completion_executor_lock. */
static enum client_surface_completion_worker_disposition execute_completion_job(
    struct client_surface *surface, struct client_surface_completion_job *job,
    struct client_surface_completion_worker *worker, BOOL poll )
{
    struct client_surface_completion_result result = client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
    struct client_surface_completion_domain *domain = job->execution_domain;
    struct user_thread_info *info = get_user_thread_info();
    struct client_surface_completion_domain *previous_domain = info->completion_domain;
    BOOL allocated = job != &surface->completion_queue->retirement;

    assert( worker && domain );
    info->completion_domain = domain;
    if (poll) result = job->native_status != CLIENT_SURFACE_COMPLETION_PENDING ?
                      client_surface_completion_result( job->native_status ) : poll_completion_job( surface, job );
    else TRACE( "cancelling completion without polling %s serial %s\n",
                debugstr_client_surface( surface ), wine_dbgstr_longlong( job->present.serial ) );
    if (result.status != CLIENT_SURFACE_COMPLETION_PENDING)
    {
        pthread_mutex_lock( &completion_executor_lock );
        worker->finishing = TRUE;
        pthread_mutex_unlock( &completion_executor_lock );
    }
    if (result.status != CLIENT_SURFACE_COMPLETION_PENDING && allocated &&
        !finish_deferred_present( surface, &job->present,
                                  job->has_expected_size ? &job->expected_size : NULL, result, poll ))
    {
        job->native_status = result.status;
        /* A RETIRE disposition belongs to the thread returning from the
         * native wait. Its domain stays blocked until that thread exits;
         * the later adoption-only attempt must not retire its new worker. */
        job->poll_due = NtGetTickCount() + job->poll_delay;
        job->poll_delay = min( job->poll_delay * 2, (DWORD)4 );
        result.status = CLIENT_SURFACE_COMPLETION_PENDING;
    }

    pthread_mutex_lock( &completion_executor_lock );
    assert( list_empty( &surface->completion_queue->ready_entry ) && completion_head( surface ) == job );
    job->pending = result.status == CLIENT_SURFACE_COMPLETION_PENDING;
    if (result.status != CLIENT_SURFACE_COMPLETION_PENDING)
    {
        list_remove( &job->entry );
        if (!allocated)
        {
            assert( domain->retirements );
            --domain->retirements;
            job->execution_domain = NULL;
        }
    }
    queue_ready_surface_locked( surface );
    pthread_mutex_unlock( &completion_executor_lock );
    /* Each queued job pins its surface independently of the worker and the
     * native callback refs. Only this worker can dispose of the dequeued job. */
    if (result.status != CLIENT_SURFACE_COMPLETION_PENDING)
    {
        if (allocated) free( job );
        client_surface_release( surface );
    }
    info->completion_domain = previous_domain;
    /* Native destruction in the last surface release is part of the lease,
     * too. Only now can another FIFO head or a newly admitted domain use this
     * slot. The worker array outlives both the job and the surface. */
    pthread_mutex_lock( &completion_executor_lock );
    assert( worker->domain == domain && completion_domain_owner_locked( domain ) == worker && domain->references );
    if (result.worker == CLIENT_SURFACE_COMPLETION_WORKER_RETIRE)
        worker->state = COMPLETION_WORKER_EXITING;
    worker->finishing = FALSE;
    if (worker->state == COMPLETION_WORKER_RUNNING)
        worker->domain = NULL;
    if (result.status != CLIENT_SURFACE_COMPLETION_PENDING) release_completion_domain_locked( domain );
    if (allocated && result.status != CLIENT_SURFACE_COMPLETION_PENDING)
        InterlockedDecrement( &client_surface_deferred_present_count );
    pthread_cond_broadcast( &completion_executor_cond );
    pthread_mutex_unlock( &completion_executor_lock );
    return result.worker;
}

static struct client_surface_completion_job *claim_completion_head_locked( struct client_surface *surface,
                                                                          struct client_surface_completion_worker *worker )
{
    struct client_surface_completion_job *job = completion_head( surface );
    struct client_surface_completion_domain *domain = job->execution_domain;
    struct client_surface_completion_worker *owner = domain ? completion_domain_owner_locked( domain ) : NULL;

    assert( !list_empty( &surface->completion_queue->jobs ) );
    assert( !list_empty( &surface->completion_queue->ready_entry ) );
    assert( !domain || (worker && (!owner || owner == worker)) );
    assert( !worker || (domain && (!worker->domain ||
            (worker->domain == domain && worker->state != COMPLETION_WORKER_RUNNING))) );
    list_remove( &surface->completion_queue->ready_entry );
    list_init( &surface->completion_queue->ready_entry );
    if (domain)
    {
        if (job == &surface->completion_queue->retirement)
            ++domain->references;
        assert( domain->references );
    }
    if (worker)
    {
        worker->domain = domain;
        TRACE( "event=completion_execute domain=%s slot=%u retirement=%u references=%u\n",
               wine_dbgstr_longlong( domain->id ), (unsigned int)(worker - completion_workers),
               job == &surface->completion_queue->retirement, domain->references );
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
        struct client_surface_completion_domain *domain = job->execution_domain;

        if (!domain) continue;
        if (cancel)
        {
            if (job == &queue->retirement || domain != worker->domain) continue;
            assert( completion_domain_owner_locked( domain ) == worker && worker->state != COMPLETION_WORKER_RUNNING );
        }
        else if (completion_domain_owner_locked( domain ) || worker->domain) continue;
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
     * all accepted tickets are queued or cancelled. Other domains may use
     * clean workers. No native poll or capture runs on this thread again. */
    for (;;)
    {
        pthread_mutex_lock( &completion_executor_lock );
        while (worker->domain->references &&
               !(surface = next_completion_surface_locked( worker, 0, &delay, TRUE )))
            pthread_cond_wait( &completion_executor_cond, &completion_executor_lock );
        if (!worker->domain->references)
        {
            /* Keep ownership until the reaper has joined the native thread.
             * Dormant cleanup jobs may only run on a clean thread then. */
            pthread_mutex_unlock( &completion_executor_lock );
            return;
        }
        job = claim_completion_head_locked( surface, worker );
        pthread_mutex_unlock( &completion_executor_lock );
        execute_completion_job( surface, job, worker, FALSE );
    }
}

static void client_surface_completion_thread( void *context )
{
    struct client_surface_completion_worker *worker = context;
    struct client_surface_completion_job *job;
    struct client_surface *surface;
    DWORD idle_started = NtGetTickCount(), now, delay;
    enum client_surface_completion_worker_disposition disposition;
    unsigned int i;

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
            if (completion_demand_locked( 1 )) idle_started = now;
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
            /* A retired GLX worker's last callback is not its thread exit.
             * An existing clean worker reaps the signalled handle; final
             * surface releases never create a replacement thread. */
            for (i = 0; i < ARRAY_SIZE(completion_workers); ++i)
                if (completion_workers[i].state == COMPLETION_WORKER_EXITING) break;
            if (i < ARRAY_SIZE(completion_workers))
            {
                pthread_mutex_unlock( &completion_executor_lock );
                reap_completion_workers();
                pthread_mutex_lock( &completion_executor_lock );
            }
        }
        job = claim_completion_head_locked( surface, worker );
        pthread_mutex_unlock( &completion_executor_lock );
        disposition = execute_completion_job( surface, job, worker, TRUE );
        idle_started = NtGetTickCount();
        if (disposition == CLIENT_SURFACE_COMPLETION_WORKER_RETIRE)
        {
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
    BOOL retirement = job == &surface->completion_queue->retirement;

    list_add_tail( &surface->completion_queue->jobs, &job->entry );
    if (empty) queue_ready_surface_locked( surface );
    TRACE_(csperf)( "ticks=%llu event=completion_queue identity=%s serial=%s control=%s target_epoch=%s "
                   "deferred=%u inline=%u head=%u\n", client_surface_perf_time(),
                   wine_dbgstr_longlong( client_surface_get_identity( surface ) ),
                   wine_dbgstr_longlong( job->present.serial ), wine_dbgstr_longlong( job->present.handoff_control ),
                   wine_dbgstr_longlong( job->present.target_epoch ),
                   !!job->execution_domain && !retirement, !job->execution_domain, empty );
    TRACE( "event=completion_enqueue surface=%p serial=%s completion=%p capture=%p deferred=%u inline=%u retirement=%u\n",
           surface, wine_dbgstr_longlong( job->present.serial ), job->present.completion.context,
           job->present.capture.context, !!job->execution_domain && !retirement,
           !job->execution_domain, retirement );
    pthread_cond_broadcast( &completion_executor_cond );
}

void client_surface_queue_retirement( struct client_surface *surface )
{
    struct client_surface_completion_queue *queue = surface->completion_queue;

    assert( ReadAcquire( &surface->ref ) == (CLIENT_SURFACE_REF_CLOSED | 1) );
    pthread_mutex_lock( &completion_executor_lock );
    assert( list_empty( &queue->jobs ) && !queue->reservations && list_empty( &queue->ready_entry ) );
    assert( queue->retirement.execution_domain );
    queue_completion_job_locked( surface, &queue->retirement );
    pthread_mutex_unlock( &completion_executor_lock );
}

/* The synchronous SHARED caller owns only its FIFO probe. Async frames and
 * native cleanup remain on their admitted executor, through final release. */
struct client_surface_completion_result client_surface_probe_shared_completion(
    struct client_surface *surface, const struct client_surface_frame *present )
{
    struct client_surface_completion_job probe = { .present = *present };
    struct client_surface_completion_result result;
    struct user_thread_info *info = get_user_thread_info();
    struct client_surface_completion_domain *previous_domain = info->completion_domain;

    assert( present->completion.kind == CLIENT_SURFACE_COMPLETION_SHARED );
    assert( !client_surface_completion_result_is_external( &present->completion ) );
    /* The caller holds completion_lock and its exclusive monitor admission.
     * Earlier tokens have drained, but their final releases may still own the
     * FIFO head. Keep our node charged while waiting; workers never execute
     * it because it has no execution domain. */
    client_surface_add_ref( surface );
    pthread_mutex_lock( &completion_executor_lock );
    queue_completion_job_locked( surface, &probe );
    while (list_empty( &surface->completion_queue->ready_entry ) || completion_head( surface ) != &probe)
        pthread_cond_wait( &completion_executor_cond, &completion_executor_lock );
    claim_completion_head_locked( surface, NULL );
    pthread_mutex_unlock( &completion_executor_lock );

    info->completion_domain = NULL;
    result = client_surface_poll_present_completion( surface, present, 0 );
    info->completion_domain = previous_domain;
    if (result.status == CLIENT_SURFACE_COMPLETION_PENDING || client_surface_present_expired( present ))
        result.status = CLIENT_SURFACE_COMPLETION_FAILED;

    pthread_mutex_lock( &completion_executor_lock );
    assert( list_empty( &surface->completion_queue->ready_entry ) && completion_head( surface ) == &probe );
    list_remove( &probe.entry );
    assert( list_empty( &surface->completion_queue->jobs ) );
    queue_ready_surface_locked( surface );
    pthread_mutex_unlock( &completion_executor_lock );
    client_surface_release( surface );
    return result;
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
 * Storage tickets do not assign a worker. The caller must separately acquire
 * execution admission, outside native locks and before consuming guest sync.
 * Vulkan uses a unique queue key; other backends share domain zero for the
 * process graphics connection. A key never dereferences native owner memory. */
struct client_surface_admission client_surface_reserve_completion_storage(
    struct client_surface *surface, struct client_surface_completion_job **ticket )
{
    struct client_surface_completion_job *job;
    struct client_surface_admission result = { CLIENT_SURFACE_ACCEPTED, CLIENT_SURFACE_CAPACITY_NONE };

    assert( !*ticket );
    if (ReadAcquire( &surface->closing ))
    {
        result.reason = CLIENT_SURFACE_STALE_OR_CLOSED;
        goto done;
    }
    if (!(job = calloc( 1, sizeof(*job) )))
    {
        result.reason = CLIENT_SURFACE_OUT_OF_MEMORY;
        goto done;
    }
    if (!client_surface_reserve_completion_slot())
    {
        free( job );
        result = (struct client_surface_admission){ CLIENT_SURFACE_CAPACITY_LIMIT, CLIENT_SURFACE_CAPACITY_GLOBAL };
        goto done;
    }
    client_surface_add_ref( surface );
    pthread_mutex_lock( &completion_executor_lock );
    if (ReadAcquire( &surface->closing )) result.reason = CLIENT_SURFACE_STALE_OR_CLOSED;
    else if (completion_queue_has_room_locked( surface ))
    {
        ++surface->completion_queue->reservations;
        job->surface = surface;
        TRACE( "event=completion_storage_reserve surface=%p\n", surface );
        pthread_mutex_unlock( &completion_executor_lock );
        *ticket = job;
        goto done;
    }
    else result = (struct client_surface_admission){ CLIENT_SURFACE_CAPACITY_LIMIT, CLIENT_SURFACE_CAPACITY_SURFACE };
    pthread_mutex_unlock( &completion_executor_lock );
    InterlockedDecrement( &client_surface_deferred_present_count );
    client_surface_release( surface );
    free( job );
done:
    if (result.reason != CLIENT_SURFACE_ACCEPTED)
        TRACE( "event=completion_admission_failed surface=%p reason=%u scope=%u\n",
               surface, result.reason, result.scope );
    TRACE( "event=completion_storage_result surface=%p reason=%u scope=%u ticket=%p\n",
           surface, result.reason, result.scope, *ticket );
    return result;
}

struct client_surface_admission client_surface_reserve_completion( struct client_surface *surface,
                                                                  struct client_surface_completion_job **ticket )
{
    struct client_surface_admission result = client_surface_reserve_completion_storage( surface, ticket );

    /* The non-Vulkan callers reserve immediately before locking their native
     * presentation path. Keep that combined admission contract. */
    if (result.reason != CLIENT_SURFACE_ACCEPTED) return result;
    result.reason = client_surface_activate_completion( *ticket, 0 );
    if (result.reason != CLIENT_SURFACE_ACCEPTED) client_surface_cancel_completion( ticket );
    return result;
}

void client_surface_cancel_completion( struct client_surface_completion_job **ticket )
{
    struct client_surface_completion_job *job = *ticket;
    struct client_surface *surface;
    struct client_surface_completion_domain *domain;

    if (!job) return;
    *ticket = NULL;
    surface = job->surface;
    domain = job->execution_domain;
    pthread_mutex_lock( &completion_executor_lock );
    assert( surface->completion_queue->reservations && (!domain || domain->references) );
    --surface->completion_queue->reservations;
    TRACE( "event=completion_storage_cancel surface=%p active=%u\n", surface, !!domain );
    pthread_mutex_unlock( &completion_executor_lock );
    client_surface_release( surface );
    free( job );
    pthread_mutex_lock( &completion_executor_lock );
    if (domain) release_completion_domain_locked( domain );
    InterlockedDecrement( &client_surface_deferred_present_count );
    pthread_cond_broadcast( &completion_executor_cond );
    pthread_mutex_unlock( &completion_executor_lock );
}

void client_surface_enqueue_present( struct client_surface_completion_job **ticket,
                                            struct client_surface_frame *present, const SIZE *expected_size )
{
    struct client_surface_completion_job *job = *ticket;
    struct client_surface *surface = job->surface;

    assert( present->serial );
    assert( !present->completion_job );
    assert( present->completion.kind != CLIENT_SURFACE_COMPLETION_NONE );
    assert( present->completion.external_result && present->completion.wait && present->completion.release );
    assert( InterlockedCompareExchange( &surface->external_completion_count, 0, 0 ) > 0 );
    *ticket = NULL;
    job->present = *present;
    job->has_expected_size = !!expected_size;
    if (expected_size) job->expected_size = *expected_size;
    job->poll_delay = 1;
    TRACE( "event=present_result surface=%p serial=%s owner=%u completion=%u image=0 handoff=%u frame=%u kind=%u mode=%u\n",
           surface, wine_dbgstr_longlong( present->serial ), CLIENT_SURFACE_PRESENT_EXECUTOR,
           CLIENT_SURFACE_COMPLETION_PENDING, CLIENT_SURFACE_HANDOFF_NOT_QUEUED, present->result,
           present->completion.kind, present->mode );
    memset( present, 0, sizeof(*present) );
    pthread_mutex_lock( &completion_executor_lock );
    assert( surface->completion_queue->reservations && job->execution_domain->references );
    --surface->completion_queue->reservations;
    TRACE( "event=completion_storage_submit surface=%p domain=%s\n",
           surface, wine_dbgstr_longlong( job->execution_domain->id ) );
    /* Publishing the job and consuming its reservation are atomic to worker
     * exit. A retiring last worker remains the cancellation owner until all
     * accepted tickets have either entered this FIFO or been cancelled. */
    queue_completion_job_locked( surface, job );
    pthread_mutex_unlock( &completion_executor_lock );
}

void client_surface_enqueue_prepared_present( struct client_surface *surface,
                                   struct client_surface_frame *present,
                                   const SIZE *expected_size )
{
    struct client_surface_completion_job *job = present->completion_job;

    assert( job && job->surface == surface );
    present->completion_job = NULL;
    client_surface_enqueue_present( &job, present, expected_size );
}
