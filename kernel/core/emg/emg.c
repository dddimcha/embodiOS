/* EMBODIOS EMG pipeline core (SPEC-M0, WS-DSP)
 *
 * Owns the pipeline state, the calibration sequence, the deterministic
 * synthetic EMG source, the 1 kHz RT tick (read -> dsp -> classify ->
 * actuator_set), and the weak hardware/actuator stubs that WS-DRV
 * overrides on Pi boards (same weak-symbol pattern as bcm_mbox_reset in
 * core/cmd_power.c, but as weak *definitions* so the link always
 * resolves).
 *
 * Signal chain per sample per channel (dsp.c): 50 Hz notch (Q=8) ->
 * 20 Hz HP -> 450 Hz LP (2nd-order Butterworth) -> rectify -> 5 Hz
 * envelope LPF. Classifier v0 (classify.c): hysteresis threshold on the
 * normalized envelope -> REST/CONTRACT + proportional level 0..1.
 *
 * Synthetic source (used when emg_hw_available() == 0): seeded LFSR
 * noise, +-30 counts idle around 512; contraction = 90/115 Hz oscillation
 * mix at +-300 counts. Modes: IDLE, BURST (calibration MVC), SCRIPT
 * (emgtest timeline: 1 s rest, 1.5 s ramp, 1 s hold, release), DEMO
 * (m0demo window driven by rt ticks). Deterministic seed -> reproducible.
 *
 * RT wiring: emg_rt_enable(1) registers emg_rt_tick on rt_timer at a
 * 1000 us period (clamps to the platform tick rate: 1 kHz LAPIC on
 * x86_64, 100 Hz generic timer on aarch64 virt — integration note for
 * the main agent). Callback rules honored: no console output from the
 * tick; the m0demo trace is handed to task context via state snapshots.
 */

#include <embodios/emg.h>
#include <embodios/actuator.h>
#include <embodios/console.h>
#include <embodios/rt_timer.h>
#include <embodios/tsc.h>
#include <embodios/types.h>
#include <embodios/hal_timer.h>

extern float sinf(float x);
extern float fabsf(float x);

/* From dsp.c / classify.c (internal to the EMG module, not in the
 * frozen contract header) */
extern void emg_dsp_init(void);
extern void emg_dsp_reset(int ch);
extern void emg_classify_reset(void);

/* ============================================================================
 * State
 * ============================================================================ */

static struct emg_state emg;
static int emg_initialized;
static int rt_active;

/* Uncalibrated fallback ranges (envelope units of the synthetic source);
 * real values come from emg_calibrate(). */
#define EMG_DEFAULT_BASELINE 10.0f
#define EMG_DEFAULT_MVC      200.0f

struct emg_state *emg_state_mut(void)
{
    return &emg;
}

const struct emg_state *emg_get_state(void)
{
    if (!emg_initialized) {
        emg_init();
    }
    return &emg;
}

/* ============================================================================
 * Synthetic source (deterministic, compiled always)
 * ============================================================================ */

#define SYNTH_CENTER     512
#define SYNTH_NOISE_AMP  30      /* idle: +/- counts */
#define SYNTH_BURST_AMP  300     /* contraction: +/- counts */
#define SYNTH_TWO_PI     6.28318530f

enum {
    SYNTH_IDLE = 0,     /* idle noise only */
    SYNTH_BURST,        /* continuous contraction (calibration MVC phase) */
    SYNTH_SCRIPT,       /* emgtest timeline: 1s rest, 1.5s ramp, 1s hold, off */
    SYNTH_DEMO,         /* m0demo: contraction window in rt ticks */
};

/* emgtest script timeline in samples @ 1 kHz */
#define SCRIPT_REST_SAMPLES   1000
#define SCRIPT_RAMP_SAMPLES   1500
#define SCRIPT_HOLD_SAMPLES   1000

#define DEMO_RAMP_TICKS       150

static struct {
    int      mode;
    uint32_t lfsr;
    uint32_t n[EMG_MAX_CHANNELS];   /* per-channel sample index */
    uint32_t demo_start;            /* contraction window [start, end), ticks */
    uint32_t demo_end;
} synth;

/* Galois LFSR, poly x^32+x^22+x^2+x+1 — maximal period, deterministic. */
static uint32_t synth_lfsr_next(void)
{
    uint32_t l = synth.lfsr;
    l = (l >> 1) ^ ((uint32_t)(-(int32_t)(l & 1u)) & 0x80200003u);
    synth.lfsr = l;
    return l;
}

/* Idle noise in [-SYNTH_NOISE_AMP, +SYNTH_NOISE_AMP] */
static int synth_noise(void)
{
    return (int)(synth_lfsr_next() % (2u * SYNTH_NOISE_AMP + 1u)) - SYNTH_NOISE_AMP;
}

/* Band-limited contraction burst: 90 Hz + 115 Hz mix, scaled by amp 0..1 */
static float synth_burst_wave(uint32_t n, float amp)
{
    float w = 0.6f * sinf(SYNTH_TWO_PI * 90.0f  * (float)n / 1000.0f)
            + 0.4f * sinf(SYNTH_TWO_PI * 115.0f * (float)n / 1000.0f);
    return amp * (float)SYNTH_BURST_AMP * w;
}

static float synth_mode_amp(uint32_t n)
{
    switch (synth.mode) {
    case SYNTH_BURST:
        return 1.0f;
    case SYNTH_SCRIPT:
        if (n < SCRIPT_REST_SAMPLES) {
            return 0.0f;
        }
        if (n < SCRIPT_REST_SAMPLES + SCRIPT_RAMP_SAMPLES) {
            return (float)(n - SCRIPT_REST_SAMPLES) / (float)SCRIPT_RAMP_SAMPLES;
        }
        if (n < SCRIPT_REST_SAMPLES + SCRIPT_RAMP_SAMPLES + SCRIPT_HOLD_SAMPLES) {
            return 1.0f;
        }
        return 0.0f;
    case SYNTH_DEMO:
        if (n < synth.demo_start || n >= synth.demo_end) {
            return 0.0f;
        }
        if (n < synth.demo_start + DEMO_RAMP_TICKS) {
            return (float)(n - synth.demo_start) / (float)DEMO_RAMP_TICKS;
        }
        return 1.0f;
    case SYNTH_IDLE:
    default:
        return 0.0f;
    }
}

/* One synthetic ADC sample: 10-bit counts centered at 512. Channel 1 is a
 * scaled (0.7x) copy with independent noise — same contraction timeline. */
static int synth_read(int ch)
{
    if (ch < 0) ch = 0;
    if (ch >= EMG_MAX_CHANNELS) ch = EMG_MAX_CHANNELS - 1;

    uint32_t n = synth.n[ch]++;
    float amp = synth_mode_amp(n);
    float ch_scale = (ch == 0) ? 1.0f : 0.7f;

    int v = SYNTH_CENTER + synth_noise()
          + (int)(synth_burst_wave(n, amp) * ch_scale);
    if (v < 0)    v = 0;
    if (v > 1023) v = 1023;
    return v;
}

/* Internal helpers used by cmd_emg.c (not part of the frozen contract) */
void emg_synth_reset(void)
{
    synth.lfsr = 0x2545F491u;       /* fixed nonzero seed: reproducible */
    for (int ch = 0; ch < EMG_MAX_CHANNELS; ch++) {
        synth.n[ch] = 0;
    }
}

void emg_synth_set_mode(int mode)
{
    synth.mode = mode;
}

int emg_synth_read(int ch)
{
    return synth_read(ch);
}

int emg_using_synth(void)
{
    return emg_hw_available() == 0;
}

/* Filter warmup (synthetic source only): the HP biquad has a startup
 * transient when the input steps from 0 to the 512-count DC offset; run
 * idle samples through the chain so the envelope starts at baseline, then
 * restart the synth timeline (indices + LFSR) with warm filters. */
void emg_dsp_warmup(int samples)
{
    if (!emg_using_synth()) {
        return;     /* live hardware: no pre-roll possible */
    }
    int saved_mode = synth.mode;
    synth.mode = SYNTH_IDLE;
    for (int i = 0; i < samples; i++) {
        for (int ch = 0; ch < emg.channels; ch++) {
            emg.envelope[ch] = emg_dsp_process(ch, (float)synth_read(ch));
        }
    }
    synth.mode = saved_mode;
    emg_synth_reset();
}

/* ============================================================================
 * Weak hardware hooks — WS-DRV overrides these on Pi boards (strong
 * definitions in drivers/adc/). Defaults select the synthetic source.
 * ============================================================================ */

__attribute__((weak)) int emg_hw_available(void)
{
    return 0;
}

__attribute__((weak)) int emg_hw_read(int ch)
{
    return synth_read(ch);
}

/* ============================================================================
 * Weak actuator stubs — WS-DRV overrides with the Feetech STS3215 driver.
 * The stub records a trace (level changes) so QEMU runs are auditable.
 * ============================================================================ */

#define ACT_TRACE_RING 32

static struct {
    uint32_t calls;
    uint32_t estops;
    float    last_level;
    int      estopped;
    uint32_t trace_head;
    struct {
        uint32_t tick;
        float    level;
    } trace[ACT_TRACE_RING];
} act_stub;

__attribute__((weak)) int actuator_init(void)
{
    act_stub.estopped = 0;
    return 0;
}

__attribute__((weak)) int actuator_set(float level)
{
    if (act_stub.estopped) {
        level = 0.0f;
    }
    act_stub.calls++;
    act_stub.last_level = level;
    /* Trace only meaningful changes (ring, IRQ-safe: single writer) */
    float d = level - act_stub.trace[(act_stub.trace_head + ACT_TRACE_RING - 1)
                                     % ACT_TRACE_RING].level;
    if (act_stub.calls == 1 || d > 0.02f || d < -0.02f) {
        act_stub.trace[act_stub.trace_head].tick  = emg.tick;
        act_stub.trace[act_stub.trace_head].level = level;
        act_stub.trace_head = (act_stub.trace_head + 1) % ACT_TRACE_RING;
    }
    return 0;
}

__attribute__((weak)) int actuator_estop(void)
{
    act_stub.estops++;
    act_stub.estopped = 1;
    act_stub.last_level = 0.0f;
    return 0;
}

__attribute__((weak)) const char *actuator_name(void)
{
    return "stub";
}

/* cmd_emg.c introspection for the stub trace */
uint32_t emg_act_trace_copy(uint32_t *ticks, float *levels, uint32_t max)
{
    uint32_t n = act_stub.calls < ACT_TRACE_RING ? act_stub.calls : ACT_TRACE_RING;
    if (n > max) n = max;
    uint32_t start = (act_stub.trace_head + ACT_TRACE_RING - n) % ACT_TRACE_RING;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (start + i) % ACT_TRACE_RING;
        ticks[i]  = act_stub.trace[idx].tick;
        levels[i] = act_stub.trace[idx].level;
    }
    return n;
}

/* ============================================================================
 * Init / calibration
 * ============================================================================ */

void emg_init(void)
{
    if (emg_initialized) {
        return;
    }
    emg_dsp_init();
    emg_classify_reset();
    emg_synth_reset();
    synth.mode = SYNTH_IDLE;

    emg.channels   = EMG_M0_CHANNELS;
    emg.calibrated = 0;
    emg.cls        = EMG_REST;
    emg.level      = 0.0f;
    emg.tick       = 0;
    emg.last_latency_us = 0;
    emg.jitter_us_max   = 0;
    emg.estop      = 0;
    for (int ch = 0; ch < EMG_MAX_CHANNELS; ch++) {
        emg.envelope[ch] = 0.0f;
        emg.baseline[ch] = EMG_DEFAULT_BASELINE;
        emg.mvc[ch]      = EMG_DEFAULT_MVC;
    }
    emg_initialized = 1;
}

/* Sample `count` samples per channel through the full DSP chain, updating
 * emg.envelope[]. Returns via out_avg the mean envelope and via out_max
 * the peak envelope over the window (per channel). Task context only. */
static void emg_sample_window(int count, float *out_avg, float *out_max)
{
    float acc[EMG_MAX_CHANNELS];
    float peak[EMG_MAX_CHANNELS];
    int nch = emg.channels;

    for (int ch = 0; ch < nch; ch++) {
        acc[ch] = 0.0f;
        peak[ch] = 0.0f;
    }
    for (int i = 0; i < count; i++) {
        for (int ch = 0; ch < nch; ch++) {
            float env = emg_dsp_process(ch, (float)emg_hw_read(ch));
            emg.envelope[ch] = env;
            if (i >= 200) {             /* skip filter warmup transient */
                acc[ch] += env;
                if (env > peak[ch]) peak[ch] = env;
            }
        }
    }
    int n = count - 200;
    if (n < 1) n = 1;
    for (int ch = 0; ch < nch; ch++) {
        out_avg[ch] = acc[ch] / (float)n;
        out_max[ch] = peak[ch];
    }
}

/* Calibration (SPEC-M0): baseline = 2 s rest average, MVC = 2 s max.
 * Runs synchronously (task context): 2000 samples per phase per channel.
 * On synthetic source the phases are driven by the synth mode; on real
 * hardware the user rests / contracts during the phase prints. */
int emg_calibrate(void)
{
    float avg[EMG_MAX_CHANNELS], peak[EMG_MAX_CHANNELS];
    int nch;

    if (!emg_initialized) {
        emg_init();
    }
    nch = emg.channels;

    console_printf("EMG: calibration — %d channels @ %d Hz\n",
                   nch, EMG_SAMPLE_RATE_HZ);
    console_printf("EMG: phase 1/2 REST (2 s): relax%s...\n",
                   emg_using_synth() ? " (synthetic idle)" : "");
    if (emg_using_synth()) {
        emg_synth_reset();
        emg_synth_set_mode(SYNTH_IDLE);
    }
    for (int ch = 0; ch < nch; ch++) {
        emg_dsp_reset(ch);
    }
    emg_classify_reset();
    emg_sample_window(2000, avg, peak);
    for (int ch = 0; ch < nch; ch++) {
        emg.baseline[ch] = avg[ch];
    }

    console_printf("EMG: phase 2/2 MVC (2 s): contract hard%s...\n",
                   emg_using_synth() ? " (synthetic burst)" : "");
    if (emg_using_synth()) {
        emg_synth_reset();
        emg_synth_set_mode(SYNTH_BURST);
    }
    for (int ch = 0; ch < nch; ch++) {
        emg_dsp_reset(ch);
    }
    emg_sample_window(2000, avg, peak);
    for (int ch = 0; ch < nch; ch++) {
        emg.mvc[ch] = peak[ch];
        if (emg.mvc[ch] <= emg.baseline[ch] + 1.0f) {
            console_printf("EMG: WARNING ch%d MVC not above baseline "
                           "(baseline=%f mvc=%f) — calibration rejected\n",
                           ch, (double)emg.baseline[ch], (double)emg.mvc[ch]);
            emg_synth_set_mode(SYNTH_IDLE);
            return -1;
        }
    }

    emg.calibrated = 1;
    if (emg_using_synth()) {
        emg_synth_reset();
        emg_synth_set_mode(SYNTH_IDLE);
    }
    for (int ch = 0; ch < nch; ch++) {
        console_printf("EMG: ch%d baseline=%f mvc=%f\n",
                       ch, (double)emg.baseline[ch], (double)emg.mvc[ch]);
    }
    console_printf("EMG: calibration OK\n");
    return 0;
}

/* ============================================================================
 * RT tick — read -> dsp -> classify -> actuator_set (IRQ context)
 * ============================================================================ */

#define EMG_STUCK_LIMIT 500   /* raw stuck at rail for >500 ms -> estop */

static uint16_t stuck_cnt[EMG_MAX_CHANNELS];
static uint64_t last_call_tsc;

/* rt_timer callback shim (contract signature is void(void)) */
static void emg_rt_cb(uint64_t tick, void *arg)
{
    (void)tick;
    (void)arg;
    emg_rt_tick();
}

void emg_rt_tick(void)
{
    uint64_t t0 = rdtsc();

    if (!rt_active) {
        return;
    }
    emg.tick++;

    /* E-stop latched: keep the actuator torqued off, stop processing. */
    if (emg.estop) {
        return;
    }

    /* Inter-tick jitter vs the nominal period at the current tick rate */
    if (last_call_tsc != 0) {
        uint32_t rate = rt_timer_get_tick_hz();
        uint64_t expected_us = rate ? (1000000ULL / rate) : 1000ULL;
        uint64_t delta_us = tsc_to_microseconds(t0 - last_call_tsc);
        uint64_t j = (delta_us > expected_us) ? (delta_us - expected_us)
                                              : (expected_us - delta_us);
        if (j > emg.jitter_us_max) {
            emg.jitter_us_max = (uint32_t)j;
        }
    }
    last_call_tsc = t0;

    int nch = emg.channels;
    for (int ch = 0; ch < nch; ch++) {
        int raw = emg_hw_read(ch);

        /* Absurd-input guard: raw pinned at an ADC rail -> lead off /
         * saturation. >500 consecutive samples = fail safe. */
        if (raw <= 0 || raw >= 1023) {
            if (++stuck_cnt[ch] > EMG_STUCK_LIMIT) {
                emg.estop = 1;
                actuator_estop();
                return;
            }
        } else {
            stuck_cnt[ch] = 0;
        }

        emg.envelope[ch] = emg_dsp_process(ch, (float)raw);
    }

    emg_classify_tick();
    if (emg.estop) {
        return;     /* classifier latched the absurd-input guard */
    }

    actuator_set(emg.cls == EMG_CONTRACT ? emg.level : 0.0f);

    emg.last_latency_us = (uint32_t)tsc_to_microseconds(rdtsc() - t0);
}

void emg_rt_enable(int on)
{
    if (!emg_initialized) {
        emg_init();
    }
    if (on) {
        if (rt_active) {
            return;
        }
        emg.tick = 0;
        emg.jitter_us_max = 0;
        emg.last_latency_us = 0;
        last_call_tsc = 0;
        for (int ch = 0; ch < EMG_MAX_CHANNELS; ch++) {
            stuck_cnt[ch] = 0;
            emg_dsp_reset(ch);
        }
        emg_classify_reset();
        /* Demo contraction window as a fraction of a 10 s run at the
         * current rt tick rate: contract 2 s..7 s, idle before/after. */
        {
            uint32_t rate = rt_timer_get_tick_hz();
            if (rate == 0) rate = 1000;
            synth.demo_start = 2 * rate;
            synth.demo_end   = 7 * rate;
        }
        emg_dsp_warmup(300);    /* settle filters before the window starts */
        rt_timer_jitter_arm();
        if (rt_timer_register(emg_rt_cb, 1000, NULL) != 0) {
            rt_timer_jitter_disarm();
            console_printf("EMG: rt_timer_register failed\n");
            return;
        }
        rt_active = 1;
    } else {
        if (!rt_active) {
            return;
        }
        rt_timer_unregister(emg_rt_cb);
        rt_timer_jitter_disarm();
        rt_active = 0;
        actuator_set(0.0f);
    }
}

int emg_rt_is_active(void)
{
    return rt_active;
}
