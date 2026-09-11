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

enum cache_operation { CACHE_IDLE, CACHE_CREATE, CACHE_COPY, CACHE_RELEASE, CACHE_TRANSFORM };
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
    const struct client_surface_cache_transform *transform;
    unsigned int transform_count;
    Pixmap pixmap, source;
    Window window;
    GC gc, transfer_gc;
    unsigned int width, height, depth;
    unsigned int copy_width, copy_height;
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

static unsigned long convert_client_surface_component( unsigned long pixel,
                                                        unsigned long source_mask,
                                                        unsigned long destination_mask )
{
    unsigned int source_shift = 0, destination_shift = 0;
    UINT64 value, source_max, destination_max;

    if (!destination_mask) return 0;
    if (!source_mask) return destination_mask;
    while (!(source_mask & (1ul << source_shift))) ++source_shift;
    while (!(destination_mask & (1ul << destination_shift))) ++destination_shift;
    source_max = source_mask >> source_shift;
    destination_max = destination_mask >> destination_shift;
    value = (pixel & source_mask) >> source_shift;
    return ((value * destination_max + source_max / 2) / source_max) << destination_shift;
}

BOOL client_surface_copy_image( Display *display, struct client_surface_memory_scope *memory,
                                       Pixmap source, Pixmap destination,
                                       GC gc, VisualID source_id, VisualID destination_id,
                                       unsigned int source_width, unsigned int source_height,
                                       const RECT *rect )
{
    XVisualInfo source_template = {.visualid = source_id};
    XVisualInfo destination_template = {.visualid = destination_id};
    XVisualInfo *source_visual = NULL, *destination_visual = NULL;
    const XPixmapFormatValues *format;
    XImage *input = NULL, *output = NULL;
    unsigned int width = rect->right - rect->left, height = rect->bottom - rect->top;
    unsigned int x, y, source_y;
    unsigned long source_alpha, destination_alpha;
    UINT64 input_bytes, output_bytes, reserved = 0;
    int count;
    BOOL ret = FALSE;

    /* Keep the exceptional conversion path on the owner connection too.
     * The caller retains the source until its final XSync, covering both readback
     * and upload. The destination GC carries the exact scene clip. */
    if (!(source_visual = XGetVisualInfo( display, VisualIDMask, &source_template, &count )) ||
        !count || (source_visual->class != TrueColor && source_visual->class != DirectColor))
        goto done;
    if (!(destination_visual = XGetVisualInfo( display, VisualIDMask, &destination_template, &count )) ||
        !count || (destination_visual->class != TrueColor && destination_visual->class != DirectColor))
        goto done;
    if (!(output = XCreateImage( display, destination_visual->visual, destination_visual->depth,
                                ZPixmap, 0, NULL, width, height, 32, 0 )))
        goto done;
    if (output->bytes_per_line <= 0 || height > ~(SIZE_T)0 / output->bytes_per_line)
        goto done;
    if (!(format = pixmap_formats[source_visual->depth]) || format->bits_per_pixel <= 0 ||
        format->scanline_pad <= 0 || format->scanline_pad % 8)
        goto done;
    input_bytes = ((UINT64)source_width * format->bits_per_pixel + format->scanline_pad - 1) /
                  format->scanline_pad * (format->scanline_pad / 8);
    if (!input_bytes || source_height > ~(UINT64)0 / input_bytes) goto done;
    input_bytes *= source_height;
    output_bytes = (UINT64)output->bytes_per_line * height;
    if (input_bytes > ~(UINT64)0 - output_bytes ||
        !client_surface_reserve_scoped_memory( memory, CLIENT_SURFACE_MEMORY_STAGING, input_bytes + output_bytes ))
        goto done;
    reserved = input_bytes + output_bytes;
    if (!(input = XGetImage( display, source, 0, 0, source_width, source_height,
                             AllPlanes, ZPixmap )) ||
        !(output->data = calloc( height, output->bytes_per_line )))
        goto done;
    source_alpha = ((1ull << source_visual->depth) - 1) &
                   ~(source_visual->red_mask | source_visual->green_mask | source_visual->blue_mask);
    destination_alpha = ((1ull << destination_visual->depth) - 1) &
                        ~(destination_visual->red_mask | destination_visual->green_mask |
                          destination_visual->blue_mask);
    for (y = 0; y < height; ++y)
    {
        source_y = (UINT64)y * source_height / height;
        for (x = 0; x < width; ++x)
        {
            unsigned long pixel = XGetPixel( input, (UINT64)x * source_width / width, source_y );
            unsigned long converted =
                convert_client_surface_component( pixel, source_visual->red_mask, destination_visual->red_mask ) |
                convert_client_surface_component( pixel, source_visual->green_mask, destination_visual->green_mask ) |
                convert_client_surface_component( pixel, source_visual->blue_mask, destination_visual->blue_mask ) |
                convert_client_surface_component( pixel, source_alpha, destination_alpha );

            XPutPixel( output, x, y, converted );
        }
    }
    XPutImage( display, destination, gc, output, 0, 0, rect->left, rect->top, width, height );
    TRACE( "software owner copy %ux%u to %ux%u visual %#lx -> %#lx\n",
           source_width, source_height, width, height, source_id, destination_id );
    ret = TRUE;
done:
    if (output) XDestroyImage( output );
    if (input) XDestroyImage( input );
    client_surface_release_scoped_memory( memory, CLIENT_SURFACE_MEMORY_STAGING, reserved );
    if (destination_visual) XFree( destination_visual );
    if (source_visual) XFree( source_visual );
    return ret;
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
    XGCValues values = {.graphics_exposures = False};

    worker->error = 0;
    /* OUTPUT creation only reserves the Pixmap. Its first private copy owns
     * this GC, including a failed native allocation, through actual reclaim. */
    if (!image->gc) image->gc = XCreateGC( worker->display, image->pixmap, GCGraphicsExposures, &values );
    if (image->gc)
    {
        XSetClipMask( worker->display, image->gc, None );
        XSetClipOrigin( worker->display, image->gc, 0, 0 );
        XCopyArea( worker->display, image->source, image->pixmap, image->gc,
                   0, 0, image->copy_width, image->copy_height, 0, 0 );
    }
    XSync( worker->display, False );
    image->success = image->gc && !worker->error;
    TRACE_(csperf)( "ticks=%llu event=%s image=%p source=%lx destination=%lx "
                   "display=%p width=%u height=%u error=%d sync_calls=1 success=%u\n", cache_time(),
                   image->purpose == CLIENT_SURFACE_MEMORY_OUTPUT ? "output_pair_native_copy" : "cache_native_fallback",
                   image, image->source, image->pixmap, worker->display, image->copy_width,
                   image->copy_height, worker->error, image->success );
}

static void transform_cache_image( struct client_surface_cache_image *image )
{
    const struct client_surface_cache_transform *transform = image->transform;
    struct cache_worker *worker = image->worker;
    Display *display = worker->display;
    XGCValues values = {.graphics_exposures = False};
    BOOL copied = TRUE, rendered = FALSE;
    unsigned int index = 0;
    int error;

    worker->error = 0;
    if (!image->gc) image->gc = XCreateGC( display, image->pixmap, GCGraphicsExposures, &values );
    for (; image->gc && copied && index < image->transform_count; ++index, transform = transform->next)
    {
        assert( transform && (!index || !transform->catchup) );
        rendered = FALSE;
        XSetClipMask( display, image->gc, None );
        XSetClipOrigin( display, image->gc, 0, 0 );
        if (transform->catchup)
        {
            const RECT *rect = &transform->catchup_rect;

            XCopyArea( display, transform->catchup, image->pixmap, image->gc,
                       rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                       rect->left, rect->top );
        }
        if (transform->clip_count)
        {
            if (transform->clipped)
                XSetClipRectangles( display, image->gc, transform->destination.left,
                                    transform->destination.top, (XRectangle *)transform->clips,
                                    transform->clip_count, YXBanded );
            if (transform->native)
            {
                const RECT *rect = &transform->source_damage;

                XCopyArea( display, transform->source, image->pixmap, image->gc,
                           rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                           transform->destination.left + rect->left, transform->destination.top + rect->top );
            }
            else
            {
                rendered = X11DRV_XRender_CopyClientSurface( display, transform->source,
                    transform->source_visual, image->pixmap, transform->destination_visual,
                    transform->source_width, transform->source_height, &transform->destination,
                    transform->clipped ? transform->clips : NULL,
                    transform->clipped ? transform->clip_count : 0, 0, 0 );
                if (!rendered)
                    copied = client_surface_copy_image( display, &image->memory, transform->source,
                        image->pixmap, image->gc, transform->source_visual, transform->destination_visual,
                        transform->source_width, transform->source_height, &transform->destination );
            }
        }
        if (image->transform_count > 1)
            TRACE_(csperf)( "ticks=%llu event=output_transform_command image=%p command=%p index=%u count=%u "
                           "source=%lx destination=%lx catchup=%lx native=%u rendered=%u copied=%u\n", cache_time(),
                           image, transform, index, image->transform_count, transform->source, image->pixmap,
                           transform->catchup, transform->native, rendered, copied );
    }
    XSync( display, False );
    error = worker->error;
    image->success = image->gc && copied && !error;
    /* A failed GC allocation can still return an Xlib handle. Retire it on
     * its private connection before returning this image to actor reuse. */
    if (!image->success && image->gc)
    {
        XFreeGC( display, image->gc );
        image->gc = NULL;
        XSync( display, False );
    }
    TRACE_(csperf)( "ticks=%llu event=output_transform_native image=%p source=%lx destination=%lx "
                   "catchup=%lx display=%p rendered=%u error=%d success=%u count=%u executed=%u\n", cache_time(),
                   image, image->transform->source, image->pixmap, image->transform->catchup, display,
                   rendered, error, image->success, image->transform_count, index );
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
    case CACHE_TRANSFORM: transform_cache_image( image ); break;
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
    image->copy_width = image->width;
    image->copy_height = image->height;
    queue_cache_image( image, CACHE_COPY, complete, context );
    pthread_mutex_unlock( &cache_mutex );
}

void client_surface_cache_copy_output( struct client_surface_cache_image *image, Pixmap source,
                                       unsigned int width, unsigned int height,
                                       client_surface_cache_callback complete, void *context )
{
    pthread_mutex_lock( &cache_mutex );
    assert( image->acquired && image->refs == 1 && image->purpose == CLIENT_SURFACE_MEMORY_OUTPUT );
    assert( source && width && height && width <= image->width && height <= image->height );
    image->source = source;
    image->copy_width = width;
    image->copy_height = height;
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

BOOL client_surface_cache_write_pending( const struct client_surface_cache_image *image )
{
    BOOL pending;

    pthread_mutex_lock( &cache_mutex );
    pending = image->operation != CACHE_IDLE;
    pthread_mutex_unlock( &cache_mutex );
    return pending;
}

BOOL client_surface_cache_transform_output( struct client_surface_cache_image *image,
    const struct client_surface_cache_transform *transform, unsigned int count,
    client_surface_cache_callback complete, void *context )
{
    const struct client_surface_cache_transform *command = transform;
    unsigned int i;

    assert( count && count <= CLIENT_SURFACE_CACHE_TRANSFORM_LIMIT );
    for (i = 0; i < count; ++i)
    {
        assert( command && (!i || !command->catchup) );
        command = command->next;
    }
    assert( !command );
    pthread_mutex_lock( &cache_mutex );
    assert( image->purpose == CLIENT_SURFACE_MEMORY_OUTPUT );
    if (!image->acquired || image->refs != 1 || image->operation != CACHE_IDLE)
    {
        pthread_mutex_unlock( &cache_mutex );
        return FALSE;
    }
    ++image->refs;
    TRACE_(csperf)( "ticks=%llu event=cache_image_reference image=%p pixmap=%lx acquire=1 refs=%u\n",
                   cache_time(), image, image->pixmap, image->refs );
    image->transform = transform;
    image->transform_count = count;
    queue_cache_image( image, CACHE_TRANSFORM, complete, context );
    pthread_mutex_unlock( &cache_mutex );
    return TRUE;
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
        image->transform = NULL;
        image->transform_count = 0;
        pthread_mutex_unlock( &cache_mutex );
        image->complete( image->context, image->success );
        progressed = TRUE;
    }
    return progressed;
}
