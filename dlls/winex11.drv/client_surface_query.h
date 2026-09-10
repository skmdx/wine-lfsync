/* Native queries own only scalar inputs and results, never actor state. */

#ifndef __WINE_CLIENT_SURFACE_QUERY_H
#define __WINE_CLIENT_SURFACE_QUERY_H

struct client_surface_geometry_query
{
    struct client_surface_geometry_query *next;
    void (*complete)( struct client_surface_geometry_query *query );
    Pixmap pixmap;
    unsigned int min_width, min_height;
    unsigned int width, height, depth;
    BOOL success;
};

enum client_surface_query_status
{
    CLIENT_SURFACE_QUERY_FAILED,
    CLIENT_SURFACE_QUERY_ACCEPTED,
    CLIENT_SURFACE_QUERY_FULL,
};

/* The caller supplies admitted storage and pins the input through complete.
 * FULL leaves ownership with the caller; an accepted completion wakes it. */
enum client_surface_query_status client_surface_query_geometry(
    struct client_surface_geometry_query *query, void (*wake)(void) );
BOOL client_surface_complete_queries( unsigned int budget );

#endif
