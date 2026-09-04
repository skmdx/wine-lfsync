/*
 * Client surface shared completion definitions
 *
 * Copyright 2026 Wine contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_CLIENT_SURFACE_H
#define __WINE_CLIENT_SURFACE_H

#include "windef.h"

enum client_surface_completion_kind
{
    CLIENT_SURFACE_COMPLETION_NONE,
    CLIENT_SURFACE_COMPLETION_EXACT,
    CLIENT_SURFACE_COMPLETION_SHARED,
};

typedef BOOL (*client_surface_completion_wait_func)( void *context, DWORD timeout );
typedef void (*client_surface_completion_release_func)( void *context );

struct client_surface_completion
{
    enum client_surface_completion_kind kind;
    BOOL external_result; /* completion result is supplied by the caller or queued token */
    client_surface_completion_wait_func wait;
    client_surface_completion_release_func release;
    void *context;
};

static inline BOOL client_surface_completion_result_is_external(
    const struct client_surface_completion *completion )
{
    return completion->external_result;
}

#endif /* __WINE_CLIENT_SURFACE_H */
