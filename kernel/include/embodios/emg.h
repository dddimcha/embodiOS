#ifndef EMBODIOS_EMG_H
#define EMBODIOS_EMG_H
/* EMG pipeline contract — SPEC-M0. DO NOT EDIT (integration contract). */
#include <stdint.h>

#define EMG_MAX_CHANNELS 8
#define EMG_M0_CHANNELS 2
#define EMG_SAMPLE_RATE_HZ 1000

typedef enum { EMG_REST = 0, EMG_CONTRACT = 1 } emg_class_t;

struct emg_state {
    int channels;
    float envelope[EMG_MAX_CHANNELS];
    float baseline[EMG_MAX_CHANNELS];
    float mvc[EMG_MAX_CHANNELS];
    int calibrated;
    emg_class_t cls;
    float level;                 /* 0..1 proportional contraction level */
    uint32_t tick;               /* 1 kHz ticks since rt enable */
    uint32_t last_latency_us;    /* last tick processing time */
    uint32_t jitter_us_max;      /* max tick-to-tick jitter observed */
    int estop;                   /* latched emergency stop */
};

void  emg_init(void);
float emg_dsp_process(int ch, float raw);   /* notch+BP+rectify+envelope */
void  emg_classify_tick(void);              /* classify from envelopes */
int   emg_calibrate(void);                  /* baseline + MVC capture */
const struct emg_state *emg_get_state(void);
void  emg_rt_tick(void);                    /* 1 kHz RT entry */
void  emg_rt_enable(int on);

/* hardware source — weak defaults (synthetic); WS-DRV overrides on hw boards */
int emg_hw_available(void);
int emg_hw_read(int ch);                    /* raw ADC counts, <0 on error */

#endif
