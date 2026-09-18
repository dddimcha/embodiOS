/* GGML Vulkan Backend Implementation
 * GPU acceleration for matrix operations via Vulkan compute shaders
 */

#define _CRT_SECURE_NO_DEPRECATE

#include "ggml-backend-impl.h"
#include "ggml-backend.h"
#include "ggml-vulkan.h"
#include "ggml-impl.h"
#include "ggml.h"

#include <vulkan/vulkan.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

//
// Vulkan Backend State
//

struct ggml_vk_device {
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory_properties;
    VkQueue compute_queue;
    uint32_t compute_queue_family_index;
    bool initialized;
};

struct ggml_vk_context {
    VkInstance instance;
    struct ggml_vk_device devices[GGML_VK_MAX_DEVICES];
    uint32_t device_count;
    bool initialized;
};

// Vulkan buffer structure
struct ggml_vk_buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    size_t size;
    void * mapped;
    struct ggml_vk_device * device;
    bool is_host;
};

// Buffer type context
struct ggml_backend_vk_buffer_type_context {
    size_t device_index;
    bool is_host;
};

// Shader module structure
struct ggml_vk_shader_module {
    VkShaderModule module;
    struct ggml_vk_device * device;
};

// Compute pipeline structure
struct ggml_vk_pipeline {
    VkPipeline pipeline;
    VkPipelineLayout pipeline_layout;
    VkDescriptorSetLayout descriptor_set_layout;
    struct ggml_vk_device * device;
};

// Command buffer context for shader dispatch
struct ggml_vk_command_context {
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    VkDescriptorPool descriptor_pool;
    struct ggml_vk_device * device;
    bool recording;
};

// Global Vulkan context
static struct ggml_vk_context g_vk_ctx = { 0 };

//
// Vulkan Instance and Device Initialization
//

static VkResult ggml_vk_create_instance(void) {
    if (g_vk_ctx.initialized) {
        return VK_SUCCESS;
    }

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = NULL,
        .pApplicationName = "GGML Vulkan Backend",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "GGML",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_2,
    };

    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .pApplicationInfo = &app_info,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = NULL,
        .enabledExtensionCount = 0,
        .ppEnabledExtensionNames = NULL,
    };

    VkResult result = vkCreateInstance(&create_info, NULL, &g_vk_ctx.instance);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "GGML Vulkan: Failed to create instance (error %d)\n", result);
        return result;
    }

    g_vk_ctx.initialized = true;
    return VK_SUCCESS;
}

static VkResult ggml_vk_enumerate_devices(void) {
    if (!g_vk_ctx.initialized) {
        VkResult result = ggml_vk_create_instance();
        if (result != VK_SUCCESS) {
            return result;
        }
    }

    // Get device count
    uint32_t device_count = 0;
    VkResult result = vkEnumeratePhysicalDevices(g_vk_ctx.instance, &device_count, NULL);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "GGML Vulkan: Failed to enumerate physical devices (error %d)\n", result);
        return result;
    }

    if (device_count == 0) {
        fprintf(stderr, "GGML Vulkan: No Vulkan devices found\n");
        g_vk_ctx.device_count = 0;
        return VK_SUCCESS;
    }

    // Limit to max supported devices
    if (device_count > GGML_VK_MAX_DEVICES) {
        fprintf(stderr, "GGML Vulkan: Found %u devices, limiting to %d\n",
                device_count, GGML_VK_MAX_DEVICES);
        device_count = GGML_VK_MAX_DEVICES;
    }

    // Get physical devices
    VkPhysicalDevice physical_devices[GGML_VK_MAX_DEVICES];
    result = vkEnumeratePhysicalDevices(g_vk_ctx.instance, &device_count, physical_devices);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "GGML Vulkan: Failed to get physical devices (error %d)\n", result);
        return result;
    }

    // Store device information
    g_vk_ctx.device_count = device_count;
    for (uint32_t i = 0; i < device_count; i++) {
        g_vk_ctx.devices[i].physical_device = physical_devices[i];
        g_vk_ctx.devices[i].initialized = false;

        // Get device properties
        vkGetPhysicalDeviceProperties(physical_devices[i], &g_vk_ctx.devices[i].properties);

        fprintf(stderr, "GGML Vulkan: Device %u: %s (vendor 0x%04x, device 0x%04x)\n",
                i,
                g_vk_ctx.devices[i].properties.deviceName,
                g_vk_ctx.devices[i].properties.vendorID,
                g_vk_ctx.devices[i].properties.deviceID);
    }

    return VK_SUCCESS;
}

static VkResult ggml_vk_init_device(size_t dev_num) {
    if (dev_num >= g_vk_ctx.device_count) {
        fprintf(stderr, "GGML Vulkan: Invalid device number %zu (only %u devices available)\n",
                dev_num, g_vk_ctx.device_count);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    struct ggml_vk_device *device = &g_vk_ctx.devices[dev_num];
    if (device->initialized) {
        return VK_SUCCESS;
    }

    // Get memory properties for buffer allocation
    vkGetPhysicalDeviceMemoryProperties(device->physical_device, &device->memory_properties);

    // For now, mark as initialized without creating logical device
    // Full device initialization will be implemented in subsequent subtasks
    device->initialized = true;

    return VK_SUCCESS;
}

//
// Vulkan Buffer Memory Management
//

static uint32_t ggml_vk_find_memory_type(struct ggml_vk_device * device, uint32_t type_filter, VkMemoryPropertyFlags properties) {
    for (uint32_t i = 0; i < device->memory_properties.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) &&
            (device->memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

static VkResult ggml_vk_allocate_buffer(struct ggml_vk_device * device, size_t size,
                                       VkBufferUsageFlags usage, VkMemoryPropertyFlags mem_properties,
                                       struct ggml_vk_buffer * buffer) {
    if (!device->device) {
        fprintf(stderr, "GGML Vulkan: Device not initialized for buffer allocation\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    buffer->device = device;
    buffer->size = size;
    buffer->mapped = NULL;
    buffer->is_host = (mem_properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;

    // Create buffer
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = NULL,
    };

    VkResult result = vkCreateBuffer(device->device, &buffer_info, NULL, &buffer->buffer);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "GGML Vulkan: Failed to create buffer (error %d)\n", result);
        return result;
    }

    // Get memory requirements
    VkMemoryRequirements mem_requirements;
    vkGetBufferMemoryRequirements(device->device, buffer->buffer, &mem_requirements);

    // Find suitable memory type
    uint32_t memory_type = ggml_vk_find_memory_type(device, mem_requirements.memoryTypeBits, mem_properties);
    if (memory_type == UINT32_MAX) {
        fprintf(stderr, "GGML Vulkan: Failed to find suitable memory type\n");
        vkDestroyBuffer(device->device, buffer->buffer, NULL);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    // Allocate memory
    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = NULL,
        .allocationSize = mem_requirements.size,
        .memoryTypeIndex = memory_type,
    };

    result = vkAllocateMemory(device->device, &alloc_info, NULL, &buffer->memory);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "GGML Vulkan: Failed to allocate buffer memory (error %d)\n", result);
        vkDestroyBuffer(device->device, buffer->buffer, NULL);
        return result;
    }

    // Bind buffer memory
    result = vkBindBufferMemory(device->device, buffer->buffer, buffer->memory, 0);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "GGML Vulkan: Failed to bind buffer memory (error %d)\n", result);
        vkFreeMemory(device->device, buffer->memory, NULL);
        vkDestroyBuffer(device->device, buffer->buffer, NULL);
        return result;
    }

    return VK_SUCCESS;
}

static void ggml_vk_free_buffer(struct ggml_vk_buffer * buffer) {
    if (!buffer || !buffer->device || !buffer->device->device) {
        return;
    }

    if (buffer->mapped) {
        vkUnmapMemory(buffer->device->device, buffer->memory);
        buffer->mapped = NULL;
    }

    if (buffer->memory != VK_NULL_HANDLE) {
        vkFreeMemory(buffer->device->device, buffer->memory, NULL);
        buffer->memory = VK_NULL_HANDLE;
    }

    if (buffer->buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(buffer->device->device, buffer->buffer, NULL);
        buffer->buffer = VK_NULL_HANDLE;
    }
}

static VkResult ggml_vk_map_buffer(struct ggml_vk_buffer * buffer) {
    if (!buffer || !buffer->device || !buffer->device->device) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (buffer->mapped) {
        return VK_SUCCESS;
    }

    if (!buffer->is_host) {
        fprintf(stderr, "GGML Vulkan: Cannot map device-local buffer\n");
        return VK_ERROR_MEMORY_MAP_FAILED;
    }

    VkResult result = vkMapMemory(buffer->device->device, buffer->memory, 0, buffer->size, 0, &buffer->mapped);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "GGML Vulkan: Failed to map buffer memory (error %d)\n", result);
        return result;
    }

    return VK_SUCCESS;
}

//
// Vulkan Shader Module and Pipeline Management
//

static VkResult ggml_vk_create_shader_module(struct ggml_vk_device * device, const uint32_t * code, size_t code_size,
                                             struct ggml_vk_shader_module * shader_module) {
    if (!device || !device->device) {
        fprintf(stderr, "GGML Vulkan: Device not initialized for shader module creation\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    shader_module->device = device;

    VkShaderModuleCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .codeSize = code_size,
        .pCode = code,
    };

    VkResult result = vkCreateShaderModule(device->device, &create_info, NULL, &shader_module->module);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "GGML Vulkan: Failed to create shader module (error %d)\n", result);
        return result;
    }

    return VK_SUCCESS;
}

static void ggml_vk_destroy_shader_module(struct ggml_vk_shader_module * shader_module) {
    if (!shader_module || !shader_module->device || !shader_module->device->device) {
        return;
    }

    if (shader_module->module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(shader_module->device->device, shader_module->module, NULL);
        shader_module->module = VK_NULL_HANDLE;
    }
}

static VkResult ggml_vk_create_descriptor_set_layout(struct ggml_vk_device * device, uint32_t binding_count,
                                                      VkDescriptorSetLayoutBinding * bindings,
                                                      VkDescriptorSetLayout * descriptor_set_layout) {
    if (!device || !device->device) {
        fprintf(stderr, "GGML Vulkan: Device not initialized for descriptor set layout creation\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .bindingCount = binding_count,
        .pBindings = bindings,
    };

    VkResult result = vkCreateDescriptorSetLayout(device->device, &layout_info, NULL, descriptor_set_layout);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "GGML Vulkan: Failed to create descriptor set layout (error %d)\n", result);
        return result;
    }

    return VK_SUCCESS;
}

static VkResult ggml_vk_create_pipeline_layout(struct ggml_vk_device * device,
                                                VkDescriptorSetLayout descriptor_set_layout,
                                                VkPipelineLayout * pipeline_layout) {
    if (!device || !device->device) {
        fprintf(stderr, "GGML Vulkan: Device not initialized for pipeline layout creation\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_set_layout,
        .pushConstantRangeCount = 0,
        .pPushConstantRanges = NULL,
    };

    VkResult result = vkCreatePipelineLayout(device->device, &layout_info, NULL, pipeline_layout);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "GGML Vulkan: Failed to create pipeline layout (error %d)\n", result);
        return result;
    }

    return VK_SUCCESS;
}

static VkResult ggml_vk_create_compute_pipeline(struct ggml_vk_device * device,
                                                 struct ggml_vk_shader_module * shader_module,
                                                 VkPipelineLayout pipeline_layout,
                                                 struct ggml_vk_pipeline * pipeline) {
    if (!device || !device->device) {
        fprintf(stderr, "GGML Vulkan: Device not initialized for compute pipeline creation\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    pipeline->device = device;
    pipeline->pipeline_layout = pipeline_layout;

    VkPipelineShaderStageCreateInfo shader_stage_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = shader_module->module,
        .pName = "main",
        .pSpecializationInfo = NULL,
    };

    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .stage = shader_stage_info,
        .layout = pipeline_layout,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1,
    };

    VkResult result = vkCreateComputePipelines(device->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline->pipeline);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "GGML Vulkan: Failed to create compute pipeline (error %d)\n", result);
        return result;
    }

    return VK_SUCCESS;
}

static void ggml_vk_destroy_pipeline(struct ggml_vk_pipeline * pipeline) {
    if (!pipeline || !pipeline->device || !pipeline->device->device) {
        return;
    }

    if (pipeline->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(pipeline->device->device, pipeline->pipeline, NULL);
        pipeline->pipeline = VK_NULL_HANDLE;
    }

    if (pipeline->pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(pipeline->device->device, pipeline->pipeline_layout, NULL);
        pipeline->pipeline_layout = VK_NULL_HANDLE;
    }

    if (pipeline->descriptor_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(pipeline->device->device, pipeline->descriptor_set_layout, NULL);
        pipeline->descriptor_set_layout = VK_NULL_HANDLE;
    }
}

//
// Vulkan Command Buffer and Descriptor Management
//

static VkResult ggml_vk_create_command_pool(struct ggml_vk_device * device, VkCommandPool * command_pool) {
    if (!device || !device->device) {
        fprintf(stderr, "GGML Vulkan: Device not initialized for command pool creation\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = NULL,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = device->compute_queue_family_index,
    };

    VkResult result = vkCreateCommandPool(device->device, &pool_info, NULL, command_pool);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "GGML Vulkan: Failed to create command pool (error %d)\n", result);
        return result;
    }

    return VK_SUCCESS;
}

static VkResult ggml_vk_allocate_command_buffer(struct ggml_vk_device * device, VkCommandPool command_pool,
                                                 VkCommandBuffer * command_buffer) {
    if (!device || !device->device) {
        fprintf(stderr, "GGML Vulkan: Device not initialized for command buffer allocation\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = NULL,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };

    VkResult result = vkAllocateCommandBuffers(device->device, &alloc_info, command_buffer);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "GGML Vulkan: Failed to allocate command buffer (error %d)\n", result);
        return result;
    }

    return VK_SUCCESS;
}

static VkResult ggml_vk_create_descriptor_pool(struct ggml_vk_device * device, uint32_t max_sets,
                                                VkDescriptorPool * descriptor_pool) {
    if (!device || !device->device) {
        fprintf(stderr, "GGML Vulkan: Device not initialized for descriptor pool creation\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = max_sets * 3, // 3 buffers per set (A, x, y)
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = NULL,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = max_sets,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };

    VkResult result = vkCreateDescriptorPool(device->device, &pool_info, NULL, descriptor_pool);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "GGML Vulkan: Failed to create descriptor pool (error %d)\n", result);
        return result;
    }

    return VK_SUCCESS;
}

static VkResult ggml_vk_init_command_context(struct ggml_vk_device * device,
                                              struct ggml_vk_command_context * ctx) {
    if (!device || !device->device) {
        fprintf(stderr, "GGML Vulkan: Device not initialized for command context\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    ctx->device = device;
    ctx->recording = false;

    VkResult result = ggml_vk_create_command_pool(device, &ctx->command_pool);
    if (result != VK_SUCCESS) {
        return result;
    }

    result = ggml_vk_allocate_command_buffer(device, ctx->command_pool, &ctx->command_buffer);
    if (result != VK_SUCCESS) {
        vkDestroyCommandPool(device->device, ctx->command_pool, NULL);
        return result;
    }

    result = ggml_vk_create_descriptor_pool(device, 16, &ctx->descriptor_pool);
    if (result != VK_SUCCESS) {
        vkDestroyCommandPool(device->device, ctx->command_pool, NULL);
        return result;
    }

    return VK_SUCCESS;
}

static void ggml_vk_destroy_command_context(struct ggml_vk_command_context * ctx) {
    if (!ctx || !ctx->device || !ctx->device->device) {
        return;
    }

    if (ctx->descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(ctx->device->device, ctx->descriptor_pool, NULL);
        ctx->descriptor_pool = VK_NULL_HANDLE;
    }

    if (ctx->command_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(ctx->device->device, ctx->command_pool, NULL);
        ctx->command_pool = VK_NULL_HANDLE;
    }

    ctx->command_buffer = VK_NULL_HANDLE;
    ctx->recording = false;
}

//
// Vulkan Shader Dispatch for Matrix Operations
//

struct ggml_vk_matmul_push_constants {
    uint32_t m;          // Number of output rows
    uint32_t n;          // Number of input elements
    uint32_t n_blocks_per_row;  // Number of quantized blocks per row
};

static VkResult ggml_vk_dispatch_matmul(struct ggml_vk_command_context * ctx,
                                        struct ggml_vk_pipeline * pipeline,
                                        struct ggml_vk_buffer * matrix_a,
                                        struct ggml_vk_buffer * vector_x,
                                        struct ggml_vk_buffer * vector_y,
                                        uint32_t m, uint32_t n, uint32_t n_blocks) {
    if (!ctx || !pipeline || !matrix_a || !vector_x || !vector_y) {
        fprintf(stderr, "GGML Vulkan: Invalid parameters for matmul dispatch\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkDevice device = ctx->device->device;

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = NULL,
        .descriptorPool = ctx->descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &pipeline->descriptor_set_layout,
    };

    VkDescriptorSet descriptor_set;
    VkResult result = vkAllocateDescriptorSets(device, &alloc_info, &descriptor_set);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "GGML Vulkan: Failed to allocate descriptor set (error %d)\n", result);
        return result;
    }

    // Update descriptor set with buffer bindings
    VkDescriptorBufferInfo buffer_infos[3] = {
        {
            .buffer = matrix_a->buffer,
            .offset = 0,
            .range = matrix_a->size,
        },
        {
            .buffer = vector_x->buffer,
            .offset = 0,
            .range = vector_x->size,
        },
        {
            .buffer = vector_y->buffer,
            .offset = 0,
            .range = vector_y->size,
        },
    };

    VkWriteDescriptorSet descriptor_writes[3] = {
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = NULL,
            .dstSet = descriptor_set,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pImageInfo = NULL,
            .pBufferInfo = &buffer_infos[0],
            .pTexelBufferView = NULL,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = NULL,
            .dstSet = descriptor_set,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pImageInfo = NULL,
            .pBufferInfo = &buffer_infos[1],
            .pTexelBufferView = NULL,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = NULL,
            .dstSet = descriptor_set,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pImageInfo = NULL,
            .pBufferInfo = &buffer_infos[2],
            .pTexelBufferView = NULL,
        },
    };

    vkUpdateDescriptorSets(device, 3, descriptor_writes, 0, NULL);

    // Begin command buffer recording
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = NULL,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = NULL,
    };

    result = vkBeginCommandBuffer(ctx->command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "GGML Vulkan: Failed to begin command buffer (error %d)\n", result);
        return result;
    }

    ctx->recording = true;

    // Bind compute pipeline
    vkCmdBindPipeline(ctx->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);

    // Bind descriptor sets
    vkCmdBindDescriptorSets(ctx->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline->pipeline_layout, 0, 1, &descriptor_set, 0, NULL);

    // Set push constants
    struct ggml_vk_matmul_push_constants push_constants = {
        .m = m,
        .n = n,
        .n_blocks_per_row = n_blocks,
    };

    vkCmdPushConstants(ctx->command_buffer, pipeline->pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants), &push_constants);

    // Dispatch compute shader
    // Workgroup size is 256 (local_size_x in shader), so dispatch (m + 255) / 256 workgroups
    uint32_t workgroup_count_x = (m + 255) / 256;
    vkCmdDispatch(ctx->command_buffer, workgroup_count_x, 1, 1); // matmul shader dispatch

    // End command buffer recording
    result = vkEndCommandBuffer(ctx->command_buffer);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "GGML Vulkan: Failed to end command buffer (error %d)\n", result);
        ctx->recording = false;
        return result;
    }

    ctx->recording = false;

    // Submit command buffer
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = NULL,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = NULL,
        .pWaitDstStageMask = NULL,
        .commandBufferCount = 1,
        .pCommandBuffers = &ctx->command_buffer,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = NULL,
    };

    result = vkQueueSubmit(ctx->device->compute_queue, 1, &submit_info, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "GGML Vulkan: Failed to submit command buffer (error %d)\n", result);
        return result;
    }

    // Wait for completion (synchronous for now)
    result = vkQueueWaitIdle(ctx->device->compute_queue);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "GGML Vulkan: Failed to wait for queue idle (error %d)\n", result);
        return result;
    }

    // Free descriptor set
    vkFreeDescriptorSets(device, ctx->descriptor_pool, 1, &descriptor_set);

    return VK_SUCCESS;
}

//
// GGML Backend Buffer Interface
//

static const char * ggml_backend_vk_buffer_get_name(ggml_backend_buffer_t buffer) {
    return "Vulkan";
}

static void ggml_backend_vk_buffer_free_buffer(ggml_backend_buffer_t buffer) {
    struct ggml_vk_buffer * vk_buffer = (struct ggml_vk_buffer *)buffer->context;
    ggml_vk_free_buffer(vk_buffer);
    free(vk_buffer);
}

static void * ggml_backend_vk_buffer_get_base(ggml_backend_buffer_t buffer) {
    struct ggml_vk_buffer * vk_buffer = (struct ggml_vk_buffer *)buffer->context;

    if (!vk_buffer->is_host) {
        return NULL;
    }

    if (!vk_buffer->mapped) {
        VkResult result = ggml_vk_map_buffer(vk_buffer);
        if (result != VK_SUCCESS) {
            return NULL;
        }
    }

    return vk_buffer->mapped;
}

static void ggml_backend_vk_buffer_set_tensor(ggml_backend_buffer_t buffer, struct ggml_tensor * tensor,
                                              const void * data, size_t offset, size_t size) {
    struct ggml_vk_buffer * vk_buffer = (struct ggml_vk_buffer *)buffer->context;

    if (!vk_buffer->is_host) {
        fprintf(stderr, "GGML Vulkan: Cannot directly set tensor on device-local buffer\n");
        return;
    }

    void * base = ggml_backend_vk_buffer_get_base(buffer);
    if (!base) {
        return;
    }

    memcpy((char *)base + tensor->data - (char *)base + offset, data, size);
}

static void ggml_backend_vk_buffer_get_tensor(ggml_backend_buffer_t buffer, const struct ggml_tensor * tensor,
                                              void * data, size_t offset, size_t size) {
    struct ggml_vk_buffer * vk_buffer = (struct ggml_vk_buffer *)buffer->context;

    if (!vk_buffer->is_host) {
        fprintf(stderr, "GGML Vulkan: Cannot directly get tensor from device-local buffer\n");
        return;
    }

    void * base = ggml_backend_vk_buffer_get_base(buffer);
    if (!base) {
        return;
    }

    memcpy(data, (const char *)base + tensor->data - (const char *)base + offset, size);
}

static bool ggml_backend_vk_buffer_cpy_tensor(ggml_backend_buffer_t buffer, const struct ggml_tensor * src,
                                             struct ggml_tensor * dst) {
    (void)buffer;
    (void)src;
    (void)dst;
    return false;
}

static void ggml_backend_vk_buffer_clear(ggml_backend_buffer_t buffer, uint8_t value) {
    struct ggml_vk_buffer * vk_buffer = (struct ggml_vk_buffer *)buffer->context;

    if (!vk_buffer->is_host) {
        return;
    }

    void * base = ggml_backend_vk_buffer_get_base(buffer);
    if (base) {
        memset(base, value, vk_buffer->size);
    }
}

static struct ggml_backend_buffer_i ggml_backend_vk_buffer_interface = {
    .get_name        = ggml_backend_vk_buffer_get_name,
    .free_buffer     = ggml_backend_vk_buffer_free_buffer,
    .get_base        = ggml_backend_vk_buffer_get_base,
    .init_tensor     = NULL,
    .memset_tensor   = NULL,
    .set_tensor      = ggml_backend_vk_buffer_set_tensor,
    .get_tensor      = ggml_backend_vk_buffer_get_tensor,
    .cpy_tensor      = ggml_backend_vk_buffer_cpy_tensor,
    .clear           = ggml_backend_vk_buffer_clear,
    .reset           = NULL,
};

//
// GGML Backend Buffer Type Interface
//

static const char * ggml_backend_vk_buffer_type_get_name(ggml_backend_buffer_type_t buft) {
    struct ggml_backend_vk_buffer_type_context * ctx = (struct ggml_backend_vk_buffer_type_context *)buft->context;
    return ctx->is_host ? "Vulkan_Host" : "Vulkan";
}

static ggml_backend_buffer_t ggml_backend_vk_buffer_type_alloc_buffer(ggml_backend_buffer_type_t buft, size_t size) {
    struct ggml_backend_vk_buffer_type_context * ctx = (struct ggml_backend_vk_buffer_type_context *)buft->context;

    if (ctx->device_index >= g_vk_ctx.device_count) {
        fprintf(stderr, "GGML Vulkan: Invalid device index for buffer allocation\n");
        return NULL;
    }

    struct ggml_vk_device * device = &g_vk_ctx.devices[ctx->device_index];
    if (!device->initialized) {
        VkResult result = ggml_vk_init_device(ctx->device_index);
        if (result != VK_SUCCESS) {
            return NULL;
        }
    }

    struct ggml_vk_buffer * vk_buffer = (struct ggml_vk_buffer *)malloc(sizeof(struct ggml_vk_buffer));
    if (!vk_buffer) {
        return NULL;
    }

    VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkMemoryPropertyFlags mem_props = ctx->is_host ?
        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) :
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    VkResult result = ggml_vk_allocate_buffer(device, size, usage, mem_props, vk_buffer);
    if (result != VK_SUCCESS) {
        free(vk_buffer);
        return NULL;
    }

    ggml_backend_buffer_t buffer = (ggml_backend_buffer_t)malloc(sizeof(struct ggml_backend_buffer));
    if (!buffer) {
        ggml_vk_free_buffer(vk_buffer);
        free(vk_buffer);
        return NULL;
    }

    buffer->iface = ggml_backend_vk_buffer_interface;
    buffer->buft = buft;
    buffer->context = vk_buffer;
    buffer->size = size;
    buffer->usage = GGML_BACKEND_BUFFER_USAGE_ANY;

    return buffer;
}

static size_t ggml_backend_vk_buffer_type_get_alignment(ggml_backend_buffer_type_t buft) {
    (void)buft;
    return 128;
}

static size_t ggml_backend_vk_buffer_type_get_max_size(ggml_backend_buffer_type_t buft) {
    (void)buft;
    return SIZE_MAX;
}

static size_t ggml_backend_vk_buffer_type_get_alloc_size(ggml_backend_buffer_type_t buft, const struct ggml_tensor * tensor) {
    (void)buft;
    return ggml_nbytes(tensor);
}

static bool ggml_backend_vk_buffer_type_is_host(ggml_backend_buffer_type_t buft) {
    struct ggml_backend_vk_buffer_type_context * ctx = (struct ggml_backend_vk_buffer_type_context *)buft->context;
    return ctx->is_host;
}

static struct ggml_backend_buffer_type_i ggml_backend_vk_buffer_type_interface = {
    .get_name         = ggml_backend_vk_buffer_type_get_name,
    .alloc_buffer     = ggml_backend_vk_buffer_type_alloc_buffer,
    .get_alignment    = ggml_backend_vk_buffer_type_get_alignment,
    .get_max_size     = ggml_backend_vk_buffer_type_get_max_size,
    .get_alloc_size   = ggml_backend_vk_buffer_type_get_alloc_size,
    .is_host          = ggml_backend_vk_buffer_type_is_host,
};

//
// GGML Backend API Implementation
//

GGML_BACKEND_API int ggml_backend_vk_get_device_count(void) {
    if (!g_vk_ctx.initialized || g_vk_ctx.device_count == 0) {
        VkResult result = ggml_vk_enumerate_devices();
        if (result != VK_SUCCESS) {
            return 0;
        }
    }
    return (int)g_vk_ctx.device_count;
}

GGML_BACKEND_API void ggml_backend_vk_get_device_description(int device, char * description, size_t description_size) {
    if (device < 0 || device >= (int)g_vk_ctx.device_count) {
        snprintf(description, description_size, "Invalid device");
        return;
    }

    if (!g_vk_ctx.initialized || g_vk_ctx.device_count == 0) {
        VkResult result = ggml_vk_enumerate_devices();
        if (result != VK_SUCCESS) {
            snprintf(description, description_size, "Vulkan enumeration failed");
            return;
        }
    }

    const struct ggml_vk_device *dev = &g_vk_ctx.devices[device];
    snprintf(description, description_size, "%s (Vulkan)", dev->properties.deviceName);
}

GGML_BACKEND_API void ggml_backend_vk_get_device_memory(int device, size_t * free, size_t * total) {
    if (device < 0 || device >= (int)g_vk_ctx.device_count) {
        *free = 0;
        *total = 0;
        return;
    }

    if (!g_vk_ctx.initialized || g_vk_ctx.device_count == 0) {
        VkResult result = ggml_vk_enumerate_devices();
        if (result != VK_SUCCESS) {
            *free = 0;
            *total = 0;
            return;
        }
    }

    // Memory query will be implemented in buffer management subtask
    // For now, return placeholder values
    *free = 0;
    *total = 0;
}

GGML_BACKEND_API bool ggml_backend_is_vk(ggml_backend_t backend) {
    // Backend type checking will be implemented in buffer management subtask
    (void)backend;
    return false;
}

GGML_BACKEND_API ggml_backend_buffer_type_t ggml_backend_vk_buffer_type(size_t dev_num) {
    if (dev_num >= g_vk_ctx.device_count) {
        fprintf(stderr, "GGML Vulkan: Invalid device number %zu for buffer type\n", dev_num);
        return NULL;
    }

    static struct ggml_backend_buffer_type vk_buffer_types[GGML_VK_MAX_DEVICES] = { 0 };
    static struct ggml_backend_vk_buffer_type_context vk_buffer_type_contexts[GGML_VK_MAX_DEVICES] = { 0 };

    if (vk_buffer_types[dev_num].iface.get_name == NULL) {
        vk_buffer_type_contexts[dev_num].device_index = dev_num;
        vk_buffer_type_contexts[dev_num].is_host = false;

        vk_buffer_types[dev_num].iface = ggml_backend_vk_buffer_type_interface;
        vk_buffer_types[dev_num].device = NULL;
        vk_buffer_types[dev_num].context = &vk_buffer_type_contexts[dev_num];
    }

    return &vk_buffer_types[dev_num];
}

GGML_BACKEND_API ggml_backend_buffer_type_t ggml_backend_vk_host_buffer_type(void) {
    static struct ggml_backend_buffer_type vk_host_buffer_type = { 0 };
    static struct ggml_backend_vk_buffer_type_context vk_host_buffer_type_context = { 0 };

    if (vk_host_buffer_type.iface.get_name == NULL) {
        vk_host_buffer_type_context.device_index = 0;
        vk_host_buffer_type_context.is_host = true;

        vk_host_buffer_type.iface = ggml_backend_vk_buffer_type_interface;
        vk_host_buffer_type.device = NULL;
        vk_host_buffer_type.context = &vk_host_buffer_type_context;
    }

    return &vk_host_buffer_type;
}

GGML_BACKEND_API ggml_backend_t ggml_backend_vk_init(size_t dev_num) {
    // Ensure devices are enumerated
    if (!g_vk_ctx.initialized || g_vk_ctx.device_count == 0) {
        VkResult result = ggml_vk_enumerate_devices();
        if (result != VK_SUCCESS) {
            fprintf(stderr, "GGML Vulkan: Failed to enumerate devices\n");
            return NULL;
        }
    }

    if (dev_num >= g_vk_ctx.device_count) {
        fprintf(stderr, "GGML Vulkan: Device %zu not found (only %u devices available)\n",
                dev_num, g_vk_ctx.device_count);
        return NULL;
    }

    // Initialize the specific device
    VkResult result = ggml_vk_init_device(dev_num);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "GGML Vulkan: Failed to initialize device %zu\n", dev_num);
        return NULL;
    }

    fprintf(stderr, "GGML Vulkan: Initialized device %zu: %s\n",
            dev_num, g_vk_ctx.devices[dev_num].properties.deviceName);

    // Full backend structure implementation in next subtask
    return NULL;
}

GGML_BACKEND_API ggml_backend_reg_t ggml_backend_vk_reg(void) {
    // Backend registry implementation in next subtask
    return NULL;
}
