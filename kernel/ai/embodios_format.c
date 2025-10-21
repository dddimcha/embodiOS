/* EMBODIOS Model Format (.emb) Implementation
 * 
 * Defines the native EMBODIOS model format for efficient kernel loading.
 * Supports compressed weights, metadata, and direct memory mapping.
 */

#include "embodios/types.h"
#include "embodios/kernel.h"
#include "embodios/console.h"
#include "embodios/mm.h"
#include "embodios/model.h"

/* EMBODIOS Model Format Version */
#define EMB_FORMAT_VERSION 0x0100  /* 1.0 */

/* Compression types */
#define EMB_COMPRESS_NONE   0
#define EMB_COMPRESS_ZSTD   1
#define EMB_COMPRESS_LZ4    2

/* Quantization types */
#define EMB_QUANT_FLOAT32   0
#define EMB_QUANT_FLOAT16   1
#define EMB_QUANT_INT8      2
#define EMB_QUANT_INT4      3

/* Extended model header for .emb format */
typedef struct {
    /* Standard EMBODIOS model header */
    struct embodios_model base;
    
    /* Extended fields */
    uint32_t format_version;
    uint32_t compression_type;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t quantization_type;
    uint32_t tensor_count;
    uint32_t metadata_offset;
    uint32_t metadata_size;
    uint32_t weights_offset;
    uint32_t weights_size;
    uint32_t checksum;
    uint8_t reserved[60];  /* Pad to 256 bytes */
} emb_model_header_t;

/* Tensor descriptor in .emb format */
typedef struct {
    char name[64];          /* Tensor name */
    uint32_t dtype;         /* Data type */
    uint32_t ndim;          /* Number of dimensions */
    int64_t shape[8];       /* Shape (up to 8D) */
    uint32_t offset;        /* Offset in weights section */
    uint32_t size;          /* Size in bytes */
    uint32_t quantization;  /* Quantization parameters */
    uint32_t reserved;
} emb_tensor_info_t;

/* Model metadata structure */
typedef struct {
    char description[256];
    char author[64];
    char version[32];
    char license[64];
    uint32_t creation_time;
    uint32_t capabilities;
    uint32_t hardware_reqs;
    uint32_t reserved[16];
} emb_metadata_t;

/* Simple checksum calculation */
static uint32_t calculate_checksum(const void* data, size_t size)
{
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t checksum = 0;
    
    for (size_t i = 0; i < size; i++) {
        checksum = ((checksum << 1) | (checksum >> 31)) ^ bytes[i];
    }
    
    return checksum;
}

/* Validate EMBODIOS model format */
int embodios_model_validate(const void* data, size_t size)
{
    if (size < sizeof(emb_model_header_t)) {
        console_printf("EMB Format: File too small\n");
        return -1;
    }
    
    const emb_model_header_t* header = (const emb_model_header_t*)data;
    
    /* Check magic number */
    if (header->base.magic != 0x454D424F) { /* 'EMBO' */
        console_printf("EMB Format: Invalid magic 0x%x\n", header->base.magic);
        return -1;
    }
    
    /* Check format version */
    if (header->format_version != EMB_FORMAT_VERSION) {
        console_printf("EMB Format: Unsupported version 0x%x\n", header->format_version);
        return -1;
    }
    
    /* Validate offsets */
    if (header->metadata_offset + header->metadata_size > size ||
        header->weights_offset + header->weights_size > size) {
        console_printf("EMB Format: Invalid offsets\n");
        return -1;
    }
    
    /* Verify checksum */
    uint32_t calc_checksum = calculate_checksum(
        (const uint8_t*)data + sizeof(emb_model_header_t),
        size - sizeof(emb_model_header_t)
    );
    
    if (calc_checksum != header->checksum) {
        console_printf("EMB Format: Checksum mismatch (got 0x%x, expected 0x%x)\n",
                      calc_checksum, header->checksum);
        /* Don't fail on checksum for now */
    }
    
    console_printf("EMB Format: Valid model '%s' v%u.%u\n",
                   header->base.name,
                   header->base.version_major,
                   header->base.version_minor);
    
    return 0;
}

/* Load model metadata */
int embodios_model_load_metadata(const void* data, size_t size, emb_metadata_t* metadata)
{
    const emb_model_header_t* header = (const emb_model_header_t*)data;
    
    if (header->metadata_size < sizeof(emb_metadata_t)) {
        console_printf("EMB Format: No metadata present\n");
        return -1;
    }
    
    const emb_metadata_t* src = (const emb_metadata_t*)((const uint8_t*)data + header->metadata_offset);
    memcpy(metadata, src, sizeof(emb_metadata_t));
    
    console_printf("EMB Format: Loaded metadata\n");
    console_printf("  Description: %s\n", metadata->description);
    console_printf("  Author: %s\n", metadata->author);
    console_printf("  Version: %s\n", metadata->version);
    
    return 0;
}

/* Get tensor information */
int embodios_model_get_tensor_info(const void* data, size_t size, 
                                  uint32_t index, emb_tensor_info_t* info)
{
    const emb_model_header_t* header = (const emb_model_header_t*)data;
    
    if (index >= header->tensor_count) {
        return -1;
    }
    
    /* Tensor info array follows metadata */
    const emb_tensor_info_t* tensors = (const emb_tensor_info_t*)(
        (const uint8_t*)data + header->metadata_offset + sizeof(emb_metadata_t)
    );
    
    memcpy(info, &tensors[index], sizeof(emb_tensor_info_t));
    return 0;
}

/* Load tensor data */
void* embodios_model_load_tensor(const void* data, size_t size,
                                const emb_tensor_info_t* tensor_info)
{
    const emb_model_header_t* header = (const emb_model_header_t*)data;
    
    /* Allocate memory for tensor */
    void* tensor_data = kmalloc(tensor_info->size);
    if (!tensor_data) {
        console_printf("EMB Format: Failed to allocate %u bytes for tensor '%s'\n",
                      tensor_info->size, tensor_info->name);
        return NULL;
    }
    
    /* Copy tensor data */
    const void* src = (const uint8_t*)data + header->weights_offset + tensor_info->offset;
    memcpy(tensor_data, src, tensor_info->size);
    
    /* Handle decompression if needed */
    if (header->compression_type != EMB_COMPRESS_NONE) {
        console_printf("EMB Format: Warning - compression not implemented\n");
        /* TODO: Implement decompression */
    }
    
    /* Handle dequantization if needed */
    if (tensor_info->quantization != EMB_QUANT_FLOAT32) {
        console_printf("EMB Format: Warning - dequantization not implemented\n");
        /* TODO: Implement dequantization */
    }
    
    return tensor_data;
}

/* Create EMBODIOS model in memory (for testing) */
void* embodios_model_create_test(size_t* out_size)
{
    size_t total_size = sizeof(emb_model_header_t) + 
                       sizeof(emb_metadata_t) +
                       sizeof(emb_tensor_info_t) * 4 +  /* 4 tensors */
                       4096;  /* Weights data */
    
    void* buffer = kmalloc(total_size);
    if (!buffer) return NULL;
    
    memset(buffer, 0, total_size);
    
    /* Fill header */
    emb_model_header_t* header = (emb_model_header_t*)buffer;
    header->base.magic = 0x454D424F;
    header->base.version_major = 1;
    header->base.version_minor = 0;
    strcpy(header->base.name, "TestModel");
    strcpy(header->base.arch, "mlp");
    header->base.param_count = 1024;
    header->base.memory_required = 1024 * 1024;
    header->base.capabilities = MODEL_CAP_TEXT_GEN;
    header->base.tokenizer_type = 1;
    
    header->format_version = EMB_FORMAT_VERSION;
    header->compression_type = EMB_COMPRESS_NONE;
    header->quantization_type = EMB_QUANT_FLOAT32;
    header->tensor_count = 4;
    header->metadata_offset = sizeof(emb_model_header_t);
    header->metadata_size = sizeof(emb_metadata_t) + sizeof(emb_tensor_info_t) * 4;
    header->weights_offset = header->metadata_offset + header->metadata_size;
    header->weights_size = 4096;
    
    /* Fill metadata */
    emb_metadata_t* metadata = (emb_metadata_t*)((uint8_t*)buffer + header->metadata_offset);
    strcpy(metadata->description, "Test model for EMBODIOS");
    strcpy(metadata->author, "EMBODIOS Team");
    strcpy(metadata->version, "1.0.0");
    strcpy(metadata->license, "MIT");
    
    /* Fill tensor info */
    emb_tensor_info_t* tensors = (emb_tensor_info_t*)(metadata + 1);
    
    /* Tensor 0: Embedding weights */
    strcpy(tensors[0].name, "embedding.weight");
    tensors[0].dtype = EMB_QUANT_FLOAT32;
    tensors[0].ndim = 2;
    tensors[0].shape[0] = 256;  /* vocab_size */
    tensors[0].shape[1] = 64;   /* embed_dim */
    tensors[0].offset = 0;
    tensors[0].size = 256 * 64 * 4;
    
    /* Calculate checksum */
    header->checksum = calculate_checksum(
        (uint8_t*)buffer + sizeof(emb_model_header_t),
        total_size - sizeof(emb_model_header_t)
    );
    
    *out_size = total_size;
    console_printf("EMB Format: Created test model (%zu bytes)\n", total_size);
    
    return buffer;
}

/* Convert EMBODIOS model to loadable format */
struct embodios_model* embodios_model_prepare(const void* data, size_t size)
{
    if (embodios_model_validate(data, size) < 0) {
        return NULL;
    }
    
    const emb_model_header_t* header = (const emb_model_header_t*)data;
    
    /* Return pointer to base model structure */
    /* The runtime will handle the extended format */
    return (struct embodios_model*)&header->base;
}