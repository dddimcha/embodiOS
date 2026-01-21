/* EMBODIOS GDB Stub for Kernel Debugging
 *
 * Implements GDB remote serial protocol for kernel debugging.
 * Allows connection via QEMU: -s -S (or -gdb tcp::1234)
 *
 * Features:
 * - Register read/write
 * - Memory read/write
 * - Breakpoints
 * - Single stepping
 * - Continue execution
 *
 * Usage with QEMU:
 *   qemu-system-x86_64 -kernel embodios.elf -m 256M -s -S
 *   gdb embodios.elf -ex "target remote :1234"
 */

#ifndef EMBODIOS_GDB_STUB_H
#define EMBODIOS_GDB_STUB_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Register Indices (x86_64)
 * ============================================================================ */

/* General purpose registers */
#define GDB_REG_RAX     0
#define GDB_REG_RBX     1
#define GDB_REG_RCX     2
#define GDB_REG_RDX     3
#define GDB_REG_RSI     4
#define GDB_REG_RDI     5
#define GDB_REG_RBP     6
#define GDB_REG_RSP     7
#define GDB_REG_R8      8
#define GDB_REG_R9      9
#define GDB_REG_R10     10
#define GDB_REG_R11     11
#define GDB_REG_R12     12
#define GDB_REG_R13     13
#define GDB_REG_R14     14
#define GDB_REG_R15     15
#define GDB_REG_RIP     16
#define GDB_REG_RFLAGS  17
#define GDB_REG_CS      18
#define GDB_REG_SS      19
#define GDB_REG_DS      20
#define GDB_REG_ES      21
#define GDB_REG_FS      22
#define GDB_REG_GS      23

#define GDB_NUM_REGS    24

/* ============================================================================
 * CPU Context (saved during debug exception)
 * ============================================================================ */

typedef struct gdb_regs {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t rsp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rip;
    uint64_t rflags;
    uint64_t cs;
    uint64_t ss;
    uint64_t ds;
    uint64_t es;
    uint64_t fs;
    uint64_t gs;
} gdb_regs_t;

/* ============================================================================
 * Breakpoint Management
 * ============================================================================ */

#define GDB_MAX_BREAKPOINTS     32

typedef struct gdb_breakpoint {
    uint64_t addr;          /* Address of breakpoint */
    uint8_t  saved_byte;    /* Original byte at address */
    bool     active;        /* Breakpoint is active */
} gdb_breakpoint_t;

/* ============================================================================
 * GDB Stub State
 * ============================================================================ */

typedef struct gdb_stub {
    bool initialized;           /* Stub is initialized */
    bool connected;             /* GDB is connected */
    bool single_stepping;       /* Single-step mode active */

    gdb_regs_t regs;            /* Current CPU registers */

    gdb_breakpoint_t breakpoints[GDB_MAX_BREAKPOINTS];
    int num_breakpoints;

    /* Packet buffer */
    char packet_buf[4096];
    int packet_len;

    /* Statistics */
    uint64_t packets_rx;        /* Packets received */
    uint64_t packets_tx;        /* Packets sent */
} gdb_stub_t;

/* ============================================================================
 * Signal Numbers (for GDB)
 * ============================================================================ */

#define GDB_SIGNAL_INT      2   /* Interrupt */
#define GDB_SIGNAL_TRAP     5   /* Breakpoint */
#define GDB_SIGNAL_ABRT     6   /* Abort */
#define GDB_SIGNAL_FPE      8   /* Floating point exception */
#define GDB_SIGNAL_SEGV     11  /* Segmentation fault */

/* ============================================================================
 * Error Codes
 * ============================================================================ */

#define GDB_OK              0
#define GDB_ERR_INIT        -1
#define GDB_ERR_COMM        -2
#define GDB_ERR_TIMEOUT     -3
#define GDB_ERR_INVALID     -4

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize GDB stub
 * Sets up serial communication and exception handlers
 * @return GDB_OK on success, error code on failure
 */
int gdb_stub_init(void);

/**
 * Check if GDB stub is initialized
 * @return true if initialized
 */
bool gdb_stub_is_initialized(void);

/**
 * Check if GDB is connected
 * @return true if connected
 */
bool gdb_stub_is_connected(void);

/**
 * Enter the debugger
 * Call this to break into the debugger from code
 * Similar to __asm__("int3")
 */
void gdb_breakpoint(void);

/**
 * Handle debug exception
 * Called from interrupt handler when breakpoint or single-step occurs
 * @param regs Pointer to saved CPU registers
 * @param signal Signal number to report to GDB
 */
void gdb_handle_exception(gdb_regs_t *regs, int signal);

/**
 * Set a software breakpoint
 * @param addr Address to set breakpoint at
 * @return 0 on success, -1 on failure
 */
int gdb_set_breakpoint(uint64_t addr);

/**
 * Remove a software breakpoint
 * @param addr Address to remove breakpoint from
 * @return 0 on success, -1 on failure
 */
int gdb_remove_breakpoint(uint64_t addr);

/**
 * Process incoming GDB packets (polling mode)
 * Call periodically to handle GDB commands
 */
void gdb_stub_poll(void);

/**
 * Print GDB stub status
 */
void gdb_stub_print_info(void);

/**
 * Run GDB stub self-tests
 * @return 0 on success, -1 on failure
 */
int gdb_stub_run_tests(void);

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_GDB_STUB_H */
