/* EMBODIOS EMG classifier v0 (SPEC-M0, WS-DSP)
 *
 * Threshold classifier with hysteresis on the normalized envelope:
 *
 *   range      = MVC - baseline          (from calibration)
 *   on  thresh = baseline + 0.25 * range (REST -> CONTRACT)
 *   off thresh = baseline + 0.15 * range (CONTRACT -> REST)
 *   level      = clamp((env - baseline) / range, 0, 1)
 *
 * Per-channel hysteresis state; the aggregate class is CONTRACT when any
 * active channel is CONTRACT, and the aggregate level is the max channel
 * level (2 channels for M0).
 *
 * Absurd-input guard: a non-finite (or wildly out-of-range) envelope means
 * the signal chain is broken (saturated ADC, loose electrode, numeric bug)
 * -> latch estop and torque the actuator OFF immediately. The raw-sample
 * stuck-at-rail guard lives in emg.c (it owns the raw stream).
 *
 * No allocation, no console output: safe in tick IRQ context.
 */

#include <embodios/emg.h>
#include <embodios/actuator.h>
#include <embodios/types.h>

/* Owned by emg.c */
extern struct emg_state *emg_state_mut(void);

#define EMG_ON_FRAC     0.25f   /* fraction of (MVC-baseline) to enter CONTRACT */
#define EMG_OFF_FRAC    0.15f   /* fraction of (MVC-baseline) to leave CONTRACT */
#define EMG_ENV_ABSURD  1.0e9f  /* envelope beyond this is nonsense */

static emg_class_t ch_cls[EMG_MAX_CHANNELS];

void emg_classify_reset(void)
{
    for (int ch = 0; ch < EMG_MAX_CHANNELS; ch++) {
        ch_cls[ch] = EMG_REST;
    }
}

emg_class_t emg_classify(int ch, float env, float *level)
{
    struct emg_state *st = emg_state_mut();
    float lv = 0.0f;

    if (ch < 0 || ch >= EMG_MAX_CHANNELS) {
        if (level) *level = 0.0f;
        return EMG_REST;
    }

    /* Absurd-input guard: non-finite (env != env catches NaN) or absurdly
     * large envelope -> fail safe. */
    if (env != env || env < 0.0f || env > EMG_ENV_ABSURD) {
        if (!st->estop) {
            st->estop = 1;
            actuator_estop();
        }
        ch_cls[ch] = EMG_REST;
        if (level) *level = 0.0f;
        return EMG_REST;
    }

    float base  = st->baseline[ch];
    float range = st->mvc[ch] - base;
    if (range < 1.0f) {
        range = 1.0f;   /* degenerate calibration: avoid div-by-tiny */
    }

    float on_th  = base + EMG_ON_FRAC * range;
    float off_th = base + EMG_OFF_FRAC * range;

    if (ch_cls[ch] == EMG_REST) {
        if (env > on_th) {
            ch_cls[ch] = EMG_CONTRACT;
        }
    } else {
        if (env < off_th) {
            ch_cls[ch] = EMG_REST;
        }
    }

    lv = (env - base) / range;
    if (lv < 0.0f) lv = 0.0f;
    if (lv > 1.0f) lv = 1.0f;

    if (level) *level = lv;
    return ch_cls[ch];
}

void emg_classify_tick(void)
{
    struct emg_state *st = emg_state_mut();
    int any_contract = 0;
    float max_level = 0.0f;

    int nch = st->channels;
    if (nch > EMG_MAX_CHANNELS) nch = EMG_MAX_CHANNELS;

    for (int ch = 0; ch < nch; ch++) {
        float lv = 0.0f;
        emg_class_t c = emg_classify(ch, st->envelope[ch], &lv);
        if (c == EMG_CONTRACT) {
            any_contract = 1;
            if (lv > max_level) max_level = lv;
        }
    }

    st->cls   = any_contract ? EMG_CONTRACT : EMG_REST;
    st->level = any_contract ? max_level : 0.0f;
}
