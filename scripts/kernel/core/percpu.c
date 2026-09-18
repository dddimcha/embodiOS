/* Per-CPU data structures implementation */
#include <embodios/percpu.h>
#include <embodios/cpu.h>
#include <embodios/console.h>
#include <embodios/mm.h>

/* Per-CPU areas for all CPUs */
static struct percpu_area percpu_areas[MAX_CPUS] __aligned(PERCPU_ALIGN);

/* Number of initialized CPUs */
static uint32_t num_cpus_initialized = 0;

void percpu_init(void)
{
    uint32_t i;

    /* Initialize all per-CPU areas to zero */
    for (i = 0; i < MAX_CPUS; i++) {
        percpu_areas[i].cpu_id = i;
        percpu_areas[i].flags = 0;
        percpu_areas[i].kernel_stack = NULL;
        percpu_areas[i].user_stack = NULL;
        percpu_areas[i].preempt_count = 0;
        percpu_areas[i].current_task = NULL;
        percpu_areas[i].irq_count = 0;
        percpu_areas[i].softirq_count = 0;
    }

    /* Initialize BSP (Bootstrap Processor) */
    percpu_init_cpu(0);

    console_printf("Per-CPU data structures initialized\n");
}

void percpu_init_cpu(uint32_t cpu_id)
{
    if (cpu_id >= MAX_CPUS) {
        console_printf("ERROR: CPU ID %u exceeds MAX_CPUS (%u)\n",
                      cpu_id, MAX_CPUS);
        return;
    }

    /* Initialize this CPU's per-CPU area */
    percpu_areas[cpu_id].cpu_id = cpu_id;
    percpu_areas[cpu_id].flags = 1; /* Mark as initialized */

    /* Allocate kernel stack for this CPU if not BSP */
    if (cpu_id != 0) {
        /* TODO: Allocate kernel stack from PMM */
        /* percpu_areas[cpu_id].kernel_stack = pmm_alloc_pages(4); */
    }

    num_cpus_initialized++;

    console_printf("CPU %u per-CPU area initialized\n", cpu_id);
}

struct percpu_area* percpu_get_area(uint32_t cpu_id)
{
    if (cpu_id >= MAX_CPUS) {
        return NULL;
    }

    return &percpu_areas[cpu_id];
}

struct percpu_area* percpu_get_current_area(void)
{
    uint32_t cpu_id = cpu_get_id();
    return percpu_get_area(cpu_id);
}

void percpu_print_stats(void)
{
    uint32_t i;

    console_printf("\n=== Per-CPU Statistics ===\n");
    console_printf("CPUs initialized: %u\n", num_cpus_initialized);
    console_printf("\n%-4s %-8s %-8s %-8s %-12s\n",
                  "CPU", "Flags", "IRQs", "SoftIRQs", "PreemptCnt");
    console_printf("------------------------------------------------\n");

    for (i = 0; i < MAX_CPUS; i++) {
        if (percpu_areas[i].flags == 0) {
            continue; /* Skip uninitialized CPUs */
        }

        console_printf("%-4u %-8u %-8llu %-8llu %-12llu\n",
                      percpu_areas[i].cpu_id,
                      percpu_areas[i].flags,
                      percpu_areas[i].irq_count,
                      percpu_areas[i].softirq_count,
                      percpu_areas[i].preempt_count);
    }

    console_printf("\n");
}
