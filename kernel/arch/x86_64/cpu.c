#include "embodios/types.h"
#include "embodios/cpu.h"
#include "embodios/console.h"

void cpu_init(void)
{
    console_printf("x86_64 CPU initialized\n");
}

void cpu_halt(void)
{
    __asm__ volatile("hlt");
}

void cpu_enable_interrupts(void)
{
    __asm__ volatile("sti");
}

void cpu_disable_interrupts(void)
{
    __asm__ volatile("cli");
}
