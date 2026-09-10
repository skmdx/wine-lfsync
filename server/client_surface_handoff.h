/*
 * Client surface handoff registry
 *
 * Copyright 2026 Wine contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_SERVER_CLIENT_SURFACE_HANDOFF_H
#define __WINE_SERVER_CLIENT_SURFACE_HANDOFF_H

#include "object.h"

struct client_surface_handoff_binding;

/* A transient snapshot of registry authority. The window is a shared-memory
 * lookup hint and requires authentication by the scene authority before use.
 * The owner is borrowed while the binding retains its object reference. */
struct client_surface_handoff_state
{
    struct object *owner;
    struct process *consumer;
    user_handle_t window;
    UINT64 cookie;
    unsigned int mapped;
    int retired;
    int lost;
};

struct client_surface_handoff_mapping
{
    UINT64 id, cookie;
    data_size_t size, offset;
};

/* The changed callback invalidates scene-side reuse before endpoint wakes.
 * It must not release the binding. The registration retains its callback
 * context until the binding is freed. No scene decisions belong here. */
extern struct client_surface_handoff_binding *client_surface_handoff_create(
    struct process *producer, struct process *consumer, struct object *owner,
    user_handle_t window, user_handle_t toplevel, UINT64 identity,
    void (*changed)( void *context ), void *context );
extern struct client_surface_handoff_state client_surface_handoff_get_state(
    const struct client_surface_handoff_binding *binding );
extern void client_surface_handoff_free( struct client_surface_handoff_binding *binding );
extern struct client_surface_handoff_binding *client_surface_handoff_retire(
    struct client_surface_handoff_binding *binding );
extern struct client_surface_handoff_binding *client_surface_handoff_retarget(
    struct client_surface_handoff_binding *binding );
extern obj_handle_t client_surface_handoff_map( struct client_surface_handoff_binding *binding,
    struct process *process, int producer, struct client_surface_handoff_mapping *mapping );
extern obj_handle_t client_surface_handoff_get_event( const struct client_surface_handoff_binding *binding,
    struct process *process, int producer );
extern void client_surface_handoff_release_endpoint( struct client_surface_handoff_binding *binding, int producer );
extern struct client_surface_handoff_binding *client_surface_handoff_cleanup_binding(
    struct client_surface_handoff_binding *binding, struct process *process );
extern void client_surface_handoff_cleanup_pools( struct process *process );

#endif /* __WINE_SERVER_CLIENT_SURFACE_HANDOFF_H */
