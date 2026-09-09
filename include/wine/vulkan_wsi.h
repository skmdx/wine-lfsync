/* Private Wine/native WSI source transaction ABI. Not a Vulkan extension.
 * Copyright 2026 Wine contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef __WINE_VULKAN_WSI_H
#define __WINE_VULKAN_WSI_H

#define WINE_VK_SOURCE_ABI_VERSION 1
#define WINE_VK_SURFACE_SOURCE_CAPS ((VkStructureType)0x7fff0001)
#define WINE_VK_SWAPCHAIN_SOURCE_CREATE_INFO ((VkStructureType)0x7fff0002)
#define WINE_VK_PRESENT_SOURCE_INFO ((VkStructureType)0x7fff0003)

/* Queried on the native surface capabilities chain. An unaware provider
 * leaves supported FALSE. No support is inferred from its name or version. */
struct wine_vk_surface_source_caps
{
    VkStructureType sType;
    void *pNext;
    uint32_t version;
    VkBool32 supported;
};

/* The provider must acknowledge the actual swapchain, including native
 * image allocation and queue/completion support, before it can be used. */
struct wine_vk_swapchain_source_create_info
{
    VkStructureType sType;
    const void *pNext;
    uint32_t version;
    VkBool32 *enabled;
};

/* All pointers and commands are borrowed for the duration of QueuePresent.
 * Commands restore PRESENT_SRC_KHR and are owned by Wine until ready_fence.
 * The provider prepares the entire batch before consuming application waits,
 * then submits commands, WSI signals, ready_fence and application Present
 * fences together. submitted distinguishes cancellation from accepted GPU
 * work even when the presentation engine rejects an output. Image readiness
 * does not substitute for a Present ID's display-completion semantics. */
struct wine_vk_present_source_info
{
    VkStructureType sType;
    const void *pNext;
    uint32_t version;
    uint32_t command_count;
    const VkCommandBuffer *commands;
    VkFence ready_fence;
    VkBool32 *submitted;
};

static inline const void *wine_vk_find_source_struct( const void *next, VkStructureType type )
{
    const VkBaseInStructure *entry;
    for (entry = next; entry; entry = entry->pNext)
        if (entry->sType == type) return entry;
    return NULL;
}

#endif
