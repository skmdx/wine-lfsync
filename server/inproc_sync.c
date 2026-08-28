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
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "ntstatus.h"
#include "winternl.h"

#include "file.h"
#include "handle.h"
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
#ifdef __linux__
# include <linux/futex.h>
# include <linux/memfd.h>
#endif

static struct lf_sync_shared *lockfree_shared;
static struct lf_sync_dispatcher lockfree_dispatcher;

static void lockfree_wake( uint32_t *address )
{
#ifdef __linux__
    syscall( SYS_futex, address, FUTEX_WAKE, 1, NULL, NULL, 0 );
#endif
}

static int create_lockfree_device(void)
{
#ifdef HAVE_MEMFD_CREATE
    int fd;

    if ((fd = memfd_create( "wine-lockfree-sync", MFD_CLOEXEC )) < 0) return -1;
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
#else
    return -1;
#endif
}

int get_inproc_device_fd(void)
{
    static int fd = -2;
    if (fd == -2)
    {
        if (getenv( "WINELOCKFREE_SYNC" )) fd = create_lockfree_device();
        if (fd < 0 && !lockfree_shared)
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
    int                    ephemeral;
    struct list            entry;
    struct lockfree_lifetime *lifetime;
};

static struct list inproc_mutexes = LIST_INIT( inproc_mutexes );
static struct list retired_lifetimes = LIST_INIT( retired_lifetimes );
static int creating_lockfree_lifetime;

struct lockfree_lifetime
{
    struct object obj;
    struct fd *fd;
    uint32_t object;
    int retired;
    int hung_up;
    struct list entry;
};

static void lockfree_lifetime_dump( struct object *obj, int verbose );
static void lockfree_lifetime_destroy( struct object *obj );
static void lockfree_lifetime_poll_event( struct fd *fd, int event );

static const struct object_ops lockfree_lifetime_ops =
{
    .size = sizeof(struct lockfree_lifetime),
    .type = &no_type,
    .dump = lockfree_lifetime_dump,
    .destroy = lockfree_lifetime_destroy,
};

static const struct fd_ops lockfree_lifetime_fd_ops =
{
    .poll_event = lockfree_lifetime_poll_event,
};

static void lockfree_lifetime_dump( struct object *obj, int verbose )
{
    struct lockfree_lifetime *lifetime = (struct lockfree_lifetime *)obj;
    fprintf( stderr, "Lock-free lifetime object=%u active=%u retired=%u hung_up=%u\n",
             lifetime->object, !!lifetime->fd, lifetime->retired, lifetime->hung_up );
}

static void lockfree_lifetime_destroy( struct object *obj )
{
    struct lockfree_lifetime *lifetime = (struct lockfree_lifetime *)obj;

    list_remove( &lifetime->entry );
    if (lifetime->fd) release_object( lifetime->fd );
}

static int reap_lockfree_lifetime( struct lockfree_lifetime *lifetime )
{
    if (!lifetime->retired || !lifetime->hung_up || lifetime->object == ~0u ||
        !lf_sync_free_object( &lockfree_dispatcher, lifetime->object )) return 0;

    if (lifetime->fd) set_fd_events( lifetime->fd, -1 );
    lifetime->object = ~0u;
    list_remove( &lifetime->entry );
    list_init( &lifetime->entry );
    release_object( lifetime ); /* persistent retirement reference */
    return 1;
}

static void reap_lockfree_lifetimes(void)
{
    struct lockfree_lifetime *lifetime, *next;

    LIST_FOR_EACH_ENTRY_SAFE( lifetime, next, &retired_lifetimes, struct lockfree_lifetime, entry )
        reap_lockfree_lifetime( lifetime );
}

static void lockfree_lifetime_poll_event( struct fd *fd, int event )
{
    struct lockfree_lifetime *lifetime = get_fd_user( fd );

    grab_object( lifetime );
    if (event & (POLLERR | POLLHUP))
    {
        set_fd_events( fd, -1 );
        lifetime->hung_up = 1;
        reap_lockfree_lifetime( lifetime );
    }
    release_object( lifetime );
}

static struct lockfree_lifetime *create_lockfree_lifetime( uint32_t object )
{
    struct lockfree_lifetime *lifetime;

    if (!(lifetime = alloc_object( &lockfree_lifetime_ops )))
        return NULL;
    lifetime->fd = NULL;
    lifetime->object = object;
    lifetime->retired = 0;
    lifetime->hung_up = 1; /* no client references until the pipe is activated */
    list_init( &lifetime->entry );
    return lifetime;
}

static int activate_lockfree_lifetime( struct lockfree_lifetime *lifetime )
{
    int pipe_fd[2];

    if (lifetime->fd) return -1;
    if (pipe( pipe_fd ) < 0) return -1;
    creating_lockfree_lifetime = 1;
    lifetime->fd = create_anonymous_fd( &lockfree_lifetime_fd_ops, pipe_fd[0], &lifetime->obj, 0 );
    creating_lockfree_lifetime = 0;
    if (!lifetime->fd)
    {
        close( pipe_fd[1] );
        return -1;
    }
    set_fd_events( lifetime->fd, POLLIN );
    lifetime->hung_up = 0;
    return pipe_fd[1];
}

static int create_lockfree_sync( struct inproc_sync *sync, enum lf_sync_object_type type,
                                 uint32_t initial, uint32_t limit, uint32_t flags )
{
    struct lockfree_lifetime *lifetime = NULL;

    reap_lockfree_lifetimes();
    /* An object that has never been sent to a client has no stale shared
     * references to track, so defer the lifetime pipe and its server object
     * until the first get_inproc_sync_fd request. An initially-owned mutex is
     * the exception: it can outlive its last handle until its owner exits. */
    if (type == LF_SYNC_MUTEX && initial && !(lifetime = create_lockfree_lifetime( ~0u ))) return 0;
    if (!lf_sync_alloc_object( &lockfree_dispatcher, type, initial, limit, flags, &sync->shm_idx ))
    {
        if (lifetime) release_object( lifetime );
        return 0;
    }
    if (lifetime) lifetime->object = sync->shm_idx;
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
    if (!sync) return -1;
    if (sync->shm_idx != ~0u && sync->fd < 0)
    {
        if (!sync->lifetime && !(sync->lifetime = create_lockfree_lifetime( sync->shm_idx ))) return -1;
        sync->fd = activate_lockfree_lifetime( sync->lifetime );
    }
    return sync->fd;
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
    event->ephemeral = creating_lockfree_lifetime;
    event->lifetime = NULL;
    if (lockfree_shared)
    {
        if (event->ephemeral)
        {
            if (!lf_sync_alloc_object( &lockfree_dispatcher, LF_SYNC_EVENT, signaled, 0,
                                       manual ? LF_SYNC_EVENT_MANUAL : 0, &event->shm_idx )) event->fd = -1;
            else event->fd = -1;
        }
        else if (!create_lockfree_sync( event, LF_SYNC_EVENT, signaled, 0,
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
    event->ephemeral = 0;
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
    mutex->ephemeral = 0;
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
    sem->ephemeral = 0;
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
    if (sync->ephemeral)
        lf_sync_free_object( &lockfree_dispatcher, sync->shm_idx );
    else if (sync->shm_idx != ~0u)
    {
        struct lockfree_lifetime *lifetime = sync->lifetime;

        if (lifetime)
        {
            if (sync->fd >= 0) close( sync->fd );
            sync->fd = -1;
            lifetime->retired = 1;
            list_add_tail( &retired_lifetimes, &lifetime->entry );
            sync->lifetime = NULL; /* transfer its reference to the retired list */
            reap_lockfree_lifetime( lifetime );
        }
        else
        {
            /* No client could have registered a waiter without first
             * activating the lifetime. Initially-owned mutexes also get a
             * lifetime at creation, so an unpublished object is reusable now. */
            int freed = lf_sync_free_object( &lockfree_dispatcher, sync->shm_idx );
            assert( freed );
        }
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
         * being settled. */
        lf_sync_abandon_owned_mutexes( &lockfree_dispatcher, tid );
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

static int get_obj_inproc_sync( struct object *obj, int *type, unsigned int *shm_idx )
{
    struct object *sync;
    int fd = -1;

    if (!(sync = get_obj_sync( obj ))) return -1;
    if (sync->ops == &inproc_sync_ops)
    {
        struct inproc_sync *inproc = (struct inproc_sync *)sync;
        *type = inproc->type;
        *shm_idx = inproc->shm_idx;
        fd = get_inproc_sync_fd( inproc );
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

static int get_obj_inproc_sync( struct object *obj, int *type, unsigned int *shm_idx )
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
    fd = get_obj_inproc_sync( obj, &reply->type, &reply->shm_idx );
    if (fd < 0) set_error( STATUS_NOT_IMPLEMENTED );
    else send_client_fd( current->process, fd, req->handle );

    release_object( obj );
}
