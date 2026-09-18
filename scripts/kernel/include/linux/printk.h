/* SPDX-License-Identifier: GPL-2.0 */
/**
 * Linux Compatibility Layer - Kernel Printing
 *
 * Provides Linux kernel printing APIs mapped to EMBODIOS console_printf.
 * Reference: linux/include/linux/printk.h
 *
 * Part of EMBODIOS Linux Driver Compatibility Shim (~50 APIs)
 */

#ifndef _LINUX_PRINTK_H
#define _LINUX_PRINTK_H

#include <linux/types.h>
#include <embodios/console.h>

/* ============================================================================
 * Log levels
 * ============================================================================
 *
 * Linux uses numeric log levels embedded in the format string.
 * EMBODIOS doesn't filter by level, all messages go to console.
 */

#define KERN_EMERG      "<0>"   /* System is unusable */
#define KERN_ALERT      "<1>"   /* Action must be taken immediately */
#define KERN_CRIT       "<2>"   /* Critical conditions */
#define KERN_ERR        "<3>"   /* Error conditions */
#define KERN_WARNING    "<4>"   /* Warning conditions */
#define KERN_NOTICE     "<5>"   /* Normal but significant condition */
#define KERN_INFO       "<6>"   /* Informational */
#define KERN_DEBUG      "<7>"   /* Debug-level messages */

/* Default level when none specified */
#define KERN_DEFAULT    ""
#define KERN_CONT       "c"     /* Continuation of previous line */

/* Numeric log levels */
#define LOGLEVEL_EMERG      0
#define LOGLEVEL_ALERT      1
#define LOGLEVEL_CRIT       2
#define LOGLEVEL_ERR        3
#define LOGLEVEL_WARNING    4
#define LOGLEVEL_NOTICE     5
#define LOGLEVEL_INFO       6
#define LOGLEVEL_DEBUG      7

/* ============================================================================
 * printk - Kernel message printing
 * ============================================================================ */

/*
 * printk - Print a kernel message
 * @fmt: format string (may contain log level prefix)
 * @...: arguments
 *
 * Maps to EMBODIOS console_printf, stripping log level prefix if present.
 *
 * Note: In EMBODIOS, all log levels are printed. In Linux, messages
 * below the configured console_loglevel are suppressed.
 */
#define printk(fmt, args...) \
    do { \
        const char *__fmt = fmt; \
        /* Strip log level prefix */ \
        if (__fmt[0] == '<' && __fmt[1] >= '0' && __fmt[1] <= '7' && __fmt[2] == '>') \
            __fmt += 3; \
        console_printf(__fmt, ##args); \
    } while (0)

/* ============================================================================
 * pr_* macros - Preferred printing interface
 * ============================================================================
 *
 * These are the preferred way to print in Linux drivers.
 * They automatically add the log level and can include module name.
 */

/* Emergency - system is unusable */
#define pr_emerg(fmt, ...) \
    console_printf("[EMERG] " fmt, ##__VA_ARGS__)

/* Alert - action must be taken immediately */
#define pr_alert(fmt, ...) \
    console_printf("[ALERT] " fmt, ##__VA_ARGS__)

/* Critical - critical conditions */
#define pr_crit(fmt, ...) \
    console_printf("[CRIT] " fmt, ##__VA_ARGS__)

/* Error - error conditions */
#define pr_err(fmt, ...) \
    console_printf("[ERROR] " fmt, ##__VA_ARGS__)

/* Warning - warning conditions */
#define pr_warn(fmt, ...) \
    console_printf("[WARN] " fmt, ##__VA_ARGS__)

#define pr_warning(fmt, ...) pr_warn(fmt, ##__VA_ARGS__)

/* Notice - normal but significant */
#define pr_notice(fmt, ...) \
    console_printf("[NOTICE] " fmt, ##__VA_ARGS__)

/* Info - informational */
#define pr_info(fmt, ...) \
    console_printf("[INFO] " fmt, ##__VA_ARGS__)

/* Debug - debug messages */
#ifdef DEBUG
#define pr_debug(fmt, ...) \
    console_printf("[DEBUG] " fmt, ##__VA_ARGS__)
#else
#define pr_debug(fmt, ...) \
    do { if (0) console_printf("[DEBUG] " fmt, ##__VA_ARGS__); } while (0)
#endif

/* pr_devel - development debugging (compiled out in release) */
#ifdef DEBUG
#define pr_devel(fmt, ...) pr_debug(fmt, ##__VA_ARGS__)
#else
#define pr_devel(fmt, ...) \
    do { if (0) console_printf(fmt, ##__VA_ARGS__); } while (0)
#endif

/* Continuation - no prefix */
#define pr_cont(fmt, ...) \
    console_printf(fmt, ##__VA_ARGS__)

/* ============================================================================
 * pr_fmt - Format prefix macro
 * ============================================================================
 *
 * Drivers can define pr_fmt before including printk.h to add a prefix.
 * Example: #define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
 */
#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif

/* ============================================================================
 * dev_* macros - Device-specific printing
 * ============================================================================
 *
 * These print messages with device identification.
 * Simplified in EMBODIOS - device info is optional.
 */

struct device;  /* Forward declaration */

#define dev_emerg(dev, fmt, ...) \
    pr_emerg("dev: " fmt, ##__VA_ARGS__)

#define dev_alert(dev, fmt, ...) \
    pr_alert("dev: " fmt, ##__VA_ARGS__)

#define dev_crit(dev, fmt, ...) \
    pr_crit("dev: " fmt, ##__VA_ARGS__)

#define dev_err(dev, fmt, ...) \
    pr_err("dev: " fmt, ##__VA_ARGS__)

#define dev_warn(dev, fmt, ...) \
    pr_warn("dev: " fmt, ##__VA_ARGS__)

#define dev_notice(dev, fmt, ...) \
    pr_notice("dev: " fmt, ##__VA_ARGS__)

#define dev_info(dev, fmt, ...) \
    pr_info("dev: " fmt, ##__VA_ARGS__)

#ifdef DEBUG
#define dev_dbg(dev, fmt, ...) \
    pr_debug("dev: " fmt, ##__VA_ARGS__)
#else
#define dev_dbg(dev, fmt, ...) \
    do { if (0) console_printf(fmt, ##__VA_ARGS__); } while (0)
#endif

/* ============================================================================
 * Rate-limited printing
 * ============================================================================
 *
 * These macros limit print frequency to avoid log flooding.
 * Simplified in EMBODIOS - no rate limiting, always prints.
 */

#define printk_ratelimited(fmt, ...) \
    printk(fmt, ##__VA_ARGS__)

#define pr_emerg_ratelimited(fmt, ...)  pr_emerg(fmt, ##__VA_ARGS__)
#define pr_alert_ratelimited(fmt, ...)  pr_alert(fmt, ##__VA_ARGS__)
#define pr_crit_ratelimited(fmt, ...)   pr_crit(fmt, ##__VA_ARGS__)
#define pr_err_ratelimited(fmt, ...)    pr_err(fmt, ##__VA_ARGS__)
#define pr_warn_ratelimited(fmt, ...)   pr_warn(fmt, ##__VA_ARGS__)
#define pr_notice_ratelimited(fmt, ...) pr_notice(fmt, ##__VA_ARGS__)
#define pr_info_ratelimited(fmt, ...)   pr_info(fmt, ##__VA_ARGS__)
#define pr_debug_ratelimited(fmt, ...)  pr_debug(fmt, ##__VA_ARGS__)

#define dev_emerg_ratelimited(dev, fmt, ...)  dev_emerg(dev, fmt, ##__VA_ARGS__)
#define dev_alert_ratelimited(dev, fmt, ...)  dev_alert(dev, fmt, ##__VA_ARGS__)
#define dev_crit_ratelimited(dev, fmt, ...)   dev_crit(dev, fmt, ##__VA_ARGS__)
#define dev_err_ratelimited(dev, fmt, ...)    dev_err(dev, fmt, ##__VA_ARGS__)
#define dev_warn_ratelimited(dev, fmt, ...)   dev_warn(dev, fmt, ##__VA_ARGS__)
#define dev_notice_ratelimited(dev, fmt, ...) dev_notice(dev, fmt, ##__VA_ARGS__)
#define dev_info_ratelimited(dev, fmt, ...)   dev_info(dev, fmt, ##__VA_ARGS__)
#define dev_dbg_ratelimited(dev, fmt, ...)    dev_dbg(dev, fmt, ##__VA_ARGS__)

/* ============================================================================
 * Once-only printing
 * ============================================================================
 *
 * These print only once - subsequent calls are suppressed.
 */

#define printk_once(fmt, ...) ({                \
    static bool __once = false;                 \
    if (!__once) {                              \
        __once = true;                          \
        printk(fmt, ##__VA_ARGS__);             \
    }                                           \
})

#define pr_emerg_once(fmt, ...)  printk_once(KERN_EMERG fmt, ##__VA_ARGS__)
#define pr_alert_once(fmt, ...)  printk_once(KERN_ALERT fmt, ##__VA_ARGS__)
#define pr_crit_once(fmt, ...)   printk_once(KERN_CRIT fmt, ##__VA_ARGS__)
#define pr_err_once(fmt, ...)    printk_once(KERN_ERR fmt, ##__VA_ARGS__)
#define pr_warn_once(fmt, ...)   printk_once(KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_notice_once(fmt, ...) printk_once(KERN_NOTICE fmt, ##__VA_ARGS__)
#define pr_info_once(fmt, ...)   printk_once(KERN_INFO fmt, ##__VA_ARGS__)
#define pr_debug_once(fmt, ...)  printk_once(KERN_DEBUG fmt, ##__VA_ARGS__)

/* ============================================================================
 * Hex dump utilities
 * ============================================================================ */

/*
 * print_hex_dump - Print a hex dump to console
 * @level: log level
 * @prefix_str: prefix for each line
 * @prefix_type: type of prefix (ignored)
 * @rowsize: number of bytes per line
 * @groupsize: number of bytes per group
 * @buf: buffer to dump
 * @len: length of buffer
 * @ascii: whether to print ASCII representation
 */
static inline void print_hex_dump(const char *level, const char *prefix_str,
                                   int prefix_type, int rowsize, int groupsize,
                                   const void *buf, size_t len, bool ascii)
{
    const u8 *ptr = (const u8 *)buf;
    size_t i, j;

    (void)level;
    (void)prefix_type;
    (void)groupsize;

    for (i = 0; i < len; i += rowsize) {
        console_printf("%s%04zx: ", prefix_str, i);

        /* Hex bytes */
        for (j = 0; j < (size_t)rowsize && i + j < len; j++) {
            console_printf("%02x ", ptr[i + j]);
        }

        /* Padding */
        for (; j < (size_t)rowsize; j++) {
            console_printf("   ");
        }

        /* ASCII */
        if (ascii) {
            console_printf(" |");
            for (j = 0; j < (size_t)rowsize && i + j < len; j++) {
                u8 c = ptr[i + j];
                console_printf("%c", (c >= 32 && c < 127) ? c : '.');
            }
            console_printf("|");
        }

        console_printf("\n");
    }
}

/* Simplified hex dump */
#define print_hex_dump_bytes(prefix, type, buf, len) \
    print_hex_dump(KERN_DEBUG, prefix, type, 16, 1, buf, len, true)

/* ============================================================================
 * Misc utilities
 * ============================================================================ */

/* Dump stack trace - simplified */
static inline void dump_stack(void)
{
    console_printf("[STACK] Stack dump not available\n");
}

/* Log level control (no-op in EMBODIOS) */
static inline int console_loglevel_set(int level)
{
    (void)level;
    return 0;
}

/* WARN macros */
#define WARN(condition, fmt, ...) ({                    \
    int __ret_warn = !!(condition);                     \
    if (__ret_warn)                                     \
        pr_warn("WARNING: " fmt, ##__VA_ARGS__);        \
    __ret_warn;                                         \
})

#define WARN_ON(condition) ({                           \
    int __ret_warn = !!(condition);                     \
    if (__ret_warn)                                     \
        pr_warn("WARNING: %s:%d\n", __FILE__, __LINE__);\
    __ret_warn;                                         \
})

#define WARN_ON_ONCE(condition) ({                      \
    static bool __warned = false;                       \
    int __ret_warn = !!(condition);                     \
    if (__ret_warn && !__warned) {                      \
        __warned = true;                                \
        pr_warn("WARNING: %s:%d\n", __FILE__, __LINE__);\
    }                                                   \
    __ret_warn;                                         \
})

#define WARN_ONCE(condition, fmt, ...) ({               \
    static bool __warned = false;                       \
    int __ret_warn = !!(condition);                     \
    if (__ret_warn && !__warned) {                      \
        __warned = true;                                \
        pr_warn("WARNING: " fmt, ##__VA_ARGS__);        \
    }                                                   \
    __ret_warn;                                         \
})

#endif /* _LINUX_PRINTK_H */
