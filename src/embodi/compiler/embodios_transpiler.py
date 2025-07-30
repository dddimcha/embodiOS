"""
EMBODIOS Custom Transpiler
Handles EMBODIOS-specific code transformation that TVM doesn't cover
"""

import ast
import re
from typing import Dict, List, Optional, Any, Tuple
from pathlib import Path
import textwrap
import logging

class EMBODIOSTranspiler:
    """Custom transpiler for EMBODIOS-specific components"""
    
    def __init__(self):
        self.logger = logging.getLogger(__name__)
        self.hardware_tokens = {}
        self.converted_functions = {}
        
    def transpile_hardware_tokens(self, tokens: Dict[str, int]) -> str:
        """Convert hardware tokens to C defines"""
        self.hardware_tokens = tokens
        
        c_code = """/* EMBODIOS Hardware Token Definitions */
#ifndef EMBODIOS_TOKENS_H
#define EMBODIOS_TOKENS_H

"""
        # Convert tokens to C defines
        for token, value in sorted(tokens.items(), key=lambda x: x[1]):
            # Clean token name: <GPIO_READ> -> GPIO_READ_TOKEN
            name = token.strip('<>').upper().replace(' ', '_') + '_TOKEN'
            c_code += f"#define {name} {value}\n"
            
        # Add helper macros
        c_code += """
/* Helper macros for token operations */
#define IS_GPIO_TOKEN(x) ((x) >= 32000 && (x) <= 32003)
#define IS_I2C_TOKEN(x) ((x) >= 32020 && (x) <= 32021)
#define IS_UART_TOKEN(x) ((x) >= 32023 && (x) <= 32024)

#endif /* EMBODIOS_TOKENS_H */
"""
        return c_code
    
    def transpile_hal_operations(self, hal_code: str) -> Dict[str, str]:
        """Convert HAL operations to native C"""
        output_files = {}
        
        # Parse Python HAL code
        tree = ast.parse(hal_code)
        
        # Generate GPIO operations
        gpio_c = self._generate_gpio_c()
        output_files['hal_gpio.c'] = gpio_c
        
        # Generate I2C operations
        i2c_c = self._generate_i2c_c()
        output_files['hal_i2c.c'] = i2c_c
        
        # Generate UART operations
        uart_c = self._generate_uart_c()
        output_files['hal_uart.c'] = uart_c
        
        # Generate main HAL header
        hal_h = self._generate_hal_header()
        output_files['hal.h'] = hal_h
        
        return output_files
    
    def transpile_command_processor(self, nl_processor_code: str) -> str:
        """Convert natural language processor to optimized C"""
        # This converts the pattern matching to C
        
        c_code = """/* EMBODIOS Command Processor - Native Implementation */
#include "embodios.h"
#include "hal.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

typedef struct {
    const char* pattern;
    command_type_t type;
    command_handler_t handler;
} command_pattern_t;

/* Command patterns */
static const command_pattern_t patterns[] = {
    /* GPIO patterns */
    {"turn on gpio", CMD_GPIO, handle_gpio_on},
    {"turn off gpio", CMD_GPIO, handle_gpio_off},
    {"set gpio * high", CMD_GPIO, handle_gpio_high},
    {"set gpio * low", CMD_GPIO, handle_gpio_low},
    {"read gpio *", CMD_GPIO, handle_gpio_read},
    
    /* I2C patterns */
    {"read i2c device *", CMD_I2C, handle_i2c_read},
    {"write i2c device *", CMD_I2C, handle_i2c_write},
    {"scan i2c", CMD_I2C, handle_i2c_scan},
    
    /* UART patterns */
    {"send * to uart", CMD_UART, handle_uart_send},
    {"read uart", CMD_UART, handle_uart_receive},
    
    /* System patterns */
    {"show status", CMD_SYSTEM, handle_system_status},
    {"reboot", CMD_SYSTEM, handle_system_reboot},
    
    {NULL, CMD_UNKNOWN, NULL}
};

/* Extract number from string */
static int extract_number(const char* str) {
    const char* p = str;
    while (*p && !isdigit(*p)) p++;
    return atoi(p);
}

/* Process natural language command */
hardware_command_t process_command(const char* input) {
    hardware_command_t cmd = {0};
    cmd.type = CMD_UNKNOWN;
    
    /* Convert to lowercase for comparison */
    char lower_input[256];
    strncpy(lower_input, input, sizeof(lower_input) - 1);
    for (char* p = lower_input; *p; p++) {
        *p = tolower(*p);
    }
    
    /* Try to match patterns */
    for (const command_pattern_t* pat = patterns; pat->pattern; pat++) {
        if (strstr(lower_input, pat->pattern)) {
            cmd.type = pat->type;
            cmd.handler = pat->handler;
            
            /* Extract parameters based on command type */
            if (cmd.type == CMD_GPIO) {
                cmd.params.gpio.pin = extract_number(lower_input);
                cmd.params.gpio.value = strstr(lower_input, "high") || 
                                       strstr(lower_input, "on");
            } else if (cmd.type == CMD_I2C) {
                cmd.params.i2c.device = extract_number(lower_input);
                /* Extract register if present */
                const char* reg_pos = strstr(lower_input, "register");
                if (reg_pos) {
                    cmd.params.i2c.register_addr = extract_number(reg_pos);
                }
            }
            
            break;
        }
    }
    
    return cmd;
}

/* Convert hardware command to token sequence */
void command_to_tokens(const hardware_command_t* cmd, int32_t* tokens, size_t* len) {
    size_t idx = 0;
    
    switch (cmd->type) {
        case CMD_GPIO:
            if (cmd->params.gpio.value) {
                tokens[idx++] = GPIO_WRITE_TOKEN;
                tokens[idx++] = cmd->params.gpio.pin;
                tokens[idx++] = GPIO_HIGH_TOKEN;
            } else {
                tokens[idx++] = GPIO_WRITE_TOKEN;
                tokens[idx++] = cmd->params.gpio.pin;
                tokens[idx++] = GPIO_LOW_TOKEN;
            }
            break;
            
        case CMD_I2C:
            tokens[idx++] = I2C_READ_TOKEN;
            tokens[idx++] = cmd->params.i2c.device;
            tokens[idx++] = cmd->params.i2c.register_addr;
            break;
            
        default:
            break;
    }
    
    *len = idx;
}
"""
        return c_code
    
    def transpile_runtime_kernel(self, kernel_code: str) -> str:
        """Convert runtime kernel to bare metal C"""
        # This creates the main kernel loop
        
        return """/* EMBODIOS Runtime Kernel - Native Implementation */
#include "embodios.h"
#include "hal.h"
#include "inference.h"
#include <stdint.h>
#include <stdbool.h>

/* Main kernel state */
typedef struct {
    bool running;
    uint32_t tick_count;
    model_state_t* model;
    hal_state_t* hal;
} kernel_state_t;

static kernel_state_t g_kernel = {0};

/* Initialize kernel */
int kernel_init(void) {
    /* Initialize HAL */
    g_kernel.hal = hal_init();
    if (!g_kernel.hal) {
        return -1;
    }
    
    /* Initialize model */
    g_kernel.model = model_init();
    if (!g_kernel.model) {
        hal_cleanup(g_kernel.hal);
        return -1;
    }
    
    g_kernel.running = true;
    return 0;
}

/* Main kernel loop */
void kernel_main_loop(void) {
    char input_buffer[256];
    int32_t tokens[MAX_TOKENS];
    int32_t output_tokens[MAX_TOKENS];
    
    while (g_kernel.running) {
        /* Check for input */
        if (hal_uart_available(g_kernel.hal)) {
            /* Read command */
            size_t len = hal_uart_read(g_kernel.hal, input_buffer, sizeof(input_buffer));
            input_buffer[len] = '\\0';
            
            /* Process command */
            hardware_command_t cmd = process_command(input_buffer);
            
            if (cmd.type != CMD_UNKNOWN) {
                /* Execute hardware command */
                execute_hardware_command(g_kernel.hal, &cmd);
            } else {
                /* Use AI model for complex commands */
                size_t token_len;
                text_to_tokens(input_buffer, tokens, &token_len);
                
                /* Run inference */
                inference_result_t result = model_inference(
                    g_kernel.model, tokens, token_len, output_tokens
                );
                
                /* Process AI output */
                process_ai_output(g_kernel.hal, output_tokens, result.length);
            }
        }
        
        /* Update tick count */
        g_kernel.tick_count++;
        
        /* Yield CPU */
        hal_cpu_yield();
    }
}

/* Shutdown kernel */
void kernel_shutdown(void) {
    g_kernel.running = false;
    
    if (g_kernel.model) {
        model_cleanup(g_kernel.model);
    }
    
    if (g_kernel.hal) {
        hal_cleanup(g_kernel.hal);
    }
}
"""
    
    def _generate_gpio_c(self) -> str:
        """Generate native GPIO implementation"""
        return """/* EMBODIOS HAL - GPIO Implementation */
#include "hal.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* BCM2835/2837 GPIO registers for Raspberry Pi */
#define GPIO_BASE_BCM2835  0x20200000
#define GPIO_BASE_BCM2837  0x3F200000
#define GPIO_BASE_BCM2711  0xFE200000

/* GPIO register offsets */
#define GPFSEL0    0x00  /* Function select */
#define GPSET0     0x1C  /* Pin output set */
#define GPCLR0     0x28  /* Pin output clear */
#define GPLEV0     0x34  /* Pin level */

static volatile uint32_t* gpio_base = ((void*)0);

/* Initialize GPIO */
void gpio_init(void) {
    /* Map GPIO registers - use default base */
    gpio_base = (volatile uint32_t*)(uintptr_t)GPIO_BASE_BCM2837;
}

/* Set pin mode */
void gpio_set_mode(uint32_t pin, uint8_t mode) {
    if (!gpio_base) return;
    
    uint32_t fsel_reg = pin / 10;
    uint32_t fsel_shift = (pin % 10) * 3;
    
    uint32_t reg_val = gpio_base[GPFSEL0/4 + fsel_reg];
    reg_val &= ~(7 << fsel_shift);
    
    if (mode == 1) {  /* GPIO_OUTPUT */
        reg_val |= (1 << fsel_shift);
    }
    
    gpio_base[GPFSEL0/4 + fsel_reg] = reg_val;
}

/* Write to pin */
void gpio_write(uint32_t pin, bool value) {
    if (value) {
        gpio_base[GPSET0/4 + pin/32] = (1 << (pin % 32));
    } else {
        gpio_base[GPCLR0/4 + pin/32] = (1 << (pin % 32));
    }
}

/* Read from pin */
bool gpio_read(uint32_t pin) {
    return (gpio_base[GPLEV0/4 + pin/32] >> (pin % 32)) & 1;
}

/* Direct register access for performance */
inline void gpio_set_fast(uint32_t pin) {
    *(gpio_base + 7) = 1 << pin;  /* GPSET0 */
}

inline void gpio_clear_fast(uint32_t pin) {
    *(gpio_base + 10) = 1 << pin; /* GPCLR0 */
}
"""
    
    def _generate_i2c_c(self) -> str:
        """Generate native I2C implementation"""
        return """/* EMBODIOS HAL - I2C Implementation */
#include "hal.h"
#include <stdint.h>

/* BCM2835 I2C registers */
#define I2C_BASE       0x3F804000
#define BSC_C          0x00
#define BSC_S          0x04
#define BSC_DLEN       0x08
#define BSC_A          0x0C
#define BSC_FIFO       0x10
#define BSC_DIV        0x14
#define BSC_DEL        0x18
#define BSC_CLKT       0x1C

/* Control register bits */
#define BSC_C_I2CEN    (1 << 15)
#define BSC_C_INTR     (1 << 10)
#define BSC_C_INTT     (1 << 9)
#define BSC_C_INTD     (1 << 8)
#define BSC_C_ST       (1 << 7)
#define BSC_C_CLEAR    (1 << 5)
#define BSC_C_READ     (1 << 0)

/* Status register bits */
#define BSC_S_CLKT     (1 << 9)
#define BSC_S_ERR      (1 << 8)
#define BSC_S_RXF      (1 << 7)
#define BSC_S_TXE      (1 << 6)
#define BSC_S_RXD      (1 << 5)
#define BSC_S_TXD      (1 << 4)
#define BSC_S_RXR      (1 << 3)
#define BSC_S_TXW      (1 << 2)
#define BSC_S_DONE     (1 << 1)
#define BSC_S_TA       (1 << 0)

static volatile uint32_t* i2c_base = NULL;

/* Initialize I2C */
int i2c_init(uint32_t base_addr, uint32_t clock_speed) {
    i2c_base = (volatile uint32_t*)base_addr;
    
    /* Set clock divider for desired speed */
    uint32_t divider = 150000000 / clock_speed;
    i2c_base[BSC_DIV/4] = divider;
    
    /* Clear FIFO */
    i2c_base[BSC_C/4] = BSC_C_CLEAR;
    
    return 0;
}

/* Write to I2C device */
int i2c_write(uint8_t addr, uint8_t reg, const uint8_t* data, size_t len) {
    /* Set slave address */
    i2c_base[BSC_A/4] = addr;
    
    /* Clear status */
    i2c_base[BSC_S/4] = BSC_S_CLKT | BSC_S_ERR | BSC_S_DONE;
    
    /* Set data length (register + data) */
    i2c_base[BSC_DLEN/4] = len + 1;
    
    /* Write register address */
    i2c_base[BSC_FIFO/4] = reg;
    
    /* Write data */
    for (size_t i = 0; i < len; i++) {
        i2c_base[BSC_FIFO/4] = data[i];
    }
    
    /* Start transfer */
    i2c_base[BSC_C/4] = BSC_C_I2CEN | BSC_C_ST;
    
    /* Wait for completion */
    while (!(i2c_base[BSC_S/4] & BSC_S_DONE)) {
        if (i2c_base[BSC_S/4] & BSC_S_ERR) {
            return -1;
        }
    }
    
    return 0;
}

/* Read from I2C device */
int i2c_read(uint8_t addr, uint8_t reg, uint8_t* data, size_t len) {
    /* Write register address first */
    i2c_base[BSC_A/4] = addr;
    i2c_base[BSC_DLEN/4] = 1;
    i2c_base[BSC_FIFO/4] = reg;
    i2c_base[BSC_C/4] = BSC_C_I2CEN | BSC_C_ST;
    
    while (!(i2c_base[BSC_S/4] & BSC_S_DONE));
    
    /* Now read data */
    i2c_base[BSC_DLEN/4] = len;
    i2c_base[BSC_C/4] = BSC_C_I2CEN | BSC_C_ST | BSC_C_READ;
    
    /* Read from FIFO */
    for (size_t i = 0; i < len; i++) {
        while (!(i2c_base[BSC_S/4] & BSC_S_RXD));
        data[i] = i2c_base[BSC_FIFO/4];
    }
    
    return 0;
}
"""
    
    def _generate_uart_c(self) -> str:
        """Generate native UART implementation"""
        return """/* EMBODIOS HAL - UART Implementation */
#include "hal.h"
#include <stdint.h>
#include <stdbool.h>

/* PL011 UART registers */
#define UART_BASE      0x3F201000
#define UART_DR        0x00
#define UART_FR        0x18
#define UART_IBRD      0x24
#define UART_FBRD      0x28
#define UART_LCRH      0x2C
#define UART_CR        0x30
#define UART_IMSC      0x38
#define UART_ICR       0x44

/* Flag register bits */
#define UART_FR_RXFE   (1 << 4)
#define UART_FR_TXFF   (1 << 5)
#define UART_FR_RXFF   (1 << 6)
#define UART_FR_TXFE   (1 << 7)

/* Control register bits */
#define UART_CR_UARTEN (1 << 0)
#define UART_CR_TXE    (1 << 8)
#define UART_CR_RXE    (1 << 9)

static volatile uint32_t* uart_base = NULL;

/* Initialize UART */
int uart_init(uint32_t base_addr, uint32_t baudrate) {
    uart_base = (volatile uint32_t*)base_addr;
    
    /* Disable UART */
    uart_base[UART_CR/4] = 0;
    
    /* Set baud rate */
    uint32_t uart_clock = 48000000;
    uint32_t baud_div = (uart_clock * 4) / baudrate;
    uart_base[UART_IBRD/4] = baud_div >> 6;
    uart_base[UART_FBRD/4] = baud_div & 0x3F;
    
    /* 8N1, enable FIFOs */
    uart_base[UART_LCRH/4] = (3 << 5) | (1 << 4);
    
    /* Enable UART, TX, RX */
    uart_base[UART_CR/4] = UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE;
    
    return 0;
}

/* Write to UART */
void uart_write(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        /* Wait for TX FIFO space */
        while (uart_base[UART_FR/4] & UART_FR_TXFF);
        uart_base[UART_DR/4] = data[i];
    }
}

/* Read from UART */
size_t uart_read(uint8_t* data, size_t max_len) {
    size_t count = 0;
    
    while (count < max_len && !(uart_base[UART_FR/4] & UART_FR_RXFE)) {
        data[count++] = uart_base[UART_DR/4] & 0xFF;
    }
    
    return count;
}

/* Check if data available */
bool uart_available(void) {
    return !(uart_base[UART_FR/4] & UART_FR_RXFE);
}

/* Write string helper */
void uart_puts(const char* str) {
    while (*str) {
        while (uart_base[UART_FR/4] & UART_FR_TXFF);
        uart_base[UART_DR/4] = *str++;
    }
}
"""
    
    def _generate_hal_header(self) -> str:
        """Generate main HAL header file"""
        return """/* EMBODIOS Hardware Abstraction Layer */
#ifndef EMBODIOS_HAL_H
#define EMBODIOS_HAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GPIO definitions */
#define GPIO_INPUT 0
#define GPIO_OUTPUT 1

/* GPIO functions */
void gpio_init(void);
void gpio_set_mode(uint32_t pin, uint8_t mode);
void gpio_write(uint32_t pin, bool value);
bool gpio_read(uint32_t pin);

/* I2C functions */
int i2c_init(uint32_t base_addr, uint32_t clock_speed);
int i2c_write(uint8_t addr, uint8_t reg, const uint8_t* data, size_t len);
int i2c_read(uint8_t addr, uint8_t reg, uint8_t* data, size_t len);

/* UART functions */
int uart_init(uint32_t base_addr, uint32_t baudrate);
void uart_write(const uint8_t* data, size_t len);
size_t uart_read(uint8_t* data, size_t max_len);
bool uart_available(void);
void uart_puts(const char* str);

/* Command types */
typedef enum {
    CMD_UNKNOWN = 0,
    CMD_GPIO,
    CMD_I2C,
    CMD_SPI,
    CMD_UART,
    CMD_MEMORY,
    CMD_SYSTEM
} command_type_t;

/* Hardware command structure */
typedef struct {
    command_type_t type;
    union {
        struct {
            uint32_t pin;
            bool value;
        } gpio;
        struct {
            uint8_t device;
            uint8_t register_addr;
            uint8_t* data;
            size_t len;
        } i2c;
        struct {
            uint8_t* data;
            size_t len;
        } uart;
    } params;
    void (*handler)(void*);
} hardware_command_t;

/* HAL state */
typedef struct {
    uint32_t gpio_base;
    uint32_t i2c_base;
    uint32_t uart_base;
    bool initialized;
} hal_state_t;

/* Main HAL functions */
hal_state_t* hal_init(void);
void hal_cleanup(hal_state_t* hal);
int execute_hardware_command(hal_state_t* hal, const hardware_command_t* cmd);
bool hal_uart_available(hal_state_t* hal);
size_t hal_uart_read(hal_state_t* hal, char* buffer, size_t max_len);
void hal_cpu_yield(void);

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_HAL_H */
"""
    
    def transpile_minimal_boot(self) -> str:
        """Generate minimal boot code for bare metal"""
        return """/* EMBODIOS Minimal Boot Code - ARM64 */
.section __TEXT,__text
.global _start

_start:
    /* Set up stack pointer */
    mov x0, #0x80000
    mov sp, x0
    
    /* Clear BSS section */
    adrp x0, __bss_start@PAGE
    add x0, x0, __bss_start@PAGEOFF
    adrp x1, __bss_end@PAGE
    add x1, x1, __bss_end@PAGEOFF
    mov x2, #0
    
bss_loop:
    cmp x0, x1
    b.ge bss_done
    str xzr, [x0], #8
    b bss_loop
    
bss_done:
    /* Call kernel init */
    bl _kernel_init
    
    /* Call kernel main */
    bl _kernel_main_loop
    
    /* Halt if we return */
halt:
    wfe
    b halt

.section __DATA,__data
boot_msg:
    .ascii "EMBODIOS Native AI Kernel booting...\\n\\0"
    
.align 3
"""


def transpile_embodios_component(component: str, code: str) -> Dict[str, str]:
    """Convenience function to transpile a specific EMBODIOS component"""
    transpiler = EMBODIOSTranspiler()
    
    if component == "hardware_tokens":
        # Extract tokens from code
        import ast
        tree = ast.parse(code)
        tokens = {}
        for node in ast.walk(tree):
            if isinstance(node, ast.Dict):
                for k, v in zip(node.keys, node.values):
                    if isinstance(k, ast.Constant) and isinstance(v, ast.Constant):
                        tokens[k.value] = v.value
        return {"tokens.h": transpiler.transpile_hardware_tokens(tokens)}
        
    elif component == "hal":
        return transpiler.transpile_hal_operations(code)
        
    elif component == "nl_processor":
        return {"nl_processor.c": transpiler.transpile_command_processor(code)}
        
    elif component == "runtime_kernel":
        return {"kernel.c": transpiler.transpile_runtime_kernel(code)}
        
    else:
        raise ValueError(f"Unknown component: {component}")