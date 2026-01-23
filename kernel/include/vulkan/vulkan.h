/* Vulkan API Header for EmbodIOS Kernel
 * Minimal Vulkan definitions for bare-metal GPU acceleration
 */

#ifndef VULKAN_H_
#define VULKAN_H_

#include <stdint.h>
#include <stddef.h>

/* Vulkan API Version */
#define VK_API_VERSION_1_0 VK_MAKE_VERSION(1, 0, 0)
#define VK_API_VERSION_1_1 VK_MAKE_VERSION(1, 1, 0)
#define VK_API_VERSION_1_2 VK_MAKE_VERSION(1, 2, 0)
#define VK_API_VERSION_1_3 VK_MAKE_VERSION(1, 3, 0)

#define VK_MAKE_VERSION(major, minor, patch) \
    (((major) << 22) | ((minor) << 12) | (patch))

/* Vulkan Handle Types */
#define VK_DEFINE_HANDLE(object) typedef struct object##_T* object;
#define VK_DEFINE_NON_DISPATCHABLE_HANDLE(object) typedef uint64_t object;

/* Core Vulkan Handles */
VK_DEFINE_HANDLE(VkInstance)
VK_DEFINE_HANDLE(VkPhysicalDevice)
VK_DEFINE_HANDLE(VkDevice)
VK_DEFINE_HANDLE(VkQueue)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDeviceMemory)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkBuffer)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkImage)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkCommandPool)
VK_DEFINE_HANDLE(VkCommandBuffer)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkPipeline)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkShaderModule)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDescriptorSet)

/* Vulkan Result Codes */
typedef enum VkResult {
    VK_SUCCESS = 0,
    VK_NOT_READY = 1,
    VK_TIMEOUT = 2,
    VK_EVENT_SET = 3,
    VK_EVENT_RESET = 4,
    VK_INCOMPLETE = 5,
    VK_ERROR_OUT_OF_HOST_MEMORY = -1,
    VK_ERROR_OUT_OF_DEVICE_MEMORY = -2,
    VK_ERROR_INITIALIZATION_FAILED = -3,
    VK_ERROR_DEVICE_LOST = -4,
    VK_ERROR_MEMORY_MAP_FAILED = -5,
    VK_ERROR_LAYER_NOT_PRESENT = -6,
    VK_ERROR_EXTENSION_NOT_PRESENT = -7,
    VK_ERROR_FEATURE_NOT_PRESENT = -8,
    VK_ERROR_INCOMPATIBLE_DRIVER = -9,
    VK_ERROR_TOO_MANY_OBJECTS = -10,
    VK_ERROR_FORMAT_NOT_SUPPORTED = -11,
} VkResult;

/* Vulkan Boolean Type */
typedef uint32_t VkBool32;
#define VK_TRUE  1
#define VK_FALSE 0

/* Vulkan Flags Types */
typedef uint32_t VkFlags;
typedef VkFlags VkInstanceCreateFlags;
typedef VkFlags VkDeviceCreateFlags;
typedef VkFlags VkMemoryPropertyFlags;
typedef VkFlags VkBufferUsageFlags;
typedef VkFlags VkCommandPoolCreateFlags;
typedef VkFlags VkCommandBufferUsageFlags;

/* Memory Property Flags */
#define VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT     0x00000001
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT     0x00000002
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT    0x00000004
#define VK_MEMORY_PROPERTY_HOST_CACHED_BIT      0x00000008

/* Buffer Usage Flags */
#define VK_BUFFER_USAGE_TRANSFER_SRC_BIT        0x00000001
#define VK_BUFFER_USAGE_TRANSFER_DST_BIT        0x00000002
#define VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT      0x00000010
#define VK_BUFFER_USAGE_STORAGE_BUFFER_BIT      0x00000020
#define VK_BUFFER_USAGE_VERTEX_BUFFER_BIT       0x00000080
#define VK_BUFFER_USAGE_INDEX_BUFFER_BIT        0x00000100

/* Vulkan Structure Types */
typedef enum VkStructureType {
    VK_STRUCTURE_TYPE_APPLICATION_INFO = 0,
    VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2,
    VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3,
    VK_STRUCTURE_TYPE_SUBMIT_INFO = 4,
    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5,
    VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12,
    VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16,
    VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO = 29,
} VkStructureType;

/* Vulkan Core Structures */
typedef struct VkApplicationInfo {
    VkStructureType    sType;
    const void*        pNext;
    const char*        pApplicationName;
    uint32_t           applicationVersion;
    const char*        pEngineName;
    uint32_t           engineVersion;
    uint32_t           apiVersion;
} VkApplicationInfo;

typedef struct VkInstanceCreateInfo {
    VkStructureType             sType;
    const void*                 pNext;
    VkInstanceCreateFlags       flags;
    const VkApplicationInfo*    pApplicationInfo;
    uint32_t                    enabledLayerCount;
    const char* const*          ppEnabledLayerNames;
    uint32_t                    enabledExtensionCount;
    const char* const*          ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct VkPhysicalDeviceProperties {
    uint32_t            apiVersion;
    uint32_t            driverVersion;
    uint32_t            vendorID;
    uint32_t            deviceID;
    uint32_t            deviceType;
    char                deviceName[256];
    uint8_t             pipelineCacheUUID[16];
} VkPhysicalDeviceProperties;

typedef struct VkMemoryAllocateInfo {
    VkStructureType    sType;
    const void*        pNext;
    uint64_t           allocationSize;
    uint32_t           memoryTypeIndex;
} VkMemoryAllocateInfo;

typedef struct VkBufferCreateInfo {
    VkStructureType        sType;
    const void*            pNext;
    VkFlags                flags;
    uint64_t               size;
    VkBufferUsageFlags     usage;
    uint32_t               sharingMode;
    uint32_t               queueFamilyIndexCount;
    const uint32_t*        pQueueFamilyIndices;
} VkBufferCreateInfo;

/* Core Vulkan Functions (to be implemented by kernel driver) */
VkResult vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const void*                 pAllocator,
    VkInstance*                 pInstance);

void vkDestroyInstance(
    VkInstance                  instance,
    const void*                 pAllocator);

VkResult vkEnumeratePhysicalDevices(
    VkInstance                  instance,
    uint32_t*                   pPhysicalDeviceCount,
    VkPhysicalDevice*           pPhysicalDevices);

void vkGetPhysicalDeviceProperties(
    VkPhysicalDevice            physicalDevice,
    VkPhysicalDeviceProperties* pProperties);

VkResult vkCreateDevice(
    VkPhysicalDevice            physicalDevice,
    const void*                 pCreateInfo,
    const void*                 pAllocator,
    VkDevice*                   pDevice);

void vkDestroyDevice(
    VkDevice                    device,
    const void*                 pAllocator);

VkResult vkAllocateMemory(
    VkDevice                    device,
    const VkMemoryAllocateInfo* pAllocateInfo,
    const void*                 pAllocator,
    VkDeviceMemory*             pMemory);

void vkFreeMemory(
    VkDevice                    device,
    VkDeviceMemory              memory,
    const void*                 pAllocator);

VkResult vkCreateBuffer(
    VkDevice                    device,
    const VkBufferCreateInfo*   pCreateInfo,
    const void*                 pAllocator,
    VkBuffer*                   pBuffer);

void vkDestroyBuffer(
    VkDevice                    device,
    VkBuffer                    buffer,
    const void*                 pAllocator);

#endif /* VULKAN_H_ */
