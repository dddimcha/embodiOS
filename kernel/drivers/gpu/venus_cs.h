/* EMBODIOS Venus command-stream wire format (VK_EXT_command_stream / MESA).
 *
 * Maxwell v0.7.0, WS-A. Freestanding VNCS (encoder) / VNCR (decoder+walker)
 * implementing the Venus protocol serialization for the Vulkan subset the
 * compute backend needs.
 *
 * Provenance: byte-modeled on the Mesa venus-protocol generated encoders
 * (vn_protocol_driver_*.h, venus-protocol git-ca1e9220 as shipped in Mesa's
 * src/virtio/venus-protocol/, VkCommandTypeEXT ids from the venus-protocol
 * vk.xml). Every encoder below mirrors the corresponding generated
 * vn_encode_*() field-for-field:
 *
 *   - record header: u32 VkCommandTypeEXT + u32 VkCommandFlagsEXT
 *     (VK_COMMAND_GENERATE_REPLY_BIT_EXT = 1 requests a reply record)
 *   - scalars: u32/enum/flags/float = 4 bytes, u64/DeviceSize/size_t = 8,
 *     VkBool32 = 4; all wire units are 4-byte multiples
 *   - handles: guest-assigned u64 object ids (vn_cs_handle_load_id)
 *   - arrays: u64 array_size followed by elements (u8/blob arrays padded to
 *     a 4-byte multiple); strings = array_size(strlen+1) + char bytes
 *   - pointers: u64 presence word (0/1), contents follow when present
 *   - chain structs: sType (i32) + pNext presence word + self fields
 *   - out-only structs on the encode side emit their *_partial form (nothing
 *     for fully-out structs, only fixed array_size words for
 *     VkPhysicalDeviceMemoryProperties)
 *   - reply records: u32 VkCommandTypeEXT, then VkResult (i32, only when the
 *     command returns one), then out-parameters (presence words included)
 *
 * This header compiles both freestanding (kernel, via include/compat) and
 * hosted (tools/host_test_venus.c) without modification.
 *
 * Author: EMBODIOS Team
 * License: MIT
 */

#ifndef EMBODIOS_VENUS_CS_H
#define EMBODIOS_VENUS_CS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ============================================================================
 * VkCommandTypeEXT — real ids from the venus-protocol VkCommandTypeEXT enum
 * (NOT placeholder values). Only the commands embodiOS emits are listed.
 * ============================================================================ */

typedef enum vncs_cmd_type {
    VNCS_CMD_vkCreateInstance_EXT                    = 0,
    VNCS_CMD_vkDestroyInstance_EXT                   = 1,
    VNCS_CMD_vkEnumeratePhysicalDevices_EXT          = 2,
    VNCS_CMD_vkGetPhysicalDeviceProperties_EXT       = 6,
    VNCS_CMD_vkGetPhysicalDeviceQueueFamilyProperties_EXT = 7,
    VNCS_CMD_vkGetPhysicalDeviceMemoryProperties_EXT = 8,
    VNCS_CMD_vkCreateDevice_EXT                      = 11,
    VNCS_CMD_vkDestroyDevice_EXT                     = 12,
    VNCS_CMD_vkGetDeviceQueue_EXT                    = 17,
    VNCS_CMD_vkQueueSubmit_EXT                       = 18,
    VNCS_CMD_vkAllocateMemory_EXT                    = 21,
    VNCS_CMD_vkFreeMemory_EXT                        = 22,
    VNCS_CMD_vkBindBufferMemory_EXT                  = 28,
    VNCS_CMD_vkGetBufferMemoryRequirements_EXT       = 30,
    VNCS_CMD_vkCreateFence_EXT                       = 35,
    VNCS_CMD_vkDestroyFence_EXT                      = 36,
    VNCS_CMD_vkResetFences_EXT                       = 37,
    VNCS_CMD_vkGetFenceStatus_EXT                    = 38,
    VNCS_CMD_vkWaitForFences_EXT                     = 39,
    VNCS_CMD_vkCreateBuffer_EXT                      = 50,
    VNCS_CMD_vkDestroyBuffer_EXT                     = 51,
    VNCS_CMD_vkCreateShaderModule_EXT                = 59,
    VNCS_CMD_vkCreateComputePipelines_EXT            = 66,
    VNCS_CMD_vkCreatePipelineLayout_EXT              = 68,
    VNCS_CMD_vkCreateDescriptorSetLayout_EXT         = 72,
    VNCS_CMD_vkCreateDescriptorPool_EXT              = 74,
    VNCS_CMD_vkAllocateDescriptorSets_EXT            = 77,
    VNCS_CMD_vkUpdateDescriptorSets_EXT              = 79,
    VNCS_CMD_vkCreateCommandPool_EXT                 = 85,
    VNCS_CMD_vkAllocateCommandBuffers_EXT            = 88,
    VNCS_CMD_vkBeginCommandBuffer_EXT                = 90,
    VNCS_CMD_vkEndCommandBuffer_EXT                  = 91,
    VNCS_CMD_vkCmdBindPipeline_EXT                   = 93,
    VNCS_CMD_vkCmdBindDescriptorSets_EXT             = 103,
    VNCS_CMD_vkCmdDispatch_EXT                       = 110,
    VNCS_CMD_vkCmdPushConstants_EXT                  = 132,
    VNCS_CMD_vkSetReplyCommandStreamMESA_EXT         = 178,
    VNCS_CMD_vkCreateRingMESA_EXT                    = 188,
    VNCS_CMD_vkDestroyRingMESA_EXT                   = 189,
    VNCS_CMD_vkNotifyRingMESA_EXT                    = 190,
    VNCS_CMD_vkGetMemoryResourcePropertiesMESA_EXT   = 192,
} vncs_cmd_type_t;

#define VNCS_COMMAND_GENERATE_REPLY_BIT  0x00000001u

/* VkStructureType values used by the emitted commands */
#define VNCS_STYPE_APPLICATION_INFO                    0
#define VNCS_STYPE_INSTANCE_CREATE_INFO                1
#define VNCS_STYPE_DEVICE_QUEUE_CREATE_INFO            2
#define VNCS_STYPE_DEVICE_CREATE_INFO                  3
#define VNCS_STYPE_SUBMIT_INFO                         4
#define VNCS_STYPE_MEMORY_ALLOCATE_INFO                5
#define VNCS_STYPE_FENCE_CREATE_INFO                   8
#define VNCS_STYPE_BUFFER_CREATE_INFO                  12
#define VNCS_STYPE_SHADER_MODULE_CREATE_INFO           16
#define VNCS_STYPE_PIPELINE_SHADER_STAGE_CREATE_INFO   18
#define VNCS_STYPE_COMPUTE_PIPELINE_CREATE_INFO        29
#define VNCS_STYPE_PIPELINE_LAYOUT_CREATE_INFO         30
#define VNCS_STYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO   32
#define VNCS_STYPE_DESCRIPTOR_POOL_CREATE_INFO         33
#define VNCS_STYPE_DESCRIPTOR_SET_ALLOCATE_INFO        34
#define VNCS_STYPE_WRITE_DESCRIPTOR_SET                35
#define VNCS_STYPE_COMMAND_POOL_CREATE_INFO            39
#define VNCS_STYPE_COMMAND_BUFFER_ALLOCATE_INFO        40
#define VNCS_STYPE_COMMAND_BUFFER_BEGIN_INFO           42
/* VK_MESA_venus_protocol private sTypes (vn_protocol_driver_defines.h) */
#define VNCS_STYPE_RING_CREATE_INFO_MESA               1000384000
#define VNCS_STYPE_MEMORY_RESOURCE_PROPERTIES_MESA     1000384001
#define VNCS_STYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA    1000384002

/* Vk* constants used by the compute backend */
#define VNCS_VK_SUCCESS                     0
#define VNCS_QUEUE_COMPUTE_BIT              0x00000002u
#define VNCS_DESCRIPTOR_TYPE_STORAGE_BUFFER 7
#define VNCS_SHADER_STAGE_COMPUTE_BIT       0x00000020u
#define VNCS_BUFFER_USAGE_STORAGE_BUFFER    0x00000020u
#define VNCS_PIPELINE_BIND_POINT_COMPUTE    1
#define VNCS_SHARING_MODE_EXCLUSIVE         0
#define VNCS_COMMAND_BUFFER_LEVEL_PRIMARY   0
#define VNCS_MEMORY_PROPERTY_HOST_VISIBLE   0x00000002u
#define VNCS_MEMORY_PROPERTY_HOST_COHERENT  0x00000004u
#define VNCS_MAX_MEMORY_TYPES               32
#define VNCS_MAX_MEMORY_HEAPS               16
#define VNCS_MAX_PHYSICAL_DEVICE_NAME_SIZE  256
#define VNCS_UUID_SIZE                      16
#define VNCS_WHOLE_SIZE                     (~0ULL)

/* ============================================================================
 * Encoder — cursor over a caller-provided buffer (single-buffer variant of
 * Mesa's struct vn_cs_encoder; same byte semantics as vn_cs_encoder_write:
 * wire size is always a multiple of 4, tail padding zeroed for determinism).
 * ============================================================================ */

typedef struct vncs_encoder {
    uint8_t *cur;
    uint8_t *end;
    bool fatal;     /* set on overflow; all further writes become no-ops */
} vncs_encoder_t;

typedef struct vncs_decoder {
    const uint8_t *cur;
    const uint8_t *end;
    bool fatal;     /* set on underrun/parse error */
} vncs_decoder_t;

static inline void vncs_encoder_init(vncs_encoder_t *e, void *buf, size_t size)
{
    e->cur = (uint8_t *)buf;
    e->end = (uint8_t *)buf + size;
    e->fatal = false;
}

/* bytes written so far, given the original buffer base */
static inline size_t vncs_len(const vncs_encoder_t *e, const void *base)
{
    return (size_t)(e->cur - (const uint8_t *)base);
}

static inline void vncs_decoder_init(vncs_decoder_t *d, const void *buf,
                                     size_t size)
{
    d->cur = (const uint8_t *)buf;
    d->end = (const uint8_t *)buf + size;
    d->fatal = false;
}

/* Core write: `size` wire bytes (multiple of 4), first `val_size` bytes
 * copied from `val`, tail zero-padded (Mesa leaves padding uninit; we zero
 * it so streams are reproducible byte-for-byte). */
static inline void vncs_write(vncs_encoder_t *e, size_t size, const void *val,
                              size_t val_size)
{
    if (e->fatal) {
        return;
    }
    if (size > (size_t)(e->end - e->cur) || val_size > size) {
        e->fatal = true;
        return;
    }
    memcpy(e->cur, val, val_size);
    if (size > val_size) {
        memset(e->cur + val_size, 0, size - val_size);
    }
    e->cur += size;
}

static inline void vncs_read(vncs_decoder_t *d, size_t size, void *val,
                             size_t val_size)
{
    if (d->fatal) {
        return;
    }
    if (size > (size_t)(d->end - d->cur) || val_size > size) {
        d->fatal = true;
        memset(val, 0, val_size);
        return;
    }
    memcpy(val, d->cur, val_size);
    d->cur += size;
}

static inline void vncs_skip(vncs_decoder_t *d, size_t size)
{
    if (d->fatal) {
        return;
    }
    if (size > (size_t)(d->end - d->cur)) {
        d->fatal = true;
        return;
    }
    d->cur += size;
}

static inline uint64_t vncs_peek_u64(const vncs_decoder_t *d)
{
    uint64_t v = 0;
    if ((size_t)(d->end - d->cur) >= 8) {
        memcpy(&v, d->cur, 8);
    }
    return v;
}

/* ---- scalar primitives (vn_encode_* equivalents) ---- */

static inline void vncs_u32(vncs_encoder_t *e, uint32_t v)
{
    vncs_write(e, 4, &v, 4);
}

static inline void vncs_i32(vncs_encoder_t *e, int32_t v)
{
    vncs_write(e, 4, &v, 4);
}

static inline void vncs_u64(vncs_encoder_t *e, uint64_t v)
{
    vncs_write(e, 8, &v, 8);
}

static inline void vncs_f32(vncs_encoder_t *e, float v)
{
    vncs_write(e, 4, &v, 4);
}

static inline void vncs_blob(vncs_encoder_t *e, const void *data, size_t size)
{
    vncs_write(e, (size + 3) & ~(size_t)3, data, size);
}

/* u64 array_size (vn_encode_array_size) */
static inline void vncs_array_size(vncs_encoder_t *e, uint64_t count)
{
    vncs_u64(e, count);
}

/* non-array pointer presence word; returns whether contents follow
 * (vn_encode_simple_pointer) */
static inline bool vncs_ptr(vncs_encoder_t *e, bool present)
{
    vncs_array_size(e, present ? 1 : 0);
    return present;
}

static inline void vncs_string(vncs_encoder_t *e, const char *s)
{
    if (s) {
        size_t n = strlen(s) + 1;
        vncs_array_size(e, n);
        vncs_blob(e, s, n);
    } else {
        vncs_array_size(e, 0);
    }
}

/* handle = guest-assigned u64 object id */
static inline void vncs_handle(vncs_encoder_t *e, uint64_t id)
{
    vncs_u64(e, id);
}

static inline void vncs_handle_array(vncs_encoder_t *e, const uint64_t *ids,
                                     uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        vncs_handle(e, ids[i]);
    }
}

/* record header: VkCommandTypeEXT + VkCommandFlagsEXT */
static inline void vncs_cmd_header(vncs_encoder_t *e, uint32_t type,
                                   uint32_t flags)
{
    vncs_u32(e, type);
    vncs_u32(e, flags);
}

/* ---- decoder primitives ---- */

static inline uint32_t vncr_u32(vncs_decoder_t *d)
{
    uint32_t v = 0;
    vncs_read(d, 4, &v, 4);
    return v;
}

static inline int32_t vncr_i32(vncs_decoder_t *d)
{
    int32_t v = 0;
    vncs_read(d, 4, &v, 4);
    return v;
}

static inline uint64_t vncr_u64(vncs_decoder_t *d)
{
    uint64_t v = 0;
    vncs_read(d, 8, &v, 8);
    return v;
}

/* array_size, checked against an expected value (vn_decode_array_size) */
static inline uint64_t vncr_array_size(vncs_decoder_t *d, uint64_t expected)
{
    uint64_t v = vncr_u64(d);
    if (v != expected) {
        d->fatal = true;
        return 0;
    }
    return v;
}

static inline uint64_t vncr_array_size_any(vncs_decoder_t *d)
{
    return vncr_u64(d);
}

static inline bool vncr_ptr(vncs_decoder_t *d)
{
    return vncr_u64(d) != 0;
}

static inline uint64_t vncr_handle(vncs_decoder_t *d)
{
    return vncr_u64(d);
}

/* ============================================================================
 * Command encoders (vn_encode_* equivalents)
 *
 * All handles are guest-assigned u64 object ids. `flags` is normally 0 or
 * VNCS_COMMAND_GENERATE_REPLY_BIT when a reply record is wanted.
 * ============================================================================ */

/* ---- vkCreateInstance (id 0) ---- */

static inline void vncs_vkCreateInstance(vncs_encoder_t *e, uint32_t flags,
                                         const char *app_name,
                                         uint32_t app_version,
                                         const char *engine_name,
                                         uint32_t engine_version,
                                         uint32_t api_version,
                                         uint64_t out_instance)
{
    vncs_cmd_header(e, VNCS_CMD_vkCreateInstance_EXT, flags);

    /* pCreateInfo: VkInstanceCreateInfo */
    if (vncs_ptr(e, true)) {
        vncs_i32(e, VNCS_STYPE_INSTANCE_CREATE_INFO);
        vncs_ptr(e, false);                     /* pNext */
        vncs_u32(e, 0);                         /* flags */
        if (vncs_ptr(e, true)) {                /* pApplicationInfo */
            vncs_i32(e, VNCS_STYPE_APPLICATION_INFO);
            vncs_ptr(e, false);                 /* pNext */
            vncs_string(e, app_name);
            vncs_u32(e, app_version);
            vncs_string(e, engine_name);
            vncs_u32(e, engine_version);
            vncs_u32(e, api_version);
        }
        vncs_u32(e, 0);                         /* enabledLayerCount */
        vncs_array_size(e, 0);                  /* ppEnabledLayerNames */
        vncs_u32(e, 0);                         /* enabledExtensionCount */
        vncs_array_size(e, 0);                  /* ppEnabledExtensionNames */
    }
    vncs_ptr(e, false);                         /* pAllocator */
    if (vncs_ptr(e, true)) {                    /* pInstance */
        vncs_handle(e, out_instance);
    }
}

static inline int32_t vncr_vkCreateInstance_reply(vncs_decoder_t *d,
                                                  uint64_t *out_instance)
{
    if (vncr_u32(d) != VNCS_CMD_vkCreateInstance_EXT) {
        d->fatal = true;
        return -100;
    }
    int32_t ret = vncr_i32(d);
    if (vncr_ptr(d)) {
        *out_instance = vncr_handle(d);
    }
    return ret;
}

/* ---- vkDestroyInstance (id 1) ---- */

static inline void vncs_vkDestroyInstance(vncs_encoder_t *e, uint32_t flags,
                                          uint64_t instance)
{
    vncs_cmd_header(e, VNCS_CMD_vkDestroyInstance_EXT, flags);
    vncs_handle(e, instance);
    vncs_ptr(e, false);                         /* pAllocator */
}

/* ---- vkEnumeratePhysicalDevices (id 2) ---- */

static inline void vncs_vkEnumeratePhysicalDevices(vncs_encoder_t *e,
                                                   uint32_t flags,
                                                   uint64_t instance,
                                                   uint32_t count,
                                                   const uint64_t *devices)
{
    vncs_cmd_header(e, VNCS_CMD_vkEnumeratePhysicalDevices_EXT, flags);
    vncs_handle(e, instance);
    if (vncs_ptr(e, true)) {                    /* pPhysicalDeviceCount */
        vncs_u32(e, count);
    }
    vncs_array_size(e, count);                  /* pPhysicalDevices */
    vncs_handle_array(e, devices, count);
}

static inline int32_t vncr_vkEnumeratePhysicalDevices_reply(
    vncs_decoder_t *d, uint32_t *count, uint64_t *devices, uint32_t max)
{
    if (vncr_u32(d) != VNCS_CMD_vkEnumeratePhysicalDevices_EXT) {
        d->fatal = true;
        return -100;
    }
    int32_t ret = vncr_i32(d);
    if (vncr_ptr(d)) {
        *count = vncr_u32(d);
    }
    if (vncs_peek_u64(d)) {
        uint64_t n = vncr_array_size(d, *count);
        for (uint64_t i = 0; i < n; i++) {
            uint64_t id = vncr_handle(d);
            if (i < max) {
                devices[i] = id;
            }
        }
    } else {
        vncr_array_size_any(d);
    }
    return ret;
}

/* ---- vkGetPhysicalDeviceProperties (id 6) ---- */

static inline void vncs_vkGetPhysicalDeviceProperties(vncs_encoder_t *e,
                                                      uint32_t flags,
                                                      uint64_t phys)
{
    vncs_cmd_header(e, VNCS_CMD_vkGetPhysicalDeviceProperties_EXT, flags);
    vncs_handle(e, phys);
    vncs_ptr(e, true);          /* pProperties (partial: emits nothing) */
}

/* Wire size of VkPhysicalDeviceProperties in a reply:
 *   5 scalars (apiVersion, driverVersion, vendorID, deviceID, deviceType)
 *   + array_size + deviceName[256] + array_size + pipelineCacheUUID[16]
 *   + VkPhysicalDeviceLimits (436 bytes of scalars + 6 fixed arrays with
 *     8-byte array_size prefixes: u32[3], u32[3], u32[2], f32[2] x3 = 104)
 *   + VkPhysicalDeviceSparseProperties (5 x VkBool32) */
#define VNCS_WIRE_PHYSICAL_DEVICE_LIMITS          (436 + 104)
#define VNCS_WIRE_PHYSICAL_DEVICE_SPARSE          20
#define VNCS_WIRE_PHYSICAL_DEVICE_PROPERTIES      \
    (20 + 8 + 256 + 8 + 16 + VNCS_WIRE_PHYSICAL_DEVICE_LIMITS + \
     VNCS_WIRE_PHYSICAL_DEVICE_SPARSE)

/* Reply has no VkResult (void command): type + presence + struct. */
static inline void vncr_vkGetPhysicalDeviceProperties_reply(
    vncs_decoder_t *d, uint32_t *vendor_id, uint32_t *device_type,
    char *name /* VNCS_MAX_PHYSICAL_DEVICE_NAME_SIZE bytes */)
{
    if (vncr_u32(d) != VNCS_CMD_vkGetPhysicalDeviceProperties_EXT) {
        d->fatal = true;
        return;
    }
    if (!vncr_ptr(d)) {
        return;
    }
    vncr_u32(d);                                /* apiVersion */
    vncr_u32(d);                                /* driverVersion */
    *vendor_id = vncr_u32(d);
    vncr_u32(d);                                /* deviceID */
    *device_type = vncr_u32(d);                 /* VkPhysicalDeviceType */
    uint64_t nlen = vncr_array_size(d, VNCS_MAX_PHYSICAL_DEVICE_NAME_SIZE);
    if (name && !d->fatal) {
        vncs_read(d, (nlen + 3) & ~3ULL, name, (size_t)nlen);
        name[255] = '\0';
    } else {
        vncs_skip(d, (nlen + 3) & ~3ULL);
    }
    vncr_array_size(d, VNCS_UUID_SIZE);         /* pipelineCacheUUID */
    vncs_skip(d, 16);
    vncs_skip(d, VNCS_WIRE_PHYSICAL_DEVICE_LIMITS +
                 VNCS_WIRE_PHYSICAL_DEVICE_SPARSE);
}

/* ---- vkGetPhysicalDeviceQueueFamilyProperties (id 7) ---- */

static inline void vncs_vkGetPhysicalDeviceQueueFamilyProperties(
    vncs_encoder_t *e, uint32_t flags, uint64_t phys, uint32_t count)
{
    vncs_cmd_header(e,
                    VNCS_CMD_vkGetPhysicalDeviceQueueFamilyProperties_EXT,
                    flags);
    vncs_handle(e, phys);
    if (vncs_ptr(e, true)) {                    /* pQueueFamilyPropertyCount */
        vncs_u32(e, count);
    }
    /* pQueueFamilyProperties: array_size only (partial emits nothing) */
    vncs_array_size(e, count);
}

typedef struct vncs_queue_family_props {
    uint32_t queue_flags;
    uint32_t queue_count;
    uint32_t timestamp_valid_bits;
    uint32_t min_transfer_w, min_transfer_h, min_transfer_d;
} vncs_queue_family_props_t;

static inline void vncr_vkGetPhysicalDeviceQueueFamilyProperties_reply(
    vncs_decoder_t *d, uint32_t *count, vncs_queue_family_props_t *props,
    uint32_t max)
{
    if (vncr_u32(d) !=
        VNCS_CMD_vkGetPhysicalDeviceQueueFamilyProperties_EXT) {
        d->fatal = true;
        return;
    }
    if (vncr_ptr(d)) {
        *count = vncr_u32(d);
    }
    if (vncs_peek_u64(d)) {
        uint64_t n = vncr_array_size(d, *count);
        for (uint64_t i = 0; i < n; i++) {
            vncs_queue_family_props_t p;
            p.queue_flags = vncr_u32(d);
            p.queue_count = vncr_u32(d);
            p.timestamp_valid_bits = vncr_u32(d);
            p.min_transfer_w = vncr_u32(d);
            p.min_transfer_h = vncr_u32(d);
            p.min_transfer_d = vncr_u32(d);
            if (i < max) {
                props[i] = p;
            }
        }
    } else {
        vncr_array_size_any(d);
    }
}

/* ---- vkGetPhysicalDeviceMemoryProperties (id 8) ---- */

static inline void vncs_vkGetPhysicalDeviceMemoryProperties(
    vncs_encoder_t *e, uint32_t flags, uint64_t phys)
{
    vncs_cmd_header(e, VNCS_CMD_vkGetPhysicalDeviceMemoryProperties_EXT,
                    flags);
    vncs_handle(e, phys);
    vncs_ptr(e, true);                          /* pMemoryProperties */
    /* partial: fixed arrays only */
    vncs_array_size(e, VNCS_MAX_MEMORY_TYPES);
    vncs_array_size(e, VNCS_MAX_MEMORY_HEAPS);
}

typedef struct vncs_memory_type {
    uint32_t property_flags;
    uint32_t heap_index;
} vncs_memory_type_t;

typedef struct vncs_memory_props {
    uint32_t memory_type_count;
    vncs_memory_type_t memory_types[VNCS_MAX_MEMORY_TYPES];
    uint32_t memory_heap_count;
    struct {
        uint64_t size;
        uint32_t flags;
    } memory_heaps[VNCS_MAX_MEMORY_HEAPS];
} vncs_memory_props_t;

static inline void vncr_vkGetPhysicalDeviceMemoryProperties_reply(
    vncs_decoder_t *d, vncs_memory_props_t *props)
{
    if (vncr_u32(d) != VNCS_CMD_vkGetPhysicalDeviceMemoryProperties_EXT) {
        d->fatal = true;
        return;
    }
    if (!vncr_ptr(d)) {
        return;
    }
    props->memory_type_count = vncr_u32(d);
    vncr_array_size(d, VNCS_MAX_MEMORY_TYPES);
    for (uint32_t i = 0; i < VNCS_MAX_MEMORY_TYPES; i++) {
        props->memory_types[i].property_flags = vncr_u32(d);
        props->memory_types[i].heap_index = vncr_u32(d);
    }
    props->memory_heap_count = vncr_u32(d);
    vncr_array_size(d, VNCS_MAX_MEMORY_HEAPS);
    for (uint32_t i = 0; i < VNCS_MAX_MEMORY_HEAPS; i++) {
        props->memory_heaps[i].size = vncr_u64(d);
        props->memory_heaps[i].flags = vncr_u32(d);
    }
}

/* ---- vkCreateDevice (id 11): one compute queue family, no features ---- */

static inline void vncs_vkCreateDevice(vncs_encoder_t *e, uint32_t flags,
                                       uint64_t phys,
                                       uint32_t queue_family_index,
                                       float queue_priority,
                                       uint64_t out_device)
{
    vncs_cmd_header(e, VNCS_CMD_vkCreateDevice_EXT, flags);
    vncs_handle(e, phys);
    if (vncs_ptr(e, true)) {                    /* pCreateInfo */
        vncs_i32(e, VNCS_STYPE_DEVICE_CREATE_INFO);
        vncs_ptr(e, false);                     /* pNext */
        vncs_u32(e, 0);                         /* flags */
        vncs_u32(e, 1);                         /* queueCreateInfoCount */
        vncs_array_size(e, 1);                  /* pQueueCreateInfos */
        {
            vncs_i32(e, VNCS_STYPE_DEVICE_QUEUE_CREATE_INFO);
            vncs_ptr(e, false);                 /* pNext */
            vncs_u32(e, 0);                     /* flags */
            vncs_u32(e, queue_family_index);
            vncs_u32(e, 1);                     /* queueCount */
            vncs_array_size(e, 1);              /* pQueuePriorities */
            vncs_f32(e, queue_priority);
        }
        vncs_u32(e, 0);                         /* enabledLayerCount */
        vncs_array_size(e, 0);
        vncs_u32(e, 0);                         /* enabledExtensionCount */
        vncs_array_size(e, 0);
        vncs_ptr(e, false);                     /* pEnabledFeatures */
    }
    vncs_ptr(e, false);                         /* pAllocator */
    if (vncs_ptr(e, true)) {                    /* pDevice */
        vncs_handle(e, out_device);
    }
}

static inline int32_t vncr_vkCreateDevice_reply(vncs_decoder_t *d,
                                                uint64_t *out_device)
{
    if (vncr_u32(d) != VNCS_CMD_vkCreateDevice_EXT) {
        d->fatal = true;
        return -100;
    }
    int32_t ret = vncr_i32(d);
    if (vncr_ptr(d)) {
        *out_device = vncr_handle(d);
    }
    return ret;
}

/* ---- vkGetDeviceQueue (id 17, no reply) ---- */

static inline void vncs_vkGetDeviceQueue(vncs_encoder_t *e, uint32_t flags,
                                         uint64_t device,
                                         uint32_t queue_family_index,
                                         uint32_t queue_index,
                                         uint64_t out_queue)
{
    vncs_cmd_header(e, VNCS_CMD_vkGetDeviceQueue_EXT, flags);
    vncs_handle(e, device);
    vncs_u32(e, queue_family_index);
    vncs_u32(e, queue_index);
    if (vncs_ptr(e, true)) {
        vncs_handle(e, out_queue);
    }
}

/* ---- vkCreateBuffer (id 50) ---- */

static inline void vncs_vkCreateBuffer(vncs_encoder_t *e, uint32_t flags,
                                       uint64_t device, uint64_t size,
                                       uint32_t usage, uint64_t out_buffer)
{
    vncs_cmd_header(e, VNCS_CMD_vkCreateBuffer_EXT, flags);
    vncs_handle(e, device);
    if (vncs_ptr(e, true)) {                    /* pCreateInfo */
        vncs_i32(e, VNCS_STYPE_BUFFER_CREATE_INFO);
        vncs_ptr(e, false);                     /* pNext */
        vncs_u32(e, 0);                         /* flags */
        vncs_u64(e, size);
        vncs_u32(e, usage);
        vncs_i32(e, VNCS_SHARING_MODE_EXCLUSIVE);
        vncs_u32(e, 0);                         /* queueFamilyIndexCount */
        vncs_array_size(e, 0);                  /* pQueueFamilyIndices */
    }
    vncs_ptr(e, false);                         /* pAllocator */
    if (vncs_ptr(e, true)) {                    /* pBuffer */
        vncs_handle(e, out_buffer);
    }
}

static inline int32_t vncr_vkCreateBuffer_reply(vncs_decoder_t *d,
                                                uint64_t *out_buffer)
{
    if (vncr_u32(d) != VNCS_CMD_vkCreateBuffer_EXT) {
        d->fatal = true;
        return -100;
    }
    int32_t ret = vncr_i32(d);
    if (vncr_ptr(d)) {
        *out_buffer = vncr_handle(d);
    }
    return ret;
}

/* ---- vkDestroyBuffer (id 51, no reply) ---- */

static inline void vncs_vkDestroyBuffer(vncs_encoder_t *e, uint32_t flags,
                                        uint64_t device, uint64_t buffer)
{
    vncs_cmd_header(e, VNCS_CMD_vkDestroyBuffer_EXT, flags);
    vncs_handle(e, device);
    vncs_handle(e, buffer);
    vncs_ptr(e, false);                         /* pAllocator */
}

/* ---- vkGetBufferMemoryRequirements (id 30) ---- */

static inline void vncs_vkGetBufferMemoryRequirements(vncs_encoder_t *e,
                                                      uint32_t flags,
                                                      uint64_t device,
                                                      uint64_t buffer)
{
    vncs_cmd_header(e, VNCS_CMD_vkGetBufferMemoryRequirements_EXT, flags);
    vncs_handle(e, device);
    vncs_handle(e, buffer);
    vncs_ptr(e, true);      /* pMemoryRequirements (partial: nothing) */
}

/* Reply has no VkResult (void command): type + presence + struct. */
static inline void vncr_vkGetBufferMemoryRequirements_reply(
    vncs_decoder_t *d, uint64_t *size, uint64_t *alignment,
    uint32_t *memory_type_bits)
{
    if (vncr_u32(d) != VNCS_CMD_vkGetBufferMemoryRequirements_EXT) {
        d->fatal = true;
        return;
    }
    if (vncr_ptr(d)) {
        *size = vncr_u64(d);
        *alignment = vncr_u64(d);
        *memory_type_bits = vncr_u32(d);
    }
}

/* ---- vkAllocateMemory (id 21) ----
 * import_resource_id != 0 chains VkImportMemoryResourceInfoMESA on pNext. */

static inline void vncs_vkAllocateMemory(vncs_encoder_t *e, uint32_t flags,
                                         uint64_t device,
                                         uint64_t allocation_size,
                                         uint32_t memory_type_index,
                                         uint32_t import_resource_id,
                                         uint64_t out_memory)
{
    vncs_cmd_header(e, VNCS_CMD_vkAllocateMemory_EXT, flags);
    vncs_handle(e, device);
    if (vncs_ptr(e, true)) {                    /* pAllocateInfo */
        vncs_i32(e, VNCS_STYPE_MEMORY_ALLOCATE_INFO);
        if (vncs_ptr(e, import_resource_id != 0)) { /* pNext */
            vncs_i32(e, VNCS_STYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA);
            vncs_ptr(e, false);                 /* pNext of the chain node */
            vncs_u32(e, import_resource_id);
        }
        vncs_u64(e, allocation_size);
        vncs_u32(e, memory_type_index);
    }
    vncs_ptr(e, false);                         /* pAllocator */
    if (vncs_ptr(e, true)) {                    /* pMemory */
        vncs_handle(e, out_memory);
    }
}

static inline int32_t vncr_vkAllocateMemory_reply(vncs_decoder_t *d,
                                                  uint64_t *out_memory)
{
    if (vncr_u32(d) != VNCS_CMD_vkAllocateMemory_EXT) {
        d->fatal = true;
        return -100;
    }
    int32_t ret = vncr_i32(d);
    if (vncr_ptr(d)) {
        *out_memory = vncr_handle(d);
    }
    return ret;
}

/* ---- vkFreeMemory (id 22, no reply) ---- */

static inline void vncs_vkFreeMemory(vncs_encoder_t *e, uint32_t flags,
                                     uint64_t device, uint64_t memory)
{
    vncs_cmd_header(e, VNCS_CMD_vkFreeMemory_EXT, flags);
    vncs_handle(e, device);
    vncs_handle(e, memory);
    vncs_ptr(e, false);                         /* pAllocator */
}

/* ---- vkBindBufferMemory (id 28) ---- */

static inline void vncs_vkBindBufferMemory(vncs_encoder_t *e, uint32_t flags,
                                           uint64_t device, uint64_t buffer,
                                           uint64_t memory, uint64_t offset)
{
    vncs_cmd_header(e, VNCS_CMD_vkBindBufferMemory_EXT, flags);
    vncs_handle(e, device);
    vncs_handle(e, buffer);
    vncs_handle(e, memory);
    vncs_u64(e, offset);
}

static inline int32_t vncr_ret_only_reply(vncs_decoder_t *d,
                                          uint32_t expected_type)
{
    if (vncr_u32(d) != expected_type) {
        d->fatal = true;
        return -100;
    }
    return vncr_i32(d);
}

/* ---- vkGetMemoryResourcePropertiesMESA (id 192) ---- */

static inline void vncs_vkGetMemoryResourcePropertiesMESA(vncs_encoder_t *e,
                                                          uint32_t flags,
                                                          uint64_t device,
                                                          uint32_t resource_id)
{
    vncs_cmd_header(e, VNCS_CMD_vkGetMemoryResourcePropertiesMESA_EXT, flags);
    vncs_handle(e, device);
    vncs_u32(e, resource_id);
    if (vncs_ptr(e, true)) {  /* pMemoryResourceProperties (partial) */
        vncs_i32(e, VNCS_STYPE_MEMORY_RESOURCE_PROPERTIES_MESA);
        vncs_ptr(e, false);                     /* pNext */
        /* self_partial: memoryTypeBits skipped (out-only) */
    }
}

static inline int32_t vncr_vkGetMemoryResourcePropertiesMESA_reply(
    vncs_decoder_t *d, uint32_t *memory_type_bits)
{
    if (vncr_u32(d) != VNCS_CMD_vkGetMemoryResourcePropertiesMESA_EXT) {
        d->fatal = true;
        return -100;
    }
    int32_t ret = vncr_i32(d);
    if (vncr_ptr(d)) {
        /* VkMemoryResourcePropertiesMESA: sType + pNext(NULL) + self */
        if (vncr_i32(d) != VNCS_STYPE_MEMORY_RESOURCE_PROPERTIES_MESA) {
            d->fatal = true;
            return -100;
        }
        if (vncr_ptr(d)) {  /* pNext chain: not expected from our request */
            d->fatal = true;
            return -100;
        }
        *memory_type_bits = vncr_u32(d);
    }
    return ret;
}

/* ---- vkCreateShaderModule (id 59) ---- */

static inline void vncs_vkCreateShaderModule(vncs_encoder_t *e, uint32_t flags,
                                             uint64_t device,
                                             const uint32_t *code,
                                             uint32_t code_word_count,
                                             uint64_t out_module)
{
    vncs_cmd_header(e, VNCS_CMD_vkCreateShaderModule_EXT, flags);
    vncs_handle(e, device);
    if (vncs_ptr(e, true)) {                    /* pCreateInfo */
        vncs_i32(e, VNCS_STYPE_SHADER_MODULE_CREATE_INFO);
        vncs_ptr(e, false);                     /* pNext */
        vncs_u32(e, 0);                         /* flags */
        vncs_u64(e, (uint64_t)code_word_count * 4);  /* codeSize (size_t) */
        vncs_array_size(e, code_word_count);
        vncs_write(e, (size_t)code_word_count * 4, code,
                   (size_t)code_word_count * 4);
    }
    vncs_ptr(e, false);                         /* pAllocator */
    if (vncs_ptr(e, true)) {                    /* pShaderModule */
        vncs_handle(e, out_module);
    }
}

static inline int32_t vncr_vkCreateShaderModule_reply(vncs_decoder_t *d,
                                                      uint64_t *out_module)
{
    if (vncr_u32(d) != VNCS_CMD_vkCreateShaderModule_EXT) {
        d->fatal = true;
        return -100;
    }
    int32_t ret = vncr_i32(d);
    if (vncr_ptr(d)) {
        *out_module = vncr_handle(d);
    }
    return ret;
}

/* ---- vkCreateDescriptorSetLayout (id 72) ---- */

typedef struct vncs_dset_layout_binding {
    uint32_t binding;
    uint32_t descriptor_type;   /* VkDescriptorType */
    uint32_t descriptor_count;
    uint32_t stage_flags;
} vncs_dset_layout_binding_t;

static inline void vncs_vkCreateDescriptorSetLayout(
    vncs_encoder_t *e, uint32_t flags, uint64_t device,
    const vncs_dset_layout_binding_t *bindings, uint32_t binding_count,
    uint64_t out_layout)
{
    vncs_cmd_header(e, VNCS_CMD_vkCreateDescriptorSetLayout_EXT, flags);
    vncs_handle(e, device);
    if (vncs_ptr(e, true)) {                    /* pCreateInfo */
        vncs_i32(e, VNCS_STYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        vncs_ptr(e, false);                     /* pNext */
        vncs_u32(e, 0);                         /* flags */
        vncs_u32(e, binding_count);
        vncs_array_size(e, binding_count);
        for (uint32_t i = 0; i < binding_count; i++) {
            vncs_u32(e, bindings[i].binding);
            vncs_i32(e, (int32_t)bindings[i].descriptor_type);
            vncs_u32(e, bindings[i].descriptor_count);
            vncs_u32(e, bindings[i].stage_flags);
            vncs_array_size(e, 0);              /* pImmutableSamplers */
        }
    }
    vncs_ptr(e, false);                         /* pAllocator */
    if (vncs_ptr(e, true)) {
        vncs_handle(e, out_layout);
    }
}

static inline int32_t vncr_vkCreateDescriptorSetLayout_reply(
    vncs_decoder_t *d, uint64_t *out_layout)
{
    if (vncr_u32(d) != VNCS_CMD_vkCreateDescriptorSetLayout_EXT) {
        d->fatal = true;
        return -100;
    }
    int32_t ret = vncr_i32(d);
    if (vncr_ptr(d)) {
        *out_layout = vncr_handle(d);
    }
    return ret;
}

/* ---- vkCreatePipelineLayout (id 68) ---- */

static inline void vncs_vkCreatePipelineLayout(
    vncs_encoder_t *e, uint32_t flags, uint64_t device,
    const uint64_t *set_layouts, uint32_t set_layout_count,
    uint32_t pc_stage_flags, uint32_t pc_offset, uint32_t pc_size,
    uint64_t out_layout)
{
    vncs_cmd_header(e, VNCS_CMD_vkCreatePipelineLayout_EXT, flags);
    vncs_handle(e, device);
    if (vncs_ptr(e, true)) {                    /* pCreateInfo */
        vncs_i32(e, VNCS_STYPE_PIPELINE_LAYOUT_CREATE_INFO);
        vncs_ptr(e, false);                     /* pNext */
        vncs_u32(e, 0);                         /* flags */
        vncs_u32(e, set_layout_count);
        vncs_array_size(e, set_layout_count);
        vncs_handle_array(e, set_layouts, set_layout_count);
        vncs_u32(e, pc_size ? 1 : 0);           /* pushConstantRangeCount */
        if (pc_size) {
            vncs_array_size(e, 1);
            vncs_u32(e, pc_stage_flags);
            vncs_u32(e, pc_offset);
            vncs_u32(e, pc_size);
        } else {
            vncs_array_size(e, 0);
        }
    }
    vncs_ptr(e, false);                         /* pAllocator */
    if (vncs_ptr(e, true)) {
        vncs_handle(e, out_layout);
    }
}

static inline int32_t vncr_vkCreatePipelineLayout_reply(vncs_decoder_t *d,
                                                        uint64_t *out_layout)
{
    if (vncr_u32(d) != VNCS_CMD_vkCreatePipelineLayout_EXT) {
        d->fatal = true;
        return -100;
    }
    int32_t ret = vncr_i32(d);
    if (vncr_ptr(d)) {
        *out_layout = vncr_handle(d);
    }
    return ret;
}

/* ---- vkCreateComputePipelines (id 66) ---- */

static inline void vncs_vkCreateComputePipelines(
    vncs_encoder_t *e, uint32_t flags, uint64_t device,
    const uint64_t *modules, uint64_t layout,
    uint64_t *out_pipelines, uint32_t count)
{
    vncs_cmd_header(e, VNCS_CMD_vkCreateComputePipelines_EXT, flags);
    vncs_handle(e, device);
    vncs_handle(e, 0);                          /* pipelineCache = VK_NULL */
    vncs_u32(e, count);
    vncs_array_size(e, count);                  /* pCreateInfos */
    for (uint32_t i = 0; i < count; i++) {
        vncs_i32(e, VNCS_STYPE_COMPUTE_PIPELINE_CREATE_INFO);
        vncs_ptr(e, false);                     /* pNext */
        vncs_u32(e, 0);                         /* flags */
        /* stage: VkPipelineShaderStageCreateInfo */
        vncs_i32(e, VNCS_STYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        vncs_ptr(e, false);                     /* pNext */
        vncs_u32(e, 0);                         /* flags */
        vncs_i32(e, VNCS_SHADER_STAGE_COMPUTE_BIT);
        vncs_handle(e, modules[i]);
        vncs_string(e, "main");
        vncs_ptr(e, false);                     /* pSpecializationInfo */
        /* */
        vncs_handle(e, layout);
        vncs_handle(e, 0);                      /* basePipelineHandle */
        vncs_i32(e, 0);                         /* basePipelineIndex */
    }
    vncs_ptr(e, false);                         /* pAllocator */
    vncs_array_size(e, count);                  /* pPipelines */
    vncs_handle_array(e, out_pipelines, count);
}

static inline int32_t vncr_vkCreateComputePipelines_reply(
    vncs_decoder_t *d, uint64_t *pipelines, uint32_t count)
{
    if (vncr_u32(d) != VNCS_CMD_vkCreateComputePipelines_EXT) {
        d->fatal = true;
        return -100;
    }
    int32_t ret = vncr_i32(d);
    if (vncs_peek_u64(d)) {
        uint64_t n = vncr_array_size(d, count);
        for (uint64_t i = 0; i < n; i++) {
            uint64_t id = vncr_handle(d);
            if (i < count) {
                pipelines[i] = id;
            }
        }
    } else {
        vncr_array_size_any(d);
    }
    return ret;
}

/* ---- vkCreateDescriptorPool (id 74) ---- */

static inline void vncs_vkCreateDescriptorPool(vncs_encoder_t *e,
                                               uint32_t flags,
                                               uint64_t device,
                                               uint32_t max_sets,
                                               uint32_t descriptor_type,
                                               uint32_t descriptor_count,
                                               uint64_t out_pool)
{
    vncs_cmd_header(e, VNCS_CMD_vkCreateDescriptorPool_EXT, flags);
    vncs_handle(e, device);
    if (vncs_ptr(e, true)) {                    /* pCreateInfo */
        vncs_i32(e, VNCS_STYPE_DESCRIPTOR_POOL_CREATE_INFO);
        vncs_ptr(e, false);                     /* pNext */
        vncs_u32(e, 0);                         /* flags */
        vncs_u32(e, max_sets);
        vncs_u32(e, 1);                         /* poolSizeCount */
        vncs_array_size(e, 1);
        vncs_i32(e, (int32_t)descriptor_type);
        vncs_u32(e, descriptor_count);
    }
    vncs_ptr(e, false);                         /* pAllocator */
    if (vncs_ptr(e, true)) {
        vncs_handle(e, out_pool);
    }
}

static inline int32_t vncr_vkCreateDescriptorPool_reply(vncs_decoder_t *d,
                                                        uint64_t *out_pool)
{
    if (vncr_u32(d) != VNCS_CMD_vkCreateDescriptorPool_EXT) {
        d->fatal = true;
        return -100;
    }
    int32_t ret = vncr_i32(d);
    if (vncr_ptr(d)) {
        *out_pool = vncr_handle(d);
    }
    return ret;
}

/* ---- vkAllocateDescriptorSets (id 77) ---- */

static inline void vncs_vkAllocateDescriptorSets(vncs_encoder_t *e,
                                                 uint32_t flags,
                                                 uint64_t device,
                                                 uint64_t pool,
                                                 const uint64_t *layouts,
                                                 uint64_t *out_sets,
                                                 uint32_t count)
{
    vncs_cmd_header(e, VNCS_CMD_vkAllocateDescriptorSets_EXT, flags);
    vncs_handle(e, device);
    if (vncs_ptr(e, true)) {                    /* pAllocateInfo */
        vncs_i32(e, VNCS_STYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
        vncs_ptr(e, false);                     /* pNext */
        vncs_handle(e, pool);
        vncs_u32(e, count);
        vncs_array_size(e, count);
        vncs_handle_array(e, layouts, count);
    }
    vncs_array_size(e, count);                  /* pDescriptorSets */
    vncs_handle_array(e, out_sets, count);
}

static inline int32_t vncr_handle_array_reply(vncs_decoder_t *d,
                                              uint32_t expected_type,
                                              uint64_t *handles,
                                              uint32_t count)
{
    if (vncr_u32(d) != expected_type) {
        d->fatal = true;
        return -100;
    }
    int32_t ret = vncr_i32(d);
    if (vncs_peek_u64(d)) {
        uint64_t n = vncr_array_size(d, count);
        for (uint64_t i = 0; i < n; i++) {
            uint64_t id = vncr_handle(d);
            if (i < count) {
                handles[i] = id;
            }
        }
    } else {
        vncr_array_size_any(d);
    }
    return ret;
}

/* ---- vkUpdateDescriptorSets (id 79, no reply): storage buffers only ---- */

typedef struct vncs_desc_buffer_write {
    uint64_t dst_set;
    uint32_t dst_binding;
    uint64_t buffer;
    uint64_t offset;
    uint64_t range;
} vncs_desc_buffer_write_t;

static inline void vncs_vkUpdateDescriptorSets(
    vncs_encoder_t *e, uint32_t flags, uint64_t device,
    const vncs_desc_buffer_write_t *writes, uint32_t write_count)
{
    vncs_cmd_header(e, VNCS_CMD_vkUpdateDescriptorSets_EXT, flags);
    vncs_handle(e, device);
    vncs_u32(e, write_count);
    vncs_array_size(e, write_count);            /* pDescriptorWrites */
    for (uint32_t i = 0; i < write_count; i++) {
        vncs_i32(e, VNCS_STYPE_WRITE_DESCRIPTOR_SET);
        vncs_ptr(e, false);                     /* pNext */
        vncs_handle(e, writes[i].dst_set);
        vncs_u32(e, writes[i].dst_binding);
        vncs_u32(e, 0);                         /* dstArrayElement */
        vncs_u32(e, 1);                         /* descriptorCount */
        vncs_i32(e, VNCS_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        vncs_array_size(e, 0);                  /* pImageInfo */
        vncs_array_size(e, 1);                  /* pBufferInfo */
        vncs_handle(e, writes[i].buffer);
        vncs_u64(e, writes[i].offset);
        vncs_u64(e, writes[i].range);
        vncs_array_size(e, 0);                  /* pTexelBufferView */
    }
    vncs_u32(e, 0);                             /* descriptorCopyCount */
    vncs_array_size(e, 0);                      /* pDescriptorCopies */
}

/* ---- vkCreateCommandPool (id 85) ---- */

static inline void vncs_vkCreateCommandPool(vncs_encoder_t *e, uint32_t flags,
                                            uint64_t device,
                                            uint32_t queue_family_index,
                                            uint64_t out_pool)
{
    vncs_cmd_header(e, VNCS_CMD_vkCreateCommandPool_EXT, flags);
    vncs_handle(e, device);
    if (vncs_ptr(e, true)) {                    /* pCreateInfo */
        vncs_i32(e, VNCS_STYPE_COMMAND_POOL_CREATE_INFO);
        vncs_ptr(e, false);                     /* pNext */
        vncs_u32(e, 0);                         /* flags */
        vncs_u32(e, queue_family_index);
    }
    vncs_ptr(e, false);                         /* pAllocator */
    if (vncs_ptr(e, true)) {
        vncs_handle(e, out_pool);
    }
}

static inline int32_t vncr_vkCreateCommandPool_reply(vncs_decoder_t *d,
                                                     uint64_t *out_pool)
{
    if (vncr_u32(d) != VNCS_CMD_vkCreateCommandPool_EXT) {
        d->fatal = true;
        return -100;
    }
    int32_t ret = vncr_i32(d);
    if (vncr_ptr(d)) {
        *out_pool = vncr_handle(d);
    }
    return ret;
}

/* ---- vkAllocateCommandBuffers (id 88) ---- */

static inline void vncs_vkAllocateCommandBuffers(vncs_encoder_t *e,
                                                 uint32_t flags,
                                                 uint64_t device,
                                                 uint64_t pool,
                                                 uint64_t *out_buffers,
                                                 uint32_t count)
{
    vncs_cmd_header(e, VNCS_CMD_vkAllocateCommandBuffers_EXT, flags);
    vncs_handle(e, device);
    if (vncs_ptr(e, true)) {                    /* pAllocateInfo */
        vncs_i32(e, VNCS_STYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        vncs_ptr(e, false);                     /* pNext */
        vncs_handle(e, pool);
        vncs_i32(e, VNCS_COMMAND_BUFFER_LEVEL_PRIMARY);
        vncs_u32(e, count);
    }
    vncs_array_size(e, count);                  /* pCommandBuffers */
    vncs_handle_array(e, out_buffers, count);
}

/* ---- vkBeginCommandBuffer (id 90) / vkEndCommandBuffer (id 91) ---- */

static inline void vncs_vkBeginCommandBuffer(vncs_encoder_t *e, uint32_t flags,
                                             uint64_t cmdbuf)
{
    vncs_cmd_header(e, VNCS_CMD_vkBeginCommandBuffer_EXT, flags);
    vncs_handle(e, cmdbuf);
    if (vncs_ptr(e, true)) {                    /* pBeginInfo */
        vncs_i32(e, VNCS_STYPE_COMMAND_BUFFER_BEGIN_INFO);
        vncs_ptr(e, false);                     /* pNext */
        vncs_u32(e, 0);                         /* flags */
        vncs_ptr(e, false);                     /* pInheritanceInfo */
    }
}

static inline void vncs_vkEndCommandBuffer(vncs_encoder_t *e, uint32_t flags,
                                           uint64_t cmdbuf)
{
    vncs_cmd_header(e, VNCS_CMD_vkEndCommandBuffer_EXT, flags);
    vncs_handle(e, cmdbuf);
}

/* ---- vkCmdBindPipeline (id 93) ---- */

static inline void vncs_vkCmdBindPipeline(vncs_encoder_t *e, uint32_t flags,
                                          uint64_t cmdbuf, uint32_t bind_point,
                                          uint64_t pipeline)
{
    vncs_cmd_header(e, VNCS_CMD_vkCmdBindPipeline_EXT, flags);
    vncs_handle(e, cmdbuf);
    vncs_i32(e, (int32_t)bind_point);
    vncs_handle(e, pipeline);
}

/* ---- vkCmdBindDescriptorSets (id 103) ---- */

static inline void vncs_vkCmdBindDescriptorSets(vncs_encoder_t *e,
                                                uint32_t flags,
                                                uint64_t cmdbuf,
                                                uint32_t bind_point,
                                                uint64_t layout,
                                                uint32_t first_set,
                                                const uint64_t *sets,
                                                uint32_t set_count)
{
    vncs_cmd_header(e, VNCS_CMD_vkCmdBindDescriptorSets_EXT, flags);
    vncs_handle(e, cmdbuf);
    vncs_i32(e, (int32_t)bind_point);
    vncs_handle(e, layout);
    vncs_u32(e, first_set);
    vncs_u32(e, set_count);
    vncs_array_size(e, set_count);
    vncs_handle_array(e, sets, set_count);
    vncs_u32(e, 0);                             /* dynamicOffsetCount */
    vncs_array_size(e, 0);                      /* pDynamicOffsets */
}

/* ---- vkCmdPushConstants (id 132) ---- */

static inline void vncs_vkCmdPushConstants(vncs_encoder_t *e, uint32_t flags,
                                           uint64_t cmdbuf, uint64_t layout,
                                           uint32_t stage_flags,
                                           uint32_t offset, const void *values,
                                           uint32_t size)
{
    vncs_cmd_header(e, VNCS_CMD_vkCmdPushConstants_EXT, flags);
    vncs_handle(e, cmdbuf);
    vncs_handle(e, layout);
    vncs_u32(e, stage_flags);
    vncs_u32(e, offset);
    vncs_u32(e, size);
    vncs_array_size(e, size);
    vncs_blob(e, values, size);
}

/* ---- vkCmdDispatch (id 110) ---- */

static inline void vncs_vkCmdDispatch(vncs_encoder_t *e, uint32_t flags,
                                      uint64_t cmdbuf, uint32_t x, uint32_t y,
                                      uint32_t z)
{
    vncs_cmd_header(e, VNCS_CMD_vkCmdDispatch_EXT, flags);
    vncs_handle(e, cmdbuf);
    vncs_u32(e, x);
    vncs_u32(e, y);
    vncs_u32(e, z);
}

/* ---- vkQueueSubmit (id 18) ---- */

static inline void vncs_vkQueueSubmit(vncs_encoder_t *e, uint32_t flags,
                                      uint64_t queue, const uint64_t *cmdbufs,
                                      uint32_t cmdbuf_count, uint64_t fence)
{
    vncs_cmd_header(e, VNCS_CMD_vkQueueSubmit_EXT, flags);
    vncs_handle(e, queue);
    vncs_u32(e, 1);                             /* submitCount */
    vncs_array_size(e, 1);                      /* pSubmits */
    {
        vncs_i32(e, VNCS_STYPE_SUBMIT_INFO);
        vncs_ptr(e, false);                     /* pNext */
        vncs_u32(e, 0);                         /* waitSemaphoreCount */
        vncs_array_size(e, 0);                  /* pWaitSemaphores */
        vncs_array_size(e, 0);                  /* pWaitDstStageMask */
        vncs_u32(e, cmdbuf_count);
        vncs_array_size(e, cmdbuf_count);
        vncs_handle_array(e, cmdbufs, cmdbuf_count);
        vncs_u32(e, 0);                         /* signalSemaphoreCount */
        vncs_array_size(e, 0);                  /* pSignalSemaphores */
    }
    vncs_handle(e, fence);
}

/* ---- fences ---- */

static inline void vncs_vkCreateFence(vncs_encoder_t *e, uint32_t flags,
                                      uint64_t device, bool signaled,
                                      uint64_t out_fence)
{
    vncs_cmd_header(e, VNCS_CMD_vkCreateFence_EXT, flags);
    vncs_handle(e, device);
    if (vncs_ptr(e, true)) {                    /* pCreateInfo */
        vncs_i32(e, VNCS_STYPE_FENCE_CREATE_INFO);
        vncs_ptr(e, false);                     /* pNext */
        vncs_u32(e, signaled ? 1 : 0);          /* FENCE_CREATE_SIGNALED */
    }
    vncs_ptr(e, false);                         /* pAllocator */
    if (vncs_ptr(e, true)) {
        vncs_handle(e, out_fence);
    }
}

static inline int32_t vncr_vkCreateFence_reply(vncs_decoder_t *d,
                                               uint64_t *out_fence)
{
    if (vncr_u32(d) != VNCS_CMD_vkCreateFence_EXT) {
        d->fatal = true;
        return -100;
    }
    int32_t ret = vncr_i32(d);
    if (vncr_ptr(d)) {
        *out_fence = vncr_handle(d);
    }
    return ret;
}

static inline void vncs_vkResetFences(vncs_encoder_t *e, uint32_t flags,
                                      uint64_t device, const uint64_t *fences,
                                      uint32_t count)
{
    vncs_cmd_header(e, VNCS_CMD_vkResetFences_EXT, flags);
    vncs_handle(e, device);
    vncs_u32(e, count);
    vncs_array_size(e, count);
    vncs_handle_array(e, fences, count);
}

static inline void vncs_vkGetFenceStatus(vncs_encoder_t *e, uint32_t flags,
                                         uint64_t device, uint64_t fence)
{
    vncs_cmd_header(e, VNCS_CMD_vkGetFenceStatus_EXT, flags);
    vncs_handle(e, device);
    vncs_handle(e, fence);
}

static inline void vncs_vkWaitForFences(vncs_encoder_t *e, uint32_t flags,
                                        uint64_t device, const uint64_t *fences,
                                        uint32_t count, bool wait_all,
                                        uint64_t timeout_ns)
{
    vncs_cmd_header(e, VNCS_CMD_vkWaitForFences_EXT, flags);
    vncs_handle(e, device);
    vncs_u32(e, count);
    vncs_array_size(e, count);
    vncs_handle_array(e, fences, count);
    vncs_u32(e, wait_all ? 1 : 0);
    vncs_u64(e, timeout_ns);
}

/* ============================================================================
 * VK_MESA transport commands (submitted via VIRTIO_GPU_CMD_SUBMIT_3D, not
 * through the ring they manage)
 * ============================================================================ */

/* vkCreateRingMESA (id 188). Offsets follow Mesa vn_ring_get_layout:
 * head@0 / tail@64 / status@128 / buffer@192, extra after the buffer. */
static inline void vncs_vkCreateRingMESA(vncs_encoder_t *e, uint32_t flags,
                                         uint64_t ring_id,
                                         uint32_t resource_id,
                                         uint64_t offset, uint64_t size,
                                         uint64_t idle_timeout_ns,
                                         uint64_t head_offset,
                                         uint64_t tail_offset,
                                         uint64_t status_offset,
                                         uint64_t buffer_offset,
                                         uint64_t buffer_size,
                                         uint64_t extra_offset,
                                         uint64_t extra_size)
{
    vncs_cmd_header(e, VNCS_CMD_vkCreateRingMESA_EXT, flags);
    vncs_u64(e, ring_id);
    if (vncs_ptr(e, true)) {                    /* pCreateInfo */
        vncs_i32(e, VNCS_STYPE_RING_CREATE_INFO_MESA);
        vncs_ptr(e, false);                     /* pNext (no monitor/prio) */
        vncs_u32(e, 0);                         /* flags */
        vncs_u32(e, resource_id);
        vncs_u64(e, offset);
        vncs_u64(e, size);
        vncs_u64(e, idle_timeout_ns);
        vncs_u64(e, head_offset);
        vncs_u64(e, tail_offset);
        vncs_u64(e, status_offset);
        vncs_u64(e, buffer_offset);
        vncs_u64(e, buffer_size);
        vncs_u64(e, extra_offset);
        vncs_u64(e, extra_size);
    }
}

/* vkDestroyRingMESA (id 189) */
static inline void vncs_vkDestroyRingMESA(vncs_encoder_t *e, uint32_t flags,
                                          uint64_t ring_id)
{
    vncs_cmd_header(e, VNCS_CMD_vkDestroyRingMESA_EXT, flags);
    vncs_u64(e, ring_id);
}

/* vkNotifyRingMESA (id 190): wake an idle renderer after a ring write */
static inline void vncs_vkNotifyRingMESA(vncs_encoder_t *e, uint32_t flags,
                                         uint64_t ring_id, uint32_t seqno,
                                         uint32_t notify_flags)
{
    vncs_cmd_header(e, VNCS_CMD_vkNotifyRingMESA_EXT, flags);
    vncs_u64(e, ring_id);
    vncs_u32(e, seqno);
    vncs_u32(e, notify_flags);
}

/* vkSetReplyCommandStreamMESA (id 178): point the reply stream at `size`
 * bytes at `offset` inside shmem resource `resource_id` */
static inline void vncs_vkSetReplyCommandStreamMESA(vncs_encoder_t *e,
                                                    uint32_t flags,
                                                    uint32_t resource_id,
                                                    uint64_t offset,
                                                    uint64_t size)
{
    vncs_cmd_header(e, VNCS_CMD_vkSetReplyCommandStreamMESA_EXT, flags);
    if (vncs_ptr(e, true)) {                    /* pStream */
        vncs_u32(e, resource_id);
        vncs_u64(e, offset);
        vncs_u64(e, size);
    }
}

/* ============================================================================
 * Command table — used by host_test_venus.c for the structural check
 * (known opcodes + expected order) and by the walker below.
 * ============================================================================ */

typedef struct vncs_cmd_info {
    uint32_t type;
    const char *name;
    bool has_result;    /* reply record carries a VkResult word */
} vncs_cmd_info_t;

#define VNCS_CMD_TABLE                                                     \
    {                                                                      \
        { VNCS_CMD_vkCreateInstance_EXT, "vkCreateInstance", true },       \
        { VNCS_CMD_vkDestroyInstance_EXT, "vkDestroyInstance", false },    \
        { VNCS_CMD_vkEnumeratePhysicalDevices_EXT,                         \
          "vkEnumeratePhysicalDevices", true },                            \
        { VNCS_CMD_vkGetPhysicalDeviceProperties_EXT,                      \
          "vkGetPhysicalDeviceProperties", false },                        \
        { VNCS_CMD_vkGetPhysicalDeviceQueueFamilyProperties_EXT,           \
          "vkGetPhysicalDeviceQueueFamilyProperties", false },             \
        { VNCS_CMD_vkGetPhysicalDeviceMemoryProperties_EXT,                \
          "vkGetPhysicalDeviceMemoryProperties", false },                  \
        { VNCS_CMD_vkCreateDevice_EXT, "vkCreateDevice", true },           \
        { VNCS_CMD_vkGetDeviceQueue_EXT, "vkGetDeviceQueue", false },      \
        { VNCS_CMD_vkQueueSubmit_EXT, "vkQueueSubmit", true },             \
        { VNCS_CMD_vkAllocateMemory_EXT, "vkAllocateMemory", true },       \
        { VNCS_CMD_vkFreeMemory_EXT, "vkFreeMemory", false },              \
        { VNCS_CMD_vkBindBufferMemory_EXT, "vkBindBufferMemory", true },   \
        { VNCS_CMD_vkGetBufferMemoryRequirements_EXT,                      \
          "vkGetBufferMemoryRequirements", false },                        \
        { VNCS_CMD_vkCreateFence_EXT, "vkCreateFence", true },             \
        { VNCS_CMD_vkResetFences_EXT, "vkResetFences", true },             \
        { VNCS_CMD_vkGetFenceStatus_EXT, "vkGetFenceStatus", true },       \
        { VNCS_CMD_vkWaitForFences_EXT, "vkWaitForFences", true },         \
        { VNCS_CMD_vkCreateBuffer_EXT, "vkCreateBuffer", true },           \
        { VNCS_CMD_vkDestroyBuffer_EXT, "vkDestroyBuffer", false },        \
        { VNCS_CMD_vkCreateShaderModule_EXT, "vkCreateShaderModule",       \
          true },                                                          \
        { VNCS_CMD_vkCreateComputePipelines_EXT,                           \
          "vkCreateComputePipelines", true },                              \
        { VNCS_CMD_vkCreatePipelineLayout_EXT, "vkCreatePipelineLayout",   \
          true },                                                          \
        { VNCS_CMD_vkCreateDescriptorSetLayout_EXT,                        \
          "vkCreateDescriptorSetLayout", true },                           \
        { VNCS_CMD_vkCreateDescriptorPool_EXT, "vkCreateDescriptorPool",   \
          true },                                                          \
        { VNCS_CMD_vkAllocateDescriptorSets_EXT,                           \
          "vkAllocateDescriptorSets", true },                              \
        { VNCS_CMD_vkUpdateDescriptorSets_EXT, "vkUpdateDescriptorSets",   \
          false },                                                         \
        { VNCS_CMD_vkCreateCommandPool_EXT, "vkCreateCommandPool", true }, \
        { VNCS_CMD_vkAllocateCommandBuffers_EXT,                           \
          "vkAllocateCommandBuffers", true },                              \
        { VNCS_CMD_vkBeginCommandBuffer_EXT, "vkBeginCommandBuffer",       \
          true },                                                          \
        { VNCS_CMD_vkEndCommandBuffer_EXT, "vkEndCommandBuffer", true },   \
        { VNCS_CMD_vkCmdBindPipeline_EXT, "vkCmdBindPipeline", false },    \
        { VNCS_CMD_vkCmdBindDescriptorSets_EXT,                            \
          "vkCmdBindDescriptorSets", false },                              \
        { VNCS_CMD_vkCmdDispatch_EXT, "vkCmdDispatch", false },            \
        { VNCS_CMD_vkCmdPushConstants_EXT, "vkCmdPushConstants", false },  \
        { VNCS_CMD_vkSetReplyCommandStreamMESA_EXT,                        \
          "vkSetReplyCommandStreamMESA", false },                          \
        { VNCS_CMD_vkCreateRingMESA_EXT, "vkCreateRingMESA", false },      \
        { VNCS_CMD_vkDestroyRingMESA_EXT, "vkDestroyRingMESA", false },    \
        { VNCS_CMD_vkNotifyRingMESA_EXT, "vkNotifyRingMESA", false },      \
        { VNCS_CMD_vkGetMemoryResourcePropertiesMESA_EXT,                  \
          "vkGetMemoryResourcePropertiesMESA", true },                     \
    }

static const vncs_cmd_info_t vncs_command_table[] = VNCS_CMD_TABLE;
#define VNCS_COMMAND_TABLE_COUNT \
    (sizeof(vncs_command_table) / sizeof(vncs_command_table[0]))

static inline const vncs_cmd_info_t *vncs_cmd_lookup(uint32_t type)
{
    for (size_t i = 0; i < VNCS_COMMAND_TABLE_COUNT; i++) {
        if (vncs_command_table[i].type == type) {
            return &vncs_command_table[i];
        }
    }
    return NULL;
}

/* ============================================================================
 * Stream walker (structural decode).
 *
 * vncs_walk_next() advances `d` past one command record emitted by the
 * encoders above, validating every structural word it understands
 * (presence bits, array sizes, sTypes). Returns the command info, or NULL
 * on an unknown opcode / malformed record (d->fatal set).
 *
 * It parses the general shapes our encoders produce: dynamic counts are
 * read from the stream, so it is not tied to one fixed bring-up sequence.
 * ============================================================================ */

/* helpers for the walker */
static inline bool vncs_w_ptr(vncs_decoder_t *d)
{
    uint64_t v = vncr_u64(d);
    if (v > 1) {
        d->fatal = true;
    }
    return v != 0;
}

static inline uint64_t vncs_w_arr(vncs_decoder_t *d)
{
    return vncr_u64(d);
}

static inline void vncs_w_string(vncs_decoder_t *d)
{
    uint64_t n = vncs_w_arr(d);
    vncs_skip(d, (n + 3) & ~3ULL);
}

static inline void vncs_w_handles(vncs_decoder_t *d, uint64_t n)
{
    vncs_skip(d, n * 8);
}

static inline const vncs_cmd_info_t *vncs_walk_next(vncs_decoder_t *d)
{
    if (d->fatal || (size_t)(d->end - d->cur) < 8) {
        d->fatal = true;
        return NULL;
    }
    uint32_t type = vncr_u32(d);
    uint32_t flags = vncr_u32(d);
    (void)flags;
    const vncs_cmd_info_t *info = vncs_cmd_lookup(type);
    if (!info) {
        d->fatal = true;
        return NULL;
    }

    switch (type) {
    case VNCS_CMD_vkCreateInstance_EXT:
        if (vncs_w_ptr(d)) {                    /* pCreateInfo */
            if (vncr_i32(d) != VNCS_STYPE_INSTANCE_CREATE_INFO) d->fatal = true;
            vncs_w_ptr(d);                      /* pNext */
            vncs_skip(d, 4);                    /* flags */
            if (vncs_w_ptr(d)) {                /* pApplicationInfo */
                if (vncr_i32(d) != VNCS_STYPE_APPLICATION_INFO) d->fatal = true;
                vncs_w_ptr(d);
                vncs_w_string(d);
                vncs_skip(d, 4);
                vncs_w_string(d);
                vncs_skip(d, 8);
            }
            vncs_skip(d, 4);                    /* enabledLayerCount */
            vncs_skip(d, vncs_w_arr(d) * 0);    /* layer names (0) */
            vncs_skip(d, 4);                    /* enabledExtensionCount */
            vncs_w_arr(d);
        }
        vncs_w_ptr(d);                          /* pAllocator */
        if (vncs_w_ptr(d)) vncs_skip(d, 8);     /* pInstance */
        break;

    case VNCS_CMD_vkDestroyInstance_EXT:
    case VNCS_CMD_vkFreeMemory_EXT:
    case VNCS_CMD_vkDestroyBuffer_EXT:
        vncs_skip(d, 8);                        /* parent handle */
        if (type != VNCS_CMD_vkDestroyInstance_EXT) vncs_skip(d, 8);
        vncs_w_ptr(d);                          /* pAllocator */
        break;

    case VNCS_CMD_vkEnumeratePhysicalDevices_EXT:
        vncs_skip(d, 8);                        /* instance */
        if (vncs_w_ptr(d)) vncs_skip(d, 4);     /* count */
        vncs_w_handles(d, vncs_w_arr(d));       /* devices */
        break;

    case VNCS_CMD_vkGetPhysicalDeviceProperties_EXT:
        vncs_skip(d, 8);
        vncs_w_ptr(d);
        break;

    case VNCS_CMD_vkGetPhysicalDeviceQueueFamilyProperties_EXT:
        vncs_skip(d, 8);
        if (vncs_w_ptr(d)) vncs_skip(d, 4);
        vncs_w_arr(d);                          /* partial: array_size only */
        break;

    case VNCS_CMD_vkGetPhysicalDeviceMemoryProperties_EXT:
        vncs_skip(d, 8);
        vncs_w_ptr(d);
        vncs_w_arr(d);                          /* 32 */
        vncs_w_arr(d);                          /* 16 */
        break;

    case VNCS_CMD_vkCreateDevice_EXT:
        vncs_skip(d, 8);                        /* physicalDevice */
        if (vncs_w_ptr(d)) {                    /* pCreateInfo */
            if (vncr_i32(d) != VNCS_STYPE_DEVICE_CREATE_INFO) d->fatal = true;
            vncs_w_ptr(d);                      /* pNext */
            vncs_skip(d, 4);                    /* flags */
            uint64_t qn = vncr_u32(d);
            if (vncs_w_arr(d) != qn) d->fatal = true;
            for (uint64_t i = 0; i < qn; i++) {
                if (vncr_i32(d) != VNCS_STYPE_DEVICE_QUEUE_CREATE_INFO)
                    d->fatal = true;
                vncs_w_ptr(d);                  /* pNext */
                vncs_skip(d, 12);               /* flags, family, count */
                uint64_t pn = vncs_w_arr(d);    /* priorities */
                vncs_skip(d, pn * 4);
            }
            vncs_skip(d, 4);                    /* enabledLayerCount */
            { uint64_t ln = vncs_w_arr(d);
              for (uint64_t i = 0; i < ln; i++) vncs_w_string(d); }
            vncs_skip(d, 4);                    /* enabledExtensionCount */
            { uint64_t en = vncs_w_arr(d);
              for (uint64_t i = 0; i < en; i++) vncs_w_string(d); }
            vncs_w_ptr(d);                      /* pEnabledFeatures */
        }
        vncs_w_ptr(d);                          /* pAllocator */
        if (vncs_w_ptr(d)) vncs_skip(d, 8);     /* pDevice */
        break;

    case VNCS_CMD_vkGetDeviceQueue_EXT:
        vncs_skip(d, 8 + 4 + 4);
        if (vncs_w_ptr(d)) vncs_skip(d, 8);
        break;

    case VNCS_CMD_vkCreateBuffer_EXT:
        vncs_skip(d, 8);
        if (vncs_w_ptr(d)) {
            if (vncr_i32(d) != VNCS_STYPE_BUFFER_CREATE_INFO) d->fatal = true;
            vncs_w_ptr(d);
            vncs_skip(d, 4 + 8 + 4 + 4 + 4);    /* flags,size,usage,mode,cnt */
            { uint64_t qn = vncs_w_arr(d); vncs_skip(d, qn * 4); }
        }
        vncs_w_ptr(d);
        if (vncs_w_ptr(d)) vncs_skip(d, 8);
        break;

    case VNCS_CMD_vkGetBufferMemoryRequirements_EXT:
        vncs_skip(d, 8 + 8);
        vncs_w_ptr(d);
        break;

    case VNCS_CMD_vkAllocateMemory_EXT:
        vncs_skip(d, 8);
        if (vncs_w_ptr(d)) {
            if (vncr_i32(d) != VNCS_STYPE_MEMORY_ALLOCATE_INFO) d->fatal = true;
            if (vncs_w_ptr(d)) {                /* pNext chain */
                if (vncr_i32(d) != VNCS_STYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA)
                    d->fatal = true;
                vncs_w_ptr(d);
                vncs_skip(d, 4);                /* resourceId */
            }
            vncs_skip(d, 8 + 4);                /* allocationSize, typeIndex */
        }
        vncs_w_ptr(d);
        if (vncs_w_ptr(d)) vncs_skip(d, 8);
        break;

    case VNCS_CMD_vkBindBufferMemory_EXT:
        vncs_skip(d, 8 + 8 + 8 + 8);
        break;

    case VNCS_CMD_vkGetMemoryResourcePropertiesMESA_EXT:
        vncs_skip(d, 8 + 4);
        if (vncs_w_ptr(d)) {
            if (vncr_i32(d) != VNCS_STYPE_MEMORY_RESOURCE_PROPERTIES_MESA)
                d->fatal = true;
            vncs_w_ptr(d);
        }
        break;

    case VNCS_CMD_vkCreateShaderModule_EXT:
        vncs_skip(d, 8);
        if (vncs_w_ptr(d)) {
            if (vncr_i32(d) != VNCS_STYPE_SHADER_MODULE_CREATE_INFO)
                d->fatal = true;
            vncs_w_ptr(d);
            vncs_skip(d, 4 + 8);                /* flags, codeSize */
            uint64_t words = vncs_w_arr(d);
            vncs_skip(d, words * 4);
        }
        vncs_w_ptr(d);
        if (vncs_w_ptr(d)) vncs_skip(d, 8);
        break;

    case VNCS_CMD_vkCreateDescriptorSetLayout_EXT:
        vncs_skip(d, 8);
        if (vncs_w_ptr(d)) {
            if (vncr_i32(d) != VNCS_STYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO)
                d->fatal = true;
            vncs_w_ptr(d);
            vncs_skip(d, 4);                    /* flags */
            uint64_t bn = vncr_u32(d);
            if (vncs_w_arr(d) != bn) d->fatal = true;
            for (uint64_t i = 0; i < bn; i++) {
                vncs_skip(d, 4 + 4 + 4 + 4);
                vncs_w_arr(d);                  /* immutable samplers */
            }
        }
        vncs_w_ptr(d);
        if (vncs_w_ptr(d)) vncs_skip(d, 8);
        break;

    case VNCS_CMD_vkCreatePipelineLayout_EXT:
        vncs_skip(d, 8);
        if (vncs_w_ptr(d)) {
            if (vncr_i32(d) != VNCS_STYPE_PIPELINE_LAYOUT_CREATE_INFO)
                d->fatal = true;
            vncs_w_ptr(d);
            vncs_skip(d, 4);                    /* flags */
            uint64_t sn = vncr_u32(d);
            if (vncs_w_arr(d) != sn) d->fatal = true;
            vncs_w_handles(d, sn);
            uint64_t pn = vncr_u32(d);
            if (vncs_w_arr(d) != pn) d->fatal = true;
            vncs_skip(d, pn * 12);
        }
        vncs_w_ptr(d);
        if (vncs_w_ptr(d)) vncs_skip(d, 8);
        break;

    case VNCS_CMD_vkCreateComputePipelines_EXT:
        vncs_skip(d, 8 + 8);                    /* device, pipelineCache */
        {
            uint64_t cn = vncr_u32(d);
            if (vncs_w_arr(d) != cn) d->fatal = true;
            for (uint64_t i = 0; i < cn; i++) {
                if (vncr_i32(d) != VNCS_STYPE_COMPUTE_PIPELINE_CREATE_INFO)
                    d->fatal = true;
                vncs_w_ptr(d);
                vncs_skip(d, 4);                /* flags */
                if (vncr_i32(d) != VNCS_STYPE_PIPELINE_SHADER_STAGE_CREATE_INFO)
                    d->fatal = true;
                vncs_w_ptr(d);                  /* stage pNext */
                vncs_skip(d, 4 + 4);            /* flags, stage */
                vncs_skip(d, 8);                /* module */
                vncs_w_string(d);               /* pName */
                vncs_w_ptr(d);                  /* pSpecializationInfo */
                vncs_skip(d, 8 + 8 + 4);        /* layout, base handle, idx */
            }
            vncs_w_ptr(d);                      /* pAllocator */
            if (vncs_w_arr(d) != cn) d->fatal = true;
            vncs_w_handles(d, cn);
        }
        break;

    case VNCS_CMD_vkCreateDescriptorPool_EXT:
        vncs_skip(d, 8);
        if (vncs_w_ptr(d)) {
            if (vncr_i32(d) != VNCS_STYPE_DESCRIPTOR_POOL_CREATE_INFO)
                d->fatal = true;
            vncs_w_ptr(d);
            vncs_skip(d, 4 + 4);                /* flags, maxSets */
            uint64_t pn = vncr_u32(d);
            if (vncs_w_arr(d) != pn) d->fatal = true;
            vncs_skip(d, pn * 8);
        }
        vncs_w_ptr(d);
        if (vncs_w_ptr(d)) vncs_skip(d, 8);
        break;

    case VNCS_CMD_vkAllocateDescriptorSets_EXT:
        vncs_skip(d, 8);
        if (vncs_w_ptr(d)) {
            if (vncr_i32(d) != VNCS_STYPE_DESCRIPTOR_SET_ALLOCATE_INFO)
                d->fatal = true;
            vncs_w_ptr(d);
            vncs_skip(d, 8);                    /* descriptorPool */
            uint64_t sn = vncr_u32(d);
            if (vncs_w_arr(d) != sn) d->fatal = true;
            vncs_w_handles(d, sn);
            if (vncs_w_arr(d) != sn) d->fatal = true;
            vncs_w_handles(d, sn);
        } else {
            vncs_w_handles(d, vncs_w_arr(d));
        }
        break;

    case VNCS_CMD_vkUpdateDescriptorSets_EXT:
        vncs_skip(d, 8);
        {
            uint64_t wn = vncr_u32(d);
            if (vncs_w_arr(d) != wn) d->fatal = true;
            for (uint64_t i = 0; i < wn; i++) {
                if (vncr_i32(d) != VNCS_STYPE_WRITE_DESCRIPTOR_SET)
                    d->fatal = true;
                vncs_w_ptr(d);
                vncs_skip(d, 8 + 4 + 4);        /* dstSet, binding, elem */
                uint64_t dn = vncr_u32(d);      /* descriptorCount */
                vncs_skip(d, 4);                /* descriptorType */
                vncs_skip(d, vncs_w_arr(d) * 24);  /* VkDescriptorImageInfo */
                uint64_t bi = vncs_w_arr(d);
                vncs_skip(d, bi * 24);          /* VkDescriptorBufferInfo */
                vncs_w_handles(d, vncs_w_arr(d));  /* pTexelBufferView */
                (void)dn;
            }
            vncs_skip(d, 4);                    /* descriptorCopyCount */
            vncs_w_arr(d);
        }
        break;

    case VNCS_CMD_vkCreateCommandPool_EXT:
        vncs_skip(d, 8);
        if (vncs_w_ptr(d)) {
            if (vncr_i32(d) != VNCS_STYPE_COMMAND_POOL_CREATE_INFO)
                d->fatal = true;
            vncs_w_ptr(d);
            vncs_skip(d, 8);                    /* flags, queueFamilyIndex */
        }
        vncs_w_ptr(d);
        if (vncs_w_ptr(d)) vncs_skip(d, 8);
        break;

    case VNCS_CMD_vkAllocateCommandBuffers_EXT:
        vncs_skip(d, 8);
        if (vncs_w_ptr(d)) {
            if (vncr_i32(d) != VNCS_STYPE_COMMAND_BUFFER_ALLOCATE_INFO)
                d->fatal = true;
            vncs_w_ptr(d);
            vncs_skip(d, 8 + 4);                /* pool, level */
            uint64_t cn = vncr_u32(d);
            if (vncs_w_arr(d) != cn) d->fatal = true;
            vncs_w_handles(d, cn);
        } else {
            vncs_w_handles(d, vncs_w_arr(d));
        }
        break;

    case VNCS_CMD_vkBeginCommandBuffer_EXT:
        vncs_skip(d, 8);
        if (vncs_w_ptr(d)) {
            if (vncr_i32(d) != VNCS_STYPE_COMMAND_BUFFER_BEGIN_INFO)
                d->fatal = true;
            vncs_w_ptr(d);
            vncs_skip(d, 4);
            vncs_w_ptr(d);                      /* pInheritanceInfo */
        }
        break;

    case VNCS_CMD_vkEndCommandBuffer_EXT:
        vncs_skip(d, 8);
        break;

    case VNCS_CMD_vkCmdBindPipeline_EXT:
        vncs_skip(d, 8 + 4 + 8);
        break;

    case VNCS_CMD_vkCmdBindDescriptorSets_EXT:
        vncs_skip(d, 8 + 4 + 8 + 4);
        {
            uint64_t sn = vncr_u32(d);
            if (vncs_w_arr(d) != sn) d->fatal = true;
            vncs_w_handles(d, sn);
            uint64_t dn = vncr_u32(d);
            if (vncs_w_arr(d) != dn) d->fatal = true;
            vncs_skip(d, dn * 4);
        }
        break;

    case VNCS_CMD_vkCmdPushConstants_EXT:
        vncs_skip(d, 8 + 8 + 4 + 4);
        {
            uint64_t size = vncr_u32(d);
            if (vncs_w_arr(d) != size) d->fatal = true;
            vncs_skip(d, (size + 3) & ~3ULL);
        }
        break;

    case VNCS_CMD_vkCmdDispatch_EXT:
        vncs_skip(d, 8 + 12);
        break;

    case VNCS_CMD_vkQueueSubmit_EXT:
        vncs_skip(d, 8);                        /* queue */
        {
            uint64_t sn = vncr_u32(d);
            if (vncs_w_arr(d) != sn) d->fatal = true;
            for (uint64_t i = 0; i < sn; i++) {
                if (vncr_i32(d) != VNCS_STYPE_SUBMIT_INFO) d->fatal = true;
                vncs_w_ptr(d);
                uint64_t wc = vncr_u32(d);      /* waitSemaphoreCount */
                if (vncs_w_arr(d) != wc) d->fatal = true;
                vncs_w_handles(d, wc);
                if (vncs_w_arr(d) != wc) d->fatal = true;
                vncs_skip(d, wc * 4);           /* waitDstStageMask */
                uint64_t cc = vncr_u32(d);      /* commandBufferCount */
                if (vncs_w_arr(d) != cc) d->fatal = true;
                vncs_w_handles(d, cc);
                uint64_t sc = vncr_u32(d);      /* signalSemaphoreCount */
                if (vncs_w_arr(d) != sc) d->fatal = true;
                vncs_w_handles(d, sc);
            }
            vncs_skip(d, 8);                    /* fence */
        }
        break;

    case VNCS_CMD_vkCreateFence_EXT:
        vncs_skip(d, 8);
        if (vncs_w_ptr(d)) {
            if (vncr_i32(d) != VNCS_STYPE_FENCE_CREATE_INFO) d->fatal = true;
            vncs_w_ptr(d);
            vncs_skip(d, 4);
        }
        vncs_w_ptr(d);
        if (vncs_w_ptr(d)) vncs_skip(d, 8);
        break;

    case VNCS_CMD_vkResetFences_EXT:
        vncs_skip(d, 8);
        {
            uint64_t fn = vncr_u32(d);
            if (vncs_w_arr(d) != fn) d->fatal = true;
            vncs_w_handles(d, fn);
        }
        break;

    case VNCS_CMD_vkGetFenceStatus_EXT:
        vncs_skip(d, 8 + 8);
        break;

    case VNCS_CMD_vkWaitForFences_EXT:
        vncs_skip(d, 8);
        {
            uint64_t fn = vncr_u32(d);
            if (vncs_w_arr(d) != fn) d->fatal = true;
            vncs_w_handles(d, fn);
            vncs_skip(d, 4 + 8);                /* waitAll, timeout */
        }
        break;

    case VNCS_CMD_vkSetReplyCommandStreamMESA_EXT:
        if (vncs_w_ptr(d)) {
            vncs_skip(d, 4 + 8 + 8);            /* resourceId, offset, size */
        }
        break;

    case VNCS_CMD_vkCreateRingMESA_EXT:
        vncs_skip(d, 8);                        /* ring id */
        if (vncs_w_ptr(d)) {
            if (vncr_i32(d) != VNCS_STYPE_RING_CREATE_INFO_MESA)
                d->fatal = true;
            vncs_w_ptr(d);
            vncs_skip(d, 4 + 4 + 8 * 9);        /* flags, resourceId, 9x u64 */
        }
        break;

    case VNCS_CMD_vkDestroyRingMESA_EXT:
        vncs_skip(d, 8);
        break;

    case VNCS_CMD_vkNotifyRingMESA_EXT:
        vncs_skip(d, 8 + 4 + 4);
        break;

    default:
        d->fatal = true;
        return NULL;
    }

    return d->fatal ? NULL : info;
}

#endif /* EMBODIOS_VENUS_CS_H */
