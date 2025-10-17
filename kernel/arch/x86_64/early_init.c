#include "embodios/types.h"
#include "embodios/console.h"

void early_init(void)
{
    console_init();
    console_printf("EMBODIOS x86_64 early initialization\n");
}

void arch_early_init(void)
{
    early_init();
}
