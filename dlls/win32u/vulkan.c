/*
 * Vulkan display driver loading
 *
 * Copyright (c) 2017 Roderick Colenbrander
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "ntstatus.h"
#include "win32u_private.h"
#include "ntuser_private.h"
#include "client_surface.h"
#include "wine/vulkan_wsi.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

static PFN_vkGetDeviceProcAddr p_vkGetDeviceProcAddr;
static PFN_vkGetInstanceProcAddr p_vkGetInstanceProcAddr;
static PFN_vkCreateInstance p_vkCreateInstance;
static PFN_vkEnumerateInstanceExtensionProperties p_vkEnumerateInstanceExtensionProperties;

static void *vulkan_handle;
static struct vulkan_funcs vulkan_funcs;

WINE_DECLARE_DEBUG_CHANNEL(fps);

static const struct vulkan_driver_funcs *driver_funcs;

static const UINT EXTERNAL_MEMORY_WIN32_BITS = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT |
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT |
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT |
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT |
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT |
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
static const UINT EXTERNAL_SEMAPHORE_WIN32_BITS = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT |
                                                  VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT |
                                                  VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
static const UINT EXTERNAL_FENCE_WIN32_BITS = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT |
                                              VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT;

#define ROUND_SIZE(size, mask) ((((SIZE_T)(size) + (mask)) & ~(SIZE_T)(mask)))

static BOOL use_external_memory(void)
{
    return zero_bits != 0;
}

struct mempool
{
    struct mempool *next;
    size_t mem_used;
    char mem[2048];
};

static void mem_free( struct mempool *pool )
{
    struct mempool *iter, *next = pool->next;
    while ((iter = next)) { next = iter->next; free( iter ); }
    pool->mem_used = 0;
    pool->next = NULL;
}

static void *mem_alloc( struct mempool *pool, size_t size )
{
    struct mempool *next = pool;
    if (pool->mem_used + size > sizeof(pool->mem)) next = pool->next;
    if (next && next->mem_used <= sizeof(next->mem) && next->mem_used + size <= sizeof(next->mem))
    {
        void *ret = next->mem + next->mem_used;
        next->mem_used += ROUND_SIZE( size, sizeof(UINT64) - 1 );
        return ret;
    }
    if (!(next = malloc( max( sizeof(*next), offsetof(struct mempool, mem[size]) ) ))) return NULL;
    next->next = pool->next;
    next->mem_used = size;
    pool->next = next;
    return next->mem;
}

struct instance
{
    struct vulkan_instance obj;
    BOOL enable_win32_surface;

    struct list utils_messengers;
    struct list report_callbacks;

    struct rb_tree objects;
    pthread_rwlock_t objects_lock;
};

static struct instance *instance_from_handle( VkInstance handle )
{
    struct vulkan_instance *object = vulkan_instance_from_handle( handle );
    return CONTAINING_RECORD( object, struct instance, obj );
}

struct device_memory
{
    struct vulkan_device_memory obj;
    VkDeviceSize size;
    void *vm_map;

    D3DKMT_HANDLE local;
    D3DKMT_HANDLE global;
    HANDLE shared;

    D3DKMT_HANDLE sync;
    D3DKMT_HANDLE mutex;
    VkSemaphore semaphore;
    UINT64 semaphore_value;
};

static inline struct device_memory *device_memory_from_handle( VkDeviceMemory handle )
{
    struct vulkan_device_memory *obj = vulkan_device_memory_from_handle( handle );
    return CONTAINING_RECORD( obj, struct device_memory, obj );
}

struct surface
{
    struct vulkan_surface obj;
    struct client_surface *client;
    HWND hwnd;
    LONG refs;
};

static struct surface *surface_from_handle( VkSurfaceKHR handle )
{
    struct vulkan_surface *obj = vulkan_surface_from_handle( handle );
    return CONTAINING_RECORD( obj, struct surface, obj );
}

struct device
{
    pthread_mutex_t retirement_lock;
    pthread_cond_t retirement_cond;
    struct list retired_swapchains;
    HANDLE retirement_worker;
    BOOL retirement_starting;
    BOOL retirement_shutdown;
    BOOL swapchain_maintenance1;
    UINT64 completion_domain_base;
    struct vulkan_device obj;
};

static struct device *impl_from_vulkan_device( struct vulkan_device *device )
{
    return CONTAINING_RECORD( device, struct device, obj );
}

#define MAX_SWAPCHAIN_RETIREMENT_WORKERS 64
static LONG swapchain_retirement_workers;

struct vulkan_snapshot_fence
{
    struct vulkan_device *device;
    VkFence fence;
    LONG refs;
};

struct swapchain_snapshot
{
    VkImage *images;
    uint32_t image_count;
    SIZE_T images_bytes;
    VkBuffer buffer;
    VkDeviceMemory memory;
    UINT64 memory_bytes;
    void *pixels;
    struct vulkan_surface_snapshot *backend_snapshot;
    VkCommandPool pool;
    VkCommandBuffer command;
    uint32_t queue_family;
    struct vulkan_snapshot_fence *pending;
    struct vulkan_snapshot_capture *capture;
    BOOL busy;
};

struct swapchain
{
    struct vulkan_swapchain obj;
    struct list retirement_entry;
    struct surface *surface;
    VkExtent2D extents;
    VkExtent2D host_extents;
    VkFormat format;
    struct vulkan_surface_source source;
    struct swapchain_snapshot snapshots[2];
    unsigned int next_snapshot;
    pthread_mutex_t present_lock;
    pthread_cond_t completion_cond;
    unsigned int completion_refs;
    unsigned int present_waits;
    BOOL retired;
    uint64_t next_present_id;
    UINT64 last_source_sequence;
    BOOL incremental_damage;
};

static struct swapchain *swapchain_from_handle( VkSwapchainKHR handle )
{
    struct vulkan_swapchain *obj = vulkan_swapchain_from_handle( handle );
    return CONTAINING_RECORD( obj, struct swapchain, obj );
}

static BOOL swapchain_needs_snapshot( const struct swapchain *swapchain )
{
    return swapchain->source.type == VULKAN_SURFACE_SOURCE_READBACK;
}

struct vulkan_present_completion
{
    struct vulkan_device *device;
    struct swapchain *swapchain;
    uint64_t present_id;
};

struct vulkan_snapshot_capture
{
    struct vulkan_device *device;
    struct swapchain *swapchain;
    struct swapchain_snapshot *snapshot;
};

struct vulkan_present_reservation
{
    struct swapchain_snapshot *snapshot;
    struct client_surface_completion_job *job;
    struct vulkan_present_completion *completion;
    BOOL required;
    BOOL new_capture;
};

static struct client_surface_completion_result wait_vulkan_present_completion( void *context, DWORD timeout )
{
    struct vulkan_present_completion *completion = context;
    struct swapchain *swapchain = completion->swapchain;
    VkResult res;

    pthread_mutex_lock( &swapchain->present_lock );
    if (swapchain->retired)
    {
        pthread_mutex_unlock( &swapchain->present_lock );
        TRACE( "Skipping present wait for retired swapchain %p, id %s\n",
               swapchain, wine_dbgstr_longlong( completion->present_id ) );
        return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
    }
    /* Admission pins the host handle even for an inline completion. A
     * replacement cannot retire it between this check and the host call. */
    ++swapchain->present_waits;
    TRACE( "Admitted present wait for swapchain %p, id %s, timeout %u\n",
           swapchain, wine_dbgstr_longlong( completion->present_id ), timeout );
    pthread_mutex_unlock( &swapchain->present_lock );
    res = completion->device->p_vkWaitForPresentKHR(
        completion->device->host.device, swapchain->obj.host.swapchain,
        completion->present_id, (uint64_t)timeout * 1000000 );
    if (res != VK_SUCCESS && res != VK_TIMEOUT)
        WARN( "Failed waiting for present %s, status %d\n",
              debugstr_client_surface( swapchain->surface->client ), res );
    pthread_mutex_lock( &swapchain->present_lock );
    assert( swapchain->present_waits );
    TRACE( "Completed present wait for swapchain %p, id %s, status %d\n",
           swapchain, wine_dbgstr_longlong( completion->present_id ), res );
    if (!--swapchain->present_waits) pthread_cond_broadcast( &swapchain->completion_cond );
    pthread_mutex_unlock( &swapchain->present_lock );
    if (res == VK_SUCCESS) return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_SIGNALED );
    if (res == VK_TIMEOUT) return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_PENDING );
    return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
}

static void release_swapchain_completion( struct swapchain *swapchain )
{
    pthread_mutex_lock( &swapchain->present_lock );
    assert( swapchain->completion_refs );
    if (!--swapchain->completion_refs) pthread_cond_signal( &swapchain->completion_cond );
    pthread_mutex_unlock( &swapchain->present_lock );
}

static void release_vulkan_present_completion( void *context )
{
    struct vulkan_present_completion *completion = context;

    release_swapchain_completion( completion->swapchain );
    client_surface_free_metadata( completion, sizeof(*completion) );
}

static void retain_swapchain_completion( struct swapchain *swapchain )
{
    pthread_mutex_lock( &swapchain->present_lock );
    swapchain->completion_refs++;
    pthread_mutex_unlock( &swapchain->present_lock );
}

static struct client_surface_completion_result wait_vulkan_driver_completion( void *context, DWORD timeout )
{
    struct swapchain *swapchain = context;
    struct client_surface *surface = swapchain->surface->client;

    /* A driver monitor remains valid for an acquired image presented after
     * retirement. It does not call the retired swapchain's present-wait API. */
    return surface->backend->completion->wait( surface, timeout );
}

static void release_vulkan_driver_completion( void *context )
{
    release_swapchain_completion( context );
}

static void retire_swapchain_present_waits( struct swapchain *swapchain )
{
    pthread_mutex_lock( &swapchain->present_lock );
    swapchain->retired = TRUE;
    /* Pin the old host handle through vkCreateSwapchainKHR, including its
     * failure path. Only admitted native waits must drain before retirement;
     * queued completions and CPU captures retain their independent refs. */
    ++swapchain->completion_refs;
    TRACE( "Closing present wait admission for swapchain %p, waiting for %u native waits\n",
           swapchain, swapchain->present_waits );
    while (swapchain->present_waits)
        pthread_cond_wait( &swapchain->completion_cond, &swapchain->present_lock );
    pthread_mutex_unlock( &swapchain->present_lock );
}

struct semaphore
{
    struct vulkan_semaphore obj;
    D3DKMT_HANDLE local;
    D3DKMT_HANDLE global;
    HANDLE shared;
};

static struct semaphore *semaphore_from_handle( VkSemaphore handle )
{
    struct vulkan_semaphore *obj = vulkan_semaphore_from_handle( handle );
    return CONTAINING_RECORD( obj, struct semaphore, obj );
}

struct fence
{
    struct vulkan_fence obj;
    D3DKMT_HANDLE local;
    D3DKMT_HANDLE global;
    HANDLE shared;
};

static struct fence *fence_from_handle( VkFence handle )
{
    struct vulkan_fence *obj = vulkan_fence_from_handle( handle );
    return CONTAINING_RECORD( obj, struct fence, obj );
}

static VkResult allocate_external_host_memory( struct vulkan_device *device, VkMemoryAllocateInfo *alloc_info, uint32_t mem_flags,
                                               VkImportMemoryHostPointerInfoEXT *import_info )
{
    struct vulkan_physical_device *physical_device = device->physical_device;
    VkMemoryHostPointerPropertiesEXT props =
    {
        .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
    };
    uint32_t i, align = physical_device->external_memory_align - 1;
    SIZE_T alloc_size = alloc_info->allocationSize;
    static int once;
    void *mapping = NULL;
    VkResult res;

    if (!once++) FIXME( "Using VK_EXT_external_memory_host\n" );

    if (NtAllocateVirtualMemory( GetCurrentProcess(), &mapping, zero_bits, &alloc_size, MEM_COMMIT, PAGE_READWRITE ))
    {
        ERR( "NtAllocateVirtualMemory failed\n" );
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    if ((res = device->p_vkGetMemoryHostPointerPropertiesEXT( device->host.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                              mapping, &props )))
    {
        ERR( "vkGetMemoryHostPointerPropertiesEXT failed: %d\n", res );
        return res;
    }

    if (!(props.memoryTypeBits & (1u << alloc_info->memoryTypeIndex)))
    {
        /* If requested memory type is not allowed to use external memory, try to find a supported compatible type. */
        uint32_t mask = mem_flags & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        for (i = 0; i < physical_device->memory_properties.memoryTypeCount; i++)
        {
            if (!(props.memoryTypeBits & (1u << i))) continue;
            if ((physical_device->memory_properties.memoryTypes[i].propertyFlags & mask) != mask) continue;

            TRACE( "Memory type not compatible with host memory, using %u instead\n", i );
            alloc_info->memoryTypeIndex = i;
            break;
        }
        if (i == physical_device->memory_properties.memoryTypeCount)
        {
            FIXME( "Not found compatible memory type\n" );
            alloc_size = 0;
            NtFreeVirtualMemory( GetCurrentProcess(), &mapping, &alloc_size, MEM_RELEASE );
        }
    }

    if (props.memoryTypeBits & (1u << alloc_info->memoryTypeIndex))
    {
        import_info->sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
        import_info->handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        import_info->pHostPointer = mapping;
        import_info->pNext = alloc_info->pNext;
        alloc_info->pNext = import_info;
        alloc_info->allocationSize = (alloc_info->allocationSize + align) & ~align;
    }

    return VK_SUCCESS;
}

static VkExternalMemoryHandleTypeFlagBits get_host_external_memory_type(void)
{
    struct vulkan_device_extensions extensions = {.has_VK_KHR_external_memory_win32 = 1};
    driver_funcs->p_map_device_extensions( &extensions );
    if (extensions.has_VK_KHR_external_memory_fd) return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    return 0;
}

static VkExternalSemaphoreHandleTypeFlagBits get_host_external_semaphore_type(void)
{
    struct vulkan_device_extensions extensions = {.has_VK_KHR_external_semaphore_win32 = 1};
    driver_funcs->p_map_device_extensions( &extensions );
    if (extensions.has_VK_KHR_external_semaphore_fd) return VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    return 0;
}

static VkExternalFenceHandleTypeFlagBits get_host_external_fence_type(void)
{
    struct vulkan_device_extensions extensions = {.has_VK_KHR_external_fence_win32 = 1};
    driver_funcs->p_map_device_extensions( &extensions );
    if (extensions.has_VK_KHR_external_fence_fd) return VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
    return 0;
}

static void init_shared_resource_path( const WCHAR *name, UNICODE_STRING *str )
{
    UINT len = wcslen( name );
    char buffer[MAX_PATH];

    snprintf( buffer, ARRAY_SIZE(buffer), "\\Sessions\\%u\\BaseNamedObjects\\",
              RtlGetCurrentPeb()->SessionId );
    str->MaximumLength = asciiz_to_unicode( str->Buffer, buffer );
    str->Length = str->MaximumLength - sizeof(WCHAR);

    memcpy( str->Buffer + str->Length / sizeof(WCHAR), name, (len + 1) * sizeof(WCHAR) );
    str->MaximumLength += len * sizeof(WCHAR);
    str->Length += len * sizeof(WCHAR);
}

static HANDLE create_shared_resource_handle( D3DKMT_HANDLE local, const VkExportMemoryWin32HandleInfoKHR *info )
{
    SECURITY_DESCRIPTOR *security = info->pAttributes ? info->pAttributes->lpSecurityDescriptor : NULL;
    WCHAR bufferW[MAX_PATH * 2];
    UNICODE_STRING name = {.Buffer = bufferW};
    OBJECT_ATTRIBUTES attr;
    NTSTATUS status;
    HANDLE shared;

    if (info->name) init_shared_resource_path( info->name, &name );
    InitializeObjectAttributes( &attr, info->name ? &name : NULL, OBJ_CASE_INSENSITIVE, NULL, security );

    if (!(status = NtGdiDdDDIShareObjects( 1, &local, &attr, info->dwAccess, &shared ))) return shared;
    WARN( "Failed to share resource %#x, status %#x\n", local, status );
    return NULL;
}

HANDLE open_shared_resource_from_name( const WCHAR *name )
{
    D3DKMT_OPENNTHANDLEFROMNAME open_name = {0};
    WCHAR bufferW[MAX_PATH * 2];
    UNICODE_STRING name_str = {.Buffer = bufferW};
    OBJECT_ATTRIBUTES attr;
    NTSTATUS status;

    init_shared_resource_path( name, &name_str );
    InitializeObjectAttributes( &attr, &name_str, OBJ_OPENIF, NULL, NULL );

    open_name.dwDesiredAccess = GENERIC_ALL;
    open_name.pObjAttrib = &attr;
    status = NtGdiDdDDIOpenNtHandleFromName( &open_name );
    if (status) WARN( "Failed to open %s, status %#x\n", debugstr_w( name ), status );
    return open_name.hNtHandle;
}

static const void *find_next_struct( const VkBaseInStructure *header, VkStructureType type )
{
    for (; header; header = header->pNext) if (header->sType == type) return header;
    return NULL;
}

static int vulkan_object_compare( const void *key, const struct rb_entry *entry )
{
    struct vulkan_object *object = RB_ENTRY_VALUE( entry, struct vulkan_object, entry );
    const uint64_t *host_handle = key;
    if (*host_handle < object->host_handle) return -1;
    if (*host_handle > object->host_handle) return 1;
    return 0;
}

static uint64_t vulkan_instance_client_handle_from_host( struct vulkan_instance *instance, uint64_t host_handle )
{
    struct instance *impl = CONTAINING_RECORD( instance, struct instance, obj );
    struct rb_entry *entry;
    uint64_t result = 0;

    pthread_rwlock_rdlock( &impl->objects_lock );
    if ((entry = rb_get( &impl->objects, &host_handle )))
    {
        struct vulkan_object *object = RB_ENTRY_VALUE( entry, struct vulkan_object, entry );
        result = object->client_handle;
    }
    pthread_rwlock_unlock( &impl->objects_lock );
    return result;
}

static void vulkan_instance_insert_object( struct vulkan_instance *instance, struct vulkan_object *obj )
{
    struct instance *impl = CONTAINING_RECORD( instance, struct instance, obj );
    if (impl->objects.compare)
    {
        pthread_rwlock_wrlock( &impl->objects_lock );
        rb_put( &impl->objects, &obj->host_handle, &obj->entry );
        pthread_rwlock_unlock( &impl->objects_lock );
    }
}

static void vulkan_instance_remove_object( struct vulkan_instance *instance, struct vulkan_object *obj )
{
    struct instance *impl = CONTAINING_RECORD( instance, struct instance, obj );
    if (impl->objects.compare)
    {
        pthread_rwlock_wrlock( &impl->objects_lock );
        rb_remove( &impl->objects, &obj->entry );
        pthread_rwlock_unlock( &impl->objects_lock );
    }
}

static void free_debug_utils_messengers( struct list *messengers )
{
    struct vulkan_debug_utils_messenger *messenger, *next;

    LIST_FOR_EACH_ENTRY_SAFE( messenger, next, messengers, struct vulkan_debug_utils_messenger, entry )
    {
        list_remove( &messenger->entry );
        free( messenger );
    }
}

static void free_debug_report_callbacks( struct list *callbacks )
{
    struct vulkan_debug_report_callback *callback, *next;

    LIST_FOR_EACH_ENTRY_SAFE( callback, next, callbacks, struct vulkan_debug_report_callback, entry )
    {
        list_remove( &callback->entry );
        free( callback );
    }
}

static VkResult convert_instance_create_info( struct mempool *pool, VkInstanceCreateInfo *info, struct instance *instance )
{
    const VkBaseInStructure *header = (const VkBaseInStructure *)info;
    const VkDebugReportCallbackCreateInfoEXT *debug_report_callback;
    const char **extensions;
    uint32_t count = 0;

    while ((header = find_next_struct( header->pNext, VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT )))
    {
        const VkDebugUtilsMessengerCreateInfoEXT *debug_utils_messenger = (const VkDebugUtilsMessengerCreateInfoEXT *)header;
        struct vulkan_debug_utils_messenger *messenger = debug_utils_messenger->pUserData;

        list_remove( &messenger->entry );
        list_add_tail( &instance->utils_messengers, &messenger->entry );
        messenger->instance = &instance->obj;
    }

    if ((debug_report_callback = find_next_struct( info->pNext, VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT )))
    {
        struct vulkan_debug_report_callback *callback = debug_report_callback->pUserData;

        list_remove( &callback->entry );
        list_add_tail( &instance->report_callbacks, &callback->entry );
        callback->instance = &instance->obj;
    }

    if (info->enabledLayerCount)
    {
        FIXME( "Loading explicit layers is not supported!\n" );
        return VK_ERROR_LAYER_NOT_PRESENT;
    }

    instance->enable_win32_surface = instance->obj.extensions.has_VK_KHR_win32_surface;
    driver_funcs->p_map_instance_extensions( &instance->obj.extensions );
    instance->obj.extensions.has_VK_KHR_win32_surface = 0;

    if (instance->obj.extensions.has_VK_EXT_debug_utils || instance->obj.extensions.has_VK_EXT_debug_report)
    {
        rb_init( &instance->objects, vulkan_object_compare );
        pthread_rwlock_init( &instance->objects_lock, NULL );
    }

    if (instance->enable_win32_surface && vulkan_funcs.host_extensions.has_VK_KHR_get_surface_capabilities2)
    {
        instance->obj.extensions.has_VK_KHR_get_surface_capabilities2 = 1;
        instance->obj.extensions.has_VK_EXT_surface_maintenance1 |=
            vulkan_funcs.host_extensions.has_VK_EXT_surface_maintenance1;
        instance->obj.extensions.has_VK_KHR_surface_maintenance1 |=
            vulkan_funcs.host_extensions.has_VK_KHR_surface_maintenance1;
    }
    if (vulkan_funcs.host_extensions.has_VK_KHR_get_physical_device_properties2)
        instance->obj.extensions.has_VK_KHR_get_physical_device_properties2 = 1;
    if (use_external_memory())
        instance->obj.extensions.has_VK_KHR_external_memory_capabilities = 1;

    /* VK_KHR_win32_keyed_mutex only requires external memory extensions, but we will use
     * external semaphore fds to implement it, so we enable the instance extensions too */
    instance->obj.extensions.has_VK_KHR_external_semaphore_capabilities = 1;

    if (!(extensions = mem_alloc( pool, sizeof(instance->obj.extensions) * 8 * sizeof(*extensions) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
#define USE_VK_EXT(x) if (instance->obj.extensions.has_ ## x) extensions[count++] = #x;
    ALL_VK_INSTANCE_EXTS
#undef USE_VK_EXT

    TRACE( "Enabling %u host instance extensions\n", count );
    for (const char **extension = extensions, **end = extension + count; extension < end; extension++)
        TRACE( "  - %s\n", debugstr_a(*extension) );

    info->ppEnabledExtensionNames = extensions;
    info->enabledExtensionCount = count;
    return VK_SUCCESS;
}

static VkResult init_physical_device( struct vulkan_physical_device *physical_device, VkPhysicalDevice host_physical_device,
                                      VkPhysicalDevice client_physical_device, struct vulkan_instance *instance )
{
    struct vulkan_device_extensions extensions = {0};
    VkExtensionProperties *properties;
    uint32_t count;
    VkResult res;

    vulkan_object_init_ptr( &physical_device->obj, (UINT_PTR)host_physical_device, &client_physical_device->obj );
    physical_device->instance = instance;

    instance->p_vkGetPhysicalDeviceMemoryProperties( host_physical_device, &physical_device->memory_properties );

    if ((res = instance->p_vkEnumerateDeviceExtensionProperties( host_physical_device, NULL, &count, NULL ))) return res;
    if (!(properties = calloc( count, sizeof(*properties) ))) return res;
    if ((res = instance->p_vkEnumerateDeviceExtensionProperties( host_physical_device, NULL, &count, properties ))) goto done;

    TRACE( "Host physical device extensions:\n" );
    for (uint32_t i = 0; i < count; i++)
    {
        const char *extension = properties[i].extensionName;
#define USE_VK_EXT(x)                           \
        if (!strcmp( extension, #x ))           \
        {                                       \
            extensions.has_ ## x = 1;           \
            TRACE( "  - %s\n", extension );     \
        } else
        ALL_VK_DEVICE_EXTS
#undef USE_VK_EXT
        WARN( "Extension %s is not supported.\n", debugstr_a(extension) );
    }
    physical_device->extensions = extensions;

    if (zero_bits && physical_device->extensions.has_VK_EXT_map_memory_placed && physical_device->extensions.has_VK_KHR_map_memory2)
    {
        VkPhysicalDeviceMapMemoryPlacedFeaturesEXT map_placed_feature = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &map_placed_feature};

        instance->p_vkGetPhysicalDeviceFeatures2KHR( host_physical_device, &features );
        if (map_placed_feature.memoryMapPlaced && map_placed_feature.memoryUnmapReserve)
        {
            VkPhysicalDeviceMapMemoryPlacedPropertiesEXT map_placed_props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext = &map_placed_props};

            instance->p_vkGetPhysicalDeviceProperties2( host_physical_device, &props );
            physical_device->map_placed_align = map_placed_props.minPlacedMemoryMapAlignment;
            TRACE( "Using placed map with alignment %u\n", physical_device->map_placed_align );
        }
    }

    if (zero_bits && physical_device->extensions.has_VK_EXT_external_memory_host && !physical_device->map_placed_align)
    {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_mem_props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &host_mem_props};

        instance->p_vkGetPhysicalDeviceProperties2KHR( host_physical_device, &props );
        physical_device->external_memory_align = host_mem_props.minImportedHostPointerAlignment;
        if (physical_device->external_memory_align) WARN( "Not using VK_EXT_external_memory_host for memory mapping\n" );
        else TRACE( "Using VK_EXT_external_memory_host for memory mapping with alignment: %u\n", physical_device->external_memory_align );
    }

    driver_funcs->p_map_device_extensions( &extensions );
    if (extensions.has_VK_KHR_external_memory_win32 && zero_bits && !physical_device->map_placed_align)
    {
        WARN( "Cannot export WOW64 memory without VK_EXT_map_memory_placed\n" );
        extensions.has_VK_KHR_external_memory_win32 = 0;
    }
    extensions.has_VK_KHR_win32_keyed_mutex = extensions.has_VK_KHR_timeline_semaphore &&
                                              extensions.has_VK_KHR_external_semaphore_fd;

    /* filter out unsupported client device extensions */
#define USE_VK_EXT(x) client_physical_device->extensions.has_ ## x = extensions.has_ ## x;
    ALL_VK_CLIENT_DEVICE_EXTS
#undef USE_VK_EXT

done:
    free( properties );
    return res;
}

/* Helper function which stores wrapped physical devices in the instance object. */
static VkResult init_physical_devices( struct vulkan_instance *instance, struct vulkan_physical_device *physical_devices )
{
    VkInstance client_instance = instance->client.instance;
    VkPhysicalDevice *host_physical_devices;
    uint32_t physical_device_count;
    unsigned int i;
    VkResult res;

    if ((res = instance->p_vkEnumeratePhysicalDevices( instance->host.instance, &physical_device_count, NULL )))
    {
        ERR( "Failed to enumerate physical devices, res %d\n", res );
        return res;
    }
    if (!physical_device_count) return res;

    if (physical_device_count > client_instance->physical_device_count)
    {
        client_instance->physical_device_count = physical_device_count;
        return VK_ERROR_OUT_OF_POOL_MEMORY;
    }
    client_instance->physical_device_count = physical_device_count;

    if (!(host_physical_devices = calloc( physical_device_count, sizeof(*host_physical_devices) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if ((res = instance->p_vkEnumeratePhysicalDevices( instance->host.instance, &physical_device_count, host_physical_devices ))) goto failed;

    /* Wrap each host physical device handle into a dispatchable object for the ICD loader. */
    for (i = 0; i < physical_device_count; i++)
    {
        VkPhysicalDevice client_physical_device = &client_instance->physical_device[i];
        struct vulkan_physical_device *physical_device = physical_devices + i;
        if ((res = init_physical_device( physical_device, host_physical_devices[i], client_physical_device, instance ))) goto failed;
    }
    instance->physical_device_count = physical_device_count;
    instance->physical_devices = physical_devices;

failed:
    free( host_physical_devices );
    return res;
}

static VkResult win32u_vkCreateInstance( const VkInstanceCreateInfo *client_create_info, const VkAllocationCallbacks *allocator,
                                         VkInstance *client_instance_ptr )
{
    VkInstanceCreateInfo *create_info = (VkInstanceCreateInfo *)client_create_info; /* cast away const, chain has been copied in the thunks */
    VkInstance host_instance = VK_NULL_HANDLE, client_instance = *client_instance_ptr;
    struct vulkan_physical_device *physical_devices;
    struct mempool pool = {0};
    struct instance *instance;
    unsigned int i;
    VkResult res;

    if (!(instance = calloc( 1, sizeof(*instance) + sizeof(*physical_devices) * client_instance->physical_device_count) ))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    physical_devices = (struct vulkan_physical_device *)(instance + 1);
    instance->obj.extensions = client_instance->extensions;
    list_init( &instance->utils_messengers );
    list_init( &instance->report_callbacks );

    if ((res = convert_instance_create_info( &pool, create_info, instance ))) goto failed;
    if ((res = p_vkCreateInstance( create_info, NULL /* allocator */, &host_instance ))) goto failed;

    vulkan_object_init_ptr( &instance->obj.obj, (UINT_PTR)host_instance, &client_instance->obj );
    instance->obj.p_insert_object = vulkan_instance_insert_object;
    instance->obj.p_remove_object = vulkan_instance_remove_object;
    instance->obj.p_client_handle_from_host = vulkan_instance_client_handle_from_host;

#define USE_VK_FUNC( name )                                                                          \
    instance->obj.p_##name = (void *)p_vkGetInstanceProcAddr( instance->obj.host.instance, #name );  \
    if (!instance->obj.p_##name) TRACE( "Instance proc %s not found.\n", #name );
    ALL_VK_INSTANCE_FUNCS
#undef USE_VK_FUNC

    /* Cache physical devices for vkEnumeratePhysicalDevices within the instance as each vkPhysicalDevice is a dispatchable
     * object, which means we need to wrap the host physical devices and present those to the application.
     */
    if ((res = init_physical_devices( &instance->obj, physical_devices ))) goto failed;

    TRACE( "Created instance %p, host_instance %p.\n", instance, instance->obj.host.instance );
    for (i = 0; i < instance->obj.physical_device_count; i++)
    {
        struct vulkan_physical_device *physical_device = &instance->obj.physical_devices[i];
        vulkan_instance_insert_object( &instance->obj, &physical_device->obj );
    }
    vulkan_instance_insert_object( &instance->obj, &instance->obj.obj );

failed:
    if (res)
    {
        WARN( "Failed to create vulkan instance, res %d\n", res );
        if (host_instance) instance->obj.p_vkDestroyInstance( host_instance, NULL /* allocator */ );
        free_debug_utils_messengers( &instance->utils_messengers );
        free_debug_report_callbacks( &instance->report_callbacks );
        free( instance );
    }
    mem_free( &pool );
    return res;
}

static void win32u_vkDestroyInstance( VkInstance client_instance, const VkAllocationCallbacks *allocator )
{
    struct instance *instance = instance_from_handle( client_instance );

    if (!instance) return;

    instance->obj.p_vkDestroyInstance( instance->obj.host.instance, NULL /* allocator */ );
    for (int i = 0; i < instance->obj.physical_device_count; i++)
        vulkan_instance_remove_object( &instance->obj, &instance->obj.physical_devices[i].obj );
    vulkan_instance_remove_object( &instance->obj, &instance->obj.obj );

    if (instance->objects.compare) pthread_rwlock_destroy( &instance->objects_lock );
    free_debug_utils_messengers( &instance->utils_messengers );
    free_debug_report_callbacks( &instance->report_callbacks );
    free( instance );
}

static BOOL get_swapchain_maintenance1_features( struct vulkan_physical_device *physical_device,
                                                VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR *maintenance,
                                                BOOL *khr )
{
    struct vulkan_instance *instance = physical_device->instance;
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                         .pNext = maintenance};
    BOOL ext;

    *khr = instance->extensions.has_VK_KHR_surface_maintenance1 &&
           physical_device->extensions.has_VK_KHR_swapchain_maintenance1;
    ext = instance->extensions.has_VK_EXT_surface_maintenance1 &&
          physical_device->extensions.has_VK_EXT_swapchain_maintenance1;
    if (!*khr && !ext) return FALSE;

    if (instance->p_vkGetPhysicalDeviceFeatures2)
        instance->p_vkGetPhysicalDeviceFeatures2( physical_device->host.physical_device, &features );
    else if (instance->p_vkGetPhysicalDeviceFeatures2KHR)
        instance->p_vkGetPhysicalDeviceFeatures2KHR( physical_device->host.physical_device, &features );
    return maintenance->swapchainMaintenance1;
}

static VkResult enable_swapchain_maintenance1( struct vulkan_physical_device *physical_device,
                                              VkDeviceCreateInfo *info, struct mempool *pool,
                                              struct vulkan_device *device )
{
    struct device *impl = impl_from_vulkan_device( device );
    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maintenance =
    {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
    }, *enable;
    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR *application;
    BOOL khr;

    if (!device->extensions.has_VK_KHR_swapchain) return VK_SUCCESS;
    /* The thunk copied the application chain. A disabled feature must not
     * disable Wine's private Present fences on a capable host. KHR and EXT
     * share a structure type; enable that copy without adding a duplicate. */
    application = (void *)find_next_struct( info->pNext, maintenance.sType );
    if (application && application->swapchainMaintenance1)
    {
        impl->swapchain_maintenance1 = application->swapchainMaintenance1 &&
            (device->extensions.has_VK_KHR_swapchain_maintenance1 ||
             device->extensions.has_VK_EXT_swapchain_maintenance1);
        return VK_SUCCESS;
    }
    if (!get_swapchain_maintenance1_features( physical_device, &maintenance, &khr )) return VK_SUCCESS;

    if (application) application->swapchainMaintenance1 = VK_TRUE;
    else
    {
        if (!(enable = mem_alloc( pool, sizeof(*enable) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
        *enable = maintenance;
        enable->pNext = (void *)info->pNext;
        info->pNext = enable;
    }
    if (!device->extensions.has_VK_KHR_swapchain_maintenance1 &&
        !device->extensions.has_VK_EXT_swapchain_maintenance1)
    {
        if (khr) device->extensions.has_VK_KHR_swapchain_maintenance1 = 1;
        else device->extensions.has_VK_EXT_swapchain_maintenance1 = 1;
    }
    impl->swapchain_maintenance1 = TRUE;
    return VK_SUCCESS;
}

static VkResult convert_device_create_info( struct vulkan_physical_device *physical_device, VkDeviceCreateInfo *info,
                                            struct mempool *pool, struct vulkan_device *device )
{
    struct vulkan_instance *instance = physical_device->instance;
    const char **extensions;
    uint32_t count = 0;
    VkResult res;

    /* Should be filtered out by loader as ICDs don't support layers. */
    info->enabledLayerCount = 0;
    info->ppEnabledLayerNames = NULL;

    if (device->extensions.has_VK_KHR_win32_keyed_mutex)
    {
        device->extensions.has_VK_KHR_timeline_semaphore = 1;
        device->extensions.has_VK_KHR_external_semaphore_fd = 1;
        device->extensions.has_VK_KHR_external_semaphore = 1;
    }

    driver_funcs->p_map_device_extensions( &device->extensions );
    device->extensions.has_VK_KHR_win32_keyed_mutex = 0;
    device->extensions.has_VK_KHR_external_memory_win32 = 0;
    device->extensions.has_VK_KHR_external_fence_win32 = 0;
    device->extensions.has_VK_KHR_external_semaphore_win32 = 0;

    /* For Direct3D VA support. */
    device->extensions.has_VK_EXT_external_memory_dma_buf = physical_device->extensions.has_VK_EXT_external_memory_dma_buf;
    device->extensions.has_VK_EXT_image_drm_format_modifier = physical_device->extensions.has_VK_EXT_image_drm_format_modifier;
    device->extensions.has_VK_KHR_image_format_list = physical_device->extensions.has_VK_KHR_image_format_list;
    device->extensions.has_VK_EXT_physical_device_drm = physical_device->extensions.has_VK_EXT_physical_device_drm;

    if (physical_device->map_placed_align)
    {
        VkPhysicalDeviceMapMemoryPlacedFeaturesEXT *map_placed_features;

        if (!(map_placed_features = mem_alloc( pool, sizeof(*map_placed_features) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
        map_placed_features->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT;
        map_placed_features->pNext = (void *)info->pNext;
        map_placed_features->memoryMapPlaced = VK_TRUE;
        map_placed_features->memoryMapRangePlaced = VK_FALSE;
        map_placed_features->memoryUnmapReserve = VK_TRUE;
        info->pNext = map_placed_features;

        device->extensions.has_VK_EXT_map_memory_placed = 1;
        device->extensions.has_VK_KHR_map_memory2 = 1;
    }
    else if (physical_device->external_memory_align)
    {
        device->extensions.has_VK_KHR_external_memory = 1;
        device->extensions.has_VK_EXT_external_memory_host = 1;
    }

    /* Both presentation scaling and present fences require the feature, not
     * just an advertised extension name. */
    if ((res = enable_swapchain_maintenance1( physical_device, info, pool, device ))) return res;

    /* An offscreen client surface is copied into its top-level window after
     * QueuePresent.  Since QueuePresent is asynchronous, enable the host's
     * presentation-completion primitives internally so that the copy reads
     * the frame submitted by that call rather than the previous image.  Do
     * not add duplicate feature structures when the application supplied its
     * own (even if it left the corresponding feature disabled). */
    if (device->extensions.has_VK_KHR_swapchain &&
        !device->extensions.has_VK_KHR_present_id && !device->extensions.has_VK_KHR_present_wait &&
        !find_next_struct( info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR ) &&
        !find_next_struct( info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR ) &&
        physical_device->extensions.has_VK_KHR_present_id &&
        physical_device->extensions.has_VK_KHR_present_wait)
    {
        VkPhysicalDevicePresentWaitFeaturesKHR wait_features =
        {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
        };
        VkPhysicalDevicePresentIdFeaturesKHR id_features =
        {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
            .pNext = &wait_features,
        };
        VkPhysicalDeviceFeatures2 features =
        {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &id_features,
        };

        instance->p_vkGetPhysicalDeviceFeatures2( physical_device->host.physical_device, &features );
        if (id_features.presentId && wait_features.presentWait)
        {
            VkPhysicalDevicePresentWaitFeaturesKHR *enable_wait;
            VkPhysicalDevicePresentIdFeaturesKHR *enable_id;

            if (!(enable_id = mem_alloc( pool, sizeof(*enable_id) )) ||
                !(enable_wait = mem_alloc( pool, sizeof(*enable_wait) )))
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            *enable_wait = wait_features;
            enable_wait->pNext = (void *)info->pNext;
            *enable_id = id_features;
            enable_id->pNext = enable_wait;
            info->pNext = enable_id;
            device->extensions.has_VK_KHR_present_id = 1;
            device->extensions.has_VK_KHR_present_wait = 1;
            device->internal_present_wait = TRUE;
        }
    }

    if (!(extensions = mem_alloc( pool, sizeof(device->extensions) * 8 * sizeof(*extensions) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
#define USE_VK_EXT(x) if (device->extensions.has_ ## x) extensions[count++] = #x;
    ALL_VK_DEVICE_EXTS
#undef USE_VK_EXT

    TRACE( "Enabling %u host device extensions\n", count );
    for (const char **extension = extensions, **end = extension + count; extension < end; extension++)
        TRACE( "  - %s\n", debugstr_a(*extension) );

    info->ppEnabledExtensionNames = extensions;
    info->enabledExtensionCount = count;
    return VK_SUCCESS;
}

static void init_device_queues( struct vulkan_device *device, const VkDeviceQueueCreateInfo *create_info, VkDevice client_device )
{
    VkDeviceQueueInfo2 info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
    VkQueue client_queues = client_device->queues + device->queue_count;
    struct vulkan_queue *queues = device->queues + device->queue_count;

    TRACE( "Queue family index %u, queue count %u.\n", create_info->queueFamilyIndex, create_info->queueCount );

    info.flags = create_info->flags;
    info.queueFamilyIndex = create_info->queueFamilyIndex;
    for (info.queueIndex = 0; info.queueIndex < create_info->queueCount; info.queueIndex++)
    {
        VkQueue host_queue, client_queue = client_queues + info.queueIndex;
        struct vulkan_queue *queue = queues + info.queueIndex;

        if (info.flags && device->p_vkGetDeviceQueue2) device->p_vkGetDeviceQueue2( device->host.device, &info, &host_queue );
        else device->p_vkGetDeviceQueue( device->host.device, info.queueFamilyIndex, info.queueIndex, &host_queue );
        vulkan_object_init_ptr( &queue->obj, (UINT_PTR)host_queue, &client_queue->obj );
        queue->device = device;
        queue->info = info;

        TRACE( "Got device %p queue %p, host_queue %p.\n", device, queue, queue->host.queue );
    }

    device->queue_count += create_info->queueCount;
}

static VkResult win32u_vkCreateDevice( VkPhysicalDevice client_physical_device, const VkDeviceCreateInfo *client_create_info,
                                       const VkAllocationCallbacks *allocator, VkDevice *client_device_ptr )
{
    VkDeviceCreateInfo *create_info = (VkDeviceCreateInfo *)client_create_info; /* cast away const, chain has been copied in the thunks */
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;
    VkDevice host_device, client_device = *client_device_ptr;
    struct vulkan_device *device;
    struct device *impl;
    unsigned int queue_count, i;
    struct mempool pool = {0};
    VkResult res;

    if (TRACE_ON(vulkan))
    {
        VkPhysicalDeviceProperties properties = {0};
        instance->p_vkGetPhysicalDeviceProperties( physical_device->host.physical_device, &properties );
        TRACE( "Device name: %s.\n", debugstr_a(properties.deviceName) );
        TRACE( "Vendor ID: %#x, Device ID: %#x.\n", properties.vendorID, properties.deviceID );
        TRACE( "Driver version: %#x.\n", properties.driverVersion );
    }

    /* We need to cache all queues within the device as each requires wrapping since queues are dispatchable objects. */
    for (queue_count = 0, i = 0; i < create_info->queueCreateInfoCount; i++) queue_count += create_info->pQueueCreateInfos[i].queueCount;

    if (!(impl = calloc( 1, offsetof(struct device, obj.queues[queue_count]) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (queue_count && !(impl->completion_domain_base = client_surface_allocate_completion_domains( queue_count )))
    {
        free( impl );
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (pthread_mutex_init( &impl->retirement_lock, NULL ))
    {
        free( impl );
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (client_surface_cond_init( &impl->retirement_cond ))
    {
        pthread_mutex_destroy( &impl->retirement_lock );
        free( impl );
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    list_init( &impl->retired_swapchains );
    device = &impl->obj;
    device->extensions = client_device->extensions;

    if ((res = convert_device_create_info( physical_device, create_info, &pool, device ))) goto failed;
    if ((res = instance->p_vkCreateDevice( physical_device->host.physical_device, create_info, NULL /* allocator */, &host_device ))) goto failed;

    vulkan_object_init_ptr( &device->obj, (UINT_PTR)host_device, &client_device->obj );
    device->physical_device = physical_device;

#define USE_VK_FUNC( name )                                                          \
    device->p_##name = (void *)p_vkGetDeviceProcAddr( device->host.device, #name );  \
    if (!device->p_##name) TRACE( "Device proc %s not found.\n", #name );
    ALL_VK_DEVICE_FUNCS
#undef USE_VK_FUNC

    for (i = 0; i < create_info->queueCreateInfoCount; i++) init_device_queues( device, create_info->pQueueCreateInfos + i, client_device );

    TRACE( "Created device %p, host_device %p.\n", device, device->host.device );
    for (struct vulkan_queue *queue = device->queues; queue < device->queues + device->queue_count; queue++)
        instance->p_insert_object( instance, &queue->obj );
    instance->p_insert_object( instance, &device->obj );

failed:
    if (res)
    {
        WARN( "Failed to create device, res %d\n", res );
        pthread_cond_destroy( &impl->retirement_cond );
        pthread_mutex_destroy( &impl->retirement_lock );
        free( impl );
    }
    mem_free( &pool );
    return res;
}

static void win32u_vkDestroyDevice( VkDevice client_device, const VkAllocationCallbacks *allocator )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_instance *instance;
    struct device *impl;
    HANDLE worker;
    unsigned int i;

    if (!device) return;

    instance = device->physical_device->instance;
    impl = impl_from_vulkan_device( device );
    /* Retired copies retain the device and their host surfaces. Device
     * teardown is the final drain; individual window/swapchain destruction
     * does not wait for another surface's GPU work. */
    pthread_mutex_lock( &impl->retirement_lock );
    assert( !impl->retirement_starting );
    impl->retirement_shutdown = TRUE;
    worker = impl->retirement_worker;
    pthread_cond_broadcast( &impl->retirement_cond );
    pthread_mutex_unlock( &impl->retirement_lock );
    if (worker)
    {
        NtWaitForSingleObject( worker, FALSE, NULL );
        NtClose( worker );
        /* The slot belongs to this device until actual native thread exit. */
        InterlockedDecrement( &swapchain_retirement_workers );
    }
    assert( list_empty( &impl->retired_swapchains ) );

    device->p_vkDestroyDevice( device->host.device, NULL /* pAllocator */ );
    for (i = 0; i < device->queue_count; i++)
        instance->p_remove_object( instance, &device->queues[i].obj );
    instance->p_remove_object( instance, &device->obj );

    pthread_cond_destroy( &impl->retirement_cond );
    pthread_mutex_destroy( &impl->retirement_lock );
    free( impl );
}

static VkQueue device_find_queue( VkDevice client_device, const VkDeviceQueueInfo2 *info )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );

    for (struct vulkan_queue *queue = device->queues; queue < device->queues + device->queue_count; queue++)
        if (!memcmp( &queue->info, info, sizeof(*info) )) return queue->client.queue;

    return VK_NULL_HANDLE;
}

static void win32u_vkGetDeviceQueue( VkDevice client_device, uint32_t family_index, uint32_t queue_index, VkQueue *client_queue )
{
    VkDeviceQueueInfo2 info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
    info.queueFamilyIndex = family_index;
    info.queueIndex = queue_index;

    *client_queue = device_find_queue( client_device, &info );
}

static void win32u_vkGetDeviceQueue2( VkDevice client_device, const VkDeviceQueueInfo2 *client_info, VkQueue *client_queue )
{
    VkDeviceQueueInfo2 info = *client_info;
    if (info.pNext) FIXME( "pNext not implemented\n" );
    info.pNext = NULL;

    *client_queue = device_find_queue( client_device, &info );
}

static VkResult win32u_vkAllocateMemory( VkDevice client_device, const VkMemoryAllocateInfo *client_alloc_info,
                                         const VkAllocationCallbacks *allocator, VkDeviceMemory *ret )
{
    VkImportMemoryFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    VkMemoryAllocateInfo *alloc_info = (VkMemoryAllocateInfo *)client_alloc_info; /* cast away const, chain has been copied in the thunks */
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)alloc_info;
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct vulkan_instance *instance = device->physical_device->instance;
    VkImportMemoryHostPointerInfoEXT host_pointer_info, *pointer_info = NULL;
    VkExportMemoryWin32HandleInfoKHR export_win32 = {.dwAccess = GENERIC_ALL};
    VkImportMemoryWin32HandleInfoKHR *import_win32 = NULL;
    VkDeviceMemory host_device_memory = VK_NULL_HANDLE;
    VkExportMemoryAllocateInfo *export_info = NULL;
    struct device_memory *memory;
    BOOL nt_shared = FALSE;
    uint32_t mem_flags;
    void *mapping = NULL;
    VkResult res;

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_MEMORY_ALLOCATE_INFO_NV: break;
        case VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO:
            export_info = (VkExportMemoryAllocateInfo *)*next;
            if (!(export_info->handleTypes & EXTERNAL_MEMORY_WIN32_BITS))
                FIXME( "Unsupported handle types %#x\n", export_info->handleTypes );
            else
            {
                nt_shared = !(export_info->handleTypes & (VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT |
                                                          VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT));
                export_info->handleTypes = get_host_external_memory_type();
            }
            break;
        case VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR:
            export_win32 = *(VkExportMemoryWin32HandleInfoKHR *)*next;
            *next = (*next)->pNext; next = &prev;
            break;
        case VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT:
            pointer_info = (VkImportMemoryHostPointerInfoEXT *)*next;
            break;
        case VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR:
            import_win32 = (VkImportMemoryWin32HandleInfoKHR *)*next;
            *next = (*next)->pNext; next = &prev;
            break;
        case VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO: break;
        case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO: break;
        case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_TENSOR_ARM: break;
        case VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO: break;
        case VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    /* For host visible memory, we try to use VK_EXT_external_memory_host on wow64 to ensure that mapped pointer is 32-bit. */
    mem_flags = physical_device->memory_properties.memoryTypes[alloc_info->memoryTypeIndex].propertyFlags;
    if (physical_device->external_memory_align && (mem_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !pointer_info &&
        (res = allocate_external_host_memory( device, alloc_info, mem_flags, &host_pointer_info )))
        return res;

    if (!(memory = calloc( 1, sizeof(*memory) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;

    if (import_win32)
    {
        switch (import_win32->handleType)
        {
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT:
            memory->global = PtrToUlong( import_win32->handle );
            memory->local = d3dkmt_open_resource( memory->global, NULL, &memory->mutex, &memory->sync );
            break;
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT:
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT:
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT:
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT:
        {
            HANDLE shared = import_win32->handle;
            if (import_win32->name && !(shared = open_shared_resource_from_name( import_win32->name ))) break;
            memory->local = d3dkmt_open_resource( 0, shared, &memory->mutex, &memory->sync );
            if (shared && shared != import_win32->handle) NtClose( shared );
            break;
        }
        default:
            FIXME( "Unsupported handle type %#x\n", import_win32->handleType );
            break;
        }

        if (device->client.device->extensions.has_VK_KHR_win32_keyed_mutex && memory->sync)
        {
            VkSemaphoreTypeCreateInfo semaphore_type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            VkSemaphoreCreateInfo semaphore_create = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &semaphore_type};
            VkImportSemaphoreFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};

            semaphore_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            if ((res = device->p_vkCreateSemaphore( device->host.device, &semaphore_create, NULL, &memory->semaphore ))) goto failed;

            fd_info.handleType = get_host_external_semaphore_type();
            fd_info.semaphore = memory->semaphore;
            if ((fd_info.fd = d3dkmt_object_get_fd( memory->sync )) < 0)
            {
                res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
                goto failed;
            }

            if ((res = device->p_vkImportSemaphoreFdKHR( device->host.device, &fd_info ))) goto failed;
        }

        if ((fd_info.fd = d3dkmt_object_get_fd( memory->local )) < 0)
        {
            res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
            goto failed;
        }

        fd_info.handleType = get_host_external_memory_type();
        fd_info.pNext = alloc_info->pNext;
        alloc_info->pNext = &fd_info;
    }

    if ((res = device->p_vkAllocateMemory( device->host.device, alloc_info, NULL, &host_device_memory ))) goto failed;

    if (export_info)
    {
        if (!memory->local)
        {
            VkMemoryGetFdInfoKHR get_fd_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = host_device_memory};
            int fd = -1;

            switch ((get_fd_info.handleType = get_host_external_memory_type()))
            {
            case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:
                if ((res = device->p_vkGetMemoryFdKHR( device->host.device, &get_fd_info, &fd ))) goto failed;
                break;
            default:
                FIXME( "Unsupported handle type %#x\n", get_fd_info.handleType );
                break;
            }

            memory->local = d3dkmt_create_resource( fd, nt_shared ? NULL : &memory->global );
            close( fd );

            if (!memory->local) goto failed;
        }
        if (nt_shared && !(memory->shared = create_shared_resource_handle( memory->local, &export_win32 ))) goto failed;
    }

    vulkan_object_init( &memory->obj.obj, host_device_memory );
    memory->size = alloc_info->allocationSize;
    memory->vm_map = mapping;
    instance->p_insert_object( instance, &memory->obj.obj );

    *ret = memory->obj.client.device_memory;
    return VK_SUCCESS;

failed:
    WARN( "Failed to allocate memory, res %d\n", res );
    if (host_device_memory) device->p_vkFreeMemory( device->host.device, host_device_memory, NULL );
    if (memory->semaphore) device->p_vkDestroySemaphore( device->host.device, memory->semaphore, NULL );
    d3dkmt_destroy_resource( memory->local );
    d3dkmt_destroy_mutex( memory->mutex );
    d3dkmt_destroy_sync( memory->sync );
    free( memory );
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void win32u_vkFreeMemory( VkDevice client_device, VkDeviceMemory client_memory, const VkAllocationCallbacks *allocator )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct vulkan_instance *instance = device->physical_device->instance;
    struct device_memory *memory;

    if (!client_memory) return;
    memory = device_memory_from_handle( client_memory );

    if (memory->vm_map && !physical_device->external_memory_align)
    {
        const VkMemoryUnmapInfoKHR info =
        {
            .sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR,
            .memory = memory->obj.host.device_memory,
            .flags = VK_MEMORY_UNMAP_RESERVE_BIT_EXT,
        };
        device->p_vkUnmapMemory2KHR( device->host.device, &info );
    }

    device->p_vkFreeMemory( device->host.device, memory->obj.host.device_memory, NULL );
    instance->p_remove_object( instance, &memory->obj.obj );

    if (memory->vm_map)
    {
        SIZE_T alloc_size = 0;
        NtFreeVirtualMemory( GetCurrentProcess(), &memory->vm_map, &alloc_size, MEM_RELEASE );
    }

    if (memory->semaphore) device->p_vkDestroySemaphore( device->host.device, memory->semaphore, NULL );
    if (memory->shared) NtClose( memory->shared );
    d3dkmt_destroy_resource( memory->local );
    d3dkmt_destroy_mutex( memory->mutex );
    d3dkmt_destroy_sync( memory->sync );
    free( memory );
}

static VkResult win32u_vkGetMemoryWin32HandleKHR( VkDevice client_device, const VkMemoryGetWin32HandleInfoKHR *handle_info, HANDLE *handle )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct device_memory *memory = device_memory_from_handle( handle_info->memory );

    TRACE( "device %p, handle_info %p, handle %p\n", device, handle_info, handle );

    switch (handle_info->handleType)
    {
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT:
        TRACE( "Returning global D3DKMT handle %#x\n", memory->global );
        *handle = UlongToPtr( memory->global );
        return VK_SUCCESS;

    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT:
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT:
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT:
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT:
        NtDuplicateObject( NtCurrentProcess(), memory->shared, NtCurrentProcess(), handle, 0, 0, DUPLICATE_SAME_ATTRIBUTES | DUPLICATE_SAME_ACCESS );
        TRACE( "Returning NT shared handle %p -> %p\n", memory->shared, *handle );
        return VK_SUCCESS;

    default:
        FIXME( "Unsupported handle type %#x\n", handle_info->handleType );
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
}

static BOOL is_d3dkmt_global( D3DKMT_HANDLE handle )
{
    return (handle & 0xc0000000) && (handle & 0x3f) == 2;
}

static VkResult win32u_vkGetMemoryWin32HandlePropertiesKHR( VkDevice client_device, VkExternalMemoryHandleTypeFlagBits handle_type, HANDLE handle,
                                                            VkMemoryWin32HandlePropertiesKHR *handle_properties )
{
    static const UINT d3dkmt_type_bits = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT | VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
    struct vulkan_device *device = vulkan_device_from_handle( client_device );

    TRACE( "device %p, handle_type %#x, handle %p, handle_properties %p\n", device, handle_type, handle, handle_properties );

    if (is_d3dkmt_global( HandleToULong( handle ) )) handle_properties->memoryTypeBits = d3dkmt_type_bits;
    else handle_properties->memoryTypeBits = EXTERNAL_MEMORY_WIN32_BITS & ~d3dkmt_type_bits;

    return VK_SUCCESS;
}

static VkResult win32u_vkMapMemory2KHR( VkDevice client_device, const VkMemoryMapInfoKHR *map_info, void **data )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct device_memory *memory = device_memory_from_handle( map_info->memory );
    VkMemoryMapInfoKHR info = *map_info;
    VkMemoryMapPlacedInfoEXT placed_info =
    {
        .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT,
    };
    VkResult res;

    info.memory = memory->obj.host.device_memory;
    if (memory->vm_map)
    {
        *data = (char *)memory->vm_map + info.offset;
        TRACE( "returning %p\n", *data );
        return VK_SUCCESS;
    }

    if (physical_device->map_placed_align)
    {
        SIZE_T alloc_size = memory->size;

        placed_info.pNext = info.pNext;
        info.pNext = &placed_info;
        info.offset = 0;
        info.size = VK_WHOLE_SIZE;
        info.flags |= VK_MEMORY_MAP_PLACED_BIT_EXT;

        if (NtAllocateVirtualMemory( GetCurrentProcess(), &placed_info.pPlacedAddress, zero_bits,
                                     &alloc_size, MEM_COMMIT, PAGE_READWRITE ))
        {
            ERR( "NtAllocateVirtualMemory failed\n" );
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    if (device->p_vkMapMemory2KHR)
        res = device->p_vkMapMemory2KHR( device->host.device, &info, data );
    else
    {
        if (info.pNext) FIXME( "struct extension chain not implemented!\n" );
        res = device->p_vkMapMemory( device->host.device, info.memory, info.offset, info.size, info.flags, data );
    }

    if (placed_info.pPlacedAddress)
    {
        if (res != VK_SUCCESS)
        {
            SIZE_T alloc_size = 0;
            ERR( "vkMapMemory2EXT failed: %d\n", res );
            NtFreeVirtualMemory( GetCurrentProcess(), &placed_info.pPlacedAddress, &alloc_size, MEM_RELEASE );
            return res;
        }
        memory->vm_map = placed_info.pPlacedAddress;
        *data = (char *)memory->vm_map + map_info->offset;
        TRACE( "Using placed mapping %p\n", memory->vm_map );
    }

#ifdef _WIN64
    if (NtCurrentTeb()->WowTebOffset && res == VK_SUCCESS && (UINT_PTR)*data >> 32)
    {
        FIXME( "returned mapping %p does not fit 32-bit pointer\n", *data );
        device->p_vkUnmapMemory( device->host.device, memory->obj.host.device_memory );
        *data = NULL;
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
#endif

    return res;
}

static VkResult win32u_vkMapMemory( VkDevice client_device, VkDeviceMemory client_memory, VkDeviceSize offset,
                                    VkDeviceSize size, VkMemoryMapFlags flags, void **data )
{
    const VkMemoryMapInfoKHR info =
    {
        .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR,
        .flags = flags,
        .memory = client_memory,
        .offset = offset,
        .size = size,
    };

    return win32u_vkMapMemory2KHR( client_device, &info, data );
}

static VkResult win32u_vkUnmapMemory2KHR( VkDevice client_device, const VkMemoryUnmapInfoKHR *unmap_info )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct device_memory *memory = device_memory_from_handle( unmap_info->memory );
    VkMemoryUnmapInfoKHR info;
    VkResult res;

    if (memory->vm_map && physical_device->external_memory_align) return VK_SUCCESS;

    if (!device->p_vkUnmapMemory2KHR)
    {
        if (unmap_info->pNext || memory->vm_map) FIXME( "Not implemented\n" );
        device->p_vkUnmapMemory( device->host.device, memory->obj.host.device_memory );
        return VK_SUCCESS;
    }

    info = *unmap_info;
    info.memory = memory->obj.host.device_memory;
    if (memory->vm_map) info.flags |= VK_MEMORY_UNMAP_RESERVE_BIT_EXT;

    res = device->p_vkUnmapMemory2KHR( device->host.device, &info );

    if (res == VK_SUCCESS && memory->vm_map)
    {
        SIZE_T size = 0;
        NtFreeVirtualMemory( GetCurrentProcess(), &memory->vm_map, &size, MEM_RELEASE );
        memory->vm_map = NULL;
    }
    return res;
}

static void win32u_vkUnmapMemory( VkDevice client_device, VkDeviceMemory client_memory )
{
    const VkMemoryUnmapInfoKHR info =
    {
        .sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR,
        .memory = client_memory,
    };

    win32u_vkUnmapMemory2KHR( client_device, &info );
}

static VkResult win32u_vkCreateBuffer( VkDevice client_device, const VkBufferCreateInfo *create_info,
                                       const VkAllocationCallbacks *allocator, VkBuffer *buffer )
{
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)create_info; /* cast away const, chain has been copied in the thunks */
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    VkExternalMemoryBufferCreateInfo host_external_info, *external_info = NULL;

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_BUFFER_CREATE_INFO_NV: break;
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO:
            external_info = (VkExternalMemoryBufferCreateInfo *)*next;
            if (!(external_info->handleTypes & EXTERNAL_MEMORY_WIN32_BITS))
                FIXME( "Unsupported handle types %#x\n", external_info->handleTypes );
            else
                external_info->handleTypes = get_host_external_memory_type();
            break;
        case VK_STRUCTURE_TYPE_OPAQUE_CAPTURE_DESCRIPTOR_DATA_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    if (physical_device->external_memory_align && !external_info)
    {
        host_external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        host_external_info.pNext = create_info->pNext;
        host_external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        ((VkBufferCreateInfo *)create_info)->pNext = &host_external_info; /* cast away const, it has been copied in the thunks */
    }

    return device->p_vkCreateBuffer( device->host.device, create_info, NULL, buffer );
}

static void win32u_vkGetDeviceBufferMemoryRequirements( VkDevice client_device, const VkDeviceBufferMemoryRequirements *buffer_requirements,
                                                        VkMemoryRequirements2 *memory_requirements )
{
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)buffer_requirements->pCreateInfo; /* cast away const, chain has been copied in the thunks */
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkExternalMemoryBufferCreateInfo *external_info;

    TRACE( "device %p, buffer_requirements %p, memory_requirements %p\n", device, buffer_requirements, memory_requirements );

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_BUFFER_CREATE_INFO_NV: break;
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO:
            external_info = (VkExternalMemoryBufferCreateInfo *)*next;
            if (!(external_info->handleTypes & EXTERNAL_MEMORY_WIN32_BITS))
                FIXME( "Unsupported handle types %#x\n", external_info->handleTypes );
            else
                external_info->handleTypes = get_host_external_memory_type();
            break;
        case VK_STRUCTURE_TYPE_OPAQUE_CAPTURE_DESCRIPTOR_DATA_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    device->p_vkGetDeviceBufferMemoryRequirements( device->host.device, buffer_requirements, memory_requirements );
}

static void get_physical_device_external_buffer_properties( struct vulkan_physical_device *physical_device, const VkPhysicalDeviceExternalBufferInfo *client_buffer_info,
                                                            VkExternalBufferProperties *buffer_properties, PFN_vkGetPhysicalDeviceExternalBufferProperties p_vkGetPhysicalDeviceExternalBufferProperties )
{
    VkPhysicalDeviceExternalBufferInfo *buffer_info = (VkPhysicalDeviceExternalBufferInfo *)client_buffer_info; /* cast away const, it has been copied in the thunks */
    VkExternalMemoryHandleTypeFlagBits handle_type = 0;

    handle_type = buffer_info->handleType;
    if (handle_type & EXTERNAL_MEMORY_WIN32_BITS) buffer_info->handleType = get_host_external_memory_type();

    p_vkGetPhysicalDeviceExternalBufferProperties( physical_device->host.physical_device, buffer_info, buffer_properties );
    buffer_properties->externalMemoryProperties.compatibleHandleTypes = handle_type;
    buffer_properties->externalMemoryProperties.exportFromImportedHandleTypes = handle_type;
}

static void win32u_vkGetPhysicalDeviceExternalBufferProperties( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalBufferInfo *buffer_info,
                                                                VkExternalBufferProperties *buffer_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, buffer_info %p, buffer_properties %p\n", physical_device, buffer_info, buffer_properties );

    get_physical_device_external_buffer_properties( physical_device, buffer_info, buffer_properties, instance->p_vkGetPhysicalDeviceExternalBufferProperties );
}

static void win32u_vkGetPhysicalDeviceExternalBufferPropertiesKHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalBufferInfo *buffer_info,
                                                                   VkExternalBufferProperties *buffer_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, buffer_info %p, buffer_properties %p\n", physical_device, buffer_info, buffer_properties );

    get_physical_device_external_buffer_properties( physical_device, buffer_info, buffer_properties, instance->p_vkGetPhysicalDeviceExternalBufferPropertiesKHR );
}

static VkResult win32u_vkCreateImage( VkDevice client_device, const VkImageCreateInfo *create_info,
                                      const VkAllocationCallbacks *allocator, VkImage *image )
{
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)create_info; /* cast away const, chain has been copied in the thunks */
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    VkExternalMemoryImageCreateInfo host_external_info, *external_info = NULL;

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_IMAGE_CREATE_INFO_NV: break;
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO:
            external_info = (VkExternalMemoryImageCreateInfo *)*next;
            if (!(external_info->handleTypes & EXTERNAL_MEMORY_WIN32_BITS))
                FIXME( "Unsupported handle types %#x\n", external_info->handleTypes );
            else
                external_info->handleTypes = get_host_external_memory_type();
            break;
        case VK_STRUCTURE_TYPE_IMAGE_ALIGNMENT_CONTROL_CREATE_INFO_MESA: break;
        case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT: break;
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR: break;
        case VK_STRUCTURE_TYPE_OPAQUE_CAPTURE_DESCRIPTOR_DATA_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV: break;
        case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    if (physical_device->external_memory_align && !external_info)
    {
        host_external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        host_external_info.pNext = create_info->pNext;
        host_external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        ((VkImageCreateInfo *)create_info)->pNext = &host_external_info; /* cast away const, it has been copied in the thunks */
    }

    return device->p_vkCreateImage( device->host.device, create_info, NULL, image );
}

static void win32u_vkGetDeviceImageMemoryRequirements( VkDevice client_device, const VkDeviceImageMemoryRequirements *image_requirements,
                                                       VkMemoryRequirements2 *memory_requirements )
{
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)image_requirements->pCreateInfo; /* cast away const, chain has been copied in the thunks */
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkExternalMemoryImageCreateInfo *external_info;

    TRACE( "device %p, image_requirements %p, memory_requirements %p\n", device, image_requirements, memory_requirements );

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_IMAGE_CREATE_INFO_NV: break;
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO:
            external_info = (VkExternalMemoryImageCreateInfo *)*next;
            if (!(external_info->handleTypes & EXTERNAL_MEMORY_WIN32_BITS))
                FIXME( "Unsupported handle types %#x\n", external_info->handleTypes );
            else
                external_info->handleTypes = get_host_external_memory_type();
            break;
        case VK_STRUCTURE_TYPE_IMAGE_ALIGNMENT_CONTROL_CREATE_INFO_MESA: break;
        case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT: break;
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR: break;
        case VK_STRUCTURE_TYPE_OPAQUE_CAPTURE_DESCRIPTOR_DATA_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV: break;
        case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    device->p_vkGetDeviceImageMemoryRequirements( device->host.device, image_requirements, memory_requirements );
}

static VkResult get_physical_device_image_format_properties( struct vulkan_physical_device *physical_device, const VkPhysicalDeviceImageFormatInfo2 *format_info,
                                                             VkImageFormatProperties2 *format_properties, PFN_vkGetPhysicalDeviceImageFormatProperties2 p_vkGetPhysicalDeviceImageFormatProperties2 )
{
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)format_info; /* cast away const, chain has been copied in the thunks */
    VkExternalMemoryHandleTypeFlagBits handle_type = 0;
    VkResult res;

    TRACE( "physical_device %p, format_info %p, format_properties %p\n", physical_device, format_info, format_properties );

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT: break;
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV: break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO:
        {
            VkPhysicalDeviceExternalImageFormatInfo *external_info = (VkPhysicalDeviceExternalImageFormatInfo *)*next;
            handle_type = external_info->handleType;
            if (handle_type & EXTERNAL_MEMORY_WIN32_BITS) external_info->handleType = get_host_external_memory_type();
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_IMAGE_FORMAT_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    res = p_vkGetPhysicalDeviceImageFormatProperties2( physical_device->host.physical_device, format_info, format_properties );
    for (prev = (VkBaseOutStructure *)format_properties, next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES:
        {
            VkExternalImageFormatProperties *props = (VkExternalImageFormatProperties *)*next;
            props->externalMemoryProperties.compatibleHandleTypes = handle_type;
            props->externalMemoryProperties.exportFromImportedHandleTypes = handle_type;
            break;
        }
        case VK_STRUCTURE_TYPE_FILTER_CUBIC_IMAGE_VIEW_IMAGE_FORMAT_PROPERTIES_EXT: break;
        case VK_STRUCTURE_TYPE_HOST_IMAGE_COPY_DEVICE_PERFORMANCE_QUERY: break;
        case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_PROPERTIES_EXT: break;
        case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES: break;
        case VK_STRUCTURE_TYPE_TEXTURE_LOD_GATHER_FORMAT_PROPERTIES_AMD: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    return res;
}

static VkResult win32u_vkGetPhysicalDeviceImageFormatProperties2( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceImageFormatInfo2 *format_info,
                                                                  VkImageFormatProperties2 *format_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, format_info %p, format_properties %p\n", physical_device, format_info, format_properties );

    return get_physical_device_image_format_properties( physical_device, format_info, format_properties, instance->p_vkGetPhysicalDeviceImageFormatProperties2 );
}

static VkResult win32u_vkGetPhysicalDeviceImageFormatProperties2KHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceImageFormatInfo2 *format_info,
                                                                     VkImageFormatProperties2 *format_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, format_info %p, format_properties %p\n", physical_device, format_info, format_properties );

    return get_physical_device_image_format_properties( physical_device, format_info, format_properties, instance->p_vkGetPhysicalDeviceImageFormatProperties2KHR );
}

static VkResult win32u_vkCreateWin32SurfaceKHR( VkInstance client_instance, const VkWin32SurfaceCreateInfoKHR *create_info,
                                                const VkAllocationCallbacks *allocator, VkSurfaceKHR *ret )
{
    struct vulkan_instance *instance = vulkan_instance_from_handle( client_instance );
    VkSurfaceKHR host_surface;
    struct surface *surface;
    HWND dummy = NULL;
    VkResult res;

    TRACE( "client_instance %p, create_info %p, allocator %p, ret %p\n", client_instance, create_info, allocator, ret );
    if (allocator) FIXME( "Support for allocation callbacks not implemented yet\n" );

    if (!(surface = calloc( 1, sizeof(*surface) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;

    /* Windows allows surfaces to be created with no HWND, they return VK_ERROR_SURFACE_LOST_KHR later */
    if (!(surface->hwnd = create_info->hwnd))
    {
        static const WCHAR staticW[] = {'s','t','a','t','i','c',0};
        UNICODE_STRING static_us = RTL_CONSTANT_STRING( staticW );
        dummy = NtUserCreateWindowEx( 0, &static_us, NULL, &static_us, WS_POPUP, 0, 0, 0, 0,
                                      NULL, NULL, NULL, NULL, 0, NULL, NULL, FALSE );
        WARN( "Created dummy window %p for null surface window\n", dummy );
        surface->hwnd = dummy;
    }

    if (!(surface->client = get_unused_client_surface( surface->hwnd, 0, FALSE ))) res = VK_ERROR_OUT_OF_HOST_MEMORY;
    else
    {
        res = driver_funcs->p_vulkan_surface_create( surface->client, instance, &host_surface );
        use_window_client_surface( surface->client, !res );
    }
    if (res)
    {
        if (surface->client) client_surface_release( surface->client );
        if (dummy) NtUserDestroyWindow( dummy );
        free( surface );
        return res;
    }
    set_window_pixel_format( surface->hwnd, -1, TRUE );

    vulkan_object_init( &surface->obj.obj, host_surface );
    surface->obj.instance = instance;
    surface->refs = 1;
    instance->p_insert_object( instance, &surface->obj.obj );

    if (dummy) NtUserDestroyWindow( dummy );

    *ret = surface->obj.client.surface;
    return VK_SUCCESS;
}

static void release_surface( struct surface *surface )
{
    struct vulkan_instance *instance = surface->obj.instance;

    if (InterlockedDecrement( &surface->refs )) return;
    instance->p_vkDestroySurfaceKHR( instance->host.instance, surface->obj.host.surface, NULL );
    client_surface_release( surface->client );
    free( surface );
}

static void win32u_vkDestroySurfaceKHR( VkInstance client_instance, VkSurfaceKHR client_surface,
                                        const VkAllocationCallbacks *allocator )
{
    struct vulkan_instance *instance = vulkan_instance_from_handle( client_instance );
    struct surface *surface = surface_from_handle( client_surface );

    if (!surface) return;

    TRACE( "instance %p, handle 0x%s, allocator %p\n", instance, wine_dbgstr_longlong( client_surface ), allocator );
    if (allocator) FIXME( "Support for allocation callbacks not implemented yet\n" );

    use_window_client_surface( surface->client, FALSE );
    instance->p_remove_object( instance, &surface->obj.obj );
    release_surface( surface );
}

static BOOL get_surface_rect( HWND hwnd, RECT *rect, struct ratio dpi )
{
    if (!get_present_rect( hwnd, rect, dpi ) && !get_client_rect( hwnd, rect, dpi )) return FALSE;
    OffsetRect( rect, -rect->left, -rect->top );
    return TRUE;
}

static VkResult get_vulkan_surface_source( struct surface *surface, VkFormat format,
                                           struct vulkan_surface_source *source )
{
    *source = (struct vulkan_surface_source){VULKAN_SURFACE_SOURCE_NATIVE};
    if (!driver_funcs->p_vulkan_surface_get_source) return VK_SUCCESS;
    return driver_funcs->p_vulkan_surface_get_source( surface->client, format, source );
}

static void adjust_surface_capabilities( struct vulkan_instance *instance, struct surface *surface,
                                         VkSurfaceCapabilitiesKHR *capabilities )
{
    struct vulkan_surface_source source;
    RECT client_rect;

    if (!get_vulkan_surface_source( surface, VK_FORMAT_UNDEFINED, &source ) &&
        source.type == VULKAN_SURFACE_SOURCE_READBACK)
        capabilities->maxImageArrayLayers = 1;

    /* Many Windows games, for example Strange Brigade, No Man's Sky, Path of Exile
     * and World War Z, do not expect that maxImageCount can be set to 0.
     * A value of 0 means that there is no limit on the number of images.
     * Nvidia reports 8 on Windows, AMD 16.
     * https://vulkan.gpuinfo.org/displayreport.php?id=9122#surface
     * https://vulkan.gpuinfo.org/displayreport.php?id=9121#surface
     */
    if (!capabilities->maxImageCount) capabilities->maxImageCount = max( capabilities->minImageCount, 16 );

    /* Update the image extents to match what the Win32 WSI would provide. */
    /* FIXME: handle DPI scaling, somehow */
    get_surface_rect( surface->hwnd, &client_rect, get_dpi_for_window( surface->hwnd ) );
    capabilities->minImageExtent.width = client_rect.right - client_rect.left;
    capabilities->minImageExtent.height = client_rect.bottom - client_rect.top;
    capabilities->maxImageExtent.width = client_rect.right - client_rect.left;
    capabilities->maxImageExtent.height = client_rect.bottom - client_rect.top;
    capabilities->currentExtent.width = client_rect.right - client_rect.left;
    capabilities->currentExtent.height = client_rect.bottom - client_rect.top;
}

static VkResult win32u_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( VkPhysicalDevice client_physical_device, VkSurfaceKHR client_surface,
                                                                  VkSurfaceCapabilitiesKHR *capabilities )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct surface *surface = surface_from_handle( client_surface );
    struct vulkan_instance *instance = physical_device->instance;
    VkResult res;

    if (!NtUserIsWindow( surface->hwnd )) return VK_ERROR_SURFACE_LOST_KHR;
    res = instance->p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( physical_device->host.physical_device,
                                                       surface->obj.host.surface, capabilities );
    if (!res) adjust_surface_capabilities( instance, surface, capabilities );
    return res;
}

static VkResult win32u_vkGetPhysicalDeviceSurfaceCapabilities2KHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceSurfaceInfo2KHR *surface_info,
                                                                   VkSurfaceCapabilities2KHR *capabilities )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct surface *surface = surface_from_handle( surface_info->surface );
    VkPhysicalDeviceSurfaceInfo2KHR surface_info_host = *surface_info;
    struct vulkan_instance *instance = physical_device->instance;
    VkResult res;

    if (!instance->p_vkGetPhysicalDeviceSurfaceCapabilities2KHR)
    {
        /* Until the loader version exporting this function is common, emulate it using the older non-2 version. */
        if (surface_info->pNext || capabilities->pNext) FIXME( "Emulating vkGetPhysicalDeviceSurfaceCapabilities2KHR, ignoring pNext.\n" );
        return win32u_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( client_physical_device, surface_info->surface,
                                                                 &capabilities->surfaceCapabilities );
    }

    surface_info_host.surface = surface->obj.host.surface;

    if (!NtUserIsWindow( surface->hwnd )) return VK_ERROR_SURFACE_LOST_KHR;
    res = instance->p_vkGetPhysicalDeviceSurfaceCapabilities2KHR( physical_device->host.physical_device,
                                                                     &surface_info_host, capabilities );
    if (!res) adjust_surface_capabilities( instance, surface, &capabilities->surfaceCapabilities );
    return res;
}

static VkResult win32u_vkGetPhysicalDevicePresentRectanglesKHR( VkPhysicalDevice client_physical_device, VkSurfaceKHR client_surface,
                                                                uint32_t *rect_count, VkRect2D *rects )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct surface *surface = surface_from_handle( client_surface );
    struct vulkan_instance *instance = physical_device->instance;

    if (!NtUserIsWindow( surface->hwnd ))
    {
        if (rects && !*rect_count) return VK_INCOMPLETE;
        if (rects) memset( rects, 0, sizeof(VkRect2D) );
        *rect_count = 1;
        return VK_SUCCESS;
    }

    return instance->p_vkGetPhysicalDevicePresentRectanglesKHR( physical_device->host.physical_device,
                                                                   surface->obj.host.surface, rect_count, rects );
}

static void *find_vk_struct( void *s, VkStructureType t )
{
    VkBaseOutStructure *header;

    for (header = s; header; header = header->pNext)
    {
        if (header->sType == t) return header;
    }

    return NULL;
}

static void get_physical_device_properties2( struct vulkan_physical_device *physical_device, VkPhysicalDeviceProperties2 *properties2,
                                             PFN_vkGetPhysicalDeviceProperties2 p_vkGetPhysicalDeviceProperties2 )
{
    VkPhysicalDeviceIDProperties id_host = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 properties2_host;
    VkPhysicalDeviceVulkan11Properties *vk11;
    VkPhysicalDeviceIDProperties *id;
    VkBool32 device_luid_valid;
    UINT32 node_mask = 0;
    const GUID *uuid;
    LUID luid;

    vk11 = find_vk_struct( properties2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES );
    id = find_vk_struct( properties2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES );

    if (!vk11 && !id)
    {
        properties2_host = *properties2;
        id_host.pNext = properties2->pNext;
        properties2_host.pNext = &id_host;
        p_vkGetPhysicalDeviceProperties2( physical_device->host.physical_device, &properties2_host );
        properties2->properties = properties2_host.properties;
    }
    else p_vkGetPhysicalDeviceProperties2( physical_device->host.physical_device, properties2 );

    if (id)        uuid = (const GUID *)id->deviceUUID;
    else if (vk11) uuid = (const GUID *)vk11->deviceUUID;
    else           uuid = (const GUID *)id_host.deviceUUID;

    device_luid_valid = get_gpu_info_from_uuid( uuid, &luid, &node_mask, properties2->properties.deviceName );
    if (!device_luid_valid) WARN( "luid for %s not found\n", debugstr_guid(uuid) );

    if (id)
    {
        if (device_luid_valid) memcpy( &id->deviceLUID, &luid, sizeof(id->deviceLUID) );
        id->deviceLUIDValid = device_luid_valid;
        id->deviceNodeMask = node_mask;
    }

    if (vk11)
    {
        if (device_luid_valid) memcpy( &vk11->deviceLUID, &luid, sizeof(vk11->deviceLUID) );
        vk11->deviceLUIDValid = device_luid_valid;
        vk11->deviceNodeMask = node_mask;
    }

    TRACE( "deviceName:%s deviceLUIDValid:%d LUID:%08x:%08x.\n",
           properties2->properties.deviceName, device_luid_valid, luid.HighPart, luid.LowPart );
}

static void win32u_vkGetPhysicalDeviceProperties( VkPhysicalDevice client_physical_device, VkPhysicalDeviceProperties *properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    VkPhysicalDeviceProperties2 properties2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };

    if (!physical_device->instance->extensions.has_VK_KHR_get_physical_device_properties2)
    {
        physical_device->instance->p_vkGetPhysicalDeviceProperties( physical_device->host.physical_device, properties );
        return;
    }
    get_physical_device_properties2( physical_device, &properties2,
                                     physical_device->instance->p_vkGetPhysicalDeviceProperties2KHR );
    *properties = properties2.properties;
}

static void win32u_vkGetPhysicalDeviceProperties2( VkPhysicalDevice client_physical_device, VkPhysicalDeviceProperties2 *properties2 )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );

    get_physical_device_properties2( physical_device, properties2, physical_device->instance->p_vkGetPhysicalDeviceProperties2 );
}

static void win32u_vkGetPhysicalDeviceProperties2KHR( VkPhysicalDevice client_physical_device, VkPhysicalDeviceProperties2 *properties2 )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );

    get_physical_device_properties2( physical_device, properties2, physical_device->instance->p_vkGetPhysicalDeviceProperties2KHR );
}

static VkResult get_vulkan_surface_formats( struct vulkan_physical_device *physical_device, struct surface *surface,
                                            const VkPhysicalDeviceSurfaceInfo2KHR *info, uint32_t *count,
                                            VkSurfaceFormatKHR *formats, VkSurfaceFormat2KHR *formats2 )
{
    struct vulkan_instance *instance = physical_device->instance;
    VkSurfaceFormatKHR *host_formats = NULL;
    VkSurfaceFormat2KHR *host_formats2 = NULL;
    VkImageCompressionPropertiesEXT *compression = NULL, *out;
    struct vulkan_surface_source source;
    uint32_t host_count, capacity = formats || formats2 ? *count : 0, written = 0, supported = 0, i;
    BOOL want_compression = FALSE;
    VkResult res;

    if ((res = get_vulkan_surface_source( surface, VK_FORMAT_UNDEFINED, &source ))) return res;
    if (source.type == VULKAN_SURFACE_SOURCE_NATIVE)
    {
        if (info) return instance->p_vkGetPhysicalDeviceSurfaceFormats2KHR( physical_device->host.physical_device,
                                                                          info, count, formats2 );
        return instance->p_vkGetPhysicalDeviceSurfaceFormatsKHR( physical_device->host.physical_device,
                                                                  surface->obj.host.surface, count, formats );
    }

    if (formats2) for (i = 0; i < capacity; ++i)
        want_compression |= !!find_next_struct( formats2[i].pNext, VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_PROPERTIES_EXT );

    /* Count and output queries use the same backend source contract. Preserve
     * native order and per-format extension outputs when compacting the list. */
    for (;;)
    {
        if (info) res = instance->p_vkGetPhysicalDeviceSurfaceFormats2KHR( physical_device->host.physical_device,
                                                                          info, &host_count, NULL );
        else res = instance->p_vkGetPhysicalDeviceSurfaceFormatsKHR( physical_device->host.physical_device,
                                                                    surface->obj.host.surface, &host_count, NULL );
        if (res) return res;
        if (!host_count) break;
        if (info)
        {
            if (!(host_formats2 = calloc( host_count, sizeof(*host_formats2) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
            if (want_compression && !(compression = calloc( host_count, sizeof(*compression) )))
            {
                free( host_formats2 );
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            for (i = 0; i < host_count; ++i)
            {
                host_formats2[i].sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR;
                if (!compression) continue;
                compression[i].sType = VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_PROPERTIES_EXT;
                host_formats2[i].pNext = &compression[i];
            }
            res = instance->p_vkGetPhysicalDeviceSurfaceFormats2KHR( physical_device->host.physical_device,
                                                                      info, &host_count, host_formats2 );
        }
        else
        {
            if (!(host_formats = calloc( host_count, sizeof(*host_formats) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
            res = instance->p_vkGetPhysicalDeviceSurfaceFormatsKHR( physical_device->host.physical_device,
                                                                    surface->obj.host.surface, &host_count, host_formats );
        }
        if (res != VK_INCOMPLETE) break;
        free( host_formats );
        free( host_formats2 );
        free( compression );
        host_formats = NULL;
        host_formats2 = NULL;
        compression = NULL;
    }

    if (!res) for (i = 0; i < host_count; ++i)
    {
        VkSurfaceFormatKHR format = info ? host_formats2[i].surfaceFormat : host_formats[i];

        if (get_vulkan_surface_source( surface, format.format, &source )) continue;
        ++supported;
        if ((!formats && !formats2) || written == capacity) continue;
        if (formats) formats[written] = format;
        else
        {
            formats2[written].surfaceFormat = format;
            out = find_vk_struct( formats2[written].pNext, VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_PROPERTIES_EXT );
            if (out && compression)
            {
                out->imageCompressionFlags = compression[i].imageCompressionFlags;
                out->imageCompressionFixedRateFlags = compression[i].imageCompressionFixedRateFlags;
            }
        }
        ++written;
    }
    free( host_formats );
    free( host_formats2 );
    free( compression );
    if (res) return res;
    *count = formats || formats2 ? written : supported;
    return (formats || formats2) && written < supported ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult win32u_vkGetPhysicalDeviceSurfaceFormatsKHR( VkPhysicalDevice client_physical_device, VkSurfaceKHR client_surface,
                                                             uint32_t *format_count, VkSurfaceFormatKHR *formats )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct surface *surface = surface_from_handle( client_surface );
    return get_vulkan_surface_formats( physical_device, surface, NULL, format_count, formats, NULL );
}

static VkResult win32u_vkGetPhysicalDeviceSurfaceFormats2KHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceSurfaceInfo2KHR *surface_info,
                                                              uint32_t *format_count, VkSurfaceFormat2KHR *formats )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct surface *surface = surface_from_handle( surface_info->surface );
    VkPhysicalDeviceSurfaceInfo2KHR surface_info_host = *surface_info;
    struct vulkan_instance *instance = physical_device->instance;
    VkResult res;

    if (!instance->p_vkGetPhysicalDeviceSurfaceFormats2KHR)
    {
        VkSurfaceFormatKHR *surface_formats;
        UINT i;

        /* Until the loader version exporting this function is common, emulate it using the older non-2 version. */
        if (surface_info->pNext) FIXME( "Emulating vkGetPhysicalDeviceSurfaceFormats2KHR, ignoring pNext.\n" );
        if (!formats) return win32u_vkGetPhysicalDeviceSurfaceFormatsKHR( client_physical_device, surface_info->surface, format_count, NULL );

        surface_formats = calloc( max( *format_count, 1 ), sizeof(*surface_formats) );
        if (!surface_formats) return VK_ERROR_OUT_OF_HOST_MEMORY;

        res = win32u_vkGetPhysicalDeviceSurfaceFormatsKHR( client_physical_device, surface_info->surface, format_count, surface_formats );
        if (!res || res == VK_INCOMPLETE) for (i = 0; i < *format_count; i++) formats[i].surfaceFormat = surface_formats[i];

        free( surface_formats );
        return res;
    }

    surface_info_host.surface = surface->obj.host.surface;

    return get_vulkan_surface_formats( physical_device, surface, &surface_info_host, format_count, NULL, formats );
}

static VkBool32 win32u_vkGetPhysicalDeviceWin32PresentationSupportKHR( VkPhysicalDevice client_physical_device, uint32_t queue )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    return driver_funcs->p_get_physical_device_presentation_support( physical_device, queue );
}

static VkResult get_vulkan_source_capabilities( struct vulkan_physical_device *physical_device,
                                               struct surface *surface, VkSurfaceCapabilitiesKHR *capabilities )
{
    struct vulkan_instance *instance = physical_device->instance;
    struct wine_vk_surface_source_caps source = {WINE_VK_SURFACE_SOURCE_CAPS, NULL,
                                                 WINE_VK_SOURCE_ABI_VERSION, VK_FALSE};
    VkSurfaceCapabilities2KHR caps = {VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, &source};
    VkPhysicalDeviceSurfaceInfo2KHR info = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
                                           NULL, surface->obj.host.surface};
    VkResult res;

    if (!instance->p_vkGetPhysicalDeviceSurfaceCapabilities2KHR) return VK_ERROR_FEATURE_NOT_PRESENT;
    res = instance->p_vkGetPhysicalDeviceSurfaceCapabilities2KHR( physical_device->host.physical_device,
                                                                &info, &caps );
    if (res) return res;
    if (!source.supported || !(caps.surfaceCapabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    *capabilities = caps.surfaceCapabilities;
    return VK_SUCCESS;
}

static VkResult win32u_vkGetPhysicalDeviceSurfaceSupportKHR( VkPhysicalDevice client_physical_device,
                                                            uint32_t queue, VkSurfaceKHR client_surface,
                                                            VkBool32 *supported )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;
    struct surface *surface = surface_from_handle( client_surface );
    struct vulkan_surface_source source;
    VkSurfaceCapabilitiesKHR capabilities;
    uint32_t format_count;
    VkResult res;

    if ((res = get_vulkan_surface_source( surface, VK_FORMAT_UNDEFINED, &source ))) return res;
    res = instance->p_vkGetPhysicalDeviceSurfaceSupportKHR( physical_device->host.physical_device,
                                                           queue, surface->obj.host.surface, supported );
    if (!res && *supported && source.type == VULKAN_SURFACE_SOURCE_READBACK)
    {
        res = get_vulkan_source_capabilities( physical_device, surface, &capabilities );
        if (res == VK_ERROR_FEATURE_NOT_PRESENT)
        {
            *supported = VK_FALSE;
            res = VK_SUCCESS;
        }
        else if (!res)
        {
            res = get_vulkan_surface_formats( physical_device, surface, NULL, &format_count, NULL, NULL );
            if (!res && !format_count) *supported = VK_FALSE;
        }
    }
    return res;
}

static BOOL extents_equals( const VkExtent2D *extents, const RECT *rect )
{
    return extents->width == rect->right - rect->left && extents->height == rect->bottom - rect->top;
}

static int compare_swapchain_ptrs( const void *left, const void *right )
{
    UINT_PTR a = (UINT_PTR)*(const struct swapchain * const *)left;
    UINT_PTR b = (UINT_PTR)*(const struct swapchain * const *)right;

    return (a > b) - (a < b);
}

static int compare_client_surface_ptrs( const void *left, const void *right )
{
    UINT_PTR a = (UINT_PTR)*(const struct client_surface * const *)left;
    UINT_PTR b = (UINT_PTR)*(const struct client_surface * const *)right;

    return (a > b) - (a < b);
}

static void release_snapshot_fence( struct vulkan_snapshot_fence *pending )
{
    if (!pending || InterlockedDecrement( &pending->refs )) return;
    pending->device->p_vkDestroyFence( pending->device->host.device, pending->fence, NULL );
    client_surface_free_metadata( pending, sizeof(*pending) );
}

static void release_snapshot_reservation( struct swapchain *swapchain, struct swapchain_snapshot *snapshot )
{
    pthread_mutex_lock( &swapchain->present_lock );
    assert( snapshot->busy && swapchain->completion_refs );
    snapshot->busy = FALSE;
    --swapchain->completion_refs;
    pthread_cond_broadcast( &swapchain->completion_cond );
    pthread_mutex_unlock( &swapchain->present_lock );
}

static VkResult acquire_snapshot_reservation( struct vulkan_device *device, struct swapchain *swapchain,
                                              struct swapchain_snapshot **ret )
{
    struct swapchain_snapshot *snapshot = NULL;
    DWORD start = NtGetTickCount();
    unsigned int i;
    VkResult res;

    /* Reserve storage before acquiring any surface submission mutex. The
     * previous completion owns this storage through its CPU upload, but it
     * must not prevent window geometry and unrelated surfaces from moving. */
    pthread_mutex_lock( &swapchain->present_lock );
    for (;;)
    {
        DWORD elapsed, remaining;

        for (i = 0; i < ARRAY_SIZE(swapchain->snapshots); ++i)
        {
            unsigned int index = (swapchain->next_snapshot + i) % ARRAY_SIZE(swapchain->snapshots);

            if (swapchain->snapshots[index].busy) continue;
            snapshot = &swapchain->snapshots[index];
            swapchain->next_snapshot = (index + 1) % ARRAY_SIZE(swapchain->snapshots);
            break;
        }
        if (snapshot) break;
        elapsed = NtGetTickCount() - start;
        remaining = elapsed < CLIENT_SURFACE_PRESENT_TIMEOUT ? CLIENT_SURFACE_PRESENT_TIMEOUT - elapsed : 0;
        if (!remaining || client_surface_cond_timedwait( &swapchain->completion_cond, &swapchain->present_lock,
                                                        remaining ) == ETIMEDOUT)
        {
            pthread_mutex_unlock( &swapchain->present_lock );
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
    }
    snapshot->busy = TRUE;
    ++swapchain->completion_refs;
    pthread_mutex_unlock( &swapchain->present_lock );

    /* A stale or timed-out completion may have returned without reading the
     * GPU result. Its storage remains quarantined until the submission fence
     * completes; neither the buffer nor command pool may be reset earlier. */
    if (snapshot->pending)
    {
        DWORD elapsed = NtGetTickCount() - start;
        DWORD remaining = elapsed < CLIENT_SURFACE_PRESENT_TIMEOUT ? CLIENT_SURFACE_PRESENT_TIMEOUT - elapsed : 0;

        res = device->p_vkWaitForFences( device->host.device, 1, &snapshot->pending->fence,
                                        VK_TRUE, (uint64_t)remaining * 1000000 );
        if (res)
        {
            release_snapshot_reservation( swapchain, snapshot );
            /* The bounded staging allocation is still occupied. Keep it
             * alive, including on timeout, rather than recycling GPU work. */
            return res == VK_TIMEOUT ? VK_ERROR_OUT_OF_DEVICE_MEMORY : res;
        }
    }
    *ret = snapshot;
    return VK_SUCCESS;
}

static struct client_surface_completion_result wait_vulkan_snapshot( void *context, DWORD timeout )
{
    struct vulkan_snapshot_fence *pending = context;
    struct vulkan_device *device = pending->device;
    VkResult res;

    res = device->p_vkWaitForFences( device->host.device, 1, &pending->fence,
                                   VK_TRUE, (uint64_t)timeout * 1000000 );
    if (res == VK_SUCCESS) return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_SIGNALED );
    if (res == VK_TIMEOUT) return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_PENDING );
    return client_surface_completion_result( CLIENT_SURFACE_COMPLETION_FAILED );
}

static void release_vulkan_snapshot_completion( void *context )
{
    release_snapshot_fence( context );
}

static BOOL read_vulkan_snapshot( void *context )
{
    struct vulkan_snapshot_capture *capture = context;
    struct vulkan_device *device = capture->device;
    struct swapchain_snapshot *snapshot = capture->snapshot;
    struct swapchain *swapchain = capture->swapchain;
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                 .memory = snapshot->memory, .size = VK_WHOLE_SIZE};

    return !device->p_vkInvalidateMappedMemoryRanges( device->host.device, 1, &range ) &&
           driver_funcs->p_vulkan_surface_read_snapshot( &snapshot->backend_snapshot, snapshot->pixels,
               swapchain->host_extents.width, swapchain->host_extents.height, swapchain->format );
}

static BOOL apply_vulkan_snapshot( void *context, struct client_surface *surface,
                                   struct client_surface_frame *present )
{
    struct vulkan_snapshot_capture *capture = context;

    return driver_funcs->p_vulkan_surface_apply_snapshot( surface, present, &capture->snapshot->backend_snapshot );
}

static void release_vulkan_snapshot_capture( void *context )
{
    struct vulkan_snapshot_capture *capture = context;

    release_snapshot_reservation( capture->swapchain, capture->snapshot );
}

static void destroy_swapchain_snapshot( struct vulkan_device *device, struct swapchain_snapshot *snapshot )
{
    /* The retiring swapchain has checked completion of its source reads.
     * Partial initialization has not submitted any work. */
    if (snapshot->backend_snapshot)
        driver_funcs->p_vulkan_surface_destroy_snapshot( snapshot->backend_snapshot );
    if (snapshot->pending)
        release_snapshot_fence( snapshot->pending );
    if (snapshot->pool) device->p_vkDestroyCommandPool( device->host.device, snapshot->pool, NULL );
    if (snapshot->pixels) device->p_vkUnmapMemory( device->host.device, snapshot->memory );
    if (snapshot->buffer) device->p_vkDestroyBuffer( device->host.device, snapshot->buffer, NULL );
    if (snapshot->memory) device->p_vkFreeMemory( device->host.device, snapshot->memory, NULL );
    client_surface_release_memory( CLIENT_SURFACE_MEMORY_STAGING, snapshot->memory_bytes );
    client_surface_free_metadata( snapshot->images, snapshot->images_bytes );
    client_surface_free_metadata( snapshot->capture, sizeof(*snapshot->capture) );
    memset( snapshot, 0, sizeof(*snapshot) );
}

static VkResult prepare_swapchain_snapshot( struct vulkan_queue *queue, struct swapchain *swapchain,
                                           struct swapchain_snapshot *snapshot )
{
    struct vulkan_device *device = queue->device;
    const VkPhysicalDeviceMemoryProperties *properties = &device->physical_device->memory_properties;
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                        .queueFamilyIndex = queue->info.queueFamilyIndex};
    VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                               .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                     .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkMemoryAllocateInfo memory_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkMemoryRequirements requirements;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkCommandPool pool;
    VkCommandBuffer command;
    uint32_t image_count;
    void *pixels;
    BOOL fresh = !snapshot->buffer;
    unsigned int i;
    VkResult res;

    if (fresh)
    {
        if ((res = device->p_vkGetSwapchainImagesKHR( device->host.device, swapchain->obj.host.swapchain,
                                                     &image_count, NULL ))) goto failed;
        snapshot->image_count = image_count;
        if (!(snapshot->images = client_surface_alloc_metadata( image_count, sizeof(*snapshot->images) )))
        {
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto failed;
        }
        snapshot->images_bytes = (SIZE_T)image_count * sizeof(*snapshot->images);
        if ((res = device->p_vkGetSwapchainImagesKHR( device->host.device, swapchain->obj.host.swapchain,
                                                     &image_count, snapshot->images ))) goto failed;
        snapshot->image_count = image_count;
        buffer_info.size = (VkDeviceSize)swapchain->host_extents.width * swapchain->host_extents.height *
                           swapchain->source.texel_size;
        /* Runtime errors leave output parameters undefined. Commit ownership
         * only after success, so cleanup never consumes an unsuccessful output. */
        if ((res = device->p_vkCreateBuffer( device->host.device, &buffer_info, NULL, &buffer ))) goto failed;
        snapshot->buffer = buffer;
        device->p_vkGetBufferMemoryRequirements( device->host.device, snapshot->buffer, &requirements );
        for (i = 0; i < properties->memoryTypeCount; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (properties->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) break;
        if (i == properties->memoryTypeCount)
        {
            res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
            goto failed;
        }
        memory_info.allocationSize = requirements.size;
        memory_info.memoryTypeIndex = i;
        if (!client_surface_reserve_memory( CLIENT_SURFACE_MEMORY_STAGING, requirements.size ))
        {
            res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
            goto failed;
        }
        snapshot->memory_bytes = requirements.size;
        if ((res = device->p_vkAllocateMemory( device->host.device, &memory_info, NULL, &memory ))) goto failed;
        snapshot->memory = memory;
        if ((res = device->p_vkBindBufferMemory( device->host.device, snapshot->buffer, snapshot->memory, 0 ))) goto failed;
        if ((res = device->p_vkMapMemory( device->host.device, snapshot->memory, 0, VK_WHOLE_SIZE,
                                         0, &pixels ))) goto failed;
        snapshot->pixels = pixels;
    }
    if (snapshot->pool && snapshot->queue_family != queue->info.queueFamilyIndex)
    {
        device->p_vkDestroyCommandPool( device->host.device, snapshot->pool, NULL );
        snapshot->pool = 0;
        snapshot->command = 0;
    }
    if (!snapshot->pool)
    {
        if ((res = device->p_vkCreateCommandPool( device->host.device, &pool_info, NULL, &pool ))) goto failed;
        snapshot->pool = pool;
        snapshot->queue_family = queue->info.queueFamilyIndex;
        command_info.commandPool = snapshot->pool;
        if ((res = device->p_vkAllocateCommandBuffers( device->host.device, &command_info, &command ))) goto failed;
        snapshot->command = command;
    }
    if ((res = device->p_vkResetCommandPool( device->host.device, snapshot->pool, 0 ))) goto failed;
    return VK_SUCCESS;

failed:
    /* Fresh storage has no submitted source read. Previously used storage
     * reached its source fence before admission and keeps its allocations. */
    if (fresh)
    {
        struct vulkan_snapshot_capture *capture = snapshot->capture;

        /* The unsubmitted reservation still owns its capture until the
         * batch cancels it, even when native storage preparation fails. */
        snapshot->capture = NULL;
        destroy_swapchain_snapshot( device, snapshot );
        snapshot->capture = capture;
        snapshot->busy = TRUE;
    }
    else if (snapshot->pool)
    {
        device->p_vkDestroyCommandPool( device->host.device, snapshot->pool, NULL );
        snapshot->pool = 0;
        snapshot->command = 0;
    }
    return res;
}

static struct vulkan_snapshot_fence *take_snapshot_fence( struct client_surface_frame *presents,
                                                          struct vulkan_present_reservation *reservations,
                                                          unsigned int count )
{
    struct vulkan_snapshot_fence *pending = NULL;
    unsigned int i;

    for (i = 0; i < count; ++i)
    {
        struct swapchain_snapshot *snapshot = reservations[i].snapshot;
        struct vulkan_snapshot_fence *previous;

        if (!presents[i].capture.apply || !(previous = snapshot->pending)) continue;
        /* Admission observed this fence and owns the image reservation.
         * Drop all selected images' old references before deciding whether
         * any callback or unselected swapchain still owns the candidate. */
        snapshot->pending = NULL;
        if (!pending) pending = previous;
        else if (previous != pending && InterlockedCompareExchange( &previous->refs, 0, 0 ) == 1 &&
                 InterlockedCompareExchange( &pending->refs, 0, 0 ) != 1)
        {
            release_snapshot_fence( pending );
            pending = previous;
        }
        else release_snapshot_fence( previous );
    }
    if (pending && InterlockedCompareExchange( &pending->refs, 0, 0 ) != 1)
    {
        release_snapshot_fence( pending );
        pending = NULL;
    }
    return pending;
}

static VkResult snapshot_vulkan_present( struct vulkan_queue *queue, VkPresentInfoKHR *present_info,
                                         const VkSwapchainKHR *client_swapchains,
                                         struct client_surface_frame *presents,
                                         struct vulkan_present_reservation *reservations )
{
    struct vulkan_device *device = queue->device;
    VkCommandBuffer commands_buffer[16], *commands = commands_buffer;
    VkBool32 submitted = VK_FALSE;
    struct wine_vk_present_source_info source = {WINE_VK_PRESENT_SOURCE_INFO, present_info->pNext,
                                                 WINE_VK_SOURCE_ABI_VERSION, 0, commands_buffer, 0, &submitted};
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    struct vulkan_snapshot_fence *pending = NULL;
    VkFence fence;
    unsigned int i, count = 0;
    VkResult res = VK_SUCCESS;

    if (present_info->swapchainCount > ARRAY_SIZE(commands_buffer) &&
        !(commands = client_surface_alloc_metadata( present_info->swapchainCount, sizeof(*commands) )))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    /* Admit every capture owner before allocating native snapshot storage.
     * Host metadata failure must not consume guest synchronization or leave
     * a freshly allocated image without its capture owner. */
    for (i = 0; i < present_info->swapchainCount; ++i)
    {
        struct swapchain *swapchain = swapchain_from_handle( client_swapchains[i] );
        struct swapchain_snapshot *snapshot = reservations[i].snapshot;
        struct vulkan_snapshot_capture *capture;

        if (!swapchain_needs_snapshot( swapchain ) || presents[i].completion.kind != CLIENT_SURFACE_COMPLETION_EXACT) continue;
        assert( snapshot && snapshot->busy );
        if (!(capture = snapshot->capture))
        {
            if (!(capture = client_surface_alloc_metadata( 1, sizeof(*capture) )))
            {
                res = VK_ERROR_OUT_OF_HOST_MEMORY;
                goto done;
            }
            capture->device = device;
            capture->swapchain = swapchain;
            capture->snapshot = snapshot;
            snapshot->capture = capture;
            reservations[i].new_capture = TRUE;
        }
        presents[i].capture.read = read_vulkan_snapshot;
        presents[i].capture.apply = apply_vulkan_snapshot;
        presents[i].capture.release = release_vulkan_snapshot_capture;
        presents[i].capture.context = capture;
    }
    if (!(pending = take_snapshot_fence( presents, reservations, present_info->swapchainCount )))
    {
        if (!(pending = client_surface_alloc_metadata( 1, sizeof(*pending) )))
        {
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto done;
        }
        pending->device = device;
        pending->refs = 1;
    }
    if (pending->fence)
    {
        if ((res = device->p_vkResetFences( device->host.device, 1, &pending->fence ))) goto done;
    }
    else
    {
        if ((res = device->p_vkCreateFence( device->host.device, &fence_info, NULL, &fence ))) goto done;
        pending->fence = fence;
    }
    for (i = 0; i < present_info->swapchainCount; ++i)
    {
        struct swapchain *swapchain = swapchain_from_handle( client_swapchains[i] );
        struct swapchain_snapshot *snapshot = reservations[i].snapshot;
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                          .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        VkImageMemoryBarrier image = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                      .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                                      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                      .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkBufferMemoryBarrier buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .size = VK_WHOLE_SIZE};
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {swapchain->host_extents.width, swapchain->host_extents.height, 1}};

        /* An exact completion is armed for a valid offscreen native target,
         * including while its owner scene is still preparing. */
        if (!swapchain_needs_snapshot( swapchain ) || presents[i].completion.kind != CLIENT_SURFACE_COMPLETION_EXACT) continue;
        assert( snapshot && snapshot->busy );
        if ((res = prepare_swapchain_snapshot( queue, swapchain, snapshot ))) goto done;
        image.image = snapshot->images[present_info->pImageIndices[i]];
        buffer.buffer = snapshot->buffer;
        if ((res = device->p_vkBeginCommandBuffer( snapshot->command, &begin ))) goto done;
        device->p_vkCmdPipelineBarrier( snapshot->command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &image );
        device->p_vkCmdCopyImageToBuffer( snapshot->command, image.image, image.newLayout, snapshot->buffer, 1, &copy );
        image.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        image.dstAccessMask = 0;
        image.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        image.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        device->p_vkCmdPipelineBarrier( snapshot->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                        0, 0, NULL, 1, &buffer, 1, &image );
        if ((res = device->p_vkEndCommandBuffer( snapshot->command ))) goto done;
        commands[count++] = snapshot->command;
    }
    source.command_count = count;
    source.commands = commands;
    source.ready_fence = pending->fence;
    if (count) present_info->pNext = &source;
    res = device->p_vkQueuePresentKHR( queue->host.queue, present_info );
    present_info->pNext = source.pNext;
    if (!submitted) goto done;

    /* The native provider consumed the original waits and accepted every
     * copy in its one WSI submit. There is no private Present semaphore. */
    for (i = 0; i < present_info->swapchainCount; ++i)
    {
        struct swapchain_snapshot *snapshot = reservations[i].snapshot;

        if (!presents[i].capture.apply) continue;
        assert( !snapshot->pending );
        snapshot->pending = pending;
        InterlockedIncrement( &pending->refs );
        /* The completion owns only the submitted fence. Capture owns the
         * buffer reservation separately, and the image retains another fence
         * reference to prevent reuse after an abandoned or timed-out wait. */
        InterlockedIncrement( &pending->refs );
        client_surface_set_present_completion( &presents[i], wait_vulkan_snapshot,
                                               release_vulkan_snapshot_completion, pending );
    }
done:
    if (!submitted)
        for (i = 0; i < present_info->swapchainCount; ++i)
        {
            struct client_surface_capture *capture = &presents[i].capture;

            /* No submitted fence was installed. The caller still owns and
             * releases all reservations after dropping the submission locks. */
            if (reservations[i].new_capture)
            {
                client_surface_free_metadata( reservations[i].snapshot->capture,
                                               sizeof(struct vulkan_snapshot_capture) );
                reservations[i].snapshot->capture = NULL;
            }
            memset( capture, 0, sizeof(*capture) );
        }
    if (!submitted && res && present_info->pResults)
        for (i = 0; i < present_info->swapchainCount; ++i) present_info->pResults[i] = res;
    release_snapshot_fence( pending );
    if (commands != commands_buffer)
        client_surface_free_metadata( commands, present_info->swapchainCount * sizeof(*commands) );
    return res;
}

static VkResult win32u_vkCreateSwapchainKHR( VkDevice client_device, const VkSwapchainCreateInfoKHR *create_info,
                                             const VkAllocationCallbacks *allocator, VkSwapchainKHR *ret )
{
    VkSwapchainPresentScalingCreateInfoEXT scaling = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT};
    struct swapchain *swapchain, *old_swapchain = swapchain_from_handle( create_info->oldSwapchain );
    struct surface *surface = surface_from_handle( create_info->surface );
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct vulkan_instance *instance = physical_device->instance;
    VkSwapchainCreateInfoKHR create_info_host = *create_info;
    VkSurfaceCapabilitiesKHR capabilities;
    VkSwapchainKHR host_swapchain;
    VkBool32 source_enabled = VK_FALSE;
    struct wine_vk_swapchain_source_create_info source = {WINE_VK_SWAPCHAIN_SOURCE_CREATE_INFO, NULL,
                                                          WINE_VK_SOURCE_ABI_VERSION, &source_enabled};
    struct vulkan_surface_source source_caps;
    BOOL needs_snapshot;
    struct ratio raw_dpi;
    RECT client_rect;
    VkResult res;

    if (!NtUserIsWindow( surface->hwnd ))
    {
        ERR( "surface %p, hwnd %p is invalid!\n", surface, surface->hwnd );
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (surface) create_info_host.surface = surface->obj.host.surface;
    if (old_swapchain) create_info_host.oldSwapchain = old_swapchain->obj.host.swapchain;

    if ((res = get_vulkan_surface_source( surface, create_info->imageFormat, &source_caps ))) return res;
    needs_snapshot = source_caps.type == VULKAN_SURFACE_SOURCE_READBACK;

    /* Windows allows client rect to be empty, but host Vulkan often doesn't, adjust extents back to the host capabilities */
    if (needs_snapshot)
        res = get_vulkan_source_capabilities( physical_device, surface, &capabilities );
    else
        res = instance->p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( physical_device->host.physical_device,
                                                                    surface->obj.host.surface, &capabilities );
    if (res) return res;

    if (needs_snapshot)
    {
        /* The thunk owns this chain, including the 64-bit usage override.
         * Updating imageUsage alone has no effect when that override exists. */
        VkImageUsageFlags2CreateInfoKHR *usage = (VkImageUsageFlags2CreateInfoKHR *)find_next_struct( create_info_host.pNext,
            VK_STRUCTURE_TYPE_IMAGE_USAGE_FLAGS_2_CREATE_INFO_KHR );

        if ((create_info->flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR) || create_info->imageArrayLayers != 1)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        create_info_host.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (usage) usage->usage |= VK_IMAGE_USAGE_2_TRANSFER_SRC_BIT_KHR;
        source.pNext = create_info_host.pNext;
        create_info_host.pNext = &source;
    }

    create_info_host.imageExtent.width = max( create_info_host.imageExtent.width, capabilities.minImageExtent.width );
    create_info_host.imageExtent.height = max( create_info_host.imageExtent.height, capabilities.minImageExtent.height );

    /* If the swapchain image size is not equal to the presentation size (e.g. because of DPI virtualization or
     * display mode change emulation), MoltenVK's vkQueuePresentKHR returns VK_SUBOPTIMAL_KHR.
     * Create the swapchain with VkSwapchainPresentScalingCreateInfoEXT to avoid this.
     */
    get_win_monitor_dpi( surface->hwnd, &raw_dpi );
    if (get_surface_rect( surface->hwnd, &client_rect, raw_dpi ) &&
        !extents_equals( &create_info_host.imageExtent, &client_rect ) &&
        impl_from_vulkan_device( device )->swapchain_maintenance1 &&
        !find_next_struct( create_info_host.pNext, scaling.sType ))
    {
        scaling.scalingBehavior = VK_PRESENT_SCALING_STRETCH_BIT_EXT;
        scaling.pNext = create_info_host.pNext;
        create_info_host.pNext = &scaling;
    }

    if (!(swapchain = calloc( 1, sizeof(*swapchain) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (pthread_mutex_init( &swapchain->present_lock, NULL ))
    {
        free( swapchain );
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (client_surface_cond_init( &swapchain->completion_cond ))
    {
        pthread_mutex_destroy( &swapchain->present_lock );
        free( swapchain );
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    /* No USER, client-surface, or device lock spans this drain. A wait which
     * already entered may finish, while later waits observe closed admission.
     * Native retirement is irreversible even when creating its replacement
     * fails. Keep the old host object pinned until that call returns. */
    if (old_swapchain) retire_swapchain_present_waits( old_swapchain );
    TRACE( "Entering native swapchain create replacing %p\n", old_swapchain );
    res = device->p_vkCreateSwapchainKHR( device->host.device, &create_info_host, NULL, &host_swapchain );
    if (!res && needs_snapshot && !source_enabled)
    {
        device->p_vkDestroySwapchainKHR( device->host.device, host_swapchain, NULL );
        res = VK_ERROR_FEATURE_NOT_PRESENT;
    }
    TRACE( "Native swapchain create replacing %p returned %d\n", old_swapchain, res );
    if (old_swapchain) release_swapchain_completion( old_swapchain );
    if (res)
    {
        pthread_cond_destroy( &swapchain->completion_cond );
        pthread_mutex_destroy( &swapchain->present_lock );
        free( swapchain );
        return res;
    }
    vulkan_object_init( &swapchain->obj.obj, host_swapchain );
    swapchain->surface = surface;
    InterlockedIncrement( &surface->refs );
    swapchain->extents = create_info->imageExtent;
    swapchain->host_extents = create_info_host.imageExtent;
    swapchain->format = create_info->imageFormat;
    swapchain->source = source_caps;
    swapchain->incremental_damage = create_info->preTransform == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR &&
                                   create_info->imageArrayLayers == 1 &&
                                   swapchain->extents.width == swapchain->host_extents.width &&
                                   swapchain->extents.height == swapchain->host_extents.height;
    instance->p_insert_object( instance, &swapchain->obj.obj );

    *ret = swapchain->obj.client.swapchain;
    return VK_SUCCESS;
}

static BOOL destroy_swapchain( struct vulkan_device *device, struct swapchain *swapchain )
{
    unsigned int i;
    BOOL busy;

    pthread_mutex_lock( &swapchain->present_lock );
    /* Public destruction has closed admission. Once the retained callbacks
     * and admitted waits leave, no new internal host access can enter. */
    assert( swapchain->retired );
    busy = swapchain->completion_refs || swapchain->present_waits;
    pthread_mutex_unlock( &swapchain->present_lock );
    if (busy) return FALSE;

    for (i = 0; i < ARRAY_SIZE(swapchain->snapshots); ++i)
    {
        struct swapchain_snapshot *snapshot = &swapchain->snapshots[i];
        struct vulkan_snapshot_fence *pending = snapshot->pending;
        VkResult res;

        /* Timeout, allocation failure and unknown errors do not authorize
         * freeing storage which the GPU may still access. */
        if (pending)
        {
            res = device->p_vkGetFenceStatus( device->host.device, pending->fence );
            if (res != VK_SUCCESS && res != VK_ERROR_DEVICE_LOST) return FALSE;
        }
    }

    TRACE( "destroying retired swapchain %p after its source copies\n", swapchain );
    for (i = 0; i < ARRAY_SIZE(swapchain->snapshots); ++i)
        destroy_swapchain_snapshot( device, &swapchain->snapshots[i] );
    device->p_vkDestroySwapchainKHR( device->host.device, swapchain->obj.host.swapchain, NULL );
    release_surface( swapchain->surface );

    pthread_cond_destroy( &swapchain->completion_cond );
    pthread_mutex_destroy( &swapchain->present_lock );
    free( swapchain );
    return TRUE;
}

static void swapchain_retirement_thread( void *context )
{
    struct device *device = context;
    struct swapchain *swapchain, *next;
    struct list pending = LIST_INIT(pending), retry = LIST_INIT(retry);

    pthread_mutex_lock( &device->retirement_lock );
    while (!device->retirement_shutdown || !list_empty( &device->retired_swapchains ))
    {
        if (list_empty( &device->retired_swapchains ))
        {
            pthread_cond_wait( &device->retirement_cond, &device->retirement_lock );
            continue;
        }
        list_move_tail( &pending, &device->retired_swapchains );
        pthread_mutex_unlock( &device->retirement_lock );
        /* No state mutex spans a host call. One stalled copy cannot hold
         * up reclamation of completed swapchains on another queue. */
        LIST_FOR_EACH_ENTRY_SAFE( swapchain, next, &pending, struct swapchain, retirement_entry )
        {
            list_remove( &swapchain->retirement_entry );
            if (!destroy_swapchain( &device->obj, swapchain ))
                list_add_tail( &retry, &swapchain->retirement_entry );
        }
        pthread_mutex_lock( &device->retirement_lock );
        list_move_tail( &device->retired_swapchains, &retry );
        if (!list_empty( &device->retired_swapchains ))
            client_surface_cond_timedwait( &device->retirement_cond, &device->retirement_lock, 10 );
    }
    pthread_mutex_unlock( &device->retirement_lock );
}

/* Acquire the device's retirement owner before accepting any asynchronous
 * presentation. It remains available through the device's last destruction,
 * including periods with no retired chains. No future destroy or completion
 * callback then needs to allocate a job, start a thread or poll on its caller. */
static BOOL start_swapchain_retirement_thread( struct device *device )
{
    NTSTATUS status = STATUS_NO_MEMORY;
    HANDLE thread = NULL;
    LONG count;

    pthread_mutex_lock( &device->retirement_lock );
    while (device->retirement_starting)
        pthread_cond_wait( &device->retirement_cond, &device->retirement_lock );
    assert( !device->retirement_shutdown );
    if (device->retirement_worker)
    {
        pthread_mutex_unlock( &device->retirement_lock );
        return TRUE;
    }
    device->retirement_starting = TRUE;
    pthread_mutex_unlock( &device->retirement_lock );

    count = ReadAcquire( &swapchain_retirement_workers );
    while (count < MAX_SWAPCHAIN_RETIREMENT_WORKERS)
    {
        LONG previous = InterlockedCompareExchange( &swapchain_retirement_workers, count + 1, count );

        if (previous != count) { count = previous; continue; }
        status = PsCreateSystemThread( &thread, THREAD_ALL_ACCESS, NULL, 0, NULL,
                                       swapchain_retirement_thread, device );
        if (status) InterlockedDecrement( &swapchain_retirement_workers );
        break;
    }
    pthread_mutex_lock( &device->retirement_lock );
    device->retirement_worker = status ? NULL : thread;
    device->retirement_starting = FALSE;
    pthread_cond_broadcast( &device->retirement_cond );
    pthread_mutex_unlock( &device->retirement_lock );
    if (status) WARN( "Failed to admit swapchain retirement worker, status %#lx\n", (unsigned long)status );
    return !status;
}

static void win32u_vkDestroySwapchainKHR( VkDevice client_device, VkSwapchainKHR client_swapchain,
                                          const VkAllocationCallbacks *allocator )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_instance *instance = device->physical_device->instance;
    struct device *impl = impl_from_vulkan_device( device );
    struct swapchain *swapchain = swapchain_from_handle( client_swapchain );

    if (allocator) FIXME( "Support for allocation callbacks not implemented yet\n" );
    if (!swapchain) return;

    pthread_mutex_lock( &swapchain->present_lock );
    swapchain->retired = TRUE;
    pthread_mutex_unlock( &swapchain->present_lock );
    instance->p_remove_object( instance, &swapchain->obj.obj );
    pthread_mutex_lock( &impl->retirement_lock );
    if (impl->retirement_worker)
    {
        /* Intrusive queue storage is part of the original swapchain. Native
         * destruction also belongs to this owner, outside the caller's path. */
        list_add_tail( &impl->retired_swapchains, &swapchain->retirement_entry );
        pthread_cond_signal( &impl->retirement_cond );
        pthread_mutex_unlock( &impl->retirement_lock );
        return;
    }
    pthread_mutex_unlock( &impl->retirement_lock );
    /* A device which never admitted asynchronous presentation has no private
     * copy or completion references. Its native-only destruction stays direct. */
    if (!destroy_swapchain( device, swapchain )) assert( 0 );
}

static VkResult win32u_vkAcquireNextImage2KHR( VkDevice client_device, const VkAcquireNextImageInfoKHR *acquire_info,
                                               uint32_t *image_index )
{
    struct vulkan_semaphore *semaphore = acquire_info->semaphore ? vulkan_semaphore_from_handle( acquire_info->semaphore ) : NULL;
    struct vulkan_fence *fence = acquire_info->fence ? vulkan_fence_from_handle( acquire_info->fence ) : NULL;
    struct swapchain *swapchain = swapchain_from_handle( acquire_info->swapchain );
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkAcquireNextImageInfoKHR acquire_info_host = *acquire_info;
    struct surface *surface = swapchain->surface;
    RECT client_rect;
    VkResult res;

    /* Do not acquire an image from a swapchain whose drawable has already
     * changed size.  Returning OUT_OF_DATE after the host acquire succeeds
     * would leave the caller with an acquired image and a signalled semaphore
     * which it believes do not exist.  Detect the normal resize path first so
     * the application can recreate the swapchain without consuming either. */
    if (get_surface_rect( surface->hwnd, &client_rect, get_dpi_for_window( surface->hwnd ) ) &&
        !extents_equals( &swapchain->extents, &client_rect ))
    {
        WARN( "Swapchain size %dx%d does not match client rect %s before acquire, returning VK_ERROR_OUT_OF_DATE_KHR\n",
              swapchain->extents.width, swapchain->extents.height, wine_dbgstr_rect( &client_rect ) );
        return VK_ERROR_OUT_OF_DATE_KHR;
    }

    acquire_info_host.swapchain = swapchain->obj.host.swapchain;
    acquire_info_host.semaphore = semaphore ? semaphore->host.semaphore : 0;
    acquire_info_host.fence = fence ? fence->host.fence : 0;
    res = device->p_vkAcquireNextImage2KHR( device->host.device, &acquire_info_host, image_index );

    if (!res && get_surface_rect( surface->hwnd, &client_rect, get_dpi_for_window( surface->hwnd ) ) &&
        !extents_equals( &swapchain->extents, &client_rect ))
    {
        WARN( "Swapchain size %dx%d does not match client rect %s, returning VK_SUBOPTIMAL_KHR\n",
              swapchain->extents.width, swapchain->extents.height, wine_dbgstr_rect( &client_rect ) );
        return VK_SUBOPTIMAL_KHR;
    }
    return res;
}

static VkResult win32u_vkAcquireNextImageKHR( VkDevice client_device, VkSwapchainKHR client_swapchain, uint64_t timeout,
                                              VkSemaphore client_semaphore, VkFence client_fence, uint32_t *image_index )
{
    struct vulkan_semaphore *semaphore = client_semaphore ? vulkan_semaphore_from_handle( client_semaphore ) : NULL;
    struct vulkan_fence *fence = client_fence ? vulkan_fence_from_handle( client_fence ) : NULL;
    struct swapchain *swapchain = swapchain_from_handle( client_swapchain );
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct surface *surface = swapchain->surface;
    RECT client_rect;
    VkResult res;

    /* See win32u_vkAcquireNextImage2KHR().  This check must precede the host
     * acquire so an OUT_OF_DATE result cannot orphan acquired-image state. */
    if (get_surface_rect( surface->hwnd, &client_rect, get_dpi_for_window( surface->hwnd ) ) &&
        !extents_equals( &swapchain->extents, &client_rect ))
    {
        WARN( "Swapchain size %dx%d does not match client rect %s before acquire, returning VK_ERROR_OUT_OF_DATE_KHR\n",
              swapchain->extents.width, swapchain->extents.height, wine_dbgstr_rect( &client_rect ) );
        return VK_ERROR_OUT_OF_DATE_KHR;
    }

    res = device->p_vkAcquireNextImageKHR( device->host.device, swapchain->obj.host.swapchain, timeout,
                                              semaphore ? semaphore->host.semaphore : 0, fence ? fence->host.fence : 0,
                                              image_index );

    if (!res && get_surface_rect( surface->hwnd, &client_rect, get_dpi_for_window( surface->hwnd ) ) &&
        !extents_equals( &swapchain->extents, &client_rect ))
    {
        WARN( "Swapchain size %dx%d does not match client rect %s, returning VK_SUBOPTIMAL_KHR\n",
              swapchain->extents.width, swapchain->extents.height, wine_dbgstr_rect( &client_rect ) );
        return VK_SUBOPTIMAL_KHR;
    }
    return res;
}

static void set_vulkan_present_damage( struct swapchain *swapchain, struct client_surface_frame *frame,
                                      const VkPresentRegionKHR *region )
{
    RECT damage = {0};
    unsigned int i;

    /* Incremental regions are relative to the previous presentation of this
     * swapchain. Name that source explicitly; the owner falls back to a full
     * image if it skipped the base, changed scene, or needs scaling. */
    if (!swapchain->incremental_damage || !swapchain->last_source_sequence ||
        !region || !region->rectangleCount || !region->pRectangles) return;
    for (i = 0; i < region->rectangleCount; ++i)
    {
        const VkRectLayerKHR *rect = &region->pRectangles[i];
        RECT next;

        if (rect->layer || rect->offset.x < 0 || rect->offset.y < 0 ||
            rect->offset.x > swapchain->extents.width || rect->offset.y > swapchain->extents.height ||
            rect->extent.width > swapchain->extents.width - rect->offset.x ||
            rect->extent.height > swapchain->extents.height - rect->offset.y) return;
        SetRect( &next, rect->offset.x, rect->offset.y,
                 rect->offset.x + rect->extent.width, rect->offset.y + rect->extent.height );
        union_rect( &damage, &damage, &next );
    }
    if (IsRectEmpty( &damage )) return;
    frame->damage = damage;
    frame->damage_base_sequence = swapchain->last_source_sequence;
}

static VkResult win32u_vkQueuePresentKHR( VkQueue client_queue, const VkPresentInfoKHR *client_present_info )
{
    VkPresentInfoKHR *present_info = (VkPresentInfoKHR *)client_present_info; /* cast away const, it has been copied in the thunks */
    struct vulkan_queue *queue = vulkan_queue_from_handle( client_queue );
    VkSwapchainKHR swapchains_buffer[16], *swapchains = swapchains_buffer;
    struct swapchain *present_swapchains_buffer[16], **present_swapchains = present_swapchains_buffer;
    struct client_surface *present_surfaces_buffer[16], **present_surfaces = present_surfaces_buffer;
    struct client_surface_frame presents_buffer[16], *presents = presents_buffer;
    struct vulkan_present_reservation reservations_buffer[16] = {{0}}, *reservations = reservations_buffer;
    VkResult results_buffer[16], *results = results_buffer;
    uint64_t present_ids_buffer[16], *present_ids = present_ids_buffer;
    VkPresentIdKHR present_id_info = {VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
    struct vulkan_device *device = queue->device;
    const VkSwapchainKHR *client_swapchains = present_info->pSwapchains;
    const VkPresentRegionsKHR *regions = find_next_struct( present_info->pNext,
                                                          VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR );
    uint32_t locked_count = 0, surface_locked_count = 0, reservation_count = 0;
    BOOL use_internal_present_wait, have_snapshots = FALSE, reserve_more;
    VkResult res;

    TRACE( "queue %p, present_info %p\n", queue, present_info );

    /* A pNext chain must not contain two VkPresentIdKHR structures.  Keep an
     * application's IDs untouched (vkWaitForPresentKHR observes that exact
     * namespace) and use the driver completion fallback for this call. */
    use_internal_present_wait = device->internal_present_wait &&
        !find_next_struct( present_info->pNext, VK_STRUCTURE_TYPE_PRESENT_ID_KHR );

    if (present_info->swapchainCount > ARRAY_SIZE(swapchains_buffer) &&
        !(swapchains = client_surface_alloc_metadata( present_info->swapchainCount, sizeof(*swapchains) )))
        goto allocation_failed;
    if (present_info->swapchainCount > ARRAY_SIZE(presents_buffer) &&
        !(presents = client_surface_alloc_metadata( present_info->swapchainCount, sizeof(*presents) )))
        goto allocation_failed;
    if (present_info->swapchainCount > ARRAY_SIZE(present_surfaces_buffer) &&
        !(present_surfaces = client_surface_alloc_metadata( present_info->swapchainCount, sizeof(*present_surfaces) )))
        goto allocation_failed;
    if (use_internal_present_wait && present_info->swapchainCount > ARRAY_SIZE(present_ids_buffer) &&
        !(present_ids = client_surface_alloc_metadata( present_info->swapchainCount, sizeof(*present_ids) )))
        goto allocation_failed;
    if (present_info->swapchainCount > ARRAY_SIZE(present_swapchains_buffer) &&
        !(present_swapchains = client_surface_alloc_metadata( present_info->swapchainCount, sizeof(*present_swapchains) )))
        goto allocation_failed;
    if (present_info->swapchainCount > ARRAY_SIZE(reservations_buffer) &&
        !(reservations = client_surface_alloc_metadata( present_info->swapchainCount, sizeof(*reservations) )))
        goto allocation_failed;
    reservation_count = present_info->swapchainCount;

    /* The aggregate native result does not describe every swapchain. Even
     * when the application omits pResults, successful snapshots in a batch
     * must still be published if another swapchain is out of date or lost. */
    if (!present_info->pResults)
    {
        if (present_info->swapchainCount > ARRAY_SIZE(results_buffer) &&
            !(results = client_surface_alloc_metadata( present_info->swapchainCount, sizeof(*results) )))
            goto allocation_failed;
        present_info->pResults = results;
    }
    for (uint32_t i = 0; i < present_info->swapchainCount; ++i)
        present_info->pResults[i] = VK_ERROR_UNKNOWN;

    for (uint32_t i = 0; i < present_info->waitSemaphoreCount; i++)
    {
        VkSemaphore *semaphores = (VkSemaphore *)present_info->pWaitSemaphores; /* cast away const, it has been copied in the thunks */
        struct vulkan_semaphore *semaphore = vulkan_semaphore_from_handle( semaphores[i] );
        semaphores[i] = semaphore->host.semaphore;
    }

    for (uint32_t i = 0; i < present_info->swapchainCount; i++)
    {
        struct swapchain *swapchain = swapchain_from_handle( present_info->pSwapchains[i] );
        swapchains[i] = swapchain->obj.host.swapchain;
        present_surfaces[i] = swapchain->surface->client;
        client_surface_prepare_scene( present_surfaces[i] );
        present_swapchains[i] = swapchain;
        if (use_internal_present_wait)
        {
            /* An acquired image may still be presented after retirement.
             * Select its driver completion before submitting it; present ID
             * zero leaves that swapchain out of the internal wait namespace. */
            pthread_mutex_lock( &swapchain->present_lock );
            present_ids[i] = !swapchain->retired;
            pthread_mutex_unlock( &swapchain->present_lock );
        }
    }

    if (present_info->swapchainCount > 1)
        qsort( present_swapchains, present_info->swapchainCount,
               sizeof(*present_swapchains), compare_swapchain_ptrs );
    present_info->pSwapchains = swapchains;
    if (use_internal_present_wait)
    {
        present_id_info.pNext = present_info->pNext;
        present_id_info.swapchainCount = present_info->swapchainCount;
        present_id_info.pPresentIds = present_ids;
        present_info->pNext = &present_id_info;
    }

reserve_completions:
    for (uint32_t i = 0; i < present_info->swapchainCount; ++i)
    {
        struct swapchain *swapchain = present_swapchains[i];
        unsigned int index;

        for (index = 0; index < present_info->swapchainCount; ++index)
            if (swapchain_from_handle( client_swapchains[index] ) == swapchain) break;
        assert( index < present_info->swapchainCount );
        if (!reservations[index].required &&
            !client_surface_needs_completion_reservation( swapchain->surface->client )) continue;
        /* Reserve every batch member's completion owner before any native
         * acceptance. Admission and allocation cannot fall back to a wait on
         * the submitting thread after its guest semaphore has been consumed. */
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        if (!reservations[index].job &&
            !(reservations[index].job = client_surface_reserve_completion_domain( swapchain->surface->client,
                impl_from_vulkan_device( device )->completion_domain_base + (queue - device->queues) )))
            goto reservation_failed;
        if (!start_swapchain_retirement_thread( impl_from_vulkan_device( device ) ))
            goto reservation_failed;
        if (!swapchain_needs_snapshot( swapchain ) && use_internal_present_wait && present_ids[index] &&
            !reservations[index].completion &&
            !(reservations[index].completion = client_surface_alloc_metadata( 1, sizeof(*reservations[index].completion) )))
            goto reservation_failed;
        if (!swapchain_needs_snapshot( swapchain ) || reservations[index].snapshot ||
            !(res = acquire_snapshot_reservation( device, swapchain, &reservations[index].snapshot ))) continue;
reservation_failed:
        if (present_info->pResults)
            for (uint32_t j = 0; j < present_info->swapchainCount; ++j) present_info->pResults[j] = res;
        goto done;
    }
    have_snapshots = reserve_more = FALSE;

    /* Completion ordering is per client surface.  Acquire locks in a stable
     * order across queues; exact external IDs release them after submission,
     * while fallback monitors keep them through composition so events cannot
     * be stolen. */
    if (present_info->swapchainCount > 1)
        qsort( present_surfaces, present_info->swapchainCount,
               sizeof(*present_surfaces), compare_client_surface_ptrs );
    for (uint32_t i = 0; i < present_info->swapchainCount; i++)
    {
        BOOL external_completion = TRUE;

        if (surface_locked_count && present_surfaces[i] == present_surfaces[surface_locked_count - 1])
            continue;
        for (uint32_t j = 0; j < present_info->swapchainCount; ++j)
        {
            struct swapchain *swapchain = swapchain_from_handle( client_swapchains[j] );

            if (swapchain->surface->client != present_surfaces[i]) continue;
            if (!swapchain_needs_snapshot( swapchain ) && !(use_internal_present_wait && present_ids[j]))
                external_completion = FALSE;
        }
        /* A shared monitor has no per-frame identity. All non-snapshot
         * presents to this surface must use it together, without draining
         * independent exact completions on another surface in the batch. */
        if (!external_completion && use_internal_present_wait)
            for (uint32_t j = 0; j < present_info->swapchainCount; ++j)
                if (swapchain_from_handle( client_swapchains[j] )->surface->client == present_surfaces[i])
                    present_ids[j] = 0;
        client_surface_lock_present( present_surfaces[i] );
        /* A completion wait releases its mutex.  Wait before taking any
         * later surface locks, otherwise another queue can take this mutex
         * and block on a later one while we wait to reacquire this one. */
        client_surface_wait_present_locked( present_surfaces[i], external_completion );
        present_surfaces[surface_locked_count++] = present_surfaces[i];
    }
    for (uint32_t i = 0; i < present_info->swapchainCount; i++)
    {
        struct swapchain *swapchain = swapchain_from_handle( client_swapchains[i] );

        client_surface_prepare_present_locked( swapchain->surface->client, &presents[i],
                                               (use_internal_present_wait && present_ids[i]) || swapchain_needs_snapshot( swapchain ) );
        have_snapshots |= swapchain_needs_snapshot( swapchain ) &&
                          presents[i].completion.kind == CLIENT_SURFACE_COMPLETION_EXACT;
        if (presents[i].completion.kind != CLIENT_SURFACE_COMPLETION_NONE &&
            (!reservations[i].job ||
             (swapchain_needs_snapshot( swapchain ) && !reservations[i].snapshot)))
            reserve_more = reservations[i].required = TRUE;
    }
    if (reserve_more)
    {
        /* A scene may become offscreen between the lock-free inspection and
         * preparation. Cancel only our unsubmitted tokens, then reserve its
         * completion owner and staging storage outside every surface lock.
         * DIRECT calls do not reserve resources they will not use. */
        for (uint32_t i = 0; i < present_info->swapchainCount; ++i)
        {
            struct client_surface *surface = swapchain_from_handle( client_swapchains[i] )->surface->client;

            client_surface_cancel_prepare_locked( surface, &presents[i] );
        }
        while (surface_locked_count)
            client_surface_unlock_present( present_surfaces[--surface_locked_count] );
        goto reserve_completions;
    }

    if (TRACE_ON(vulkan)) for (uint32_t i = 0; i < present_info->swapchainCount; ++i)
    {
        struct swapchain *swapchain = swapchain_from_handle( client_swapchains[i] );

        TRACE( "source selection swapchain %p format %u type %u mode %u readback %u\n",
               swapchain, swapchain->format, swapchain->source.type, presents[i].mode,
               swapchain_needs_snapshot( swapchain ) &&
               presents[i].completion.kind == CLIENT_SURFACE_COMPLETION_EXACT );
    }

    if (use_internal_present_wait)
    {
        /* Queue synchronization does not serialize the same swapchain across
         * distinct queues.  Lock every referenced swapchain in a stable order
         * so ID allocation and host submission preserve the required strictly
         * increasing order without serializing unrelated swapchains. */
        if (present_info->swapchainCount > 1)
            qsort( present_swapchains, present_info->swapchainCount,
                   sizeof(*present_swapchains), compare_swapchain_ptrs );
        for (uint32_t i = 0; i < present_info->swapchainCount; i++)
        {
            if (locked_count && present_swapchains[i] == present_swapchains[locked_count - 1]) continue;
            pthread_mutex_lock( &present_swapchains[i]->present_lock );
            present_swapchains[locked_count++] = present_swapchains[i];
        }
        for (uint32_t i = 0; i < present_info->swapchainCount; i++)
        {
            struct swapchain *swapchain = swapchain_from_handle( client_swapchains[i] );
            if (present_ids[i]) present_ids[i] = ++swapchain->next_present_id;
        }
    }

    if (have_snapshots)
        res = snapshot_vulkan_present( queue, present_info, client_swapchains, presents, reservations );
    else
        res = device->p_vkQueuePresentKHR( queue->host.queue, present_info );
    if (res == VK_ERROR_OUT_OF_HOST_MEMORY || res == VK_ERROR_OUT_OF_DEVICE_MEMORY ||
        res == VK_ERROR_DEVICE_LOST)
        for (uint32_t i = 0; i < present_info->swapchainCount; ++i) present_info->pResults[i] = res;

    /* Allocate producer serials before releasing either ordering domain.
     * This records host submission order even when another queue targets the
     * same client surface through a different swapchain. */
    for (uint32_t i = 0; i < present_info->swapchainCount; i++)
    {
        struct swapchain *swapchain = swapchain_from_handle( client_swapchains[i] );

        client_surface_submit_present_locked( swapchain->surface->client, &presents[i] );
        if ((present_info->pResults ? present_info->pResults[i] : res) >= VK_SUCCESS)
        {
            if (regions && regions->swapchainCount == present_info->swapchainCount)
                set_vulkan_present_damage( swapchain, &presents[i], &regions->pRegions[i] );
            swapchain->last_source_sequence = presents[i].serial;
        }
        else swapchain->last_source_sequence = 0;
    }
    while (locked_count)
        pthread_mutex_unlock( &present_swapchains[--locked_count]->present_lock );
    while (surface_locked_count)
        client_surface_unlock_present( present_surfaces[--surface_locked_count] );

    for (uint32_t i = 0; i < present_info->swapchainCount; ++i)
    {
        struct swapchain *swapchain = swapchain_from_handle( client_swapchains[i] );

        if (reservations[i].snapshot && !presents[i].capture.apply)
            release_snapshot_reservation( swapchain, reservations[i].snapshot );
        /* A submitted snapshot is now owned by its capture context. */
        reservations[i].snapshot = NULL;
    }

    for (uint32_t i = 0; i < present_info->swapchainCount; i++)
    {
        struct swapchain *swapchain = swapchain_from_handle( client_swapchains[i] );
        VkResult swapchain_res = present_info->pResults ? present_info->pResults[i] : res;
        struct surface *surface = swapchain->surface;
        SIZE expected_size = {swapchain->extents.width, swapchain->extents.height};
        BOOL compose = swapchain_res >= VK_SUCCESS;
        BOOL snapshot_submitted = !!presents[i].capture.apply;
        struct client_surface_completion completion = presents[i].completion;
        struct client_surface_capture capture = presents[i].capture;
        RECT client_rect;

        if (compose && !get_surface_rect( surface->hwnd, &client_rect,
                                          get_dpi_for_window( surface->hwnd ) ))
        {
            WARN( "Swapchain window %p is invalid, returning VK_ERROR_OUT_OF_DATE_KHR\n", surface->hwnd );
            if (present_info->pResults) present_info->pResults[i] = VK_ERROR_OUT_OF_DATE_KHR;
            if (res >= VK_SUCCESS || res == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT)
                res = VK_ERROR_OUT_OF_DATE_KHR;
            compose = FALSE;
        }
        else if (swapchain_res > VK_SUCCESS)
            WARN( "Present returned status %d for swapchain %p\n", swapchain_res, swapchain );
        if (compose && !extents_equals( &swapchain->extents, &client_rect ))
        {
            WARN( "Swapchain size %dx%d does not match client rect %s, returning VK_SUBOPTIMAL_KHR\n",
                  swapchain->extents.width, swapchain->extents.height, wine_dbgstr_rect( &client_rect ) );
            if (present_info->pResults) present_info->pResults[i] = VK_SUBOPTIMAL_KHR;
            if (!res) res = VK_SUBOPTIMAL_KHR;
            /* The host present has already been submitted.  Do not abandon
             * its completion wait merely because the shared Win32 geometry
             * is between resize states: this may be the application's only
             * frame at the new extent.  The wait supplies the required image
             * completion boundary, then the client-surface transaction refreshes
             * and checks the geometry again before taking its composition
             * snapshot.  It copies only if that check matches the presented
             * extent. */
        }

        if (compose && snapshot_submitted)
        {
            client_surface_defer_reserved_present( reservations[i].job, &presents[i], &expected_size );
            reservations[i].job = NULL;
            continue;
        }
        if (compose && presents[i].completion.kind == CLIENT_SURFACE_COMPLETION_SHARED)
        {
            /* Queue idle does not drain a host WSI worker's pending Present.
             * Keep its swapchain alive through driver completion and capture,
             * even when the application destroys an already retired chain. */
            retain_swapchain_completion( swapchain );
            client_surface_set_present_completion( &presents[i], wait_vulkan_driver_completion,
                                                   release_vulkan_driver_completion, swapchain );
            client_surface_defer_reserved_present( reservations[i].job, &presents[i], &expected_size );
            reservations[i].job = NULL;
            continue;
        }
        if (compose && presents[i].completion.kind == CLIENT_SURFACE_COMPLETION_EXACT &&
            use_internal_present_wait && present_ids[i] && !snapshot_submitted)
        {
            struct vulkan_present_completion *completion = reservations[i].completion;

            assert( completion );
            completion->device = device;
            completion->swapchain = swapchain;
            completion->present_id = present_ids[i];
            retain_swapchain_completion( swapchain );
            client_surface_set_present_completion( &presents[i], wait_vulkan_present_completion,
                                                   release_vulkan_present_completion, completion );
            reservations[i].completion = NULL;
            client_surface_defer_reserved_present( reservations[i].job, &presents[i], &expected_size );
            reservations[i].job = NULL;
            continue;
        }

        {
            BOOL completed;
            BOOL wait_skipped = use_internal_present_wait && !present_ids[i] &&
                                presents[i].completion.kind == CLIENT_SURFACE_COMPLETION_NONE;
            DWORD elapsed = NtGetTickCount() - presents[i].submission_time;
            DWORD remaining = elapsed < CLIENT_SURFACE_PRESENT_TIMEOUT ?
                              CLIENT_SURFACE_PRESENT_TIMEOUT - elapsed : 0;

            completed = client_surface_complete_present( surface->client, &presents[i], compose,
                                                         FALSE, &expected_size,
                                                         use_internal_present_wait && present_ids[i] ? 0 : remaining );
            if (!completed && compose)
            {
                /* The window changed after the post-present check, or the
                 * native completion source failed.  Preserve the staged
                 * generation for a correctly completed frame. */
                WARN( "Swapchain size %dx%d changed or did not complete before composition\n",
                      swapchain->extents.width, swapchain->extents.height );
                /* Retirement can close admission after this native Present
                 * succeeded. Do not publish an uncompleted frame or turn that
                 * internal wait restriction into an application Present error. */
                if (presents[i].result == CLIENT_SURFACE_FRAME_PENDING && !wait_skipped)
                {
                    if (present_info->pResults) present_info->pResults[i] = VK_SUBOPTIMAL_KHR;
                    if (!res) res = VK_SUBOPTIMAL_KHR;
                }
            }
        }
        if (snapshot_submitted)
        {
            completion.release( completion.context );
            capture.release( capture.context );
        }
    }

done:
    for (uint32_t i = 0; i < reservation_count; ++i)
    {
        client_surface_cancel_completion( reservations[i].job );
        client_surface_free_metadata( reservations[i].completion, sizeof(*reservations[i].completion) );
        if (reservations[i].snapshot)
            release_snapshot_reservation( swapchain_from_handle( client_swapchains[i] ), reservations[i].snapshot );
    }
    if (reservations != reservations_buffer)
        client_surface_free_metadata( reservations, present_info->swapchainCount * sizeof(*reservations) );
    if (results != results_buffer)
        client_surface_free_metadata( results, present_info->swapchainCount * sizeof(*results) );
    if (present_swapchains != present_swapchains_buffer)
        client_surface_free_metadata( present_swapchains, present_info->swapchainCount * sizeof(*present_swapchains) );
    if (present_ids != present_ids_buffer)
        client_surface_free_metadata( present_ids, present_info->swapchainCount * sizeof(*present_ids) );
    if (present_surfaces != present_surfaces_buffer)
        client_surface_free_metadata( present_surfaces, present_info->swapchainCount * sizeof(*present_surfaces) );
    if (presents != presents_buffer)
        client_surface_free_metadata( presents, present_info->swapchainCount * sizeof(*presents) );
    if (swapchains != swapchains_buffer)
        client_surface_free_metadata( swapchains, present_info->swapchainCount * sizeof(*swapchains) );

    if (TRACE_ON( fps ))
    {
        static unsigned long frames, frames_total;
        static long prev_time, start_time;
        DWORD time;

        time = NtGetTickCount();
        frames++;
        frames_total++;

        if (time - prev_time > 1500)
        {
            TRACE_(fps)( "%p @ approx %.2ffps, total %.2ffps\n", queue, 1000.0 * frames / (time - prev_time),
                         1000.0 * frames_total / (time - start_time) );
            prev_time = time;
            frames = 0;

            if (!start_time) start_time = time;
        }
    }

    return res;

allocation_failed:
    res = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (present_info->pResults)
        for (uint32_t i = 0; i < present_info->swapchainCount; ++i) present_info->pResults[i] = res;
    goto done;
}

static LARGE_INTEGER *get_nt_timeout( LARGE_INTEGER *time, DWORD timeout )
{
    if (timeout == INFINITE) return NULL;
    time->QuadPart = (ULONGLONG)timeout * -10000;
    return time;
}

static VkResult acquire_keyed_mutexes( VkWin32KeyedMutexAcquireReleaseInfoKHR *mutex_info, struct mempool *pool,
                                       const VkSemaphoreSubmitInfo **semaphores, UINT *semaphores_count )
{
    UINT i, count = *semaphores_count;
    VkSemaphoreSubmitInfo *submits;
    NTSTATUS status;

    if (!(submits = mem_alloc( pool, (count + mutex_info->acquireCount) * sizeof(*submits) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy( submits, *semaphores, count * sizeof(*submits) );
    memset( submits + count, 0, mutex_info->acquireCount * sizeof(*submits) );

    for (i = 0; i < mutex_info->acquireCount; i++)
    {
        LARGE_INTEGER timeout;
        struct device_memory *memory = device_memory_from_handle( mutex_info->pAcquireSyncs[i] );
        D3DKMT_ACQUIREKEYEDMUTEX acquire =
        {
            .hKeyedMutex = memory->mutex,
            .Key = mutex_info->pAcquireKeys[i],
            .pTimeout = get_nt_timeout( &timeout, mutex_info->pAcquireTimeouts[i] ),
        };
        VkSemaphoreSubmitInfo submit =
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = memory->semaphore,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0,
        };

        if ((status = NtGdiDdDDIAcquireKeyedMutex( &acquire ))) goto error;
        submit.value = memory->semaphore_value = acquire.FenceValue;
        submits[count++] = submit;
    }

    *semaphores = submits;
    *semaphores_count = count;
    return VK_SUCCESS;

error:
    WARN( "Failed to acquire keyed mutex 0x%s key 0x%s, status %#x\n", wine_dbgstr_longlong( mutex_info->pAcquireSyncs[i] ),
          wine_dbgstr_longlong( mutex_info->pAcquireKeys[i] ), status );

    while (i--)
    {
        struct device_memory *memory = device_memory_from_handle( mutex_info->pAcquireSyncs[i] );
        D3DKMT_RELEASEKEYEDMUTEX release =
        {
            .hKeyedMutex = memory->mutex,
            .Key = mutex_info->pAcquireKeys[i],
            .FenceValue = memory->semaphore_value,
        };
        NtGdiDdDDIReleaseKeyedMutex( &release );
    }
    return status == STATUS_TIMEOUT ? VK_TIMEOUT : VK_ERROR_UNKNOWN;
}

static VkResult release_keyed_mutexes( VkWin32KeyedMutexAcquireReleaseInfoKHR *mutex_info, struct mempool *pool,
                                       const VkSemaphoreSubmitInfo **semaphores, UINT *semaphores_count )
{
    UINT i, count = *semaphores_count;
    VkSemaphoreSubmitInfo *submits;
    NTSTATUS status;

    if (!(submits = mem_alloc( pool, (count + mutex_info->releaseCount) * sizeof(*submits) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy( submits, *semaphores, count * sizeof(*submits) );
    memset( submits + count, 0, mutex_info->releaseCount * sizeof(*submits) );

    for (i = 0; i < mutex_info->releaseCount; i++)
    {
        struct device_memory *memory = device_memory_from_handle( mutex_info->pReleaseSyncs[i] );
        D3DKMT_RELEASEKEYEDMUTEX release =
        {
            .hKeyedMutex = memory->mutex,
            .Key = mutex_info->pReleaseKeys[i],
            .FenceValue = memory->semaphore_value + 1,
        };
        VkSemaphoreSubmitInfo submit =
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = memory->semaphore,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0,
        };

        if ((status = NtGdiDdDDIReleaseKeyedMutex( &release ))) goto failed;
        submit.value = memory->semaphore_value + 1;
        submits[count++] = submit;
    }

    *semaphores = submits;
    *semaphores_count = count;
    return VK_SUCCESS;

failed:
    WARN( "Failed to release keyed mutex 0x%s key 0x%s, status %#x\n", wine_dbgstr_longlong( mutex_info->pReleaseSyncs[i] ),
          wine_dbgstr_longlong( mutex_info->pReleaseKeys[i] ), status );
    return VK_ERROR_UNKNOWN;
}

static VkResult win32u_vkQueueSubmit( VkQueue client_queue, uint32_t count, const VkSubmitInfo *submits, VkFence client_fence )
{
    struct vulkan_fence *fence = client_fence ? vulkan_fence_from_handle( client_fence ) : NULL;
    struct vulkan_queue *queue = vulkan_queue_from_handle( client_queue );
    struct vulkan_device *device = queue->device;
    VkResult res = VK_ERROR_OUT_OF_HOST_MEMORY;
    VkTimelineSemaphoreSubmitInfo *timelines;
    struct mempool pool = {0};

    TRACE( "queue %p, count %u, submits %p, fence %p\n", queue, count, submits, fence );

    if (!(timelines = mem_alloc( &pool, count * sizeof(*timelines) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset( timelines, 0, count * sizeof(*timelines) );

    for (uint32_t i = 0; i < count; i++)
    {
        VkSubmitInfo *submit = (VkSubmitInfo *)submits + i; /* cast away const, chain has been copied in the thunks */
        const VkSemaphoreSubmitInfo *wait_infos = NULL, *signal_infos = NULL;
        VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)submit;
        VkTimelineSemaphoreSubmitInfo *timeline = timelines + i;
        VkSemaphore *wait_semaphores, *signal_semaphores;
        VkDeviceGroupSubmitInfo *device_group = NULL;
        UINT wait_count = 0, signal_count = 0;
        VkPipelineStageFlags *wait_stages;
        uint32_t *indexes;
        uint64_t *values;

        for (uint32_t j = 0; j < submit->commandBufferCount; j++)
        {
            VkCommandBuffer *command_buffers = (VkCommandBuffer *)submit->pCommandBuffers; /* cast away const, chain has been copied in the thunks */
            struct vulkan_command_buffer *command_buffer = vulkan_command_buffer_from_handle( command_buffers[j] );
            command_buffers[j] = command_buffer->host.command_buffer;
        }

        for (uint32_t j = 0; j < submit->waitSemaphoreCount; j++)
        {
            VkSemaphore *semaphores = (VkSemaphore *)submit->pWaitSemaphores; /* cast away const, it has been copied in the thunks */
            struct vulkan_semaphore *semaphore = vulkan_semaphore_from_handle( semaphores[j] );
            semaphores[j] = semaphore->host.semaphore;
        }

        for (uint32_t j = 0; j < submit->signalSemaphoreCount; j++)
        {
            VkSemaphore *semaphores = (VkSemaphore *)submit->pSignalSemaphores; /* cast away const, it has been copied in the thunks */
            struct vulkan_semaphore *semaphore = vulkan_semaphore_from_handle( semaphores[j] );
            semaphores[j] = semaphore->host.semaphore;
        }

        for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
        {
            switch ((*next)->sType)
            {
            case VK_STRUCTURE_TYPE_D3D12_FENCE_SUBMIT_INFO_KHR:
            {
                VkD3D12FenceSubmitInfoKHR *info = (VkD3D12FenceSubmitInfoKHR *)*next;

                if (timeline->sType) ERR( "Duplicated timeline sync info.\n" );
                timeline->sType = info->sType;
                timeline->waitSemaphoreValueCount = info->waitSemaphoreValuesCount;
                timeline->pWaitSemaphoreValues = info->pWaitSemaphoreValues;
                timeline->signalSemaphoreValueCount = info->signalSemaphoreValuesCount;
                timeline->pSignalSemaphoreValues = info->pSignalSemaphoreValues;
                *next = (*next)->pNext; next = &prev;
                break;
            }
            case VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO:
                device_group = (VkDeviceGroupSubmitInfo *)*next;
                break;
            case VK_STRUCTURE_TYPE_FRAME_BOUNDARY_EXT: break;
            case VK_STRUCTURE_TYPE_FRAME_BOUNDARY_TENSORS_ARM: break;
            case VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV: break;
            case VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR: break;
            case VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO: break;
            case VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO:
                if (timeline->sType) ERR( "Duplicated timeline sync info.\n" );
                *timeline = *(VkTimelineSemaphoreSubmitInfo *)*next;
                *next = (*next)->pNext; next = &prev; /* remove it from the chain, we'll add it back below */
                break;
            case VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR:
            {
                VkWin32KeyedMutexAcquireReleaseInfoKHR *mutex_info = (VkWin32KeyedMutexAcquireReleaseInfoKHR *)*next;
                if ((res = acquire_keyed_mutexes( mutex_info, &pool, &wait_infos, &wait_count ))) goto failed;
                if ((res = release_keyed_mutexes( mutex_info, &pool, &signal_infos, &signal_count ))) goto failed;
                *next = (*next)->pNext; next = &prev;
                break;
            }
            default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
            }
        }

        if (wait_count) /* extra wait semaphores, need to update arrays and counts */
        {
            if (!(wait_semaphores = mem_alloc( &pool, (submit->waitSemaphoreCount + wait_count) * sizeof(*wait_semaphores) ))) goto failed;
            memcpy( wait_semaphores, submit->pWaitSemaphores, submit->waitSemaphoreCount * sizeof(*wait_semaphores) );
            submit->pWaitSemaphores = wait_semaphores;

            if (!(wait_stages = mem_alloc( &pool, (submit->waitSemaphoreCount + wait_count) * sizeof(*wait_stages) ))) goto failed;
            memcpy( wait_stages, submit->pWaitDstStageMask, submit->waitSemaphoreCount * sizeof(*wait_stages) );
            submit->pWaitDstStageMask = wait_stages;

            for (uint32_t j = 0; j < wait_count; j++)
            {
                wait_semaphores[submit->waitSemaphoreCount + j] = wait_infos[j].semaphore;
                wait_stages[submit->waitSemaphoreCount + j] = wait_infos[j].stageMask;
            }
            submit->waitSemaphoreCount += wait_count;

            if (!(values = mem_alloc( &pool, (timeline->waitSemaphoreValueCount + wait_count) * sizeof(*values) ))) goto failed;
            memcpy( values, timeline->pWaitSemaphoreValues, timeline->waitSemaphoreValueCount * sizeof(*values) );
            for (uint32_t j = 0; j < wait_count; j++) values[submit->waitSemaphoreCount + j] = wait_infos[j].value;
            timeline->waitSemaphoreValueCount = submit->waitSemaphoreCount;
            timeline->pWaitSemaphoreValues = values;

            if (device_group)
            {
                if (!(indexes = mem_alloc( &pool, submit->waitSemaphoreCount * sizeof(*indexes) ))) goto failed;
                memcpy( indexes, device_group->pWaitSemaphoreDeviceIndices, device_group->waitSemaphoreCount * sizeof(*indexes) );
                for (uint32_t j = 0; j < wait_count; j++) indexes[device_group->waitSemaphoreCount + j] = wait_infos[j].deviceIndex;
                device_group->waitSemaphoreCount = submit->waitSemaphoreCount;
                device_group->pWaitSemaphoreDeviceIndices = indexes;
            }
        }

        if (signal_count) /* extra signal semaphores, need to update arrays and counts */
        {
            if (!(signal_semaphores = mem_alloc( &pool, (submit->signalSemaphoreCount + signal_count) * sizeof(*signal_semaphores) ))) goto failed;
            memcpy( signal_semaphores, submit->pSignalSemaphores, submit->signalSemaphoreCount * sizeof(*signal_semaphores) );
            for (uint32_t j = 0; j < signal_count; j++) signal_semaphores[submit->signalSemaphoreCount + j] = signal_infos[j].semaphore;
            submit->signalSemaphoreCount += signal_count;
            submit->pSignalSemaphores = signal_semaphores;

            if (!(values = mem_alloc( &pool, submit->signalSemaphoreCount * sizeof(*values) ))) goto failed;
            memcpy( values, timeline->pSignalSemaphoreValues, timeline->signalSemaphoreValueCount * sizeof(*values) );
            for (uint32_t j = 0; j < signal_count; j++) values[submit->signalSemaphoreCount + j] = signal_infos[j].value;
            timeline->signalSemaphoreValueCount = submit->signalSemaphoreCount;
            timeline->pSignalSemaphoreValues = values;

            if (device_group)
            {
                if (!(indexes = mem_alloc( &pool, submit->signalSemaphoreCount * sizeof(*indexes) ))) goto failed;
                memcpy( indexes, device_group->pSignalSemaphoreDeviceIndices, device_group->signalSemaphoreCount * sizeof(*indexes) );
                for (uint32_t j = 0; j < signal_count; j++) indexes[device_group->signalSemaphoreCount + j] = signal_infos[j].deviceIndex;
                device_group->signalSemaphoreCount = submit->signalSemaphoreCount;
                device_group->pSignalSemaphoreDeviceIndices = indexes;
            }
        }

        /* insert the timeline semaphore values in the chain if it was there or has been created */
        if (timeline->sType || wait_count || signal_count)
        {
            timeline->sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            timeline->pNext = submit->pNext;
            submit->pNext = timeline;
        }
    }

    res = device->p_vkQueueSubmit( queue->host.queue, count, submits, fence ? fence->host.fence : 0 );

failed:
    mem_free( &pool );
    return res;
}

static VkResult queue_submit( struct vulkan_queue *queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence client_fence, PFN_vkQueueSubmit2 p_vkQueueSubmit2 )
{
    struct vulkan_fence *fence = client_fence ? vulkan_fence_from_handle( client_fence ) : NULL;
    struct mempool pool = {0};
    VkResult res;

    for (uint32_t i = 0; i < count; i++)
    {
        VkSubmitInfo2 *submit = (VkSubmitInfo2 *)submits + i; /* cast away const, chain has been copied in the thunks */
        VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)submit;

        for (uint32_t j = 0; j < submit->commandBufferInfoCount; j++)
        {
            VkCommandBufferSubmitInfo *command_buffer_infos = (VkCommandBufferSubmitInfo *)submit->pCommandBufferInfos; /* cast away const, chain has been copied in the thunks */
            struct vulkan_command_buffer *command_buffer = vulkan_command_buffer_from_handle( command_buffer_infos[j].commandBuffer );
            command_buffer_infos[j].commandBuffer = command_buffer->host.command_buffer;
            if (command_buffer_infos->pNext) FIXME( "Unhandled struct chain\n" );
        }

        for (uint32_t j = 0; j < submit->waitSemaphoreInfoCount; j++)
        {
            VkSemaphoreSubmitInfo *semaphore_infos = (VkSemaphoreSubmitInfo *)submit->pWaitSemaphoreInfos; /* cast away const, it has been copied in the thunks */
            struct vulkan_semaphore *semaphore = vulkan_semaphore_from_handle( semaphore_infos[j].semaphore );
            semaphore_infos[j].semaphore = semaphore->host.semaphore;
            if (semaphore_infos->pNext) FIXME( "Unhandled struct chain\n" );
        }

        for (uint32_t j = 0; j < submit->signalSemaphoreInfoCount; j++)
        {
            VkSemaphoreSubmitInfo *semaphore_infos = (VkSemaphoreSubmitInfo *)submit->pSignalSemaphoreInfos; /* cast away const, it has been copied in the thunks */
            struct vulkan_semaphore *semaphore = vulkan_semaphore_from_handle( semaphore_infos[j].semaphore );
            semaphore_infos[j].semaphore = semaphore->host.semaphore;
            if (semaphore_infos->pNext) FIXME( "Unhandled struct chain\n" );
        }

        for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
        {
            switch ((*next)->sType)
            {
            case VK_STRUCTURE_TYPE_FRAME_BOUNDARY_EXT: break;
            case VK_STRUCTURE_TYPE_FRAME_BOUNDARY_TENSORS_ARM: break;
            case VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV: break;
            case VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR: break;
            case VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR:
            {
                VkWin32KeyedMutexAcquireReleaseInfoKHR *mutex_info = (VkWin32KeyedMutexAcquireReleaseInfoKHR *)*next;
                if ((res = acquire_keyed_mutexes( mutex_info, &pool, &submit->pWaitSemaphoreInfos, &submit->waitSemaphoreInfoCount ))) goto failed;
                if ((res = release_keyed_mutexes( mutex_info, &pool, &submit->pSignalSemaphoreInfos, &submit->signalSemaphoreInfoCount ))) goto failed;
                *next = (*next)->pNext; next = &prev;
                break;
            }
            default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
            }
        }
    }

    res = p_vkQueueSubmit2( queue->host.queue, count, submits, fence ? fence->host.fence : 0 );

failed:
    mem_free( &pool );
    return res;
}

static VkResult win32u_vkQueueSubmit2( VkQueue client_queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence client_fence )
{
    struct vulkan_fence *fence = client_fence ? vulkan_fence_from_handle( client_fence ) : NULL;
    struct vulkan_queue *queue = vulkan_queue_from_handle( client_queue );
    struct vulkan_device *device = queue->device;

    TRACE( "queue %p, count %u, submits %p, fence %p\n", queue, count, submits, fence );

    return queue_submit( queue, count, submits, client_fence, device->p_vkQueueSubmit2 );
}

static VkResult win32u_vkQueueSubmit2KHR( VkQueue client_queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence client_fence )
{
    struct vulkan_fence *fence = client_fence ? vulkan_fence_from_handle( client_fence ) : NULL;
    struct vulkan_queue *queue = vulkan_queue_from_handle( client_queue );
    struct vulkan_device *device = queue->device;

    TRACE( "queue %p, count %u, submits %p, fence %p\n", queue, count, submits, fence );

    return queue_submit( queue, count, submits, client_fence, device->p_vkQueueSubmit2KHR );
}

static HANDLE create_shared_semaphore_handle( D3DKMT_HANDLE local, const VkExportSemaphoreWin32HandleInfoKHR *info )
{
    SECURITY_DESCRIPTOR *security = info->pAttributes ? info->pAttributes->lpSecurityDescriptor : NULL;
    WCHAR bufferW[MAX_PATH * 2];
    UNICODE_STRING name = {.Buffer = bufferW};
    OBJECT_ATTRIBUTES attr;
    NTSTATUS status;
    HANDLE shared;

    if (info->name) init_shared_resource_path( info->name, &name );
    InitializeObjectAttributes( &attr, info->name ? &name : NULL, OBJ_CASE_INSENSITIVE, NULL, security );

    if (!(status = NtGdiDdDDIShareObjects( 1, &local, &attr, info->dwAccess, &shared ))) return shared;
    WARN( "Failed to share resource %#x, status %#x\n", local, status );
    return NULL;
}

HANDLE open_shared_semaphore_from_name( const WCHAR *name )
{
    D3DKMT_OPENSYNCOBJECTNTHANDLEFROMNAME open_name = {0};
    WCHAR bufferW[MAX_PATH * 2];
    UNICODE_STRING name_str = {.Buffer = bufferW};
    OBJECT_ATTRIBUTES attr;
    NTSTATUS status;

    init_shared_resource_path( name, &name_str );
    InitializeObjectAttributes( &attr, &name_str, OBJ_OPENIF, NULL, NULL );

    open_name.dwDesiredAccess = GENERIC_ALL;
    open_name.pObjAttrib = &attr;
    status = NtGdiDdDDIOpenSyncObjectNtHandleFromName( &open_name );
    if (status) WARN( "Failed to open %s, status %#x\n", debugstr_w( name ), status );
    return open_name.hNtHandle;
}

static VkResult win32u_vkCreateSemaphore( VkDevice client_device, const VkSemaphoreCreateInfo *client_create_info,
                                          const VkAllocationCallbacks *allocator, VkSemaphore *ret )
{
    VkSemaphoreCreateInfo *create_info = (VkSemaphoreCreateInfo *)client_create_info; /* cast away const, chain has been copied in the thunks */
    VkExportSemaphoreWin32HandleInfoKHR export_win32 = {.dwAccess = GENERIC_ALL};
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)create_info;
    struct vulkan_instance *instance = device->physical_device->instance;
    VkExportSemaphoreCreateInfoKHR *export_info = NULL;
    struct semaphore *semaphore;
    VkSemaphore host_semaphore;
    BOOL nt_shared = FALSE;
    VkResult res;

    TRACE( "device %p, create_info %p, allocator %p, ret %p\n", device, create_info, allocator, ret );

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO:
            export_info = (VkExportSemaphoreCreateInfoKHR *)*next;
            if (!(export_info->handleTypes & EXTERNAL_SEMAPHORE_WIN32_BITS))
                FIXME( "Unsupported handle types %#x\n", export_info->handleTypes );
            else
            {
                nt_shared = !(export_info->handleTypes & VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT);
                export_info->handleTypes = get_host_external_semaphore_type();
            }
            break;
        case VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR:
            export_win32 = *(VkExportSemaphoreWin32HandleInfoKHR *)*next;
            *next = (*next)->pNext; next = &prev;
            break;
        case VK_STRUCTURE_TYPE_QUERY_LOW_LATENCY_SUPPORT_NV: break;
        case VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    if (!(semaphore = calloc( 1, sizeof(*semaphore) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;

    if ((res = device->p_vkCreateSemaphore( device->host.device, create_info, NULL /* allocator */, &host_semaphore )))
    {
        free( semaphore );
        return res;
    }

    if (export_info)
    {
        VkSemaphoreGetFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = host_semaphore};
        int fd = -1;

        switch ((fd_info.handleType = get_host_external_semaphore_type()))
        {
        case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT:
            if ((res = device->p_vkGetSemaphoreFdKHR( device->host.device, &fd_info, &fd ))) goto failed;
            break;
        default:
            FIXME( "Unsupported handle type %#x\n", fd_info.handleType );
            break;
        }

        semaphore->local = d3dkmt_create_sync( fd, nt_shared ? NULL : &semaphore->global );
        close( fd );

        if (!semaphore->local) goto failed;
        if (nt_shared && !(semaphore->shared = create_shared_semaphore_handle( semaphore->local, &export_win32 ))) goto failed;
    }

    vulkan_object_init( &semaphore->obj.obj, host_semaphore );
    instance->p_insert_object( instance, &semaphore->obj.obj );

    *ret = semaphore->obj.client.semaphore;
    return res;

failed:
    WARN( "Failed to create semaphore, res %d\n", res );
    device->p_vkDestroySemaphore( device->host.device, host_semaphore, NULL );
    d3dkmt_destroy_sync( semaphore->local );
    free( semaphore );
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void win32u_vkDestroySemaphore( VkDevice client_device, VkSemaphore client_semaphore, const VkAllocationCallbacks *allocator )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct semaphore *semaphore = semaphore_from_handle( client_semaphore );
    struct vulkan_instance *instance = device->physical_device->instance;

    TRACE( "device %p, semaphore %p, allocator %p\n", device, semaphore, allocator );

    if (!client_semaphore) return;

    device->p_vkDestroySemaphore( device->host.device, semaphore->obj.host.semaphore, NULL /* allocator */ );
    instance->p_remove_object( instance, &semaphore->obj.obj );

    if (semaphore->shared) NtClose( semaphore->shared );
    d3dkmt_destroy_sync( semaphore->local );
    free( semaphore );
}

static VkResult win32u_vkGetSemaphoreWin32HandleKHR( VkDevice client_device, const VkSemaphoreGetWin32HandleInfoKHR *handle_info, HANDLE *handle )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct semaphore *semaphore = semaphore_from_handle( handle_info->semaphore );

    TRACE( "device %p, handle_info %p, handle %p\n", device, handle_info, handle );

    switch (handle_info->handleType)
    {
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
        TRACE( "Returning global D3DKMT handle %#x\n", semaphore->global );
        *handle = UlongToPtr( semaphore->global );
        return VK_SUCCESS;

    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT:
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT:
        NtDuplicateObject( NtCurrentProcess(), semaphore->shared, NtCurrentProcess(), handle, 0, 0, DUPLICATE_SAME_ATTRIBUTES | DUPLICATE_SAME_ACCESS );
        TRACE( "Returning NT shared handle %p -> %p\n", semaphore->shared, *handle );
        return VK_SUCCESS;

    default:
        FIXME( "Unsupported handle type %#x\n", handle_info->handleType );
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
}

static VkResult win32u_vkImportSemaphoreWin32HandleKHR( VkDevice client_device, const VkImportSemaphoreWin32HandleInfoKHR *handle_info )
{
    VkImportSemaphoreFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct semaphore *semaphore = semaphore_from_handle( handle_info->semaphore );
    struct vulkan_instance *instance = device->physical_device->instance;
    D3DKMT_HANDLE local, global = 0;
    VkResult res = VK_SUCCESS;
    HANDLE shared = NULL;

    TRACE( "device %p, handle_info %p\n", device, handle_info );

    switch (handle_info->handleType)
    {
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
        global = PtrToUlong( handle_info->handle );
        if (!(local = d3dkmt_open_sync( global, NULL ))) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        break;
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT:
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT:
        if (handle_info->name && !(shared = open_shared_semaphore_from_name( handle_info->name )))
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        else if (!(shared = handle_info->handle) || NtDuplicateObject( NtCurrentProcess(), shared, NtCurrentProcess(), &shared,
                                                                       0, 0, DUPLICATE_SAME_ATTRIBUTES | DUPLICATE_SAME_ACCESS ))
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;

        if (!(local = d3dkmt_open_sync( 0, shared )))
        {
            NtClose( shared );
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        break;
    default:
        FIXME( "Unsupported handle type %#x\n", handle_info->handleType );
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    if ((fd_info.fd = d3dkmt_object_get_fd( local )) < 0) res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    if (!res && handle_info->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT)
    {
        /* Recreate semaphore to make sure it has timeline type. */
        VkSemaphoreTypeCreateInfo type_info =
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        };
        VkSemaphoreCreateInfo create_info =
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &type_info,
        };
        VkSemaphore new_semaphore;

        if ((res = device->p_vkCreateSemaphore( device->host.device, &create_info, NULL, &new_semaphore )))
        {
            ERR( "Failed to create timeline semaphore, vr %d.\n", res );
        }
        else
        {
            instance->p_remove_object( instance, &semaphore->obj.obj );
            device->p_vkDestroySemaphore( device->host.device, semaphore->obj.host.semaphore, NULL );
            vulkan_object_init( &semaphore->obj.obj, new_semaphore );
            instance->p_insert_object( instance, &semaphore->obj.obj );
        }
    }

    if (!res)
    {
        fd_info.handleType = get_host_external_semaphore_type();
        fd_info.semaphore = semaphore->obj.host.semaphore;
        fd_info.flags = handle_info->flags;
        res = device->p_vkImportSemaphoreFdKHR( device->host.device, &fd_info );
    }

    if (res || handle_info->flags & VK_SEMAPHORE_IMPORT_TEMPORARY_BIT)
    {
        /* FIXME: Should we still keep the temporary handles for vkGetSemaphoreWin32HandleKHR? */
        if (shared) NtClose( shared );
        d3dkmt_destroy_sync( local );
    }
    else
    {
        if (semaphore->shared) NtClose( semaphore->shared );
        d3dkmt_destroy_sync( semaphore->local );
        semaphore->shared = shared;
        semaphore->global = global;
        semaphore->local = local;
    }
    return res;
}

static void get_physical_device_external_semaphore_properties( struct vulkan_physical_device *physical_device, const VkPhysicalDeviceExternalSemaphoreInfo *client_semaphore_info,
                                                               VkExternalSemaphoreProperties *semaphore_properties, PFN_vkGetPhysicalDeviceExternalSemaphoreProperties p_vkGetPhysicalDeviceExternalSemaphoreProperties )
{
    VkPhysicalDeviceExternalSemaphoreInfo *semaphore_info = (VkPhysicalDeviceExternalSemaphoreInfo *)client_semaphore_info; /* cast away const, it has been copied in the thunks */
    VkExternalSemaphoreHandleTypeFlagBits handle_type;

    handle_type = semaphore_info->handleType;
    if (semaphore_info->handleType & EXTERNAL_SEMAPHORE_WIN32_BITS) semaphore_info->handleType = get_host_external_semaphore_type();

    p_vkGetPhysicalDeviceExternalSemaphoreProperties( physical_device->host.physical_device, semaphore_info, semaphore_properties );
    semaphore_properties->compatibleHandleTypes = handle_type;
    semaphore_properties->exportFromImportedHandleTypes = handle_type;
}

static void win32u_vkGetPhysicalDeviceExternalSemaphoreProperties( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalSemaphoreInfo *semaphore_info,
                                                                   VkExternalSemaphoreProperties *semaphore_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, semaphore_info %p, semaphore_properties %p\n", physical_device, semaphore_info, semaphore_properties );

    get_physical_device_external_semaphore_properties( physical_device, semaphore_info, semaphore_properties, instance->p_vkGetPhysicalDeviceExternalSemaphoreProperties );
}

static void win32u_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalSemaphoreInfo *semaphore_info,
                                                                      VkExternalSemaphoreProperties *semaphore_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, semaphore_info %p, semaphore_properties %p\n", physical_device, semaphore_info, semaphore_properties );

    get_physical_device_external_semaphore_properties( physical_device, semaphore_info, semaphore_properties, instance->p_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR );
}

static VkResult win32u_vkCreateFence( VkDevice client_device, const VkFenceCreateInfo *client_create_info, const VkAllocationCallbacks *allocator, VkFence *ret )
{
    VkFenceCreateInfo *create_info = (VkFenceCreateInfo *)client_create_info; /* cast away const, chain has been copied in the thunks */
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkExportSemaphoreWin32HandleInfoKHR export_win32 = {.dwAccess = GENERIC_ALL};
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)create_info;
    struct vulkan_instance *instance = device->physical_device->instance;
    VkExportFenceCreateInfoKHR *export_info = NULL;
    BOOL nt_shared = FALSE;
    struct fence *fence;
    VkFence host_fence;
    VkResult res;

    TRACE( "device %p, create_info %p, allocator %p, ret %p\n", device, create_info, allocator, ret );

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO:
            export_info = (VkExportFenceCreateInfoKHR *)*next;
            if (!(export_info->handleTypes & EXTERNAL_FENCE_WIN32_BITS))
                FIXME( "Unsupported handle types %#x\n", export_info->handleTypes );
            else
            {
                nt_shared = !(export_info->handleTypes & VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT);
                export_info->handleTypes = get_host_external_fence_type();
            }
            break;
        case VK_STRUCTURE_TYPE_EXPORT_FENCE_WIN32_HANDLE_INFO_KHR:
        {
            VkExportFenceWin32HandleInfoKHR *fence_win32 = (VkExportFenceWin32HandleInfoKHR *)*next;
            export_win32.pAttributes = fence_win32->pAttributes;
            export_win32.dwAccess = fence_win32->dwAccess;
            export_win32.name = fence_win32->name;
            *next = (*next)->pNext; next = &prev;
            break;
        }
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    if (!(fence = calloc( 1, sizeof(*fence) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;

    if ((res = device->p_vkCreateFence( device->host.device, create_info, NULL /* allocator */, &host_fence )))
    {
        free( fence );
        return res;
    }

    if (export_info)
    {
        VkFenceGetFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, .fence = host_fence};
        int fd = -1;

        switch ((fd_info.handleType = get_host_external_fence_type()))
        {
        case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT:
            if ((res = device->p_vkGetFenceFdKHR( device->host.device, &fd_info, &fd ))) goto failed;
            break;
        default:
            FIXME( "Unsupported handle type %#x\n", fd_info.handleType );
            break;
        }

        fence->local = d3dkmt_create_sync( fd, nt_shared ? NULL : &fence->global );
        close( fd );

        if (!fence->local) goto failed;
        if (nt_shared && !(fence->shared = create_shared_semaphore_handle( fence->local, &export_win32 ))) goto failed;
    }

    vulkan_object_init( &fence->obj.obj, host_fence );
    instance->p_insert_object( instance, &fence->obj.obj );

    *ret = fence->obj.client.fence;
    return res;

failed:
    WARN( "Failed to create fence, res %d\n", res );
    device->p_vkDestroyFence( device->host.device, host_fence, NULL );
    d3dkmt_destroy_sync( fence->local );
    free( fence );
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void win32u_vkDestroyFence( VkDevice client_device, VkFence client_fence, const VkAllocationCallbacks *allocator )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct fence *fence = fence_from_handle( client_fence );
    struct vulkan_instance *instance = device->physical_device->instance;

    TRACE( "device %p, fence %p, allocator %p\n", device, fence, allocator );

    if (!client_fence) return;

    device->p_vkDestroyFence( device->host.device, fence->obj.host.fence, NULL /* allocator */ );
    instance->p_remove_object( instance, &fence->obj.obj );

    if (fence->shared) NtClose( fence->shared );
    d3dkmt_destroy_sync( fence->local );
    free( fence );
}

static VkResult win32u_vkGetFenceWin32HandleKHR( VkDevice client_device, const VkFenceGetWin32HandleInfoKHR *handle_info, HANDLE *handle )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct fence *fence = fence_from_handle( handle_info->fence );

    TRACE( "device %p, handle_info %p, handle %p\n", device, handle_info, handle );

    switch (handle_info->handleType)
    {
    case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
        TRACE( "Returning global D3DKMT handle %#x\n", fence->global );
        *handle = UlongToPtr( fence->global );
        return VK_SUCCESS;

    case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT:
        NtDuplicateObject( NtCurrentProcess(), fence->shared, NtCurrentProcess(), handle, 0, 0, DUPLICATE_SAME_ATTRIBUTES | DUPLICATE_SAME_ACCESS );
        TRACE( "Returning NT shared handle %p -> %p\n", fence->shared, *handle );
        return VK_SUCCESS;

    default:
        FIXME( "Unsupported handle type %#x\n", handle_info->handleType );
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
}

static VkResult win32u_vkImportFenceWin32HandleKHR( VkDevice client_device, const VkImportFenceWin32HandleInfoKHR *handle_info )
{
    VkImportFenceFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR};
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct fence *fence = fence_from_handle( handle_info->fence );
    D3DKMT_HANDLE local, global = 0;
    HANDLE shared = NULL;
    VkResult res;

    TRACE( "device %p, handle_info %p\n", device, handle_info );

    switch (handle_info->handleType)
    {
    case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
        global = PtrToUlong( handle_info->handle );
        if (!(local = d3dkmt_open_sync( global, NULL ))) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        break;
    case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT:
        if (handle_info->name && !(shared = open_shared_semaphore_from_name( handle_info->name )))
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        else if (!(shared = handle_info->handle) || NtDuplicateObject( NtCurrentProcess(), shared, NtCurrentProcess(), &shared,
                                                                       0, 0, DUPLICATE_SAME_ATTRIBUTES | DUPLICATE_SAME_ACCESS ))
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;

        if (!(local = d3dkmt_open_sync( 0, shared )))
        {
            NtClose( shared );
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        break;
    default:
        FIXME( "Unsupported handle type %#x\n", handle_info->handleType );
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    if ((fd_info.fd = d3dkmt_object_get_fd( local )) < 0) res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    else
    {
        fd_info.handleType = get_host_external_fence_type();
        fd_info.fence = fence->obj.host.fence;
        fd_info.flags = handle_info->flags;
        res = device->p_vkImportFenceFdKHR( device->host.device, &fd_info );
    }

    if (res || handle_info->flags & VK_FENCE_IMPORT_TEMPORARY_BIT)
    {
        /* FIXME: Should we still keep the temporary handles for vkGetFenceWin32HandleKHR? */
        if (shared) NtClose( shared );
        d3dkmt_destroy_sync( local );
    }
    else
    {
        if (fence->shared) NtClose( fence->shared );
        if (fence->local) d3dkmt_destroy_sync( fence->local );
        fence->shared = shared;
        fence->global = global;
        fence->local = local;
    }
    return res;
}

static void get_physical_device_external_fence_properties( struct vulkan_physical_device *physical_device, const VkPhysicalDeviceExternalFenceInfo *client_fence_info,
                                                           VkExternalFenceProperties *fence_properties, PFN_vkGetPhysicalDeviceExternalFenceProperties p_vkGetPhysicalDeviceExternalFenceProperties )
{
    VkPhysicalDeviceExternalFenceInfo *fence_info = (VkPhysicalDeviceExternalFenceInfo *)client_fence_info; /* cast away const, it has been copied in the thunks */
    VkExternalFenceHandleTypeFlagBits handle_type;

    handle_type = fence_info->handleType;
    if (fence_info->handleType & EXTERNAL_FENCE_WIN32_BITS) fence_info->handleType = get_host_external_fence_type();

    p_vkGetPhysicalDeviceExternalFenceProperties( physical_device->host.physical_device, fence_info, fence_properties );
    fence_properties->compatibleHandleTypes = handle_type;
    fence_properties->exportFromImportedHandleTypes = handle_type;
}

static void win32u_vkGetPhysicalDeviceExternalFenceProperties( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalFenceInfo *fence_info,
                                                               VkExternalFenceProperties *fence_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, fence_info %p, fence_properties %p\n", physical_device, fence_info, fence_properties );

    get_physical_device_external_fence_properties( physical_device, fence_info, fence_properties, instance->p_vkGetPhysicalDeviceExternalFenceProperties );
}

static void win32u_vkGetPhysicalDeviceExternalFencePropertiesKHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalFenceInfo *fence_info,
                                                                  VkExternalFenceProperties *fence_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, fence_info %p, fence_properties %p\n", physical_device, fence_info, fence_properties );

    get_physical_device_external_fence_properties( physical_device, fence_info, fence_properties, instance->p_vkGetPhysicalDeviceExternalFencePropertiesKHR );
}

static struct vulkan_funcs vulkan_funcs =
{
    .p_vkAcquireNextImage2KHR = win32u_vkAcquireNextImage2KHR,
    .p_vkAcquireNextImageKHR = win32u_vkAcquireNextImageKHR,
    .p_vkAllocateMemory = win32u_vkAllocateMemory,
    .p_vkCreateBuffer = win32u_vkCreateBuffer,
    .p_vkCreateDevice = win32u_vkCreateDevice,
    .p_vkCreateFence = win32u_vkCreateFence,
    .p_vkCreateImage = win32u_vkCreateImage,
    .p_vkCreateInstance = win32u_vkCreateInstance,
    .p_vkCreateSemaphore = win32u_vkCreateSemaphore,
    .p_vkCreateSwapchainKHR = win32u_vkCreateSwapchainKHR,
    .p_vkCreateWin32SurfaceKHR = win32u_vkCreateWin32SurfaceKHR,
    .p_vkDestroyDevice = win32u_vkDestroyDevice,
    .p_vkDestroyFence = win32u_vkDestroyFence,
    .p_vkDestroyInstance = win32u_vkDestroyInstance,
    .p_vkDestroySemaphore = win32u_vkDestroySemaphore,
    .p_vkDestroySurfaceKHR = win32u_vkDestroySurfaceKHR,
    .p_vkDestroySwapchainKHR = win32u_vkDestroySwapchainKHR,
    .p_vkFreeMemory = win32u_vkFreeMemory,
    .p_vkGetDeviceBufferMemoryRequirements = win32u_vkGetDeviceBufferMemoryRequirements,
    .p_vkGetDeviceBufferMemoryRequirementsKHR = win32u_vkGetDeviceBufferMemoryRequirements,
    .p_vkGetDeviceImageMemoryRequirements = win32u_vkGetDeviceImageMemoryRequirements,
    .p_vkGetDeviceQueue = win32u_vkGetDeviceQueue,
    .p_vkGetDeviceQueue2 = win32u_vkGetDeviceQueue2,
    .p_vkGetFenceWin32HandleKHR = win32u_vkGetFenceWin32HandleKHR,
    .p_vkGetMemoryWin32HandleKHR = win32u_vkGetMemoryWin32HandleKHR,
    .p_vkGetMemoryWin32HandlePropertiesKHR = win32u_vkGetMemoryWin32HandlePropertiesKHR,
    .p_vkGetPhysicalDeviceExternalBufferProperties = win32u_vkGetPhysicalDeviceExternalBufferProperties,
    .p_vkGetPhysicalDeviceExternalBufferPropertiesKHR = win32u_vkGetPhysicalDeviceExternalBufferPropertiesKHR,
    .p_vkGetPhysicalDeviceExternalFenceProperties = win32u_vkGetPhysicalDeviceExternalFenceProperties,
    .p_vkGetPhysicalDeviceExternalFencePropertiesKHR = win32u_vkGetPhysicalDeviceExternalFencePropertiesKHR,
    .p_vkGetPhysicalDeviceExternalSemaphoreProperties = win32u_vkGetPhysicalDeviceExternalSemaphoreProperties,
    .p_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR = win32u_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR,
    .p_vkGetPhysicalDeviceImageFormatProperties2 = win32u_vkGetPhysicalDeviceImageFormatProperties2,
    .p_vkGetPhysicalDeviceImageFormatProperties2KHR = win32u_vkGetPhysicalDeviceImageFormatProperties2KHR,
    .p_vkGetPhysicalDevicePresentRectanglesKHR = win32u_vkGetPhysicalDevicePresentRectanglesKHR,
    .p_vkGetPhysicalDeviceProperties = win32u_vkGetPhysicalDeviceProperties,
    .p_vkGetPhysicalDeviceProperties2 = win32u_vkGetPhysicalDeviceProperties2,
    .p_vkGetPhysicalDeviceProperties2KHR = win32u_vkGetPhysicalDeviceProperties2KHR,
    .p_vkGetPhysicalDeviceSurfaceCapabilities2KHR = win32u_vkGetPhysicalDeviceSurfaceCapabilities2KHR,
    .p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = win32u_vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
    .p_vkGetPhysicalDeviceSurfaceFormats2KHR = win32u_vkGetPhysicalDeviceSurfaceFormats2KHR,
    .p_vkGetPhysicalDeviceSurfaceFormatsKHR = win32u_vkGetPhysicalDeviceSurfaceFormatsKHR,
    .p_vkGetPhysicalDeviceSurfaceSupportKHR = win32u_vkGetPhysicalDeviceSurfaceSupportKHR,
    .p_vkGetPhysicalDeviceWin32PresentationSupportKHR = win32u_vkGetPhysicalDeviceWin32PresentationSupportKHR,
    .p_vkGetSemaphoreWin32HandleKHR = win32u_vkGetSemaphoreWin32HandleKHR,
    .p_vkImportFenceWin32HandleKHR = win32u_vkImportFenceWin32HandleKHR,
    .p_vkImportSemaphoreWin32HandleKHR = win32u_vkImportSemaphoreWin32HandleKHR,
    .p_vkMapMemory = win32u_vkMapMemory,
    .p_vkMapMemory2KHR = win32u_vkMapMemory2KHR,
    .p_vkQueuePresentKHR = win32u_vkQueuePresentKHR,
    .p_vkQueueSubmit = win32u_vkQueueSubmit,
    .p_vkQueueSubmit2 = win32u_vkQueueSubmit2,
    .p_vkQueueSubmit2KHR = win32u_vkQueueSubmit2KHR,
    .p_vkUnmapMemory = win32u_vkUnmapMemory,
    .p_vkUnmapMemory2KHR = win32u_vkUnmapMemory2KHR,
};

static VkResult nulldrv_vulkan_surface_create( struct client_surface *client, const struct vulkan_instance *instance, VkSurfaceKHR *surface )
{
    VkHeadlessSurfaceCreateInfoEXT create_info = {.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    return instance->p_vkCreateHeadlessSurfaceEXT( instance->host.instance, &create_info, NULL, surface );
}

static VkBool32 nulldrv_get_physical_device_presentation_support( struct vulkan_physical_device *physical_device, uint32_t queue )
{
    return VK_TRUE;
}

static void nulldrv_map_instance_extensions( struct vulkan_instance_extensions *extensions )
{
    if (extensions->has_VK_KHR_win32_surface) extensions->has_VK_EXT_headless_surface = 1;
    if (extensions->has_VK_EXT_headless_surface) extensions->has_VK_KHR_win32_surface = 1;
}

static void nulldrv_map_device_extensions( struct vulkan_device_extensions *extensions )
{
    if (extensions->has_VK_KHR_external_memory_win32) extensions->has_VK_KHR_external_memory_fd = 1;
    if (extensions->has_VK_KHR_external_memory_fd) extensions->has_VK_KHR_external_memory_win32 = 1;
    if (extensions->has_VK_KHR_external_semaphore_win32) extensions->has_VK_KHR_external_semaphore_fd = 1;
    if (extensions->has_VK_KHR_external_semaphore_fd) extensions->has_VK_KHR_external_semaphore_win32 = 1;
    if (extensions->has_VK_KHR_external_fence_win32) extensions->has_VK_KHR_external_fence_fd = 1;
    if (extensions->has_VK_KHR_external_fence_fd) extensions->has_VK_KHR_external_fence_win32 = 1;
}

static const struct vulkan_driver_funcs nulldrv_funcs =
{
    .p_vulkan_surface_create = nulldrv_vulkan_surface_create,
    .p_get_physical_device_presentation_support = nulldrv_get_physical_device_presentation_support,
    .p_map_instance_extensions = nulldrv_map_instance_extensions,
    .p_map_device_extensions = nulldrv_map_device_extensions,
};

static void vulkan_driver_init(void)
{
    UINT status;

    if ((status = user_driver->pVulkanInit( WINE_VULKAN_DRIVER_VERSION, vulkan_handle, &driver_funcs )) &&
        status != STATUS_NOT_IMPLEMENTED)
    {
        ERR( "Failed to initialize the driver vulkan functions, status %#x\n", status );
        return;
    }

    if (status == STATUS_NOT_IMPLEMENTED) driver_funcs = &nulldrv_funcs;
}

static void vulkan_driver_load(void)
{
    static pthread_once_t init_once = PTHREAD_ONCE_INIT;
    pthread_once( &init_once, vulkan_driver_init );
}

static VkResult lazydrv_vulkan_surface_create( struct client_surface *client, const struct vulkan_instance *instance, VkSurfaceKHR *surface )
{
    vulkan_driver_load();
    return driver_funcs->p_vulkan_surface_create( client, instance, surface );
}

static VkBool32 lazydrv_get_physical_device_presentation_support( struct vulkan_physical_device *physical_device, uint32_t queue )
{
    vulkan_driver_load();
    return driver_funcs->p_get_physical_device_presentation_support( physical_device, queue );
}

static void lazydrv_map_instance_extensions( struct vulkan_instance_extensions *extensions )
{
    vulkan_driver_load();
    return driver_funcs->p_map_instance_extensions( extensions );
}

static void lazydrv_map_device_extensions( struct vulkan_device_extensions *extensions )
{
    vulkan_driver_load();
    return driver_funcs->p_map_device_extensions( extensions );
}

static const struct vulkan_driver_funcs lazydrv_funcs =
{
    .p_vulkan_surface_create = lazydrv_vulkan_surface_create,
    .p_get_physical_device_presentation_support = lazydrv_get_physical_device_presentation_support,
    .p_map_instance_extensions = lazydrv_map_instance_extensions,
    .p_map_device_extensions = lazydrv_map_device_extensions,
};

static void vulkan_init_once(void)
{
    struct vulkan_instance_extensions extensions = {0};
    VkExtensionProperties *properties = NULL;
    uint32_t count = 0;
    VkResult res;

#ifdef SONAME_LIBVULKAN
    vulkan_handle = dlopen( SONAME_LIBVULKAN, RTLD_NOW );
    if (!vulkan_handle) ERR( "Failed to load %s\n", SONAME_LIBVULKAN );
#else
    ERR( "Wine was built without Vulkan support.\n" );
#endif
    if (!vulkan_handle) return;

#define LOAD_FUNCPTR( f )                                                                          \
    if (!(p_##f = dlsym( vulkan_handle, #f )))                                                     \
    {                                                                                              \
        ERR( "Failed to find " #f "\n" );                                                          \
        dlclose( vulkan_handle );                                                                  \
        vulkan_handle = NULL;                                                                      \
        return;                                                                                    \
    }

    LOAD_FUNCPTR( vkGetDeviceProcAddr );
    LOAD_FUNCPTR( vkGetInstanceProcAddr );
#undef LOAD_FUNCPTR

    driver_funcs = &lazydrv_funcs;
    vulkan_funcs.p_vkGetInstanceProcAddr = p_vkGetInstanceProcAddr;
    vulkan_funcs.p_vkGetDeviceProcAddr = p_vkGetDeviceProcAddr;

#define LOAD_FUNCPTR( f ) p_##f = (PFN_##f)p_vkGetInstanceProcAddr( NULL, #f );
    LOAD_FUNCPTR( vkCreateInstance );
    LOAD_FUNCPTR( vkEnumerateInstanceExtensionProperties );
#undef LOAD_FUNCPTR

    do
    {
        free( properties );
        properties = NULL;
        if ((res = p_vkEnumerateInstanceExtensionProperties( NULL, &count, NULL ))) goto failed;
        if (!count || !(properties = malloc( count * sizeof(*properties) ))) goto failed;
    } while ((res = p_vkEnumerateInstanceExtensionProperties( NULL, &count, properties ) == VK_INCOMPLETE));
    if (res) goto failed;

    TRACE( "Host instance extensions:\n" );
    for (uint32_t i = 0; i < count; i++)
    {
        const char *extension = properties[i].extensionName;
#define USE_VK_EXT(x)                           \
        if (!strcmp( extension, #x ))           \
        {                                       \
            extensions.has_ ## x = 1;           \
            TRACE( "  - %s\n", extension );     \
        } else
        ALL_VK_INSTANCE_EXTS
#undef USE_VK_EXT
        WARN( "Extension %s is not supported.\n", debugstr_a(extension) );
    }
    vulkan_funcs.host_extensions = extensions;

    /* map host instance extensions for VK_KHR_win32_surface */
    driver_funcs->p_map_instance_extensions( &extensions );

    /* filter out unsupported client instance extensions */
#define USE_VK_EXT(x) vulkan_funcs.client_extensions.has_ ## x = extensions.has_ ## x;
    ALL_VK_CLIENT_INSTANCE_EXTS
#undef USE_VK_EXT

failed:
    if (res) ERR( "Failed to initialize instance extensions, res %d\n", res );
    free( properties );
}

/***********************************************************************
 *      __wine_get_vulkan_driver  (win32u.so)
 */
const struct vulkan_funcs *__wine_get_vulkan_driver( UINT version )
{
    static pthread_once_t init_once = PTHREAD_ONCE_INIT;

    if (version != WINE_VULKAN_DRIVER_VERSION)
    {
        ERR( "version mismatch, vulkan wants %u but win32u has %u\n", version, WINE_VULKAN_DRIVER_VERSION );
        return NULL;
    }

    pthread_once( &init_once, vulkan_init_once );
    if (!vulkan_handle) return NULL;
    return &vulkan_funcs;
}

/* unix side client-like instance wrapper to fit with the vulkan wrapping infrastructure */
struct instance_wrapper
{
    struct VkInstance_T client;
};

struct vulkan_instance *vulkan_instance_create( const struct vulkan_instance_extensions *extensions )
{
    const struct vulkan_funcs *funcs = __wine_get_vulkan_driver( WINE_VULKAN_DRIVER_VERSION );
    VkInstanceCreateInfo create_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    const char *extension_names[sizeof(*extensions) * 8];
    struct instance_wrapper *wrapper;
    UINT device_count = 8;
    VkResult res;

    if (!funcs) return NULL;

    create_info.ppEnabledExtensionNames = extension_names;
#define USE_VK_EXT(x) if (extensions->has_ ## x) extension_names[create_info.enabledExtensionCount++] = #x;
    ALL_VK_INSTANCE_EXTS
#undef USE_VK_EXT

    for (;;)
    {
        VkInstance instance;

        if (!(wrapper = calloc( 1, offsetof(struct instance_wrapper, client.physical_device[device_count]) ))) return NULL;
        wrapper->client.physical_device_count = device_count;
        wrapper->client.extensions = *extensions;
        instance = &wrapper->client;

        if ((res = funcs->p_vkCreateInstance( &create_info, NULL, &instance ))) break;
        if ((wrapper->client.physical_device_count <= device_count)) break;
        device_count = wrapper->client.physical_device_count;
        free( wrapper );
    }

    if (!res) return vulkan_instance_from_handle( &wrapper->client );
    WARN( "Failed to create instance, res %d\n", res );
    free( wrapper );
    return NULL;
}
