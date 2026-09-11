/*
 * Independently owned native storage for X11 client surface caches
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
#include <fcntl.h>

#include "x11drv.h"
#include "client_surface.h"
#include "client_surface_cache.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);
WINE_DECLARE_DEBUG_CHANNEL(csperf);

enum cache_operation { CACHE_IDLE, CACHE_CREATE, CACHE_COPY, CACHE_RELEASE };
enum cache_image_kind { CACHE_IMAGE_SOURCE, CACHE_IMAGE_OUTPUT_MAILBOX, CACHE_IMAGE_OUTPUT_PAIR };

struct cache_worker
{
    pthread_cond_t cond;
    struct client_surface_native_work *head, **tail;
    unsigned int pending;
    Display *display;
    struct x11drv_error_handler errors;
    int error;
};

struct client_surface_cache_image
{
    struct client_surface_native_work work;
    struct client_surface_memory_scope memory;
    struct client_surface_cache_image *next;
    struct cache_worker *worker;
    enum cache_operation operation;
    enum cache_image_kind kind;
    client_surface_cache_callback complete;
    void *context;
    Pixmap pixmap, source;
    Window window;
    GC gc, transfer_gc;
    unsigned int width, height, depth;
    unsigned int xcb_gc;
    unsigned int refs;
    enum client_surface_memory_class purpose;
    UINT64 bytes;
    BOOL acquired, success;
};

/* The bound includes live, queued, executing, completed and retiring images.
 * Each admitted image already owns its work/release node. Scoped byte limits
 * apply independently. No native operation holds the scheduler mutex.
 * Connections belong to workers for the process lifetime, never to an owner
 * target. Images keep one connection for allocation, fallback reads and
 * destruction, independently of the GUI and compositor connections. */
#define CLIENT_SURFACE_CACHE_IMAGE_LIMIT 8192
/* Source churn must not consume admission reserved for active output. This
 * is part of the same total bound, not an additional uncharged image pool. */
#define CLIENT_SURFACE_CACHE_OUTPUT_RESERVE 1024
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct cache_worker cache_workers[4];
static unsigned int worker_count, image_count, source_count, next_worker;
static struct client_surface_cache_image *completed_head, **completed_tail = &completed_head;
static void (*cache_wake)(void);

static const char *cache_image_kind( const struct client_surface_cache_image *image )
{
    static const char *const names[] = {"owner_cache", "output_mailbox", "output_pair"};

    return names[image->kind];
}

static unsigned long long cache_time(void)
{
    LARGE_INTEGER counter;

    NtQueryPerformanceCounter( &counter, NULL );
    return counter.QuadPart;
}

static int cache_error( Display *display, XErrorEvent *event, void *arg )
{
    struct cache_worker *worker = arg;

    worker->error = event->error_code;
    return TRUE;
}

static BOOL open_cache_display( struct cache_worker *worker )
{
    if (worker->display) return TRUE;
    if (!(worker->display = XOpenDisplay( DisplayString( gdi_display ) ))) return FALSE;
    worker->errors.display = worker->display;
    worker->errors.callback = cache_error;
    worker->errors.arg = worker;
    X11DRV_register_error_handler( &worker->errors );
    if (fcntl( ConnectionNumber( worker->display ), F_SETFD, FD_CLOEXEC ) == -1)
    {
        XCloseDisplay( worker->display );
        X11DRV_unregister_error_handler( &worker->errors );
        worker->display = NULL;
        return FALSE;
    }
    return TRUE;
}

static void create_cache_image( struct client_surface_cache_image *image )
{
    struct cache_worker *worker = image->worker;
    XGCValues values = {.graphics_exposures = False};
    Display *display;

    if (!open_cache_display( worker )) return;
    display = worker->display;
    worker->error = 0;
    image->pixmap = XCreatePixmap( display, root_window, image->width, image->height, image->depth );
    if (image->pixmap)
    {
        image->gc = XCreateGC( display, image->pixmap, GCGraphicsExposures, &values );
        image->transfer_gc = XCreateGC( display, image->pixmap, GCGraphicsExposures, &values );
    }
    XSync( display, False );
    image->success = image->pixmap && image->gc && image->transfer_gc && !worker->error;
    TRACE_(csperf)( "ticks=%llu event=cache_native_alloc image=%p pixmap=%lx display=%p error=%d "
                   "sync_calls=1 success=%u worker=%u\n", cache_time(), image, image->pixmap, display,
                   worker->error, image->success, (unsigned int)(worker - cache_workers) );
    if ((image->acquired = image->success))
    {
        image->xcb_gc = XGContextFromGC( image->transfer_gc );
        x11drv_client_surface_trace_image( "acquire", "owner_cache", display, image->pixmap, image->bytes );
    }
}

static void copy_cache_image( struct client_surface_cache_image *image )
{
    struct cache_worker *worker = image->worker;

    worker->error = 0;
    XCopyArea( worker->display, image->source, image->pixmap, image->gc,
               0, 0, image->width, image->height, 0, 0 );
    XSync( worker->display, False );
    image->success = !worker->error;
    TRACE_(csperf)( "ticks=%llu event=cache_native_fallback image=%p source=%lx destination=%lx "
                   "display=%p error=%d sync_calls=1\n", cache_time(), image, image->source,
                   image->pixmap, worker->display, worker->error );
}

static void create_output_image( struct client_surface_cache_image *image )
{
    struct cache_worker *worker = image->worker;
    Display *display;

    if (!open_cache_display( worker )) return;
    display = worker->display;
    worker->error = 0;
    image->pixmap = XCreatePixmap( display, root_window, image->width, image->height, image->depth );
    XSync( display, False );
    image->success = image->pixmap && !worker->error;
    TRACE_(csperf)( "ticks=%llu event=%s image=%p window=%lx pixmap=%lx display=%p "
                   "width=%u height=%u depth=%u sync_calls=1 error=%d success=%u\n",
                   cache_time(), image->kind == CACHE_IMAGE_OUTPUT_PAIR ? "output_pair_native_alloc" : "output_mailbox_alloc",
                   image, image->window, image->pixmap, display, image->width, image->height,
                   image->depth, worker->error, image->success );
    if ((image->acquired = image->success))
        x11drv_client_surface_trace_image( "acquire", cache_image_kind( image ), display, image->pixmap, image->bytes );
}

static void destroy_cache_image( struct client_surface_cache_image *image )
{
    struct cache_worker *worker = image->worker;
    Display *display = worker->display;

    /* Every native object was created on this worker's process-lifetime
     * connection. An open failure can leave only an empty image record. */
    assert( display || (!image->gc && !image->transfer_gc && !image->pixmap) );
    worker->error = 0;
    if (image->gc) XFreeGC( display, image->gc );
    if (image->transfer_gc) XFreeGC( display, image->transfer_gc );
    if (image->pixmap) XFreePixmap( display, image->pixmap );
    if (image->gc || image->transfer_gc || image->pixmap) XSync( display, False );
    TRACE_(csperf)( "ticks=%llu event=cache_native_free image=%p pixmap=%lx display=%p error=%d "
                   "owner_display=%p kind=%s\n", cache_time(), image, image->pixmap, display, worker->error,
                   display, cache_image_kind( image ) );
    if (image->acquired)
        x11drv_client_surface_trace_image( "free", cache_image_kind( image ), display, image->pixmap, image->bytes );
    client_surface_release_scoped_memory( &image->memory, image->purpose, image->bytes );
}

static void execute_cache_image( struct client_surface_native_work *work )
{
    struct client_surface_cache_image *image = CONTAINING_RECORD( work, struct client_surface_cache_image, work );

    switch (image->operation)
    {
    case CACHE_CREATE:
        if (image->purpose == CLIENT_SURFACE_MEMORY_SOURCE) create_cache_image( image );
        else create_output_image( image );
        break;
    case CACHE_COPY: copy_cache_image( image ); break;
    case CACHE_RELEASE: destroy_cache_image( image ); break;
    default: assert( 0 );
    }
}

static void finish_cache_image( struct client_surface_native_work *work )
{
    struct client_surface_cache_image *image = CONTAINING_RECORD( work, struct client_surface_cache_image, work );
    struct cache_worker *worker = image->worker;
    enum cache_operation operation = image->operation;
    void (*wake)(void);

    pthread_mutex_lock( &cache_mutex );
    if (operation == CACHE_RELEASE)
    {
        --image_count;
        if (image->purpose == CLIENT_SURFACE_MEMORY_SOURCE) --source_count;
    }
    else
    {
        image->next = NULL;
        *completed_tail = image;
        completed_tail = &image->next;
    }
    TRACE_(csperf)( "ticks=%llu event=cache_image_return image=%p operation=%u worker=%u pending=%u count=%u\n",
                   cache_time(), image, operation, (unsigned int)(worker - cache_workers), worker->pending, image_count );
    wake = cache_wake;
    pthread_mutex_unlock( &cache_mutex );
    if (operation == CACHE_RELEASE)
        client_surface_free_owned_metadata( &image->memory, image, sizeof(*image) );
    /* The actor may immediately release the completed object's context. */
    wake();
}

static void cache_worker_thread( void *context )
{
    struct cache_worker *worker = context;
    struct client_surface_native_work *work;

    for (;;)
    {
        pthread_mutex_lock( &cache_mutex );
        while (!(work = worker->head)) pthread_cond_wait( &worker->cond, &cache_mutex );
        if (!(worker->head = work->next)) worker->tail = &worker->head;
        pthread_mutex_unlock( &cache_mutex );
        work->execute( work );
        pthread_mutex_lock( &cache_mutex );
        --worker->pending;
        pthread_mutex_unlock( &cache_mutex );
        /* finished may free the embedded work. No access follows it. */
        work->finished( work );
    }
}

static struct cache_worker *create_cache_worker(void)
{
    struct cache_worker *worker = &cache_workers[worker_count];
    HANDLE thread;

    assert( worker_count < ARRAY_SIZE(cache_workers) );
    worker->tail = &worker->head;
    if (pthread_cond_init( &worker->cond, NULL )) return NULL;
    if (PsCreateSystemThread( &thread, THREAD_ALL_ACCESS, NULL, 0, NULL, cache_worker_thread, worker ))
    {
        pthread_cond_destroy( &worker->cond );
        return NULL;
    }
    ++worker_count;
    NtClose( thread );
    return worker;
}

static struct cache_worker *select_existing_cache_worker(void)
{
    struct cache_worker *worker = NULL;
    unsigned int i, index;

    for (i = 0; i < worker_count; ++i)
    {
        index = (next_worker + i) % worker_count;
        if (!worker || cache_workers[index].pending < worker->pending) worker = &cache_workers[index];
    }
    if (worker) next_worker = (worker - cache_workers + 1) % worker_count;
    return worker;
}

static struct cache_worker *select_cache_worker(void)
{
    struct cache_worker *worker = select_existing_cache_worker(), *created;

    if ((!worker || worker->pending) && worker_count < ARRAY_SIZE(cache_workers))
        if ((created = create_cache_worker())) worker = created;
    if (worker) next_worker = (worker - cache_workers + 1) % worker_count;
    return worker;
}

static void queue_native_work( struct cache_worker *worker, struct client_surface_native_work *work )
{
    work->next = NULL;
    *worker->tail = work;
    worker->tail = &work->next;
    ++worker->pending;
    pthread_cond_signal( &worker->cond );
}

BOOL client_surface_prepare_native_work(void)
{
    BOOL ret = TRUE;

    pthread_mutex_lock( &cache_mutex );
    /* All close executors exist before acquiring native resources. A stalled
     * close must not require the next GUI release to create its peer worker. */
    while (worker_count < ARRAY_SIZE(cache_workers))
        if (!create_cache_worker()) { ret = FALSE; break; }
    pthread_mutex_unlock( &cache_mutex );
    return ret;
}

void client_surface_submit_native_work( struct client_surface_native_work *work )
{
    pthread_mutex_lock( &cache_mutex );
    assert( worker_count );
    queue_native_work( select_existing_cache_worker(), work );
    pthread_mutex_unlock( &cache_mutex );
}

static void queue_cache_image( struct client_surface_cache_image *image, enum cache_operation operation,
                               client_surface_cache_callback complete, void *context )
{
    struct cache_worker *worker = image->worker;

    assert( image->operation == CACHE_IDLE );
    image->operation = operation;
    image->complete = complete;
    image->context = context;
    image->work.execute = execute_cache_image;
    image->work.finished = finish_cache_image;
    queue_native_work( worker, &image->work );
    TRACE_(csperf)( "ticks=%llu event=cache_image_queue image=%p operation=%u worker=%u pending=%u count=%u purpose=%u\n",
                   cache_time(), image, operation, (unsigned int)(worker - cache_workers), worker->pending, image_count,
                   image->purpose );
}

static void discard_cache_image( struct client_surface_cache_image *image )
{
    if (!image) return;
    assert( image->operation == CACHE_IDLE && !image->pixmap && !image->acquired );
    client_surface_release_scoped_memory( &image->memory, image->purpose, image->bytes );
    client_surface_free_owned_metadata( &image->memory, image, sizeof(*image) );
}

static struct client_surface_cache_image *allocate_cache_image(
    const struct client_surface_memory_scope *owners, enum client_surface_memory_class purpose, UINT64 bytes )
{
    struct client_surface_memory_scope memory = {0};
    struct client_surface_cache_image *image;

    client_surface_memory_scope_copy( &memory, owners, TRUE );
    if (!(image = client_surface_alloc_scoped_metadata( &memory, 1, sizeof(*image) )))
    {
        client_surface_memory_scope_destroy( &memory );
        return NULL;
    }
    image->memory = memory;
    image->refs = 1;
    image->purpose = purpose;
    image->kind = purpose == CLIENT_SURFACE_MEMORY_SOURCE ? CACHE_IMAGE_SOURCE : CACHE_IMAGE_OUTPUT_MAILBOX;
    if (!client_surface_reserve_scoped_memory( &image->memory, purpose, bytes ))
    {
        discard_cache_image( image );
        return NULL;
    }
    image->bytes = bytes;
    return image;
}

static struct client_surface_cache_image *create_client_surface_cache_image(
    const struct client_surface_memory_scope *owners, enum client_surface_memory_class purpose,
    Window window, unsigned int width, unsigned int height,
    unsigned int depth, UINT64 bytes, void (*wake)(void),
    client_surface_cache_callback complete, void *context )
{
    struct client_surface_cache_image *image;

    if (!(image = allocate_cache_image( owners, purpose, bytes ))) return NULL;
    image->window = window;
    image->width = width;
    image->height = height;
    image->depth = depth;
    pthread_mutex_lock( &cache_mutex );
    assert( !cache_wake || cache_wake == wake );
    cache_wake = wake;
    if (image_count == CLIENT_SURFACE_CACHE_IMAGE_LIMIT ||
        (purpose == CLIENT_SURFACE_MEMORY_SOURCE &&
         source_count == CLIENT_SURFACE_CACHE_IMAGE_LIMIT - CLIENT_SURFACE_CACHE_OUTPUT_RESERVE) ||
        !(image->worker = select_cache_worker()))
    {
        pthread_mutex_unlock( &cache_mutex );
        goto failed;
    }
    ++image_count;
    if (purpose == CLIENT_SURFACE_MEMORY_SOURCE) ++source_count;
    queue_cache_image( image, CACHE_CREATE, complete, context );
    pthread_mutex_unlock( &cache_mutex );
    return image;

failed:
    discard_cache_image( image );
    return NULL;
}

struct client_surface_cache_image *client_surface_cache_create(
    const struct client_surface_memory_scope *owners, unsigned int width, unsigned int height,
    unsigned int depth, UINT64 bytes, void (*wake)(void),
    client_surface_cache_callback complete, void *context )
{
    return create_client_surface_cache_image( owners, CLIENT_SURFACE_MEMORY_SOURCE, None,
                                              width, height, depth, bytes, wake, complete, context );
}

struct client_surface_cache_image *client_surface_cache_create_output(
    const struct client_surface_memory_scope *owners, Window window, unsigned int width, unsigned int height,
    unsigned int depth, UINT64 bytes, void (*wake)(void),
    client_surface_cache_callback complete, void *context )
{
    return create_client_surface_cache_image( owners, CLIENT_SURFACE_MEMORY_OUTPUT, window,
                                              width, height, depth, bytes, wake, complete, context );
}

BOOL client_surface_cache_reserve_output_pair(
    const struct client_surface_memory_scope *memory, UINT64 bytes_per_image,
    void (*wake)(void), struct client_surface_cache_image *images[2] )
{
    unsigned int i;

    images[0] = images[1] = NULL;
    for (i = 0; i < 2; ++i)
    {
        if (!(images[i] = allocate_cache_image( memory, CLIENT_SURFACE_MEMORY_OUTPUT, bytes_per_image ))) goto failed;
        images[i]->kind = CACHE_IMAGE_OUTPUT_PAIR;
    }
    pthread_mutex_lock( &cache_mutex );
    assert( !cache_wake || cache_wake == wake );
    cache_wake = wake;
    if (image_count > CLIENT_SURFACE_CACHE_IMAGE_LIMIT - 2 || !(images[0]->worker = select_cache_worker()))
    {
        pthread_mutex_unlock( &cache_mutex );
        goto failed;
    }
    /* The first admitted worker also guarantees a release executor for the
     * second record if another worker cannot be started. Creation is queued
     * only after the caller has initialized the shared completion context. */
    images[1]->worker = select_cache_worker();
    assert( images[1]->worker );
    image_count += 2;
    TRACE_(csperf)( "ticks=%llu event=output_pair_reserve first=%p second=%p bytes=%llu count=%u\n",
                   cache_time(), images[0], images[1], (unsigned long long)bytes_per_image, image_count );
    pthread_mutex_unlock( &cache_mutex );
    return TRUE;

failed:
    discard_cache_image( images[0] );
    discard_cache_image( images[1] );
    images[0] = images[1] = NULL;
    return FALSE;
}

void client_surface_cache_create_output_pair( struct client_surface_cache_image *images[2],
                                              unsigned int width, unsigned int height, unsigned int depth,
                                              client_surface_cache_callback complete, void *context )
{
    struct client_surface_cache_image *image;
    unsigned int i;

    pthread_mutex_lock( &cache_mutex );
    for (i = 0; i < 2; ++i)
    {
        image = images[i];
        assert( image && image->kind == CACHE_IMAGE_OUTPUT_PAIR && image->operation == CACHE_IDLE &&
                image->refs == 1 && !image->pixmap );
        image->width = width;
        image->height = height;
        image->depth = depth;
        queue_cache_image( image, CACHE_CREATE, complete, context );
    }
    pthread_mutex_unlock( &cache_mutex );
}

Pixmap client_surface_cache_pixmap( const struct client_surface_cache_image *image )
{
    assert( image->acquired && image->operation == CACHE_IDLE );
    return image->pixmap;
}

unsigned int client_surface_cache_gc( const struct client_surface_cache_image *image )
{
    assert( image->acquired && image->operation == CACHE_IDLE );
    return image->xcb_gc;
}

void client_surface_cache_copy( struct client_surface_cache_image *image, Pixmap source,
                                client_surface_cache_callback complete, void *context )
{
    pthread_mutex_lock( &cache_mutex );
    assert( image->acquired && image->refs == 1 && image->purpose == CLIENT_SURFACE_MEMORY_SOURCE );
    image->source = source;
    queue_cache_image( image, CACHE_COPY, complete, context );
    pthread_mutex_unlock( &cache_mutex );
}

struct client_surface_cache_image *client_surface_cache_acquire( struct client_surface_cache_image *image )
{
    pthread_mutex_lock( &cache_mutex );
    assert( image->acquired && image->operation == CACHE_IDLE && image->refs );
    ++image->refs;
    TRACE_(csperf)( "ticks=%llu event=cache_image_reference image=%p pixmap=%lx acquire=1 refs=%u\n",
                   cache_time(), image, image->pixmap, image->refs );
    pthread_mutex_unlock( &cache_mutex );
    return image;
}

BOOL client_surface_cache_shared( const struct client_surface_cache_image *image )
{
    BOOL shared;

    pthread_mutex_lock( &cache_mutex );
    shared = image->refs > 1;
    pthread_mutex_unlock( &cache_mutex );
    return shared;
}

void client_surface_cache_release( struct client_surface_cache_image *image )
{
    if (!image) return;
    pthread_mutex_lock( &cache_mutex );
    assert( image->refs );
    --image->refs;
    TRACE_(csperf)( "ticks=%llu event=cache_image_reference image=%p pixmap=%lx acquire=0 refs=%u\n",
                   cache_time(), image, image->pixmap, image->refs );
    if (!image->refs)
    {
        if (image->acquired)
            x11drv_client_surface_trace_image( "retire", cache_image_kind( image ),
                                              image->worker->display,
                                              image->pixmap, image->bytes );
        queue_cache_image( image, CACHE_RELEASE, NULL, NULL );
    }
    pthread_mutex_unlock( &cache_mutex );
}

BOOL client_surface_complete_cache( unsigned int budget )
{
    struct client_surface_cache_image *image;
    BOOL progressed = FALSE;

    while (budget--)
    {
        pthread_mutex_lock( &cache_mutex );
        if (!(image = completed_head))
        {
            pthread_mutex_unlock( &cache_mutex );
            break;
        }
        if (!(completed_head = image->next)) completed_tail = &completed_head;
        image->operation = CACHE_IDLE;
        pthread_mutex_unlock( &cache_mutex );
        image->complete( image->context, image->success );
        progressed = TRUE;
    }
    return progressed;
}
