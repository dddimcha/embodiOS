#ifndef EMBODIOS_HAL_TIMER_H
#define EMBODIOS_HAL_TIMER_H

#include <embodios/types.h>

/* Timer flags */
#define TIMER_FLAG_PERIODIC     (1 << 0)
#define TIMER_FLAG_ONESHOT      (1 << 1)
#define TIMER_FLAG_ENABLED      (1 << 2)

/* Timer callback type */
typedef void (*timer_callback_t)(void *data);

/* Timer configuration structure */
struct timer_config {
    uint64_t frequency;      /* Timer frequency in Hz */
    uint64_t interval;       /* Interval in microseconds */
    uint32_t flags;          /* Timer flags */
    timer_callback_t callback;
    void *callback_data;
};

/* HAL timer operations structure */
struct hal_timer_ops {
    /* Initialization */
    void (*init)(void);

    /* Timer control */
    void (*enable)(void);
    void (*disable)(void);
    void (*configure)(const struct timer_config *config);

    /* Time measurement */
    uint64_t (*get_ticks)(void);
    uint64_t (*get_frequency)(void);
    uint64_t (*get_microseconds)(void);
    uint64_t (*get_milliseconds)(void);

    /* Delay functions */
    void (*delay_us)(uint64_t microseconds);
    void (*delay_ms)(uint64_t milliseconds);

    /* Tick conversion */
    uint64_t (*ticks_to_us)(uint64_t ticks);
    uint64_t (*us_to_ticks)(uint64_t microseconds);
};

/* HAL timer interface functions */
void hal_timer_register(const struct hal_timer_ops *ops);
const struct hal_timer_ops* hal_timer_get_ops(void);

/* HAL timer wrapper functions */
void hal_timer_init(void);
void hal_timer_enable(void);
void hal_timer_disable(void);
void hal_timer_configure(const struct timer_config *config);
uint64_t hal_timer_get_ticks(void);
uint64_t hal_timer_get_frequency(void);
uint64_t hal_timer_get_microseconds(void);
uint64_t hal_timer_get_milliseconds(void);
void hal_timer_delay_us(uint64_t microseconds);
void hal_timer_delay_ms(uint64_t milliseconds);
uint64_t hal_timer_ticks_to_us(uint64_t ticks);
uint64_t hal_timer_us_to_ticks(uint64_t microseconds);

#endif /* EMBODIOS_HAL_TIMER_H */
