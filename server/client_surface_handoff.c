/*
 * Client surface handoff registry and endpoint lifetime
 *
 * Copyright 2026 Wine contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#ifdef __linux__
#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/eventfd.h>
#endif

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "object.h"
#include "file.h"
#include "handle.h"
#include "request.h"
#include "thread.h"
#include "process.h"
#include "client_surface_handoff.h"
#include "wine/client_surface.h"

struct client_surface_handoff_binding
{
    struct client_surface_handoff_pool *pool; /* NULL marks an unused slot */
    struct object *owner; /* exact owner lifetime, independently of its HWND */
    UINT64 cookie;
    unsigned int index;
    unsigned int mapped;
    int retired;
    void (*changed)( void *context );
    void *context;
};

struct client_surface_handoff_pool
{
    struct list entry;
    struct process *producer; /* raw keys, removed on either process cleanup */
    struct process *consumer;
    struct object *mapping;
    struct client_surface_handoff_shared *shared;
    UINT64 id;
    struct file *ready_read;
    struct file *ready_write;
    int ready_fd;
    /* Bindings share their pool allocation; channel admission adds no new
     * allocator call or failure after creating the native notification fd. */
    struct client_surface_handoff_binding bindings[CLIENT_SURFACE_HANDOFF_CHANNELS];
};

static struct list client_surface_handoff_pools = LIST_INIT( client_surface_handoff_pools );
static UINT64 client_surface_handoff_serial;

#ifdef __linux__
static void client_surface_handoff_futex_wake( LONG *address )
{
    syscall( SYS_futex, address, FUTEX_WAKE, INT_MAX, NULL, NULL, 0 );
}
#else
static void client_surface_handoff_futex_wake( LONG *address )
{
    (void)address;
}
#endif

static void wake_client_surface_handoff( struct client_surface_handoff_pool *pool, int ready )
{
    LONG *parked = ready ? &pool->shared->ready_parked : &pool->shared->release_parked;
    LONG *sequence = ready ? &pool->shared->ready_sequence : &pool->shared->release_sequence;

    /* As in lfsync, the waiter publishes parked before rechecking state. The
     * waker which claims that publication owns the sequence increment. */
    if (!__atomic_exchange_n( parked, 0, __ATOMIC_ACQ_REL )) return;
    __atomic_add_fetch( sequence, 1, __ATOMIC_RELEASE );
    if (ready)
    {
        UINT64 value = 1;
        int ret;

        do
#ifdef __linux__
            ret = write( pool->ready_fd, &value, sizeof(value) );
#else
            ret = send( pool->ready_fd, &value, sizeof(value), 0 );
#endif
        while (ret < 0 && errno == EINTR);
    }
    else client_surface_handoff_futex_wake( sequence );
}

static void signal_client_surface_handoff_ready( struct client_surface_handoff_pool *pool,
                                                 unsigned int index )
{
    __atomic_fetch_or( &pool->shared->ready_bitmap[index / 64],
                       (UINT64)1 << (index % 64), __ATOMIC_RELEASE );
    wake_client_surface_handoff( pool, 1 );
}

struct client_surface_handoff_state client_surface_handoff_get_state(
    const struct client_surface_handoff_binding *binding )
{
    struct client_surface_handoff_state state = {0};
    const struct client_surface_handoff_channel *channel;

    if (!binding) return state;
    channel = &binding->pool->shared->channels[binding->index];
    state.owner = binding->owner;
    state.consumer = binding->pool->consumer;
    state.window = channel->window;
    state.cookie = binding->cookie;
    state.mapped = binding->mapped;
    state.retired = binding->retired;
    state.lost = __atomic_load_n( &channel->closed, __ATOMIC_ACQUIRE );
    return state;
}

static void client_surface_handoff_close( struct client_surface_handoff_binding *binding )
{
    if (!binding) return;
    __atomic_store_n( &binding->pool->shared->channels[binding->index].closed, 1, __ATOMIC_RELEASE );
    binding->changed( binding->context );
    signal_client_surface_handoff_ready( binding->pool, binding->index );
    wake_client_surface_handoff( binding->pool, 0 );
}

void client_surface_handoff_free( struct client_surface_handoff_binding *binding )
{
    struct client_surface_handoff_pool *pool;
    unsigned int index;

    if (!binding) return;
    pool = binding->pool;
    index = binding->index;
    assert( !binding->mapped );
    client_surface_handoff_close( binding );
    __atomic_fetch_and( &pool->shared->ready_bitmap[index / 64],
                        ~((UINT64)1 << (index % 64)), __ATOMIC_ACQ_REL );
    __atomic_store_n( &pool->shared->channels[index].endpoints, 0, __ATOMIC_RELEASE );
    release_object( binding->owner );
    memset( binding, 0, sizeof(*binding) );
}

struct client_surface_handoff_binding *client_surface_handoff_retire(
    struct client_surface_handoff_binding *binding )
{
    if (!binding) return NULL;
    client_surface_handoff_close( binding );
    if (binding->mapped) return binding;
    client_surface_handoff_free( binding );
    return NULL;
}

struct client_surface_handoff_binding *client_surface_handoff_retarget(
    struct client_surface_handoff_binding *binding )
{
    if (!binding) return NULL;
    binding->retired = 1;
    binding->changed( binding->context );
    /* The old owner may still be reading. Its endpoint release, not a scene
     * change, proves that those checked copies have finished. */
    if (!(binding->mapped & CLIENT_SURFACE_HANDOFF_ENDPOINT_CONSUMER))
        return client_surface_handoff_retire( binding );
    return binding;
}

static struct client_surface_handoff_pool *create_client_surface_handoff_pool(
    struct process *producer, struct process *consumer )
{
    struct client_surface_handoff_pool *pool, *cursor;
    unsigned int count = 0;
    int fds[2];
    void *ptr;

    if (client_surface_handoff_serial == ~(UINT64)0)
    {
        set_error( STATUS_NO_MEMORY );
        return NULL;
    }
    LIST_FOR_EACH_ENTRY( cursor, &client_surface_handoff_pools,
                         struct client_surface_handoff_pool, entry )
        if (cursor->consumer == consumer &&
            ++count >= CLIENT_SURFACE_HANDOFF_MAX_POOLS_PER_CONSUMER)
        {
            set_error( STATUS_INSUFFICIENT_RESOURCES );
            return NULL;
        }
    if (!(pool = mem_alloc( sizeof(*pool) ))) return NULL;
#ifdef __linux__
    fds[0] = eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK );
    fds[1] = fds[0] < 0 ? -1 : fcntl( fds[0], F_DUPFD_CLOEXEC, 0 );
#else
    if (socketpair( AF_UNIX, SOCK_DGRAM, 0, fds )) fds[0] = fds[1] = -1;
    if (fds[0] >= 0)
    {
        if (fcntl( fds[0], F_SETFD, FD_CLOEXEC ) < 0 ||
            fcntl( fds[1], F_SETFD, FD_CLOEXEC ) < 0 ||
            fcntl( fds[0], F_SETFL, O_NONBLOCK ) < 0 ||
            fcntl( fds[1], F_SETFL, O_NONBLOCK ) < 0)
        {
            int saved_errno = errno;

            close( fds[0] );
            close( fds[1] );
            fds[0] = fds[1] = -1;
            errno = saved_errno;
        }
    }
#endif
    if (fds[0] < 0 || fds[1] < 0)
    {
        file_set_error();
        if (fds[0] >= 0) close( fds[0] );
        free( pool );
        return NULL;
    }
    pool->ready_fd = fds[1];
    if (!(pool->ready_read = create_file_for_fd( fds[0], FILE_GENERIC_READ, FILE_SHARE_READ )))
    {
        close( fds[1] );
        free( pool );
        return NULL;
    }
    if (!(pool->ready_write = create_file_for_fd( fds[1], FILE_GENERIC_WRITE, FILE_SHARE_WRITE )))
    {
        release_object( pool->ready_read );
        free( pool );
        return NULL;
    }
    if (!(pool->mapping = create_shared_data_mapping(
              sizeof(struct client_surface_handoff_shared), &ptr )))
    {
        release_object( pool->ready_write );
        release_object( pool->ready_read );
        free( pool );
        return NULL;
    }
    pool->shared = ptr;
    pool->producer = producer;
    pool->consumer = consumer;
    pool->id = ++client_surface_handoff_serial;
    memset( pool->bindings, 0, sizeof(pool->bindings) );
    memset( pool->shared, 0, sizeof(*pool->shared) );
    pool->shared->mapping_id = pool->id;
    pool->shared->version = CLIENT_SURFACE_HANDOFF_VERSION;
    pool->shared->channel_count = CLIENT_SURFACE_HANDOFF_CHANNELS;
    __atomic_store_n( &pool->shared->magic, CLIENT_SURFACE_HANDOFF_MAGIC, __ATOMIC_RELEASE );
    list_add_tail( &client_surface_handoff_pools, &pool->entry );
    return pool;
}

struct client_surface_handoff_binding *client_surface_handoff_create(
    struct process *producer, struct process *consumer, struct object *owner,
    user_handle_t window, user_handle_t toplevel, UINT64 identity,
    void (*changed)( void *context ), void *context )
{
    struct client_surface_handoff_binding *binding;
    struct client_surface_handoff_pool *pool;
    struct client_surface_handoff_channel *channel;
    unsigned int i;

    assert( producer && consumer && owner && changed );
    if (client_surface_handoff_serial >= ~(UINT64)0 - 1)
    {
        set_error( STATUS_NO_MEMORY );
        return NULL;
    }
    LIST_FOR_EACH_ENTRY( pool, &client_surface_handoff_pools,
                         struct client_surface_handoff_pool, entry )
    {
        if (pool->producer != producer || pool->consumer != consumer) continue;
        for (i = 0; i < CLIENT_SURFACE_HANDOFF_CHANNELS; ++i)
            if (!pool->bindings[i].pool) goto found;
    }
    if (!(pool = create_client_surface_handoff_pool( producer, consumer ))) return NULL;
    i = 0;
found:
    binding = &pool->bindings[i];
    binding->pool = pool;
    binding->owner = grab_object( owner );
    binding->index = i;
    binding->cookie = ++client_surface_handoff_serial;
    binding->changed = changed;
    binding->context = context;
    __atomic_fetch_and( &pool->shared->ready_bitmap[i / 64],
                        ~((UINT64)1 << (i % 64)), __ATOMIC_ACQ_REL );
    channel = &pool->shared->channels[i];
    memset( channel, 0, sizeof(*channel) );
    channel->cookie = binding->cookie;
    channel->identity = identity;
    channel->producer_process = producer->id;
    channel->window = window;
    channel->toplevel = toplevel;
    return binding;
}

obj_handle_t client_surface_handoff_map( struct client_surface_handoff_binding *binding,
    struct process *process, int producer, struct client_surface_handoff_mapping *mapping )
{
    struct client_surface_handoff_pool *pool = binding->pool;
    struct client_surface_handoff_channel *channel = &pool->shared->channels[binding->index];
    unsigned int endpoint = producer ? CLIENT_SURFACE_HANDOFF_ENDPOINT_PRODUCER :
                                      CLIENT_SURFACE_HANDOFF_ENDPOINT_CONSUMER;
    obj_handle_t handle;

    if (!(handle = alloc_handle( process, pool->mapping, SECTION_MAP_READ | SECTION_MAP_WRITE, 0 ))) return 0;
    binding->mapped |= endpoint;
    __atomic_fetch_or( &channel->endpoints, endpoint, __ATOMIC_RELEASE );
    mapping->id = pool->id;
    mapping->cookie = binding->cookie;
    mapping->size = sizeof(*pool->shared);
    mapping->offset = (char *)channel - (char *)pool->shared;
    return handle;
}

obj_handle_t client_surface_handoff_get_event( const struct client_surface_handoff_binding *binding,
    struct process *process, int producer )
{
    return alloc_handle( process, producer ? binding->pool->ready_write : binding->pool->ready_read,
                         producer ? FILE_GENERIC_WRITE : FILE_GENERIC_READ, 0 );
}

void client_surface_handoff_release_endpoint( struct client_surface_handoff_binding *binding, int producer )
{
    struct client_surface_handoff_channel *channel = &binding->pool->shared->channels[binding->index];
    unsigned int endpoint = producer ? CLIENT_SURFACE_HANDOFF_ENDPOINT_PRODUCER :
                                      CLIENT_SURFACE_HANDOFF_ENDPOINT_CONSUMER;

    binding->mapped &= ~endpoint;
    if (!producer)
    {
        binding->changed( binding->context );
        /* Normal hide/show retains its completed image and may reacquire the
         * channel. Replacement or failure closes it before the final wake. */
        if (binding->retired || __atomic_load_n( &channel->closed, __ATOMIC_ACQUIRE ))
            client_surface_handoff_close( binding );
    }
    __atomic_fetch_and( &channel->endpoints, ~(LONG)endpoint, __ATOMIC_RELEASE );
}

struct client_surface_handoff_binding *client_surface_handoff_cleanup_binding(
    struct client_surface_handoff_binding *binding, struct process *process )
{
    if (!binding || (binding->pool->producer != process && binding->pool->consumer != process)) return binding;
    client_surface_handoff_close( binding );
    binding->mapped = 0;
    client_surface_handoff_free( binding );
    return NULL;
}

void client_surface_handoff_cleanup_pools( struct process *process )
{
    struct client_surface_handoff_pool *pool, *next;
    unsigned int i;

    LIST_FOR_EACH_ENTRY_SAFE( pool, next, &client_surface_handoff_pools,
                              struct client_surface_handoff_pool, entry )
    {
        if (pool->producer != process && pool->consumer != process) continue;
        for (i = 0; i < CLIENT_SURFACE_HANDOFF_CHANNELS; ++i) assert( !pool->bindings[i].pool );
        list_remove( &pool->entry );
        release_shared_data_mapping( pool->mapping, pool->shared );
        release_object( pool->ready_write );
        release_object( pool->ready_read );
        free( pool );
    }
}
