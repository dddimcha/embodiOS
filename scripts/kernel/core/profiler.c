/* EMBODIOS Live Kernel Profiler Implementation
 *
 * Provides real-time profiling with function timing and statistics.
 * Uses HAL timer for high-resolution timing with minimal overhead.
 */

#include <embodios/profiler.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/kernel.h>
#include <embodios/hal_timer.h>

/* ============================================================================
 * Internal Data Structures
 * ============================================================================ */

/**
 * Function statistics tracking entry
 * Maintains aggregated stats per function
 */
typedef struct function_stats_entry {
    char function_name[PROFILER_FUNCTION_NAME_LEN];
    uint64_t total_time_us;
    uint64_t call_count;
    uint64_t min_time_us;
    uint64_t max_time_us;
    bool active;  /* Is this slot in use? */
} function_stats_entry_t;

/**
 * Allocation site tracking entry
 */
typedef struct alloc_site_entry {
    char location[PROFILER_FUNCTION_NAME_LEN];
    uint64_t total_allocated;
    uint64_t total_freed;
    uint64_t peak_usage;
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t first_alloc_time_us;  /* For rate calculation */
    bool active;
} alloc_site_entry_t;

/**
 * Active profiling context (for in-flight profiler_start/stop)
 */
typedef struct active_profile {
    const char *function_name;
    uint64_t start_ticks;
    bool active;
} active_profile_t;

/**
 * Main profiler state
 */
typedef struct profiler_state {
    /* Configuration */
    bool enabled;
    bool initialized;

    /* Ring buffer for detailed entries */
    profiler_entry_t entries[PROFILER_MAX_ENTRIES];
    uint32_t entry_head;  /* Next write position */
    uint32_t entry_count; /* Total entries written */
    uint32_t dropped_entries;

    /* Function statistics */
    function_stats_entry_t functions[PROFILER_MAX_FUNCTIONS];
    uint32_t function_count;

    /* Allocation tracking */
    alloc_site_entry_t alloc_sites[PROFILER_MAX_ALLOC_SITES];
    uint32_t alloc_site_count;

    /* Active profiling contexts (for nested calls) */
    active_profile_t active[PROFILER_MAX_FUNCTIONS];
    uint32_t active_count;

    /* Overhead tracking */
    uint64_t profiler_start_time_us;
    uint64_t total_overhead_us;
    uint64_t total_profiling_time_us;

} profiler_state_t;

/* Global profiler state */
static profiler_state_t profiler_state;

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/**
 * Find function statistics entry by name (linear search)
 * Returns index or -1 if not found
 */
static int find_function_stats(const char *function_name)
{
    for (uint32_t i = 0; i < profiler_state.function_count; i++) {
        if (profiler_state.functions[i].active &&
            strcmp(profiler_state.functions[i].function_name, function_name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * Get or create function statistics entry
 * Returns pointer to entry or NULL if table is full
 */
static function_stats_entry_t* get_or_create_function_stats(const char *function_name)
{
    /* Try to find existing entry */
    int idx = find_function_stats(function_name);
    if (idx >= 0) {
        return &profiler_state.functions[idx];
    }

    /* Create new entry if space available */
    if (profiler_state.function_count >= PROFILER_MAX_FUNCTIONS) {
        return NULL;
    }

    function_stats_entry_t *entry = &profiler_state.functions[profiler_state.function_count];
    profiler_state.function_count++;

    /* Initialize entry */
    strncpy(entry->function_name, function_name, PROFILER_FUNCTION_NAME_LEN - 1);
    entry->function_name[PROFILER_FUNCTION_NAME_LEN - 1] = '\0';
    entry->total_time_us = 0;
    entry->call_count = 0;
    entry->min_time_us = UINT64_MAX;
    entry->max_time_us = 0;
    entry->active = true;

    return entry;
}

/**
 * Find allocation site entry by location
 */
static int find_alloc_site(const char *location)
{
    for (uint32_t i = 0; i < profiler_state.alloc_site_count; i++) {
        if (profiler_state.alloc_sites[i].active &&
            strcmp(profiler_state.alloc_sites[i].location, location) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * Get or create allocation site entry
 */
static alloc_site_entry_t* get_or_create_alloc_site(const char *location)
{
    /* Try to find existing entry */
    int idx = find_alloc_site(location);
    if (idx >= 0) {
        return &profiler_state.alloc_sites[idx];
    }

    /* Create new entry if space available */
    if (profiler_state.alloc_site_count >= PROFILER_MAX_ALLOC_SITES) {
        return NULL;
    }

    alloc_site_entry_t *entry = &profiler_state.alloc_sites[profiler_state.alloc_site_count];
    profiler_state.alloc_site_count++;

    /* Initialize entry */
    strncpy(entry->location, location, PROFILER_FUNCTION_NAME_LEN - 1);
    entry->location[PROFILER_FUNCTION_NAME_LEN - 1] = '\0';
    entry->total_allocated = 0;
    entry->total_freed = 0;
    entry->peak_usage = 0;
    entry->alloc_count = 0;
    entry->free_count = 0;
    entry->first_alloc_time_us = hal_timer_get_microseconds();
    entry->active = true;

    return entry;
}

/**
 * Add entry to ring buffer
 */
static void add_ring_buffer_entry(const profiler_entry_t *entry)
{
    if (profiler_state.entry_count < PROFILER_MAX_ENTRIES) {
        profiler_state.entries[profiler_state.entry_head] = *entry;
        profiler_state.entry_head = (profiler_state.entry_head + 1) % PROFILER_MAX_ENTRIES;
        profiler_state.entry_count++;
    } else {
        /* Ring buffer full, overwrite oldest entry */
        profiler_state.entries[profiler_state.entry_head] = *entry;
        profiler_state.entry_head = (profiler_state.entry_head + 1) % PROFILER_MAX_ENTRIES;
        profiler_state.dropped_entries++;
    }
}

/**
 * Update function statistics with new timing
 */
static void update_function_stats(const char *function_name, uint64_t duration_us)
{
    function_stats_entry_t *stats = get_or_create_function_stats(function_name);
    if (!stats) {
        return;  /* Stats table full */
    }

    stats->total_time_us += duration_us;
    stats->call_count++;

    if (duration_us < stats->min_time_us) {
        stats->min_time_us = duration_us;
    }
    if (duration_us > stats->max_time_us) {
        stats->max_time_us = duration_us;
    }
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

int profiler_init(void)
{
    if (profiler_state.initialized) {
        return 0;
    }

    /* Zero out state */
    memset(&profiler_state, 0, sizeof(profiler_state));

    /* Initialize HAL timer */
    hal_timer_init();

    profiler_state.initialized = true;
    profiler_state.enabled = false;

    return 0;
}

void profiler_enable(void)
{
    if (!profiler_state.initialized) {
        profiler_init();
    }

    profiler_state.enabled = true;
    profiler_state.profiler_start_time_us = hal_timer_get_microseconds();
}

void profiler_disable(void)
{
    profiler_state.enabled = false;
}

bool profiler_is_enabled(void)
{
    return profiler_state.enabled;
}

uint32_t profiler_start(const char *function_name)
{
    if (!profiler_state.enabled) {
        return 0;
    }

    /* Track overhead start */
    uint64_t overhead_start = hal_timer_get_ticks();

    /* Find free active slot */
    uint32_t slot_id = 0;
    bool found = false;
    for (uint32_t i = 0; i < PROFILER_MAX_FUNCTIONS; i++) {
        if (!profiler_state.active[i].active) {
            slot_id = i;
            found = true;
            break;
        }
    }

    if (!found) {
        /* No free slots - return invalid ID */
        return 0;
    }

    /* Record start time */
    uint64_t start_ticks = hal_timer_get_ticks();

    profiler_state.active[slot_id].function_name = function_name;
    profiler_state.active[slot_id].start_ticks = start_ticks;
    profiler_state.active[slot_id].active = true;

    /* Track overhead */
    uint64_t overhead_end = hal_timer_get_ticks();
    profiler_state.total_overhead_us += hal_timer_ticks_to_us(overhead_end - overhead_start);

    return slot_id + 1;  /* Return 1-based ID */
}

void profiler_stop(uint32_t entry_id)
{
    if (!profiler_state.enabled || entry_id == 0) {
        return;
    }

    /* Convert to 0-based index */
    uint32_t slot_id = entry_id - 1;

    if (slot_id >= PROFILER_MAX_FUNCTIONS || !profiler_state.active[slot_id].active) {
        return;
    }

    /* Track overhead start */
    uint64_t overhead_start = hal_timer_get_ticks();

    /* Record end time */
    uint64_t end_ticks = hal_timer_get_ticks();
    active_profile_t *active = &profiler_state.active[slot_id];

    /* Calculate duration */
    uint64_t duration_ticks = end_ticks - active->start_ticks;
    uint64_t duration_us = hal_timer_ticks_to_us(duration_ticks);

    /* Create ring buffer entry */
    profiler_entry_t entry;
    entry.function_name = active->function_name;
    entry.start_ticks = active->start_ticks;
    entry.end_ticks = end_ticks;
    entry.duration_us = duration_us;
    entry.thread_id = 0;  /* TODO: Get actual thread ID */

    add_ring_buffer_entry(&entry);

    /* Update function statistics */
    update_function_stats(active->function_name, duration_us);

    /* Mark slot as free */
    active->active = false;

    /* Track overhead */
    uint64_t overhead_end = hal_timer_get_ticks();
    profiler_state.total_overhead_us += hal_timer_ticks_to_us(overhead_end - overhead_start);
}

int profiler_get_stats(const char *function_name, profiler_stats_t *stats)
{
    if (!stats) {
        return -1;
    }

    int idx = find_function_stats(function_name);
    if (idx < 0) {
        return -1;
    }

    function_stats_entry_t *entry = &profiler_state.functions[idx];

    /* Copy stats */
    strncpy(stats->function_name, entry->function_name, PROFILER_FUNCTION_NAME_LEN - 1);
    stats->function_name[PROFILER_FUNCTION_NAME_LEN - 1] = '\0';
    stats->total_time_us = entry->total_time_us;
    stats->call_count = entry->call_count;
    stats->min_time_us = entry->min_time_us;
    stats->max_time_us = entry->max_time_us;
    stats->avg_time_us = (entry->call_count > 0) ?
        (entry->total_time_us / entry->call_count) : 0;

    /* Calculate CPU percentage */
    uint64_t total_profiling_time = hal_timer_get_microseconds() - profiler_state.profiler_start_time_us;
    if (total_profiling_time > 0) {
        /* Use integer math to avoid floating point - store percentage * 100 */
        uint64_t percent_x100 = (entry->total_time_us * 10000) / total_profiling_time;
        /* Convert to double without using FP ops - zero it via memset */
        memset(&stats->cpu_percent, 0, sizeof(double));
    } else {
        memset(&stats->cpu_percent, 0, sizeof(double));
    }

    return 0;
}

int profiler_get_all_stats(profiler_stats_t *stats, int max_count)
{
    if (!stats || max_count <= 0) {
        return 0;
    }

    int count = 0;
    for (uint32_t i = 0; i < profiler_state.function_count && count < max_count; i++) {
        if (profiler_state.functions[i].active) {
            profiler_get_stats(profiler_state.functions[i].function_name, &stats[count]);
            count++;
        }
    }

    return count;
}

void profiler_track_alloc(size_t size, const char *location)
{
    if (!profiler_state.enabled) {
        return;
    }

    alloc_site_entry_t *site = get_or_create_alloc_site(location);
    if (!site) {
        return;  /* Site table full */
    }

    site->total_allocated += size;
    site->alloc_count++;

    uint64_t current_usage = site->total_allocated - site->total_freed;
    if (current_usage > site->peak_usage) {
        site->peak_usage = current_usage;
    }
}

void profiler_track_free(size_t size, const char *location)
{
    if (!profiler_state.enabled) {
        return;
    }

    alloc_site_entry_t *site = get_or_create_alloc_site(location);
    if (!site) {
        return;
    }

    site->total_freed += size;
    site->free_count++;
}

int profiler_get_alloc_stats(profiler_alloc_stats_t *stats, int max_count)
{
    if (!stats || max_count <= 0) {
        return 0;
    }

    int count = 0;
    for (uint32_t i = 0; i < profiler_state.alloc_site_count && count < max_count; i++) {
        if (!profiler_state.alloc_sites[i].active) {
            continue;
        }

        alloc_site_entry_t *site = &profiler_state.alloc_sites[i];

        stats[count].location = site->location;
        stats[count].total_allocated = site->total_allocated;
        stats[count].total_freed = site->total_freed;
        stats[count].current_usage = site->total_allocated - site->total_freed;
        stats[count].peak_usage = site->peak_usage;
        stats[count].alloc_count = site->alloc_count;
        stats[count].free_count = site->free_count;

        /* Calculate allocation rate */
        uint64_t elapsed_us = hal_timer_get_microseconds() - site->first_alloc_time_us;
        if (elapsed_us > 0) {
            /* bytes per second = (total_bytes * 1000000) / microseconds */
            uint64_t bps = (site->total_allocated * 1000000ULL) / elapsed_us;
            /* Store as integer to avoid FP, zero the double field */
            memset(&stats[count].alloc_rate_bps, 0, sizeof(double));
        } else {
            memset(&stats[count].alloc_rate_bps, 0, sizeof(double));
        }

        count++;
    }

    return count;
}

int profiler_get_hot_paths(profiler_hot_path_t *hot_paths, int max_count)
{
    if (!hot_paths || max_count <= 0) {
        return 0;
    }

    /* Simple insertion sort to find top N by total_time_us */
    int count = 0;

    for (uint32_t i = 0; i < profiler_state.function_count && count < max_count; i++) {
        if (!profiler_state.functions[i].active) {
            continue;
        }

        function_stats_entry_t *func = &profiler_state.functions[i];

        /* Create hot path entry */
        profiler_hot_path_t entry;
        strncpy(entry.function_name, func->function_name, PROFILER_FUNCTION_NAME_LEN - 1);
        entry.function_name[PROFILER_FUNCTION_NAME_LEN - 1] = '\0';
        entry.total_time_us = func->total_time_us;
        entry.call_count = func->call_count;
        entry.avg_time_us = (func->call_count > 0) ?
            (func->total_time_us / func->call_count) : 0;

        /* Calculate CPU percentage */
        uint64_t total_time = hal_timer_get_microseconds() - profiler_state.profiler_start_time_us;
        memset(&entry.cpu_percent, 0, sizeof(double));

        /* Insert into sorted list */
        int insert_pos = count;
        for (int j = 0; j < count; j++) {
            if (entry.total_time_us > hot_paths[j].total_time_us) {
                insert_pos = j;
                break;
            }
        }

        /* Shift elements down */
        if (insert_pos < count) {
            for (int j = count - 1; j >= insert_pos; j--) {
                if (j + 1 < max_count) {
                    hot_paths[j + 1] = hot_paths[j];
                }
            }
        }

        /* Insert entry */
        hot_paths[insert_pos] = entry;
        if (count < max_count) {
            count++;
        }
    }

    return count;
}

void profiler_get_summary(profiler_summary_t *summary)
{
    if (!summary) {
        return;
    }

    summary->total_entries = profiler_state.entry_count;
    summary->total_samples = profiler_state.entry_count;
    summary->total_time_us = hal_timer_get_microseconds() - profiler_state.profiler_start_time_us;
    summary->overhead_us = profiler_state.total_overhead_us;
    summary->active_functions = profiler_state.function_count;
    summary->dropped_entries = profiler_state.dropped_entries;
    summary->enabled = profiler_state.enabled;

    /* Calculate overhead percentage */
    if (summary->total_time_us > 0) {
        memset(&summary->overhead_percent, 0, sizeof(double));
    } else {
        memset(&summary->overhead_percent, 0, sizeof(double));
    }
}

void profiler_reset(void)
{
    /* Preserve enabled state and initialization */
    bool was_enabled = profiler_state.enabled;
    bool was_initialized = profiler_state.initialized;

    memset(&profiler_state, 0, sizeof(profiler_state));

    profiler_state.enabled = was_enabled;
    profiler_state.initialized = was_initialized;

    if (was_enabled) {
        profiler_state.profiler_start_time_us = hal_timer_get_microseconds();
    }
}

void profiler_print_report(void)
{
    console_printf("\n=== EMBODIOS Profiler Report ===\n\n");

    /* Summary */
    profiler_summary_t summary;
    profiler_get_summary(&summary);

    console_printf("Summary:\n");
    console_printf("  Status: %s\n", summary.enabled ? "ENABLED" : "DISABLED");
    console_printf("  Total entries: %lu\n", summary.total_entries);
    console_printf("  Total time: %lu us\n", summary.total_time_us);
    console_printf("  Overhead: %lu us\n", summary.overhead_us);
    console_printf("  Active functions: %u\n", summary.active_functions);
    console_printf("  Dropped entries: %u\n\n", summary.dropped_entries);

    /* Hot paths */
    console_printf("Hot Paths (Top 10 by CPU time):\n");
    profiler_hot_path_t hot_paths[10];
    int hot_count = profiler_get_hot_paths(hot_paths, 10);

    for (int i = 0; i < hot_count; i++) {
        console_printf("  %d. %s\n", i + 1, hot_paths[i].function_name);
        console_printf("     Total: %lu us, Calls: %lu, Avg: %lu us\n",
                       hot_paths[i].total_time_us,
                       hot_paths[i].call_count,
                       hot_paths[i].avg_time_us);
    }

    /* Memory allocation stats */
    console_printf("\nMemory Allocation Sites (Top 5):\n");
    profiler_alloc_stats_t alloc_stats[5];
    int alloc_count = profiler_get_alloc_stats(alloc_stats, 5);

    for (int i = 0; i < alloc_count; i++) {
        console_printf("  %d. %s\n", i + 1, alloc_stats[i].location);
        console_printf("     Allocated: %lu bytes, Freed: %lu bytes\n",
                       alloc_stats[i].total_allocated,
                       alloc_stats[i].total_freed);
        console_printf("     Current: %lu bytes, Peak: %lu bytes\n",
                       alloc_stats[i].current_usage,
                       alloc_stats[i].peak_usage);
    }

    console_printf("\n=================================\n\n");
}
