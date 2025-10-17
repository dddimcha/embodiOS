/* Extract weights from GGUF and convert to TVM format
 * This actually loads the model weights
 */

#include <embodios/types.h>
#include <embodios/kernel.h>
#include <embodios/console.h>
#include <embodios/mm.h>

/* External string functions */
extern size_t strlen(const char* s);
extern int memcmp(const void* s1, const void* s2, size_t n);
extern void* memcpy(void* dest, const void* src, size_t n);

/* GGUF structures */
struct gguf_header {
    uint32_t magic;
    uint32_t version;
    uint64_t n_tensors;
    uint64_t n_kv;
} __attribute__((packed));

struct gguf_string {
    uint64_t n;
    char data[];
} __attribute__((packed));

/* Parse GGUF and extract tensor data */
void* gguf_find_tensor(const uint8_t* data, size_t size, const char* name, size_t* out_size) {
    struct gguf_header* header = (struct gguf_header*)data;
    
    if (header->magic != 0x46554747) {
        console_printf("Invalid GGUF magic\n");
        return NULL;
    }
    
    /* Skip to tensor info after header and metadata */
    const uint8_t* ptr = data + sizeof(struct gguf_header);
    
    /* Skip metadata key-value pairs */
    for (uint64_t i = 0; i < header->n_kv; i++) {
        struct gguf_string* key = (struct gguf_string*)ptr;
        ptr += sizeof(uint64_t) + key->n;
        
        uint32_t vtype = *(uint32_t*)ptr;
        ptr += 4;
        
        /* Skip value based on type */
        switch (vtype) {
            case 4: ptr += 4; break;  /* uint32 */
            case 5: ptr += 4; break;  /* int32 */
            case 6: ptr += 4; break;  /* float32 */
            case 8: /* string */
                ptr += 8 + ((struct gguf_string*)ptr)->n;
                break;
            case 10: ptr += 8; break; /* uint64 */
            case 11: ptr += 8; break; /* int64 */
            default: ptr += 8; break;
        }
    }
    
    /* Align to 32 bytes for tensor info */
    ptr = (uint8_t*)(((uintptr_t)ptr + 31) & ~31);
    
    /* Now at tensor info section */
    for (uint64_t i = 0; i < header->n_tensors; i++) {
        struct gguf_string* tensor_name = (struct gguf_string*)ptr;
        ptr += sizeof(uint64_t) + tensor_name->n;
        
        uint32_t n_dims = *(uint32_t*)ptr;
        ptr += 4;
        
        uint64_t ne[4] = {1, 1, 1, 1};
        for (uint32_t j = 0; j < n_dims; j++) {
            ne[j] = *(uint64_t*)ptr;
            ptr += 8;
        }
        
        uint32_t type = *(uint32_t*)ptr;
        ptr += 4;
        
        uint64_t offset = *(uint64_t*)ptr;
        ptr += 8;
        
        /* Check if this is the tensor we want */
        if (tensor_name->n == strlen(name) && 
            memcmp(tensor_name->data, name, tensor_name->n) == 0) {
            
            console_printf("Found tensor '%.*s' - type=%d, offset=%llu\n", 
                          (int)tensor_name->n, tensor_name->data, type, offset);
            
            /* Find start of tensor data section */
            /* After all tensor infos, we need to find the aligned data section */
            const uint8_t* tensor_section_ptr = ptr;
            
            /* Skip remaining tensor infos */
            for (uint64_t j = i + 1; j < header->n_tensors; j++) {
                struct gguf_string* skip_name = (struct gguf_string*)tensor_section_ptr;
                tensor_section_ptr += sizeof(uint64_t) + skip_name->n;
                tensor_section_ptr += 4; /* n_dims */
                uint32_t skip_ndims = *(uint32_t*)(tensor_section_ptr - 4);
                tensor_section_ptr += skip_ndims * 8; /* dims */
                tensor_section_ptr += 4; /* type */
                tensor_section_ptr += 8; /* offset */
            }
            
            /* Align to find tensor data start */
            size_t alignment = 32;
            tensor_section_ptr = (uint8_t*)(((uintptr_t)tensor_section_ptr + alignment - 1) & ~(alignment - 1));
            
            /* Calculate actual tensor location */
            const uint8_t* tensor_data = tensor_section_ptr + offset;
            
            if (out_size) {
                /* Calculate size based on type and dimensions */
                uint64_t n_elements = ne[0] * ne[1] * ne[2] * ne[3];
                
                switch (type) {
                    case 0: *out_size = n_elements * 4; break;      /* f32 */
                    case 1: *out_size = n_elements * 2; break;      /* f16 */
                    case 12: *out_size = (n_elements / 256) * 144; break; /* q4_K */
                    default: *out_size = n_elements; break;
                }
            }
            
            return (void*)tensor_data;
        }
    }
    
    return NULL;
}

/* Q4_K quantization block structure */
typedef struct {
    uint16_t d;           /* delta */
    uint16_t dmin;        /* min */
    uint8_t scales[12];   /* scales */
    uint8_t qs[128];      /* quants */
} block_q4_k;

/* Dequantize Q4_K block to float */
void dequantize_q4_k(const uint8_t* src, float* dst, size_t n_blocks) {
    const block_q4_k* blocks = (const block_q4_k*)src;
    
    for (size_t i = 0; i < n_blocks; i++) {
        const block_q4_k* block = &blocks[i];
        float* out = dst + i * 256;
        
        /* Convert half to float */
        const float d = *(float*)&block->d;
        const float min = *(float*)&block->dmin;
        
        /* Dequantize 256 values (simplified for now) */
        for (int j = 0; j < 256; j++) {
            int q_idx = j / 2;
            int q_shift = (j & 1) * 4;
            uint8_t q = (block->qs[q_idx] >> q_shift) & 0xF;
            
            /* Apply scale */
            int scale_idx = j / 32;
            float scale = (block->scales[scale_idx / 2] >> ((scale_idx & 1) * 4)) & 0xF;
            
            out[j] = d * q * scale + min;
        }
    }
}

/* Get tensor type from GGUF */
int gguf_get_tensor_type(const uint8_t* data, size_t size, const char* name) {
    struct gguf_header* header = (struct gguf_header*)data;
    const uint8_t* ptr = data + sizeof(struct gguf_header);
    
    /* Skip metadata */
    for (uint64_t i = 0; i < header->n_kv; i++) {
        struct gguf_string* key = (struct gguf_string*)ptr;
        ptr += sizeof(uint64_t) + key->n;
        uint32_t vtype = *(uint32_t*)ptr;
        ptr += 4;
        
        switch (vtype) {
            case 4: ptr += 4; break;
            case 5: ptr += 4; break;
            case 6: ptr += 4; break;
            case 8: ptr += 8 + ((struct gguf_string*)ptr)->n; break;
            case 10: ptr += 8; break;
            case 11: ptr += 8; break;
            default: ptr += 8; break;
        }
    }
    
    ptr = (uint8_t*)(((uintptr_t)ptr + 31) & ~31);
    
    /* Find tensor */
    for (uint64_t i = 0; i < header->n_tensors; i++) {
        struct gguf_string* tensor_name = (struct gguf_string*)ptr;
        ptr += sizeof(uint64_t) + tensor_name->n;
        uint32_t n_dims = *(uint32_t*)ptr;
        ptr += 4;
        ptr += n_dims * 8; /* skip dims */
        uint32_t type = *(uint32_t*)ptr;
        ptr += 4;
        ptr += 8; /* skip offset */
        
        if (tensor_name->n == strlen(name) && 
            memcmp(tensor_name->data, name, tensor_name->n) == 0) {
            return type;
        }
    }
    return -1;
}

/* Load any tensor from GGUF */
float* load_gguf_tensor(const uint8_t* gguf_data, size_t gguf_size, 
                       const char* name, size_t expected_elements) {
    size_t tensor_size;
    void* tensor_data = gguf_find_tensor(gguf_data, gguf_size, name, &tensor_size);
    
    if (!tensor_data) {
        console_printf("Failed to find tensor: %s\n", name);
        return NULL;
    }
    
    /* Get tensor type */
    int type = gguf_get_tensor_type(gguf_data, gguf_size, name);
    console_printf("Loading %s: type=%d, size=%zu\n", name, type, tensor_size);
    
    /* Allocate float buffer */
    float* output = (float*)kmalloc(expected_elements * sizeof(float));
    if (!output) {
        console_printf("Failed to allocate buffer for %s\n", name);
        return NULL;
    }
    
    /* Dequantize based on type */
    if (type == 0) { /* F32 */
        memcpy(output, tensor_data, expected_elements * sizeof(float));
    } else if (type == 12) { /* Q4_K */
        size_t n_blocks = expected_elements / 256;
        dequantize_q4_k((uint8_t*)tensor_data, output, n_blocks);
    } else {
        console_printf("Unsupported tensor type: %d\n", type);
        kfree(output);
        return NULL;
    }
    
    return output;
}

/* Load token embeddings from GGUF */
float* load_token_embeddings(const uint8_t* gguf_data, size_t gguf_size) {
    return load_gguf_tensor(gguf_data, gguf_size, "token_embd.weight", 32000 * 2048);
}

/* Load output norm weights */
float* load_output_norm(const uint8_t* gguf_data, size_t gguf_size) {
    return load_gguf_tensor(gguf_data, gguf_size, "output_norm.weight", 2048);
}

/* Load layer weights */
float* load_layer_weight(const uint8_t* gguf_data, size_t gguf_size, 
                        const char* weight_name, size_t expected_elements) {
    return load_gguf_tensor(gguf_data, gguf_size, weight_name, expected_elements);
}

/* Load output projection weights */
float* load_output_weight(const uint8_t* gguf_data, size_t gguf_size) {
    return load_gguf_tensor(gguf_data, gguf_size, "output.weight", 32000 * 2048);
}