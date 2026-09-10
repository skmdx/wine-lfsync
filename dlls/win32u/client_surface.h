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

extern BOOL client_surface_completion_init( struct client_surface *surface );
extern struct client_surface_completion_job *client_surface_reserve_completion_domain(
    struct client_surface *surface, UINT64 domain );
extern void client_surface_completion_destroy( struct client_surface *surface );
extern struct client_surface *client_surface_alloc( UINT size );
extern void client_surface_handoff_destroy( struct client_surface *surface );
extern BOOL client_surface_handoff_has_source( const struct client_surface *surface );
extern BOOL client_surface_handoff_write_available( const struct client_surface *surface );
extern void client_surface_handoff_wait( struct client_surface *surface );
extern void client_surface_handoff_retire_closed( struct client_surface *surface );
extern void client_surface_handoff_completed( struct client_surface *surface );
extern BOOL client_surface_handoff_valid( const struct client_surface *surface,
                                        const struct client_surface_frame *present );
extern BOOL client_surface_prepare_source_locked( struct client_surface *surface,
                                                 struct client_surface_frame *present );

extern HWND client_surface_set_server_state( HWND hwnd, const struct client_surface *surface,
                                             UINT flags, UINT64 generation,
                                             UINT64 scene_generation, BOOL *wake );
extern void client_surface_get_geometry( const struct client_surface *surface,
                                         struct client_surface_geometry *geometry );
extern void client_surface_get_target( const struct client_surface *surface,
                                       struct client_surface_target *target );
extern BOOL get_client_surface_rects( HWND toplevel, HWND hwnd,
                                      struct client_surface_target *target );
extern BOOL client_surface_needs_completion_reservation( struct client_surface *surface );
extern BOOL client_surface_get_scene( struct client_surface *surface,
                                      struct client_surface_scene *scene );
extern BOOL client_surface_scene_current( const struct client_surface_scene *scene );
extern BOOL client_surface_update_present_scene_locked(
    struct client_surface *surface, const struct client_surface_scene *scene, BOOL allow_direct_transition );
extern void client_surface_prepare_recompose_locked( struct client_surface *surface,
                                                      struct client_surface_frame *present );
extern BOOL client_surface_update_present_locked( struct client_surface *surface );
extern void client_surface_apply_pending_update( struct client_surface *surface );
extern BOOL client_surface_end_present_internal( struct client_surface *surface,
                                                 const SIZE *expected_size, BOOL new_content,
                                                 struct client_surface_frame *present );
extern void client_surface_invalidate_source_locked( struct client_surface *surface,
                                                      const struct client_surface_frame *present );
extern void client_surface_resume_recompose( struct client_surface *surface );
extern BOOL client_surface_prepare_handoff_locked( struct client_surface *surface,
                                                    struct client_surface_frame *present );
/* A completed image is independent of scene placement. Its native storage is
 * pinned by the producer's handoff reservation until publication or abandonment. */
struct client_surface_completed_frame
{
    UINT64 surface_id;
    UINT64 frame_id;
    UINT64 target_epoch;
    UINT64 image;
    UINT64 visual;
    UINT flags;
    SIZE size;
    RECT damage;
    UINT64 damage_base_frame;
};

extern BOOL client_surface_freeze_frame_locked( struct client_surface *surface,
                                               struct client_surface_frame *present,
                                               struct client_surface_completed_frame *frame );
extern BOOL client_surface_publish_handoff_locked( struct client_surface *surface,
                                                    struct client_surface_frame *present,
                                                    const struct client_surface_completed_frame *frame );
extern void client_surface_abandon_handoff_locked( struct client_surface *surface,
                                                   struct client_surface_frame *present );
extern void client_surface_cancel_prepare_locked( struct client_surface *surface,
                                                  struct client_surface_frame *present );
extern void client_surface_release_handoff( struct client_surface *surface );

#endif /* __WINE_WIN32U_CLIENT_SURFACE_H */
