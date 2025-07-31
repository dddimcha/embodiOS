/* EMBODIOS Kernel Panic Handler */
#include <embodios/kernel.h>
#include <embodios/console.h>
#include <embodios/cpu.h>

/* External symbols */
extern const char* kernel_version;
extern const char* kernel_build;

/* Panic buffer for debugging */
static char panic_buffer[4096];
static size_t panic_buffer_pos = 0;

/* Add message to panic buffer */
static void panic_log(const char* fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    
    /* Simple vsprintf to panic buffer */
    while (*fmt && panic_buffer_pos < sizeof(panic_buffer) - 1) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
            case 's':
                {
                    const char* s = __builtin_va_arg(args, const char*);
                    while (*s && panic_buffer_pos < sizeof(panic_buffer) - 1) {
                        panic_buffer[panic_buffer_pos++] = *s++;
                    }
                }
                break;
            case 'p':
            case 'x':
                {
                    uintptr_t val = __builtin_va_arg(args, uintptr_t);
                    char hex[17];
                    int i = 0;
                    
                    if (val == 0) {
                        panic_buffer[panic_buffer_pos++] = '0';
                    } else {
                        while (val && i < 16) {
                            int digit = val & 0xF;
                            hex[i++] = digit < 10 ? '0' + digit : 'A' + digit - 10;
                            val >>= 4;
                        }
                        while (i > 0 && panic_buffer_pos < sizeof(panic_buffer) - 1) {
                            panic_buffer[panic_buffer_pos++] = hex[--i];
                        }
                    }
                }
                break;
            case 'd':
                {
                    int val = __builtin_va_arg(args, int);
                    char num[12];
                    int i = 0;
                    
                    if (val < 0) {
                        panic_buffer[panic_buffer_pos++] = '-';
                        val = -val;
                    }
                    
                    if (val == 0) {
                        panic_buffer[panic_buffer_pos++] = '0';
                    } else {
                        while (val && i < 11) {
                            num[i++] = '0' + (val % 10);
                            val /= 10;
                        }
                        while (i > 0 && panic_buffer_pos < sizeof(panic_buffer) - 1) {
                            panic_buffer[panic_buffer_pos++] = num[--i];
                        }
                    }
                }
                break;
            default:
                panic_buffer[panic_buffer_pos++] = '%';
                panic_buffer[panic_buffer_pos++] = *fmt;
                break;
            }
            fmt++;
        } else {
            panic_buffer[panic_buffer_pos++] = *fmt++;
        }
    }
    
    panic_buffer[panic_buffer_pos] = '\0';
    __builtin_va_end(args);
}

/* Stack trace helper */
static void dump_stack_trace(void)
{
    void** frame;
    
#ifdef __x86_64__
    __asm__ volatile("mov %%rbp, %0" : "=r"(frame));
#elif defined(__aarch64__)
    __asm__ volatile("mov %0, x29" : "=r"(frame));
#endif
    
    console_printf("\nStack trace:\n");
    
    for (int i = 0; i < 16 && frame; i++) {
        void* ret_addr = frame[1];
        console_printf("  [%d] %p\n", i, ret_addr);
        
        /* Move to next frame */
        frame = (void**)*frame;
        
        /* Basic sanity check */
        if ((uintptr_t)frame < 0x1000 || (uintptr_t)frame > 0xFFFFFFFFFFFF0000UL) {
            break;
        }
    }
}

/* Main kernel panic handler */
void kernel_panic(const char* msg, ...)
{
    /* Disable interrupts immediately */
    arch_disable_interrupts();
    
    /* Clear screen and set panic colors */
    console_clear();
    console_set_color(COLOR_WHITE, COLOR_RED);
    
    /* Print panic header */
    console_printf("\n");
    console_printf("================================================================================\n");
    console_printf("                            EMBODIOS KERNEL PANIC                               \n");
    console_printf("================================================================================\n");
    console_printf("\n");
    
    /* Print panic message */
    __builtin_va_list args;
    __builtin_va_start(args, msg);
    
    panic_log("PANIC: ");
    panic_log(msg, args);
    panic_log("\n");
    
    __builtin_va_end(args);
    
    /* Output panic buffer */
    console_printf("%s", panic_buffer);
    
    /* Print system information */
    console_printf("\nSystem Information:\n");
    console_printf("  Kernel Version: %s\n", kernel_version);
    console_printf("  Build Date: %s\n", kernel_build);
    console_printf("  CPU: %s\n", cpu_get_info()->model);
    console_printf("  Timestamp: %llu\n", cpu_get_timestamp());
    
    /* Dump stack trace */
    dump_stack_trace();
    
    /* Print registers */
    console_printf("\nCPU Registers:\n");
    
#ifdef __x86_64__
    uint64_t rsp, rbp, rip;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    __asm__ volatile("lea (%%rip), %0" : "=r"(rip));
    
    console_printf("  RSP: %p  RBP: %p  RIP: %p\n", rsp, rbp, rip);
#elif defined(__aarch64__)
    uint64_t sp, fp, pc;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    __asm__ volatile("mov %0, x29" : "=r"(fp));
    __asm__ volatile("adr %0, ." : "=r"(pc));
    
    console_printf("  SP: %p  FP: %p  PC: %p\n", sp, fp, pc);
#endif
    
    /* Final message */
    console_printf("\n");
    console_printf("================================================================================\n");
    console_printf("System halted. Please restart your computer.\n");
    console_printf("================================================================================\n");
    
    /* Halt the system */
    while (1) {
        arch_halt();
    }
}