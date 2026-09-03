/*
 * Android client surface backend
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

#include "android.h"

static const struct client_surface_backend android_client_surface_backend = {0};

struct client_surface *ANDROID_CreateClientSurface( HWND hwnd, int pixel_format, BOOL raw )
{
    return client_surface_create( sizeof(struct client_surface), &android_client_surface_backend,
                                  hwnd, pixel_format, raw );
}
