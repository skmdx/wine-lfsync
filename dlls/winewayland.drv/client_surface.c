/*
 * Wayland client surface backend
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

#include "waylanddrv.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

static void wayland_client_surface_destroy(struct client_surface *client)
{
    struct wayland_client_surface *surface = impl_from_client_surface(client);

    TRACE("%s\n", debugstr_client_surface(client));

    if (surface->wp_viewport)
        wp_viewport_destroy(surface->wp_viewport);
    if (surface->wl_subsurface)
        wl_subsurface_destroy(surface->wl_subsurface);
    if (surface->wl_surface)
        wl_surface_destroy(surface->wl_surface);
}

static void wayland_client_surface_detach(struct client_surface *client)
{
    struct wayland_client_surface *surface = impl_from_client_surface(client);
    struct wayland_win_data *data;

    if ((data = wayland_win_data_get(client->hwnd)))
    {
        if (data->client_surface == surface) data->client_surface = NULL;
        wayland_client_surface_attach(surface, NULL, NULL);
        wayland_win_data_release(data);
    }
}

static BOOL wayland_client_surface_update(struct client_surface *client,
                                          struct client_surface_target *target)
{
    struct wayland_client_surface *surface = impl_from_client_surface(client);
    HWND hwnd = client->hwnd, toplevel = target->toplevel;
    struct wayland_win_data *data;
    BOOL visible = FALSE;

    TRACE("%s\n", debugstr_client_surface(client));
    if(toplevel) visible = NtUserIsWindowVisible(hwnd);
    if (!(data = wayland_win_data_get(hwnd))) return FALSE;

    if (toplevel && visible)
        wayland_client_surface_attach(surface, toplevel, &target->monitor_rect);
    else
        wayland_client_surface_attach(surface, NULL, NULL);

    wayland_win_data_release(data);
    return TRUE;
}

static BOOL wayland_client_surface_present(struct client_surface *client, HDC hdc, HRGN surface_region,
                                           BOOL flush, BOOL defer_visible)
{
    struct wayland_client_surface *surface = impl_from_client_surface(client);
    HWND hwnd = client->hwnd, toplevel = client->target.toplevel;
    struct wayland_surface *wayland_surface;
    struct wayland_win_data *data;

    if (!(data = wayland_win_data_get(toplevel))) return FALSE;

    if ((wayland_surface = data->wayland_surface))
    {
        wayland_surface_ensure_contents(wayland_surface);

        /* Handle any processed configure request, to ensure the related
         * surface state is applied by the compositor. */
        if (wayland_surface->processing.serial &&
            wayland_surface->processing.processed &&
            wayland_surface_reconfigure(wayland_surface))
        {
            wl_surface_commit(wayland_surface->wl_surface);
        }
    }

    wayland_win_data_release(data);

    set_client_surface(hwnd, surface);
    return TRUE;
}

static const struct client_surface_backend wayland_client_surface_backend =
{
    .caps = 0,
    .destroy = wayland_client_surface_destroy,
    .detach = wayland_client_surface_detach,
    .update = wayland_client_surface_update,
    .present = wayland_client_surface_present,
};

struct wayland_client_surface *impl_from_client_surface(struct client_surface *client)
{
    assert(client->backend == &wayland_client_surface_backend);
    return CONTAINING_RECORD(client, struct wayland_client_surface, client);
}

struct client_surface *WAYLAND_CreateClientSurface(HWND hwnd, int pixel_format, BOOL raw)
{
    struct wayland_client_surface *client;
    struct wl_region *empty_region;

    if (!(client = client_surface_create(sizeof(*client), &wayland_client_surface_backend, hwnd, pixel_format, raw))) return NULL;

    client->wl_surface =
        wl_compositor_create_surface(process_wayland.wl_compositor);
    if (!client->wl_surface)
    {
        ERR("Failed to create client wl_surface\n");
        goto err;
    }
    wl_surface_set_user_data(client->wl_surface, hwnd);

    /* Let parent handle all pointer events. */
    empty_region = wl_compositor_create_region(process_wayland.wl_compositor);
    if (!empty_region)
    {
        ERR("Failed to create wl_region\n");
        goto err;
    }
    wl_surface_set_input_region(client->wl_surface, empty_region);
    wl_region_destroy(empty_region);

    client->wp_viewport =
        wp_viewporter_get_viewport(process_wayland.wp_viewporter,
                                    client->wl_surface);
    if (!client->wp_viewport)
    {
        ERR("Failed to create client wp_viewport\n");
        goto err;
    }

    return &client->client;

err:
    client_surface_release(&client->client);
    return NULL;
}

void wayland_client_surface_attach(struct wayland_client_surface *client, HWND toplevel, const RECT *rect)
{
    struct wayland_win_data *toplevel_data;
    struct wayland_surface *surface;

    if (!toplevel)
    {
        if (client->wl_subsurface)
        {
            wl_subsurface_destroy(client->wl_subsurface);
            client->wl_subsurface = NULL;
        }

        client->toplevel = 0;
        return;
    }

    if (!(toplevel_data = wayland_win_data_get(toplevel)) || !(surface = toplevel_data->wayland_surface))
    {
        if (toplevel_data) wayland_win_data_release(toplevel_data);
        return wayland_client_surface_attach(client, NULL, NULL);
    }

    if (client->toplevel != toplevel)
    {
        wayland_client_surface_attach(client, NULL, NULL);

        client->wl_subsurface =
            wl_subcompositor_get_subsurface(process_wayland.wl_subcompositor,
                                            client->wl_surface,
                                            surface->wl_surface);
        if (!client->wl_subsurface) goto done;

        /* Present contents independently of the parent surface. */
        wl_subsurface_set_desync(client->wl_subsurface);

        client->toplevel = toplevel;
    }

    wayland_surface_reconfigure_client(surface, client, rect);
    /* Commit to apply subsurface positioning. */
    wl_surface_commit(surface->wl_surface);

done:
    wayland_win_data_release(toplevel_data);
}
