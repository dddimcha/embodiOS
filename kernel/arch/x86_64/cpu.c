#include "embodios/types.h"
#include "embodios/cpu.h"
#include "embodios/console.h"

void cpu_init(void)
{
    console_printf("x86_64 CPU initialized\n");
}

void arch_cpu_init(void)
{
    cpu_init();
}

void cpu_halt(void)
{
    __asm__ volatile("hlt");
}

void arch_halt(void)
{
    while (1) __asm__ volatile("hlt");
}

void cpu_enable_interrupts(void)
{
    __asm__ volatile("sti");
}

void arch_enable_interrupts(void)
{
    cpu_enable_interrupts();
}

void cpu_disable_interrupts(void)
{
    __asm__ volatile("cli");
}

void arch_disable_interrupts(void)
{
    cpu_disable_interrupts();
}

static struct cpu_info x86_64_info = {
    .vendor = "Generic",
    .model = "x86_64 CPU",
    .family = 0,
    .model_id = 0,
    .stepping = 0,
    .features = 0,
    .cores = 1,
    .frequency = 0
};

struct cpu_info* cpu_get_info(void)
{
    return &x86_64_info;
}

uint32_t cpu_get_features(void)
{
    return x86_64_info.features;
}

bool cpu_has_feature(uint32_t feature)
{
    return (x86_64_info.features & feature) != 0;
}

uint32_t cpu_get_id(void)
{
    return 0;
}

uint64_t cpu_get_timestamp(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void arch_reboot(void)
{
    /* Try keyboard controller reset */
    __asm__ volatile("cli");
    uint8_t temp = 0;
    do {
        __asm__ volatile("inb $0x64, %%al" : "=a"(temp));
    } while (temp & 0x02);
    __asm__ volatile("outb %%al, $0x64" : : "a"((uint8_t)0xFE));

    /* If that fails, triple fault */
    __asm__ volatile("lidt 0");
    __asm__ volatile("int $0x03");

    /* Hang if all else fails */
    while (1) __asm__ volatile("hlt");
}
