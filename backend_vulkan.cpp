/*
 * Mental - Vulkan Backend (Linux, Windows)
 */

#ifdef MENTAL_HAS_VULKAN

#include <vulkan/vulkan.h>
#include "mental_internal.h"
#include "transpile.h"
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>

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

    if (vkCreateInstance(&create_info, nullptr, &g_instance) != VK_SUCCESS) {
        return -1;
    }

    /* Enumerate physical devices */
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(g_instance, &device_count, nullptr);

    if (device_count == 0) {
        vkDestroyInstance(g_instance, nullptr);
        g_instance = VK_NULL_HANDLE;
        return -1;
    }

    g_physical_devices.resize(device_count);
    vkEnumeratePhysicalDevices(g_instance, &device_count, g_physical_devices.data());

    return 0;
}

static void vulkan_shutdown(void) {
    if (g_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(g_instance, nullptr);
        g_instance = VK_NULL_HANDLE;
    }
    g_physical_devices.clear();
}

static int vulkan_device_count(void) {
    return (int)g_physical_devices.size();
}

static int vulkan_device_info(int index, char* name, size_t name_len) {
    if (index < 0 || index >= (int)g_physical_devices.size()) return -1;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_physical_devices[index], &props);

    strncpy(name, props.deviceName, name_len - 1);
    name[name_len - 1] = '\0';

    return 0;
}

static uint32_t find_compute_queue_family(VkPhysicalDevice device) {
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, nullptr);

    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, queue_families.data());

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

    if (vkCreateDevice(dev->physical_device, &device_create_info, nullptr, &dev->device) != VK_SUCCESS) {
        delete dev;
        return NULL;
    }

    /* Get queue */
    vkGetDeviceQueue(dev->device, dev->queue_family_index, 0, &dev->queue);

    /* Create command pool */
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = dev->queue_family_index;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    if (vkCreateCommandPool(dev->device, &pool_info, nullptr, &dev->command_pool) != VK_SUCCESS) {
        vkDestroyDevice(dev->device, nullptr);
        delete dev;
        return NULL;
    }

    return dev;
}

static void vulkan_device_destroy(void* dev) {
    if (!dev) return;

    VulkanDevice* vk_dev = (VulkanDevice*)dev;
    vkDestroyCommandPool(vk_dev->device, vk_dev->command_pool, nullptr);
    vkDestroyDevice(vk_dev->device, nullptr);
    delete vk_dev;
}

static uint32_t find_memory_type(VkPhysicalDevice physical_device, uint32_t type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_properties);

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

    if (vkCreateBuffer(vk_dev->device, &buffer_info, nullptr, &buf->buffer) != VK_SUCCESS) {
        delete buf;
        return NULL;
    }

    /* Allocate memory */
    VkMemoryRequirements mem_requirements;
    vkGetBufferMemoryRequirements(vk_dev->device, buf->buffer, &mem_requirements);

    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_requirements.size;
    alloc_info.memoryTypeIndex = find_memory_type(vk_dev->physical_device, mem_requirements.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (alloc_info.memoryTypeIndex == UINT32_MAX) {
        vkDestroyBuffer(vk_dev->device, buf->buffer, nullptr);
        delete buf;
        return NULL;
    }

    if (vkAllocateMemory(vk_dev->device, &alloc_info, nullptr, &buf->memory) != VK_SUCCESS) {
        vkDestroyBuffer(vk_dev->device, buf->buffer, nullptr);
        delete buf;
        return NULL;
    }

    vkBindBufferMemory(vk_dev->device, buf->buffer, buf->memory, 0);

    /* Map memory persistently */
    if (vkMapMemory(vk_dev->device, buf->memory, 0, bytes, 0, &buf->mapped_ptr) != VK_SUCCESS) {
        vkFreeMemory(vk_dev->device, buf->memory, nullptr);
        vkDestroyBuffer(vk_dev->device, buf->buffer, nullptr);
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
    vkUnmapMemory(old_vk_buf->device_ctx->device, old_vk_buf->memory);
    vkDestroyBuffer(old_vk_buf->device_ctx->device, old_vk_buf->buffer, nullptr);
    vkFreeMemory(old_vk_buf->device_ctx->device, old_vk_buf->memory, nullptr);
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
    vkUnmapMemory(vk_buf->device_ctx->device, vk_buf->memory);
    vkDestroyBuffer(vk_buf->device_ctx->device, vk_buf->buffer, nullptr);
    vkFreeMemory(vk_buf->device_ctx->device, vk_buf->memory, nullptr);
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

    if (vkCreateShaderModule(vk_dev->device, &module_info, nullptr, &kernel->shader_module) != VK_SUCCESS) {
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

    if (vkCreateDescriptorSetLayout(vk_dev->device, &layout_info, nullptr,
                                     &kernel->descriptor_set_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(vk_dev->device, kernel->shader_module, nullptr);
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

    if (vkCreatePipelineLayout(vk_dev->device, &pipeline_layout_info, nullptr,
                                &kernel->pipeline_layout) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(vk_dev->device, kernel->descriptor_set_layout, nullptr);
        vkDestroyShaderModule(vk_dev->device, kernel->shader_module, nullptr);
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

    if (vkCreateComputePipelines(vk_dev->device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                  &kernel->pipeline) != VK_SUCCESS) {
        vkDestroyPipelineLayout(vk_dev->device, kernel->pipeline_layout, nullptr);
        vkDestroyDescriptorSetLayout(vk_dev->device, kernel->descriptor_set_layout, nullptr);
        vkDestroyShaderModule(vk_dev->device, kernel->shader_module, nullptr);
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

    if (vkCreateDescriptorPool(vk_dev->device, &pool_info, nullptr,
                                &kernel->descriptor_pool) != VK_SUCCESS) {
        vkDestroyPipeline(vk_dev->device, kernel->pipeline, nullptr);
        vkDestroyPipelineLayout(vk_dev->device, kernel->pipeline_layout, nullptr);
        vkDestroyDescriptorSetLayout(vk_dev->device, kernel->descriptor_set_layout, nullptr);
        vkDestroyShaderModule(vk_dev->device, kernel->shader_module, nullptr);
        if (error) {
            snprintf(error, error_len, "Failed to create descriptor pool");
        }
        delete kernel;
        return NULL;
    }

    return kernel;
}

static void vulkan_kernel_dispatch(void* kernel, void** inputs, int input_count,
                                    void* output, int work_size) {
    if (!kernel) return;

    VulkanKernel* vk_kernel = (VulkanKernel*)kernel;
    VulkanBuffer* output_buf = (VulkanBuffer*)output;
    VulkanDevice* vk_dev = vk_kernel->device_ctx;

    /* Allocate descriptor set */
    VkDescriptorSet descriptor_set;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = vk_kernel->descriptor_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &vk_kernel->descriptor_set_layout;

    if (vkAllocateDescriptorSets(vk_dev->device, &alloc_info, &descriptor_set) != VK_SUCCESS) {
        return;
    }


    /* Build buffer info array first (fixed size), then build descriptor
     * writes pointing into it.  IMPORTANT: we must not push_back into
     * buffer_infos after taking pointers — vector reallocation would
     * invalidate them. Pre-allocate the full size. */
    int total_buffers = input_count + 1;
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

    /* Output buffer */
    buffer_infos[input_count] = {};
    buffer_infos[input_count].buffer = output_buf->buffer;
    buffer_infos[input_count].offset = 0;
    buffer_infos[input_count].range = output_buf->size;

    descriptor_writes[input_count] = {};
    descriptor_writes[input_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_writes[input_count].dstSet = descriptor_set;
    descriptor_writes[input_count].dstBinding = input_count;
    descriptor_writes[input_count].dstArrayElement = 0;
    descriptor_writes[input_count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptor_writes[input_count].descriptorCount = 1;
    descriptor_writes[input_count].pBufferInfo = &buffer_infos[input_count];

    vkUpdateDescriptorSets(vk_dev->device, (uint32_t)descriptor_writes.size(),
                           descriptor_writes.data(), 0, nullptr);

    /* Allocate command buffer */
    VkCommandBufferAllocateInfo cmd_buf_alloc_info = {};
    cmd_buf_alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_buf_alloc_info.commandPool = vk_dev->command_pool;
    cmd_buf_alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_buf_alloc_info.commandBufferCount = 1;

    VkCommandBuffer command_buffer;
    if (vkAllocateCommandBuffers(vk_dev->device, &cmd_buf_alloc_info, &command_buffer) != VK_SUCCESS) {
        return;
    }

    /* Record command buffer */
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(command_buffer, &begin_info);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, vk_kernel->pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            vk_kernel->pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);
    /* work_size is the total number of invocations. The shader's
     * local_size_x defines the workgroup size (typically 256).
     * vkCmdDispatch takes the number of workgroups, not invocations. */
    uint32_t local_size = 256;
    uint32_t num_groups = ((uint32_t)work_size + local_size - 1) / local_size;
    vkCmdDispatch(command_buffer, num_groups, 1, 1);
    vkEndCommandBuffer(command_buffer);

    /* Submit command buffer */
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;

    if (vkQueueSubmit(vk_dev->queue, 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS) {
        vkFreeCommandBuffers(vk_dev->device, vk_dev->command_pool, 1, &command_buffer);
        return;
    }

    vkQueueWaitIdle(vk_dev->queue);

    /* Free command buffer */
    vkFreeCommandBuffers(vk_dev->device, vk_dev->command_pool, 1, &command_buffer);

    /* Reset descriptor pool for next dispatch */
    vkResetDescriptorPool(vk_dev->device, vk_kernel->descriptor_pool, 0);
}

static void vulkan_kernel_destroy(void* kernel) {
    if (!kernel) return;

    VulkanKernel* vk_kernel = (VulkanKernel*)kernel;
    VkDevice device = vk_kernel->device_ctx->device;

    if (vk_kernel->descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, vk_kernel->descriptor_pool, nullptr);
    }
    if (vk_kernel->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, vk_kernel->pipeline, nullptr);
    }
    if (vk_kernel->pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, vk_kernel->pipeline_layout, nullptr);
    }
    if (vk_kernel->descriptor_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, vk_kernel->descriptor_set_layout, nullptr);
    }
    if (vk_kernel->shader_module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, vk_kernel->shader_module, nullptr);
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
    int has_swapchain = (vkGetPhysicalDeviceSurfaceCapabilitiesKHR != NULL &&
                         vkCreateSwapchainKHR != NULL);

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

    /* Query surface capabilities */
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_dev->physical_device, vk_surface, &capabilities);

    /* Query surface formats */
    uint32_t format_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk_dev->physical_device, vk_surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk_dev->physical_device, vk_surface, &format_count, formats.data());

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
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk_dev->physical_device, vk_surface, &present_mode_count, nullptr);
    std::vector<VkPresentModeKHR> present_modes(present_mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk_dev->physical_device, vk_surface, &present_mode_count, present_modes.data());

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
    VkResult sc_result = vkCreateSwapchainKHR(vk_dev->device, &swapchain_info, nullptr, &swapchain);

    if (sc_result == VK_SUCCESS && swapchain != VK_NULL_HANDLE) {
        /* Double-buffered path */
        viewport->swapchain = swapchain;

        uint32_t image_count;
        vkGetSwapchainImagesKHR(vk_dev->device, swapchain, &image_count, nullptr);
        viewport->swapchain_images.resize(image_count);
        vkGetSwapchainImagesKHR(vk_dev->device, swapchain, &image_count, viewport->swapchain_images.data());
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

        if (vkCreateImage(vk_dev->device, &img_info, nullptr, &viewport->headless_image) != VK_SUCCESS) {
            if (error) snprintf(error, error_len, "Failed to create headless render target");
            delete viewport;
            return NULL;
        }

        VkMemoryRequirements mem_req;
        vkGetImageMemoryRequirements(vk_dev->device, viewport->headless_image, &mem_req);

        VkMemoryAllocateInfo mem_alloc = {};
        mem_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mem_alloc.allocationSize = mem_req.size;
        mem_alloc.memoryTypeIndex = find_memory_type(vk_dev->physical_device, mem_req.memoryTypeBits,
                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(vk_dev->device, &mem_alloc, nullptr, &viewport->headless_memory) != VK_SUCCESS) {
            vkDestroyImage(vk_dev->device, viewport->headless_image, nullptr);
            if (error) snprintf(error, error_len, "Failed to allocate headless image memory");
            delete viewport;
            return NULL;
        }

        vkBindImageMemory(vk_dev->device, viewport->headless_image, viewport->headless_memory, 0);
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
        vkQueueWaitIdle(vk_dev->queue);
        return;
    }

    /* Double-buffered path: acquire, present */
    vkAcquireNextImageKHR(vk_dev->device, viewport->swapchain, UINT64_MAX,
                          VK_NULL_HANDLE, VK_NULL_HANDLE, &viewport->image_index);

    VkPresentInfoKHR present_info = {};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &viewport->swapchain;
    present_info.pImageIndices = &viewport->image_index;

    vkQueuePresentKHR(vk_dev->queue, &present_info);
    vkQueueWaitIdle(vk_dev->queue);
}

static void vulkan_viewport_detach(void* viewport_ptr) {
    if (!viewport_ptr) return;

    VulkanViewport* viewport = (VulkanViewport*)viewport_ptr;
    VkDevice device = viewport->device_ctx->device;

    if (viewport->swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, viewport->swapchain, nullptr);
    }
    if (viewport->headless_image != VK_NULL_HANDLE) {
        vkDestroyImage(device, viewport->headless_image, nullptr);
    }
    if (viewport->headless_memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, viewport->headless_memory, nullptr);
    }

    delete viewport;
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
    .kernel_dispatch = vulkan_kernel_dispatch,
    .kernel_destroy = vulkan_kernel_destroy,
    .viewport_attach = vulkan_viewport_attach,
    .viewport_present = vulkan_viewport_present,
    .viewport_detach = vulkan_viewport_detach
};

mental_backend* vulkan_backend = &g_vulkan_backend;

#else
/* Vulkan SDK not available */
mental_backend* vulkan_backend = NULL;
#endif /* MENTAL_HAS_VULKAN */
