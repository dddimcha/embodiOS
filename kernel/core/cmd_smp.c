/* EMBODIOS SMP Shell Commands
 *
 *   cpus                List online CPUs: role (BSP/AP), APIC ID, state,
 *                       mailbox work counters
 *   smpwork [iters]     Multi-core demo: runs the same integer workload on
 *                       every online CPU (BSP + AP mailboxes) and prints
 *                       per-CPU cycles plus the wall-time speedup vs a
 *                       single-core run
 *
 * Author: EMBODIOS Team
 * License: MIT
 */

#include <embodios/cmd_smp.h>
#include <embodios/cpu.h>
#include <embodios/console.h>
#include <embodios/kernel.h>
#include <embodios/mm.h>
#include <embodios/parallel_inference.h>
#include <embodios/types.h>

/* ============================================================================
 * cpus - CPU topology and state
 * ============================================================================ */

static void cmd_cpus(void)
{
    uint32_t online = smp_get_num_online();
    uint32_t detected = smp_num_cpus();

    console_printf("\nCPUs: %u detected, %u online\n", detected, online);
    console_printf("%-4s %-8s %-6s %-14s %-10s %-8s %-8s %s\n",
                   "CPU", "APIC ID", "Role", "State", "WorkItems",
                   "IPI-Wake", "AP-Ticks", "Polls");
    console_printf("--------------------------------------------------------------------------\n");

    for (uint32_t i = 0; i < online; i++) {
        struct smp_cpu_info info;
        if (smp_get_cpu_info(i, &info) != 0) {
            continue;
        }
        const char *state;
        if (!info.online) {
            state = "offline";
        } else if (info.bsp) {
            state = "online";
        } else {
            state = info.parked_if1 ? "parked IF=1" : "polling IF=0";
        }
        console_printf("%-4u %-8u %-6s %-14s %-10llu %-8llu %-8llu %llu\n",
                       info.cpu_id, info.apic_id,
                       info.bsp ? "BSP" : "AP",
                       state,
                       (unsigned long long)info.work_count,
                       (unsigned long long)info.ipi_wakeups,
                       (unsigned long long)info.ap_ticks,
                       (unsigned long long)info.polls);
    }
    console_printf("\n");
}

/* ============================================================================
 * smpwork - parallel work demo across cores
 * ============================================================================ */

/* Shared workload argument: each CPU gets its own result slot */
#define SMPWORK_MAX_CPUS 16

struct smpwork_job {
    uint32_t iterations;
    volatile uint64_t results[SMPWORK_MAX_CPUS];
    volatile uint64_t cycles[SMPWORK_MAX_CPUS];
};

/* Integer-only busy workload (safe on APs: no FP/SSE needed, no allocations) */
static void smpwork_burn(uint32_t iters, uint32_t seed, volatile uint64_t *out)
{
    uint64_t acc = seed;
    for (uint32_t i = 0; i < iters; i++) {
        acc = acc * 6364136223846793005ULL + 1442695040888963407ULL;
        acc ^= acc >> 33;
        __asm__ volatile("" : "+r"(acc));  /* keep the loop honest */
    }
    *out = acc;
}

static struct smpwork_job g_smpwork_job;

static void smpwork_ap_entry(void *arg)
{
    uint32_t cpu = (uint32_t)(uintptr_t)arg;
    uint64_t t0 = cpu_get_timestamp();
    smpwork_burn(g_smpwork_job.iterations, 0x9E3779B97F4A7C15ULL + cpu,
                 &g_smpwork_job.results[cpu]);
    g_smpwork_job.cycles[cpu] = cpu_get_timestamp() - t0;
}

static void cmd_smpwork(const char *args)
{
    uint32_t iters = 2000000;   /* default: ~few ms per CPU */
    if (args && *args) {
        uint32_t v = 0;
        const char *p = args;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (uint32_t)(*p - '0');
            p++;
        }
        if (v > 0) iters = v;
    }

    uint32_t online = smp_get_num_online();
    if (online < 2) {
        console_printf("smpwork: only %u CPU online - nothing to distribute\n",
                       online);
        return;
    }
    if (online > SMPWORK_MAX_CPUS) online = SMPWORK_MAX_CPUS;

    g_smpwork_job.iterations = iters;

    /* --- Serial baseline: everything on the BSP --- */
    uint64_t t0 = cpu_get_timestamp();
    for (uint32_t i = 0; i < online; i++) {
        smpwork_burn(iters, 0x9E3779B97F4A7C15ULL + i, &g_smpwork_job.results[i]);
    }
    uint64_t serial_cycles = cpu_get_timestamp() - t0;

    /* --- Parallel run: dispatch to AP mailboxes, BSP runs slice 0 --- */
    t0 = cpu_get_timestamp();
    for (uint32_t i = 1; i < online; i++) {
        if (smp_work_dispatch(i, smpwork_ap_entry, (void *)(uintptr_t)i) != 0) {
            console_printf("smpwork: dispatch to CPU %u failed\n", i);
        }
    }
    smpwork_burn(iters, 0x9E3779B97F4A7C15ULL, &g_smpwork_job.results[0]);
    g_smpwork_job.cycles[0] = cpu_get_timestamp() - t0;
    for (uint32_t i = 1; i < online; i++) {
        smp_work_wait(i);
    }
    uint64_t wall_cycles = cpu_get_timestamp() - t0;

    console_printf("\nsmpwork: %u iterations x %u CPUs\n", iters, online);
    for (uint32_t i = 0; i < online; i++) {
        console_printf("  CPU %u (%s): %llu cycles  result=0x%llx\n",
                       i, i == 0 ? "BSP" : "AP",
                       (unsigned long long)g_smpwork_job.cycles[i],
                       (unsigned long long)g_smpwork_job.results[i]);
    }
    console_printf("  serial:   %llu cycles (BSP alone)\n",
                   (unsigned long long)serial_cycles);
    console_printf("  parallel: %llu cycles wall\n", (unsigned long long)wall_cycles);
    if (wall_cycles > 0) {
        console_printf("  speedup:  %llu.%02llux\n",
                       (unsigned long long)(serial_cycles / wall_cycles),
                       (unsigned long long)((serial_cycles * 100 / wall_cycles) % 100));
    }
    console_printf("\n");
}

/* ============================================================================
 * smpbench - serial vs parallel matmul through the inference work pool
 * ============================================================================ */

static void smpbench_serial_matmul(float* out, const float* w, const float* x,
                                   int rows, int cols)
{
    for (int r = 0; r < rows; r++) {
        float sum = 0.0f;
        const float* row = w + r * cols;
        for (int c = 0; c < cols; c++) {
            sum += row[c] * x[c];
        }
        out[r] = sum;
    }
}

static void cmd_smpbench(void)
{
    /* SmolLM-135M-ish layer dims (hidden x dim FFN projection) */
    const int rows = 1536, cols = 576;
    const int passes = 8;

    float *w = (float *)kmalloc((size_t)rows * cols * sizeof(float));
    float *x = (float *)kmalloc((size_t)cols * sizeof(float));
    float *out = (float *)kmalloc((size_t)rows * sizeof(float));
    if (!w || !x || !out) {
        console_printf("smpbench: allocation failed\n");
        if (w) kfree(w);
        if (x) kfree(x);
        if (out) kfree(out);
        return;
    }

    /* Deterministic fill (xorshift-ish, values in [-0.5, 0.5]) */
    uint64_t s = 0x243F6A8885A308D3ULL;
    for (int i = 0; i < rows * cols; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        w[i] = ((float)(int)((s >> 40) & 0xFFFF) / 65536.0f) - 0.5f;
    }
    for (int i = 0; i < cols; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        x[i] = ((float)(int)((s >> 40) & 0xFFFF) / 65536.0f) - 0.5f;
    }

    /* Serial: all passes on the BSP */
    uint64_t t0 = cpu_get_timestamp();
    for (int p = 0; p < passes; p++) {
        smpbench_serial_matmul(out, w, x, rows, cols);
    }
    uint64_t serial_cycles = cpu_get_timestamp() - t0;
    float serial_sum = out[rows - 1];

    /* Parallel: same work through parallel_matmul_f32 (worker pool) */
    parallel_init(4);   /* no-op if the inference engine already set it up */
    t0 = cpu_get_timestamp();
    for (int p = 0; p < passes; p++) {
        parallel_matmul_f32(out, w, x, rows, cols);
    }
    uint64_t par_cycles = cpu_get_timestamp() - t0;
    float par_sum = out[rows - 1];

    console_printf("\nsmpbench: matmul [%d x %d] x %d passes, %d pool threads\n",
                   rows, cols, passes, parallel_get_num_threads());
    console_printf("  serial:   %llu cycles\n", (unsigned long long)serial_cycles);
    console_printf("  parallel: %llu cycles\n", (unsigned long long)par_cycles);
    if (par_cycles > 0) {
        console_printf("  speedup:  %llu.%02llux\n",
                       (unsigned long long)(serial_cycles / par_cycles),
                       (unsigned long long)((serial_cycles * 100 / par_cycles) % 100));
    }
    console_printf("  result check: serial=%f parallel=%f %s\n",
                   (double)serial_sum, (double)par_sum,
                   (serial_sum == par_sum) ? "(identical)" : "(differs!)");

    parallel_print_core_stats();

    kfree(w);
    kfree(x);
    kfree(out);
}

/* ============================================================================
 * Dispatch
 * ============================================================================ */

int cmd_smp_dispatch(const char* command)
{
    if (!command) {
        return 0;
    }
    if (strcmp(command, "cpus") == 0) {
        cmd_cpus();
        return 1;
    }
    if (strncmp(command, "smpwork", 7) == 0 &&
        (command[7] == '\0' || command[7] == ' ')) {
        cmd_smpwork(command + 7);
        return 1;
    }
    if (strcmp(command, "smpbench") == 0) {
        cmd_smpbench();
        return 1;
    }
    return 0;
}
