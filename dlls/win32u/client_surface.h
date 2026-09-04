/*
 * Client surface internals
 *
 * Copyright 2026 Wine contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_WIN32U_CLIENT_SURFACE_H
#define __WINE_WIN32U_CLIENT_SURFACE_H

#include "ntgdi_private.h"

struct client_surface_geometry
{
    HWND toplevel;
    RECT virtual_rect;
    RECT monitor_rect;
};

extern HWND client_surface_set_server_state( HWND hwnd, const struct client_surface *surface,
                                             UINT flags, UINT64 generation,
                                             UINT64 scene_generation, BOOL *wake );
extern void client_surface_get_geometry( const struct client_surface *surface,
                                         struct client_surface_geometry *geometry );
extern void client_surface_get_target( const struct client_surface *surface,
                                       struct client_surface_target *target );
extern BOOL client_surface_get_publication( struct client_surface *surface, UINT64 *generation,
                                            UINT64 *scene_generation, HWND *scene_toplevel,
                                            BOOL *authoritative );
extern BOOL client_surface_update_present_locked( struct client_surface *surface );
extern void client_surface_register_external_completion_locked( struct client_surface *surface,
                                                                 struct client_surface_present *present );
extern BOOL client_surface_end_present_internal( struct client_surface *surface, UINT64 generation,
                                                 const SIZE *expected_size, BOOL new_content,
                                                 struct client_surface_present *present );

#endif /* __WINE_WIN32U_CLIENT_SURFACE_H */
