/* EMBODIOS Hardware Abstraction Layer - I/O Operations
 *
 * Abstract interface for I/O operations including port I/O (x86_64)
 * and memory-mapped I/O (ARM, RISC-V, etc.).
 */

#ifndef EMBODIOS_HAL_IO_H
#define EMBODIOS_HAL_IO_H

#include <embodios/types.h>

/* I/O access types */
#define IO_TYPE_PORT        0  /* Port-based I/O (x86_64) */
#define IO_TYPE_MMIO        1  /* Memory-mapped I/O (ARM, RISC-V) */

/* HAL I/O operations structure */
struct hal_io_ops {
    /* Initialization */
    void (*init)(void);

    /* I/O type detection */
    uint32_t (*get_type)(void);

    /* 8-bit I/O operations */
    void (*write8)(uint64_t addr, uint8_t value);
    uint8_t (*read8)(uint64_t addr);

    /* 16-bit I/O operations */
    void (*write16)(uint64_t addr, uint16_t value);
    uint16_t (*read16)(uint64_t addr);

    /* 32-bit I/O operations */
    void (*write32)(uint64_t addr, uint32_t value);
    uint32_t (*read32)(uint64_t addr);

    /* 64-bit I/O operations (optional, may be NULL) */
    void (*write64)(uint64_t addr, uint64_t value);
    uint64_t (*read64)(uint64_t addr);

    /* I/O delay/wait */
    void (*wait)(void);

    /* Memory barriers (for MMIO ordering) */
    void (*mb)(void);   /* Full memory barrier */
    void (*rmb)(void);  /* Read memory barrier */
    void (*wmb)(void);  /* Write memory barrier */
};

/* HAL I/O interface functions */
void hal_io_register(const struct hal_io_ops *ops);
const struct hal_io_ops* hal_io_get_ops(void);

/* HAL I/O wrapper functions */
void hal_io_init(void);
uint32_t hal_io_get_type(void);

/* 8-bit I/O */
void hal_io_write8(uint64_t addr, uint8_t value);
uint8_t hal_io_read8(uint64_t addr);

/* 16-bit I/O */
void hal_io_write16(uint64_t addr, uint16_t value);
uint16_t hal_io_read16(uint64_t addr);

/* 32-bit I/O */
void hal_io_write32(uint64_t addr, uint32_t value);
uint32_t hal_io_read32(uint64_t addr);

/* 64-bit I/O */
void hal_io_write64(uint64_t addr, uint64_t value);
uint64_t hal_io_read64(uint64_t addr);

/* I/O wait and barriers */
void hal_io_wait(void);
void hal_io_mb(void);
void hal_io_rmb(void);
void hal_io_wmb(void);

/* Legacy compatibility macros for x86_64 port I/O */
#define hal_outb(port, value)   hal_io_write8((port), (value))
#define hal_inb(port)           hal_io_read8(port)
#define hal_outw(port, value)   hal_io_write16((port), (value))
#define hal_inw(port)           hal_io_read16(port)
#define hal_outl(port, value)   hal_io_write32((port), (value))
#define hal_inl(port)           hal_io_read32(port)

#endif /* EMBODIOS_HAL_IO_H */
