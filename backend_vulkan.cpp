/*
 * Mental - Vulkan Backend (Linux, Windows)
 *
 * The Vulkan loader is opened at runtime via dlopen / LoadLibrary so that
 * the library can be compiled on any platform and will fail gracefully
 * when no Vulkan driver is installed.
 *
 * Loading strategy:
 *   1. dlopen the Vulkan loader library
 *   2. dlsym("vkGetInstanceProcAddr")
 *   3. Use vkGetInstanceProcAddr(NULL, ...) for pre-instance functions
 *   4. Create a VkInstance
 *   5. Use vkGetInstanceProcAddr(instance, ...) for all remaining functions
 */

#ifdef _WIN32
#  include <windows.h>
#  define VK_DLOPEN(path)    ((void*)LoadLibraryA(path))
#  define VK_DLSYM(lib, sym) ((void*)GetProcAddress((HMODULE)(lib), sym))
#  define VK_DLCLOSE(lib)    FreeLibrary((HMODULE)(lib))
#else
#  include <dlfcn.h>
#  define VK_DLOPEN(path)    dlopen(path, RTLD_LAZY)
#  define VK_DLSYM(lib, sym) dlsym(lib, sym)
#  define VK_DLCLOSE(lib)    dlclose(lib)
#endif

/* Vulkan header for types and constants only — no linking required.
 * If the header isn't available (e.g. macOS without Vulkan SDK),
 * the backend exports vulkan_backend = NULL. */
#if __has_include(<vulkan/vulkan.h>)
#define MENTAL_VULKAN_AVAILABLE 1
#include <vulkan/vulkan.h>
#else
#define MENTAL_VULKAN_AVAILABLE 0
#endif

#include "mental_internal.h"

#if MENTAL_VULKAN_AVAILABLE
#include "transpile.h"
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>

/* ------------------------------------------------------------------ */
/*  Resolved Vulkan function pointers                                 */
/* ------------------------------------------------------------------ */

/* The loader entry point — resolved via dlsym */
static PFN_vkGetInstanceProcAddr p_vkGetInstanceProcAddr;

/* Pre-instance functions (resolved with instance = NULL) */
static PFN_vkCreateInstance            p_vkCreateInstance;
static PFN_vkEnumeratePhysicalDevices  p_vkEnumeratePhysicalDevices;

/* Instance-level functions */
static PFN_vkDestroyInstance                         p_vkDestroyInstance;
static PFN_vkGetPhysicalDeviceProperties             p_vkGetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties  p_vkGetPhysicalDeviceQueueFamilyProperties;
static PFN_vkGetPhysicalDeviceMemoryProperties       p_vkGetPhysicalDeviceMemoryProperties;
static PFN_vkCreateDevice                            p_vkCreateDevice;
static PFN_vkGetDeviceProcAddr                       p_vkGetDeviceProcAddr;

/* Device-level functions */
static PFN_vkDestroyDevice               p_vkDestroyDevice;
static PFN_vkGetDeviceQueue              p_vkGetDeviceQueue;
static PFN_vkCreateCommandPool           p_vkCreateCommandPool;
static PFN_vkDestroyCommandPool          p_vkDestroyCommandPool;
static PFN_vkCreateBuffer                p_vkCreateBuffer;
static PFN_vkDestroyBuffer               p_vkDestroyBuffer;
static PFN_vkGetBufferMemoryRequirements p_vkGetBufferMemoryRequirements;
static PFN_vkAllocateMemory              p_vkAllocateMemory;
static PFN_vkFreeMemory                  p_vkFreeMemory;
static PFN_vkBindBufferMemory            p_vkBindBufferMemory;
static PFN_vkMapMemory                   p_vkMapMemory;
static PFN_vkUnmapMemory                 p_vkUnmapMemory;
static PFN_vkCreateShaderModule          p_vkCreateShaderModule;
static PFN_vkDestroyShaderModule         p_vkDestroyShaderModule;
static PFN_vkCreateDescriptorSetLayout   p_vkCreateDescriptorSetLayout;
static PFN_vkDestroyDescriptorSetLayout  p_vkDestroyDescriptorSetLayout;
static PFN_vkCreatePipelineLayout        p_vkCreatePipelineLayout;
static PFN_vkDestroyPipelineLayout       p_vkDestroyPipelineLayout;
static PFN_vkCreateComputePipelines      p_vkCreateComputePipelines;
static PFN_vkDestroyPipeline             p_vkDestroyPipeline;
static PFN_vkCreateDescriptorPool        p_vkCreateDescriptorPool;
static PFN_vkDestroyDescriptorPool       p_vkDestroyDescriptorPool;
static PFN_vkResetDescriptorPool         p_vkResetDescriptorPool;
static PFN_vkAllocateDescriptorSets      p_vkAllocateDescriptorSets;
static PFN_vkUpdateDescriptorSets        p_vkUpdateDescriptorSets;
static PFN_vkAllocateCommandBuffers      p_vkAllocateCommandBuffers;
static PFN_vkFreeCommandBuffers          p_vkFreeCommandBuffers;
static PFN_vkBeginCommandBuffer          p_vkBeginCommandBuffer;
static PFN_vkEndCommandBuffer            p_vkEndCommandBuffer;
static PFN_vkCmdBindPipeline             p_vkCmdBindPipeline;
static PFN_vkCmdBindDescriptorSets       p_vkCmdBindDescriptorSets;
static PFN_vkCmdDispatch                 p_vkCmdDispatch;
static PFN_vkCmdPipelineBarrier          p_vkCmdPipelineBarrier;
static PFN_vkQueueSubmit                 p_vkQueueSubmit;
static PFN_vkQueueWaitIdle               p_vkQueueWaitIdle;

/* Image functions (for headless viewport) */
static PFN_vkCreateImage                 p_vkCreateImage;
static PFN_vkDestroyImage                p_vkDestroyImage;
static PFN_vkGetImageMemoryRequirements  p_vkGetImageMemoryRequirements;
static PFN_vkBindImageMemory             p_vkBindImageMemory;

/* KHR swapchain/surface functions (optional — may be NULL on headless) */
static PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR  p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
static PFN_vkGetPhysicalDeviceSurfaceFormatsKHR       p_vkGetPhysicalDeviceSurfaceFormatsKHR;
static PFN_vkGetPhysicalDeviceSurfacePresentModesKHR  p_vkGetPhysicalDeviceSurfacePresentModesKHR;
static PFN_vkCreateSwapchainKHR                       p_vkCreateSwapchainKHR;
static PFN_vkDestroySwapchainKHR                      p_vkDestroySwapchainKHR;
static PFN_vkGetSwapchainImagesKHR                    p_vkGetSwapchainImagesKHR;
static PFN_vkAcquireNextImageKHR                      p_vkAcquireNextImageKHR;
static PFN_vkQueuePresentKHR                          p_vkQueuePresentKHR;

/* ------------------------------------------------------------------ */
/*  Dynamic library handle                                            */
/* ------------------------------------------------------------------ */

static void* g_vk_lib = NULL;

static void* vk_try_open(const char* name) {
    void* lib = VK_DLOPEN(name);
    return lib;
}

static int load_vulkan_library(void) {
    if (g_vk_lib) return 0;

#ifdef _WIN32
    g_vk_lib = vk_try_open("vulkan-1.dll");
#elif defined(__APPLE__)
    g_vk_lib = vk_try_open("libvulkan.dylib");
    if (!g_vk_lib) g_vk_lib = vk_try_open("libvulkan.1.dylib");
#else
    g_vk_lib = vk_try_open("libvulkan.so.1");
    if (!g_vk_lib) g_vk_lib = vk_try_open("libvulkan.so");
#endif

    if (!g_vk_lib) return -1;

    /* Resolve the single entry point we need from dlsym */
    p_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)VK_DLSYM(g_vk_lib, "vkGetInstanceProcAddr");
    if (!p_vkGetInstanceProcAddr) {
        VK_DLCLOSE(g_vk_lib);
        g_vk_lib = NULL;
        return -1;
    }

    return 0;
}

/* Resolve pre-instance functions (instance = VK_NULL_HANDLE) */
static int resolve_global_functions(void) {
#define VK_LOAD_GLOBAL(fn) do { \
    p_##fn = (PFN_##fn)p_vkGetInstanceProcAddr(VK_NULL_HANDLE, #fn); \
    if (!p_##fn) return -1; \
} while (0)

    VK_LOAD_GLOBAL(vkCreateInstance);
    /* vkEnumeratePhysicalDevices is instance-level, resolved after instance creation */

#undef VK_LOAD_GLOBAL
    return 0;
}

/* Resolve instance-level functions */
static int resolve_instance_functions(VkInstance instance) {
#define VK_LOAD_INSTANCE(fn) do { \
    p_##fn = (PFN_##fn)p_vkGetInstanceProcAddr(instance, #fn); \
    if (!p_##fn) return -1; \
} while (0)

    VK_LOAD_INSTANCE(vkDestroyInstance);
    VK_LOAD_INSTANCE(vkEnumeratePhysicalDevices);
    VK_LOAD_INSTANCE(vkGetPhysicalDeviceProperties);
    VK_LOAD_INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
    VK_LOAD_INSTANCE(vkGetPhysicalDeviceMemoryProperties);
    VK_LOAD_INSTANCE(vkCreateDevice);
    VK_LOAD_INSTANCE(vkGetDeviceProcAddr);

    VK_LOAD_INSTANCE(vkDestroyDevice);
    VK_LOAD_INSTANCE(vkGetDeviceQueue);
    VK_LOAD_INSTANCE(vkCreateCommandPool);
    VK_LOAD_INSTANCE(vkDestroyCommandPool);
    VK_LOAD_INSTANCE(vkCreateBuffer);
    VK_LOAD_INSTANCE(vkDestroyBuffer);
    VK_LOAD_INSTANCE(vkGetBufferMemoryRequirements);
    VK_LOAD_INSTANCE(vkAllocateMemory);
    VK_LOAD_INSTANCE(vkFreeMemory);
    VK_LOAD_INSTANCE(vkBindBufferMemory);
    VK_LOAD_INSTANCE(vkMapMemory);
    VK_LOAD_INSTANCE(vkUnmapMemory);
    VK_LOAD_INSTANCE(vkCreateShaderModule);
    VK_LOAD_INSTANCE(vkDestroyShaderModule);
    VK_LOAD_INSTANCE(vkCreateDescriptorSetLayout);
    VK_LOAD_INSTANCE(vkDestroyDescriptorSetLayout);
    VK_LOAD_INSTANCE(vkCreatePipelineLayout);
    VK_LOAD_INSTANCE(vkDestroyPipelineLayout);
    VK_LOAD_INSTANCE(vkCreateComputePipelines);
    VK_LOAD_INSTANCE(vkDestroyPipeline);
    VK_LOAD_INSTANCE(vkCreateDescriptorPool);
    VK_LOAD_INSTANCE(vkDestroyDescriptorPool);
    VK_LOAD_INSTANCE(vkResetDescriptorPool);
    VK_LOAD_INSTANCE(vkAllocateDescriptorSets);
    VK_LOAD_INSTANCE(vkUpdateDescriptorSets);
    VK_LOAD_INSTANCE(vkAllocateCommandBuffers);
    VK_LOAD_INSTANCE(vkFreeCommandBuffers);
    VK_LOAD_INSTANCE(vkBeginCommandBuffer);
    VK_LOAD_INSTANCE(vkEndCommandBuffer);
    VK_LOAD_INSTANCE(vkCmdBindPipeline);
    VK_LOAD_INSTANCE(vkCmdBindDescriptorSets);
    VK_LOAD_INSTANCE(vkCmdDispatch);
    VK_LOAD_INSTANCE(vkCmdPipelineBarrier);
    VK_LOAD_INSTANCE(vkQueueSubmit);
    VK_LOAD_INSTANCE(vkQueueWaitIdle);

    VK_LOAD_INSTANCE(vkCreateImage);
    VK_LOAD_INSTANCE(vkDestroyImage);
    VK_LOAD_INSTANCE(vkGetImageMemoryRequirements);
    VK_LOAD_INSTANCE(vkBindImageMemory);

#undef VK_LOAD_INSTANCE

    /* KHR swapchain/surface functions are optional — NULL is OK */
    p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
        p_vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    p_vkGetPhysicalDeviceSurfaceFormatsKHR = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)
        p_vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    p_vkGetPhysicalDeviceSurfacePresentModesKHR = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)
        p_vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    p_vkCreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)
        p_vkGetInstanceProcAddr(instance, "vkCreateSwapchainKHR");
    p_vkDestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)
        p_vkGetInstanceProcAddr(instance, "vkDestroySwapchainKHR");
    p_vkGetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)
        p_vkGetInstanceProcAddr(instance, "vkGetSwapchainImagesKHR");
    p_vkAcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)
        p_vkGetInstanceProcAddr(instance, "vkAcquireNextImageKHR");
    p_vkQueuePresentKHR = (PFN_vkQueuePresentKHR)
        p_vkGetInstanceProcAddr(instance, "vkQueuePresentKHR");

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Vulkan device / buffer / kernel wrappers                          */
/* ------------------------------------------------------------------ */

/* Vulkan device wrapper */
typedef struct {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    VkCommandPool command_pool;
    uint32_t queue_family_index;
} VulkanDevice;

/* Vulkan buffer wrapper */
typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* mapped_ptr;
    size_t size;
    VulkanDevice* device_ctx;  /* Full device context for queue/command pool access */
} VulkanBuffer;

/* Vulkan kernel wrapper */
typedef struct {
    VkShaderModule shader_module;
    VkPipeline pipeline;
    VkPipelineLayout pipeline_layout;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorPool descriptor_pool;
    VulkanDevice* device_ctx;  /* Full device context for queue/command pool access */
} VulkanKernel;

/* Vulkan viewport wrapper */
typedef struct {
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VulkanBuffer* buffer;
    VulkanDevice* device_ctx;
    std::vector<VkImage> swapchain_images;
    uint32_t image_index;
    VkImage headless_image;         /* single render target when no swapchain */
    VkDeviceMemory headless_memory;
    int headless;                    /* 1 = no swapchain */
} VulkanViewport;

/* Global Vulkan state */
static VkInstance g_instance = VK_NULL_HANDLE;
static std::vector<VkPhysicalDevice> g_physical_devices;

static int vulkan_init(void) {
    /* Load the Vulkan shared library */
    if (load_vulkan_library() != 0) {
        return -1;
    }

    /* Resolve pre-instance functions */
    if (resolve_global_functions() != 0) {
        VK_DLCLOSE(g_vk_lib);
        g_vk_lib = NULL;
        return -1;
    }

    /* Create instance */
    VkApplicationInfo app_info = {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Mental";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "Mental";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;

    if (p_vkCreateInstance(&create_info, nullptr, &g_instance) != VK_SUCCESS) {
        VK_DLCLOSE(g_vk_lib);
        g_vk_lib = NULL;
        return -1;
    }

    /* Resolve all instance-level functions */
    if (resolve_instance_functions(g_instance) != 0) {
        /* We have vkDestroyInstance if it was resolved before failure,
         * but play it safe and just close the library. */
        if (p_vkDestroyInstance) p_vkDestroyInstance(g_instance, nullptr);
        g_instance = VK_NULL_HANDLE;
        VK_DLCLOSE(g_vk_lib);
        g_vk_lib = NULL;
        return -1;
    }

    /* Enumerate physical devices */
    uint32_t device_count = 0;
    p_vkEnumeratePhysicalDevices(g_instance, &device_count, nullptr);

    if (device_count == 0) {
        p_vkDestroyInstance(g_instance, nullptr);
        g_instance = VK_NULL_HANDLE;
        VK_DLCLOSE(g_vk_lib);
        g_vk_lib = NULL;
        return -1;
    }

    g_physical_devices.resize(device_count);
    p_vkEnumeratePhysicalDevices(g_instance, &device_count, g_physical_devices.data());

    return 0;
}

static void vulkan_shutdown(void) {
    if (g_instance != VK_NULL_HANDLE) {
        p_vkDestroyInstance(g_instance, nullptr);
        g_instance = VK_NULL_HANDLE;
    }
    g_physical_devices.clear();

    if (g_vk_lib) {
        VK_DLCLOSE(g_vk_lib);
        g_vk_lib = NULL;
    }
}

static int vulkan_device_count(void) {
    return (int)g_physical_devices.size();
}

static int vulkan_device_info(int index, char* name, size_t name_len) {
    if (index < 0 || index >= (int)g_physical_devices.size()) return -1;

    VkPhysicalDeviceProperties props;
    p_vkGetPhysicalDeviceProperties(g_physical_devices[index], &props);

    strncpy(name, props.deviceName, name_len - 1);
    name[name_len - 1] = '\0';

    return 0;
}

static uint32_t find_compute_queue_family(VkPhysicalDevice device) {
    uint32_t queue_family_count = 0;
    p_vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, nullptr);

    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    p_vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, queue_families.data());

    for (uint32_t i = 0; i < queue_family_count; i++) {
        if (queue_families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            return i;
        }
    }

    return UINT32_MAX;
}

static void* vulkan_device_create(int index) {
    if (index < 0 || index >= (int)g_physical_devices.size()) return NULL;

    VulkanDevice* dev = new VulkanDevice();
    dev->instance = g_instance;
    dev->physical_device = g_physical_devices[index];

    /* Find compute queue family */
    dev->queue_family_index = find_compute_queue_family(dev->physical_device);
    if (dev->queue_family_index == UINT32_MAX) {
        delete dev;
        return NULL;
    }

    /* Create logical device */
    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_create_info = {};
    queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create_info.queueFamilyIndex = dev->queue_family_index;
    queue_create_info.queueCount = 1;
    queue_create_info.pQueuePriorities = &queue_priority;

    VkDeviceCreateInfo device_create_info = {};
    device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_create_info.queueCreateInfoCount = 1;
    device_create_info.pQueueCreateInfos = &queue_create_info;

    if (p_vkCreateDevice(dev->physical_device, &device_create_info, nullptr, &dev->device) != VK_SUCCESS) {
        delete dev;
        return NULL;
    }

    /* Get queue */
    p_vkGetDeviceQueue(dev->device, dev->queue_family_index, 0, &dev->queue);

    /* Create command pool */
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = dev->queue_family_index;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    if (p_vkCreateCommandPool(dev->device, &pool_info, nullptr, &dev->command_pool) != VK_SUCCESS) {
        p_vkDestroyDevice(dev->device, nullptr);
        delete dev;
        return NULL;
    }

    return dev;
}

static void vulkan_device_destroy(void* dev) {
    if (!dev) return;

    VulkanDevice* vk_dev = (VulkanDevice*)dev;
    p_vkDestroyCommandPool(vk_dev->device, vk_dev->command_pool, nullptr);
    p_vkDestroyDevice(vk_dev->device, nullptr);
    delete vk_dev;
}

static uint32_t find_memory_type(VkPhysicalDevice physical_device, uint32_t type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_properties;
    p_vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_properties);

    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) && (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    return UINT32_MAX;
}

static void* vulkan_buffer_alloc(void* dev, size_t bytes) {
    VulkanDevice* vk_dev = (VulkanDevice*)dev;

    VulkanBuffer* buf = new VulkanBuffer();
    buf->device_ctx = vk_dev;
    buf->size = bytes;

    /* Create buffer */
    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = bytes;
    buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (p_vkCreateBuffer(vk_dev->device, &buffer_info, nullptr, &buf->buffer) != VK_SUCCESS) {
        delete buf;
        return NULL;
    }

    /* Allocate memory */
    VkMemoryRequirements mem_requirements;
    p_vkGetBufferMemoryRequirements(vk_dev->device, buf->buffer, &mem_requirements);

    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_requirements.size;
    alloc_info.memoryTypeIndex = find_memory_type(vk_dev->physical_device, mem_requirements.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (alloc_info.memoryTypeIndex == UINT32_MAX) {
        p_vkDestroyBuffer(vk_dev->device, buf->buffer, nullptr);
        delete buf;
        return NULL;
    }

    if (p_vkAllocateMemory(vk_dev->device, &alloc_info, nullptr, &buf->memory) != VK_SUCCESS) {
        p_vkDestroyBuffer(vk_dev->device, buf->buffer, nullptr);
        delete buf;
        return NULL;
    }

    p_vkBindBufferMemory(vk_dev->device, buf->buffer, buf->memory, 0);

    /* Map memory persistently */
    if (p_vkMapMemory(vk_dev->device, buf->memory, 0, bytes, 0, &buf->mapped_ptr) != VK_SUCCESS) {
        p_vkFreeMemory(vk_dev->device, buf->memory, nullptr);
        p_vkDestroyBuffer(vk_dev->device, buf->buffer, nullptr);
        delete buf;
        return NULL;
    }

    return buf;
}

static void vulkan_buffer_write(void* buf, const void* data, size_t bytes) {
    VulkanBuffer* vk_buf = (VulkanBuffer*)buf;
    memcpy(vk_buf->mapped_ptr, data, bytes);
}

static void vulkan_buffer_read(void* buf, void* data, size_t bytes) {
    VulkanBuffer* vk_buf = (VulkanBuffer*)buf;
    if (!vk_buf || !vk_buf->mapped_ptr) return;
    memcpy(data, vk_buf->mapped_ptr, bytes);
}

static void* vulkan_buffer_resize(void* dev, void* old_buf, size_t old_size, size_t new_size) {
    VulkanDevice* vk_dev = (VulkanDevice*)dev;
    VulkanBuffer* old_vk_buf = (VulkanBuffer*)old_buf;

    /* Allocate new buffer */
    VulkanBuffer* new_buf = (VulkanBuffer*)vulkan_buffer_alloc(dev, new_size);
    if (!new_buf) return NULL;

    /* Copy old data */
    size_t copy_size = old_size < new_size ? old_size : new_size;
    memcpy(new_buf->mapped_ptr, old_vk_buf->mapped_ptr, copy_size);

    /* Destroy old buffer */
    p_vkUnmapMemory(old_vk_buf->device_ctx->device, old_vk_buf->memory);
    p_vkDestroyBuffer(old_vk_buf->device_ctx->device, old_vk_buf->buffer, nullptr);
    p_vkFreeMemory(old_vk_buf->device_ctx->device, old_vk_buf->memory, nullptr);
    delete old_vk_buf;

    return new_buf;
}

static void* vulkan_buffer_clone(void* dev, void* src_buf, size_t size) {
    VulkanDevice* vk_dev = (VulkanDevice*)dev;
    VulkanBuffer* src_vk_buf = (VulkanBuffer*)src_buf;

    /* Allocate new buffer */
    VulkanBuffer* clone_buf = (VulkanBuffer*)vulkan_buffer_alloc(dev, size);
    if (!clone_buf) return NULL;

    /* Copy data from source buffer */
    memcpy(clone_buf->mapped_ptr, src_vk_buf->mapped_ptr, size);

    return clone_buf;
}

static void vulkan_buffer_destroy(void* buf) {
    if (!buf) return;

    VulkanBuffer* vk_buf = (VulkanBuffer*)buf;
    p_vkUnmapMemory(vk_buf->device_ctx->device, vk_buf->memory);
    p_vkDestroyBuffer(vk_buf->device_ctx->device, vk_buf->buffer, nullptr);
    p_vkFreeMemory(vk_buf->device_ctx->device, vk_buf->memory, nullptr);
    delete vk_buf;
}

static void* vulkan_kernel_compile(void* dev, const char* source, size_t source_len,
                                    char* error, size_t error_len) {
    VulkanDevice* vk_dev = (VulkanDevice*)dev;

    /* The source arrives as GLSL text (transpiled by mental_compile).
     * Vulkan needs SPIR-V binary, so compile it first via glslang. */
    const uint32_t* spirv_data;
    size_t spirv_len;
    unsigned char* spirv_buf = nullptr;

    /* Check if already SPIR-V (magic number 0x07230203) */
    if (source_len >= 4 &&
        (unsigned char)source[0] == 0x03 && (unsigned char)source[1] == 0x02 &&
        (unsigned char)source[2] == 0x23 && (unsigned char)source[3] == 0x07) {
        spirv_data = (const uint32_t*)source;
        spirv_len = source_len;
    } else {
        /* Compile GLSL to SPIR-V */
        spirv_buf = mental_glsl_to_spirv(source, source_len, &spirv_len, error, error_len);
        if (!spirv_buf) {
            return NULL;
        }
        spirv_data = (const uint32_t*)spirv_buf;
    }


    VulkanKernel* kernel = new VulkanKernel();
    kernel->device_ctx = vk_dev;

    /* Create shader module from SPIR-V */
    VkShaderModuleCreateInfo module_info = {};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = spirv_len;
    module_info.pCode = spirv_data;

    if (p_vkCreateShaderModule(vk_dev->device, &module_info, nullptr, &kernel->shader_module) != VK_SUCCESS) {
        if (error) {
            snprintf(error, error_len, "Failed to create shader module");
        }
        free(spirv_buf);
        delete kernel;
        return NULL;
    }

    free(spirv_buf); /* No longer needed after module creation */

    /* Create descriptor set layout for storage buffers
     * Support up to 16 buffers (inputs + outputs) */
    std::vector<VkDescriptorSetLayoutBinding> bindings(16);
    for (int i = 0; i < 16; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[i].pImmutableSamplers = nullptr;
    }

    VkDescriptorSetLayoutCreateInfo layout_info = {};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 16;
    layout_info.pBindings = bindings.data();

    if (p_vkCreateDescriptorSetLayout(vk_dev->device, &layout_info, nullptr,
                                     &kernel->descriptor_set_layout) != VK_SUCCESS) {
        p_vkDestroyShaderModule(vk_dev->device, kernel->shader_module, nullptr);
        if (error) {
            snprintf(error, error_len, "Failed to create descriptor set layout");
        }
        delete kernel;
        return NULL;
    }

    /* Create pipeline layout */
    VkPipelineLayoutCreateInfo pipeline_layout_info = {};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &kernel->descriptor_set_layout;

    if (p_vkCreatePipelineLayout(vk_dev->device, &pipeline_layout_info, nullptr,
                                &kernel->pipeline_layout) != VK_SUCCESS) {
        p_vkDestroyDescriptorSetLayout(vk_dev->device, kernel->descriptor_set_layout, nullptr);
        p_vkDestroyShaderModule(vk_dev->device, kernel->shader_module, nullptr);
        if (error) {
            snprintf(error, error_len, "Failed to create pipeline layout");
        }
        delete kernel;
        return NULL;
    }

    /* Create compute pipeline */
    VkPipelineShaderStageCreateInfo shader_stage_info = {};
    shader_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shader_stage_info.module = kernel->shader_module;
    shader_stage_info.pName = "main";

    VkComputePipelineCreateInfo pipeline_info = {};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage = shader_stage_info;
    pipeline_info.layout = kernel->pipeline_layout;

    if (p_vkCreateComputePipelines(vk_dev->device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                  &kernel->pipeline) != VK_SUCCESS) {
        p_vkDestroyPipelineLayout(vk_dev->device, kernel->pipeline_layout, nullptr);
        p_vkDestroyDescriptorSetLayout(vk_dev->device, kernel->descriptor_set_layout, nullptr);
        p_vkDestroyShaderModule(vk_dev->device, kernel->shader_module, nullptr);
        if (error) {
            snprintf(error, error_len, "Failed to create compute pipeline");
        }
        delete kernel;
        return NULL;
    }

    /* Create descriptor pool */
    VkDescriptorPoolSize pool_size = {};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 16;

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    pool_info.maxSets = 1;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    if (p_vkCreateDescriptorPool(vk_dev->device, &pool_info, nullptr,
                                &kernel->descriptor_pool) != VK_SUCCESS) {
        p_vkDestroyPipeline(vk_dev->device, kernel->pipeline, nullptr);
        p_vkDestroyPipelineLayout(vk_dev->device, kernel->pipeline_layout, nullptr);
        p_vkDestroyDescriptorSetLayout(vk_dev->device, kernel->descriptor_set_layout, nullptr);
        p_vkDestroyShaderModule(vk_dev->device, kernel->shader_module, nullptr);
        if (error) {
            snprintf(error, error_len, "Failed to create descriptor pool");
        }
        delete kernel;
        return NULL;
    }

    return kernel;
}

static int vulkan_kernel_workgroup_size(void* kernel) {
    (void)kernel;
    /* Vulkan doesn't easily expose local_size_x from a compiled pipeline
     * without SPIR-V reflection.  Return the default that shaders use. */
    return 256;
}

static void vulkan_kernel_dispatch(void* kernel, void** inputs, int input_count,
                                    void** outputs, int output_count, int work_size) {
    if (!kernel) return;

    VulkanKernel* vk_kernel = (VulkanKernel*)kernel;
    VulkanDevice* vk_dev = vk_kernel->device_ctx;

    /* Allocate descriptor set */
    VkDescriptorSet descriptor_set;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = vk_kernel->descriptor_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &vk_kernel->descriptor_set_layout;

    if (p_vkAllocateDescriptorSets(vk_dev->device, &alloc_info, &descriptor_set) != VK_SUCCESS) {
        return;
    }


    /* Build buffer info array first (fixed size), then build descriptor
     * writes pointing into it.  IMPORTANT: we must not push_back into
     * buffer_infos after taking pointers — vector reallocation would
     * invalidate them. Pre-allocate the full size. */
    int total_buffers = input_count + output_count;
    std::vector<VkDescriptorBufferInfo> buffer_infos(total_buffers);
    std::vector<VkWriteDescriptorSet> descriptor_writes(total_buffers);

    for (int i = 0; i < input_count; i++) {
        VulkanBuffer* input_buf = (VulkanBuffer*)inputs[i];
        buffer_infos[i] = {};
        buffer_infos[i].buffer = input_buf->buffer;
        buffer_infos[i].offset = 0;
        buffer_infos[i].range = input_buf->size;

        descriptor_writes[i] = {};
        descriptor_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptor_writes[i].dstSet = descriptor_set;
        descriptor_writes[i].dstBinding = i;
        descriptor_writes[i].dstArrayElement = 0;
        descriptor_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        descriptor_writes[i].descriptorCount = 1;
        descriptor_writes[i].pBufferInfo = &buffer_infos[i];
    }

    /* Output buffers */
    for (int i = 0; i < output_count; i++) {
        int idx = input_count + i;
        VulkanBuffer* output_buf = (VulkanBuffer*)outputs[i];
        buffer_infos[idx] = {};
        buffer_infos[idx].buffer = output_buf->buffer;
        buffer_infos[idx].offset = 0;
        buffer_infos[idx].range = output_buf->size;

        descriptor_writes[idx] = {};
        descriptor_writes[idx].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptor_writes[idx].dstSet = descriptor_set;
        descriptor_writes[idx].dstBinding = idx;
        descriptor_writes[idx].dstArrayElement = 0;
        descriptor_writes[idx].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        descriptor_writes[idx].descriptorCount = 1;
        descriptor_writes[idx].pBufferInfo = &buffer_infos[idx];
    }

    p_vkUpdateDescriptorSets(vk_dev->device, (uint32_t)descriptor_writes.size(),
                           descriptor_writes.data(), 0, nullptr);

    /* Allocate command buffer */
    VkCommandBufferAllocateInfo cmd_buf_alloc_info = {};
    cmd_buf_alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_buf_alloc_info.commandPool = vk_dev->command_pool;
    cmd_buf_alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_buf_alloc_info.commandBufferCount = 1;

    VkCommandBuffer command_buffer;
    if (p_vkAllocateCommandBuffers(vk_dev->device, &cmd_buf_alloc_info, &command_buffer) != VK_SUCCESS) {
        return;
    }

    /* Record command buffer */
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    p_vkBeginCommandBuffer(command_buffer, &begin_info);
    p_vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, vk_kernel->pipeline);
    p_vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            vk_kernel->pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);
    /* work_size is the total number of invocations. The shader's
     * local_size_x defines the workgroup size (typically 256).
     * vkCmdDispatch takes the number of workgroups, not invocations. */
    uint32_t local_size = (uint32_t)vulkan_kernel_workgroup_size(kernel);
    uint32_t num_groups = ((uint32_t)work_size + local_size - 1) / local_size;
    p_vkCmdDispatch(command_buffer, num_groups, 1, 1);
    p_vkEndCommandBuffer(command_buffer);

    /* Submit command buffer */
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;

    if (p_vkQueueSubmit(vk_dev->queue, 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS) {
        p_vkFreeCommandBuffers(vk_dev->device, vk_dev->command_pool, 1, &command_buffer);
        return;
    }

    p_vkQueueWaitIdle(vk_dev->queue);

    /* Free command buffer */
    p_vkFreeCommandBuffers(vk_dev->device, vk_dev->command_pool, 1, &command_buffer);

    /* Reset descriptor pool for next dispatch */
    p_vkResetDescriptorPool(vk_dev->device, vk_kernel->descriptor_pool, 0);
}

static void vulkan_kernel_destroy(void* kernel) {
    if (!kernel) return;

    VulkanKernel* vk_kernel = (VulkanKernel*)kernel;
    VkDevice device = vk_kernel->device_ctx->device;

    if (vk_kernel->descriptor_pool != VK_NULL_HANDLE) {
        p_vkDestroyDescriptorPool(device, vk_kernel->descriptor_pool, nullptr);
    }
    if (vk_kernel->pipeline != VK_NULL_HANDLE) {
        p_vkDestroyPipeline(device, vk_kernel->pipeline, nullptr);
    }
    if (vk_kernel->pipeline_layout != VK_NULL_HANDLE) {
        p_vkDestroyPipelineLayout(device, vk_kernel->pipeline_layout, nullptr);
    }
    if (vk_kernel->descriptor_set_layout != VK_NULL_HANDLE) {
        p_vkDestroyDescriptorSetLayout(device, vk_kernel->descriptor_set_layout, nullptr);
    }
    if (vk_kernel->shader_module != VK_NULL_HANDLE) {
        p_vkDestroyShaderModule(device, vk_kernel->shader_module, nullptr);
    }
    delete vk_kernel;
}

/* Viewport operations */
static void* vulkan_viewport_attach(void* dev, void* buffer, void* surface, char* error, size_t error_len) {
    VulkanDevice* vk_dev = (VulkanDevice*)dev;
    VulkanBuffer* vk_buf = (VulkanBuffer*)buffer;

    /* Surface should be VkSurfaceKHR - user must create this via platform-specific extensions */
    VkSurfaceKHR vk_surface = (VkSurfaceKHR)surface;

    if (vk_surface == VK_NULL_HANDLE) {
        if (error) {
            snprintf(error, error_len, "Invalid VkSurfaceKHR");
        }
        return NULL;
    }

    /* Check if swapchain functions are available.
     * On headless systems (lavapipe without display), the VK_KHR_swapchain
     * extension may not be loaded, leaving function pointers NULL. */
    int has_swapchain = (p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR != NULL &&
                         p_vkCreateSwapchainKHR != NULL);

    VulkanViewport* viewport = new VulkanViewport();
    viewport->surface = vk_surface;
    viewport->buffer = vk_buf;
    viewport->device_ctx = vk_dev;
    viewport->image_index = 0;
    viewport->headless = 0;
    viewport->headless_image = VK_NULL_HANDLE;
    viewport->headless_memory = VK_NULL_HANDLE;

    if (!has_swapchain) {
        /* No swapchain support — go straight to headless */
        viewport->swapchain = VK_NULL_HANDLE;
        viewport->headless = 1;
        return viewport;
    }

    /* Query surface capabilities — if this fails, fall back to headless */
    VkSurfaceCapabilitiesKHR capabilities = {};
    if (p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_dev->physical_device, vk_surface, &capabilities) != VK_SUCCESS) {
        viewport->swapchain = VK_NULL_HANDLE;
        viewport->headless = 1;
        return viewport;
    }

    /* Query surface formats */
    uint32_t format_count = 0;
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(vk_dev->physical_device, vk_surface, &format_count, nullptr);
    if (format_count == 0) {
        viewport->swapchain = VK_NULL_HANDLE;
        viewport->headless = 1;
        return viewport;
    }
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(vk_dev->physical_device, vk_surface, &format_count, formats.data());

    /* Choose format - prefer BGRA8 UNORM */
    VkSurfaceFormatKHR surface_format = formats[0];
    for (const auto& fmt : formats) {
        if (fmt.format == VK_FORMAT_B8G8R8A8_UNORM) {
            surface_format = fmt;
            break;
        }
    }

    /* Query present modes */
    uint32_t present_mode_count;
    p_vkGetPhysicalDeviceSurfacePresentModesKHR(vk_dev->physical_device, vk_surface, &present_mode_count, nullptr);
    std::vector<VkPresentModeKHR> present_modes(present_mode_count);
    p_vkGetPhysicalDeviceSurfacePresentModesKHR(vk_dev->physical_device, vk_surface, &present_mode_count, present_modes.data());

    /* Create swapchain */
    VkSwapchainCreateInfoKHR swapchain_info = {};
    swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchain_info.surface = vk_surface;
    swapchain_info.minImageCount = capabilities.minImageCount;
    swapchain_info.imageFormat = surface_format.format;
    swapchain_info.imageColorSpace = surface_format.colorSpace;
    swapchain_info.imageExtent = capabilities.currentExtent;
    swapchain_info.imageArrayLayers = 1;
    swapchain_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_info.preTransform = capabilities.currentTransform;
    swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchain_info.clipped = VK_TRUE;

    /* Try to create swapchain for double-buffered present */
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkResult sc_result = p_vkCreateSwapchainKHR(vk_dev->device, &swapchain_info, nullptr, &swapchain);

    if (sc_result == VK_SUCCESS && swapchain != VK_NULL_HANDLE) {
        /* Double-buffered path */
        viewport->swapchain = swapchain;

        uint32_t image_count;
        p_vkGetSwapchainImagesKHR(vk_dev->device, swapchain, &image_count, nullptr);
        viewport->swapchain_images.resize(image_count);
        p_vkGetSwapchainImagesKHR(vk_dev->device, swapchain, &image_count, viewport->swapchain_images.data());
    } else {
        /* Headless fallback: create a single image as render target.
         * Present becomes a copy-only operation (no display output). */
        viewport->swapchain = VK_NULL_HANDLE;
        viewport->headless = 1;

        VkImageCreateInfo img_info = {};
        img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_info.imageType = VK_IMAGE_TYPE_2D;
        img_info.format = VK_FORMAT_B8G8R8A8_UNORM;
        img_info.extent.width = capabilities.currentExtent.width > 0 ? capabilities.currentExtent.width : 1;
        img_info.extent.height = capabilities.currentExtent.height > 0 ? capabilities.currentExtent.height : 1;
        img_info.extent.depth = 1;
        img_info.mipLevels = 1;
        img_info.arrayLayers = 1;
        img_info.samples = VK_SAMPLE_COUNT_1_BIT;
        img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        img_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (p_vkCreateImage(vk_dev->device, &img_info, nullptr, &viewport->headless_image) != VK_SUCCESS) {
            if (error) snprintf(error, error_len, "Failed to create headless render target");
            delete viewport;
            return NULL;
        }

        VkMemoryRequirements mem_req;
        p_vkGetImageMemoryRequirements(vk_dev->device, viewport->headless_image, &mem_req);

        VkMemoryAllocateInfo mem_alloc = {};
        mem_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mem_alloc.allocationSize = mem_req.size;
        mem_alloc.memoryTypeIndex = find_memory_type(vk_dev->physical_device, mem_req.memoryTypeBits,
                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (p_vkAllocateMemory(vk_dev->device, &mem_alloc, nullptr, &viewport->headless_memory) != VK_SUCCESS) {
            p_vkDestroyImage(vk_dev->device, viewport->headless_image, nullptr);
            if (error) snprintf(error, error_len, "Failed to allocate headless image memory");
            delete viewport;
            return NULL;
        }

        p_vkBindImageMemory(vk_dev->device, viewport->headless_image, viewport->headless_memory, 0);
    }

    return viewport;
}

static void vulkan_viewport_present(void* viewport_ptr) {
    if (!viewport_ptr) return;

    VulkanViewport* viewport = (VulkanViewport*)viewport_ptr;
    VulkanDevice* vk_dev = viewport->device_ctx;

    if (viewport->headless) {
        /* Headless: just wait for any pending work to complete.
         * The data is in the buffer — no display output needed. */
        p_vkQueueWaitIdle(vk_dev->queue);
        return;
    }

    /* Double-buffered path: acquire, present */
    p_vkAcquireNextImageKHR(vk_dev->device, viewport->swapchain, UINT64_MAX,
                          VK_NULL_HANDLE, VK_NULL_HANDLE, &viewport->image_index);

    VkPresentInfoKHR present_info = {};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &viewport->swapchain;
    present_info.pImageIndices = &viewport->image_index;

    p_vkQueuePresentKHR(vk_dev->queue, &present_info);
    p_vkQueueWaitIdle(vk_dev->queue);
}

static void vulkan_viewport_detach(void* viewport_ptr) {
    if (!viewport_ptr) return;

    VulkanViewport* viewport = (VulkanViewport*)viewport_ptr;
    VkDevice device = viewport->device_ctx->device;

    if (viewport->swapchain != VK_NULL_HANDLE) {
        p_vkDestroySwapchainKHR(device, viewport->swapchain, nullptr);
    }
    if (viewport->headless_image != VK_NULL_HANDLE) {
        p_vkDestroyImage(device, viewport->headless_image, nullptr);
    }
    if (viewport->headless_memory != VK_NULL_HANDLE) {
        p_vkFreeMemory(device, viewport->headless_memory, nullptr);
    }

    delete viewport;
}

/* ── Pipe ──────────────────────────────────────────────────────── */

typedef struct {
    VulkanDevice* device_ctx;
    VkCommandBuffer command_buffer;
} VulkanPipe;

static void* vulkan_pipe_create(void* dev) {
    VulkanDevice* vk_dev = (VulkanDevice*)dev;

    VulkanPipe* pipe = new VulkanPipe();
    if (!pipe) return NULL;
    pipe->device_ctx = vk_dev;

    /* Allocate command buffer */
    VkCommandBufferAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = vk_dev->command_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;

    if (p_vkAllocateCommandBuffers(vk_dev->device, &alloc_info, &pipe->command_buffer) != VK_SUCCESS) {
        delete pipe;
        return NULL;
    }

    /* Begin recording */
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (p_vkBeginCommandBuffer(pipe->command_buffer, &begin_info) != VK_SUCCESS) {
        p_vkFreeCommandBuffers(vk_dev->device, vk_dev->command_pool, 1, &pipe->command_buffer);
        delete pipe;
        return NULL;
    }

    return pipe;
}

static int vulkan_pipe_add(void* pipe_ptr, void* kernel, void** inputs,
                            int input_count, void** outputs, int output_count,
                            int work_size) {
    VulkanPipe* pipe = (VulkanPipe*)pipe_ptr;
    VulkanKernel* vk_kernel = (VulkanKernel*)kernel;
    VulkanDevice* vk_dev = pipe->device_ctx;

    /* Allocate descriptor set */
    VkDescriptorSet descriptor_set;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = vk_kernel->descriptor_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &vk_kernel->descriptor_set_layout;

    if (p_vkAllocateDescriptorSets(vk_dev->device, &alloc_info, &descriptor_set) != VK_SUCCESS) {
        return -1;
    }

    /* Build buffer info and descriptor writes */
    int total_buffers = input_count + output_count;
    std::vector<VkDescriptorBufferInfo> buffer_infos(total_buffers);
    std::vector<VkWriteDescriptorSet> descriptor_writes(total_buffers);

    for (int i = 0; i < input_count; i++) {
        VulkanBuffer* input_buf = (VulkanBuffer*)inputs[i];
        buffer_infos[i] = {};
        buffer_infos[i].buffer = input_buf->buffer;
        buffer_infos[i].offset = 0;
        buffer_infos[i].range = input_buf->size;

        descriptor_writes[i] = {};
        descriptor_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptor_writes[i].dstSet = descriptor_set;
        descriptor_writes[i].dstBinding = i;
        descriptor_writes[i].dstArrayElement = 0;
        descriptor_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        descriptor_writes[i].descriptorCount = 1;
        descriptor_writes[i].pBufferInfo = &buffer_infos[i];
    }

    for (int i = 0; i < output_count; i++) {
        int idx = input_count + i;
        VulkanBuffer* output_buf = (VulkanBuffer*)outputs[i];
        buffer_infos[idx] = {};
        buffer_infos[idx].buffer = output_buf->buffer;
        buffer_infos[idx].offset = 0;
        buffer_infos[idx].range = output_buf->size;

        descriptor_writes[idx] = {};
        descriptor_writes[idx].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptor_writes[idx].dstSet = descriptor_set;
        descriptor_writes[idx].dstBinding = idx;
        descriptor_writes[idx].dstArrayElement = 0;
        descriptor_writes[idx].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        descriptor_writes[idx].descriptorCount = 1;
        descriptor_writes[idx].pBufferInfo = &buffer_infos[idx];
    }

    p_vkUpdateDescriptorSets(vk_dev->device, (uint32_t)descriptor_writes.size(),
                           descriptor_writes.data(), 0, nullptr);

    /* Insert pipeline barrier between dispatches */
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    p_vkCmdPipelineBarrier(pipe->command_buffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);

    /* Bind pipeline and descriptor set, then dispatch */
    p_vkCmdBindPipeline(pipe->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, vk_kernel->pipeline);
    p_vkCmdBindDescriptorSets(pipe->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            vk_kernel->pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);

    uint32_t local_size = (uint32_t)vulkan_kernel_workgroup_size(kernel);
    uint32_t num_groups = ((uint32_t)work_size + local_size - 1) / local_size;
    p_vkCmdDispatch(pipe->command_buffer, num_groups, 1, 1);

    return 0;
}

static int vulkan_pipe_execute(void* pipe_ptr) {
    VulkanPipe* pipe = (VulkanPipe*)pipe_ptr;
    VulkanDevice* vk_dev = pipe->device_ctx;

    p_vkEndCommandBuffer(pipe->command_buffer);

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &pipe->command_buffer;

    if (p_vkQueueSubmit(vk_dev->queue, 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS) {
        return -1;
    }

    p_vkQueueWaitIdle(vk_dev->queue);
    return 0;
}

static void vulkan_pipe_destroy(void* pipe_ptr) {
    if (!pipe_ptr) return;
    VulkanPipe* pipe = (VulkanPipe*)pipe_ptr;
    p_vkFreeCommandBuffers(pipe->device_ctx->device, pipe->device_ctx->command_pool,
                         1, &pipe->command_buffer);
    delete pipe;
}

/* Backend implementation */
static mental_backend g_vulkan_backend = {
    .name = "Vulkan",
    .api = MENTAL_API_VULKAN,
    .init = vulkan_init,
    .shutdown = vulkan_shutdown,
    .device_count = vulkan_device_count,
    .device_info = vulkan_device_info,
    .device_create = vulkan_device_create,
    .device_destroy = vulkan_device_destroy,
    .buffer_alloc = vulkan_buffer_alloc,
    .buffer_write = vulkan_buffer_write,
    .buffer_read = vulkan_buffer_read,
    .buffer_resize = vulkan_buffer_resize,
    .buffer_clone = vulkan_buffer_clone,
    .buffer_destroy = vulkan_buffer_destroy,
    .kernel_compile = vulkan_kernel_compile,
    .kernel_workgroup_size = vulkan_kernel_workgroup_size,
    .kernel_dispatch = vulkan_kernel_dispatch,
    .kernel_destroy = vulkan_kernel_destroy,
    .pipe_create = vulkan_pipe_create,
    .pipe_add = vulkan_pipe_add,
    .pipe_execute = vulkan_pipe_execute,
    .pipe_destroy = vulkan_pipe_destroy,
    .viewport_attach = vulkan_viewport_attach,
    .viewport_present = vulkan_viewport_present,
    .viewport_detach = vulkan_viewport_detach
};

extern "C" {
    mental_backend* vulkan_backend = &g_vulkan_backend;
}

#else /* !MENTAL_VULKAN_AVAILABLE */

extern "C" {
    mental_backend* vulkan_backend = NULL;
}

#endif /* MENTAL_VULKAN_AVAILABLE */
