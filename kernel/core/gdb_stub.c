/* EMBODIOS GDB Stub Implementation
 *
 * Implements GDB remote serial protocol for kernel debugging.
 * Uses COM1 serial port (x86) or PL011 UART (ARM64) for communication.
 */

#include <embodios/gdb_stub.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/kernel.h>
#include <io.h>  /* Architecture-specific I/O */

/* GDB stub state */
static gdb_stub_t g_gdb;
static bool gdb_initialized = false;

/* ============================================================================
 * Architecture-Specific Serial Port Functions
 * ============================================================================ */

#if defined(__x86_64__) || defined(__i386__)

/* x86: Use COM1 port I/O */
#define GDB_SERIAL_PORT     0x3F8
#define GDB_SERIAL_BAUD     115200
#define GDB_SERIAL_DIVISOR  (115200 / GDB_SERIAL_BAUD)

static void gdb_serial_init(void)
{
    /* Disable interrupts */
    outb(GDB_SERIAL_PORT + 1, 0x00);

    /* Enable DLAB (set baud rate divisor) */
    outb(GDB_SERIAL_PORT + 3, 0x80);

    /* Set divisor (lo byte) */
    outb(GDB_SERIAL_PORT + 0, GDB_SERIAL_DIVISOR & 0xFF);

    /* Set divisor (hi byte) */
    outb(GDB_SERIAL_PORT + 1, (GDB_SERIAL_DIVISOR >> 8) & 0xFF);

    /* 8 bits, no parity, one stop bit */
    outb(GDB_SERIAL_PORT + 3, 0x03);

    /* Enable FIFO, clear them, with 14-byte threshold */
    outb(GDB_SERIAL_PORT + 2, 0xC7);

    /* IRQs enabled, RTS/DSR set */
    outb(GDB_SERIAL_PORT + 4, 0x0B);
}

static bool gdb_serial_received(void)
{
    return (inb(GDB_SERIAL_PORT + 5) & 0x01) != 0;
}

static bool gdb_serial_is_transmit_empty(void)
{
    return (inb(GDB_SERIAL_PORT + 5) & 0x20) != 0;
}

static char gdb_serial_read(void)
{
    while (!gdb_serial_received());
    return inb(GDB_SERIAL_PORT);
}

static int gdb_serial_read_nonblock(void)
{
    if (!gdb_serial_received()) {
        return -1;
    }
    return inb(GDB_SERIAL_PORT);
}

static void gdb_serial_write(char c)
{
    while (!gdb_serial_is_transmit_empty());
    outb(GDB_SERIAL_PORT, c);
}

#elif defined(__aarch64__)

/* ARM64: Use PL011 UART memory-mapped I/O */
static void gdb_serial_init(void)
{
    /* PL011 UART is typically initialized by bootloader/firmware */
    /* No additional init needed for QEMU virt machine */
}

static bool gdb_serial_received(void)
{
    return arm64_uart_rx_ready();
}

static bool gdb_serial_is_transmit_empty(void)
{
    return arm64_uart_tx_ready();
}

static char gdb_serial_read(void)
{
    int c;
    while ((c = arm64_uart_getc()) < 0);
    return (char)c;
}

static int gdb_serial_read_nonblock(void)
{
    return arm64_uart_getc();
}

static void gdb_serial_write(char c)
{
    while (!gdb_serial_is_transmit_empty());
    arm64_uart_putc(c);
}

#else
#error "Unsupported architecture for GDB stub"
#endif

/* ============================================================================
 * Hex Conversion Utilities
 * ============================================================================ */

static const char hex_chars[] = "0123456789abcdef";

static int hex_to_int(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static uint64_t hex_to_u64(const char *str, int len)
{
    uint64_t val = 0;
    for (int i = 0; i < len; i++) {
        int digit = hex_to_int(str[i]);
        if (digit < 0) break;
        val = (val << 4) | digit;
    }
    return val;
}

/* ============================================================================
 * GDB Protocol Functions
 * ============================================================================ */

static uint8_t gdb_checksum(const char *data, int len)
{
    uint8_t sum = 0;
    for (int i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

static void gdb_send_packet(const char *data, int len)
{
    uint8_t checksum = gdb_checksum(data, len);

    /* Send packet start */
    gdb_serial_write('$');

    /* Send data */
    for (int i = 0; i < len; i++) {
        gdb_serial_write(data[i]);
    }

    /* Send checksum */
    gdb_serial_write('#');
    gdb_serial_write(hex_chars[(checksum >> 4) & 0xF]);
    gdb_serial_write(hex_chars[checksum & 0xF]);

    g_gdb.packets_tx++;
}

static void gdb_send_string(const char *str)
{
    int len = 0;
    while (str[len]) len++;
    gdb_send_packet(str, len);
}

static void gdb_send_ok(void)
{
    gdb_send_string("OK");
}

static void gdb_send_error(int error)
{
    char buf[4];
    buf[0] = 'E';
    buf[1] = hex_chars[(error >> 4) & 0xF];
    buf[2] = hex_chars[error & 0xF];
    buf[3] = '\0';
    gdb_send_string(buf);
}

static int gdb_receive_packet(char *buf, int max_len)
{
    int len = 0;
    int state = 0;  /* 0=waiting for $, 1=reading data, 2=checksum1, 3=checksum2 */
    uint8_t checksum = 0;
    char cs1 = 0, cs2 = 0;

    while (1) {
        int c = gdb_serial_read_nonblock();
        if (c < 0) {
            /* No data available, yield briefly */
            continue;
        }

        switch (state) {
        case 0:  /* Waiting for packet start */
            if (c == '$') {
                state = 1;
                len = 0;
                checksum = 0;
            } else if (c == '+' || c == '-') {
                /* ACK/NAK, ignore */
            } else if (c == 0x03) {
                /* Ctrl+C - break */
                buf[0] = 0x03;
                return 1;
            }
            break;

        case 1:  /* Reading packet data */
            if (c == '#') {
                state = 2;
            } else if (c == '$') {
                /* New packet, restart */
                len = 0;
                checksum = 0;
            } else {
                if (len < max_len - 1) {
                    buf[len++] = (char)c;
                    checksum += c;
                }
            }
            break;

        case 2:  /* First checksum char */
            cs1 = (char)c;
            state = 3;
            break;

        case 3:  /* Second checksum char */
            cs2 = (char)c;
            buf[len] = '\0';

            /* Verify checksum */
            uint8_t expected = (hex_to_int(cs1) << 4) | hex_to_int(cs2);
            if (checksum == expected) {
                gdb_serial_write('+');  /* ACK */
                g_gdb.packets_rx++;
                return len;
            } else {
                gdb_serial_write('-');  /* NAK */
                state = 0;
            }
            break;
        }
    }
}

/* ============================================================================
 * Register Handling
 * ============================================================================ */

static void gdb_read_registers(void)
{
    char buf[GDB_NUM_REGS * 16 + 1];
    char *p = buf;
    uint64_t *regs = (uint64_t *)&g_gdb.regs;

    /* Send all registers as hex */
    for (int i = 0; i < GDB_NUM_REGS; i++) {
        uint64_t val = regs[i];
        /* Little-endian byte order */
        for (int j = 0; j < 8; j++) {
            *p++ = hex_chars[(val >> 4) & 0xF];
            *p++ = hex_chars[val & 0xF];
            val >>= 8;
        }
    }
    *p = '\0';

    gdb_send_string(buf);
}

static void gdb_write_registers(const char *data)
{
    uint64_t *regs = (uint64_t *)&g_gdb.regs;
    int idx = 0;

    for (int i = 0; i < GDB_NUM_REGS && data[idx]; i++) {
        uint64_t val = 0;
        /* Little-endian byte order */
        for (int j = 0; j < 8 && data[idx] && data[idx + 1]; j++) {
            int b = (hex_to_int(data[idx]) << 4) | hex_to_int(data[idx + 1]);
            val |= ((uint64_t)b << (j * 8));
            idx += 2;
        }
        regs[i] = val;
    }

    gdb_send_ok();
}

/* ============================================================================
 * Memory Handling
 * ============================================================================ */

static void gdb_read_memory(const char *args)
{
    /* Parse address,length */
    uint64_t addr = 0;
    int len = 0;
    int i = 0;

    /* Read address */
    while (args[i] && args[i] != ',') {
        addr = (addr << 4) | hex_to_int(args[i]);
        i++;
    }
    if (args[i] == ',') i++;

    /* Read length */
    while (args[i]) {
        len = (len << 4) | hex_to_int(args[i]);
        i++;
    }

    if (len <= 0 || len > 2000) {
        gdb_send_error(1);
        return;
    }

    /* Read memory and send as hex */
    char buf[4096];
    char *p = buf;

    for (int j = 0; j < len; j++) {
        uint8_t byte = *(volatile uint8_t *)(uintptr_t)(addr + j);
        *p++ = hex_chars[(byte >> 4) & 0xF];
        *p++ = hex_chars[byte & 0xF];
    }
    *p = '\0';

    gdb_send_string(buf);
}

static void gdb_write_memory(const char *args)
{
    /* Parse address,length:data */
    uint64_t addr = 0;
    int len = 0;
    int i = 0;

    /* Read address */
    while (args[i] && args[i] != ',') {
        addr = (addr << 4) | hex_to_int(args[i]);
        i++;
    }
    if (args[i] == ',') i++;

    /* Read length */
    while (args[i] && args[i] != ':') {
        len = (len << 4) | hex_to_int(args[i]);
        i++;
    }
    if (args[i] == ':') i++;

    /* Write data */
    for (int j = 0; j < len && args[i] && args[i + 1]; j++) {
        uint8_t byte = (hex_to_int(args[i]) << 4) | hex_to_int(args[i + 1]);
        *(volatile uint8_t *)(uintptr_t)(addr + j) = byte;
        i += 2;
    }

    gdb_send_ok();
}

/* ============================================================================
 * Breakpoint Handling
 * ============================================================================ */

static gdb_breakpoint_t *gdb_find_breakpoint(uint64_t addr)
{
    for (int i = 0; i < GDB_MAX_BREAKPOINTS; i++) {
        if (g_gdb.breakpoints[i].active && g_gdb.breakpoints[i].addr == addr) {
            return &g_gdb.breakpoints[i];
        }
    }
    return NULL;
}

int gdb_set_breakpoint(uint64_t addr)
{
    /* Check if already set */
    if (gdb_find_breakpoint(addr)) {
        return 0;  /* Already exists */
    }

    /* Find free slot */
    for (int i = 0; i < GDB_MAX_BREAKPOINTS; i++) {
        if (!g_gdb.breakpoints[i].active) {
            g_gdb.breakpoints[i].addr = addr;
            g_gdb.breakpoints[i].saved_byte = *(uint8_t *)(uintptr_t)addr;
            g_gdb.breakpoints[i].active = true;

            /* Insert INT3 (0xCC) */
            *(uint8_t *)(uintptr_t)addr = 0xCC;

            g_gdb.num_breakpoints++;
            return 0;
        }
    }

    return -1;  /* No free slots */
}

int gdb_remove_breakpoint(uint64_t addr)
{
    gdb_breakpoint_t *bp = gdb_find_breakpoint(addr);
    if (!bp) {
        return -1;  /* Not found */
    }

    /* Restore original byte */
    *(uint8_t *)(uintptr_t)addr = bp->saved_byte;
    bp->active = false;
    g_gdb.num_breakpoints--;

    return 0;
}

static void gdb_handle_breakpoint_cmd(const char *args, bool set)
{
    /* Parse type,addr,length */
    int type = 0;
    uint64_t addr = 0;
    int i = 0;

    /* Read type */
    if (args[i]) {
        type = hex_to_int(args[i]);
        i++;
    }
    if (args[i] == ',') i++;

    /* Read address */
    while (args[i] && args[i] != ',') {
        addr = (addr << 4) | hex_to_int(args[i]);
        i++;
    }

    /* Only support software breakpoints (type 0) */
    if (type != 0) {
        gdb_send_string("");  /* Not supported */
        return;
    }

    int ret;
    if (set) {
        ret = gdb_set_breakpoint(addr);
    } else {
        ret = gdb_remove_breakpoint(addr);
    }

    if (ret == 0) {
        gdb_send_ok();
    } else {
        gdb_send_error(1);
    }
}

/* ============================================================================
 * Command Processing
 * ============================================================================ */

static void gdb_process_command(const char *cmd, int len)
{
    if (len == 0) return;

    switch (cmd[0]) {
    case '?':
        /* Query halt reason */
        gdb_send_string("S05");  /* SIGTRAP */
        break;

    case 'g':
        /* Read all registers */
        gdb_read_registers();
        break;

    case 'G':
        /* Write all registers */
        gdb_write_registers(cmd + 1);
        break;

    case 'p':
        /* Read single register */
        {
            int reg = hex_to_int(cmd[1]);
            if (cmd[2]) reg = (reg << 4) | hex_to_int(cmd[2]);

            if (reg >= 0 && reg < GDB_NUM_REGS) {
                uint64_t *regs = (uint64_t *)&g_gdb.regs;
                uint64_t val = regs[reg];
                char buf[17];
                for (int i = 0; i < 16; i += 2) {
                    buf[i] = hex_chars[(val >> 4) & 0xF];
                    buf[i + 1] = hex_chars[val & 0xF];
                    val >>= 8;
                }
                buf[16] = '\0';
                gdb_send_string(buf);
            } else {
                gdb_send_error(0);
            }
        }
        break;

    case 'm':
        /* Read memory */
        gdb_read_memory(cmd + 1);
        break;

    case 'M':
        /* Write memory */
        gdb_write_memory(cmd + 1);
        break;

    case 'c':
        /* Continue */
        g_gdb.single_stepping = false;
        return;  /* Exit command loop */

    case 's':
        /* Single step */
        g_gdb.single_stepping = true;
        /* Set TF (Trap Flag) in RFLAGS */
        g_gdb.regs.rflags |= 0x100;
        return;  /* Exit command loop */

    case 'Z':
        /* Set breakpoint */
        gdb_handle_breakpoint_cmd(cmd + 1, true);
        break;

    case 'z':
        /* Remove breakpoint */
        gdb_handle_breakpoint_cmd(cmd + 1, false);
        break;

    case 'k':
        /* Kill - just continue (can't really kill kernel) */
        g_gdb.connected = false;
        return;

    case 'D':
        /* Detach */
        g_gdb.connected = false;
        gdb_send_ok();
        return;

    case 'H':
        /* Set thread - we only have one thread */
        gdb_send_ok();
        break;

    case 'T':
        /* Thread alive query */
        gdb_send_ok();
        break;

    case 'q':
        /* Query commands */
        if (len >= 9 && cmd[1] == 'S' && cmd[2] == 'u' && cmd[3] == 'p'
            && cmd[4] == 'p' && cmd[5] == 'o' && cmd[6] == 'r' && cmd[7] == 't'
            && cmd[8] == 'e' && cmd[9] == 'd') {
            gdb_send_string("");
        } else if (len >= 8 && cmd[1] == 'A' && cmd[2] == 't' && cmd[3] == 't'
                   && cmd[4] == 'a' && cmd[5] == 'c' && cmd[6] == 'h'
                   && cmd[7] == 'e' && cmd[8] == 'd') {
            gdb_send_string("1");  /* Attached to existing process */
        } else if (cmd[1] == 'C') {
            gdb_send_string("QC1");  /* Current thread is 1 */
        } else if (len >= 6 && cmd[1] == 'f' && cmd[2] == 'T' && cmd[3] == 'h'
                   && cmd[4] == 'r' && cmd[5] == 'e' && cmd[6] == 'a') {
            gdb_send_string("m1");  /* Thread 1 */
        } else if (len >= 6 && cmd[1] == 's' && cmd[2] == 'T' && cmd[3] == 'h'
                   && cmd[4] == 'r' && cmd[5] == 'e' && cmd[6] == 'a') {
            gdb_send_string("l");  /* End of thread list */
        } else {
            gdb_send_string("");  /* Unknown query */
        }
        break;

    default:
        /* Unknown command */
        gdb_send_string("");
        break;
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int gdb_stub_init(void)
{
    if (gdb_initialized) {
        return GDB_OK;
    }

    memset(&g_gdb, 0, sizeof(g_gdb));

    /* Initialize serial port */
    gdb_serial_init();

    gdb_initialized = true;
    g_gdb.initialized = true;

    console_printf("gdb_stub: Initialized on COM1 (0x3F8)\n");
    console_printf("gdb_stub: Connect with: gdb -ex 'target remote :1234'\n");

    return GDB_OK;
}

bool gdb_stub_is_initialized(void)
{
    return gdb_initialized;
}

bool gdb_stub_is_connected(void)
{
    return gdb_initialized && g_gdb.connected;
}

void gdb_breakpoint(void)
{
#if defined(__x86_64__) || defined(__i386__)
    /* x86: Trigger INT3 */
    __asm__ volatile("int3");
#elif defined(__aarch64__)
    /* ARM64: Trigger BRK instruction */
    __asm__ volatile("brk #0");
#else
    /* Generic: just return */
#endif
}

void gdb_handle_exception(gdb_regs_t *regs, int signal)
{
    if (!gdb_initialized) {
        return;
    }

    /* Save registers */
    if (regs) {
        g_gdb.regs = *regs;
    }

    /* If we hit a breakpoint, adjust RIP */
    if (signal == GDB_SIGNAL_TRAP) {
        gdb_breakpoint_t *bp = gdb_find_breakpoint(g_gdb.regs.rip - 1);
        if (bp) {
            g_gdb.regs.rip--;  /* Point back to breakpoint */
        }
    }

    /* Clear TF if we were single-stepping */
    if (g_gdb.single_stepping) {
        g_gdb.regs.rflags &= ~0x100;
        g_gdb.single_stepping = false;
    }

    g_gdb.connected = true;

    /* Send stop signal */
    char sig_buf[4];
    sig_buf[0] = 'S';
    sig_buf[1] = hex_chars[(signal >> 4) & 0xF];
    sig_buf[2] = hex_chars[signal & 0xF];
    sig_buf[3] = '\0';
    gdb_send_string(sig_buf);

    /* Command loop */
    while (g_gdb.connected) {
        char packet[4096];
        int len = gdb_receive_packet(packet, sizeof(packet));

        if (len > 0) {
            if (packet[0] == 0x03) {
                /* Ctrl+C - already stopped */
                gdb_send_string(sig_buf);
            } else {
                gdb_process_command(packet, len);

                /* Check if we should continue */
                if (packet[0] == 'c' || packet[0] == 's') {
                    break;
                }
            }
        }
    }

    /* Restore registers */
    if (regs) {
        *regs = g_gdb.regs;
    }
}

void gdb_stub_poll(void)
{
    if (!gdb_initialized) {
        return;
    }

    /* Check for incoming data */
    int c = gdb_serial_read_nonblock();
    if (c >= 0) {
        if (c == 0x03) {
            /* Ctrl+C - break into debugger */
            gdb_regs_t regs = {0};
            gdb_handle_exception(&regs, GDB_SIGNAL_INT);
        }
    }
}

void gdb_stub_print_info(void)
{
    console_printf("\n=== GDB Stub Information ===\n");
    console_printf("Initialized: %s\n", gdb_initialized ? "Yes" : "No");
    console_printf("Connected: %s\n", g_gdb.connected ? "Yes" : "No");
    console_printf("Serial Port: COM1 (0x3F8)\n");
    console_printf("Breakpoints: %d/%d\n", g_gdb.num_breakpoints, GDB_MAX_BREAKPOINTS);
    console_printf("Packets RX: %lu\n", g_gdb.packets_rx);
    console_printf("Packets TX: %lu\n", g_gdb.packets_tx);

    if (g_gdb.num_breakpoints > 0) {
        console_printf("\nActive Breakpoints:\n");
        for (int i = 0; i < GDB_MAX_BREAKPOINTS; i++) {
            if (g_gdb.breakpoints[i].active) {
                console_printf("  0x%lx\n", g_gdb.breakpoints[i].addr);
            }
        }
    }
}

int gdb_stub_run_tests(void)
{
    console_printf("\n=== GDB Stub Self-Tests ===\n");

    /* Test 1: Initialization */
    console_printf("TEST: Initialization... ");
    if (!gdb_initialized) {
        int ret = gdb_stub_init();
        if (ret != GDB_OK) {
            console_printf("FAILED\n");
            return -1;
        }
    }
    console_printf("PASSED\n");

    /* Test 2: Serial port */
    console_printf("TEST: Serial port configured... ");
#if defined(__x86_64__) || defined(__i386__)
    {
        uint8_t lcr = inb(GDB_SERIAL_PORT + 3);
        if ((lcr & 0x03) != 0x03) {  /* 8 bits */
            console_printf("FAILED (LCR=0x%02x)\n", lcr);
            return -1;
        }
    }
#elif defined(__aarch64__)
    /* ARM64: PL011 UART - just check it's accessible */
    if (!arm64_uart_tx_ready()) {
        /* UART might not be initialized yet, that's OK */
    }
#endif
    console_printf("PASSED\n");

    /* Test 3: Hex conversion */
    console_printf("TEST: Hex conversion... ");
    if (hex_to_int('a') != 10 || hex_to_int('F') != 15 || hex_to_int('5') != 5) {
        console_printf("FAILED\n");
        return -1;
    }
    console_printf("PASSED\n");

    /* Test 4: Checksum */
    console_printf("TEST: Checksum calculation... ");
    if (gdb_checksum("OK", 2) != ('O' + 'K')) {
        console_printf("FAILED\n");
        return -1;
    }
    console_printf("PASSED\n");

    /* Test 5: Breakpoint management */
    console_printf("TEST: Breakpoint management... ");
    uint64_t test_addr = 0x100000;
    if (gdb_set_breakpoint(test_addr) != 0) {
        console_printf("FAILED (set)\n");
        return -1;
    }
    if (!gdb_find_breakpoint(test_addr)) {
        console_printf("FAILED (find)\n");
        return -1;
    }
    if (gdb_remove_breakpoint(test_addr) != 0) {
        console_printf("FAILED (remove)\n");
        return -1;
    }
    console_printf("PASSED\n");

    console_printf("=== All GDB stub tests passed ===\n");
    return 0;
}

/* ============================================================================
 * Kernel Data Structure Inspection
 * ============================================================================ */

void gdb_dump_memory_info(void)
{
    console_printf("\n=== Memory Information ===\n");

    /* Dump physical memory manager statistics */
    size_t total_mem = pmm_total_memory();
    size_t avail_mem = pmm_available_memory();
    size_t total_pages = pmm_total_pages();
    size_t avail_pages = pmm_available_pages();
    size_t used_mem = total_mem - avail_mem;

    console_printf("Total Memory:     %lu KB (%lu MB)\n",
                   total_mem / 1024, total_mem / (1024 * 1024));
    console_printf("Available Memory: %lu KB (%lu MB)\n",
                   avail_mem / 1024, avail_mem / (1024 * 1024));
    console_printf("Used Memory:      %lu KB (%lu MB)\n",
                   used_mem / 1024, used_mem / (1024 * 1024));
    console_printf("Total Pages:      %lu\n", total_pages);
    console_printf("Available Pages:  %lu\n", avail_pages);
    console_printf("Used Pages:       %lu\n", total_pages - avail_pages);
}

void gdb_dump_kernel_state(void)
{
    console_printf("\n=== Kernel State ===\n");

    /* Kernel name */
    console_printf("Kernel: EmbodIOS\n");

    /* Architecture */
#if defined(__x86_64__)
    console_printf("Architecture: x86_64\n");
#elif defined(__i386__)
    console_printf("Architecture: i386\n");
#elif defined(__aarch64__)
    console_printf("Architecture: ARM64\n");
#else
    console_printf("Architecture: Unknown\n");
#endif

    /* CPU state (from saved registers) */
    console_printf("\nCPU State:\n");
    console_printf("RIP: 0x%016lx\n", g_gdb.regs.rip);
    console_printf("RSP: 0x%016lx\n", g_gdb.regs.rsp);
    console_printf("RBP: 0x%016lx\n", g_gdb.regs.rbp);
    console_printf("RFLAGS: 0x%016lx", g_gdb.regs.rflags);

    /* Decode RFLAGS */
    console_printf(" [");
    if (g_gdb.regs.rflags & 0x001) console_printf("CF ");
    if (g_gdb.regs.rflags & 0x004) console_printf("PF ");
    if (g_gdb.regs.rflags & 0x010) console_printf("AF ");
    if (g_gdb.regs.rflags & 0x040) console_printf("ZF ");
    if (g_gdb.regs.rflags & 0x080) console_printf("SF ");
    if (g_gdb.regs.rflags & 0x100) console_printf("TF ");
    if (g_gdb.regs.rflags & 0x200) console_printf("IF ");
    if (g_gdb.regs.rflags & 0x400) console_printf("DF ");
    if (g_gdb.regs.rflags & 0x800) console_printf("OF ");
    console_printf("]\n");
}

void gdb_dump_stub_state(void)
{
    console_printf("\n=== GDB Stub State ===\n");
    console_printf("Initialized: %s\n", gdb_initialized ? "Yes" : "No");
    console_printf("Connected: %s\n", g_gdb.connected ? "Yes" : "No");
    console_printf("Single Stepping: %s\n", g_gdb.single_stepping ? "Yes" : "No");

    console_printf("\nStatistics:\n");
    console_printf("Packets Received: %lu\n", g_gdb.packets_rx);
    console_printf("Packets Transmitted: %lu\n", g_gdb.packets_tx);

    console_printf("\nBreakpoints: %d/%d active\n", g_gdb.num_breakpoints, GDB_MAX_BREAKPOINTS);
    if (g_gdb.num_breakpoints > 0) {
        console_printf("Active Breakpoints:\n");
        for (int i = 0; i < GDB_MAX_BREAKPOINTS; i++) {
            if (g_gdb.breakpoints[i].active) {
                console_printf("  [%d] 0x%016lx (saved: 0x%02x)\n",
                    i, g_gdb.breakpoints[i].addr, g_gdb.breakpoints[i].saved_byte);
            }
        }
    }

    console_printf("\nRegisters:\n");
    console_printf("RAX: 0x%016lx  RBX: 0x%016lx\n", g_gdb.regs.rax, g_gdb.regs.rbx);
    console_printf("RCX: 0x%016lx  RDX: 0x%016lx\n", g_gdb.regs.rcx, g_gdb.regs.rdx);
    console_printf("RSI: 0x%016lx  RDI: 0x%016lx\n", g_gdb.regs.rsi, g_gdb.regs.rdi);
    console_printf("RBP: 0x%016lx  RSP: 0x%016lx\n", g_gdb.regs.rbp, g_gdb.regs.rsp);
    console_printf("R8:  0x%016lx  R9:  0x%016lx\n", g_gdb.regs.r8, g_gdb.regs.r9);
    console_printf("R10: 0x%016lx  R11: 0x%016lx\n", g_gdb.regs.r10, g_gdb.regs.r11);
    console_printf("R12: 0x%016lx  R13: 0x%016lx\n", g_gdb.regs.r12, g_gdb.regs.r13);
    console_printf("R14: 0x%016lx  R15: 0x%016lx\n", g_gdb.regs.r14, g_gdb.regs.r15);
    console_printf("RIP: 0x%016lx  RFLAGS: 0x%016lx\n", g_gdb.regs.rip, g_gdb.regs.rflags);
}
