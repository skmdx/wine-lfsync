/*
 * In-process synchronization primitives
 *
 * Copyright (C) 2021-2022 Elizabeth Figura for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "ntstatus.h"
#include "winternl.h"

#include "file.h"
#include "handle.h"
#include "process.h"
#include "request.h"
#include "thread.h"
#include "user.h"
#include "wine/lockfree_sync.h"

#ifdef HAVE_LINUX_NTSYNC_H
# include <linux/ntsync.h>
#endif

#ifdef __linux__

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>
# include <linux/futex.h>
# include <linux/memfd.h>

static struct lf_sync_shared *lockfree_shared;
static struct lf_sync_dispatcher lockfree_dispatcher;

static void lockfree_wake( uint32_t *address )
{
    syscall( SYS_futex, address, FUTEX_WAKE, 1, NULL, NULL, 0 );
}

static int create_lockfree_memfd(void)
{
#ifdef HAVE_MEMFD_CREATE
    return memfd_create( "wine-lockfree-sync", MFD_CLOEXEC );
#elif defined(SYS_memfd_create)
    return syscall( SYS_memfd_create, "wine-lockfree-sync", MFD_CLOEXEC );
#else
    errno = ENOSYS;
    return -1;
#endif
}

static int create_lockfree_device(void)
{
    int fd;

    if ((fd = create_lockfree_memfd()) < 0) return -1;
    if (ftruncate( fd, sizeof(*lockfree_shared) ))
    {
        close( fd );
        return -1;
    }
    lockfree_shared = mmap( NULL, sizeof(*lockfree_shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
    if (lockfree_shared == MAP_FAILED)
    {
        lockfree_shared = NULL;
        close( fd );
        return -1;
    }
    lf_sync_init_shared( lockfree_shared );
    if (!lf_sync_open_shared( &lockfree_dispatcher, lockfree_shared, NULL, lockfree_wake ))
    {
        munmap( lockfree_shared, sizeof(*lockfree_shared) );
        lockfree_shared = NULL;
        close( fd );
        return -1;
    }
    return fd;
}

int get_inproc_device_fd(void)
{
    static int fd = -2;
    if (fd == -2)
    {
        if (getenv( "WINELOCKFREE_SYNC" )) fd = create_lockfree_device();
        if (fd < 0)
#ifdef NTSYNC_IOC_EVENT_READ
            fd = open( "/dev/ntsync", O_CLOEXEC | O_RDONLY );
#else
            fd = -1;
#endif
    }
    return fd;
}

struct inproc_sync
{
    struct object          obj;  /* object header */
    enum inproc_sync_type  type;
    int                    fd;
    unsigned int           shm_idx;
    struct list            entry;
    struct lockfree_lifetime *lifetime;
};

static struct list inproc_mutexes = LIST_INIT( inproc_mutexes );
static struct list retired_lifetimes = LIST_INIT( retired_lifetimes );

struct lockfree_lease
{
    uint64_t token;
    struct lockfree_lifetime *lifetime;
    struct list process_entry;
    struct list object_entry;
};

struct lockfree_lifetime
{
    uint32_t object;
    int retired;
    struct list leases;
    struct list entry;
};

struct ntsync_lease
{
    uint64_t token;
    struct inproc_sync *sync;
    struct list process_entry;
};

union lockfree_lease_entry
{
    struct lockfree_lease *lease;
    uint32_t next_free;
};

static union lockfree_lease_entry *lockfree_leases;
static uint32_t next_lease_slot;
static uint32_t free_lease_slot = UINT32_MAX;
static uint64_t next_ntsync_lease = 1;

static int reap_lockfree_lifetime( struct lockfree_lifetime *lifetime );

static int init_lockfree_leases(void)
{
    if (lockfree_leases) return 1;
    if (!(lockfree_leases = calloc( LF_SYNC_SHARED_LEASES, sizeof(*lockfree_leases) )))
    {
        set_error( STATUS_NO_MEMORY );
        return 0;
    }
    return 1;
}

static void destroy_lockfree_lease( struct lockfree_lease *lease )
{
    uint32_t slot = lease->token & LF_SYNC_LEASE_SLOT_MASK;

    assert( lockfree_leases[slot].lease == lease );
    if (!lf_sync_free_lease( lockfree_shared, lease->token )) assert( 0 );
    lockfree_leases[slot].next_free = free_lease_slot;
    free_lease_slot = slot;
    list_remove( &lease->process_entry );
    list_remove( &lease->object_entry );
    free( lease );
}

static void sweep_lockfree_lifetime_leases( struct lockfree_lifetime *lifetime )
{
    struct lockfree_lease *lease, *next;

    LIST_FOR_EACH_ENTRY_SAFE( lease, next, &lifetime->leases, struct lockfree_lease, object_entry )
        if (lf_sync_lease_is_released( lockfree_shared, lease->token )) destroy_lockfree_lease( lease );
}

static int reap_lockfree_lifetime( struct lockfree_lifetime *lifetime )
{
    sweep_lockfree_lifetime_leases( lifetime );
    if (!lifetime->retired || !list_empty( &lifetime->leases ) ||
        !lf_sync_free_object( &lockfree_dispatcher, lifetime->object )) return 0;

    list_remove( &lifetime->entry );
    free( lifetime );
    return 1;
}

static void reap_lockfree_lifetimes(void)
{
    struct lockfree_lifetime *lifetime, *next;

    LIST_FOR_EACH_ENTRY_SAFE( lifetime, next, &retired_lifetimes, struct lockfree_lifetime, entry )
        reap_lockfree_lifetime( lifetime );
}

static void abandon_lockfree_mutex( uint32_t object, thread_id_t tid )
{
    if (lf_sync_abandon_mutex( &lockfree_dispatcher.arena,
                               &lockfree_dispatcher.objects[object], tid ) == LF_SYNC_SUCCESS)
        lf_sync_wake_object( &lockfree_dispatcher, object );
}

static void abandon_lockfree_mutexes( thread_id_t tid )
{
    struct lockfree_lifetime *lifetime;
    struct inproc_sync *mutex;

    LIST_FOR_EACH_ENTRY( mutex, &inproc_mutexes, struct inproc_sync, entry )
        abandon_lockfree_mutex( mutex->shm_idx, tid );

    /* A destroyed handle can leave its shared mutex alive while a client
     * lease, waiter, or owner still references it. Such objects have left
     * inproc_mutexes, but remain uniquely represented by this list. */
    LIST_FOR_EACH_ENTRY( lifetime, &retired_lifetimes, struct lockfree_lifetime, entry )
        if (lf_sync_get_object_type( &lockfree_dispatcher.objects[lifetime->object] ) == LF_SYNC_MUTEX)
            abandon_lockfree_mutex( lifetime->object, tid );
}

static void drain_released_lockfree_leases(void)
{
    uint32_t word;

    for (word = 0; word < LF_SYNC_SHARED_LEASE_WORDS; ++word)
    {
        uint64_t bits = lf_sync_take_released_leases( lockfree_shared, word );

        while (bits)
        {
            uint32_t bit = __builtin_ctzll( bits );
            uint32_t slot = word * 64 + bit;
            struct lockfree_lease *lease = lockfree_leases[slot].lease;
            struct lockfree_lifetime *lifetime;

            bits &= bits - 1;
            if (!lease || !lf_sync_lease_is_released( lockfree_shared, lease->token )) continue;
            lifetime = lease->lifetime;
            destroy_lockfree_lease( lease );
            if (lifetime->retired) reap_lockfree_lifetime( lifetime );
        }
    }
}

static struct lockfree_lifetime *create_lockfree_lifetime(void)
{
    struct lockfree_lifetime *lifetime;

    if (!(lifetime = mem_alloc( sizeof(*lifetime) ))) return NULL;
    lifetime->retired = 0;
    list_init( &lifetime->leases );
    return lifetime;
}

static uint64_t create_lockfree_lease( struct lockfree_lifetime *lifetime, struct process *process )
{
    struct lockfree_lease *lease;
    uint32_t slot;

    if (!init_lockfree_leases() || !(lease = mem_alloc( sizeof(*lease) ))) return 0;
    if (next_lease_slot < LF_SYNC_SHARED_LEASES) slot = next_lease_slot++;
    else
    {
        if (free_lease_slot == UINT32_MAX) drain_released_lockfree_leases();
        if (free_lease_slot == UINT32_MAX)
        {
            free( lease );
            set_error( STATUS_TOO_MANY_OPENED_FILES );
            return 0;
        }
        slot = free_lease_slot;
        free_lease_slot = lockfree_leases[slot].next_free;
    }

    lease->lifetime = lifetime;
    list_add_tail( &process->lockfree_leases, &lease->process_entry );
    list_add_tail( &lifetime->leases, &lease->object_entry );
    lockfree_leases[slot].lease = lease;
    if (!lf_sync_activate_lease( lockfree_shared, slot, &lease->token ))
    {
        lockfree_leases[slot].next_free = free_lease_slot;
        free_lease_slot = slot;
        list_remove( &lease->process_entry );
        list_remove( &lease->object_entry );
        free( lease );
        set_error( STATUS_INTERNAL_ERROR );
        return 0;
    }
    return lease->token;
}

static uint64_t create_ntsync_lease( struct inproc_sync *sync, struct process *process )
{
    struct ntsync_lease *lease;

    if (!(lease = mem_alloc( sizeof(*lease) ))) return 0;
    if (!(lease->token = next_ntsync_lease++)) lease->token = next_ntsync_lease++;
    grab_object( sync );
    lease->sync = sync;
    list_add_tail( &process->ntsync_leases, &lease->process_entry );
    return lease->token;
}

static void destroy_ntsync_lease( struct ntsync_lease *lease )
{
    struct inproc_sync *sync = lease->sync;

    list_remove( &lease->process_entry );
    free( lease );
    release_object( sync );
}

static int create_lockfree_sync( struct inproc_sync *sync, enum lf_sync_object_type type,
                                 uint32_t initial, uint32_t limit, uint32_t flags )
{
    struct lockfree_lifetime *lifetime;

    if (!(lifetime = create_lockfree_lifetime())) return 0;
    if (!lf_sync_alloc_object( &lockfree_dispatcher, type, initial, limit, flags, &sync->shm_idx ))
    {
        /* Hung-up lifetimes normally reap themselves, while thread teardown
         * retries waits and owned mutexes that initially blocked reclamation.
         * Scan the retired list here only as an arena-exhaustion fallback. */
        reap_lockfree_lifetimes();
        if (!lf_sync_alloc_object( &lockfree_dispatcher, type, initial, limit, flags, &sync->shm_idx ))
        {
            free( lifetime );
            return 0;
        }
    }
    lifetime->object = sync->shm_idx;
    sync->lifetime = lifetime;
    sync->fd = -1;
    return 1;
}

static void inproc_sync_dump( struct object *obj, int verbose );
static int inproc_sync_signal( struct object *obj, unsigned int access, int signal );
static void inproc_sync_destroy( struct object *obj );

static const struct object_ops inproc_sync_ops =
{
    .size    = sizeof(struct inproc_sync),
    .type    = &no_type,
    .dump    = inproc_sync_dump,
    .signal  = inproc_sync_signal,
    .destroy = inproc_sync_destroy,
};

int get_inproc_sync_fd( struct inproc_sync *sync )
{
    return sync ? sync->fd : -1;
}

unsigned int get_inproc_sync_idx( struct inproc_sync *sync )
{
    return sync ? sync->shm_idx : ~0u;
}

struct inproc_sync *create_inproc_internal_sync( int manual, int signaled )
{
#ifdef NTSYNC_IOC_EVENT_READ
    struct ntsync_event_args args = {.signaled = signaled, .manual = manual};
#endif
    struct inproc_sync *event;

    if (!(event = alloc_object( &inproc_sync_ops ))) return NULL;
    event->type = INPROC_SYNC_INTERNAL;
    event->shm_idx = ~0u;
    event->lifetime = NULL;
    if (lockfree_shared)
    {
        if (!create_lockfree_sync( event, LF_SYNC_EVENT, signaled, 0,
                                   manual ? LF_SYNC_EVENT_MANUAL : 0 )) event->fd = -1;
    }
    else
#ifdef NTSYNC_IOC_EVENT_READ
        event->fd = ioctl( get_inproc_device_fd(), NTSYNC_IOC_CREATE_EVENT, &args );
#else
        event->fd = -1;
#endif
    list_init( &event->entry );

    if (event->fd == -1 && event->shm_idx == ~0u)
    {
        set_error( STATUS_TOO_MANY_OPENED_FILES );
        release_object( event );
        return NULL;
    }
    return event;
}

struct inproc_sync *create_inproc_event_sync( int manual, int signaled )
{
#ifdef NTSYNC_IOC_EVENT_READ
    struct ntsync_event_args args = {.signaled = signaled, .manual = manual};
#endif
    struct inproc_sync *event;

    if (!(event = alloc_object( &inproc_sync_ops ))) return NULL;
    event->type = INPROC_SYNC_EVENT;
    event->shm_idx = ~0u;
    event->lifetime = NULL;
    if (lockfree_shared)
    {
        if (!create_lockfree_sync( event, LF_SYNC_EVENT, signaled, 0,
                                   manual ? LF_SYNC_EVENT_MANUAL : 0 )) event->fd = -1;
    }
    else
#ifdef NTSYNC_IOC_EVENT_READ
        event->fd = ioctl( get_inproc_device_fd(), NTSYNC_IOC_CREATE_EVENT, &args );
#else
        event->fd = -1;
#endif
    list_init( &event->entry );

    if (event->fd == -1 && event->shm_idx == ~0u)
    {
        set_error( STATUS_TOO_MANY_OPENED_FILES );
        release_object( event );
        return NULL;
    }
    return event;
}

struct inproc_sync *create_inproc_mutex_sync( thread_id_t owner, unsigned int count )
{
#ifdef NTSYNC_IOC_EVENT_READ
    struct ntsync_mutex_args args = {.owner = owner, .count = count};
#endif
    struct inproc_sync *mutex;

    if (!(mutex = alloc_object( &inproc_sync_ops ))) return NULL;
    mutex->type = INPROC_SYNC_MUTEX;
    mutex->shm_idx = ~0u;
    mutex->lifetime = NULL;
    if (lockfree_shared)
    {
        if (!create_lockfree_sync( mutex, LF_SYNC_MUTEX, count, 0, owner )) mutex->fd = -1;
    }
    else
#ifdef NTSYNC_IOC_EVENT_READ
        mutex->fd = ioctl( get_inproc_device_fd(), NTSYNC_IOC_CREATE_MUTEX, &args );
#else
        mutex->fd = -1;
#endif
    list_add_tail( &inproc_mutexes, &mutex->entry );

    if (mutex->fd == -1 && mutex->shm_idx == ~0u)
    {
        set_error( STATUS_TOO_MANY_OPENED_FILES );
        release_object( mutex );
        return NULL;
    }
    return mutex;
}

struct inproc_sync *create_inproc_semaphore_sync( unsigned int initial, unsigned int max )
{
#ifdef NTSYNC_IOC_EVENT_READ
    struct ntsync_sem_args args = {.count = initial, .max = max};
#endif
    struct inproc_sync *sem;

    if (!(sem = alloc_object( &inproc_sync_ops ))) return NULL;
    sem->type = INPROC_SYNC_SEMAPHORE;
    sem->shm_idx = ~0u;
    sem->lifetime = NULL;
    if (lockfree_shared)
    {
        if (!create_lockfree_sync( sem, LF_SYNC_SEMAPHORE, initial, max, 0 )) sem->fd = -1;
    }
    else
#ifdef NTSYNC_IOC_EVENT_READ
        sem->fd = ioctl( get_inproc_device_fd(), NTSYNC_IOC_CREATE_SEM, &args );
#else
        sem->fd = -1;
#endif
    list_init( &sem->entry );

    if (sem->fd == -1 && sem->shm_idx == ~0u)
    {
        set_error( STATUS_TOO_MANY_OPENED_FILES );
        release_object( sem );
        return NULL;
    }
    return sem;
}

static void inproc_sync_dump( struct object *obj, int verbose )
{
    struct inproc_sync *sync = (struct inproc_sync *)obj;
    assert( obj->ops == &inproc_sync_ops );
    fprintf( stderr, "Inproc sync type=%d, fd=%d, shm_idx=%u\n", sync->type, sync->fd, sync->shm_idx );
}

void signal_inproc_sync( struct inproc_sync *sync )
{
    if (sync->shm_idx != ~0u)
    {
        uint32_t previous;

        lf_sync_set_event( &lockfree_dispatcher.arena,
                           &lockfree_dispatcher.objects[sync->shm_idx], &previous );
        if (!previous) lf_sync_wake_object( &lockfree_dispatcher, sync->shm_idx );
    }
    else
    {
#ifdef NTSYNC_IOC_EVENT_READ
        __u32 count;
        if (debug_level) fprintf( stderr, "set_inproc_event %d\n", sync->fd );
        ioctl( sync->fd, NTSYNC_IOC_EVENT_SET, &count );
#endif
    }
}

void reset_inproc_sync( struct inproc_sync *sync )
{
    if (sync->shm_idx != ~0u)
        lf_sync_reset_event( &lockfree_dispatcher.arena, &lockfree_dispatcher.objects[sync->shm_idx], NULL );
    else
    {
#ifdef NTSYNC_IOC_EVENT_READ
        __u32 count;
        if (debug_level) fprintf( stderr, "reset_inproc_event %d\n", sync->fd );
        ioctl( sync->fd, NTSYNC_IOC_EVENT_RESET, &count );
#endif
    }
}

static int inproc_sync_signal( struct object *obj, unsigned int access, int signal )
{
    struct inproc_sync *sync = (struct inproc_sync *)obj;
    assert( obj->ops == &inproc_sync_ops );

    assert( sync->type == INPROC_SYNC_INTERNAL || sync->type == INPROC_SYNC_EVENT ); /* never called for mutex / semaphore */
    assert( signal == 0 || signal == 1 ); /* never called from signal_object */

    if (signal) signal_inproc_sync( sync );
    else reset_inproc_sync( sync );
    return 1;
}

static void inproc_sync_destroy( struct object *obj )
{
    struct inproc_sync *sync = (struct inproc_sync *)obj;
    assert( obj->ops == &inproc_sync_ops );
    list_remove( &sync->entry );
    if (sync->shm_idx != ~0u)
    {
        struct lockfree_lifetime *lifetime = sync->lifetime;

        assert( lifetime );
        lifetime->retired = 1;
        list_add_tail( &retired_lifetimes, &lifetime->entry );
        sync->lifetime = NULL;
        reap_lockfree_lifetime( lifetime );
    }
    else if (sync->fd >= 0) close( sync->fd );
}

void abandon_inproc_mutexes( thread_id_t tid )
{
    struct inproc_sync *mutex;

    if (lockfree_shared)
    {
        lf_sync_set_owner_alive( &lockfree_dispatcher, tid, 0 );
        lf_sync_abandon_descriptors( &lockfree_dispatcher.arena, tid );
        lf_sync_abandon_waits( &lockfree_dispatcher, tid );
        /* Scan mutexes last. An ACTIVE transaction that linearized before
         * owner death may have acquired another mutex while descriptors were
         * being settled. Use the server's live and retired object indices
         * instead of scanning the entire shared object high-water mark. */
        abandon_lockfree_mutexes( tid );
        reap_lockfree_lifetimes();
        return;
    }

    LIST_FOR_EACH_ENTRY( mutex, &inproc_mutexes, struct inproc_sync, entry )
#ifdef NTSYNC_IOC_EVENT_READ
        ioctl( mutex->fd, NTSYNC_IOC_MUTEX_KILL, &tid );
#else
        assert( 0 );
#endif
}

void set_inproc_sync_owner_alive( thread_id_t tid )
{
    if (lockfree_shared) lf_sync_set_owner_alive( &lockfree_dispatcher, tid, 1 );
}

void release_process_inproc_sync_leases( struct process *process )
{
    while (!list_empty( &process->lockfree_leases ))
    {
        struct lockfree_lease *lease = LIST_ENTRY( list_head( &process->lockfree_leases ),
                                                   struct lockfree_lease, process_entry );
        struct lockfree_lifetime *lifetime = lease->lifetime;

        if (!lf_sync_lease_is_released( lockfree_shared, lease->token ) &&
            !lf_sync_mark_lease_released( lockfree_shared, lease->token )) assert( 0 );
        destroy_lockfree_lease( lease );
        if (lifetime->retired) reap_lockfree_lifetime( lifetime );
    }

    while (!list_empty( &process->ntsync_leases ))
    {
        struct ntsync_lease *lease = LIST_ENTRY( list_head( &process->ntsync_leases ),
                                                 struct ntsync_lease, process_entry );
        destroy_ntsync_lease( lease );
    }
}

static int release_ntsync_lease( struct process *process, uint64_t token )
{
    struct ntsync_lease *lease;

    LIST_FOR_EACH_ENTRY( lease, &process->ntsync_leases, struct ntsync_lease, process_entry )
    {
        if (lease->token != token) continue;
        destroy_ntsync_lease( lease );
        return 1;
    }
    return 0;
}

static int get_obj_inproc_sync( struct object *obj, int *type, unsigned int *shm_idx, uint64_t *lease )
{
    struct object *sync;
    int fd = -1;

    if (!(sync = get_obj_sync( obj ))) return -1;
    if (sync->ops == &inproc_sync_ops)
    {
        struct inproc_sync *inproc = (struct inproc_sync *)sync;
        *type = inproc->type;
        *shm_idx = inproc->shm_idx;
        if (inproc->shm_idx != ~0u)
        {
            assert( inproc->lifetime );
            *lease = create_lockfree_lease( inproc->lifetime, current->process );
        }
        else
        {
            fd = get_inproc_sync_fd( inproc );
            if (fd >= 0 && inproc->type == INPROC_SYNC_MUTEX)
                *lease = create_ntsync_lease( inproc, current->process );
        }
    }

    release_object( sync );
    return fd;
}

#else /* __linux__ */

int get_inproc_device_fd(void)
{
    return -1;
}

int get_inproc_sync_fd( struct inproc_sync *sync )
{
    return -1;
}

unsigned int get_inproc_sync_idx( struct inproc_sync *sync )
{
    return ~0u;
}

struct inproc_sync *create_inproc_internal_sync( int manual, int signaled )
{
    return NULL;
}

struct inproc_sync *create_inproc_event_sync( int manual, int signaled )
{
    return NULL;
}

struct inproc_sync *create_inproc_mutex_sync( thread_id_t owner, unsigned int count )
{
    return NULL;
}

struct inproc_sync *create_inproc_semaphore_sync( unsigned int initial, unsigned int max )
{
    return NULL;
}

void signal_inproc_sync( struct inproc_sync *sync )
{
}

void reset_inproc_sync( struct inproc_sync *sync )
{
}

void abandon_inproc_mutexes( thread_id_t tid )
{
}

void set_inproc_sync_owner_alive( thread_id_t tid )
{
}

void release_process_inproc_sync_leases( struct process *process )
{
}

static int release_ntsync_lease( struct process *process, uint64_t token )
{
    return 0;
}

static int get_obj_inproc_sync( struct object *obj, int *type, unsigned int *shm_idx, uint64_t *lease )
{
    return -1;
}

#endif /* __linux__ */

DECL_HANDLER(get_inproc_sync_fd)
{
    struct object *obj;
    int fd;

    if (!(obj = get_handle_obj( current->process, req->handle, 0, NULL ))) return;

    reply->access = get_handle_access( current->process, req->handle );

    reply->shm_idx = ~0u;
    reply->lease = 0;
    fd = get_obj_inproc_sync( obj, &reply->type, &reply->shm_idx, &reply->lease );
    if (reply->shm_idx != ~0u)
    {
        if (!reply->lease && !get_error()) set_error( STATUS_TOO_MANY_OPENED_FILES );
    }
    else if (fd < 0) set_error( STATUS_NOT_IMPLEMENTED );
    else if (reply->type == INPROC_SYNC_MUTEX && !reply->lease)
        set_error( STATUS_TOO_MANY_OPENED_FILES );
    else send_client_fd( current->process, fd, req->handle );

    release_object( obj );
}

DECL_HANDLER(release_inproc_sync_lease)
{
    if (!release_ntsync_lease( current->process, req->lease )) set_error( STATUS_INVALID_HANDLE );
}
