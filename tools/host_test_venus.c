/* Host-side validation of the embodiOS Venus protocol layer (WS-A, Maxwell).
 *
 * Links the REAL kernel Venus device code (kernel/drivers/gpu/venus.c) against
 * a mock virtio-gpu backend and a mock Venus renderer that consumes the ring
 * exactly like virglrenderer would:
 *
 *   (a) STRUCTURAL: every record written to the ring is walked with the
 *       vncs_walk_next() structural decoder (from venus_cs.h) — cursor must
 *       land exactly on the record end, all opcodes must be known, and the
 *       init/matmul opcode sequences must match the expected orders.
 *
 *   (b) SEMANTIC: the mock renderer translates the decoded records 1:1 into
 *       real Vulkan calls on the host (lavapipe software driver), writes
 *       genuinely-encoded reply records into the reply shmem, and mirrors
 *       blob memory into vkMapMemory'd buffers. The kernel's
 *       venus_vk_device_init() and venus_vk_matmul() then run a matmul_f32
 *       and a matmul_q4_k_q8_0 whose outputs are compared against the CPU
 *       reference (f32: rel err < 1e-3; Q4_K: bit-exact, same float op
 *       order as the shader).
 *
 * Build/run:  make -C tools venustest   (lavapipe via VK_ICD_FILENAMES)
 *
 * Mock simplifications (documented, not part of the validated surface):
 *   - VkPhysicalDeviceProperties reply encodes real header fields + device
 *     name but zeroed limits/sparse (the guest decoder never reads them).
 *   - vkGetMemoryResourcePropertiesMESA answers with the host's
 *     HOST_VISIBLE|HOST_COHERENT type bits.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include <embodios/venus.h>         /* kernel transport+device API */
#include "venus_cs.h"               /* wire format under test */
#include "../kernel/ai/vk_shaders.h"

/* ============================================================================
 * Minimal Vulkan declarations (same approach as host_test_vulkan.c)
 * ============================================================================ */

#define VK_MAKE_API_VERSION(variant, major, minor, patch) \
    ((((uint32_t)(variant)) << 29U) | (((uint32_t)(major)) << 22U) | \
     (((uint32_t)(minor)) << 12U) | ((uint32_t)(patch)))
#define VK_API_VERSION_1_1 VK_MAKE_API_VERSION(0, 1, 1, 0)

typedef uint32_t VkFlags;
typedef uint32_t VkBool32;
typedef uint64_t VkDeviceSize;
typedef struct VkInstance_T *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
typedef struct VkDevice_T *VkDevice;
typedef struct VkQueue_T *VkQueue;
typedef struct VkCommandBuffer_T *VkCommandBuffer;
typedef uint64_t VkDeviceMemory;
typedef uint64_t VkBuffer;
typedef uint64_t VkShaderModule;
typedef uint64_t VkDescriptorSetLayout;
typedef uint64_t VkPipelineLayout;
typedef uint64_t VkPipeline;
typedef uint64_t VkDescriptorPool;
typedef uint64_t VkDescriptorSet;
typedef uint64_t VkCommandPool;
typedef uint64_t VkFence;

typedef enum VkResult {
    VK_SUCCESS = 0,
    VK_INCOMPLETE = 5,
    VK_TIMEOUT = 2,
    VK_ERROR_OUT_OF_HOST_MEMORY = -1,
    VK_ERROR_OUT_OF_DEVICE_MEMORY = -2,
    VK_ERROR_INITIALIZATION_FAILED = -3,
} VkResult;

typedef enum VkStructureType {
    VK_STRUCTURE_TYPE_APPLICATION_INFO = 0,
    VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2,
    VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3,
    VK_STRUCTURE_TYPE_SUBMIT_INFO = 4,
    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5,
    VK_STRUCTURE_TYPE_FENCE_CREATE_INFO = 8,
    VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12,
    VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16,
    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO = 18,
    VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO = 29,
    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO = 30,
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32,
    VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO = 33,
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO = 34,
    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET = 35,
    VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39,
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40,
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42,
} VkStructureType;

#define VK_QUEUE_COMPUTE_BIT                0x00000002u
#define VK_SHADER_STAGE_COMPUTE_BIT         0x00000020u
#define VK_BUFFER_USAGE_STORAGE_BUFFER_BIT  0x00000020u
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT 0x00000002u
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT 0x00000004u
#define VK_PIPELINE_BIND_POINT_COMPUTE      1
#define VK_SHARING_MODE_EXCLUSIVE           0
#define VK_DESCRIPTOR_TYPE_STORAGE_BUFFER   7
#define VK_WHOLE_SIZE                       (~0ULL)

typedef struct VkApplicationInfo {
    uint32_t sType; const void *pNext;
    const char *pApplicationName; uint32_t applicationVersion;
    const char *pEngineName; uint32_t engineVersion; uint32_t apiVersion;
} VkApplicationInfo;

typedef struct VkInstanceCreateInfo {
    uint32_t sType; const void *pNext; VkFlags flags;
    const VkApplicationInfo *pApplicationInfo;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct VkExtent3D { uint32_t width, height, depth; } VkExtent3D;

typedef struct VkQueueFamilyProperties {
    VkFlags queueFlags; uint32_t queueCount; uint32_t timestampValidBits;
    VkExtent3D minImageTransferGranularity;
} VkQueueFamilyProperties;

/* Full-size physical device properties (real struct incl. limits is ~800B;
 * generous tail so lavapipe can write the whole thing). */
typedef struct VkPhysicalDeviceProperties {
    uint32_t apiVersion, driverVersion, vendorID, deviceID, deviceType;
    char deviceName[256];
    uint8_t pipelineCacheUUID[16];
    uint8_t _tail[2048];
} VkPhysicalDeviceProperties;

typedef struct VkMemoryType { VkFlags propertyFlags; uint32_t heapIndex; }
    VkMemoryType;
typedef struct VkMemoryHeap { VkDeviceSize size; VkFlags flags; } VkMemoryHeap;
typedef struct VkPhysicalDeviceMemoryProperties {
    uint32_t memoryTypeCount; VkMemoryType memoryTypes[32];
    uint32_t memoryHeapCount; VkMemoryHeap memoryHeaps[16];
} VkPhysicalDeviceMemoryProperties;

typedef struct VkDeviceQueueCreateInfo {
    uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t queueFamilyIndex; uint32_t queueCount;
    const float *pQueuePriorities;
} VkDeviceQueueCreateInfo;

typedef struct VkDeviceCreateInfo {
    uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t queueCreateInfoCount; const VkDeviceQueueCreateInfo *pQueueCreateInfos;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
    const void *pEnabledFeatures;
} VkDeviceCreateInfo;

typedef struct VkMemoryRequirements {
    VkDeviceSize size, alignment; uint32_t memoryTypeBits;
} VkMemoryRequirements;

typedef struct VkMemoryAllocateInfo {
    uint32_t sType; const void *pNext;
    VkDeviceSize allocationSize; uint32_t memoryTypeIndex;
} VkMemoryAllocateInfo;

typedef struct VkBufferCreateInfo {
    uint32_t sType; const void *pNext; VkFlags flags;
    VkDeviceSize size; VkFlags usage; uint32_t sharingMode;
    uint32_t queueFamilyIndexCount; const uint32_t *pQueueFamilyIndices;
} VkBufferCreateInfo;

typedef struct VkShaderModuleCreateInfo {
    uint32_t sType; const void *pNext; VkFlags flags;
    size_t codeSize; const uint32_t *pCode;
} VkShaderModuleCreateInfo;

typedef struct VkDescriptorSetLayoutBinding {
    uint32_t binding; uint32_t descriptorType; uint32_t descriptorCount;
    VkFlags stageFlags; const void *pImmutableSamplers;
} VkDescriptorSetLayoutBinding;

typedef struct VkDescriptorSetLayoutCreateInfo {
    uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t bindingCount; const VkDescriptorSetLayoutBinding *pBindings;
} VkDescriptorSetLayoutCreateInfo;

typedef struct VkPushConstantRange {
    VkFlags stageFlags; uint32_t offset; uint32_t size;
} VkPushConstantRange;

typedef struct VkPipelineLayoutCreateInfo {
    uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t setLayoutCount; const VkDescriptorSetLayout *pSetLayouts;
    uint32_t pushConstantRangeCount; const VkPushConstantRange *pPushConstantRanges;
} VkPipelineLayoutCreateInfo;

typedef struct VkPipelineShaderStageCreateInfo {
    uint32_t sType; const void *pNext; VkFlags flags;
    VkFlags stage; VkShaderModule module; const char *pName;
    const void *pSpecializationInfo;
} VkPipelineShaderStageCreateInfo;

typedef struct VkComputePipelineCreateInfo {
    uint32_t sType; const void *pNext; VkFlags flags;
    VkPipelineShaderStageCreateInfo stage; VkPipelineLayout layout;
    uint64_t basePipelineHandle; int32_t basePipelineIndex;
} VkComputePipelineCreateInfo;

typedef struct VkDescriptorPoolSize {
    uint32_t type; uint32_t descriptorCount;
} VkDescriptorPoolSize;

typedef struct VkDescriptorPoolCreateInfo {
    uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t maxSets; uint32_t poolSizeCount;
    const VkDescriptorPoolSize *pPoolSizes;
} VkDescriptorPoolCreateInfo;

typedef struct VkDescriptorSetAllocateInfo {
    uint32_t sType; const void *pNext;
    VkDescriptorPool descriptorPool; uint32_t descriptorSetCount;
    const VkDescriptorSetLayout *pSetLayouts;
} VkDescriptorSetAllocateInfo;

typedef struct VkDescriptorBufferInfo {
    VkBuffer buffer; VkDeviceSize offset; VkDeviceSize range;
} VkDescriptorBufferInfo;

typedef struct VkWriteDescriptorSet {
    uint32_t sType; const void *pNext;
    VkDescriptorSet dstSet; uint32_t dstBinding; uint32_t dstArrayElement;
    uint32_t descriptorCount; uint32_t descriptorType;
    const void *pImageInfo; const VkDescriptorBufferInfo *pBufferInfo;
    const void *pTexelBufferView;
} VkWriteDescriptorSet;

typedef struct VkCommandPoolCreateInfo {
    uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t queueFamilyIndex;
} VkCommandPoolCreateInfo;

typedef struct VkCommandBufferAllocateInfo {
    uint32_t sType; const void *pNext;
    VkCommandPool commandPool; uint32_t level; uint32_t commandBufferCount;
} VkCommandBufferAllocateInfo;

typedef struct VkCommandBufferBeginInfo {
    uint32_t sType; const void *pNext; VkFlags flags;
    const void *pInheritanceInfo;
} VkCommandBufferBeginInfo;

typedef struct VkSubmitInfo {
    uint32_t sType; const void *pNext;
    uint32_t waitSemaphoreCount; const void *pWaitSemaphores;
    const void *pWaitDstStageMask;
    uint32_t commandBufferCount; const VkCommandBuffer *pCommandBuffers;
    uint32_t signalSemaphoreCount; const void *pSignalSemaphores;
} VkSubmitInfo;

typedef struct VkFenceCreateInfo {
    uint32_t sType; const void *pNext; VkFlags flags;
} VkFenceCreateInfo;

VkResult vkCreateInstance(const VkInstanceCreateInfo *, const void *, VkInstance *);
void vkDestroyInstance(VkInstance, const void *);
VkResult vkEnumeratePhysicalDevices(VkInstance, uint32_t *, VkPhysicalDevice *);
void vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties *);
void vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t *,
                                              VkQueueFamilyProperties *);
void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice,
                                         VkPhysicalDeviceMemoryProperties *);
VkResult vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *,
                        const void *, VkDevice *);
void vkDestroyDevice(VkDevice, const void *);
void vkGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue *);
VkResult vkCreateBuffer(VkDevice, const VkBufferCreateInfo *, const void *,
                        VkBuffer *);
void vkDestroyBuffer(VkDevice, VkBuffer, const void *);
void vkGetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements *);
VkResult vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo *, const void *,
                          VkDeviceMemory *);
void vkFreeMemory(VkDevice, VkDeviceMemory, const void *);
VkResult vkBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
VkResult vkMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize,
                     VkFlags, void **);
void vkUnmapMemory(VkDevice, VkDeviceMemory);
VkResult vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo *,
                              const void *, VkShaderModule *);
void vkDestroyShaderModule(VkDevice, VkShaderModule, const void *);
VkResult vkCreateDescriptorSetLayout(VkDevice,
    const VkDescriptorSetLayoutCreateInfo *, const void *, VkDescriptorSetLayout *);
VkResult vkCreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo *,
                                const void *, VkPipelineLayout *);
VkResult vkCreateComputePipelines(VkDevice, uint64_t, uint32_t,
                                  const VkComputePipelineCreateInfo *,
                                  const void *, VkPipeline *);
void vkDestroyPipeline(VkDevice, VkPipeline, const void *);
VkResult vkCreateDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo *,
                                const void *, VkDescriptorPool *);
VkResult vkAllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo *,
                                  VkDescriptorSet *);
void vkUpdateDescriptorSets(VkDevice, uint32_t, const VkWriteDescriptorSet *,
                            uint32_t, const void *);
VkResult vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo *,
                             const void *, VkCommandPool *);
VkResult vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo *,
                                  VkCommandBuffer *);
VkResult vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo *);
void vkCmdBindPipeline(VkCommandBuffer, uint32_t, VkPipeline);
void vkCmdBindDescriptorSets(VkCommandBuffer, uint32_t, VkPipelineLayout,
                             uint32_t, uint32_t, const VkDescriptorSet *,
                             uint32_t, const uint32_t *);
void vkCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkFlags, uint32_t,
                        uint32_t, const void *);
void vkCmdDispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t);
VkResult vkEndCommandBuffer(VkCommandBuffer);
VkResult vkQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo *, VkFence);
VkResult vkCreateFence(VkDevice, const VkFenceCreateInfo *, const void *,
                       VkFence *);
VkResult vkResetFences(VkDevice, uint32_t, const VkFence *);
VkResult vkGetFenceStatus(VkDevice, VkFence);
VkResult vkWaitForFences(VkDevice, uint32_t, const VkFence *, VkBool32,
                         uint64_t);

/* ============================================================================
 * Mock kernel services consumed by venus.c
 * ============================================================================ */

void console_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
}

bool arch_identity_map_mmio(uint64_t base, uint64_t size)
{
    (void)base; (void)size;
    return true;    /* host: every address is already mapped */
}

/* ---- fake shared-memory window: blob contents live in one arena so that
 * hostmem_base + map_info IS the guest pointer (identity mapping) ---- */

#define MOCK_HOSTMEM_SIZE (64u << 20)
static uint8_t *g_hostmem;          /* base of the fake window */
static size_t g_hostmem_off;

typedef struct mock_blob {
    uint32_t res_id;
    uint64_t size;
    uint8_t *data;                  /* inside g_hostmem */
    /* renderer-side Vulkan memory mirroring (imported blobs) */
    VkDeviceMemory vk_mem;
    void *vk_mapped;
    bool in_use;
} mock_blob_t;

#define MOCK_MAX_BLOBS 64
static mock_blob_t g_blobs[MOCK_MAX_BLOBS];

static mock_blob_t *blob_find(uint32_t res_id)
{
    for (int i = 0; i < MOCK_MAX_BLOBS; i++) {
        if (g_blobs[i].in_use && g_blobs[i].res_id == res_id) {
            return &g_blobs[i];
        }
    }
    return NULL;
}

/* ---- mock virtio_gpu entry points used by venus.c ---- */

static const virtio_gpu_capset_t g_mock_capset = {
    .id = VIRTIO_GPU_CAPSET_VENUS, .max_version = 1,
    .max_size = sizeof(struct venus_capset),
};

const virtio_gpu_capset_t *virtio_gpu_find_capset(uint32_t capset_id,
                                                  uint32_t *out_index)
{
    if (capset_id != VIRTIO_GPU_CAPSET_VENUS) {
        return NULL;
    }
    if (out_index) {
        *out_index = 0;
    }
    return &g_mock_capset;
}

int virtio_gpu_get_capset(uint32_t capset_index, uint32_t version,
                          void *buf, uint32_t size)
{
    (void)capset_index; (void)version;
    struct venus_capset cs = {
        .wire_format_version = 1,
        .vk_xml_version = 357,
        .vk_ext_command_stream_version = 1,
    };
    memcpy(buf, &cs, size < sizeof(cs) ? size : sizeof(cs));
    return 0;
}

uint32_t virtio_gpu_features(void)
{
    return VIRTIO_GPU_F_CONTEXT_INIT_MASK;
}

bool virtio_gpu_has_blob(void) { return true; }

bool virtio_gpu_hostmem_window(uint64_t *base, uint64_t *size)
{
    if (base) *base = (uint64_t)(uintptr_t)g_hostmem;
    if (size) *size = MOCK_HOSTMEM_SIZE;
    return true;
}

int virtio_gpu_ctrl_send(const void *req, uint32_t req_len,
                         void *resp, uint32_t resp_len, uint64_t *fence_id)
{
    (void)req_len; (void)fence_id;
    const struct virtio_gpu_ctrl_hdr *hdr = req;
    if (hdr->type != VIRTIO_GPU_CMD_CTX_CREATE &&
        hdr->type != VIRTIO_GPU_CMD_CTX_DESTROY) {
        fprintf(stderr, "MOCK: unexpected ctrl cmd 0x%x\n", hdr->type);
        return VIRTIO_GPU_ERR_DEVICE;
    }
    if (resp && resp_len >= sizeof(*hdr)) {
        struct virtio_gpu_ctrl_hdr *r = resp;
        memset(r, 0, sizeof(*r));
        r->type = VIRTIO_GPU_RESP_OK_NODATA;
    }
    return VIRTIO_GPU_OK;
}

int virtio_gpu_resource_create_blob(uint32_t resource_id, uint32_t blob_mem,
                                    uint32_t blob_flags, uint64_t blob_id,
                                    uint64_t size)
{
    (void)blob_mem; (void)blob_flags; (void)blob_id;
    for (int i = 0; i < MOCK_MAX_BLOBS; i++) {
        if (!g_blobs[i].in_use) {
            if (g_hostmem_off + size > MOCK_HOSTMEM_SIZE) {
                fprintf(stderr, "MOCK: hostmem arena exhausted\n");
                return VIRTIO_GPU_ERR_NO_MEMORY;
            }
            g_blobs[i].res_id = resource_id;
            g_blobs[i].size = size;
            g_blobs[i].data = g_hostmem + g_hostmem_off;
            g_blobs[i].vk_mem = 0;
            g_blobs[i].vk_mapped = NULL;
            g_blobs[i].in_use = true;
            g_hostmem_off += (size_t)((size + 4095) & ~4095ULL);
            memset(g_blobs[i].data, 0, size);
            return VIRTIO_GPU_OK;
        }
    }
    return VIRTIO_GPU_ERR_NO_MEMORY;
}

int virtio_gpu_resource_attach_backing(uint32_t resource_id,
                                       const void *entries,
                                       uint32_t nr_entries)
{
    (void)resource_id; (void)entries; (void)nr_entries;
    fprintf(stderr, "MOCK: attach_backing unexpected (host-allocated blobs)\n");
    return VIRTIO_GPU_ERR_DEVICE;
}

int virtio_gpu_resource_map_blob(uint32_t resource_id, uint64_t offset,
                                 uint64_t *map_info)
{
    (void)offset;
    mock_blob_t *b = blob_find(resource_id);
    if (!b) {
        return VIRTIO_GPU_ERR_INVALID;
    }
    *map_info = (uint64_t)(b->data - g_hostmem);
    return VIRTIO_GPU_OK;
}

int virtio_gpu_resource_unmap_blob(uint32_t resource_id)
{
    return blob_find(resource_id) ? VIRTIO_GPU_OK : VIRTIO_GPU_ERR_INVALID;
}

int virtio_gpu_resource_unref(uint32_t resource_id)
{
    mock_blob_t *b = blob_find(resource_id);
    if (!b) {
        return VIRTIO_GPU_ERR_INVALID;
    }
    b->in_use = false;
    return VIRTIO_GPU_OK;
}

/* ============================================================================
 * Mock Venus renderer
 *
 * Consumes ring records like virglrenderer: decodes each record, executes
 * the equivalent real Vulkan call on lavapipe, and writes reply records
 * (venus-protocol reply wire format) into the reply shmem blob.
 * ============================================================================ */

/* ---- renderer state ---- */

static struct {
    bool ring_ok;
    uint64_t ring_id;
    mock_blob_t *ring_blob;
    uint64_t buffer_off, buffer_size;

    /* reply target (vkSetReplyCommandStreamMESA) */
    mock_blob_t *reply_blob;
    uint64_t reply_off;

    /* real Vulkan objects */
    VkInstance instance;
    VkPhysicalDevice phys[8];
    uint32_t phys_count;
    VkDevice device;
    VkQueue queue;
} R;

/* id -> real Vulkan handle map */
typedef struct { uint64_t id; uint64_t handle; } idmap_t;
static idmap_t g_idmap[4096];
static uint32_t g_idmap_count;

static void idmap_set(uint64_t id, uint64_t handle)
{
    if (g_idmap_count >= 4096) {
        fprintf(stderr, "MOCK: idmap full\n");
        exit(2);
    }
    g_idmap[g_idmap_count].id = id;
    g_idmap[g_idmap_count].handle = handle;
    g_idmap_count++;
}

static uint64_t idmap_get(uint64_t id)
{
    for (uint32_t i = 0; i < g_idmap_count; i++) {
        if (g_idmap[i].id == id) {
            return g_idmap[i].handle;
        }
    }
    fprintf(stderr, "MOCK: unknown object id 0x%llx\n",
            (unsigned long long)id);
    exit(2);
}

/* buffer id -> renderer bookkeeping (for blob<->vkMapMemory mirroring) */
typedef struct {
    uint64_t buffer_id;         /* guest id */
    VkBuffer buffer;
    VkDeviceMemory mem;
    uint64_t size;
    mock_blob_t *blob;          /* imported resource */
} mock_vkbuf_t;

static mock_vkbuf_t g_vkbufs[64];
static uint32_t g_vkbuf_count;

static mock_vkbuf_t *vkbuf_by_id(uint64_t id)
{
    for (uint32_t i = 0; i < g_vkbuf_count; i++) {
        if (g_vkbufs[i].buffer_id == id) {
            return &g_vkbufs[i];
        }
    }
    return NULL;
}

/* mem id (guest) -> blob resource, set at vkAllocateMemory(import) */
typedef struct { uint64_t mem_id; mock_blob_t *blob; } memmap_t;
static memmap_t g_memmap[64];
static uint32_t g_memmap_count;

/* ---- opcode recording for the structural check ---- */

#define MOCK_MAX_OPCODES 4096
static uint32_t g_opcodes[MOCK_MAX_OPCODES];
static uint32_t g_opcode_count;

/* ---- reply writer ---- */

typedef struct {
    uint8_t *cur;
    uint8_t *end;
    bool fatal;
} rw_t;

static rw_t g_rw;

static void reply_begin(void)
{
    if (!R.reply_blob) {
        fprintf(stderr, "MOCK: reply without vkSetReplyCommandStreamMESA\n");
        exit(2);
    }
    g_rw.cur = R.reply_blob->data + R.reply_off;
    g_rw.end = R.reply_blob->data + R.reply_blob->size;
    g_rw.fatal = false;
}

static void rw_u32(uint32_t v)
{
    if ((size_t)(g_rw.end - g_rw.cur) < 4) { g_rw.fatal = true; return; }
    memcpy(g_rw.cur, &v, 4); g_rw.cur += 4;
}

static void rw_u64(uint64_t v)
{
    if ((size_t)(g_rw.end - g_rw.cur) < 8) { g_rw.fatal = true; return; }
    memcpy(g_rw.cur, &v, 8); g_rw.cur += 8;
}

static void rw_bytes(const void *p, size_t n)
{
    size_t wire = (n + 3) & ~(size_t)3;
    if ((size_t)(g_rw.end - g_rw.cur) < wire) { g_rw.fatal = true; return; }
    memcpy(g_rw.cur, p, n);
    if (wire > n) memset(g_rw.cur + n, 0, wire - n);
    g_rw.cur += wire;
}

/* reply: {type, ret, ptr=1, handle} */
static void reply_handle(uint32_t type, VkResult ret, uint64_t handle)
{
    reply_begin();
    rw_u32(type); rw_u32((uint32_t)ret); rw_u64(1); rw_u64(handle);
}

/* reply: {type, ret} */
static void reply_ret(uint32_t type, VkResult ret)
{
    reply_begin();
    rw_u32(type); rw_u32((uint32_t)ret);
}

/* VkPhysicalDeviceLimits wire size: 436 bytes of scalars + 6 fixed arrays
 * with 8-byte array_size prefixes (u32[3], u32[3], u32[2], f32[2] x3).
 * The mock encodes zeros — the guest decoder only reads the header fields
 * and deviceName, and skips this region by size. */
static void rw_limits_zeros(void)
{
    uint8_t zero[436] = {0};
    /* scalar prefix block: fields up to maxComputeWorkGroupCount */
    rw_bytes(zero, 216);
    rw_u64(3); rw_bytes(zero, 12);      /* maxComputeWorkGroupCount[3] */
    rw_bytes(zero, 4);                  /* maxComputeWorkGroupInvocations */
    rw_u64(3); rw_bytes(zero, 12);      /* maxComputeWorkGroupSize[3] */
    rw_bytes(zero, 32);                 /* fields 56..63 (8 x u32) */
    rw_u64(2); rw_bytes(zero, 8);       /* maxViewportDimensions[2] */
    rw_u64(2); rw_bytes(zero, 8);       /* viewportBoundsRange[2] */
    rw_bytes(zero, 4);                  /* viewportSubPixelBits */
    rw_bytes(zero, 32);                 /* min*Alignment (4 x u64) */
    rw_bytes(zero, 108);                /* fields 71..97 (27 x u32) */
    rw_u64(2); rw_bytes(zero, 8);       /* pointSizeRange[2] */
    rw_u64(2); rw_bytes(zero, 8);       /* lineWidthRange[2] */
    rw_bytes(zero, 32);                 /* granularity/strict/standard */
    rw_bytes(zero, 24);                 /* 3 x u64 */
    /* sparse properties: 5 x VkBool32 */
    rw_bytes(zero, 20);
}

/* ============================================================================
 * Record dispatch (decoder + real Vulkan execution + reply)
 * ============================================================================ */

static void mock_exec(vncs_decoder_t *d, uint32_t type, uint32_t flags)
{
    bool want_reply = (flags & VNCS_COMMAND_GENERATE_REPLY_BIT) != 0;
    (void)want_reply;

    switch (type) {
    case VNCS_CMD_vkCreateInstance_EXT: {
        /* decode */
        if (!vncr_ptr(d)) goto bad;
        if (vncr_i32(d) != VNCS_STYPE_INSTANCE_CREATE_INFO) goto bad;
        vncr_ptr(d);                    /* pNext */
        vncr_u32(d);                    /* flags */
        char app_name[64] = {0}, eng_name[64] = {0};
        uint32_t app_ver = 0, eng_ver = 0, api = 0;
        if (vncr_ptr(d)) {
            if (vncr_i32(d) != VNCS_STYPE_APPLICATION_INFO) goto bad;
            vncr_ptr(d);
            uint64_t alen = vncr_u64(d);
            if (alen >= sizeof(app_name)) goto bad;
            vncs_read(d, (alen + 3) & ~3ULL, app_name, (size_t)alen);
            app_ver = vncr_u32(d);
            uint64_t elen = vncr_u64(d);
            if (elen >= sizeof(eng_name)) goto bad;
            vncs_read(d, (elen + 3) & ~3ULL, eng_name, (size_t)elen);
            eng_ver = vncr_u32(d);
            api = vncr_u32(d);
        }
        vncr_u32(d); vncr_u64(d);       /* layers */
        vncr_u32(d); vncr_u64(d);       /* extensions */
        vncr_ptr(d);                    /* pAllocator */
        if (!vncr_ptr(d)) goto bad;
        uint64_t out_id = vncr_handle(d);
        if (d->fatal) goto bad;

        VkApplicationInfo ai = {
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = app_name, .applicationVersion = app_ver,
            .pEngineName = eng_name, .engineVersion = eng_ver,
            .apiVersion = api,
        };
        VkInstanceCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &ai,
        };
        VkInstance inst = NULL;
        VkResult r = vkCreateInstance(&ici, NULL, &inst);
        if (r == VK_SUCCESS) {
            R.instance = inst;
            idmap_set(out_id, (uint64_t)(uintptr_t)inst);
        }
        reply_handle(type, r, out_id);
        return;
    }

    case VNCS_CMD_vkEnumeratePhysicalDevices_EXT: {
        uint64_t inst_id = vncr_handle(d);
        uint32_t count __attribute__((unused)) = 0;
        if (!vncr_ptr(d)) goto bad;
        count = vncr_u32(d);
        uint64_t arrn = vncr_u64(d);
        uint64_t ids[8] = {0};
        for (uint64_t i = 0; i < arrn; i++) {
            uint64_t id = vncr_handle(d);
            if (i < 8) ids[i] = id;
        }
        if (d->fatal || arrn > 8) goto bad;
        VkInstance inst = (VkInstance)(uintptr_t)idmap_get(inst_id);

        VkResult r;
        if (arrn == 0) {
            uint32_t rc = 0;
            r = vkEnumeratePhysicalDevices(inst, &rc, NULL);
            R.phys_count = rc;
            reply_begin();
            rw_u32(type); rw_u32((uint32_t)r);
            rw_u64(1); rw_u32(rc);
            rw_u64(0);                  /* no array */
        } else {
            uint32_t rc = (uint32_t)arrn;
            r = vkEnumeratePhysicalDevices(inst, &rc, R.phys);
            if (r == VK_INCOMPLETE) r = VK_SUCCESS;
            for (uint32_t i = 0; i < rc; i++) {
                idmap_set(ids[i], (uint64_t)(uintptr_t)R.phys[i]);
            }
            reply_begin();
            rw_u32(type); rw_u32((uint32_t)r);
            rw_u64(1); rw_u32(rc);
            rw_u64(rc);
            for (uint32_t i = 0; i < rc; i++) rw_u64(ids[i]);
        }
        return;
    }

    case VNCS_CMD_vkGetPhysicalDeviceProperties_EXT: {
        uint64_t phys_id = vncr_handle(d);
        if (!vncr_ptr(d)) goto bad;
        VkPhysicalDevice pd = (VkPhysicalDevice)(uintptr_t)idmap_get(phys_id);
        VkPhysicalDeviceProperties props;
        memset(&props, 0, sizeof(props));
        vkGetPhysicalDeviceProperties(pd, &props);
        /* reply: {type, ptr=1, props} — no VkResult (void command) */
        reply_begin();
        rw_u32(type); rw_u64(1);
        rw_u32(props.apiVersion); rw_u32(props.driverVersion);
        rw_u32(props.vendorID); rw_u32(props.deviceID);
        rw_u32(props.deviceType);
        rw_u64(256); rw_bytes(props.deviceName, 256);
        rw_u64(16); rw_bytes(props.pipelineCacheUUID, 16);
        rw_limits_zeros();
        return;
    }

    case VNCS_CMD_vkGetPhysicalDeviceQueueFamilyProperties_EXT: {
        uint64_t phys_id = vncr_handle(d);
        uint32_t count __attribute__((unused)) = 0;
        if (!vncr_ptr(d)) goto bad;
        count = vncr_u32(d);
        uint64_t arrn = vncr_u64(d);
        if (d->fatal) goto bad;
        VkPhysicalDevice pd = (VkPhysicalDevice)(uintptr_t)idmap_get(phys_id);
        if (arrn == 0) {
            uint32_t rc = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &rc, NULL);
            reply_begin();
            rw_u32(type); rw_u64(1); rw_u32(rc); rw_u64(0);
        } else {
            VkQueueFamilyProperties *qp =
                calloc(arrn, sizeof(*qp));
            uint32_t rc = (uint32_t)arrn;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &rc, qp);
            reply_begin();
            rw_u32(type); rw_u64(1); rw_u32(rc); rw_u64(rc);
            for (uint32_t i = 0; i < rc; i++) {
                rw_u32(qp[i].queueFlags); rw_u32(qp[i].queueCount);
                rw_u32(qp[i].timestampValidBits);
                rw_u32(qp[i].minImageTransferGranularity.width);
                rw_u32(qp[i].minImageTransferGranularity.height);
                rw_u32(qp[i].minImageTransferGranularity.depth);
            }
            free(qp);
        }
        return;
    }

    case VNCS_CMD_vkGetPhysicalDeviceMemoryProperties_EXT: {
        uint64_t phys_id = vncr_handle(d);
        if (!vncr_ptr(d)) goto bad;
        vncr_u64(d); vncr_u64(d);       /* partial array_size words */
        VkPhysicalDevice pd = (VkPhysicalDevice)(uintptr_t)idmap_get(phys_id);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        reply_begin();
        rw_u32(type); rw_u64(1);
        rw_u32(mp.memoryTypeCount);
        rw_u64(32);
        for (int i = 0; i < 32; i++) {
            rw_u32(mp.memoryTypes[i].propertyFlags);
            rw_u32(mp.memoryTypes[i].heapIndex);
        }
        rw_u32(mp.memoryHeapCount);
        rw_u64(16);
        for (int i = 0; i < 16; i++) {
            rw_u64(mp.memoryHeaps[i].size);
            rw_u32(mp.memoryHeaps[i].flags);
        }
        return;
    }

    case VNCS_CMD_vkCreateDevice_EXT: {
        uint64_t phys_id = vncr_handle(d);
        if (!vncr_ptr(d)) goto bad;
        if (vncr_i32(d) != VNCS_STYPE_DEVICE_CREATE_INFO) goto bad;
        vncr_ptr(d);                    /* pNext */
        vncr_u32(d);                    /* flags */
        uint32_t qn = vncr_u32(d);
        uint64_t qarr = vncr_u64(d);
        if (qarr != qn || qn != 1) goto bad;
        if (vncr_i32(d) != VNCS_STYPE_DEVICE_QUEUE_CREATE_INFO) goto bad;
        vncr_ptr(d);
        vncr_u32(d);                    /* flags */
        uint32_t qfi = vncr_u32(d);
        uint32_t qc = vncr_u32(d);
        uint64_t parr = vncr_u64(d);
        float prio = 1.0f;
        if (parr) vncs_read(d, 4, &prio, 4);
        vncr_u32(d); vncr_u64(d);       /* layers */
        vncr_u32(d); vncr_u64(d);       /* extensions */
        vncr_ptr(d);                    /* pEnabledFeatures */
        vncr_ptr(d);                    /* pAllocator */
        if (!vncr_ptr(d)) goto bad;
        uint64_t out_id = vncr_handle(d);
        if (d->fatal) goto bad;

        VkPhysicalDevice pd = (VkPhysicalDevice)(uintptr_t)idmap_get(phys_id);
        VkDeviceQueueCreateInfo qci = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = qfi, .queueCount = qc,
            .pQueuePriorities = &prio,
        };
        VkDeviceCreateInfo dci = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        };
        VkDevice dev = NULL;
        VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
        if (r == VK_SUCCESS) {
            R.device = dev;
            idmap_set(out_id, (uint64_t)(uintptr_t)dev);
        }
        reply_handle(type, r, out_id);
        return;
    }

    case VNCS_CMD_vkGetDeviceQueue_EXT: {
        uint64_t dev_id = vncr_handle(d);
        uint32_t qfi = vncr_u32(d);
        uint32_t qi = vncr_u32(d);
        if (!vncr_ptr(d)) goto bad;
        uint64_t out_id = vncr_handle(d);
        if (d->fatal) goto bad;
        VkQueue q = NULL;
        vkGetDeviceQueue((VkDevice)(uintptr_t)idmap_get(dev_id), qfi, qi, &q);
        R.queue = q;
        idmap_set(out_id, (uint64_t)(uintptr_t)q);
        return;                         /* void: no reply */
    }

    case VNCS_CMD_vkCreateBuffer_EXT: {
        vncr_handle(d);                 /* device */
        if (!vncr_ptr(d)) goto bad;
        if (vncr_i32(d) != VNCS_STYPE_BUFFER_CREATE_INFO) goto bad;
        vncr_ptr(d);
        vncr_u32(d);                    /* flags */
        uint64_t size = vncr_u64(d);
        uint32_t usage = vncr_u32(d);
        vncr_i32(d);                    /* sharingMode */
        vncr_u32(d);                    /* queueFamilyIndexCount */
        vncr_u64(d);                    /* array */
        vncr_ptr(d);                    /* pAllocator */
        if (!vncr_ptr(d)) goto bad;
        uint64_t out_id = vncr_handle(d);
        if (d->fatal) goto bad;

        VkBufferCreateInfo bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size, .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VkBuffer buf = 0;
        VkResult r = vkCreateBuffer(R.device, &bci, NULL, &buf);
        if (r == VK_SUCCESS) {
            idmap_set(out_id, buf);
            g_vkbufs[g_vkbuf_count].buffer_id = out_id;
            g_vkbufs[g_vkbuf_count].buffer = buf;
            g_vkbufs[g_vkbuf_count].mem = 0;
            g_vkbufs[g_vkbuf_count].size = size;
            g_vkbufs[g_vkbuf_count].blob = NULL;
            g_vkbuf_count++;
        }
        reply_handle(type, r, out_id);
        return;
    }

    case VNCS_CMD_vkGetBufferMemoryRequirements_EXT: {
        vncr_handle(d);                 /* device */
        uint64_t buf_id = vncr_handle(d);
        if (!vncr_ptr(d)) goto bad;
        if (d->fatal) goto bad;
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(R.device,
                                      (VkBuffer)idmap_get(buf_id), &mr);
        reply_begin();                  /* void command: no VkResult */
        rw_u32(type); rw_u64(1);
        rw_u64(mr.size); rw_u64(mr.alignment); rw_u32(mr.memoryTypeBits);
        return;
    }

    case VNCS_CMD_vkGetMemoryResourcePropertiesMESA_EXT: {
        vncr_handle(d);                 /* device */
        uint32_t res_id = vncr_u32(d);
        if (!vncr_ptr(d)) goto bad;
        if (vncr_i32(d) != VNCS_STYPE_MEMORY_RESOURCE_PROPERTIES_MESA) goto bad;
        vncr_ptr(d);
        if (d->fatal) goto bad;
        if (!blob_find(res_id)) goto bad;
        /* answer: any HOST_VISIBLE|HOST_COHERENT memory type of the phys dev */
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(R.phys[0], &mp);
        uint32_t bits = 0;
        for (uint32_t i = 0; i < mp.memoryTypeCount && i < 32; i++) {
            uint32_t f = mp.memoryTypes[i].propertyFlags;
            if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                bits |= 1u << i;
            }
        }
        reply_begin();
        rw_u32(type); rw_u32(VK_SUCCESS); rw_u64(1);
        rw_u32(VNCS_STYPE_MEMORY_RESOURCE_PROPERTIES_MESA);
        rw_u64(0);                      /* pNext = NULL */
        rw_u32(bits);
        return;
    }

    case VNCS_CMD_vkAllocateMemory_EXT: {
        vncr_handle(d);                 /* device */
        if (!vncr_ptr(d)) goto bad;
        if (vncr_i32(d) != VNCS_STYPE_MEMORY_ALLOCATE_INFO) goto bad;
        uint32_t import_res = 0;
        if (vncr_ptr(d)) {              /* pNext: VkImportMemoryResourceInfoMESA */
            if (vncr_i32(d) != VNCS_STYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA)
                goto bad;
            vncr_ptr(d);
            import_res = vncr_u32(d);
        }
        uint64_t alloc_size = vncr_u64(d);
        uint32_t type_index = vncr_u32(d);
        vncr_ptr(d);                    /* pAllocator */
        if (!vncr_ptr(d)) goto bad;
        uint64_t out_id = vncr_handle(d);
        if (d->fatal) goto bad;

        mock_blob_t *blob = import_res ? blob_find(import_res) : NULL;
        if (import_res && !blob) goto bad;
        /* real allocation (host memory stands in for the shared blob) */
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = alloc_size, .memoryTypeIndex = type_index,
        };
        VkDeviceMemory mem = 0;
        VkResult r = vkAllocateMemory(R.device, &mai, NULL, &mem);
        if (r == VK_SUCCESS) {
            idmap_set(out_id, mem);
            if (blob) {
                void *mapped = NULL;
                r = vkMapMemory(R.device, mem, 0, alloc_size, 0, &mapped);
                if (r != VK_SUCCESS) goto bad;
                blob->vk_mem = mem;
                blob->vk_mapped = mapped;
                g_memmap[g_memmap_count].mem_id = out_id;
                g_memmap[g_memmap_count].blob = blob;
                g_memmap_count++;
            }
        }
        reply_handle(type, r, out_id);
        return;
    }

    case VNCS_CMD_vkBindBufferMemory_EXT: {
        vncr_handle(d);                 /* device */
        uint64_t buf_id = vncr_handle(d);
        uint64_t mem_id = vncr_handle(d);
        uint64_t offset = vncr_u64(d);
        if (d->fatal) goto bad;
        VkResult r = vkBindBufferMemory(R.device, (VkBuffer)idmap_get(buf_id),
                                        (VkDeviceMemory)idmap_get(mem_id),
                                        offset);
        mock_vkbuf_t *vb = vkbuf_by_id(buf_id);
        if (vb) {
            vb->mem = (VkDeviceMemory)idmap_get(mem_id);
            for (uint32_t i = 0; i < g_memmap_count; i++) {
                if (g_memmap[i].mem_id == mem_id) {
                    vb->blob = g_memmap[i].blob;
                }
            }
        }
        reply_ret(type, r);
        return;
    }

    case VNCS_CMD_vkCreateShaderModule_EXT: {
        vncr_handle(d);                 /* device */
        if (!vncr_ptr(d)) goto bad;
        if (vncr_i32(d) != VNCS_STYPE_SHADER_MODULE_CREATE_INFO) goto bad;
        vncr_ptr(d);
        vncr_u32(d);                    /* flags */
        uint64_t code_size = vncr_u64(d);
        uint64_t words = vncr_u64(d);
        if (words * 4 != code_size) goto bad;
        /* code follows inline in the record */
        const uint32_t *code = (const uint32_t *)d->cur;
        if ((size_t)(d->end - d->cur) < words * 4) goto bad;
        vncs_skip(d, words * 4);
        vncr_ptr(d);                    /* pAllocator */
        if (!vncr_ptr(d)) goto bad;
        uint64_t out_id = vncr_handle(d);
        if (d->fatal) goto bad;

        VkShaderModuleCreateInfo smi = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = code_size, .pCode = code,
        };
        VkShaderModule mod = 0;
        VkResult r = vkCreateShaderModule(R.device, &smi, NULL, &mod);
        if (r == VK_SUCCESS) {
            idmap_set(out_id, mod);
        }
        reply_handle(type, r, out_id);
        return;
    }

    case VNCS_CMD_vkCreateDescriptorSetLayout_EXT: {
        vncr_handle(d);
        if (!vncr_ptr(d)) goto bad;
        if (vncr_i32(d) != VNCS_STYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO) goto bad;
        vncr_ptr(d);
        vncr_u32(d);
        uint32_t bn = vncr_u32(d);
        uint64_t barr = vncr_u64(d);
        if (barr != bn || bn > 8) goto bad;
        VkDescriptorSetLayoutBinding binds[8];
        for (uint32_t i = 0; i < bn; i++) {
            binds[i].binding = vncr_u32(d);
            binds[i].descriptorType = (uint32_t)vncr_i32(d);
            binds[i].descriptorCount = vncr_u32(d);
            binds[i].stageFlags = vncr_u32(d);
            vncr_u64(d);                /* immutable samplers */
        }
        vncr_ptr(d);
        if (!vncr_ptr(d)) goto bad;
        uint64_t out_id = vncr_handle(d);
        if (d->fatal) goto bad;

        VkDescriptorSetLayoutCreateInfo li = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = bn, .pBindings = binds,
        };
        VkDescriptorSetLayout l = 0;
        VkResult r = vkCreateDescriptorSetLayout(R.device, &li, NULL, &l);
        if (r == VK_SUCCESS) idmap_set(out_id, l);
        reply_handle(type, r, out_id);
        return;
    }

    case VNCS_CMD_vkCreatePipelineLayout_EXT: {
        vncr_handle(d);
        if (!vncr_ptr(d)) goto bad;
        if (vncr_i32(d) != VNCS_STYPE_PIPELINE_LAYOUT_CREATE_INFO) goto bad;
        vncr_ptr(d);
        vncr_u32(d);
        uint32_t sn = vncr_u32(d);
        uint64_t sarr = vncr_u64(d);
        if (sarr != sn || sn > 4) goto bad;
        VkDescriptorSetLayout sets[4];
        for (uint32_t i = 0; i < sn; i++) {
            sets[i] = (VkDescriptorSetLayout)idmap_get(vncr_handle(d));
        }
        uint32_t pn = vncr_u32(d);
        uint64_t parr = vncr_u64(d);
        if (parr != pn || pn > 4) goto bad;
        VkPushConstantRange pcs[4];
        for (uint32_t i = 0; i < pn; i++) {
            pcs[i].stageFlags = vncr_u32(d);
            pcs[i].offset = vncr_u32(d);
            pcs[i].size = vncr_u32(d);
        }
        vncr_ptr(d);
        if (!vncr_ptr(d)) goto bad;
        uint64_t out_id = vncr_handle(d);
        if (d->fatal) goto bad;

        VkPipelineLayoutCreateInfo li = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = sn, .pSetLayouts = sets,
            .pushConstantRangeCount = pn, .pPushConstantRanges = pcs,
        };
        VkPipelineLayout l = 0;
        VkResult r = vkCreatePipelineLayout(R.device, &li, NULL, &l);
        if (r == VK_SUCCESS) idmap_set(out_id, l);
        reply_handle(type, r, out_id);
        return;
    }

    case VNCS_CMD_vkCreateComputePipelines_EXT: {
        vncr_handle(d);                 /* device */
        vncr_handle(d);                 /* pipelineCache */
        uint32_t cn = vncr_u32(d);
        uint64_t carr = vncr_u64(d);
        if (carr != cn || cn > 8) goto bad;
        VkComputePipelineCreateInfo cis[8];
        char names[8][8];
        memset(cis, 0, sizeof(cis));
        for (uint32_t i = 0; i < cn; i++) {
            if (vncr_i32(d) != VNCS_STYPE_COMPUTE_PIPELINE_CREATE_INFO) goto bad;
            vncr_ptr(d);                /* pNext */
            cis[i].flags = vncr_u32(d);
            if (vncr_i32(d) != VNCS_STYPE_PIPELINE_SHADER_STAGE_CREATE_INFO)
                goto bad;
            vncr_ptr(d);                /* stage pNext */
            cis[i].stage.flags = vncr_u32(d);
            cis[i].stage.stage = (uint32_t)vncr_i32(d);
            cis[i].stage.module =
                (VkShaderModule)idmap_get(vncr_handle(d));
            uint64_t nlen = vncr_u64(d);
            if (nlen >= sizeof(names[i])) goto bad;
            vncs_read(d, (nlen + 3) & ~3ULL, names[i], (size_t)nlen);
            cis[i].stage.pName = names[i];
            vncr_ptr(d);                /* pSpecializationInfo */
            cis[i].layout = (VkPipelineLayout)idmap_get(vncr_handle(d));
            vncr_handle(d);             /* basePipelineHandle */
            cis[i].basePipelineIndex = vncr_i32(d);
            cis[i].sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            cis[i].stage.sType =
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        }
        vncr_ptr(d);                    /* pAllocator */
        uint64_t oarr = vncr_u64(d);
        if (oarr != cn) goto bad;
        uint64_t out_ids[8];
        for (uint32_t i = 0; i < cn; i++) out_ids[i] = vncr_handle(d);
        if (d->fatal) goto bad;

        VkPipeline pipes[8] = {0};
        VkResult r = vkCreateComputePipelines(R.device, 0, cn, cis, NULL, pipes);
        if (r == VK_SUCCESS) {
            for (uint32_t i = 0; i < cn; i++) idmap_set(out_ids[i], pipes[i]);
        }
        reply_begin();
        rw_u32(type); rw_u32((uint32_t)r);
        rw_u64(cn);
        for (uint32_t i = 0; i < cn; i++) rw_u64(out_ids[i]);
        return;
    }

    case VNCS_CMD_vkCreateDescriptorPool_EXT: {
        vncr_handle(d);
        if (!vncr_ptr(d)) goto bad;
        if (vncr_i32(d) != VNCS_STYPE_DESCRIPTOR_POOL_CREATE_INFO) goto bad;
        vncr_ptr(d);
        vncr_u32(d);                    /* flags */
        uint32_t max_sets = vncr_u32(d);
        uint32_t pn = vncr_u32(d);
        uint64_t parr = vncr_u64(d);
        if (parr != pn || pn > 4) goto bad;
        VkDescriptorPoolSize sizes[4];
        for (uint32_t i = 0; i < pn; i++) {
            sizes[i].type = (uint32_t)vncr_i32(d);
            sizes[i].descriptorCount = vncr_u32(d);
        }
        vncr_ptr(d);
        if (!vncr_ptr(d)) goto bad;
        uint64_t out_id = vncr_handle(d);
        if (d->fatal) goto bad;

        VkDescriptorPoolCreateInfo pi = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = max_sets, .poolSizeCount = pn, .pPoolSizes = sizes,
        };
        VkDescriptorPool pool = 0;
        VkResult r = vkCreateDescriptorPool(R.device, &pi, NULL, &pool);
        if (r == VK_SUCCESS) idmap_set(out_id, pool);
        reply_handle(type, r, out_id);
        return;
    }

    case VNCS_CMD_vkAllocateDescriptorSets_EXT: {
        vncr_handle(d);
        if (!vncr_ptr(d)) goto bad;
        if (vncr_i32(d) != VNCS_STYPE_DESCRIPTOR_SET_ALLOCATE_INFO) goto bad;
        vncr_ptr(d);
        VkDescriptorPool pool = (VkDescriptorPool)idmap_get(vncr_handle(d));
        uint32_t sn = vncr_u32(d);
        uint64_t sarr = vncr_u64(d);
        if (sarr != sn || sn > 4) goto bad;
        VkDescriptorSetLayout layouts[4];
        for (uint32_t i = 0; i < sn; i++) {
            layouts[i] = (VkDescriptorSetLayout)idmap_get(vncr_handle(d));
        }
        uint64_t oarr = vncr_u64(d);
        if (oarr != sn) goto bad;
        uint64_t out_ids[4];
        for (uint32_t i = 0; i < sn; i++) out_ids[i] = vncr_handle(d);
        if (d->fatal) goto bad;

        VkDescriptorSetAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = pool, .descriptorSetCount = sn,
            .pSetLayouts = layouts,
        };
        VkDescriptorSet sets[4] = {0};
        VkResult r = vkAllocateDescriptorSets(R.device, &ai, sets);
        if (r == VK_SUCCESS) {
            for (uint32_t i = 0; i < sn; i++) idmap_set(out_ids[i], sets[i]);
        }
        reply_begin();
        rw_u32(type); rw_u32((uint32_t)r);
        rw_u64(sn);
        for (uint32_t i = 0; i < sn; i++) rw_u64(out_ids[i]);
        return;
    }

    case VNCS_CMD_vkUpdateDescriptorSets_EXT: {
        vncr_handle(d);                 /* device */
        uint32_t wn = vncr_u32(d);
        uint64_t warr = vncr_u64(d);
        if (warr != wn || wn > 8) goto bad;
        VkWriteDescriptorSet writes[8];
        VkDescriptorBufferInfo binfos[8];
        memset(writes, 0, sizeof(writes));
        for (uint32_t i = 0; i < wn; i++) {
            if (vncr_i32(d) != VNCS_STYPE_WRITE_DESCRIPTOR_SET) goto bad;
            vncr_ptr(d);
            writes[i].dstSet = (VkDescriptorSet)idmap_get(vncr_handle(d));
            writes[i].dstBinding = vncr_u32(d);
            writes[i].dstArrayElement = vncr_u32(d);
            writes[i].descriptorCount = vncr_u32(d);
            writes[i].descriptorType = (uint32_t)vncr_i32(d);
            if (vncr_u64(d) != 0) goto bad;     /* image infos */
            uint64_t bi = vncr_u64(d);
            if (bi != 1) goto bad;
            binfos[i].buffer = (VkBuffer)idmap_get(vncr_handle(d));
            binfos[i].offset = vncr_u64(d);
            binfos[i].range = vncr_u64(d);
            if (vncr_u64(d) != 0) goto bad;     /* texel buffer views */
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].pBufferInfo = &binfos[i];
        }
        vncr_u32(d); vncr_u64(d);       /* copies */
        if (d->fatal) goto bad;
        vkUpdateDescriptorSets(R.device, wn, writes, 0, NULL);
        return;                         /* void: no reply */
    }

    case VNCS_CMD_vkCreateCommandPool_EXT: {
        vncr_handle(d);
        if (!vncr_ptr(d)) goto bad;
        if (vncr_i32(d) != VNCS_STYPE_COMMAND_POOL_CREATE_INFO) goto bad;
        vncr_ptr(d);
        uint32_t fl = vncr_u32(d);
        uint32_t qfi = vncr_u32(d);
        vncr_ptr(d);
        if (!vncr_ptr(d)) goto bad;
        uint64_t out_id = vncr_handle(d);
        if (d->fatal) goto bad;
        VkCommandPoolCreateInfo ci = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = fl, .queueFamilyIndex = qfi,
        };
        VkCommandPool pool = 0;
        VkResult r = vkCreateCommandPool(R.device, &ci, NULL, &pool);
        if (r == VK_SUCCESS) idmap_set(out_id, pool);
        reply_handle(type, r, out_id);
        return;
    }

    case VNCS_CMD_vkAllocateCommandBuffers_EXT: {
        vncr_handle(d);
        if (!vncr_ptr(d)) goto bad;
        if (vncr_i32(d) != VNCS_STYPE_COMMAND_BUFFER_ALLOCATE_INFO) goto bad;
        vncr_ptr(d);
        VkCommandPool pool = (VkCommandPool)idmap_get(vncr_handle(d));
        uint32_t level = (uint32_t)vncr_i32(d);
        uint32_t cn = vncr_u32(d);
        uint64_t oarr = vncr_u64(d);
        if (oarr != cn || cn > 4) goto bad;
        uint64_t out_ids[4];
        for (uint32_t i = 0; i < cn; i++) out_ids[i] = vncr_handle(d);
        if (d->fatal) goto bad;
        VkCommandBufferAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool, .level = level, .commandBufferCount = cn,
        };
        VkCommandBuffer bufs[4] = {0};
        VkResult r = vkAllocateCommandBuffers(R.device, &ai, bufs);
        if (r == VK_SUCCESS) {
            for (uint32_t i = 0; i < cn; i++) {
                idmap_set(out_ids[i], (uint64_t)(uintptr_t)bufs[i]);
            }
        }
        reply_begin();
        rw_u32(type); rw_u32((uint32_t)r);
        rw_u64(cn);
        for (uint32_t i = 0; i < cn; i++) rw_u64(out_ids[i]);
        return;
    }

    case VNCS_CMD_vkBeginCommandBuffer_EXT: {
        uint64_t cb_id = vncr_handle(d);
        if (!vncr_ptr(d)) goto bad;
        if (vncr_i32(d) != VNCS_STYPE_COMMAND_BUFFER_BEGIN_INFO) goto bad;
        vncr_ptr(d);
        uint32_t fl = vncr_u32(d);
        vncr_ptr(d);                    /* inheritance */
        if (d->fatal) goto bad;
        VkCommandBufferBeginInfo bi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = fl,
        };
        VkResult r = vkBeginCommandBuffer(
            (VkCommandBuffer)(uintptr_t)idmap_get(cb_id), &bi);
        reply_ret(type, r);
        return;
    }

    case VNCS_CMD_vkEndCommandBuffer_EXT: {
        uint64_t cb_id = vncr_handle(d);
        if (d->fatal) goto bad;
        VkResult r = vkEndCommandBuffer(
            (VkCommandBuffer)(uintptr_t)idmap_get(cb_id));
        reply_ret(type, r);
        return;
    }

    case VNCS_CMD_vkCmdBindPipeline_EXT: {
        uint64_t cb_id = vncr_handle(d);
        uint32_t bp = (uint32_t)vncr_i32(d);
        uint64_t pipe_id = vncr_handle(d);
        if (d->fatal) goto bad;
        vkCmdBindPipeline((VkCommandBuffer)(uintptr_t)idmap_get(cb_id), bp,
                          (VkPipeline)idmap_get(pipe_id));
        return;
    }

    case VNCS_CMD_vkCmdBindDescriptorSets_EXT: {
        uint64_t cb_id = vncr_handle(d);
        uint32_t bp = (uint32_t)vncr_i32(d);
        VkPipelineLayout layout = (VkPipelineLayout)idmap_get(vncr_handle(d));
        uint32_t first = vncr_u32(d);
        uint32_t sn = vncr_u32(d);
        uint64_t sarr = vncr_u64(d);
        if (sarr != sn || sn > 4) goto bad;
        VkDescriptorSet sets[4];
        for (uint32_t i = 0; i < sn; i++) {
            sets[i] = (VkDescriptorSet)idmap_get(vncr_handle(d));
        }
        uint32_t dn = vncr_u32(d);
        uint64_t darr = vncr_u64(d);
        vncs_skip(d, darr * 4);
        if (d->fatal || dn != 0) goto bad;
        vkCmdBindDescriptorSets((VkCommandBuffer)(uintptr_t)idmap_get(cb_id),
                                bp, layout, first, sn, sets, 0, NULL);
        return;
    }

    case VNCS_CMD_vkCmdPushConstants_EXT: {
        uint64_t cb_id = vncr_handle(d);
        VkPipelineLayout layout = (VkPipelineLayout)idmap_get(vncr_handle(d));
        uint32_t stages = vncr_u32(d);
        uint32_t offset = vncr_u32(d);
        uint32_t size = vncr_u32(d);
        uint64_t arr = vncr_u64(d);
        if (arr != size || size > 256) goto bad;
        uint8_t vals[256];
        vncs_read(d, (size + 3) & ~3u, vals, size);
        if (d->fatal) goto bad;
        vkCmdPushConstants((VkCommandBuffer)(uintptr_t)idmap_get(cb_id),
                           layout, stages, offset, size, vals);
        return;
    }

    case VNCS_CMD_vkCmdDispatch_EXT: {
        uint64_t cb_id = vncr_handle(d);
        uint32_t x = vncr_u32(d), y = vncr_u32(d), z = vncr_u32(d);
        if (d->fatal) goto bad;
        vkCmdDispatch((VkCommandBuffer)(uintptr_t)idmap_get(cb_id), x, y, z);
        return;
    }

    case VNCS_CMD_vkQueueSubmit_EXT: {
        uint64_t queue_id = vncr_handle(d);
        uint32_t sn = vncr_u32(d);
        uint64_t sarr = vncr_u64(d);
        if (sarr != sn || sn != 1) goto bad;
        if (vncr_i32(d) != VNCS_STYPE_SUBMIT_INFO) goto bad;
        vncr_ptr(d);
        uint32_t wc = vncr_u32(d);
        if (vncr_u64(d) != wc) goto bad;
        vncs_skip(d, wc * 8);
        if (vncr_u64(d) != wc) goto bad;
        vncs_skip(d, wc * 4);
        uint32_t cc = vncr_u32(d);
        if (vncr_u64(d) != cc || cc > 4) goto bad;
        VkCommandBuffer cbs[4];
        for (uint32_t i = 0; i < cc; i++) {
            cbs[i] = (VkCommandBuffer)(uintptr_t)idmap_get(vncr_handle(d));
        }
        uint32_t sc = vncr_u32(d);
        if (vncr_u64(d) != sc) goto bad;
        vncs_skip(d, sc * 8);
        uint64_t fence_id = vncr_handle(d);
        if (d->fatal) goto bad;

        /* mirror guest-written blob contents into the vk-mapped memory */
        for (uint32_t i = 0; i < g_vkbuf_count; i++) {
            mock_vkbuf_t *vb = &g_vkbufs[i];
            if (vb->blob && vb->blob->vk_mapped) {
                memcpy(vb->blob->vk_mapped, vb->blob->data,
                       vb->size < vb->blob->size ? vb->size : vb->blob->size);
            }
        }

        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = cc, .pCommandBuffers = cbs,
        };
        VkResult r = vkQueueSubmit((VkQueue)(uintptr_t)idmap_get(queue_id),
                                   1, &si, (VkFence)idmap_get(fence_id));
        reply_ret(type, r);
        return;
    }

    case VNCS_CMD_vkCreateFence_EXT: {
        vncr_handle(d);                 /* device */
        if (!vncr_ptr(d)) goto bad;
        if (vncr_i32(d) != VNCS_STYPE_FENCE_CREATE_INFO) goto bad;
        vncr_ptr(d);
        uint32_t fl = vncr_u32(d);
        vncr_ptr(d);
        if (!vncr_ptr(d)) goto bad;
        uint64_t out_id = vncr_handle(d);
        if (d->fatal) goto bad;
        VkFenceCreateInfo fi = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = fl,
        };
        VkFence f = 0;
        VkResult r = vkCreateFence(R.device, &fi, NULL, &f);
        if (r == VK_SUCCESS) idmap_set(out_id, f);
        reply_handle(type, r, out_id);
        return;
    }

    case VNCS_CMD_vkResetFences_EXT: {
        vncr_handle(d);                 /* device */
        uint32_t fn = vncr_u32(d);
        uint64_t farr = vncr_u64(d);
        if (farr != fn || fn > 4) goto bad;
        VkFence fences[4];
        for (uint32_t i = 0; i < fn; i++) {
            fences[i] = (VkFence)idmap_get(vncr_handle(d));
        }
        if (d->fatal) goto bad;
        VkResult r = vkResetFences(R.device, fn, fences);
        reply_ret(type, r);
        return;
    }

    case VNCS_CMD_vkGetFenceStatus_EXT: {
        vncr_handle(d);
        uint64_t fence_id = vncr_handle(d);
        if (d->fatal) goto bad;
        VkResult r = vkGetFenceStatus(R.device, (VkFence)idmap_get(fence_id));
        reply_ret(type, r);
        return;
    }

    case VNCS_CMD_vkWaitForFences_EXT: {
        vncr_handle(d);                 /* device */
        uint32_t fn = vncr_u32(d);
        uint64_t farr = vncr_u64(d);
        if (farr != fn || fn > 4) goto bad;
        VkFence fences[4];
        for (uint32_t i = 0; i < fn; i++) {
            fences[i] = (VkFence)idmap_get(vncr_handle(d));
        }
        uint32_t wait_all = vncr_u32(d);
        uint64_t timeout = vncr_u64(d);
        if (d->fatal) goto bad;
        VkResult r = vkWaitForFences(R.device, fn, fences, wait_all, timeout);

        /* mirror results back into the guest-visible blobs */
        for (uint32_t i = 0; i < g_vkbuf_count; i++) {
            mock_vkbuf_t *vb = &g_vkbufs[i];
            if (vb->blob && vb->blob->vk_mapped) {
                memcpy(vb->blob->data, vb->blob->vk_mapped,
                       vb->size < vb->blob->size ? vb->size : vb->blob->size);
            }
        }
        reply_ret(type, r);
        return;
    }

    case VNCS_CMD_vkDestroyBuffer_EXT: {
        vncr_handle(d);                 /* device */
        uint64_t buf_id = vncr_handle(d);
        vncr_ptr(d);                    /* pAllocator */
        if (d->fatal) goto bad;
        vkDestroyBuffer(R.device, (VkBuffer)idmap_get(buf_id), NULL);
        return;
    }

    case VNCS_CMD_vkFreeMemory_EXT: {
        vncr_handle(d);                 /* device */
        uint64_t mem_id = vncr_handle(d);
        vncr_ptr(d);
        if (d->fatal) goto bad;
        vkFreeMemory(R.device, (VkDeviceMemory)idmap_get(mem_id), NULL);
        return;
    }

    case VNCS_CMD_vkSetReplyCommandStreamMESA_EXT: {
        if (!vncr_ptr(d)) goto bad;
        uint32_t res_id = vncr_u32(d);
        uint64_t offset = vncr_u64(d);
        uint64_t size = vncr_u64(d);
        if (d->fatal) goto bad;
        mock_blob_t *b = blob_find(res_id);
        if (!b || offset + size > b->size) goto bad;
        R.reply_blob = b;
        R.reply_off = offset;
        return;
    }

    default:
        fprintf(stderr, "MOCK: unexpected ring command type %u\n", type);
        goto bad;
    }

bad:
    fprintf(stderr, "MOCK: malformed record (type %u)\n", type);
    exit(2);
}

/* ============================================================================
 * Ring consumption + transport entry points
 * ============================================================================ */

static uint32_t ring_rd32(uint64_t off)
{
    uint32_t v;
    memcpy(&v, R.ring_blob->data + off, 4);
    return v;
}

static void ring_wr32(uint64_t off, uint32_t v)
{
    memcpy(R.ring_blob->data + off, &v, 4);
}

/* Consume ring records in [head, tail): structural walk (record boundaries,
 * known opcodes, opcode log) + semantic dispatch to real Vulkan. */
static void mock_renderer_consume(void)
{
    if (!R.ring_ok) {
        fprintf(stderr, "MOCK: ring write before vkCreateRingMESA\n");
        exit(2);
    }
    uint32_t head = ring_rd32(VN_RING_HEAD_OFF);
    uint32_t tail = ring_rd32(VN_RING_TAIL_OFF);
    uint32_t len = tail - head;
    if (len == 0) {
        return;
    }
    if (len > R.buffer_size) {
        fprintf(stderr, "MOCK: ring overrun\n");
        exit(2);
    }

    /* linearize the segment (wrap-aware) */
    uint8_t *seg = malloc(len);
    uint32_t off = head % R.buffer_size;
    uint32_t first = R.buffer_size - off;
    if (first > len) first = len;
    memcpy(seg, R.ring_blob->data + R.buffer_off + off, first);
    if (len > first) {
        memcpy(seg + first, R.ring_blob->data + R.buffer_off, len - first);
    }

    /* (a) structural pass: walk every record; cursor must land exactly at
     * the end; every opcode must be known. */
    vncs_decoder_t walk;
    vncs_decoder_init(&walk, seg, len);
    while (walk.cur < walk.end) {
        const vncs_cmd_info_t *info = vncs_walk_next(&walk);
        if (!info) {
            fprintf(stderr, "STRUCTURAL: walker failed at +%zu\n",
                    (size_t)(walk.cur - seg));
            exit(2);
        }
        if (g_opcode_count < MOCK_MAX_OPCODES) {
            g_opcodes[g_opcode_count++] = info->type;
        }
    }
    if (walk.fatal) {
        fprintf(stderr, "STRUCTURAL: walker fatal\n");
        exit(2);
    }

    /* (b) semantic pass: dispatch each record to real Vulkan */
    vncs_decoder_t d;
    vncs_decoder_init(&d, seg, len);
    while (d.cur < d.end) {
        uint32_t type = vncr_u32(&d);
        uint32_t flags = vncr_u32(&d);
        if (d.fatal) {
            fprintf(stderr, "MOCK: header underrun\n");
            exit(2);
        }
        mock_exec(&d, type, flags);
    }

    /* renderer consumed everything: head = tail */
    ring_wr32(VN_RING_HEAD_OFF, tail);
    free(seg);
}

/* VIRTIO_GPU_CMD_SUBMIT_3D — carries the transport commands that manage the
 * ring itself (vkCreateRingMESA / vkNotifyRingMESA / vkDestroyRingMESA). */
int virtio_gpu_submit_3d(uint32_t ctx_id, const void *cmds,
                         uint32_t cmds_len, uint64_t *fence_id)
{
    (void)ctx_id;
    if (fence_id) {
        static uint64_t seq;
        *fence_id = ++seq;
    }
    vncs_decoder_t d;
    vncs_decoder_init(&d, cmds, cmds_len);
    while (d.cur < d.end) {
        uint32_t type = vncr_u32(&d);
        uint32_t flags = vncr_u32(&d);
        (void)flags;
        switch (type) {
        case VNCS_CMD_vkCreateRingMESA_EXT: {
            uint64_t ring_id = vncr_u64(&d);
            if (!vncr_ptr(&d)) goto bad;
            if (vncr_i32(&d) != VNCS_STYPE_RING_CREATE_INFO_MESA) goto bad;
            vncr_ptr(&d);               /* pNext */
            vncr_u32(&d);               /* flags */
            uint32_t res_id = vncr_u32(&d);
            uint64_t offset = vncr_u64(&d);
            uint64_t size = vncr_u64(&d);
            vncr_u64(&d);               /* idle timeout */
            uint64_t head_off = vncr_u64(&d);
            uint64_t tail_off = vncr_u64(&d);
            uint64_t status_off = vncr_u64(&d);
            uint64_t buffer_off = vncr_u64(&d);
            uint64_t buffer_size = vncr_u64(&d);
            uint64_t extra_off = vncr_u64(&d);
            uint64_t extra_size = vncr_u64(&d);
            if (d.fatal) goto bad;
            /* Mesa layout contract */
            if (head_off != 0 || tail_off != 64 || status_off != 128 ||
                buffer_off != 192 || buffer_off + buffer_size != extra_off ||
                offset != 0 || extra_off + extra_size > size) {
                fprintf(stderr, "MOCK: bad ring layout\n");
                goto bad;
            }
            mock_blob_t *b = blob_find(res_id);
            if (!b || size > b->size) goto bad;
            R.ring_ok = true;
            R.ring_id = ring_id;
            R.ring_blob = b;
            R.buffer_off = buffer_off;
            R.buffer_size = buffer_size;
            break;
        }
        case VNCS_CMD_vkNotifyRingMESA_EXT: {
            uint64_t ring_id = vncr_u64(&d);
            uint32_t seqno = vncr_u32(&d);
            vncr_u32(&d);               /* flags */
            if (d.fatal) goto bad;
            if (!R.ring_ok || ring_id != R.ring_id) goto bad;
            uint32_t tail = ring_rd32(VN_RING_TAIL_OFF);
            if (seqno != tail) {
                fprintf(stderr, "MOCK: notify seqno %u != tail %u\n",
                        seqno, tail);
                goto bad;
            }
            mock_renderer_consume();
            break;
        }
        case VNCS_CMD_vkDestroyRingMESA_EXT: {
            uint64_t ring_id = vncr_u64(&d);
            if (d.fatal || !R.ring_ok || ring_id != R.ring_id) goto bad;
            R.ring_ok = false;
            break;
        }
        default:
            fprintf(stderr, "MOCK: unexpected SUBMIT_3D record type %u\n", type);
            goto bad;
        }
    }
    return VIRTIO_GPU_OK;

bad:
    fprintf(stderr, "MOCK: bad SUBMIT_3D stream\n");
    return VIRTIO_GPU_ERR_DEVICE;
}

/* ============================================================================
 * CPU references (ported from tools/host_test_vulkan.c; identical math)
 * ============================================================================ */

static float fp16_to_fp32(uint16_t h)
{
    uint32_t sign = (h >> 15) & 1;
    uint32_t exp = (h >> 10) & 0x1f;
    uint32_t mant = h & 0x3ff;
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) {
            f = sign << 31;
        } else {
            exp = 1;
            while (!(mant & 0x400)) { mant <<= 1; exp--; }
            mant &= 0x3ff;
            f = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = (sign << 31) | 0x7f800000 | (mant << 13);
    } else {
        f = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    union { uint32_t i; float f; } u;
    u.i = f;
    return u.f;
}

static uint16_t fp32_to_fp16(float f)
{
    union { float f; uint32_t i; } u;
    u.f = f;
    uint32_t x = u.i;
    uint32_t sign = (x >> 16) & 0x8000;
    int exp = (int)((x >> 23) & 0xff) - 127 + 15;
    uint32_t mant = x & 0x7fffff;
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;
        mant |= 0x800000;
        uint32_t denorm = mant >> (14 - exp);
        return (uint16_t)(sign | denorm);
    }
    if (exp >= 31) return (uint16_t)(sign | 0x7c00);
    return (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
}

#define QK8_0 32
#define Q8_0_BLOCK_BYTES 34
#define QK_K 256
#define Q4_K_SUPERBLOCK_BYTES 144

typedef struct __attribute__((packed)) {
    uint16_t d;
    int8_t qs[QK8_0];
} block_q8_0;

static void quantize_row_q8_0(const float *x, block_q8_0 *blocks, int k)
{
    int nb = k / QK8_0;
    for (int b = 0; b < nb; b++) {
        float amax = 0.0f;
        for (int j = 0; j < QK8_0; j++) {
            float v = fabsf(x[b * QK8_0 + j]);
            if (v > amax) amax = v;
        }
        float d = amax / 127.0f;
        uint16_t d16 = fp32_to_fp16(d);
        float d_deq = fp16_to_fp32(d16);
        blocks[b].d = d16;
        if (d_deq == 0.0f) {
            memset(blocks[b].qs, 0, QK8_0);
            continue;
        }
        float id = 1.0f / d_deq;
        for (int j = 0; j < QK8_0; j++) {
            float q = roundf(x[b * QK8_0 + j] * id);
            if (q > 127.0f) q = 127.0f;
            if (q < -127.0f) q = -127.0f;
            blocks[b].qs[j] = (int8_t)q;
        }
    }
}

static void get_scale_min_k4(int j, const uint8_t *q, uint8_t *d, uint8_t *mn)
{
    if (j < 4) {
        *d = q[j] & 63;
        *mn = q[j + 4] & 63;
    } else {
        *d = (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4);
        *mn = (q[j + 4] >> 4) | ((q[j] >> 6) << 4);
    }
}

static uint16_t rd_u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void wr_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint64_t g_rng_state;

static uint64_t rng_next(void)
{
    uint64_t z = (g_rng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static float rng_float(void)
{
    return ((double)(rng_next() >> 11) / (double)(1ULL << 53)) * 2.0f - 1.0f;
}

static void matmul_f32_ref(const float *A, const float *B, float *C,
                           uint32_t m, uint32_t k, uint32_t n)
{
    for (uint32_t i = 0; i < m; i++) {
        for (uint32_t j = 0; j < n; j++) {
            float acc = 0.0f;
            for (uint32_t kk = 0; kk < k; kk++)
                acc += A[i * k + kk] * B[kk * n + j];
            C[i * n + j] = acc;
        }
    }
}

static void matmul_q4_k_q8_0_ref(const uint8_t *A, const uint8_t *B, float *C,
                                 uint32_t m, uint32_t k, uint32_t n)
{
    uint32_t nb = k / QK_K;
    uint32_t nb8 = k / QK8_0;
    for (uint32_t i = 0; i < m; i++) {
        for (uint32_t j = 0; j < n; j++) {
            float acc = 0.0f;
            for (uint32_t sb = 0; sb < nb; sb++) {
                const uint8_t *sbp = A + ((size_t)i * nb + sb) *
                                         Q4_K_SUPERBLOCK_BYTES;
                float xd = fp16_to_fp32(rd_u16le(sbp));
                float xdmin = fp16_to_fp32(rd_u16le(sbp + 2));
                const uint8_t *sc12 = sbp + 4;
                float sumi = 0.0f, summs = 0.0f;
                for (int j8 = 0; j8 < 8; j8++) {
                    uint8_t sc, mn;
                    get_scale_min_k4(j8, sc12, &sc, &mn);
                    const uint8_t *yb = B + ((size_t)j * nb8 + sb * 8 + j8) *
                                            Q8_0_BLOCK_BYTES;
                    float yd = fp16_to_fp32(rd_u16le(yb));
                    const uint8_t *qs = sbp + 16 + (j8 / 2) * 32;
                    int use_high = j8 & 1;
                    int32_t isum = 0, iysum = 0;
                    for (int l = 0; l < 32; l++) {
                        int q = use_high ? (qs[l] >> 4) : (qs[l] & 0xF);
                        int yq = (int8_t)yb[2 + l];
                        isum += q * yq;
                        iysum += yq;
                    }
                    sumi += (float)sc * yd * (float)isum;
                    summs += yd * (float)(mn * iysum);
                }
                acc = (acc + xd * sumi) - xdmin * summs;
            }
            C[i * n + j] = acc;
        }
    }
}

static void fill_q4_k_blocks(uint8_t *A, size_t nblocks)
{
    for (size_t b = 0; b < nblocks; b++) {
        uint8_t *p = A + b * Q4_K_SUPERBLOCK_BYTES;
        float dv, mv;
        if (b % 7 == 3) {
            dv = 0.0f;
        } else if (b % 11 == 5) {
            dv = 1e-6f;
        } else {
            dv = 0.001f + fabsf(rng_float()) * 0.5f;
        }
        mv = (b % 5 == 2) ? 0.0f : 0.001f + fabsf(rng_float()) * 0.25f;
        wr_u16le(p, fp32_to_fp16(dv));
        wr_u16le(p + 2, fp32_to_fp16(mv));
        for (int t = 4; t < Q4_K_SUPERBLOCK_BYTES; t++)
            p[t] = (uint8_t)(rng_next() & 0xFF);
    }
}

static void quantize_cols_q8_0(const float *B_f32, uint8_t *out,
                               uint32_t k, uint32_t n)
{
    uint32_t nb8 = k / QK8_0;
    float *col = malloc((size_t)k * 4);
    for (uint32_t j = 0; j < n; j++) {
        for (uint32_t r = 0; r < k; r++)
            col[r] = B_f32[(size_t)r * n + j];
        quantize_row_q8_0(col, (block_q8_0 *)(out + (size_t)j * nb8 *
                                                Q8_0_BLOCK_BYTES), (int)k);
    }
    free(col);
}

/* ============================================================================
 * Structural check: expected opcode sequences
 * ============================================================================ */

static int check_opcode_seq(uint32_t start, const uint32_t *expected,
                            uint32_t count, const char *what)
{
    if (start + count > g_opcode_count) {
        printf("STRUCTURAL %s: too few opcodes (%u logged, want %u+%u)\n",
               what, g_opcode_count, start, count);
        return 0;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (g_opcodes[start + i] != expected[i]) {
            const vncs_cmd_info_t *got = vncs_cmd_lookup(g_opcodes[start + i]);
            const vncs_cmd_info_t *want = vncs_cmd_lookup(expected[i]);
            printf("STRUCTURAL %s: opcode[%u] = %s (%u), want %s (%u)\n",
                   what, start + i, got ? got->name : "?", g_opcodes[start + i],
                   want ? want->name : "?", expected[i]);
            return 0;
        }
    }
    printf("STRUCTURAL %s: %u opcodes in expected order -> PASS\n",
           what, count);
    return 1;
}

/* ============================================================================
 * Tests
 * ============================================================================ */

static venus_transport_t *g_transport;
static venus_vk_device_t g_dev;

static int test_matmul_f32(uint32_t m, uint32_t k, uint32_t n)
{
    float *A = malloc((size_t)m * k * 4);
    float *B = malloc((size_t)k * n * 4);
    float *C = malloc((size_t)m * n * 4);
    float *C_ref = malloc((size_t)m * n * 4);
    for (uint32_t i = 0; i < m * k; i++) A[i] = rng_float();
    for (uint32_t i = 0; i < k * n; i++) B[i] = rng_float();
    for (uint32_t i = 0; i < m * n; i++) C[i] = -999.0f;

    matmul_f32_ref(A, B, C_ref, m, k, n);
    int rc = venus_vk_matmul(&g_dev, 0, A, (uint64_t)m * k * 4,
                             B, (uint64_t)k * n * 4, C, m, k, n);
    if (rc != 0) {
        printf("matmul_f32 %ux%ux%u: venus_vk_matmul failed: %d -> FAIL\n",
               m, k, n, rc);
        return 0;
    }

    double max_abs = 0.0, max_rel = 0.0;
    for (uint32_t i = 0; i < m * n; i++) {
        double err = fabs((double)C[i] - (double)C_ref[i]);
        double denom = fabs((double)C_ref[i]) > 1e-6 ? fabs((double)C_ref[i]) : 1.0;
        if (err > max_abs) max_abs = err;
        if (err / denom > max_rel) max_rel = err / denom;
    }
    int pass = max_abs < 1e-3 && max_rel < 1e-3;
    printf("matmul_f32 %3ux%-3ux%-3u : max_abs_err=%.3g max_rel_err=%.3g -> %s\n",
           m, k, n, max_abs, max_rel, pass ? "PASS" : "FAIL");
    free(A); free(B); free(C); free(C_ref);
    return pass;
}

static int test_matmul_q4_k(uint32_t m, uint32_t k, uint32_t n)
{
    uint32_t nb = k / QK_K, nb8 = k / QK8_0;
    size_t a_bytes = (size_t)m * nb * Q4_K_SUPERBLOCK_BYTES;
    size_t b_bytes = (size_t)n * nb8 * Q8_0_BLOCK_BYTES;
    uint8_t *A = malloc(a_bytes);
    float *B_f32 = malloc((size_t)k * n * 4);
    uint8_t *B = malloc(b_bytes);
    float *C = malloc((size_t)m * n * 4);
    float *C_ref = malloc((size_t)m * n * 4);

    fill_q4_k_blocks(A, (size_t)m * nb);
    for (uint32_t i = 0; i < k * n; i++) B_f32[i] = rng_float() * 2.0f;
    quantize_cols_q8_0(B_f32, B, k, n);
    for (uint32_t i = 0; i < m * n; i++) C[i] = -999.0f;

    matmul_q4_k_q8_0_ref(A, B, C_ref, m, k, n);
    int rc = venus_vk_matmul(&g_dev, 2, A, a_bytes, B, b_bytes, C, m, k, n);
    if (rc != 0) {
        printf("matmul_q4_k %ux%ux%u: venus_vk_matmul failed: %d -> FAIL\n",
               m, k, n, rc);
        return 0;
    }

    double max_abs = 0.0;
    for (uint32_t i = 0; i < m * n; i++) {
        double err = fabs((double)C[i] - (double)C_ref[i]);
        if (err > max_abs) max_abs = err;
    }
    int pass = max_abs == 0.0;   /* bit-exact: identical float op order */
    printf("matmul_q4_k_q8_0 %3ux%-3ux%-3u : max_abs_err=%.3g -> %s\n",
           m, k, n, max_abs, pass ? "PASS" : "FAIL");
    free(A); free(B_f32); free(B); free(C); free(C_ref);
    return pass;
}

int main(void)
{
    if (!getenv("VK_ICD_FILENAMES") && !getenv("VK_DRIVER_FILES") &&
        !getenv("VK_ADD_DRIVER_FILES")) {
        setenv("VK_ICD_FILENAMES",
               "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json", 0);
    }
    g_rng_state = 0x5EED1234567890ABULL;
    g_hostmem = malloc(MOCK_HOSTMEM_SIZE);
    if (!g_hostmem) {
        fprintf(stderr, "hostmem arena alloc failed\n");
        return 2;
    }

    printf("=== host_test_venus: kernel Venus layer on mock virtio-gpu + "
           "lavapipe renderer ===\n");

    /* transport probe (creates context, ring, reply shmem) */
    static virtio_gpu_dev_t fake_gpu;
    memset(&fake_gpu, 0, sizeof(fake_gpu));
    fake_gpu.initialized = true;
    g_transport = venus_transport_probe(&fake_gpu);
    if (!g_transport || !g_transport->ring_registered) {
        printf("transport probe failed -> FAIL\n");
        return 1;
    }
    printf("transport: probed, ring registered -> PASS\n");

    /* compute device bring-up through the ring */
    venus_vk_shader_t shaders[4] = {
        { vk_spv_matmul_f32, vk_spv_matmul_f32_word_count, "matmul_f32" },
        { vk_spv_matmul_q8_0, vk_spv_matmul_q8_0_word_count, "matmul_q8_0" },
        { vk_spv_matmul_q4_k_q8_0, vk_spv_matmul_q4_k_q8_0_word_count,
          "matmul_q4_k_q8_0" },
        { vk_spv_matmul_q6_k_q8_0, vk_spv_matmul_q6_k_q8_0_word_count,
          "matmul_q6_k_q8_0" },
    };

    uint32_t init_start = g_opcode_count;
    int rc = venus_vk_device_init(g_transport, &g_dev, shaders, 4);
    if (rc != 0 || !g_dev.ready) {
        printf("venus_vk_device_init failed: %d -> FAIL\n", rc);
        return 1;
    }
    printf("device init: ready, device \"%s\" -> PASS\n", g_dev.device_name);

    /* structural: expected init opcode order */
    static const uint32_t S = VNCS_CMD_vkSetReplyCommandStreamMESA_EXT;
    const uint32_t expect_init[] = {
        S, VNCS_CMD_vkCreateInstance_EXT,
        S, VNCS_CMD_vkEnumeratePhysicalDevices_EXT,
        S, VNCS_CMD_vkEnumeratePhysicalDevices_EXT,
        S, VNCS_CMD_vkGetPhysicalDeviceProperties_EXT,
        S, VNCS_CMD_vkGetPhysicalDeviceQueueFamilyProperties_EXT,
        S, VNCS_CMD_vkGetPhysicalDeviceQueueFamilyProperties_EXT,
        S, VNCS_CMD_vkGetPhysicalDeviceMemoryProperties_EXT,
        S, VNCS_CMD_vkCreateDevice_EXT,
        VNCS_CMD_vkGetDeviceQueue_EXT,
        S, VNCS_CMD_vkCreateShaderModule_EXT,
        S, VNCS_CMD_vkCreateShaderModule_EXT,
        S, VNCS_CMD_vkCreateShaderModule_EXT,
        S, VNCS_CMD_vkCreateShaderModule_EXT,
        S, VNCS_CMD_vkCreateDescriptorSetLayout_EXT,
        S, VNCS_CMD_vkCreatePipelineLayout_EXT,
        S, VNCS_CMD_vkCreateComputePipelines_EXT,
        S, VNCS_CMD_vkCreateDescriptorPool_EXT,
        S, VNCS_CMD_vkAllocateDescriptorSets_EXT,
        S, VNCS_CMD_vkCreateCommandPool_EXT,
        S, VNCS_CMD_vkAllocateCommandBuffers_EXT,
        S, VNCS_CMD_vkCreateFence_EXT,
    };
    int pass = check_opcode_seq(init_start, expect_init,
                                sizeof(expect_init) / sizeof(expect_init[0]),
                                "init stream");

    /* semantic: matmul_f32 + matmul_q4_k through the full stack */
    uint32_t mm_start = g_opcode_count;
    pass &= test_matmul_f32(3, 64, 5);

    /* structural: expected matmul opcode order (3 buffers, record, submit,
     * wait, teardown) */
    const uint32_t expect_mm[] = {
        S, VNCS_CMD_vkCreateBuffer_EXT,
        S, VNCS_CMD_vkGetBufferMemoryRequirements_EXT,
        S, VNCS_CMD_vkGetMemoryResourcePropertiesMESA_EXT,
        S, VNCS_CMD_vkAllocateMemory_EXT,
        S, VNCS_CMD_vkBindBufferMemory_EXT,
        S, VNCS_CMD_vkCreateBuffer_EXT,
        S, VNCS_CMD_vkGetBufferMemoryRequirements_EXT,
        S, VNCS_CMD_vkGetMemoryResourcePropertiesMESA_EXT,
        S, VNCS_CMD_vkAllocateMemory_EXT,
        S, VNCS_CMD_vkBindBufferMemory_EXT,
        S, VNCS_CMD_vkCreateBuffer_EXT,
        S, VNCS_CMD_vkGetBufferMemoryRequirements_EXT,
        S, VNCS_CMD_vkGetMemoryResourcePropertiesMESA_EXT,
        S, VNCS_CMD_vkAllocateMemory_EXT,
        S, VNCS_CMD_vkBindBufferMemory_EXT,
        VNCS_CMD_vkUpdateDescriptorSets_EXT,
        S, VNCS_CMD_vkResetFences_EXT,
        VNCS_CMD_vkBeginCommandBuffer_EXT,
        VNCS_CMD_vkCmdBindPipeline_EXT,
        VNCS_CMD_vkCmdBindDescriptorSets_EXT,
        VNCS_CMD_vkCmdPushConstants_EXT,
        VNCS_CMD_vkCmdDispatch_EXT,
        VNCS_CMD_vkEndCommandBuffer_EXT,
        S, VNCS_CMD_vkQueueSubmit_EXT,
        S, VNCS_CMD_vkWaitForFences_EXT,
        VNCS_CMD_vkDestroyBuffer_EXT, VNCS_CMD_vkFreeMemory_EXT,
        VNCS_CMD_vkDestroyBuffer_EXT, VNCS_CMD_vkFreeMemory_EXT,
        VNCS_CMD_vkDestroyBuffer_EXT, VNCS_CMD_vkFreeMemory_EXT,
    };
    pass &= check_opcode_seq(mm_start, expect_mm,
                             sizeof(expect_mm) / sizeof(expect_mm[0]),
                             "matmul stream");

    pass &= test_matmul_f32(16, 512, 9);
    pass &= test_matmul_q4_k(3, 256, 5);
    pass &= test_matmul_q4_k(1, 256, 1);
    pass &= test_matmul_q4_k(4, 512, 7);

    venus_transport_shutdown();

    printf("=== host_test_venus: %s ===\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
