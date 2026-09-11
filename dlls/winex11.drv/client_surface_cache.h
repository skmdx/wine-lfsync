/* Native cache storage owns its connection, pending work and final release. */

#ifndef __WINE_CLIENT_SURFACE_CACHE_H
#define __WINE_CLIENT_SURFACE_CACHE_H

struct client_surface_cache_image;
/* Embedded work owns its storage through finished(). Prepare before creating
 * native resources; submit cannot allocate or start a worker. */
struct client_surface_native_work
{
    struct client_surface_native_work *next;
    void (*execute)( struct client_surface_native_work *work );
    void (*finished)( struct client_surface_native_work *work );
};
BOOL client_surface_prepare_native_work(void);
void client_surface_submit_native_work( struct client_surface_native_work *work );
BOOL x11drv_reserve_release_capacity( unsigned int count, SIZE_T bytes );
void x11drv_return_release_capacity( unsigned int count, SIZE_T bytes );

typedef void (*client_surface_cache_callback)( void *context, BOOL success );

/* Admission does no native I/O. The caller retains its source and callback
 * context until completion is delivered by client_surface_complete_cache(). */
struct client_surface_cache_image *client_surface_cache_create(
    const struct client_surface_memory_scope *memory, unsigned int width, unsigned int height,
    unsigned int depth, UINT64 bytes, void (*wake)(void),
    client_surface_cache_callback complete, void *context );

/* Output admission retains its own purpose charge. Native storage has no
 * borrowed target pointer; the caller validates installation on completion. */
struct client_surface_cache_image *client_surface_cache_create_output(
    const struct client_surface_memory_scope *memory, Window window, unsigned int width, unsigned int height,
    unsigned int depth, UINT64 bytes, void (*wake)(void),
    client_surface_cache_callback complete, void *context );

/* Reserve both records, OUTPUT charges and release executors before native
 * pair creation. Failure rolls back without native I/O. Each record retains
 * every returned XID, including a failed allocation, until native reclaim. */
BOOL client_surface_cache_reserve_output_pair(
    const struct client_surface_memory_scope *memory, UINT64 bytes_per_image,
    void (*wake)(void), struct client_surface_cache_image *images[2] );
/* Both reserved images report once on the actor. The context must retain
 * both records until both callbacks arrive, including after cancellation. */
void client_surface_cache_create_output_pair( struct client_surface_cache_image *images[2],
                                              unsigned int width, unsigned int height, unsigned int depth,
                                              client_surface_cache_callback complete, void *context );

/* These descriptors are available after successful creation. The GC is for
 * checked XCB reads only; Xlib drawing uses a separate, private GC. */
Pixmap client_surface_cache_pixmap( const struct client_surface_cache_image *image );
unsigned int client_surface_cache_gc( const struct client_surface_cache_image *image );
struct client_surface_cache_image *client_surface_cache_acquire( struct client_surface_cache_image *image );
BOOL client_surface_cache_shared( const struct client_surface_cache_image *image );
void client_surface_cache_copy( struct client_surface_cache_image *image, Pixmap source,
                                client_surface_cache_callback complete, void *context );
/* Copy a completed, independently retained OUTPUT rectangle into private
 * storage. The caller owns the source read until this callback returns. */
void client_surface_cache_copy_output( struct client_surface_cache_image *image, Pixmap source,
                                       unsigned int width, unsigned int height,
                                       client_surface_cache_callback complete, void *context );

/* Each output read holds a reference through its checked reply. A shared
 * image is immutable; replace a shared spare before beginning another write.
 * Release needs no allocation and retains charges through actual reclaim. */
void client_surface_cache_release( struct client_surface_cache_image *image );
BOOL client_surface_complete_cache( unsigned int budget );

#endif
