/* Kernel-side client for userspace accelerator
 * Allows kernel to offload inference to userspace when available
 */

#include <embodios/types.h>
#include <embodios/kernel.h>
#include <embodios/console.h>
#include <embodios/mm.h>

/* Shared memory structure (must match userspace) */
struct ai_request {
    uint32_t magic;
    uint32_t version;
    uint32_t request_id;
    uint32_t status;
    
    uint32_t prompt_len;
    uint32_t max_tokens;
    float temperature;
    uint32_t flags;
    
    uint32_t response_len;
    uint32_t tokens_generated;
    uint64_t inference_time_us;
    
    char prompt[1024];
    char response[4096];
};

/* Physical address for shared memory (would be set up during boot) */
#define ACCEL_SHARED_MEM_PHYS 0x50000000  /* Example: 1.25GB mark */
#define ACCEL_SHARED_MEM_SIZE (4 * 1024 * 1024)

static struct ai_request* accel_mem = NULL;
static uint32_t next_request_id = 1;
static bool accelerator_available = false;

/* Initialize accelerator client */
int accelerator_client_init(void) {
    console_printf("Accelerator Client: Initializing...\n");
    
    /* Map shared memory region */
    accel_mem = (struct ai_request*)vmm_map_physical(
        ACCEL_SHARED_MEM_PHYS,
        ACCEL_SHARED_MEM_SIZE,
        VMM_WRITE | VMM_READ
    );
    
    if (!accel_mem) {
        console_printf("Accelerator Client: Failed to map shared memory\n");
        return -1;
    }
    
    /* Check if accelerator is running */
    if (accel_mem->magic == 0x41494F53 && accel_mem->version == 1) {
        accelerator_available = true;
        console_printf("Accelerator Client: Connected to userspace accelerator\n");
    } else {
        console_printf("Accelerator Client: No accelerator detected\n");
        accelerator_available = false;
    }
    
    return 0;
}

/* Check if accelerator is available */
bool is_accelerator_available(void) {
    return accelerator_available && accel_mem && 
           accel_mem->magic == 0x41494F53;
}

/* Send inference request to accelerator */
int accelerator_inference(const char* prompt, char* response, 
                         int max_response, int max_tokens) {
    if (!is_accelerator_available()) {
        return -1;
    }
    
    /* Wait for slot to be available */
    int timeout = 1000;  /* 1 second timeout */
    while (accel_mem->status != 0 && accel_mem->status != 3 && timeout > 0) {
        timer_sleep(1);  /* 1ms */
        timeout--;
    }
    
    if (timeout == 0) {
        console_printf("Accelerator Client: Timeout waiting for slot\n");
        return -1;
    }
    
    /* Prepare request */
    accel_mem->request_id = next_request_id++;
    accel_mem->prompt_len = strlen(prompt);
    strncpy(accel_mem->prompt, prompt, 1023);
    accel_mem->prompt[1023] = '\0';
    accel_mem->max_tokens = max_tokens;
    accel_mem->temperature = 0.7f;
    accel_mem->flags = 0;
    
    /* Submit request */
    accel_mem->status = 1;  /* Pending */
    
    /* Wait for completion */
    timeout = 5000;  /* 5 second timeout */
    while (accel_mem->status != 3 && timeout > 0) {
        timer_sleep(1);
        timeout--;
    }
    
    if (timeout == 0) {
        console_printf("Accelerator Client: Timeout waiting for response\n");
        return -1;
    }
    
    /* Copy response */
    strncpy(response, accel_mem->response, max_response - 1);
    response[max_response - 1] = '\0';
    
    /* Mark as consumed */
    accel_mem->status = 0;
    
    return 0;
}

/* Fallback to kernel inference if accelerator not available */
extern int kernel_inference(const char* prompt, char* output, int max_output);

/* Unified inference interface */
int embodios_inference(const char* prompt, char* response, int max_response) {
    /* Try accelerator first */
    if (is_accelerator_available()) {
        console_printf("Using userspace accelerator for inference\n");
        if (accelerator_inference(prompt, response, max_response, 50) == 0) {
            return 0;
        }
        console_printf("Accelerator failed, falling back to kernel\n");
    }
    
    /* Fall back to kernel inference */
    console_printf("Using kernel inference\n");
    return kernel_inference(prompt, response, max_response);
}