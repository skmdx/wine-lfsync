/*
 * Immutable producer caches and owner scene composition
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
#include <unistd.h>

#include "x11drv.h"
#include "client_surface_query.h"
#include "client_surface_cache.h"
#include "wine/server.h"
#include "client_surface_compositor_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);
WINE_DECLARE_DEBUG_CHANNEL(csperf);

static void start_client_surface_cache_copy( struct client_surface_compositor_binding *binding,
                                             unsigned int depth );
static void complete_client_surface_output_transform( void *context, BOOL success );
static void complete_client_surface_output_transform_batch( void *context, BOOL success );
static void complete_client_surface_frame_copy( struct client_surface_compositor_target *target,
    struct client_surface_compositor_frame *frame, struct client_surface_compositor_binding *binding,
    UINT64 epoch, UINT64 sequence, const RECT *damage, BOOL success );
static BOOL client_surface_cached_frame_matches_layout( const struct client_surface_compositor_binding *binding,
                                                         const struct client_surface_scene_layout *layout );

struct client_surface_owner_copy
{
    struct client_surface_compositor_binding *binding;
    unsigned int buffer_index;
    UINT64 control, generation, epoch;
    UINT64 sequence;
    BOOL replay;
};

struct client_surface_copy_batch
{
    struct client_surface_compositor_target *target;
    struct client_surface_compositor_frame *frame;
    struct client_surface_output_transform *transform, *tail;
    struct client_surface_owner_copy copies[CLIENT_SURFACE_COPY_BATCH_SIZE];
    unsigned int count;
    int error;
    UINT64 revision;
};

/* Owner-local placement of an immutable completed image. Source damage may
 * become a full copy for scene replay without changing the cached frame. */
struct client_surface_composition_plan
{
    UINT64 generation, publication_generation;
    UINT64 epoch;
    BOOL steady;
    RECT destination;
    RECT source_damage;
    const RGNDATA *clip;
};

static BOOL client_surface_cache_read_pending( const struct client_surface_compositor_binding *binding )
{
    return binding->cache_copy.query_pending || binding->cache_copy.native_pending;
}

static void trace_client_surface_source( const char *event,
    const struct client_surface_compositor_binding *binding, UINT64 control, UINT64 sequence,
    Window window, Pixmap pixmap, BOOL success )
{
    TRACE_(csperf)( "ticks=%llu event=%s identity=%s cookie=%s token=%s sequence=%s "
                   "window=%lx pixmap=%lx success=%u\n", client_surface_perf_time(), event,
                   wine_dbgstr_longlong( binding->identity ), wine_dbgstr_longlong( binding->cookie ),
                   wine_dbgstr_longlong( control ),
                   wine_dbgstr_longlong( sequence ), window, pixmap, success );
}

/* A queued native request is not a committed source checkpoint. Newer cache
 * reception may race an asynchronous copy, and must keep its replay dirty. */
static void note_client_surface_source_copy( struct client_surface_compositor_binding *binding,
                                             UINT64 epoch, UINT64 sequence, UINT64 replay_generation )
{
    binding->source_epoch = epoch;
    binding->source_sequence = sequence;
    if (binding->latest_frame.source_sequence == sequence)
    {
        binding->replay_epoch = epoch;
        binding->replay_generation = replay_generation;
    }
}

void reset_client_surface_owner_repair( struct client_surface_owner_repair *repair )
{
    client_surface_free_owned_array( repair->receipts );
    memset( repair, 0, sizeof(*repair) );
}

static struct client_surface_compositor_pool *client_surface_compositor_pools;
static struct client_surface_compositor_pool *client_surface_compositor_next_pool;
static unsigned int client_surface_compositor_pool_count;
static UINT64 client_surface_compositor_pool_generation;
static UINT64 client_surface_compositor_query_generation;

static int compare_client_surface_compositor_binding( const void *key, const struct rb_entry *entry )
{
    const struct client_surface_binding_key *lookup = key;
    const struct client_surface_compositor_binding *binding =
        CONTAINING_RECORD( entry, const struct client_surface_compositor_binding, registry_entry );
    ULONG_PTR a = (ULONG_PTR)lookup->toplevel, b = (ULONG_PTR)binding->toplevel;

    if (a != b) return (a > b) - (a < b);
    if (lookup->process != binding->process) return (lookup->process > binding->process) -
                                                  (lookup->process < binding->process);
    return (lookup->identity > binding->identity) - (lookup->identity < binding->identity);
}

/* Bindings can precede their output target. Keep the actor-owned registry
 * independent of target lifetime, with each owner's entries contiguous. */
static struct rb_tree client_surface_compositor_bindings = {compare_client_surface_compositor_binding};

static struct client_surface_compositor_binding *find_client_surface_compositor_binding(
    HWND toplevel, process_id_t process, UINT64 identity )
{
    struct client_surface_binding_key key = {toplevel, process, identity};
    struct rb_entry *entry = rb_get( &client_surface_compositor_bindings, &key );

    return entry ? CONTAINING_RECORD( entry, struct client_surface_compositor_binding, registry_entry ) : NULL;
}

static struct client_surface_compositor_binding *first_client_surface_compositor_binding( HWND toplevel )
{
    struct rb_entry *entry = client_surface_compositor_bindings.root;
    struct client_surface_compositor_binding *first = NULL;

    while (entry)
    {
        struct client_surface_compositor_binding *binding =
            CONTAINING_RECORD( entry, struct client_surface_compositor_binding, registry_entry );

        if ((ULONG_PTR)binding->toplevel >= (ULONG_PTR)toplevel)
        {
            first = binding;
            entry = entry->left;
        }
        else entry = entry->right;
    }
    return first && first->toplevel == toplevel ? first : NULL;
}

struct client_surface_compositor_binding *next_client_surface_compositor_binding(
    struct client_surface_compositor_binding *binding )
{
    struct rb_entry *entry = rb_next( &binding->registry_entry );
    struct client_surface_compositor_binding *next;

    if (!entry) return NULL;
    next = CONTAINING_RECORD( entry, struct client_surface_compositor_binding, registry_entry );
    return next->toplevel == binding->toplevel ? next : NULL;
}
/* Repair can start in cache completion, handoff replay or a queued job.
 * All of those actor paths share this interval's receipt budget. */
static unsigned int client_surface_compositor_repair_budget;
static BOOL client_surface_compositor_repair_pending;

static struct client_surface_copy_batch client_surface_copy_batch;

static struct client_surface_compositor_pool *find_client_surface_compositor_pool( UINT64 id )
{
    struct client_surface_compositor_pool *pool;

    for (pool = client_surface_compositor_pools; pool; pool = pool->next)
        if (pool->id == id) return pool;
    return NULL;
}

static void release_client_surface_compositor_binding_server(
    const struct client_surface_compositor_binding *binding )
{
    SERVER_START_REQ( release_client_surface_handoff )
    {
        req->handle = wine_server_user_handle( binding->toplevel );
        req->producer = binding->process;
        req->surface = binding->identity;
        req->cookie = binding->cookie;
        req->owner = 1;
        wine_server_call( req );
    }
    SERVER_END_REQ;
}

static void release_client_surface_compositor_pool( struct client_surface_compositor_pool *pool )
{
    assert( pool->refs );
    if (--pool->refs) return;
    *pool->prev = pool->next;
    if (pool->next) pool->next->prev = pool->prev;
    if (client_surface_compositor_next_pool == pool)
        client_surface_compositor_next_pool = pool->next ? pool->next : client_surface_compositor_pools;
    --client_surface_compositor_pool_count;
    ++client_surface_compositor_pool_generation;
    NtUnmapViewOfSection( NtCurrentProcess(), pool->shared );
    close( pool->ready_fd );
    client_surface_free_owned_metadata( &pool->memory, pool, sizeof(*pool) );
}

static void free_client_surface_cached_image( struct client_surface_cached_image *image )
{
    client_surface_cache_release( image->storage );
    memset( image, 0, sizeof(*image) );
}

static void free_client_surface_compositor_binding( struct client_surface_compositor_binding *binding )
{
    unsigned int i;

    assert( binding->retired && !client_surface_cache_read_pending( binding ) );
    /* Releasing the consumer endpoint also permits the producer to retire
     * unacknowledged slots. Keep it, the mapping and both cache images until
     * our last native read and checked reply have completed. */
    release_client_surface_compositor_binding_server( binding );
    release_client_surface_compositor_pool( binding->pool );
    free_client_surface_cached_image( &binding->latest_image );
    free_client_surface_cached_image( &binding->spare_image );
    for (i = 0; i < ARRAY_SIZE(binding->retained_images); ++i)
        free_client_surface_cached_image( &binding->retained_images[i] );
    client_surface_free_owned_metadata( &binding->memory, binding, sizeof(*binding) );
}

static void remove_client_surface_compositor_binding( struct client_surface_compositor_binding *binding )
{
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( binding->toplevel );
    unsigned int index = binding->channel - binding->pool->shared->channels;

    rb_remove( &client_surface_compositor_bindings, &binding->registry_entry );
    if (binding->pool->bindings[index] == binding) binding->pool->bindings[index] = NULL;
    binding->pool->query_wait_bitmap[index / 64] &= ~((UINT64)1 << (index % 64));
    if (target)
    {
        ++target->binding_generation;
        detach_client_surface_output_transform( target );
        finish_client_surface_compositor_assembly( target, TRUE );
        target->scene.valid = FALSE;
    }
    /* Acknowledged descriptors refer to this binding's cached images. Once
     * the cache is discarded, a new consumer needs a new channel and frame;
     * it cannot resume at the old consumer sequence without those images. */
    __atomic_store_n( &binding->channel->closed, 1, __ATOMIC_RELEASE );
    binding->retired = TRUE;
    release_client_surface_compositor_queue( binding->queue );
    binding->queue = NULL;
    TRACE_(csperf)( "ticks=%llu event=cache_detach identity=%s cookie=%s query_pending=%u native_pending=%u token=%s "
                   "consumed=%s produced=%s endpoints=%u\n", client_surface_perf_time(),
                   wine_dbgstr_longlong( binding->identity ), wine_dbgstr_longlong( binding->cookie ),
                   binding->cache_copy.query_pending,
                   binding->cache_copy.native_pending,
                   wine_dbgstr_longlong( binding->cache_copy.control ),
                   wine_dbgstr_longlong( __atomic_load_n( &binding->channel->consumer_sequence, __ATOMIC_ACQUIRE ) ),
                   wine_dbgstr_longlong( __atomic_load_n( &binding->channel->producer_sequence, __ATOMIC_ACQUIRE ) ),
                   __atomic_load_n( &binding->channel->endpoints, __ATOMIC_ACQUIRE ) );
    if (!client_surface_cache_read_pending( binding )) free_client_surface_compositor_binding( binding );
}

static struct client_surface_compositor_pool *acquire_client_surface_compositor_pool(
    struct client_surface_compositor_job *job )
{
    struct client_surface_handoff_shared *shared;
    struct client_surface_compositor_pool *pool;
    struct client_surface_memory_scope memory = {0};
    SIZE_T size = job->u.registration.view_size;
    void *view = NULL;

    if ((pool = find_client_surface_compositor_pool( job->u.registration.mapping_id )))
    {
        size = pool->size;
        view = pool->shared;
    }
    else
    {
        /* The job's caller keeps its section handle until this synchronous job
         * returns. Only the first binding maps a pool; later registrations use
         * this connection's retained view, including across scene changes. */
        if (NtMapViewOfSection( job->u.registration.mapping, NtCurrentProcess(), &view, 0, 0, NULL,
                               &size, ViewShare, 0, PAGE_READWRITE )) return NULL;
    }
    shared = view;

    if (size < sizeof(*shared) ||
        __atomic_load_n( &shared->magic, __ATOMIC_ACQUIRE ) != CLIENT_SURFACE_HANDOFF_MAGIC ||
        shared->version != CLIENT_SURFACE_HANDOFF_VERSION ||
        shared->channel_count != CLIENT_SURFACE_HANDOFF_CHANNELS ||
        shared->mapping_id != job->u.registration.mapping_id)
        goto failed;
    if (pool)
    {
        ++pool->refs;
        return pool;
    }
    /* A shared mapping can serve several owners on this connection. */
    if (!(pool = alloc_client_surface_compositor_metadata( NULL, sizeof(*pool), &memory ))) goto failed;
    pool->memory = memory;
    pool->next = client_surface_compositor_pools;
    pool->prev = &client_surface_compositor_pools;
    if (pool->next) pool->next->prev = &pool->next;
    pool->shared = shared;
    pool->id = job->u.registration.mapping_id;
    pool->size = size;
    pool->refs = 1;
    pool->ready_fd = job->u.registration.ready_fd;
    job->u.registration.ready_fd = -1;
    client_surface_compositor_pools = pool;
    if (!client_surface_compositor_next_pool) client_surface_compositor_next_pool = pool;
    ++client_surface_compositor_pool_count;
    ++client_surface_compositor_pool_generation;
    return pool;

failed:
    if (!pool) NtUnmapViewOfSection( NtCurrentProcess(), view );
    return NULL;
}

static BOOL client_surface_compositor_binding_is_live( const struct client_surface_compositor_binding *binding )
{
    return !__atomic_load_n( &binding->channel->closed, __ATOMIC_ACQUIRE ) &&
           (__atomic_load_n( &binding->channel->endpoints, __ATOMIC_ACQUIRE ) &
            CLIENT_SURFACE_HANDOFF_ENDPOINT_CONSUMER);
}

BOOL register_client_surface_compositor_handoff(
    struct client_surface_compositor_job *job )
{
    struct client_surface_compositor_binding *binding;
    struct client_surface_binding_key key = {job->toplevel, job->u.registration.process, job->u.registration.identity};
    struct client_surface_memory_scope memory = {0};
    struct client_surface_compositor_pool *pool;
    struct client_surface_handoff_channel *channel;
    BOOL ret = FALSE;

    if (!(pool = acquire_client_surface_compositor_pool( job ))) goto done;
    if (job->u.registration.offset < offsetof(struct client_surface_handoff_shared, channels) ||
        job->u.registration.offset > sizeof(*pool->shared) - sizeof(*channel) ||
        (job->u.registration.offset - offsetof(struct client_surface_handoff_shared, channels)) % sizeof(*channel)) goto done;
    channel = (struct client_surface_handoff_channel *)((char *)pool->shared + job->u.registration.offset);
    if (channel->cookie != job->u.registration.cookie || channel->identity != job->u.registration.identity ||
        channel->producer_process != job->u.registration.process ||
        channel->window != wine_server_user_handle( job->u.registration.window ) ||
        channel->toplevel != wine_server_user_handle( job->toplevel ) ||
        __atomic_load_n( &channel->closed, __ATOMIC_ACQUIRE )) goto done;

    if ((binding = find_client_surface_compositor_binding( key.toplevel, key.process, key.identity )))
    {
        if (binding->cookie == job->u.registration.cookie)
        {
            binding->mark = job->u.registration.mark;
            ret = TRUE;
            goto done;
        }
        /* Keep the acquired pool alive if this was its last old binding. */
        remove_client_surface_compositor_binding( binding );
    }

    if (!(binding = alloc_client_surface_compositor_metadata( job->toplevel, sizeof(*binding), &memory ))) goto done;
    binding->memory = memory;
    /* A targetless binding still needs an admitted removal slot. */
    binding->queue = get_client_surface_compositor_queue( job->toplevel );
    assert( binding->queue == job->queue );
    binding->pool = pool;
    binding->channel = channel;
    pool->bindings[channel - pool->shared->channels] = binding;
    binding->toplevel = job->toplevel;
    binding->window = job->u.registration.window;
    binding->process = job->u.registration.process;
    binding->identity = job->u.registration.identity;
    binding->cookie = job->u.registration.cookie;
    binding->mark = job->u.registration.mark;
    pool->refs++;
    rb_put( &client_surface_compositor_bindings, &key, &binding->registry_entry );
    {
        struct client_surface_compositor_target *target = find_client_surface_compositor_target( job->toplevel );

        if (target) ++target->binding_generation;
    }
    TRACE( "registered handoff hwnd %p identity %s producer %04x pool %s cookie %s\n",
           binding->window, wine_dbgstr_longlong( binding->identity ), binding->process,
           wine_dbgstr_longlong( pool->id ), wine_dbgstr_longlong( binding->cookie ) );
    ret = TRUE;

done:
    if (pool) release_client_surface_compositor_pool( pool );
    return ret;
}

void update_client_surface_compositor_scene(
    struct client_surface_compositor_target *target, UINT64 scene_epoch )
{
    unsigned int i;

    if (target->scene.epoch == scene_epoch) return;
    finish_client_surface_compositor_assembly( target, TRUE );
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
    {
        struct client_surface_compositor_frame *frame = &target->frames[i];

        if (!frame->publish_pending || frame->publish_epoch == scene_epoch) continue;
        publish_client_surface_handoff_generation( target->toplevel,
            frame->publish_generation, frame->publish_epoch, FALSE );
        frame->publish_pending = FALSE;
    }
    if (target->mailbox_pending && target->mailbox_publish_generation &&
        target->mailbox_publish_epoch != scene_epoch)
    {
        publish_client_surface_handoff_generation( target->toplevel,
            target->mailbox_publish_generation, target->mailbox_publish_epoch, FALSE );
        target->mailbox_pending = FALSE;
        target->mailbox_publish_generation = 0;
        target->mailbox_publish_epoch = 0;
    }
    target->scene.epoch = scene_epoch;
}

int compare_client_surface_scene_layouts( const void *a, const void *b )
{
    const struct client_surface_scene_layout *left = a, *right = b;
    user_handle_t l = wine_server_user_handle( left->window ), r = wine_server_user_handle( right->window );

    return (l > r) - (l < r);
}

int compare_client_surface_handoff_descs( const void *a, const void *b )
{
    const struct client_surface_handoff_desc *left = a, *right = b;

    return (left->handle > right->handle) - (left->handle < right->handle);
}

struct client_surface_compositor_binding *next_client_surface_job_binding(
    const struct client_surface_compositor_job *job )
{
    struct client_surface_binding_key key = {job->toplevel, job->scan.process, job->scan.identity};
    struct rb_entry *entry = client_surface_compositor_bindings.root, *next = NULL;
    struct client_surface_compositor_binding *binding;

    if (!job->scan.cursor_set) return first_client_surface_compositor_binding( job->toplevel );
    while (entry)
    {
        if (compare_client_surface_compositor_binding( &key, entry ) < 0)
        {
            next = entry;
            entry = entry->left;
        }
        else entry = entry->right;
    }
    if (!next) return NULL;
    binding = CONTAINING_RECORD( next, struct client_surface_compositor_binding, registry_entry );
    return binding->toplevel == job->toplevel ? binding : NULL;
}

void advance_client_surface_job_binding( struct client_surface_compositor_job *job,
                                                const struct client_surface_compositor_binding *binding )
{
    job->scan.cursor_set = TRUE;
    job->scan.process = binding->process;
    job->scan.identity = binding->identity;
}

/* Return completion separately from the operation's result. A yielded head
 * stays runnable and retains FIFO order; native waits still park it. */
BOOL reuse_client_surface_compositor_handoffs( struct client_surface_compositor_job *job,
                                                      unsigned int *budget )
{
    struct client_surface_compositor_binding *binding;

    while (job->scan.index < job->u.reuse.count && *budget)
    {
        unsigned int i = job->scan.index++;
        const struct client_surface_handoff_desc *desc = &job->u.reuse.handoffs[i];

        --*budget;
        job->u.reuse.reused[i] = FALSE;
        binding = find_client_surface_compositor_binding( job->toplevel, desc->process, desc->surface );
        if (!binding || wine_server_user_handle( binding->window ) != desc->handle) continue;
        if (binding->cookie != desc->cookie)
        {
            /* A -> B -> A can leave the old consumer mapped although
             * the server has retired its cookie. Pending source reads keep
             * their endpoint through the existing retirement path. */
            remove_client_surface_compositor_binding( binding );
            continue;
        }
        if ((job->u.reuse.reused[i] = client_surface_compositor_binding_is_live( binding )))
            binding->mark = job->u.reuse.mark;
    }
    if (job->scan.index < job->u.reuse.count) return FALSE;
    job->result = TRUE;
    return TRUE;
}

void free_client_surface_scene_layouts( struct client_surface_scene_layout *layouts )
{
    /* Layouts and their immutable clips share one caller-built allocation. */
    client_surface_free_owned_array( layouts );
}

void free_client_surface_scene_plan( struct client_surface_compositor_target *target )
{
    reset_client_surface_owner_repair( &target->repair );
    detach_client_surface_output_transform( target );
    x11drv_native_window_release( target->scene.direct_owner );
    target->scene.direct_owner = NULL;
    client_surface_free_owned_array( target->receipts );
    client_surface_free_owned_array( target->scene.members );
    free_client_surface_scene_layouts( target->scene.layouts );
    target->receipts = NULL;
    target->scene.members = NULL;
    target->scene.layouts = NULL;
    target->scene.count = 0;
    target->scene.valid = FALSE;
}

BOOL check_client_surface_compositor_scene( struct client_surface_compositor_job *job,
                                                   unsigned int *budget )
{
    struct client_surface_compositor_target *target =
        find_client_surface_compositor_target( job->toplevel );
    struct client_surface_compositor_binding *binding;

    job->result = FALSE;
    if (!target || !target->scene.valid || target->scene.strategy != OWNER_COMPOSITE ||
        target->scene.epoch != job->u.scene_check.epoch) return TRUE;
    if (!job->scan.phase)
    {
        job->scan.generation = target->binding_generation;
        job->scan.phase = 1;
    }
    if (job->scan.generation != target->binding_generation) return TRUE;
    while (job->scan.index < job->u.scene_check.count && *budget)
    {
        const struct client_surface_handoff_desc *desc = &job->u.scene_check.handoffs[job->scan.index++];

        --*budget;
        job->scan.count += !!desc->visible;
        job->scan.needed += !!(desc->visible || desc->producer_mapped);
    }
    if (job->scan.index < job->u.scene_check.count) return FALSE;
    if (target->scene.count != job->scan.count) return TRUE;
    binding = next_client_surface_job_binding( job );
    while (binding && *budget)
    {
        const struct client_surface_handoff_desc *desc;
        unsigned int low = 0, high = job->u.scene_check.count;

        --*budget;
        advance_client_surface_job_binding( job, binding );
        /* The caller sorted this authoritative roster. Validate retained
         * hidden bindings too; a changed producer or retired cookie must not
         * survive merely because the visible scene is unchanged. */
        while (low < high)
        {
            unsigned int mid = low + (high - low) / 2;

            if (job->u.scene_check.handoffs[mid].handle < wine_server_user_handle( binding->window )) low = mid + 1;
            else high = mid;
        }
        if (low == job->u.scene_check.count) return TRUE;
        desc = &job->u.scene_check.handoffs[low];
        if (wine_server_user_handle( binding->window ) != desc->handle ||
            binding->process != desc->process || binding->identity != desc->surface ||
            binding->cookie != desc->cookie || !client_surface_compositor_binding_is_live( binding )) return TRUE;
        job->scan.bound += !!(desc->visible || desc->producer_mapped);
        if (desc->visible)
        {
            if (binding->scene_index >= target->scene.count ||
                target->scene.members[binding->scene_index] != binding) return TRUE;
            ++job->scan.found;
        }
        binding = next_client_surface_compositor_binding( binding );
    }
    if (binding) return FALSE;
    job->result = job->scan.found == job->scan.count && job->scan.bound == job->scan.needed;
    return TRUE;
}

BOOL install_client_surface_scene_plan( struct client_surface_compositor_target *target,
                                               struct client_surface_compositor_job *job,
                                               unsigned int *budget )
{
    struct client_surface_compositor_binding *binding;
    unsigned int count = job->u.scene_install.count;

    job->result = FALSE;
    if (job->scan.phase == 1)
    {
        if (count)
        {
            if (!(job->scan.members = client_surface_alloc_owned_array( &target->memory, count,
                                                                       sizeof(*job->scan.members) ))) return TRUE;
            if (!(job->scan.receipts = client_surface_alloc_owned_array( &target->memory, count,
                                                                        sizeof(*job->scan.receipts) ))) return TRUE;
        }
        /* No old plan can observe partially assigned member indices. A
         * closing handoff can still remove a binding while this job yields;
         * its generation invalidates the borrowed pointers before adoption. */
        finish_client_surface_compositor_assembly( target, TRUE );
        free_client_surface_scene_plan( target );
        job->scan.generation = target->binding_generation;
        job->scan.phase = 2;
    }
    if (job->scan.generation != target->binding_generation) return TRUE;
    while (job->scan.index < count && *budget)
    {
        unsigned int i = job->scan.index++;
        const struct client_surface_scene_layout *layout = &job->u.scene_install.layouts[i];

        --*budget;
        binding = find_client_surface_compositor_binding( target->toplevel, layout->process, layout->identity );
        if (!binding || binding->window != layout->window) return TRUE;
        job->scan.members[i] = binding;
        binding->scene_index = i;
        TRACE( "owner scene member hwnd %p epoch %s destination %s clip count %lu\n",
               binding->window, wine_dbgstr_longlong( job->u.scene_install.epoch ),
               wine_dbgstr_rect( &layout->geometry.monitor_rect ), (unsigned long)layout->clip->rdh.nCount );
    }
    if (job->scan.index < count) return FALSE;
    if (!client_surface_scene_snapshot_current( target->toplevel, job->u.scene_install.epoch )) return TRUE;
    update_client_surface_compositor_scene( target, job->u.scene_install.epoch );
    target->scene.members = job->scan.members;
    job->scan.members = NULL;
    target->scene.strategy = OWNER_COMPOSITE;
    target->scene.direct_identity = 0;
    target->scene.direct_drawable = None;
    target->scene.layouts = job->u.scene_install.layouts;
    job->u.scene_install.layouts = NULL;
    job->u.scene_install.count = 0;
    target->scene.count = count;
    target->scene.valid = TRUE;
    update_client_surface_notification_plan( target );
    target->quiescing = target->native_updates || target->deferred_update;
    target->receipts = job->scan.receipts;
    job->scan.receipts = NULL;
    target->replay_member = 0;
    retry_client_surface_compositor_mailbox( target );
    job->result = TRUE;
    return TRUE;
}

BOOL sweep_client_surface_compositor_handoffs( struct client_surface_compositor_job *job,
                                                       UINT64 mark, unsigned int *budget )
{
    struct client_surface_compositor_binding *binding, *next;

    binding = next_client_surface_job_binding( job );
    while (binding && *budget)
    {
        --*budget;
        advance_client_surface_job_binding( job, binding );
        next = next_client_surface_compositor_binding( binding );
        if (binding->mark != mark)
            remove_client_surface_compositor_binding( binding );
        binding = next;
    }
    return !binding;
}

BOOL client_surface_compositor_pool_retirable( const struct client_surface_compositor_target *target )
{
    struct client_surface_scene scene;

    return target && target->scene.valid && target->scene.strategy == DIRECT_ATTACH &&
           client_surface_get_toplevel_scene( target->toplevel, &scene ) && !scene.generation &&
           scene.epoch == target->scene.epoch && scene.mode == CLIENT_SURFACE_PRESENTATION_DIRECT;
}

/* The logical owner and its immutable plan outlive a retired output pool.
 * Native attachment does not use any of these old copy/Present resources. */
BOOL retire_client_surface_compositor_pool( HWND toplevel )
{
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( toplevel );
    unsigned int i;

    if (!client_surface_compositor_pool_retirable( target )) return FALSE;
    drain_client_surface_compositor_target( target );
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
    {
        if (i < 2) set_client_surface_compositor_pixmap( &target->frames[i], 0, NULL );
    }
    free_client_surface_compositor_mailbox( target );
    memset( target->frames, 0, sizeof(target->frames) );
    target->backing = target->latest = target->published = None;
    target->published_width = target->published_height = 0;
    SetRectEmpty( &target->restore_rect );
    return TRUE;
}

static BOOL client_surface_source_cache_matches( const struct client_surface_source_cache *cache,
                                                 const struct client_surface_handoff_slot *slot )
{
    return cache->pixmap == slot->source && cache->target_epoch == slot->target_epoch &&
           cache->width == slot->width && cache->height == slot->height && cache->visual == slot->source_visual;
}

static BOOL get_client_surface_compositor_source(
    struct client_surface_compositor_binding *binding,
    const struct client_surface_handoff_slot *slot, Pixmap *source,
    unsigned int *source_depth )
{
    struct client_surface_source_cache *cache;
    unsigned int i;

    if (!(slot->flags & CLIENT_SURFACE_HANDOFF_COPY_SOURCE)) return FALSE;
    /* Cache validated metadata, without owning or retaining the producer's
     * XID. The slot pins this independent image through copy completion, and
     * every actual read still participates in the X error validation boundary. */
    /* Transport slots and native images rotate independently. In particular,
     * three source images through a four-slot ring never match the previous
     * image at the same ring position. Reuse metadata by its exact native
     * tuple within this binding; the ring index only selects a miss victim. */
    for (i = 0; i < ARRAY_SIZE(binding->sources); ++i)
        if (client_surface_source_cache_matches( &binding->sources[i], slot )) break;
    if (i == ARRAY_SIZE(binding->sources)) return FALSE;
    cache = &binding->sources[i];
    *source = cache->pixmap;
    *source_depth = cache->depth;
    return TRUE;
}

static void release_client_surface_cached_source( struct client_surface_compositor_binding *binding,
                                                  BOOL success )
{
    const struct client_surface_cache_copy *copy = &binding->cache_copy;
    struct client_surface_handoff_channel *channel = binding->channel;
    unsigned int index = channel - binding->pool->shared->channels;

    trace_client_surface_source( "cache_copy", binding, copy->control, copy->frame.source_sequence,
                                 0, binding->latest_image.pixmap, success );
    /* Descriptor publication pins the independent producer image. Sending a
     * request is not permission to return it; this runs after the checked
     * read, or after rejecting a descriptor without submitting any read. */
    __atomic_store_n( &channel->consumer_sequence, copy->control, __ATOMIC_RELEASE );
    if (!success && !binding->latest_image.pixmap)
        __atomic_store_n( &channel->closed, 1, __ATOMIC_RELEASE );
    trace_client_surface_source( "cache_release", binding, copy->control, copy->frame.source_sequence,
                                 0, binding->latest_image.pixmap, success );
    TRACE( "%s handoff hwnd %p identity %s sequence %s after owner cache copy\n",
           success || binding->latest_image.pixmap ? "released" : "lost", binding->window,
           wine_dbgstr_longlong( binding->identity ), wine_dbgstr_longlong( copy->control ) );
    client_surface_handoff_wake_release( binding->pool->shared );
    if (binding->retired)
    {
        free_client_surface_compositor_binding( binding );
        return;
    }
    if (success)
    {
        struct client_surface_compositor_target *target = find_client_surface_compositor_target( binding->toplevel );

        binding->replay_epoch = 0;
        /* A new completed source also retries preceding dirty scene members.
         * No target pointer is borrowed across this native copy. */
        if (target)
        {
            target->replay_member = 0;
            retry_client_surface_compositor_mailbox( target );
        }
    }
    /* A producer may have filled the ring while the read was pending. Its
     * hint was consumed then; recheck the authoritative sequence after the
     * completion without requiring another producer notification. */
    __atomic_fetch_or( &binding->pool->shared->ready_bitmap[index / 64],
                       (UINT64)1 << (index % 64), __ATOMIC_RELEASE );
}

static void finish_client_surface_cache_copy( struct client_surface_compositor_binding *binding, BOOL success )
{
    struct client_surface_cache_copy *copy = &binding->cache_copy;

    if (success && !binding->retired)
    {
        struct client_surface_cached_image previous = binding->latest_image;
        struct client_surface_compositor_target *target = find_client_surface_compositor_target( binding->toplevel );
        const struct client_surface_scene_layout *layout = NULL;
        BOOL matched = FALSE;

        if (target && target->scene.valid && binding->scene_index < target->scene.count &&
            target->scene.members[binding->scene_index] == binding)
        {
            layout = &target->scene.layouts[binding->scene_index];
            matched = client_surface_cached_frame_matches_layout( binding, layout );
        }

        binding->latest_image = binding->spare_image;
        binding->spare_image = previous;
        binding->latest_frame = copy->frame;
        binding->latest_frame.source = binding->latest_image.pixmap;
        binding->latest_control = copy->control;
        binding->latest_index = copy->index;
        if (target)
        {
            ++target->cache_generation;
            /* Inventory receipts prove a usable cache, not a claim on its
             * producer ring slot. Compatible newer images preserve that
             * proof, so a continuously rendering producer cannot restart
             * the whole collection on every completed frame. */
            if (layout && matched != client_surface_cached_frame_matches_layout( binding, layout ))
                ++target->inventory_generation;
        }
    }
    release_client_surface_cached_source( binding, success && !binding->retired );
}

static void complete_client_surface_cache_read( struct client_surface_compositor_binding *binding,
                                                BOOL success, BOOL async )
{
    struct client_surface_cache_copy *copy = &binding->cache_copy;
    struct client_surface_cached_image *image = &binding->spare_image;
    struct client_surface_compositor_target *target;
    BOOL retired = binding->retired;
    unsigned int budget = 1;

    TRACE_(csperf)( "ticks=%llu event=cache_native_copy identity=%s cookie=%s token=%s sequence=%s "
                   "source=%lx destination=%lx width=%u height=%u depth=%u pixel_bits=%u copied=1 error=%u "
                   "sync_calls=%u async=%u elapsed=%s retired=%u\n",
                   client_surface_perf_time(), wine_dbgstr_longlong( binding->identity ),
                   wine_dbgstr_longlong( binding->cookie ), wine_dbgstr_longlong( copy->control ),
                   wine_dbgstr_longlong( copy->frame.source_sequence ), (Pixmap)copy->frame.source, image->pixmap,
                   image->width, image->height, image->depth,
                   pixmap_formats[image->depth] ? pixmap_formats[image->depth]->bits_per_pixel : 0, !success, !async, async,
                   wine_dbgstr_longlong( client_surface_perf_time() - copy->started ), binding->retired );
    if (!success) free_client_surface_cached_image( image );
    finish_client_surface_cache_copy( binding, success );
    /* Give an accepted cache one bounded output opportunity before another
     * READY can start replacing it. Waiting for a newer read below must not
     * starve output under a continuous producer. Use the scene's replay order,
     * including its assembly obligations, rather than composing out of order. */
    if (success && !retired && (target = find_client_surface_compositor_target( binding->toplevel )))
        replay_client_surface_scene_sources( target, &budget );
}

static void complete_client_surface_cache_creation( void *context, BOOL success )
{
    struct client_surface_compositor_binding *binding = context;
    struct client_surface_cached_image *image = &binding->spare_image;

    assert( binding->cache_copy.native_pending );
    binding->cache_copy.native_pending = FALSE;
    TRACE_(csperf)( "ticks=%llu event=cache_image_complete identity=%s cookie=%s token=%s image=%p "
                   "success=%u retired=%u\n", client_surface_perf_time(),
                   wine_dbgstr_longlong( binding->identity ), wine_dbgstr_longlong( binding->cookie ),
                   wine_dbgstr_longlong( binding->cache_copy.control ), image->storage, success, binding->retired );
    if (!success || binding->retired)
    {
        free_client_surface_cached_image( image );
        release_client_surface_cached_source( binding, FALSE );
        return;
    }
    image->pixmap = client_surface_cache_pixmap( image->storage );
    start_client_surface_cache_copy( binding, image->depth );
}

static void complete_client_surface_cache_fallback( void *context, BOOL success )
{
    struct client_surface_compositor_binding *binding = context;
    struct client_surface_cache_copy *copy = &binding->cache_copy;

    assert( copy->native_pending );
    copy->native_pending = FALSE;
    complete_client_surface_cache_read( binding, success, FALSE );
}

static void start_client_surface_cache_copy( struct client_surface_compositor_binding *binding,
                                             unsigned int depth )
{
    struct client_surface_cached_image *image = &binding->spare_image;
    struct client_surface_cache_copy *copy = &binding->cache_copy;
    struct client_surface_handoff_slot frame = copy->frame;
    Pixmap source = frame.source;
    UINT64 control = copy->control;

    if (image->storage && client_surface_cache_shared( image->storage ))
    {
        unsigned int i;

        /* Output frames may keep complete cache images instead of copying
         * them. Retain a bounded set until those readers return, rather than
         * continually destroying and reallocating their former spare. */
        for (i = 0; i < ARRAY_SIZE(binding->retained_images); ++i)
            if (!binding->retained_images[i].storage ||
                !client_surface_cache_shared( binding->retained_images[i].storage ))
            {
                struct client_surface_cached_image previous = *image;
                *image = binding->retained_images[i];
                binding->retained_images[i] = previous;
                break;
            }
    }

    assert( !binding->retired && !client_surface_cache_read_pending( binding ) );
    TRACE( "reading handoff hwnd %p identity %s sequence %s into owner cache\n", binding->window,
           wine_dbgstr_longlong( binding->identity ), wine_dbgstr_longlong( control ) );
    /* An old latest image can still be a scene output's source after swapping
     * into the spare. Both cross-connection reclaim and fallback writes must
     * wait for that read; retain its reference and admit a separate candidate. */
    if (!image->storage || client_surface_cache_shared( image->storage ) ||
        image->width != frame.width || image->height != frame.height || image->depth != depth)
    {
        UINT64 bytes = client_surface_pixmap_bytes( frame.width, frame.height, depth );

        free_client_surface_cached_image( image );
        image->storage = client_surface_cache_create( &binding->memory, frame.width, frame.height, depth, bytes,
            wake_client_surface_compositor, complete_client_surface_cache_creation, binding );
        if (!image->storage) goto done;
        image->width = frame.width;
        image->height = frame.height;
        image->depth = depth;
        copy->native_pending = TRUE;
        TRACE_(csperf)( "ticks=%llu event=cache_image_pending identity=%s cookie=%s token=%s image=%p operation=1\n",
                       client_surface_perf_time(), wine_dbgstr_longlong( binding->identity ),
                       wine_dbgstr_longlong( binding->cookie ), wine_dbgstr_longlong( control ), image->storage );
        return;
    }
    /* Keep the last complete cache intact until the new full image and its
     * error check succeed. Neither buffer borrows a producer XID. */
    copy->native_pending = TRUE;
    client_surface_cache_copy( image->storage, source, complete_client_surface_cache_fallback, binding );
    TRACE_(csperf)( "ticks=%llu event=cache_image_pending identity=%s cookie=%s token=%s image=%p operation=2\n",
                   client_surface_perf_time(), wine_dbgstr_longlong( binding->identity ),
                   wine_dbgstr_longlong( binding->cookie ), wine_dbgstr_longlong( control ), image->storage );
    return;
done:
    release_client_surface_cached_source( binding, FALSE );
}

static void complete_client_surface_source_query( struct client_surface_geometry_query *query )
{
    struct client_surface_compositor_binding *binding =
        CONTAINING_RECORD( query, struct client_surface_compositor_binding, cache_copy.query );
    struct client_surface_cache_copy *copy = &binding->cache_copy;
    const struct client_surface_handoff_slot *frame = &copy->frame;

    assert( copy->query_pending );
    copy->query_pending = FALSE;
    TRACE_(csperf)( "ticks=%llu event=cache_query_complete identity=%s cookie=%s token=%s query=%p "
                   "success=%u retired=%u\n", client_surface_perf_time(),
                   wine_dbgstr_longlong( binding->identity ), wine_dbgstr_longlong( binding->cookie ),
                   wine_dbgstr_longlong( copy->control ), query, query->success, binding->retired );
    if (binding->retired || !query->success)
    {
        release_client_surface_cached_source( binding, FALSE );
        return;
    }
    binding->sources[copy->index] = (struct client_surface_source_cache){frame->source,
        frame->target_epoch, frame->source_visual, frame->width, frame->height, query->depth};
    start_client_surface_cache_copy( binding, query->depth );
}

static BOOL cache_client_surface_handoff( struct client_surface_compositor_binding *binding,
                                          unsigned int index, UINT64 control )
{
    struct client_surface_handoff_slot frame = binding->channel->slots[index];
    struct client_surface_cache_copy *copy = &binding->cache_copy;
    enum client_surface_query_status status;
    unsigned int depth;
    Pixmap source;
    BOOL success = FALSE;

    assert( !binding->retired && !client_surface_cache_read_pending( binding ) );
    copy->frame = frame;
    copy->index = index;
    copy->control = control;
    copy->started = client_surface_perf_time();
    if (frame.cookie != binding->cookie || frame.identity != binding->identity ||
        frame.producer_process != binding->process ||
        frame.window != wine_server_user_handle( binding->window ) ||
        frame.toplevel != wine_server_user_handle( binding->toplevel ) ||
        !(frame.flags & CLIENT_SURFACE_HANDOFF_NATIVE_X11) ||
        !(frame.flags & CLIENT_SURFACE_HANDOFF_COPY_SOURCE) || !frame.width || !frame.height ||
        !frame.source_visual) goto rejected;
    if (binding->latest_image.pixmap && frame.source_sequence < binding->latest_frame.source_sequence)
    {
        success = TRUE;
        goto rejected;
    }
    if (get_client_surface_compositor_source( binding, &frame, &source, &depth ))
    {
        trace_client_surface_source( "claim", binding, control, frame.source_sequence, 0, frame.source, TRUE );
        start_client_surface_cache_copy( binding, depth );
        return TRUE;
    }
    copy->query = (struct client_surface_geometry_query){.pixmap = frame.source,
        .min_width = frame.width, .min_height = frame.height, .complete = complete_client_surface_source_query};
    status = client_surface_query_geometry( &copy->query, wake_client_surface_compositor );
    if (status == CLIENT_SURFACE_QUERY_FULL) return FALSE;
    if (status == CLIENT_SURFACE_QUERY_FAILED) goto rejected;
    copy->query_pending = TRUE;
    trace_client_surface_source( "claim", binding, control, frame.source_sequence, 0, frame.source, TRUE );
    TRACE_(csperf)( "ticks=%llu event=cache_query_pending identity=%s cookie=%s token=%s sequence=%s "
                   "query=%p source=%lx\n", client_surface_perf_time(),
                   wine_dbgstr_longlong( binding->identity ), wine_dbgstr_longlong( binding->cookie ),
                   wine_dbgstr_longlong( control ), wine_dbgstr_longlong( frame.source_sequence ),
                   &copy->query, (Pixmap)frame.source );
    return TRUE;
rejected:
    trace_client_surface_source( "claim", binding, control, frame.source_sequence, 0, frame.source, TRUE );
    release_client_surface_cached_source( binding, success );
    return TRUE;
}

static BOOL get_client_surface_compositor_catchup(
    const struct client_surface_compositor_target *target,
    const struct client_surface_compositor_frame *frame, RECT *rect )
{
    UINT64 revision;
    BOOL initialized = FALSE;

    if (frame->revision == target->revision) return TRUE;
    if (!frame->revision || frame->revision > target->revision ||
        target->revision - frame->revision > ARRAY_SIZE(target->damages))
    {
        *rect = (RECT){0, 0, target->window_width, target->window_height};
        return TRUE;
    }

    for (revision = frame->revision + 1; revision <= target->revision; ++revision)
    {
        const struct client_surface_compositor_damage *damage =
            &target->damages[revision % ARRAY_SIZE(target->damages)];

        if (damage->revision != revision)
        {
            *rect = (RECT){0, 0, target->window_width, target->window_height};
            return TRUE;
        }
        if (!initialized)
        {
            *rect = damage->rect;
            initialized = TRUE;
        }
        else
        {
            rect->left = min( rect->left, damage->rect.left );
            rect->top = min( rect->top, damage->rect.top );
            rect->right = max( rect->right, damage->rect.right );
            rect->bottom = max( rect->bottom, damage->rect.bottom );
        }
    }
    return TRUE;
}

static struct client_surface_output_transform *alloc_client_surface_output_transform(
    struct client_surface_compositor_target *target,
    struct client_surface_compositor_binding *binding, struct client_surface_compositor_frame *frame,
    const struct client_surface_composition_plan *plan, const RECT *damage,
    const RECT *catchup, BOOL needs_catchup, BOOL clipped, BOOL native )
{
    struct client_surface_compositor_frame *latest =
        get_client_surface_compositor_pixmap( target, target->latest );
    struct client_surface_output_transform *transform;
    const struct client_surface_handoff_slot *slot = &binding->latest_frame;
    SIZE_T count = plan->clip->rdh.nCount;

    assert( !target->transform );
    if (count > (~(SIZE_T)0 - sizeof(*transform)) / sizeof(XRectangle) ||
        (needs_catchup && (!latest || !latest->image || !latest->revision ||
                          client_surface_cache_write_pending( latest->image )))) return NULL;
    if (!(transform = client_surface_alloc_owned_array( &target->memory, 1,
                         sizeof(*transform) + count * sizeof(XRectangle) ))) return NULL;
    transform->image = frame->image;
    transform->source = client_surface_cache_acquire( binding->latest_image.storage );
    if (needs_catchup) transform->catchup = client_surface_cache_acquire( client_surface_compositor_frame_image( latest ) );
    transform->native = (struct client_surface_cache_transform){
        .source = binding->latest_image.pixmap,
        .catchup = needs_catchup ? client_surface_cache_pixmap( transform->catchup ) : 0,
        .source_visual = slot->source_visual, .destination_visual = target->visual,
        .source_width = slot->width, .source_height = slot->height,
        .destination = plan->destination, .catchup_rect = *catchup,
        .source_damage = plan->source_damage,
        .clips = transform->clips, .clip_count = count, .clipped = clipped, .native = native,
    };
    memcpy( transform->clips, plan->clip->Buffer, count * sizeof(XRectangle) );
    transform->toplevel = target->toplevel;
    transform->window = binding->window;
    transform->destination = target->window;
    transform->process = binding->process;
    transform->identity = binding->identity;
    transform->cookie = binding->cookie;
    transform->epoch = plan->epoch;
    transform->sequence = slot->source_sequence;
    transform->control = binding->latest_control;
    transform->revision = target->revision;
    transform->generation = plan->generation;
    transform->publication_generation = plan->publication_generation;
    transform->buffer_index = binding->latest_index;
    transform->scene_index = binding->scene_index;
    transform->width = target->window_width;
    transform->height = target->window_height;
    transform->replay = binding->source_sequence == slot->source_sequence;
    transform->damage = *damage;
    return transform;
}

static void free_client_surface_output_transforms( struct client_surface_output_transform *transform )
{
    struct client_surface_output_transform *next;

    for (; transform; transform = next)
    {
        next = transform->next;
        client_surface_cache_release( transform->source );
        client_surface_cache_release( transform->catchup );
        client_surface_free_owned_array( transform );
    }
}

static BOOL submit_client_surface_output_transform( struct client_surface_compositor_target *target,
    struct client_surface_compositor_binding *binding, struct client_surface_compositor_frame *frame,
    const struct client_surface_composition_plan *plan, const RECT *damage,
    const RECT *catchup, BOOL needs_catchup, BOOL clipped, BOOL native )
{
    struct client_surface_output_transform *transform;

    assert( !plan->generation && (plan->steady || plan->publication_generation) );
    assert( !plan->publication_generation || frame->pixmap != target->backing );
    if (!(transform = alloc_client_surface_output_transform( target, binding, frame, plan, damage,
                                                             catchup, needs_catchup, clipped, native ))) return FALSE;
    if (!client_surface_cache_transform_output( frame->image, &transform->native, 1,
                                                complete_client_surface_output_transform, transform ))
    {
        free_client_surface_output_transforms( transform );
        return FALSE;
    }
    target->transform = transform;
    /* Cancellation must not leave a partially written old checkpoint with
     * a valid journal revision, even when its image remains in this pool. */
    frame->revision = 0;
    TRACE_(csperf)( "ticks=%llu event=output_transform_submit transform=%p hwnd=%p window=%lx "
                   "image=%p source=%p catchup=%p destination=%lx epoch=%llu generation=0 sequence=%llu revision=%llu publication=%llu\n",
                   client_surface_perf_time(), transform, target->toplevel, target->window,
                   transform->image, transform->source, transform->catchup, frame->pixmap,
                   (unsigned long long)transform->epoch, (unsigned long long)transform->sequence,
                   (unsigned long long)transform->revision, (unsigned long long)transform->publication_generation );
    return TRUE;
}

static BOOL copy_client_surface_handoff_to_frame(
    struct client_surface_compositor_target *target,
    struct client_surface_compositor_binding *binding,
    struct client_surface_compositor_frame *frame, unsigned int source_depth,
    const struct client_surface_handoff_slot *slot,
    const struct client_surface_composition_plan *plan, const RECT *damage, BOOL batch )
{
    const XRectangle *clips = (const XRectangle *)plan->clip->Buffer;
    unsigned int clip_count = plan->clip->rdh.nCount;
    RECT catchup = {0};
    UINT64 revision = batch ? client_surface_copy_batch.revision : frame->revision;
    BOOL clipped, incoming_full, needs_catchup, native;
    unsigned int destination_width = plan->destination.right - plan->destination.left;
    unsigned int destination_height = plan->destination.bottom - plan->destination.top;

    if (!client_surface_compositor_frame_writable( frame )) return FALSE;
    if (!target->latest || !get_client_surface_compositor_catchup( target, frame, &catchup ))
        return FALSE;
    clipped = clip_count != 1 || clips[0].x || clips[0].y ||
              clips[0].width != destination_width || clips[0].height != destination_height;
    incoming_full = !clipped && damage->left == 0 && damage->top == 0 &&
                    (unsigned int)damage->right >= target->window_width &&
                    (unsigned int)damage->bottom >= target->window_height;
    needs_catchup = !incoming_full && frame->pixmap != target->latest &&
                    revision != target->revision && !IsRectEmpty( &catchup );
    native = source_depth == target->depth && slot->source_visual == target->visual &&
             slot->width == destination_width && slot->height == destination_height;

    TRACE_(csperf)( "ticks=%llu event=copy_route native=%u full=%u transaction=%u steady=%u assembly=%u "
                   "mailbox=%u ticket=%u latest=%u published=%u inflight=%u\n",
                   client_surface_perf_time(), native, incoming_full, !!plan->generation,
                   plan->steady, !!target->assembly_generation, target->mailbox_pending,
                   !!target->mailbox_publish_generation, frame->pixmap == target->latest,
                   frame->pixmap == target->published, !!frame->serial );
    if (!batch && plan->steady && native && incoming_full &&
        !plan->destination.left && !plan->destination.top &&
        slot->width == target->window_width && slot->height == target->window_height)
    {
        /* The completed owner image is already the whole output. Keep its
         * immutable reference rather than copying it into another private
         * pixmap. The pool ID remains the GUI checkpoint/retirement identity;
         * later partial assemblies catch up from this retained image. */
        retain_client_surface_frame_image( frame, binding->latest_image.storage );
        TRACE_(csperf)( "ticks=%llu event=output_image_retain frame=%lx image=%p source=%lx\n",
                       client_surface_perf_time(), frame->pixmap, frame->retained_image,
                       binding->latest_image.pixmap );
        trace_client_surface_source( "retain_output", binding, binding->latest_control,
            slot->source_sequence, target->window, binding->latest_image.pixmap, TRUE );
        complete_client_surface_frame_copy( target, frame, binding, plan->epoch,
                                            slot->source_sequence, damage, TRUE );
        return TRUE;
    }
    if (batch)
    {
        struct client_surface_output_transform *transform;

        assert( plan->generation && frame->pixmap != target->backing );
        if (!(transform = alloc_client_surface_output_transform( target, binding, frame, plan, damage,
                                                                 &catchup, needs_catchup, clipped, native ))) return FALSE;
        if (client_surface_copy_batch.tail)
        {
            client_surface_copy_batch.tail->next = transform;
            client_surface_copy_batch.tail->native.next = &transform->native;
        }
        else client_surface_copy_batch.transform = transform;
        client_surface_copy_batch.tail = transform;
        if (needs_catchup || incoming_full) client_surface_copy_batch.revision = target->revision;
        return TRUE;
    }
    return submit_client_surface_output_transform( target, binding, frame, plan, damage,
                                                    &catchup, needs_catchup, clipped, native );
}

static BOOL complete_client_surface_handoff_generation( struct client_surface_compositor_target *target,
                                                        UINT64 generation,
                                                        UINT64 scene_generation )
{
    BOOL accepted = FALSE;
    NTSTATUS status;

    SERVER_START_REQ( complete_client_surface_handoffs )
    {
        req->handle = wine_server_user_handle( target->toplevel );
        req->generation = generation;
        req->scene_generation = scene_generation;
        wine_server_add_data( req, target->receipts, target->scene.count * sizeof(*target->receipts) );
        status = wine_server_call( req );
        if (!status) accepted = reply->accepted;
    }
    SERVER_END_REQ;
    TRACE( "completed owner generation %s epoch %s status %#lx accepted %u\n",
           wine_dbgstr_longlong( generation ), wine_dbgstr_longlong( scene_generation ),
           (unsigned long)status, accepted );
    return !status && accepted;
}

BOOL publish_client_surface_handoff_generation( HWND toplevel, UINT64 generation,
                                                       UINT64 scene_generation, BOOL success )
{
    BOOL accepted = FALSE;
    NTSTATUS status;

    SERVER_START_REQ( publish_client_surface_handoff )
    {
        req->handle = wine_server_user_handle( toplevel );
        req->generation = generation;
        req->scene_generation = scene_generation;
        req->success = success;
        status = wine_server_call( req );
        if (!status) accepted = reply->accepted;
    }
    SERVER_END_REQ;
    TRACE( "published owner generation %s epoch %s status %#lx accepted %u success %u\n",
           wine_dbgstr_longlong( generation ), wine_dbgstr_longlong( scene_generation ),
           (unsigned long)status, accepted, success );
    return !status && accepted && success;
}

static struct client_surface_compositor_frame *find_client_surface_pending_publication(
    struct client_surface_compositor_target *target, UINT64 generation, UINT64 epoch )
{
    unsigned int i;

    if (target->mailbox_pending && target->mailbox_publish_generation == generation &&
        target->mailbox_publish_epoch == epoch)
        return &target->frames[target->mailbox_frame];
    for (i = 0; i < ARRAY_SIZE(target->frames); ++i)
        if (target->frames[i].publish_pending &&
            target->frames[i].publish_generation == generation &&
            target->frames[i].publish_epoch == epoch) return &target->frames[i];
    return NULL;
}

static BOOL client_surface_handoff_generation_assembled(
    struct client_surface_compositor_target *target,
    struct client_surface_compositor_frame *frame, UINT64 generation, UINT64 epoch )
{
    return target->scene.valid && target->scene.epoch == epoch && target->scene.count &&
           target->assembly_generation && target->assembly_generation == generation &&
           target->assembly_epoch == epoch && target->assembly_frame == frame - target->frames &&
           target->received == target->scene.count;
}

static BOOL publish_client_surface_handoff_assembly(
    struct client_surface_compositor_target *target,
    struct client_surface_compositor_frame *frame, UINT64 generation, UINT64 epoch )
{
    BOOL visible = FALSE, queued = FALSE, deferred = FALSE;
    RECT full = {0, 0, target->window_width, target->window_height};

    if (!complete_client_surface_handoff_generation( target, generation, epoch ))
        goto done;

    if (!target->mailbox_pending &&
        count_client_surface_compositor_frames( target ) < CLIENT_SURFACE_COMPOSITOR_MAX_INFLIGHT)
        visible = queued = submit_client_surface_present( target, frame, generation, epoch, NULL, NULL, NULL, NULL );
    else
    {
        target->mailbox_frame = frame - target->frames;
        target->mailbox_pending = TRUE;
        target->mailbox_publish_generation = generation;
        target->mailbox_publish_epoch = epoch;
        visible = deferred = TRUE;
    }
    if (!queued && !deferred)
        publish_client_surface_handoff_generation( target->toplevel, generation, epoch, visible );
    if (visible) note_client_surface_compositor_damage( target, frame, &full );

done:
    /* Receipts describe the private assembled image. Source storage was
     * returned after each copy and is independent of this publication. */
    finish_client_surface_compositor_assembly( target, !visible );
    return visible;
}

static void apply_client_surface_owner_copies( struct client_surface_compositor_target *target,
    struct client_surface_compositor_frame *frame, const struct client_surface_owner_copy *copies,
    unsigned int count, BOOL success )
{
    unsigned int i;
    UINT64 generation, epoch;
    BOOL assembly_valid;

    if (!count) return;
    generation = copies[0].generation;
    epoch = copies[0].epoch;
    assembly_valid = target->assembly_generation && target->scene.valid &&
        target->assembly_generation == generation && target->assembly_epoch == epoch &&
        target->scene.epoch == epoch && target->assembly_frame == frame - target->frames;
    for (i = 0; i < count; ++i)
    {
        const struct client_surface_owner_copy *copy = &copies[i];
        struct client_surface_compositor_binding *binding = copy->binding;

        trace_client_surface_source( copy->replay ? "replay_copy_async" : "copy_async",
                                     binding, copy->control, copy->sequence,
                                     target->window, frame->pixmap, success && assembly_valid );
        if (success && assembly_valid)
        {
            struct client_surface_handoff_receipt *receipt = &target->receipts[binding->scene_index];

            note_client_surface_source_copy( binding, copy->epoch, copy->sequence, target->replay_generation );
            if (!receipt->source_generation) ++target->received;
            *receipt = (struct client_surface_handoff_receipt){
                .handle = wine_server_user_handle( binding->window ),
                .process = binding->process,
                .surface = binding->identity,
                .cookie = binding->cookie,
                .source_generation = copy->sequence,
                .buffer_index = copy->buffer_index,
            };
        }
        else if (!success)
        {
            binding->source_epoch = binding->source_sequence = 0;
            binding->replay_epoch = 0;
        }
    }
    TRACE( "owner copy batch %u sources, success %u, generation %s epoch %s pixmap %#lx\n",
           count, success, wine_dbgstr_longlong( generation ), wine_dbgstr_longlong( epoch ),
           frame->pixmap );
    if (!success)
    {
        invalidate_client_surface_compositor_assembly( target );
        wake_client_surface_compositor();
    }
    else if (client_surface_handoff_generation_assembled( target, frame, generation, epoch ))
        publish_client_surface_handoff_assembly( target, frame, generation, epoch );
}

static void flush_client_surface_copy_batch(void)
{
    struct client_surface_copy_batch *batch = &client_surface_copy_batch;
    struct client_surface_output_transform *transform;
    unsigned int i;

    if (!client_surface_copy_batch.count) return;
    transform = batch->transform;
    if (!batch->error)
    {
        assert( transform && !batch->target->transform );
        transform->frame_revision = batch->revision;
        if (client_surface_cache_transform_output( batch->frame->image, &transform->native, batch->count,
                                                   complete_client_surface_output_transform_batch, transform ))
        {
            struct client_surface_output_transform *member;

            batch->target->transform = transform;
            batch->frame->revision = 0;
            TRACE_(csperf)( "ticks=%llu event=output_transform_batch_submit transform=%p hwnd=%p window=%lx "
                           "image=%p destination=%lx epoch=%llu generation=%llu count=%u revision=%llu\n",
                           client_surface_perf_time(), transform, transform->toplevel, transform->destination,
                           transform->image, batch->frame->pixmap, (unsigned long long)transform->epoch,
                           (unsigned long long)transform->generation, batch->count,
                           (unsigned long long)transform->frame_revision );
            for (member = transform, i = 0; member; member = member->next, ++i)
                TRACE_(csperf)( "ticks=%llu event=output_transform_batch_member transform=%p member=%p "
                               "image=%p source=%p catchup=%p index=%u scene_index=%u identity=%llu cookie=%llu "
                               "epoch=%llu generation=%llu sequence=%llu control=%llu\n",
                               client_surface_perf_time(), transform, member, member->image, member->source,
                               member->catchup, i, member->scene_index, (unsigned long long)member->identity,
                               (unsigned long long)member->cookie, (unsigned long long)member->epoch,
                               (unsigned long long)member->generation, (unsigned long long)member->sequence,
                               (unsigned long long)member->control );
            batch->count = 0;
        }
    }
    if (batch->count)
    {
        /* Nothing was submitted. The current actor-local builder may fail
         * its assembly, but has no native GC to retire on this connection. */
        free_client_surface_output_transforms( transform );
        i = batch->count;
        batch->count = 0;
        apply_client_surface_owner_copies( batch->target, batch->frame, batch->copies, i, FALSE );
    }
    batch->transform = batch->tail = NULL;
}

static BOOL publish_client_surface_handoff_frame(
    struct client_surface_compositor_target *target,
    struct client_surface_compositor_frame *frame,
    struct client_surface_compositor_binding *binding, const RECT *damage )
{
    /* Reserve this bounded frame through its SOURCE commit. Coalescing it
     * into a later full-frame mailbox would also replay untouched SOURCEs. */
    return submit_client_surface_present( target, frame, 0, 0, NULL, NULL, binding, damage );
}

static void complete_client_surface_frame_copy( struct client_surface_compositor_target *target,
    struct client_surface_compositor_frame *frame, struct client_surface_compositor_binding *binding,
    UINT64 epoch, UINT64 sequence, const RECT *damage, BOOL success )
{
    if (!success)
    {
        binding->source_epoch = binding->source_sequence = 0;
        binding->replay_epoch = 0;
        frame->revision = 0;
        return;
    }
    note_client_surface_source_copy( binding, epoch, sequence, target->replay_generation );
    note_client_surface_compositor_damage( target, frame, damage );
    publish_client_surface_handoff_frame( target, frame, binding, damage );
}

static void complete_client_surface_output_transform( void *context, BOOL success )
{
    struct client_surface_output_transform *transform = context;
    struct client_surface_compositor_target *target =
        find_client_surface_compositor_target( transform->toplevel );
    struct client_surface_compositor_frame *frame = NULL;
    struct client_surface_compositor_binding *binding = NULL;
    struct client_surface_scene scene;
    BOOL current = target && target->transform == transform;

    if (current)
    {
        target->transform = NULL;
        frame = get_client_surface_compositor_pixmap( target, client_surface_cache_pixmap( transform->image ) );
        current = frame && frame->image == transform->image && !frame->serial &&
                  (!transform->publication_generation || frame->pixmap != target->backing) &&
                  target->window == transform->destination && !target->quiescing &&
                  target->revision == transform->revision && target->window_width == transform->width &&
                  target->window_height == transform->height && target->visual == transform->native.destination_visual &&
                  target->scene.valid &&
                  target->scene.epoch == transform->epoch && transform->scene_index < target->scene.count &&
                  client_surface_get_toplevel_scene( target->toplevel, &scene ) &&
                  scene.epoch == transform->epoch && scene.mode != CLIENT_SURFACE_PRESENTATION_DIRECT &&
                  (!scene.generation || (scene.publication_pending &&
                   scene.generation == transform->publication_generation));
        if (current)
        {
            binding = target->scene.members[transform->scene_index];
            current = binding->window == transform->window && binding->process == transform->process &&
                      binding->identity == transform->identity && binding->cookie == transform->cookie &&
                      client_surface_compositor_binding_is_live( binding ) &&
                      (binding->source_epoch != transform->epoch || binding->source_sequence <= transform->sequence);
        }
        if (!current) target->replay_member = 0;
    }
    TRACE_(csperf)( "ticks=%llu event=output_transform_complete transform=%p hwnd=%p window=%lx "
                   "image=%p source=%p destination=%lx epoch=%llu sequence=%llu success=%u current=%u publication=%llu\n",
                   client_surface_perf_time(), transform, transform->toplevel, transform->destination,
                   transform->image, transform->source, client_surface_cache_pixmap( transform->image ),
                   (unsigned long long)transform->epoch, (unsigned long long)transform->sequence, success, current,
                   (unsigned long long)transform->publication_generation );
    /* Completion removed CACHE_TRANSFORM before invoking us. Drop all native
     * leases even when REMOVE has already released the target and binding. */
    client_surface_cache_release( transform->source );
    client_surface_cache_release( transform->catchup );
    client_surface_cache_release( transform->image );
    if (current)
    {
        trace_client_surface_source( transform->replay ? "replay_transform" : "transform",
            binding, transform->control, transform->sequence, target->window, frame->pixmap, success );
        complete_client_surface_frame_copy( target, frame, binding, transform->epoch,
                                            transform->sequence, &transform->damage, success );
    }
    client_surface_free_owned_array( transform );
}

static void complete_client_surface_output_transform_batch( void *context, BOOL success )
{
    struct client_surface_output_transform *transform = context, *member;
    struct client_surface_cache_image *image = transform->image;
    struct client_surface_compositor_target *target =
        find_client_surface_compositor_target( transform->toplevel );
    struct client_surface_compositor_frame *frame = NULL;
    struct client_surface_owner_copy copies[CLIENT_SURFACE_COPY_BATCH_SIZE];
    struct client_surface_scene scene;
    unsigned int count = 0;
    BOOL attached = target && target->transform == transform, current = attached;

    if (current)
    {
        frame = get_client_surface_compositor_pixmap( target, client_surface_cache_pixmap( transform->image ) );
        current = frame && frame->image == transform->image && !frame->serial &&
                  frame->pixmap != target->backing && target->window == transform->destination &&
                  !target->quiescing && target->revision == transform->revision &&
                  target->window_width == transform->width && target->window_height == transform->height &&
                  target->visual == transform->native.destination_visual && target->scene.valid &&
                  target->scene.epoch == transform->epoch && target->assembly_generation &&
                  target->assembly_generation == transform->generation &&
                  target->assembly_epoch == transform->epoch && target->assembly_frame == frame - target->frames &&
                  client_surface_get_toplevel_scene( target->toplevel, &scene ) &&
                  scene.epoch == transform->epoch && scene.generation == transform->generation &&
                  scene.mode != CLIENT_SURFACE_PRESENTATION_DIRECT && !scene.publication_pending;
    }
    /* Resolve all saved identities before changing any receipt or source
     * epoch. A stale result must never abort a replacement assembly. */
    for (member = transform; member; member = member->next, ++count)
    {
        struct client_surface_compositor_binding *binding;

        assert( count < ARRAY_SIZE(copies) );
        if (!current) continue;
        current = member->scene_index < target->scene.count;
        if (!current) continue;
        binding = target->scene.members[member->scene_index];
        current = binding->window == member->window && binding->process == member->process &&
                  binding->identity == member->identity && binding->cookie == member->cookie &&
                  client_surface_compositor_binding_is_live( binding ) &&
                  (binding->source_epoch != member->epoch || binding->source_sequence <= member->sequence);
        copies[count] = (struct client_surface_owner_copy){binding, member->buffer_index,
            member->control, member->generation, member->epoch, member->sequence, member->replay};
    }
    TRACE_(csperf)( "ticks=%llu event=output_transform_batch_complete transform=%p hwnd=%p window=%lx "
                   "image=%p destination=%lx epoch=%llu generation=%llu count=%u success=%u current=%u\n",
                   client_surface_perf_time(), transform, transform->toplevel, transform->destination,
                   transform->image, client_surface_cache_pixmap( transform->image ),
                   (unsigned long long)transform->epoch, (unsigned long long)transform->generation,
                   count, success, current );
    /* The destination's sole native write reference is shared by the whole
     * group. Return it before the existing assembly commit can publish. */
    if (current && success) frame->revision = transform->frame_revision;
    if (attached)
    {
        if (!current) abort_client_surface_output_transform_assembly( target, transform );
        target->transform = NULL;
    }
    free_client_surface_output_transforms( transform );
    client_surface_cache_release( image );
    if (current) apply_client_surface_owner_copies( target, frame, copies, count, success );
}

/* Use the same immutable source predicate for composition and native owner
 * repair. Backend capability and consumed channel sequences are not cache proof. */
static BOOL client_surface_cached_frame_matches_layout( const struct client_surface_compositor_binding *binding,
                                                         const struct client_surface_scene_layout *layout )
{
    const struct client_surface_handoff_slot *slot = &binding->latest_frame;
    const RECT *destination = &layout->geometry.monitor_rect;
    unsigned int width = destination->right > destination->left ? destination->right - destination->left : 0;
    unsigned int height = destination->bottom > destination->top ? destination->bottom - destination->top : 0;

    return binding->latest_image.pixmap && slot->cookie == binding->cookie && slot->identity == binding->identity &&
           slot->producer_process == binding->process && slot->window == wine_server_user_handle( binding->window ) &&
           slot->toplevel == wine_server_user_handle( binding->toplevel ) &&
           (slot->flags & CLIENT_SURFACE_HANDOFF_NATIVE_X11) && width && height &&
           slot->width && slot->height && slot->source_visual &&
           ((slot->width == width && slot->height == height) ||
            (slot->width == layout->geometry.virtual_rect.right - layout->geometry.virtual_rect.left &&
             slot->height == layout->geometry.virtual_rect.bottom - layout->geometry.virtual_rect.top)) &&
           slot->damage.left >= 0 && slot->damage.top >= 0 && !IsRectEmpty( &slot->damage ) &&
           (unsigned int)slot->damage.right <= slot->width && (unsigned int)slot->damage.bottom <= slot->height;
}

BOOL repair_client_surface_compositor_owner( HWND toplevel, BOOL resolve,
                                                     struct client_surface_owner_repair *repair )
{
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( toplevel );
    struct client_surface_scene current;
    unsigned int before = client_surface_compositor_repair_budget;
    BOOL accepted = FALSE;

    if (resolve && (!client_surface_get_toplevel_scene( toplevel, &current ) || !current.source_pending))
    {
        accepted = TRUE;
        goto done;
    }
    if (!target || !target->scene.valid || !target->scene.count) goto done;
    if (repair->phase && (repair->resolve != resolve || repair->epoch != target->scene.epoch ||
                         repair->bindings != target->binding_generation || repair->inventory != target->inventory_generation ||
                         (repair->phase == OWNER_REPAIR_COMPLETE && repair->caches != target->cache_generation)))
        reset_client_surface_owner_repair( repair );
    /* Like failed mailbox allocation, a failed automatic repair is retried
     * on new scene/source input. Retaining its result avoids polling forever
     * just because collecting a large rejected inventory required yields.
     * Explicit GUI jobs have their own fresh continuation. */
    if (repair->phase == OWNER_REPAIR_COMPLETE) return TRUE;
    if (!client_surface_compositor_repair_budget) goto pending;
    if (repair->phase == OWNER_REPAIR_NEW)
    {
        repair->phase = OWNER_REPAIR_COLLECTING;
        repair->resolve = resolve;
        repair->epoch = target->scene.epoch;
        repair->bindings = target->binding_generation;
        repair->inventory = target->inventory_generation;
        if (!client_surface_scene_snapshot_current( toplevel, repair->epoch )) goto done;
        if (!(repair->receipts = client_surface_alloc_owned_array( &target->memory, target->scene.count,
                                                                  sizeof(*repair->receipts) ))) goto done;
    }
    while (repair->index < target->scene.count && client_surface_compositor_repair_budget)
    {
        unsigned int i = repair->index++;
        struct client_surface_compositor_binding *binding = target->scene.members[i];

        --client_surface_compositor_repair_budget;
        if (!client_surface_compositor_binding_is_live( binding ) ||
            !client_surface_cached_frame_matches_layout( binding, &target->scene.layouts[i] ))
        {
            if (resolve) continue;
            goto done;
        }
        repair->receipts[repair->count++] = (struct client_surface_handoff_receipt){
            .handle = wine_server_user_handle( binding->window ), .process = binding->process,
            .surface = binding->identity, .cookie = binding->cookie,
            .source_generation = binding->latest_frame.source_sequence, .buffer_index = binding->latest_index,
        };
    }
    if (repair->index < target->scene.count) goto pending;
    if (resolve)
    {
        SERVER_START_REQ( resolve_client_surface_scene_sources )
        {
            req->handle = wine_server_user_handle( toplevel );
            req->scene_id = repair->epoch;
            wine_server_add_data( req, repair->receipts, repair->count * sizeof(*repair->receipts) );
            if (!wine_server_call( req )) accepted = reply->accepted;
        }
        SERVER_END_REQ;
        TRACE( "owner scene sources hwnd %p scene %s images %u/%u accepted %u\n", toplevel,
               wine_dbgstr_longlong( repair->epoch ), repair->count, target->scene.count, accepted );
    }
    else
    {
        SERVER_START_REQ( request_client_surface_owner_repair )
        {
            req->handle = wine_server_user_handle( toplevel );
            req->scene_id = repair->epoch;
            wine_server_add_data( req, repair->receipts, repair->count * sizeof(*repair->receipts) );
            if (!wine_server_call( req )) accepted = reply->accepted;
        }
        SERVER_END_REQ;
        TRACE( "owner cache repair hwnd %p scene %s images %u accepted %u\n", toplevel,
               wine_dbgstr_longlong( repair->epoch ), repair->count, accepted );
    }
    if (accepted)
    {
        /* A repair of an existing assembly can keep its scene ID. Backing
         * damage must nevertheless replay every retained image in that scene. */
        target->replay_member = 0;
        ++target->replay_generation;
    }
done:
    client_surface_free_owned_array( repair->receipts );
    repair->receipts = NULL;
    repair->phase = OWNER_REPAIR_COMPLETE;
    repair->result = accepted;
    if (target) repair->caches = target->cache_generation;
    TRACE_(csperf)( "ticks=%llu event=owner_repair_scan hwnd=%p resolve=%u inspected=%u index=%u receipts=%u complete=1 accepted=%u\n",
                   client_surface_perf_time(), toplevel, resolve, before - client_surface_compositor_repair_budget,
                   repair->index, repair->count, accepted );
    return TRUE;

pending:
    client_surface_compositor_repair_pending = TRUE;
    if (before != client_surface_compositor_repair_budget)
        TRACE_(csperf)( "ticks=%llu event=owner_repair_scan hwnd=%p resolve=%u inspected=%u index=%u receipts=%u complete=0 accepted=0\n",
                       client_surface_perf_time(), toplevel, resolve, before - client_surface_compositor_repair_budget,
                       repair->index, repair->count );
    return FALSE;
}

static BOOL compose_client_surface_cached_frame( struct client_surface_compositor_binding *binding )
{
    const struct client_surface_handoff_slot *slot;
    struct client_surface_composition_plan plan;
    struct client_surface_scene current;
    const struct client_surface_scene_layout *layout;
    struct client_surface_compositor_target *target;
    struct client_surface_compositor_frame *frame = NULL;
    UINT64 control;
    unsigned int buffer_index;
    Pixmap source = 0;
    RECT damage;
    unsigned int destination_width, destination_height, source_depth = 0;
    BOOL copied = FALSE, dropped = TRUE, batch, replay;

    target = find_client_surface_compositor_target( binding->toplevel );
    if (!target || target->transform || target->quiescing || !target->scene.valid) return FALSE;
    if (!binding->latest_image.pixmap) return FALSE;
    /* From here onwards only owner storage is read. The producer has already
     * received its slot, and may overwrite or destroy it during publication. */
    slot = &binding->latest_frame;
    control = binding->latest_control;
    buffer_index = binding->latest_index;
    source = binding->latest_image.pixmap;
    source_depth = binding->latest_image.depth;
    replay = binding->source_sequence == slot->source_sequence;
    if (!client_surface_get_toplevel_scene( binding->toplevel, &current ) ||
        current.epoch != target->scene.epoch ||
        current.mode == CLIENT_SURFACE_PRESENTATION_DIRECT) return FALSE;
    /* A returned output credit also starts reading the newest retained
     * producer image. Do not spend that credit on an older dirty cache while
     * its replacement is being checked. Scene obligations still use their
     * complete owner images independently of this steady-frame preference. */
    if (!current.generation && !current.source_pending && !current.publication_pending &&
        client_surface_cache_read_pending( binding )) return FALSE;
    /* Inventory resolution precedes the first copy, so the full assembly
     * cannot race its own pending decision or publish a stale cache proof. */
    if (current.source_pending &&
        (!repair_client_surface_compositor_owner( binding->toplevel, TRUE, &target->repair ) || !target->repair.result ||
         !client_surface_get_toplevel_scene( binding->toplevel, &current ) || current.source_pending ||
         current.epoch != target->scene.epoch || current.mode == CLIENT_SURFACE_PRESENTATION_DIRECT))
        return FALSE;
    if (replay) trace_client_surface_source( "replay_cache", binding, control,
                                             slot->source_sequence, target->window, source, TRUE );
    /* PUBLISHING still carries the transaction generation while the GUI
     * exposes its native output, even after its Present ticket is released.
     * Its assembly was already accepted: newer images use the steady path
     * in the same scene, never another completion of that transaction. */
    plan.generation = current.publication_pending ? 0 : current.generation;
    plan.publication_generation = current.publication_pending ? current.generation : 0;
    /* PREPARE and STAGED install a fresh GUI checkpoint, using pair COW if
     * an older transform still owns its spare. Keep that checkpoint intact
     * through the subsequent nonzero generation, including PUBLISHING where
     * plan.generation is zero but the GUI still owes its native publication.
     * Only a truly steady result may detach without a publication receipt. */
    plan.steady = !current.generation;
    TRACE( "owner source window %#lx generation %s epoch %s publication %u output %s sequence %s\n",
           target->window, wine_dbgstr_longlong( current.generation ), wine_dbgstr_longlong( current.epoch ),
           current.publication_pending, wine_dbgstr_longlong( plan.generation ),
           wine_dbgstr_longlong( slot->source_sequence ) );
    plan.epoch = current.epoch;
    plan.source_damage = slot->damage;
    /* Cache reception already validated the native source. Compatible scene
     * members can share the existing checked output batch without opening
     * another Xlib error scope or retaining any producer storage. */
    if (client_surface_copy_batch.count &&
        (client_surface_copy_batch.target != target || !plan.generation ||
         client_surface_copy_batch.copies[0].generation != plan.generation ||
         client_surface_copy_batch.copies[0].epoch != plan.epoch))
        flush_client_surface_copy_batch();
    if (target->transform) goto retry;
    if (target->scene.epoch != plan.epoch || binding->scene_index >= target->scene.count ||
        target->scene.members[binding->scene_index] != binding ||
        (binding->source_epoch == plan.epoch && slot->source_sequence < binding->source_sequence))
    {
        dropped = TRUE;
        goto release;
    }
    layout = &target->scene.layouts[binding->scene_index];
    plan.destination = layout->geometry.monitor_rect;
    plan.clip = layout->clip;
    destination_width = plan.destination.right > plan.destination.left ?
                        plan.destination.right - plan.destination.left : 0;
    destination_height = plan.destination.bottom > plan.destination.top ?
                         plan.destination.bottom - plan.destination.top : 0;
    if (target->backing && target->frames[0].pixmap && target->frames[1].pixmap && target->window &&
        client_surface_cached_frame_matches_layout( binding, layout ))
    {
        dropped = FALSE;
        /* Placement and clip come from the owner's scene. Children may
         * extend outside the top-level; the scene clip and destination
         * drawable bound the copy, without changing its source mapping. */
        damage = plan.destination;
        /* Transactions need each participant's complete visible contribution.
         * Steady frames may use source damage; align the owner journal with
         * the actual source rectangle instead of declaring it fully replaced. */
        /* A missed or superseded source breaks incremental continuity. The
         * immutable image always contains the full frame, so recover by
         * copying it in full. Scaled copies use a conservative full journal. */
        if (plan.generation || (slot->flags & CLIENT_SURFACE_HANDOFF_FULL_DAMAGE) ||
            binding->source_epoch != plan.epoch || !slot->damage_base_sequence ||
            slot->damage_base_sequence != binding->source_sequence ||
            slot->width != destination_width || slot->height != destination_height)
            SetRect( &plan.source_damage, 0, 0, slot->width, slot->height );
        else
        {
            TRACE( "incremental owner copy hwnd %p sequence %s base %s damage %s\n", binding->window,
                   wine_dbgstr_longlong( slot->source_sequence ),
                   wine_dbgstr_longlong( slot->damage_base_sequence ), wine_dbgstr_rect( &plan.source_damage ) );
            damage.left += (UINT64)plan.source_damage.left * destination_width / slot->width;
            damage.top += (UINT64)plan.source_damage.top * destination_height / slot->height;
            damage.right = plan.destination.left +
                ((UINT64)plan.source_damage.right * destination_width + slot->width - 1) / slot->width;
            damage.bottom = plan.destination.top +
                ((UINT64)plan.source_damage.bottom * destination_height + slot->height - 1) / slot->height;
        }
        /* The reserved assembly already owns publication. Retain newer
         * SOURCE in its cache until that receipt advances the scene. */
        if (plan.generation && find_client_surface_pending_publication( target, plan.generation, plan.epoch ))
            goto retry;
        if (target->assembly_generation &&
            (target->assembly_generation != plan.generation ||
             target->assembly_epoch != plan.epoch))
            finish_client_surface_compositor_assembly( target, TRUE );
        /* A single steady layer can retain newer complete images in its
         * owner cache until a native output can be submitted. Rewriting the
         * mailbox while both Present credits are owned only replaces work
         * that cannot become visible. Keep source_sequence at the last copy;
         * a skipped damage base then takes the existing full-image recovery.
         * Transactions and multi-layer replay keep their assembly ordering. */
        if (!plan.generation && (target->mailbox_pending ||
            count_client_surface_compositor_frames( target ) >= CLIENT_SURFACE_COMPOSITOR_MAX_INFLIGHT))
            goto retry;
        if (!plan.generation)
            frame = get_client_surface_compositor_frame( target, !plan.steady );
        else if (target->assembly_generation &&
                 target->assembly_generation == plan.generation &&
                 target->assembly_epoch == plan.epoch)
            frame = &target->frames[target->assembly_frame];
        else
            frame = acquire_client_surface_compositor_assembly_frame( target );

        if (!frame || !client_surface_compositor_frame_writable( frame ))
        {
retry:
            /* Output backpressure retains the owner cache, never a producer
             * slot. Retry this image when output work makes progress. */
            binding->replay_epoch = 0;
            target->replay_member = min( target->replay_member, binding->scene_index );
            return FALSE;
        }
        /* A failed publication can leave staging without a generation.
         * Once both retained source and output storage are usable, rebuild
         * that publication before copying: a steady Present cannot expose
         * the staged window. Keep these owned resources on rejection and
         * wait for a current plan after repair changes the scene epoch. */
        if (current.mode == CLIENT_SURFACE_PRESENTATION_STAGED && !current.generation)
        {
            repair_client_surface_compositor_owner( binding->toplevel, FALSE, &target->repair );
            goto retry;
        }
        if (plan.generation && !target->assembly_generation)
        {
            target->assembly_generation = plan.generation;
            target->assembly_epoch = plan.epoch;
            target->assembly_frame = frame - target->frames;
        }

        /* Every participant is copied into one private assembly frame. Its
         * receipt survives the source release, allowing the producer to make
         * progress while other participants are still completing. */
        batch = !!plan.generation;
        if (batch)
        {
            if (!client_surface_copy_batch.count)
            {
                client_surface_copy_batch.target = target;
                client_surface_copy_batch.frame = frame;
                client_surface_copy_batch.error = 0;
                client_surface_copy_batch.revision = frame->revision;
                client_surface_copy_batch.transform = client_surface_copy_batch.tail = NULL;
            }
            assert( client_surface_copy_batch.frame == frame );
            client_surface_copy_batch.copies[client_surface_copy_batch.count++] =
                (struct client_surface_owner_copy){binding,
                                                   buffer_index, control, plan.generation, plan.epoch,
                                                   slot->source_sequence, replay};
        }
        copied = copy_client_surface_handoff_to_frame( target, binding, frame, source_depth,
                                                       slot, &plan, &damage, batch );
        if (copied)
        {
            frame->width = target->window_width;
            frame->height = target->window_height;
        }
        if (!batch && copied) return TRUE;
        if (batch)
        {
            if (!copied) client_surface_copy_batch.error = 1;
            if (!copied || client_surface_copy_batch.count == CLIENT_SURFACE_COPY_BATCH_SIZE)
                flush_client_surface_copy_batch();
            return copied;
        }
    }
release:
    /* A resized or otherwise incompatible image is still valid storage.
     * Metadata rejection leaves it returned; a real copy/import error
     * still retires the binding through the normal failure path. */
    if (replay && (copied || dropped))
    {
        binding->replay_epoch = target->scene.epoch;
        binding->replay_generation = target->replay_generation;
    }
    if (!copied)
        trace_client_surface_source( "discard", binding, control, slot->source_sequence,
                                     target->window, frame ? frame->pixmap : 0, dropped );
    if (!copied && !dropped) flush_client_surface_copy_batch();
    if (!copied && !dropped && plan.generation && target &&
        target->assembly_generation && target->assembly_generation == plan.generation &&
        target->assembly_epoch == plan.epoch)
        finish_client_surface_compositor_assembly( target, TRUE );
    return FALSE;
}

static BOOL client_surface_handoff_matches_cache( const struct client_surface_compositor_binding *binding,
                                                 const struct client_surface_handoff_slot *slot )
{
    const struct client_surface_handoff_slot *cached = &binding->latest_frame;

    return slot->cookie == cached->cookie && slot->identity == cached->identity &&
           slot->producer_process == cached->producer_process && slot->window == cached->window &&
           slot->toplevel == cached->toplevel && slot->target_epoch == cached->target_epoch &&
           slot->source && slot->source_visual == cached->source_visual &&
           slot->width == cached->width && slot->height == cached->height &&
           (slot->flags & (CLIENT_SURFACE_HANDOFF_NATIVE_X11 | CLIENT_SURFACE_HANDOFF_COPY_SOURCE)) ==
                         (CLIENT_SURFACE_HANDOFF_NATIVE_X11 | CLIENT_SURFACE_HANDOFF_COPY_SOURCE);
}

/* Only unread steady images can be superseded. Keep the last complete owner
 * image and the newest published producer image; a scene transaction or a
 * native read must never depend on a slot returned by this path. */
static BOOL coalesce_client_surface_handoffs( struct client_surface_compositor_binding *binding,
                                             UINT64 *consumed, UINT64 produced, unsigned int limit )
{
    struct client_surface_compositor_target *target = find_client_surface_compositor_target( binding->toplevel );
    struct client_surface_handoff_channel *channel = binding->channel;
    const struct client_surface_handoff_slot *newest;
    struct client_surface_scene scene;
    UINT64 previous = *consumed;

    assert( !client_surface_cache_read_pending( binding ) && produced != *consumed );
    if (!target || target->quiescing || target->assembly_generation || !target->scene.valid ||
        binding->scene_index >= target->scene.count || target->scene.members[binding->scene_index] != binding ||
        !client_surface_cached_frame_matches_layout( binding, &target->scene.layouts[binding->scene_index] ) ||
        !client_surface_get_toplevel_scene( binding->toplevel, &scene ) || scene.epoch != target->scene.epoch ||
        scene.mode != CLIENT_SURFACE_PRESENTATION_COMPOSITED || scene.generation ||
        scene.source_pending || scene.publication_pending) return FALSE;
    newest = &channel->slots[(produced - 1) & (CLIENT_SURFACE_HANDOFF_RING_SIZE - 1)];
    if (!client_surface_handoff_matches_cache( binding, newest ) ||
        newest->source_sequence < binding->latest_frame.source_sequence) return FALSE;
    while (produced - *consumed > 1 && limit--)
    {
        const struct client_surface_handoff_slot slot =
            channel->slots[*consumed & (CLIENT_SURFACE_HANDOFF_RING_SIZE - 1)];

        if (!client_surface_handoff_matches_cache( binding, &slot ) ||
            slot.source_sequence >= newest->source_sequence) break;
        ++*consumed;
        /* No native request has read this slot. The producer can now reuse
         * it, but cannot replace newest before its separate read receipt. */
        __atomic_store_n( &channel->consumer_sequence, *consumed, __ATOMIC_RELEASE );
        TRACE_(csperf)( "ticks=%llu event=cache_skip identity=%s cookie=%s token=%s sequence=%s "
                       "pixmap=%s retained_token=%s retained_sequence=%s cached_sequence=%s scene=%s\n",
                       client_surface_perf_time(), wine_dbgstr_longlong( binding->identity ),
                       wine_dbgstr_longlong( binding->cookie ), wine_dbgstr_longlong( *consumed ),
                       wine_dbgstr_longlong( slot.source_sequence ), wine_dbgstr_longlong( slot.source ),
                       wine_dbgstr_longlong( produced ), wine_dbgstr_longlong( newest->source_sequence ),
                       wine_dbgstr_longlong( binding->latest_frame.source_sequence ), wine_dbgstr_longlong( scene.epoch ) );
    }
    if (*consumed != previous) client_surface_handoff_wake_release( binding->pool->shared );
    if (produced - *consumed != 1) return FALSE;
    /* Native Complete/Idle or a new READY wakes the actor. Preserve the hint
     * across its armed rescan without reporting a parked image as progress.
     * A newer READY may replace this one; the final image waits for credit. */
    if (count_client_surface_compositor_frames( target ) >= CLIENT_SURFACE_COMPOSITOR_MAX_INFLIGHT)
    {
        TRACE_(csperf)( "ticks=%llu event=cache_defer identity=%s cookie=%s token=%s sequence=%s scene=%s\n",
                       client_surface_perf_time(), wine_dbgstr_longlong( binding->identity ),
                       wine_dbgstr_longlong( binding->cookie ), wine_dbgstr_longlong( produced ),
                       wine_dbgstr_longlong( newest->source_sequence ), wine_dbgstr_longlong( scene.epoch ) );
        return TRUE;
    }
    return FALSE;
}

BOOL process_client_surface_handoffs( struct client_surface_compositor_scan *scan )
{
    struct client_surface_compositor_pool *pool;
    unsigned int words = 0, hints = 0;
    BOOL progressed = FALSE;
    int budget = CLIENT_SURFACE_COPY_BATCH_SIZE;

    if (scan->pool_generation != client_surface_compositor_pool_generation)
    {
        init_client_surface_source_scan( scan );
    }
    while (scan->handoff_remaining && words < 64 && hints < 64 && budget > 0)
    {
        unsigned int n, word;
        UINT64 bits;

        pool = client_surface_compositor_next_pool;
        assert( pool );
        client_surface_compositor_next_pool = pool->next ? pool->next : client_surface_compositor_pools;
        word = pool->next_word;
        pool->next_word = (word + 1) % CLIENT_SURFACE_HANDOFF_BITMAP_WORDS;
        --scan->handoff_remaining;
        ++words;
        ++pool->refs; /* Removing a lost final binding must not unmap the scan. */
        if (pool->query_generation != client_surface_compositor_query_generation)
        {
            unsigned int restored = 0;

            /* Native return alone does not free query capacity: the actor
             * must consume the result. Rearm parked hints once per actual
             * capacity change, without repeatedly reporting FULL as work.
             * New producer READYs can still coalesce or hit warm metadata. */
            for (n = 0; n < CLIENT_SURFACE_HANDOFF_BITMAP_WORDS; ++n)
            {
                restored += __builtin_popcountll( pool->query_wait_bitmap[n] );
                __atomic_fetch_or( &pool->shared->ready_bitmap[n], pool->query_wait_bitmap[n], __ATOMIC_RELEASE );
                pool->query_wait_bitmap[n] = 0;
            }
            pool->query_generation = client_surface_compositor_query_generation;
            if (restored)
                TRACE_(csperf)( "ticks=%llu event=source_query_retry pool=%s generation=%s hints=%u\n",
                               client_surface_perf_time(), wine_dbgstr_longlong( pool->id ),
                               wine_dbgstr_longlong( pool->query_generation ), restored );
        }
        bits = __atomic_load_n( &pool->shared->ready_bitmap[word], __ATOMIC_ACQUIRE );

        while (bits)
        {
            unsigned int bit = __builtin_ctzll( bits ), index = word * 64 + bit;
            struct client_surface_compositor_binding *binding = pool->bindings[index];
            struct client_surface_handoff_channel *channel = &pool->shared->channels[index];
            UINT64 consumed, produced, previous;
            unsigned int frames = 0;

            bits &= bits - 1;
            ++hints;
            if (!binding) continue;
            pool->query_wait_bitmap[word] &= ~((UINT64)1 << bit);
            /* Clear the hint before acquiring the sequence. A concurrent
             * publisher either appears in that load or leaves its bit set. */
            __atomic_fetch_and( &pool->shared->ready_bitmap[word], ~((UINT64)1 << bit), __ATOMIC_ACQ_REL );
            while (!client_surface_cache_read_pending( binding ) &&
                   !__atomic_load_n( &channel->closed, __ATOMIC_ACQUIRE ))
            {
                consumed = __atomic_load_n( &channel->consumer_sequence, __ATOMIC_RELAXED );
                produced = __atomic_load_n( &channel->producer_sequence, __ATOMIC_ACQUIRE );
                if (produced == consumed) break;
                if (produced - consumed > CLIENT_SURFACE_HANDOFF_RING_SIZE)
                {
                    __atomic_store_n( &channel->closed, 1, __ATOMIC_RELEASE );
                    break;
                }
                if (frames++ == CLIENT_SURFACE_HANDOFF_RING_SIZE)
                {
                    __atomic_fetch_or( &pool->shared->ready_bitmap[word], (UINT64)1 << bit, __ATOMIC_RELEASE );
                    break;
                }
                flush_client_surface_copy_batch();
                previous = consumed;
                if (coalesce_client_surface_handoffs( binding, &consumed, produced,
                                                     CLIENT_SURFACE_HANDOFF_RING_SIZE - frames ))
                {
                    progressed |= consumed != previous;
                    budget -= consumed - previous;
                    __atomic_fetch_or( &pool->shared->ready_bitmap[word], (UINT64)1 << bit, __ATOMIC_RELEASE );
                    break;
                }
                budget -= consumed - previous;
                frames += consumed - previous;
                /* A full native queue still consumes an admission attempt. */
                --budget;
                if (!cache_client_surface_handoff( binding, consumed & (CLIENT_SURFACE_HANDOFF_RING_SIZE - 1),
                                                    consumed + 1 ))
                {
                    progressed |= consumed != previous;
                    pool->query_wait_bitmap[word] |= (UINT64)1 << bit;
                    TRACE_(csperf)( "ticks=%llu event=source_query_defer identity=%s cookie=%s token=%s pool=%s hwnd=%p channel=%u\n",
                                   client_surface_perf_time(), wine_dbgstr_longlong( binding->identity ),
                                   wine_dbgstr_longlong( binding->cookie ), wine_dbgstr_longlong( consumed + 1 ),
                                   wine_dbgstr_longlong( pool->id ), binding->window, index );
                    break;
                }
                progressed = TRUE;
            }
            if (!__atomic_load_n( &channel->closed, __ATOMIC_ACQUIRE )) continue;
            flush_client_surface_copy_batch();
            /* Native copies own their images and scalar adoption identity.
             * Retiring this binding cannot invalidate those native inputs. */
            __atomic_fetch_and( &pool->shared->ready_bitmap[word], ~((UINT64)1 << bit), __ATOMIC_ACQ_REL );
            remove_client_surface_compositor_binding( binding );
            progressed = TRUE;
        }
        /* Finish each word so a busy low bit cannot starve its neighbours.
         * Count empty words and unbound hints too. Each pool gets one word
         * per turn; at most 64 words and 127 hints precede the next job slice. */
        flush_client_surface_copy_batch();
        release_client_surface_compositor_pool( pool );
        if (scan->pool_generation != client_surface_compositor_pool_generation)
        {
            init_client_surface_source_scan( scan );
        }
    }
    TRACE_(csperf)( "ticks=%llu event=compositor_handoff_scan pools=%u words=%u hints=%u frames=%u remaining=%u progressed=%u generation=%s\n",
                   client_surface_perf_time(), client_surface_compositor_pool_count, words, hints,
                   CLIENT_SURFACE_COPY_BATCH_SIZE - budget, scan->handoff_remaining, progressed,
                   wine_dbgstr_longlong( scan->pool_generation ) );
    return progressed || budget <= 0;
}

BOOL replay_client_surface_scene_sources( struct client_surface_compositor_target *target,
                                                 unsigned int *budget )
{
    BOOL progressed = FALSE;

    /* Scene replay reads owner-local images. It never claims a returned
     * producer slot or depends on the producer retaining its previous XID. */
    if (!target->scene.valid || target->quiescing || target->transform) return FALSE;
    while (target->replay_member < target->scene.count && *budget)
    {
        struct client_surface_compositor_binding *binding = target->scene.members[target->replay_member];

        --*budget;
        if (binding->latest_image.pixmap && (binding->replay_epoch != target->scene.epoch ||
                                           binding->replay_generation != target->replay_generation))
        {
            BOOL queued = compose_client_surface_cached_frame( binding );

            /* Accepted asynchronous copies advance this bounded scan,
             * but commit their source checkpoint only with their real
             * reply. A newer source resets the scan above. */
            if (!queued && binding->latest_image.pixmap &&
                (binding->replay_epoch != target->scene.epoch ||
                 binding->replay_generation != target->replay_generation)) break;
        }
        ++target->replay_member;
        progressed = TRUE;
        if (target->transform) break;
    }
    flush_client_surface_copy_batch();
    return progressed;
}

BOOL client_surface_frame_copy_pending( const struct client_surface_compositor_frame *frame )
{
    return client_surface_copy_batch.count && client_surface_copy_batch.frame == frame;
}

/* Each transport owns its wake descriptor and parked handshake. The scheduler
 * must arm all transports, then rescan, before blocking on these descriptors. */
void arm_client_surface_sources(void)
{
    struct client_surface_compositor_pool *pool;

    for (pool = client_surface_compositor_pools; pool; pool = pool->next)
    {
        drain_client_surface_notification( pool->ready_fd );
        __atomic_store_n( &pool->shared->ready_parked, 1, __ATOMIC_RELEASE );
    }
}

void init_client_surface_source_scan( struct client_surface_compositor_scan *scan )
{
    scan->pool_generation = client_surface_compositor_pool_generation;
    scan->pool_count = client_surface_compositor_pool_count;
    scan->handoff_remaining = scan->pool_count * CLIENT_SURFACE_HANDOFF_BITMAP_WORDS;
}

unsigned int get_client_surface_source_waiters( const struct client_surface_compositor_scan *scan,
                                               struct pollfd *waiters, unsigned int capacity )
{
    struct client_surface_compositor_pool *pool;
    unsigned int count = 0;

    assert( !scan->handoff_remaining && scan->pool_generation == client_surface_compositor_pool_generation );
    for (pool = client_surface_compositor_pools; pool; pool = pool->next)
    {
        assert( count < capacity );
        waiters[count++] = (struct pollfd){pool->ready_fd, POLLIN, 0};
    }
    return count;
}

BOOL begin_client_surface_composition(void)
{
    BOOL progressed;

    client_surface_compositor_repair_budget = 64;
    client_surface_compositor_repair_pending = FALSE;
    progressed = client_surface_complete_queries( CLIENT_SURFACE_COPY_BATCH_SIZE );
    if (progressed) ++client_surface_compositor_query_generation;
    progressed |= client_surface_complete_cache( CLIENT_SURFACE_COPY_BATCH_SIZE );
    return progressed;
}

BOOL finish_client_surface_composition(void)
{
    TRACE_(csperf)( "ticks=%llu event=compositor_repair_slice inspected=%u pending=%u\n",
                   client_surface_perf_time(), 64 - client_surface_compositor_repair_budget,
                   client_surface_compositor_repair_pending );
    return client_surface_compositor_repair_pending;
}
