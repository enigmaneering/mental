//go:build linux

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "vulkan_loader.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <vector>

// Helper to duplicate a string (caller must free)
static char* strdup_helper(const char* str) {
    if (str == nullptr) return nullptr;
    size_t len = strlen(str);
    char* dup = (char*)malloc(len + 1);
    if (dup) {
        memcpy(dup, str, len + 1);
    }
    return dup;
}

// Vulkan instance (global, shared across all devices)
static VkInstance g_instance = VK_NULL_HANDLE;
static std::vector<VkPhysicalDevice> g_physicalDevices;

// Device context structure
struct VulkanDevice {
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    VkQueue queue;
    VkCommandPool commandPool;
    uint32_t queueFamilyIndex;
};

// Buffer structure
struct VulkanBuffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* mappedPtr;
    size_t size;
    VkDevice device;
};

// Shader structure
struct VulkanShader {
    VkShaderModule shaderModule;
    VkPipeline pipeline;
    VkPipelineLayout pipelineLayout;
    VkDescriptorSetLayout descriptorSetLayout;
    VkDescriptorPool descriptorPool;
    VkDevice device;
};

extern "C" {

// Enumerate Vulkan devices
int vulkan_enumerate_devices(void*** devices_out, char*** names_out, int** types_out, uint32_t** vendor_ids_out) {
    // Create Vulkan instance if not already created
    if (g_instance == VK_NULL_HANDLE) {
        VkApplicationInfo appInfo = {};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "glitter";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "glitter";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;

        if (vkCreateInstance(&createInfo, nullptr, &g_instance) != VK_SUCCESS) {
            return 0;
        }
    }

    // Enumerate physical devices
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(g_instance, &deviceCount, nullptr);

    if (deviceCount == 0) {
        return 0;
    }

    g_physicalDevices.resize(deviceCount);
    vkEnumeratePhysicalDevices(g_instance, &deviceCount, g_physicalDevices.data());

    // Allocate output arrays
    void** device_array = (void**)malloc(deviceCount * sizeof(void*));
    char** name_array = (char**)malloc(deviceCount * sizeof(char*));
    int* type_array = (int*)malloc(deviceCount * sizeof(int));
    uint32_t* vendor_id_array = (uint32_t*)malloc(deviceCount * sizeof(uint32_t));

    for (uint32_t i = 0; i < deviceCount; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(g_physicalDevices[i], &props);

        device_array[i] = (void*)g_physicalDevices[i];
        name_array[i] = strdup_helper(props.deviceName);
        vendor_id_array[i] = props.vendorID;

        // Map Vulkan device type to our Type enum
        // Other=0, Integrated=1, Discrete=2, Virtual=3
        switch (props.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                type_array[i] = 1; // Integrated
                break;
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                type_array[i] = 2; // Discrete
                break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                type_array[i] = 3; // Virtual
                break;
            default:
                type_array[i] = 0; // Other
                break;
        }
    }

    *devices_out = device_array;
    *names_out = name_array;
    *types_out = type_array;
    *vendor_ids_out = vendor_id_array;

    return (int)deviceCount;
}

// Create Vulkan logical device
void* vulkan_create_device(void* physical_device_ptr, char** error_out) {
    VkPhysicalDevice physicalDevice = (VkPhysicalDevice)physical_device_ptr;

    // Find compute queue family
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

    uint32_t computeQueueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            computeQueueFamily = i;
            break;
        }
    }

    if (computeQueueFamily == UINT32_MAX) {
        if (error_out) *error_out = strdup_helper("No compute queue family found");
        return nullptr;
    }

    // Create logical device
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo = {};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = computeQueueFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo deviceCreateInfo = {};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;

    VkDevice device;
    if (vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &device) != VK_SUCCESS) {
        if (error_out) *error_out = strdup_helper("Failed to create logical device");
        return nullptr;
    }

    // Get queue handle
    VkQueue queue;
    vkGetDeviceQueue(device, computeQueueFamily, 0, &queue);

    // Create command pool
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = computeQueueFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkCommandPool commandPool;
    if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
        vkDestroyDevice(device, nullptr);
        if (error_out) *error_out = strdup_helper("Failed to create command pool");
        return nullptr;
    }

    // Create device context
    VulkanDevice* ctx = new VulkanDevice();
    ctx->physicalDevice = physicalDevice;
    ctx->device = device;
    ctx->queue = queue;
    ctx->commandPool = commandPool;
    ctx->queueFamilyIndex = computeQueueFamily;

    return (void*)ctx;
}

// Release Vulkan device
void vulkan_release_device(void* device_ctx) {
    if (device_ctx == nullptr) return;

    VulkanDevice* ctx = (VulkanDevice*)device_ctx;

    if (ctx->commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(ctx->device, ctx->commandPool, nullptr);
    }

    if (ctx->device != VK_NULL_HANDLE) {
        vkDestroyDevice(ctx->device, nullptr);
    }

    delete ctx;
}

// Find suitable memory type
static uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    return UINT32_MAX;
}

// Allocate buffer
void* vulkan_alloc_buffer(void* device_ctx, size_t size) {
    VulkanDevice* ctx = (VulkanDevice*)device_ctx;

    // Create buffer
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer;
    if (vkCreateBuffer(ctx->device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        return nullptr;
    }

    // Get memory requirements
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(ctx->device, buffer, &memRequirements);

    // Allocate memory (host-visible and coherent for easy CPU access)
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(
        ctx->physicalDevice,
        memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    VkDeviceMemory memory;
    if (vkAllocateMemory(ctx->device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(ctx->device, buffer, nullptr);
        return nullptr;
    }

    // Bind buffer to memory
    vkBindBufferMemory(ctx->device, buffer, memory, 0);

    // Map memory
    void* mappedPtr = nullptr;
    vkMapMemory(ctx->device, memory, 0, size, 0, &mappedPtr);

    // Create buffer structure
    VulkanBuffer* vkBuf = new VulkanBuffer();
    vkBuf->buffer = buffer;
    vkBuf->memory = memory;
    vkBuf->mappedPtr = mappedPtr;
    vkBuf->size = size;
    vkBuf->device = ctx->device;

    return (void*)vkBuf;
}

// Release buffer
void vulkan_release_buffer(void* buffer_ptr) {
    if (buffer_ptr == nullptr) return;

    VulkanBuffer* vkBuf = (VulkanBuffer*)buffer_ptr;

    if (vkBuf->mappedPtr != nullptr) {
        vkUnmapMemory(vkBuf->device, vkBuf->memory);
    }

    if (vkBuf->buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(vkBuf->device, vkBuf->buffer, nullptr);
    }

    if (vkBuf->memory != VK_NULL_HANDLE) {
        vkFreeMemory(vkBuf->device, vkBuf->memory, nullptr);
    }

    delete vkBuf;
}

// Get buffer contents pointer
void* vulkan_buffer_contents(void* buffer_ptr) {
    if (buffer_ptr == nullptr) return nullptr;
    VulkanBuffer* vkBuf = (VulkanBuffer*)buffer_ptr;
    return vkBuf->mappedPtr;
}

// Get buffer size
size_t vulkan_buffer_size(void* buffer_ptr) {
    if (buffer_ptr == nullptr) return 0;
    VulkanBuffer* vkBuf = (VulkanBuffer*)buffer_ptr;
    return vkBuf->size;
}

// Compile SPIR-V shader
void* vulkan_compile_shader(void* device_ctx, const uint32_t* spirv_code, size_t spirv_size, int buffer_count, char** error_out) {
    VulkanDevice* ctx = (VulkanDevice*)device_ctx;

    // Create shader module
    VkShaderModuleCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = spirv_size;
    createInfo.pCode = spirv_code;

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(ctx->device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        if (error_out) *error_out = strdup_helper("Failed to create shader module");
        return nullptr;
    }

    // Create descriptor set layout (only if we have buffers)
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    if (buffer_count > 0) {
        std::vector<VkDescriptorSetLayoutBinding> bindings(buffer_count);
        for (int i = 0; i < buffer_count; i++) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[i].pImmutableSamplers = nullptr;
        }

        VkDescriptorSetLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = buffer_count;
        layoutInfo.pBindings = bindings.data();

        if (vkCreateDescriptorSetLayout(ctx->device, &layoutInfo, nullptr, &descriptorSetLayout) != VK_SUCCESS) {
            vkDestroyShaderModule(ctx->device, shaderModule, nullptr);
            if (error_out) *error_out = strdup_helper("Failed to create descriptor set layout");
            return nullptr;
        }
    }

    // Create pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = (buffer_count > 0) ? 1 : 0;
    pipelineLayoutInfo.pSetLayouts = (buffer_count > 0) ? &descriptorSetLayout : nullptr;

    VkPipelineLayout pipelineLayout;
    if (vkCreatePipelineLayout(ctx->device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        if (descriptorSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx->device, descriptorSetLayout, nullptr);
        }
        vkDestroyShaderModule(ctx->device, shaderModule, nullptr);
        if (error_out) *error_out = strdup_helper("Failed to create pipeline layout");
        return nullptr;
    }

    // Create compute pipeline
    VkPipelineShaderStageCreateInfo shaderStageInfo = {};
    shaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStageInfo.module = shaderModule;
    shaderStageInfo.pName = "main";
    shaderStageInfo.pSpecializationInfo = nullptr;

    VkComputePipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = shaderStageInfo;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    VkPipeline pipeline;
    VkResult result = vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    if (result != VK_SUCCESS) {
        vkDestroyPipelineLayout(ctx->device, pipelineLayout, nullptr);
        if (descriptorSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx->device, descriptorSetLayout, nullptr);
        }
        vkDestroyShaderModule(ctx->device, shaderModule, nullptr);

        // Create detailed error message
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "Failed to create compute pipeline (VkResult = %d)", result);
        if (error_out) *error_out = strdup_helper(error_buf);
        return nullptr;
    }

    // Create descriptor pool (only if we have buffers)
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    if (buffer_count > 0) {
        VkDescriptorPoolSize poolSize = {};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = buffer_count;

        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = 1;

        if (vkCreateDescriptorPool(ctx->device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
            vkDestroyPipeline(ctx->device, pipeline, nullptr);
            vkDestroyPipelineLayout(ctx->device, pipelineLayout, nullptr);
            if (descriptorSetLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(ctx->device, descriptorSetLayout, nullptr);
            }
            vkDestroyShaderModule(ctx->device, shaderModule, nullptr);
            if (error_out) *error_out = strdup_helper("Failed to create descriptor pool");
            return nullptr;
        }
    }

    // Create shader structure
    VulkanShader* shader = new VulkanShader();
    shader->shaderModule = shaderModule;
    shader->pipeline = pipeline;
    shader->pipelineLayout = pipelineLayout;
    shader->descriptorSetLayout = descriptorSetLayout;
    shader->descriptorPool = descriptorPool;
    shader->device = ctx->device;

    return (void*)shader;
}

// Release shader
void vulkan_release_shader(void* shader_ptr) {
    if (shader_ptr == nullptr) return;

    VulkanShader* shader = (VulkanShader*)shader_ptr;

    if (shader->descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(shader->device, shader->descriptorPool, nullptr);
    }

    if (shader->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(shader->device, shader->pipeline, nullptr);
    }

    if (shader->pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(shader->device, shader->pipelineLayout, nullptr);
    }

    if (shader->descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(shader->device, shader->descriptorSetLayout, nullptr);
    }

    if (shader->shaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(shader->device, shader->shaderModule, nullptr);
    }

    delete shader;
}

// Dispatch compute shader
int vulkan_dispatch_compute(void* device_ctx, void* shader_ptr, void** buffers, int buffer_count, int work_size, char** error_out) {
    VulkanDevice* ctx = (VulkanDevice*)device_ctx;
    VulkanShader* shader = (VulkanShader*)shader_ptr;

    // Allocate and update descriptor set (only if we have buffers)
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    if (buffer_count > 0) {
        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = shader->descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &shader->descriptorSetLayout;

        if (vkAllocateDescriptorSets(ctx->device, &allocInfo, &descriptorSet) != VK_SUCCESS) {
            if (error_out) *error_out = strdup_helper("Failed to allocate descriptor set");
            return 0;
        }

        // Update descriptor set with buffers
        std::vector<VkWriteDescriptorSet> descriptorWrites(buffer_count);
        std::vector<VkDescriptorBufferInfo> bufferInfos(buffer_count);

        for (int i = 0; i < buffer_count; i++) {
            VulkanBuffer* vkBuf = (VulkanBuffer*)buffers[i];

            bufferInfos[i].buffer = vkBuf->buffer;
            bufferInfos[i].offset = 0;
            bufferInfos[i].range = vkBuf->size;

            descriptorWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[i].dstSet = descriptorSet;
            descriptorWrites[i].dstBinding = i;
            descriptorWrites[i].dstArrayElement = 0;
            descriptorWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            descriptorWrites[i].descriptorCount = 1;
            descriptorWrites[i].pBufferInfo = &bufferInfos[i];
        }

        vkUpdateDescriptorSets(ctx->device, buffer_count, descriptorWrites.data(), 0, nullptr);
    }

    // Create command buffer
    VkCommandBufferAllocateInfo cmdBufAllocInfo = {};
    cmdBufAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdBufAllocInfo.commandPool = ctx->commandPool;
    cmdBufAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdBufAllocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    if (vkAllocateCommandBuffers(ctx->device, &cmdBufAllocInfo, &commandBuffer) != VK_SUCCESS) {
        if (error_out) *error_out = strdup_helper("Failed to allocate command buffer");
        return 0;
    }

    // Record command buffer
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, shader->pipeline);

    // Only bind descriptor sets if we have buffers
    if (buffer_count > 0) {
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, shader->pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
    }

    vkCmdDispatch(commandBuffer, work_size, 1, 1);

    vkEndCommandBuffer(commandBuffer);

    // Submit command buffer
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    if (vkQueueSubmit(ctx->queue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
        if (error_out) *error_out = strdup_helper("Failed to submit command buffer");
        vkFreeCommandBuffers(ctx->device, ctx->commandPool, 1, &commandBuffer);
        return 0;
    }

    // Wait for completion (synchronous execution like Metal)
    vkQueueWaitIdle(ctx->queue);

    // Clean up
    vkFreeCommandBuffers(ctx->device, ctx->commandPool, 1, &commandBuffer);

    // Reset descriptor pool only if we have buffers
    if (buffer_count > 0) {
        vkResetDescriptorPool(ctx->device, shader->descriptorPool, 0);
    }

    return 1;
}

} // extern "C"
