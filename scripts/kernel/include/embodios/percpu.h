#ifndef EMBODIOS_PERCPU_H
#define EMBODIOS_PERCPU_H

#include <embodios/types.h>

/* Maximum number of CPUs supported */
#define MAX_CPUS 256

/* Per-CPU data section alignment */
#define PERCPU_ALIGN 64

/* Define a per-CPU variable */
#define DEFINE_PER_CPU(type, name) \
    __attribute__((section(".percpu"))) \
    __aligned(PERCPU_ALIGN) \
    type name[MAX_CPUS]

/* Declare a per-CPU variable defined elsewhere */
#define DECLARE_PER_CPU(type, name) \
    extern type name[MAX_CPUS]

/* Get pointer to per-CPU variable for specific CPU */
#define per_cpu_ptr(var, cpu) (&(var)[(cpu)])

/* Get pointer to per-CPU variable for current CPU */
#define this_cpu_ptr(var) per_cpu_ptr(var, cpu_get_id())

/* Read per-CPU variable for specific CPU */
#define per_cpu(var, cpu) ((var)[(cpu)])

/* Read per-CPU variable for current CPU */
#define this_cpu_read(var) per_cpu(var, cpu_get_id())

/* Write per-CPU variable for current CPU */
#define this_cpu_write(var, val) do { \
    (var)[cpu_get_id()] = (val); \
} while (0)

/* Per-CPU area structure */
struct percpu_area {
    uint32_t cpu_id;
    uint32_t flags;
    void* kernel_stack;
    void* user_stack;
    uint64_t preempt_count;
    void* current_task;
    uint64_t irq_count;
    uint64_t softirq_count;
};

/* Per-CPU initialization */
void percpu_init(void);
void percpu_init_cpu(uint32_t cpu_id);

/* Per-CPU area access */
struct percpu_area* percpu_get_area(uint32_t cpu_id);
struct percpu_area* percpu_get_current_area(void);

/* Per-CPU statistics */
void percpu_print_stats(void);

#endif /* EMBODIOS_PERCPU_H */
