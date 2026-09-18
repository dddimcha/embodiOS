/* EMBODIOS High Precision Event Timer (HPET) Implementation
 *
 * Provides high-resolution timing using HPET hardware timer.
 * Alternative to TSC for microsecond-accurate timing on x86_64.
 */

#include <embodios/hpet.h>
#include <embodios/types.h>
#include <embodios/console.h>

/* ============================================================================
 * Memory-Mapped I/O Access
 * ============================================================================ */

/* Read 64-bit value from HPET register */
static inline uint64_t hpet_read_reg(volatile void *base, uint32_t offset)
{
    volatile uint64_t *reg = (volatile uint64_t *)((uintptr_t)base + offset);
    return *reg;
}

/* Write 64-bit value to HPET register */
static inline void hpet_write_reg(volatile void *base, uint32_t offset, uint64_t value)
{
    volatile uint64_t *reg = (volatile uint64_t *)((uintptr_t)base + offset);
    *reg = value;
}

/* ============================================================================
 * HPET State
 * ============================================================================ */

/* HPET base address (memory-mapped registers) */
static volatile void *hpet_base = NULL;

/* HPET frequency in Hz */
static uint64_t hpet_frequency = 0;

/* HPET period in femtoseconds */
static uint64_t hpet_period_fs = 0;

/* HPET feature flags */
static uint32_t hpet_features = 0;

/* Initialization flag */
static bool hpet_initialized = false;

/* Starting counter value (for relative time measurements) */
static uint64_t hpet_start_counter = 0;

/* ============================================================================
 * HPET Detection
 * ============================================================================ */

/* Detect HPET via fixed address
 *
 * Most x86_64 systems place HPET at 0xFED00000.
 * We attempt to read capabilities register to verify presence.
 */
static volatile void *hpet_detect_fixed_address(void)
{
    /* Skip HPET in QEMU without proper memory mapping
     * HPET base (0xFED00000) not mapped in early boot
     * Fall back to TSC-based timing instead */
    return NULL;

    /* Original code for systems with HPET mapped: */
#if 0
    volatile void *base = (volatile void *)(uintptr_t)HPET_DEFAULT_BASE_ADDR;

    /* Try to read capabilities register
     * If HPET is present, capabilities should be non-zero */
    uint64_t capabilities = hpet_read_reg(base, HPET_REG_CAPABILITIES);

    /* Sanity check: capabilities register should have valid values
     * - Revision ID (bits 0-7) should be non-zero
     * - Counter period (bits 32-63) should be non-zero and reasonable
     *   (typical range: 10-100 nanoseconds = 10,000,000-100,000,000 femtoseconds)
     */
    uint8_t rev_id = capabilities & HPET_CAP_REV_ID_MASK;
    uint64_t period = (capabilities >> HPET_CAP_PERIOD_SHIFT) & HPET_CAP_PERIOD_MASK;

    if (rev_id == 0 || rev_id == 0xFF) {
        /* Invalid revision - HPET probably not present */
        return NULL;
    }

    if (period == 0 || period > 100000000ULL) {
        /* Invalid period - either not present or broken */
        return NULL;
    }

    return base;
#endif
}

/* ============================================================================
 * HPET Initialization
 * ============================================================================ */

uint32_t hpet_detect_features(void)
{
    if (!hpet_base) {
        return 0;
    }

    uint32_t features = HPET_FEATURE_PRESENT;
    uint64_t capabilities = hpet_read_reg(hpet_base, HPET_REG_CAPABILITIES);

    /* Check if main counter is 64-bit */
    if (capabilities & HPET_CAP_COUNT_SIZE_64) {
        features |= HPET_FEATURE_64BIT;
    }

    /* Check if legacy replacement route is capable */
    if (capabilities & HPET_CAP_LEGACY_ROUTE) {
        features |= HPET_FEATURE_LEGACY;
    }

    /* Check if any timer supports periodic mode
     * Read timer 0 configuration to check */
    uint64_t timer0_config = hpet_read_reg(hpet_base, HPET_REG_TIMER_CONFIG(0));
    if (timer0_config & HPET_TIMER_CFG_PERIODIC_CAP) {
        features |= HPET_FEATURE_PERIODIC;
    }

    return features;
}

int hpet_init(void)
{
    if (hpet_initialized) {
        return 0;
    }

    console_printf("hpet: Initializing HPET subsystem...\n");

    /* Detect HPET base address */
    hpet_base = hpet_detect_fixed_address();
    if (!hpet_base) {
        console_printf("hpet: ERROR - HPET not found at fixed address 0x%lx\n",
                       (unsigned long)HPET_DEFAULT_BASE_ADDR);
        return -1;
    }

    console_printf("hpet: Found HPET at 0x%lx\n",
                   (unsigned long)HPET_DEFAULT_BASE_ADDR);

    /* Read capabilities */
    uint64_t capabilities = hpet_read_reg(hpet_base, HPET_REG_CAPABILITIES);

    /* Extract revision ID */
    uint8_t rev_id = capabilities & HPET_CAP_REV_ID_MASK;

    /* Extract number of timers */
    uint8_t num_timers = ((capabilities >> HPET_CAP_NUM_TIMERS_SHIFT)
                          & HPET_CAP_NUM_TIMERS_MASK) + 1;

    /* Extract vendor ID */
    uint16_t vendor_id = (capabilities >> HPET_CAP_VENDOR_ID_SHIFT)
                         & HPET_CAP_VENDOR_ID_MASK;

    /* Extract counter period in femtoseconds */
    hpet_period_fs = (capabilities >> HPET_CAP_PERIOD_SHIFT) & HPET_CAP_PERIOD_MASK;

    /* Calculate frequency from period
     * Frequency (Hz) = 10^15 / period (femtoseconds)
     * Using femtoseconds: 1 second = 10^15 femtoseconds
     */
    hpet_frequency = 1000000000000000ULL / hpet_period_fs;

    console_printf("hpet: Revision: %u\n", rev_id);
    console_printf("hpet: Vendor ID: 0x%04x\n", vendor_id);
    console_printf("hpet: Timers: %u\n", num_timers);
    console_printf("hpet: Period: %lu fs\n", hpet_period_fs);
    console_printf("hpet: Frequency: %lu Hz (%lu MHz)\n",
                   hpet_frequency, hpet_frequency / 1000000);

    /* Detect features */
    hpet_features = hpet_detect_features();

    console_printf("hpet: Features:");
    if (hpet_features & HPET_FEATURE_64BIT) {
        console_printf(" 64BIT");
    }
    if (hpet_features & HPET_FEATURE_LEGACY) {
        console_printf(" LEGACY");
    }
    if (hpet_features & HPET_FEATURE_PERIODIC) {
        console_printf(" PERIODIC");
    }
    console_printf("\n");

    /* Disable HPET before configuration */
    hpet_disable();

    /* Reset main counter to 0 */
    hpet_write_reg(hpet_base, HPET_REG_MAIN_COUNTER, 0);

    /* Enable HPET */
    int result = hpet_enable();
    if (result != 0) {
        console_printf("hpet: ERROR - Failed to enable HPET\n");
        return -1;
    }

    /* Record starting counter value */
    hpet_start_counter = hpet_read_counter();

    console_printf("hpet: HPET initialized successfully\n");
    hpet_initialized = true;

    return 0;
}

int hpet_enable(void)
{
    if (!hpet_base) {
        return -1;
    }

    /* Read current configuration */
    uint64_t config = hpet_read_reg(hpet_base, HPET_REG_CONFIGURATION);

    /* Set enable bit */
    config |= HPET_CFG_ENABLE;

    /* Write back configuration */
    hpet_write_reg(hpet_base, HPET_REG_CONFIGURATION, config);

    return 0;
}

void hpet_disable(void)
{
    if (!hpet_base) {
        return;
    }

    /* Read current configuration */
    uint64_t config = hpet_read_reg(hpet_base, HPET_REG_CONFIGURATION);

    /* Clear enable bit */
    config &= ~HPET_CFG_ENABLE;

    /* Write back configuration */
    hpet_write_reg(hpet_base, HPET_REG_CONFIGURATION, config);
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

uint64_t hpet_get_frequency(void)
{
    return hpet_frequency;
}

uint64_t hpet_get_period_fs(void)
{
    return hpet_period_fs;
}

uint64_t hpet_read_counter(void)
{
    if (!hpet_base) {
        return 0;
    }

    return hpet_read_reg(hpet_base, HPET_REG_MAIN_COUNTER);
}

uint64_t hpet_ticks_to_microseconds(uint64_t ticks)
{
    if (hpet_frequency == 0) {
        return 0;
    }

    /* Convert ticks to microseconds
     * microseconds = (ticks * 1,000,000) / frequency */
    return (ticks * 1000000ULL) / hpet_frequency;
}

uint64_t hpet_ticks_to_nanoseconds(uint64_t ticks)
{
    if (hpet_period_fs == 0) {
        return 0;
    }

    /* Convert ticks to nanoseconds using period
     * nanoseconds = (ticks * period_fs) / 1,000,000
     * Since period is in femtoseconds (10^-15), and we want nanoseconds (10^-9)
     * we divide by 10^6 */
    return (ticks * hpet_period_fs) / 1000000ULL;
}

uint64_t hpet_microseconds_to_ticks(uint64_t us)
{
    if (hpet_frequency == 0) {
        return 0;
    }

    /* Convert microseconds to ticks
     * ticks = (microseconds * frequency) / 1,000,000 */
    return (us * hpet_frequency) / 1000000ULL;
}

uint64_t hpet_get_microseconds(void)
{
    if (!hpet_initialized) {
        return 0;
    }

    uint64_t current = hpet_read_counter();
    uint64_t ticks = current - hpet_start_counter;
    return hpet_ticks_to_microseconds(ticks);
}

uint64_t hpet_get_nanoseconds(void)
{
    if (!hpet_initialized) {
        return 0;
    }

    uint64_t current = hpet_read_counter();
    uint64_t ticks = current - hpet_start_counter;
    return hpet_ticks_to_nanoseconds(ticks);
}

void hpet_delay_us(uint64_t us)
{
    if (!hpet_initialized) {
        return;
    }

    uint64_t start = hpet_read_counter();
    uint64_t ticks_to_wait = hpet_microseconds_to_ticks(us);

    /* Busy wait until the desired number of ticks has elapsed */
    while ((hpet_read_counter() - start) < ticks_to_wait) {
        __asm__ volatile("pause");
    }
}

int hpet_is_available(void)
{
    return hpet_initialized ? 1 : 0;
}
