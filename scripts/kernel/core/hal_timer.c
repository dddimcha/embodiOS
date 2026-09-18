/* EMBODIOS HAL Timer Implementation */
#include <embodios/hal_timer.h>
#include <embodios/types.h>

/* Global HAL timer operations pointer */
static const struct hal_timer_ops *timer_ops = NULL;

/* Register HAL timer operations */
void hal_timer_register(const struct hal_timer_ops *ops)
{
    timer_ops = ops;
}

/* Get HAL timer operations */
const struct hal_timer_ops* hal_timer_get_ops(void)
{
    return timer_ops;
}

/* HAL timer wrapper functions */
void hal_timer_init(void)
{
    if (timer_ops && timer_ops->init) {
        timer_ops->init();
    }
}

void hal_timer_enable(void)
{
    if (timer_ops && timer_ops->enable) {
        timer_ops->enable();
    }
}

void hal_timer_disable(void)
{
    if (timer_ops && timer_ops->disable) {
        timer_ops->disable();
    }
}

void hal_timer_configure(const struct timer_config *config)
{
    if (timer_ops && timer_ops->configure) {
        timer_ops->configure(config);
    }
}

uint64_t hal_timer_get_ticks(void)
{
    if (timer_ops && timer_ops->get_ticks) {
        return timer_ops->get_ticks();
    }
    return 0;
}

uint64_t hal_timer_get_frequency(void)
{
    if (timer_ops && timer_ops->get_frequency) {
        return timer_ops->get_frequency();
    }
    return 0;
}

uint64_t hal_timer_get_microseconds(void)
{
    if (timer_ops && timer_ops->get_microseconds) {
        return timer_ops->get_microseconds();
    }
    return 0;
}

uint64_t hal_timer_get_milliseconds(void)
{
    if (timer_ops && timer_ops->get_milliseconds) {
        return timer_ops->get_milliseconds();
    }
    return 0;
}

void hal_timer_delay_us(uint64_t microseconds)
{
    if (timer_ops && timer_ops->delay_us) {
        timer_ops->delay_us(microseconds);
    }
}

void hal_timer_delay_ms(uint64_t milliseconds)
{
    if (timer_ops && timer_ops->delay_ms) {
        timer_ops->delay_ms(milliseconds);
    }
}

uint64_t hal_timer_ticks_to_us(uint64_t ticks)
{
    if (timer_ops && timer_ops->ticks_to_us) {
        return timer_ops->ticks_to_us(ticks);
    }
    return 0;
}

uint64_t hal_timer_us_to_ticks(uint64_t microseconds)
{
    if (timer_ops && timer_ops->us_to_ticks) {
        return timer_ops->us_to_ticks(microseconds);
    }
    return 0;
}
