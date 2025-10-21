/* EMBODIOS Initrd Model Loader
 * Loads AI models from initrd at boot time
 */

#include <embodios/types.h>
#include <embodios/kernel.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/string.h>

/* External functions */
extern void* initrd_get_base(void);
extern size_t initrd_get_size(void);
extern struct ai_model* model_heap_alloc(size_t size);
extern int model_register(struct ai_model* model, const char* name);

/* CPIO header structure (newc format) */
struct cpio_header {
    char magic[6];      /* "070701" */
    char ino[8];
    char mode[8];
    char uid[8];
    char gid[8];
    char nlink[8];
    char mtime[8];
    char filesize[8];
    char devmajor[8];
    char devminor[8];
    char rdevmajor[8];
    char rdevminor[8];
    char namesize[8];
    char check[8];
} __attribute__((packed));

/* GGUF header for detection */
struct gguf_header {
    uint32_t magic;     /* 'GGUF' */
    uint32_t version;
    uint64_t tensor_count;
    uint64_t metadata_kv_count;
};

/* Convert hex string to number */
static uint32_t hex_to_u32(const char* hex, int len) {
    uint32_t val = 0;
    for (int i = 0; i < len; i++) {
        val *= 16;
        if (hex[i] >= '0' && hex[i] <= '9') {
            val += hex[i] - '0';
        } else if (hex[i] >= 'A' && hex[i] <= 'F') {
            val += hex[i] - 'A' + 10;
        } else if (hex[i] >= 'a' && hex[i] <= 'f') {
            val += hex[i] - 'a' + 10;
        }
    }
    return val;
}

/* Check if file is a model */
static bool is_model_file(const char* name, size_t name_len) {
    /* Check for .gguf extension */
    if (name_len > 5 && strcmp(name + name_len - 5, ".gguf") == 0) {
        return true;
    }
    
    /* Check for .emb extension */
    if (name_len > 4 && strcmp(name + name_len - 4, ".emb") == 0) {
        return true;
    }
    
    return false;
}

/* Create GGUF model instance */
static struct ai_model* create_gguf_model(const char* name, void* data, size_t size) {
    struct gguf_header* header = (struct gguf_header*)data;
    
    /* Validate GGUF magic */
    if (header->magic != 0x46554747) {  /* 'GGUF' */
        console_printf("Invalid GGUF magic: %08x\n", header->magic);
        return NULL;
    }
    
    /* Allocate model structure */
    struct ai_model* model = model_heap_alloc(sizeof(struct ai_model));
    if (!model) return NULL;
    
    memset(model, 0, sizeof(struct ai_model));
    
    /* Extract base name */
    char base_name[64];
    const char* slash = strrchr(name, '/');
    const char* base = slash ? slash + 1 : name;
    strncpy(base_name, base, 63);
    char* dot = strchr(base_name, '.');
    if (dot) *dot = '\0';
    
    /* Set metadata */
    strncpy(model->meta.name, base_name, MAX_MODEL_NAME - 1);
    snprintf(model->meta.description, 127, "GGUF model from initrd");
    model->meta.format = MODEL_FORMAT_GGUF;
    model->meta.capabilities = MODEL_CAP_TEXT_GEN | MODEL_CAP_CHAT;
    
    /* Estimate parameters from file size */
    model->meta.weight_size = size;
    
    if (size < 100 * 1024 * 1024) {  /* < 100MB */
        model->meta.vocab_size = 32000;
        model->meta.hidden_dim = 512;
        model->meta.n_layers = 6;
    } else if (size < 500 * 1024 * 1024) {  /* < 500MB */
        model->meta.vocab_size = 32000;
        model->meta.hidden_dim = 2048;
        model->meta.n_layers = 22;
    } else {  /* Large model */
        model->meta.vocab_size = 32000;
        model->meta.hidden_dim = 4096;
        model->meta.n_layers = 32;
    }
    
    model->meta.n_heads = model->meta.hidden_dim / 64;
    model->meta.context_length = 2048;
    model->meta.runtime_memory = 16 * 1024 * 1024;  /* 16MB runtime */
    
    /* Store pointer to weights (no copy) */
    model->weights = data;
    
    /* TODO: Set inference function based on quantization type */
    model->inference = NULL;  /* Would be set based on model type */
    
    console_printf("Created GGUF model: %s (%.1f MB)\n", 
                  model->meta.name, size / (1024.0 * 1024.0));
    
    return model;
}

/* Scan initrd for models */
int initrd_scan_models(void) {
    void* initrd_base = initrd_get_base();
    size_t initrd_size = initrd_get_size();
    
    if (!initrd_base || initrd_size == 0) {
        console_printf("Initrd: No initrd loaded\n");
        return 0;
    }
    
    console_printf("Initrd: Scanning for models at %p (size: %zu KB)\n",
                  initrd_base, initrd_size / 1024);
    
    int models_found = 0;
    char* ptr = (char*)initrd_base;
    char* end = ptr + initrd_size;
    
    /* Parse CPIO archive */
    while (ptr < end - sizeof(struct cpio_header)) {
        struct cpio_header* hdr = (struct cpio_header*)ptr;
        
        /* Check magic */
        if (memcmp(hdr->magic, "070701", 6) != 0) {
            break;  /* End of archive or corruption */
        }
        
        /* Parse header fields */
        uint32_t namesize = hex_to_u32(hdr->namesize, 8);
        uint32_t filesize = hex_to_u32(hdr->filesize, 8);
        
        /* Get filename */
        char* filename = ptr + sizeof(struct cpio_header);
        
        /* Check for TRAILER!!! */
        if (strcmp(filename, "TRAILER!!!") == 0) {
            break;
        }
        
        /* Calculate padded sizes */
        size_t hdr_size = sizeof(struct cpio_header) + namesize;
        hdr_size = (hdr_size + 3) & ~3;  /* Align to 4 bytes */
        
        /* Get file data */
        void* filedata = ptr + hdr_size;
        
        /* Check if this is a model file */
        if (is_model_file(filename, namesize - 1)) {
            console_printf("Found model: %s (%u bytes)\n", filename, filesize);
            
            /* Create model based on type */
            struct ai_model* model = NULL;
            
            if (strstr(filename, ".gguf")) {
                model = create_gguf_model(filename, filedata, filesize);
            }
            /* Add other formats here */
            
            if (model) {
                if (model_register(model, NULL) == 0) {
                    models_found++;
                }
            }
        }
        
        /* Move to next entry */
        size_t entry_size = hdr_size + filesize;
        entry_size = (entry_size + 3) & ~3;  /* Align to 4 bytes */
        ptr += entry_size;
    }
    
    console_printf("Initrd: Found %d models\n", models_found);
    
    /* Set default to first non-embedded model if found */
    if (models_found > 0) {
        /* TODO: Better default selection logic */
        console_printf("Initrd: Models loaded successfully\n");
    }
    
    return models_found;
}

/* Load specific model from initrd */
struct ai_model* initrd_load_model(const char* path) {
    /* TODO: Implement targeted loading */
    console_printf("Initrd: Direct loading not yet implemented\n");
    return NULL;
}

/* Get initrd stats */
void initrd_get_stats(char* buffer, size_t size) {
    void* base = initrd_get_base();
    size_t initrd_size = initrd_get_size();
    
    if (base && initrd_size > 0) {
        snprintf(buffer, size, "Initrd: %zu KB at %p", 
                initrd_size / 1024, base);
    } else {
        snprintf(buffer, size, "Initrd: Not loaded");
    }
}