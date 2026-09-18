/* EMBODIOS Live Kernel Profiler
 *
 * Real-time profiling infrastructure for function-level CPU timing,
 * memory allocation tracking, and hot path detection during inference.
 *
 * Features:
 * - Function-level timing with microsecond precision
 * - Memory allocation rate and location tracking
 * - Hot path identification
 * - Low overhead (<5% slowdown target)
 * - Ring buffer for profiling entries
 *
 * Design:
 * - Uses hal_timer for high-resolution timing
 * - Compact data structures for minimal memory footprint
 * - Compile-time enable/disable via PROFILING_ENABLED
 */

#ifndef EMBODIOS_PROFILER_H
#define EMBODIOS_PROFILER_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define PROFILER_MAX_ENTRIES        1024    /* Ring buffer size */
#define PROFILER_MAX_FUNCTIONS      256     /* Max tracked functions */
#define PROFILER_MAX_ALLOC_SITES    128     /* Max allocation sites */
#define PROFILER_FUNCTION_NAME_LEN  64      /* Max function name length */

/* ============================================================================
 * Data Structures
 * ============================================================================ */

/**
 * Individual profiling entry for a function call
 * Stored in ring buffer for detailed timing analysis
 */
typedef struct profiler_entry {
    const char *function_name;  /* Function identifier */
    uint64_t start_ticks;       /* Start timestamp (timer ticks) */
    uint64_t end_ticks;         /* End timestamp (timer ticks) */
    uint64_t duration_us;       /* Duration in microseconds */
    uint32_t thread_id;         /* Thread/CPU ID */
} profiler_entry_t;

/**
 * Aggregated statistics per function
 * Tracks min/max/avg timing and call counts
 */
typedef struct profiler_stats {
    char function_name[PROFILER_FUNCTION_NAME_LEN];
    uint64_t total_time_us;     /* Total time spent in function */
    uint64_t call_count;        /* Number of calls */
    uint64_t min_time_us;       /* Minimum call duration */
    uint64_t max_time_us;       /* Maximum call duration */
    uint64_t avg_time_us;       /* Average call duration */
    double cpu_percent;         /* Percentage of total CPU time */
} profiler_stats_t;

/**
 * Memory allocation tracking entry
 * Tracks allocation size, location, and rate
 */
typedef struct profiler_alloc_stats {
    const char *location;       /* Allocation site (file:line or hash) */
    uint64_t total_allocated;   /* Total bytes allocated */
    uint64_t total_freed;       /* Total bytes freed */
    uint64_t current_usage;     /* Current allocated bytes */
    uint64_t peak_usage;        /* Peak allocated bytes */
    uint64_t alloc_count;       /* Number of allocations */
    uint64_t free_count;        /* Number of frees */
    double alloc_rate_bps;      /* Allocation rate (bytes/sec) */
} profiler_alloc_stats_t;

/**
 * Hot path entry - functions consuming most CPU time
 * Sorted by total_time_us for optimization targeting
 */
typedef struct profiler_hot_path {
    char function_name[PROFILER_FUNCTION_NAME_LEN];
    uint64_t total_time_us;     /* Total time in this function */
    uint64_t call_count;        /* Number of calls */
    double cpu_percent;         /* Percentage of total CPU time */
    uint64_t avg_time_us;       /* Average time per call */
} profiler_hot_path_t;

/**
 * Overall profiler state and summary
 */
typedef struct profiler_summary {
    uint64_t total_entries;     /* Total profiling entries recorded */
    uint64_t total_samples;     /* Total samples collected */
    uint64_t total_time_us;     /* Total profiling time */
    uint64_t overhead_us;       /* Profiler overhead time */
    double overhead_percent;    /* Overhead as percentage */
    uint32_t active_functions;  /* Number of tracked functions */
    uint32_t dropped_entries;   /* Entries dropped (buffer full) */
    bool enabled;               /* Profiler active? */
} profiler_summary_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize profiler subsystem
 * Must be called before any profiling operations
 * @return 0 on success, -1 on error
 */
int profiler_init(void);

/**
 * Enable profiling
 * Starts collecting profiling data
 */
void profiler_enable(void);

/**
 * Disable profiling
 * Stops collecting profiling data (data remains available)
 */
void profiler_disable(void);

/**
 * Check if profiler is enabled
 * @return true if profiling is active
 */
bool profiler_is_enabled(void);

/**
 * Start profiling a function
 * Records entry timestamp and function name
 * @param function_name Function identifier (static string)
 * @return Entry ID for matching with profiler_stop()
 */
uint32_t profiler_start(const char *function_name);

/**
 * Stop profiling a function
 * Records exit timestamp and calculates duration
 * @param entry_id Entry ID from profiler_start()
 */
void profiler_stop(uint32_t entry_id);

/**
 * Get aggregated statistics for a specific function
 * @param function_name Function to query
 * @param stats Output statistics structure
 * @return 0 on success, -1 if function not found
 */
int profiler_get_stats(const char *function_name, profiler_stats_t *stats);

/**
 * Get statistics for all tracked functions
 * @param stats Array to fill with statistics
 * @param max_count Maximum number of entries in array
 * @return Number of entries written
 */
int profiler_get_all_stats(profiler_stats_t *stats, int max_count);

/**
 * Track memory allocation
 * Records allocation size and location
 * @param size Number of bytes allocated
 * @param location Allocation site identifier
 */
void profiler_track_alloc(size_t size, const char *location);

/**
 * Track memory deallocation
 * Records freed bytes
 * @param size Number of bytes freed
 * @param location Allocation site identifier
 */
void profiler_track_free(size_t size, const char *location);

/**
 * Get memory allocation statistics
 * @param stats Array to fill with allocation statistics
 * @param max_count Maximum number of entries
 * @return Number of entries written
 */
int profiler_get_alloc_stats(profiler_alloc_stats_t *stats, int max_count);

/**
 * Get hot paths (functions consuming most CPU time)
 * Returns functions sorted by total_time_us descending
 * @param hot_paths Array to fill with hot path data
 * @param max_count Maximum number of entries (top N)
 * @return Number of entries written
 */
int profiler_get_hot_paths(profiler_hot_path_t *hot_paths, int max_count);

/**
 * Get overall profiler summary
 * @param summary Output summary structure
 */
void profiler_get_summary(profiler_summary_t *summary);

/**
 * Reset all profiling data
 * Clears all collected statistics and entries
 */
void profiler_reset(void);

/**
 * Print profiling report to console
 * Shows function statistics and hot paths
 */
void profiler_print_report(void);

/* ============================================================================
 * Convenience Macros
 * ============================================================================ */

#ifdef PROFILING_ENABLED

/* Automatic profiling with scope-based cleanup */
#define PROFILER_START(name) \
    uint32_t __profiler_id_##__LINE__ = profiler_start(name)

#define PROFILER_STOP() \
    profiler_stop(__profiler_id_##__LINE__)

/* Memory tracking macros */
#define PROFILER_ALLOC(size) \
    profiler_track_alloc(size, __FILE__ ":" __XSTRING(__LINE__))

#define PROFILER_FREE(size) \
    profiler_track_free(size, __FILE__ ":" __XSTRING(__LINE__))

#else

/* No-op macros when profiling disabled */
#define PROFILER_START(name) do { } while(0)
#define PROFILER_STOP() do { } while(0)
#define PROFILER_ALLOC(size) do { } while(0)
#define PROFILER_FREE(size) do { } while(0)

#endif /* PROFILING_ENABLED */

/* Helper for stringification in macros */
#define __XSTRING(x) __STRING(x)
#define __STRING(x) #x

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_PROFILER_H */
