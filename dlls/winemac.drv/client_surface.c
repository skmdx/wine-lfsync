/*
 * Mac client surface backend
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

#include "macdrv.h"

WINE_DEFAULT_DEBUG_CHANNEL(macdrv);

static void macdrv_client_surface_destroy(struct client_surface *client)
{
    struct macdrv_client_surface *surface = impl_from_client_surface(client);

    TRACE("%s\n", debugstr_client_surface(client));

    if (surface->metal_swapchain) macdrv_destroy_swapchain(surface->metal_swapchain);
}

static void macdrv_client_surface_detach(struct client_surface *client)
{
    struct macdrv_client_surface *surface = impl_from_client_surface(client);

    TRACE("%s\n", debugstr_client_surface(client));

    if (surface->cocoa_view)
    {
        struct macdrv_win_data *data;

        if ((data = get_win_data(client->hwnd)))
        {
            if (data->client_view == surface->cocoa_view)
                data->client_view = NULL;
            release_win_data(data);
        }

        macdrv_dispose_view(surface->cocoa_view);
    }
}

static BOOL macdrv_client_surface_update(struct client_surface *client)
{
    struct macdrv_client_surface *surface = impl_from_client_surface(client);
    HWND hwnd = client->hwnd, toplevel = NtUserGetAncestor(hwnd, GA_ROOT);
    struct macdrv_win_data *data;

    TRACE("%s\n", debugstr_client_surface(client));

    if (!(data = get_win_data(toplevel))) return FALSE;
    macdrv_set_view_frame(surface->cocoa_view, cgrect_from_rect(client->monitor_rect));
    macdrv_set_view_superview(surface->cocoa_view, toplevel == hwnd ? NULL : data->client_view, data->cocoa_window, NULL, NULL);
    release_win_data(data);
    return TRUE;
}

static BOOL macdrv_client_surface_present(struct client_surface *client, HDC hdc, HRGN surface_region, BOOL flush)
{
    struct macdrv_client_surface *surface = impl_from_client_surface(client);
    struct macdrv_win_data *data;

    TRACE("%s\n", debugstr_client_surface(client));

    if (!(data = get_win_data(surface->client.hwnd))) return FALSE;
    if (data->client_view != surface->cocoa_view)
    {
        if (data->client_view) macdrv_set_view_hidden(data->client_view, TRUE);
        macdrv_set_view_hidden(surface->cocoa_view, FALSE);
        data->client_view = surface->cocoa_view;
    }
    release_win_data(data);
    return TRUE;
}

static const struct client_surface_backend macdrv_client_surface_backend =
{
    .caps = 0,
    .destroy = macdrv_client_surface_destroy,
    .detach = macdrv_client_surface_detach,
    .update = macdrv_client_surface_update,
    .present = macdrv_client_surface_present,
};

struct macdrv_client_surface *impl_from_client_surface(struct client_surface *client)
{
    assert(client->backend == &macdrv_client_surface_backend);
    return CONTAINING_RECORD(client, struct macdrv_client_surface, client);
}

struct client_surface *macdrv_CreateClientSurface(HWND hwnd, int pixel_format, BOOL raw)
{
    struct macdrv_client_surface *surface;

    surface = client_surface_create(sizeof(*surface), &macdrv_client_surface_backend, hwnd, pixel_format, raw);
    surface->cocoa_view = macdrv_create_view(cgrect_from_rect(surface->client.monitor_rect));
    macdrv_set_view_hidden(surface->cocoa_view, TRUE);

    if (surface)
    {
        macdrv_client_surface_update(&surface->client);
        macdrv_client_surface_present(&surface->client, 0, 0, FALSE);
    }

    return &surface->client;
}

BOOL macdrv_client_surface_acquire_metal_swapchain(struct macdrv_client_surface *surface)
{
    HWND hwnd = surface->client.hwnd;
    struct macdrv_win_data *data;

    if (surface->metal_swapchain) return TRUE;

    if ((data = get_win_data(hwnd)))
    {
        release_win_data(data);
        surface->metal_swapchain = macdrv_create_view_swapchain(surface->cocoa_view);
    }
    else
    {
        RECT rect;

        if (NtUserGetAncestor(hwnd, GA_ROOT) != hwnd)
        {
            FIXME("Cross-process child window Metal swapchains are not implemented\n");
            return FALSE;
        }

        if (!NtUserGetClientRect(hwnd, &rect, NtUserGetWinMonitorDpi(hwnd, MDT_RAW_DPI))) return FALSE;
        surface->metal_swapchain = macdrv_create_offscreen_swapchain(hwnd, cgrect_from_rect(rect));
    }

    return surface->metal_swapchain != NULL;
}
