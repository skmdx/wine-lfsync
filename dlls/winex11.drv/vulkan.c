/* X11DRV Vulkan implementation
 *
 * Copyright 2017 Roderick Colenbrander
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* NOTE: If making changes here, consider whether they should be reflected in
 * the other drivers. */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <dlfcn.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"

#include "wine/debug.h"
#include "client_surface.h"
#include "xcomposite.h"

#define WINE_VULKAN_NO_X11_TYPES
#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

static const struct vulkan_driver_funcs x11drv_vulkan_driver_funcs;

static VkResult X11DRV_vulkan_surface_create( struct client_surface *client, const struct vulkan_instance *instance, VkSurfaceKHR *handle )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    VkXlibSurfaceCreateInfoKHR info =
    {
        .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
        .dpy = gdi_display,
        .window = surface->window,
    };
    VkResult res;

    TRACE( "%s %p %p\n", debugstr_client_surface( client ), instance, handle );

    if ((res = instance->p_vkCreateXlibSurfaceKHR( instance->host.instance, &info, NULL /* allocator */, handle ))) return res;
    TRACE( "Created surface 0x%s\n", wine_dbgstr_longlong( *handle ) );

    return VK_SUCCESS;
}

static VkBool32 X11DRV_get_physical_device_presentation_support( struct vulkan_physical_device *physical_device, uint32_t index )
{
    struct vulkan_instance *instance = physical_device->instance;
    TRACE( "%p %u\n", physical_device, index );
    return instance->p_vkGetPhysicalDeviceXlibPresentationSupportKHR( physical_device->host.physical_device, index, gdi_display,
                                                                      default_visual.visual->visualid );
}

static const struct x11drv_snapshot_format *get_snapshot_format( VkFormat format )
{
    static const struct x11drv_snapshot_format bgra8 = {4, 0xff0000, 0xff00, 0xff, 0xff000000};
    static const struct x11drv_snapshot_format rgb565 = {2, 0xf800, 0x7e0, 0x1f, 0};
    static const struct x11drv_snapshot_format bgr565 = {2, 0x1f, 0x7e0, 0xf800, 0};
    static const struct x11drv_snapshot_format a2rgb10 = {4, 0x3ff00000, 0xffc00, 0x3ff, 0xc0000000};
    static const struct x11drv_snapshot_format a2bgr10 = {4, 0x3ff, 0xffc00, 0x3ff00000, 0xc0000000};

    switch (format)
    {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB: return &x11drv_snapshot_rgba8;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB: return &bgra8;
    case VK_FORMAT_R5G6B5_UNORM_PACK16: return &rgb565;
    case VK_FORMAT_B5G6R5_UNORM_PACK16: return &bgr565;
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return &a2rgb10;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return &a2bgr10;
    default: return NULL;
    }
}

static VkResult X11DRV_vulkan_surface_get_source( struct client_surface *client, VkFormat format,
                                                 struct vulkan_surface_source *source )
{
    const struct x11drv_snapshot_format *snapshot;

    source->type = usexcomposite ? VULKAN_SURFACE_SOURCE_NATIVE : VULKAN_SURFACE_SOURCE_READBACK;
    source->texel_size = 0;
    if (usexcomposite || format == VK_FORMAT_UNDEFINED) return VK_SUCCESS;
    if (!(snapshot = get_snapshot_format( format ))) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    source->texel_size = snapshot->texel_size;
    return VK_SUCCESS;
}

static BOOL X11DRV_vulkan_surface_snapshot( struct client_surface *client,
                                           struct client_surface_frame *present,
                                           const void *pixels, uint32_t width, uint32_t height,
                                           VkFormat format )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    const struct x11drv_snapshot_format *snapshot = get_snapshot_format( format );

    if (!snapshot || !x11drv_client_surface_snapshot( client, pixels, width, height, TRUE, snapshot )) return FALSE;
    if (present->handoff_control) client->handoff_source[present->handoff_index].source = surface->snapshot;
    /* Retain the completed image even while the owner prepares a new scene.
     * The common completion path freezes it before publishing its source. */
    present->capture.size = (SIZE){width, height};
    TRACE( "captured Vulkan snapshot %#lx size %ux%u format %u for %s\n",
           surface->snapshot, width, height, format, debugstr_client_surface( client ) );
    return TRUE;
}

static void X11DRV_map_instance_extensions( struct vulkan_instance_extensions *extensions )
{
    if (extensions->has_VK_KHR_win32_surface) extensions->has_VK_KHR_xlib_surface = 1;
    if (extensions->has_VK_KHR_xlib_surface) extensions->has_VK_KHR_win32_surface = 1;
}

static void X11DRV_map_device_extensions( struct vulkan_device_extensions *extensions )
{
    if (extensions->has_VK_KHR_external_memory_win32) extensions->has_VK_KHR_external_memory_fd = 1;
    if (extensions->has_VK_KHR_external_memory_fd) extensions->has_VK_KHR_external_memory_win32 = 1;
    if (extensions->has_VK_KHR_external_semaphore_win32) extensions->has_VK_KHR_external_semaphore_fd = 1;
    if (extensions->has_VK_KHR_external_semaphore_fd) extensions->has_VK_KHR_external_semaphore_win32 = 1;
    if (extensions->has_VK_KHR_external_fence_win32) extensions->has_VK_KHR_external_fence_fd = 1;
    if (extensions->has_VK_KHR_external_fence_fd) extensions->has_VK_KHR_external_fence_win32 = 1;
}

static const struct vulkan_driver_funcs x11drv_vulkan_driver_funcs =
{
    .p_vulkan_surface_create = X11DRV_vulkan_surface_create,
    .p_vulkan_surface_get_source = X11DRV_vulkan_surface_get_source,
    .p_vulkan_surface_snapshot = X11DRV_vulkan_surface_snapshot,
    .p_get_physical_device_presentation_support = X11DRV_get_physical_device_presentation_support,
    .p_map_instance_extensions = X11DRV_map_instance_extensions,
    .p_map_device_extensions = X11DRV_map_device_extensions,
};

UINT X11DRV_VulkanInit( UINT version, void *vulkan_handle, const struct vulkan_driver_funcs **driver_funcs )
{
    if (version != WINE_VULKAN_DRIVER_VERSION)
    {
        ERR( "version mismatch, win32u wants %u but driver has %u\n", version, WINE_VULKAN_DRIVER_VERSION );
        return STATUS_INVALID_PARAMETER;
    }

    *driver_funcs = &x11drv_vulkan_driver_funcs;
    return STATUS_SUCCESS;
}
