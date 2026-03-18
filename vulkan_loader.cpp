//go:build linux

#include "vulkan_loader.h"
#include <dlfcn.h>
#include <cstring>
#include <cstdio>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

// Global state
static void* vulkan_lib = nullptr;
static int vulkan_available = 0;
static char error_message[512] = {0};

// Vulkan function pointers - only the ones we actually use
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
PFN_vkCreateInstance vkCreateInstance = nullptr;
PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
PFN_vkCreateDevice vkCreateDevice = nullptr;
PFN_vkDestroyDevice vkDestroyDevice = nullptr;
PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
PFN_vkCreateBuffer vkCreateBuffer = nullptr;
PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
PFN_vkAllocateMemory vkAllocateMemory = nullptr;
PFN_vkFreeMemory vkFreeMemory = nullptr;
PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
PFN_vkMapMemory vkMapMemory = nullptr;
PFN_vkUnmapMemory vkUnmapMemory = nullptr;
PFN_vkCreateShaderModule vkCreateShaderModule = nullptr;
PFN_vkDestroyShaderModule vkDestroyShaderModule = nullptr;
PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = nullptr;
PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout = nullptr;
PFN_vkCreatePipelineLayout vkCreatePipelineLayout = nullptr;
PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = nullptr;
PFN_vkCreateComputePipelines vkCreateComputePipelines = nullptr;
PFN_vkDestroyPipeline vkDestroyPipeline = nullptr;
PFN_vkCreateDescriptorPool vkCreateDescriptorPool = nullptr;
PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool = nullptr;
PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets = nullptr;
PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets = nullptr;
PFN_vkResetDescriptorPool vkResetDescriptorPool = nullptr;
PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
PFN_vkFreeCommandBuffers vkFreeCommandBuffers = nullptr;
PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
PFN_vkCmdBindPipeline vkCmdBindPipeline = nullptr;
PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets = nullptr;
PFN_vkCmdDispatch vkCmdDispatch = nullptr;
PFN_vkQueueSubmit vkQueueSubmit = nullptr;
PFN_vkQueueWaitIdle vkQueueWaitIdle = nullptr;

extern "C" {

int vulkan_loader_init() {
    if (vulkan_available) {
        return 1; // Already initialized
    }

    // Try to load libvulkan.so with various version suffixes
    const char* lib_names[] = {
        "libvulkan.so.1",
        "libvulkan.so",
        nullptr
    };

    for (int i = 0; lib_names[i] != nullptr; i++) {
        vulkan_lib = dlopen(lib_names[i], RTLD_NOW | RTLD_LOCAL);
        if (vulkan_lib != nullptr) {
            break;
        }
    }

    if (vulkan_lib == nullptr) {
        snprintf(error_message, sizeof(error_message),
                 "Vulkan loader not found. Please install vulkan-loader package:\n"
                 "  Ubuntu/Debian: sudo apt install libvulkan1\n"
                 "  Fedora/RHEL: sudo dnf install vulkan-loader\n"
                 "  Arch: sudo pacman -S vulkan-icd-loader");
        return 0;
    }

    // Get vkGetInstanceProcAddr - this is the only function we get from the library
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(vulkan_lib, "vkGetInstanceProcAddr");
    if (vkGetInstanceProcAddr == nullptr) {
        snprintf(error_message, sizeof(error_message),
                 "Failed to load vkGetInstanceProcAddr from Vulkan library");
        dlclose(vulkan_lib);
        vulkan_lib = nullptr;
        return 0;
    }

    // Load global-level functions (no instance needed)
    vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(nullptr, "vkCreateInstance");
    vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)vkGetInstanceProcAddr(nullptr, "vkEnumeratePhysicalDevices");

    if (vkCreateInstance == nullptr || vkEnumeratePhysicalDevices == nullptr) {
        snprintf(error_message, sizeof(error_message),
                 "Failed to load required Vulkan functions");
        dlclose(vulkan_lib);
        vulkan_lib = nullptr;
        return 0;
    }

    // These functions will be loaded after instance creation, but we can try to get their addresses now
    // Note: Proper Vulkan usage would get these via vkGetInstanceProcAddr after creating an instance
    #define LOAD_VK_FUNC(name) \
        name = (PFN_##name)vkGetInstanceProcAddr(nullptr, #name); \
        if (name == nullptr) { \
            snprintf(error_message, sizeof(error_message), "Failed to load " #name); \
            dlclose(vulkan_lib); \
            vulkan_lib = nullptr; \
            return 0; \
        }

    LOAD_VK_FUNC(vkGetPhysicalDeviceProperties)
    LOAD_VK_FUNC(vkGetPhysicalDeviceQueueFamilyProperties)
    LOAD_VK_FUNC(vkCreateDevice)
    LOAD_VK_FUNC(vkDestroyDevice)
    LOAD_VK_FUNC(vkGetDeviceQueue)
    LOAD_VK_FUNC(vkCreateCommandPool)
    LOAD_VK_FUNC(vkDestroyCommandPool)
    LOAD_VK_FUNC(vkCreateBuffer)
    LOAD_VK_FUNC(vkDestroyBuffer)
    LOAD_VK_FUNC(vkGetBufferMemoryRequirements)
    LOAD_VK_FUNC(vkGetPhysicalDeviceMemoryProperties)
    LOAD_VK_FUNC(vkAllocateMemory)
    LOAD_VK_FUNC(vkFreeMemory)
    LOAD_VK_FUNC(vkBindBufferMemory)
    LOAD_VK_FUNC(vkMapMemory)
    LOAD_VK_FUNC(vkUnmapMemory)
    LOAD_VK_FUNC(vkCreateShaderModule)
    LOAD_VK_FUNC(vkDestroyShaderModule)
    LOAD_VK_FUNC(vkCreateDescriptorSetLayout)
    LOAD_VK_FUNC(vkDestroyDescriptorSetLayout)
    LOAD_VK_FUNC(vkCreatePipelineLayout)
    LOAD_VK_FUNC(vkDestroyPipelineLayout)
    LOAD_VK_FUNC(vkCreateComputePipelines)
    LOAD_VK_FUNC(vkDestroyPipeline)
    LOAD_VK_FUNC(vkCreateDescriptorPool)
    LOAD_VK_FUNC(vkDestroyDescriptorPool)
    LOAD_VK_FUNC(vkAllocateDescriptorSets)
    LOAD_VK_FUNC(vkUpdateDescriptorSets)
    LOAD_VK_FUNC(vkResetDescriptorPool)
    LOAD_VK_FUNC(vkAllocateCommandBuffers)
    LOAD_VK_FUNC(vkFreeCommandBuffers)
    LOAD_VK_FUNC(vkBeginCommandBuffer)
    LOAD_VK_FUNC(vkEndCommandBuffer)
    LOAD_VK_FUNC(vkCmdBindPipeline)
    LOAD_VK_FUNC(vkCmdBindDescriptorSets)
    LOAD_VK_FUNC(vkCmdDispatch)
    LOAD_VK_FUNC(vkQueueSubmit)
    LOAD_VK_FUNC(vkQueueWaitIdle)

    #undef LOAD_VK_FUNC

    vulkan_available = 1;
    return 1;
}

int vulkan_loader_available() {
    return vulkan_available;
}

const char* vulkan_loader_error() {
    return error_message;
}

} // extern "C"
