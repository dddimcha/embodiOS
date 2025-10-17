#include "embodios/types.h"
#include "embodios/console.h"

void paging_init(void)
{
    console_printf("Paging: using identity mapping from bootloader\n");
}
