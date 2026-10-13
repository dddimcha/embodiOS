/* EMBODIOS EMG DSP chain (SPEC-M0, WS-DSP)
 *
 * Per-sample, per-channel signal chain at fs = 1 kHz (float32; rt_timer
 * preserves the FPU state of the interrupted context around each tick):
 *
 *   raw ADC counts
 *     -> 50 Hz notch biquad (Q = 8, mains hum)
 *     -> 20 Hz high-pass biquad  (2nd-order Butterworth, motion artifact)
 *     -> 450 Hz low-pass biquad  (2nd-order Butterworth, anti-alias)
 *     -> full-wave rectify
 *     -> 1st-order envelope LPF (fc = 5 Hz)
 *
 * Coefficients are computed at init from the RBJ audio EQ cookbook
 * (biquad notch / highpass / lowpass forms) using the kernel lib math
 * (lib/math.c). State is kept per channel (EMG_MAX_CHANNELS array; M0
 * uses EMG_M0_CHANNELS = 2).
 *
 * No allocation, no console output: safe in tick IRQ context.
 */

#include <embodios/emg.h>
#include <embodios/types.h>
#include <stddef.h>

extern float sinf(float x);
extern float cosf(float x);
extern float expf(float x);
extern float fabsf(float x);

#define EMG_FS_HZ       1000.0f
#define EMG_NOTCH_HZ    50.0f
#define EMG_NOTCH_Q     8.0f
#define EMG_HP_HZ       20.0f
#define EMG_LP_HZ       450.0f
#define EMG_BUTTER_Q    0.70710678f   /* 1/sqrt(2): Butterworth 2nd-order */
#define EMG_ENV_HZ      5.0f
#define EMG_TWO_PI      6.28318530f

typedef struct {
    float b0, b1, b2;   /* feed-forward coeffs (normalized by a0) */
    float a1, a2;       /* feedback coeffs (normalized by a0) */
    float x1, x2;       /* input history */
    float y1, y2;       /* output history */
} biquad_t;

typedef struct {
    biquad_t notch;     /* 50 Hz notch, Q = 8 */
    biquad_t hp;        /* 20 Hz high-pass */
    biquad_t lp;        /* 450 Hz low-pass */
    float    env;       /* envelope LPF state */
} emg_dsp_ch_t;

static emg_dsp_ch_t dsp_ch[EMG_MAX_CHANNELS];
static float        env_alpha;  /* 1st-order envelope LPF coefficient */
static int          dsp_ready;

/* ============================================================================
 * Biquad coefficient builders (RBJ cookbook), fs = 1 kHz
 * ============================================================================ */

static void biquad_reset(biquad_t *b)
{
    b->x1 = b->x2 = b->y1 = b->y2 = 0.0f;
}

static float biquad_step(biquad_t *b, float x)
{
    float y = b->b0 * x + b->b1 * b->x1 + b->b2 * b->x2
                     - b->a1 * b->y1 - b->a2 * b->y2;
    b->x2 = b->x1;
    b->x1 = x;
    b->y2 = b->y1;
    b->y1 = y;
    return y;
}

/* Notch: H(z) with unity gain away from f0 */
static void biquad_set_notch(biquad_t *b, float f0, float q)
{
    float w0    = EMG_TWO_PI * f0 / EMG_FS_HZ;
    float cw0   = cosf(w0);
    float alpha = sinf(w0) / (2.0f * q);
    float a0    = 1.0f + alpha;

    b->b0 = 1.0f / a0;
    b->b1 = -2.0f * cw0 / a0;
    b->b2 = 1.0f / a0;
    b->a1 = -2.0f * cw0 / a0;
    b->a2 = (1.0f - alpha) / a0;
    biquad_reset(b);
}

/* 2nd-order Butterworth high-pass (Q = 1/sqrt(2)) */
static void biquad_set_highpass(biquad_t *b, float f0, float q)
{
    float w0    = EMG_TWO_PI * f0 / EMG_FS_HZ;
    float cw0   = cosf(w0);
    float alpha = sinf(w0) / (2.0f * q);
    float a0    = 1.0f + alpha;

    b->b0 =  (1.0f + cw0) * 0.5f / a0;
    b->b1 = -(1.0f + cw0) / a0;
    b->b2 =  (1.0f + cw0) * 0.5f / a0;
    b->a1 = -2.0f * cw0 / a0;
    b->a2 =  (1.0f - alpha) / a0;
    biquad_reset(b);
}

/* 2nd-order Butterworth low-pass (Q = 1/sqrt(2)) */
static void biquad_set_lowpass(biquad_t *b, float f0, float q)
{
    float w0    = EMG_TWO_PI * f0 / EMG_FS_HZ;
    float cw0   = cosf(w0);
    float alpha = sinf(w0) / (2.0f * q);
    float a0    = 1.0f + alpha;

    b->b0 = (1.0f - cw0) * 0.5f / a0;
    b->b1 = (1.0f - cw0) / a0;
    b->b2 = (1.0f - cw0) * 0.5f / a0;
    b->a1 = -2.0f * cw0 / a0;
    b->a2 = (1.0f - alpha) / a0;
    biquad_reset(b);
}

/* ============================================================================
 * Public API (contract: embodios/emg.h)
 * ============================================================================ */

void emg_dsp_init(void)
{
    for (int ch = 0; ch < EMG_MAX_CHANNELS; ch++) {
        biquad_set_notch(&dsp_ch[ch].notch, EMG_NOTCH_HZ, EMG_NOTCH_Q);
        biquad_set_highpass(&dsp_ch[ch].hp, EMG_HP_HZ, EMG_BUTTER_Q);
        biquad_set_lowpass(&dsp_ch[ch].lp, EMG_LP_HZ, EMG_BUTTER_Q);
        dsp_ch[ch].env = 0.0f;
    }
    /* 1st-order LPF: alpha = 1 - exp(-2*pi*fc/fs) (exact discrete match) */
    env_alpha = 1.0f - expf(-EMG_TWO_PI * EMG_ENV_HZ / EMG_FS_HZ);
    dsp_ready = 1;
}

void emg_dsp_reset(int ch)
{
    if (ch < 0 || ch >= EMG_MAX_CHANNELS) {
        return;
    }
    biquad_reset(&dsp_ch[ch].notch);
    biquad_reset(&dsp_ch[ch].hp);
    biquad_reset(&dsp_ch[ch].lp);
    dsp_ch[ch].env = 0.0f;
}

float emg_dsp_process(int ch, float raw)
{
    if (!dsp_ready) {
        emg_dsp_init();
    }
    if (ch < 0 || ch >= EMG_MAX_CHANNELS) {
        return 0.0f;
    }

    emg_dsp_ch_t *d = &dsp_ch[ch];

    float y = biquad_step(&d->notch, raw);
    y = biquad_step(&d->hp, y);
    y = biquad_step(&d->lp, y);

    /* Full-wave rectify + envelope LPF */
    float r = fabsf(y);
    d->env += env_alpha * (r - d->env);
    return d->env;
}
