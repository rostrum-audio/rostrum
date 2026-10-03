/* Shared helpers for the Rostrum mic filter plugins. Everything in the run path is
 * allocation-free and lock-free; state that can decay towards zero is flushed so
 * silence never turns into denormal arithmetic. */
#ifndef ROSTRUM_DSP_COMMON_H
#define ROSTRUM_DSP_COMMON_H

#include "ladspa_abi.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RD_MAKER "Rostrum"
#define RD_COPYRIGHT "GPL-3.0-or-later"
#define RD_PROPERTIES (LADSPA_PROPERTY_REALTIME | LADSPA_PROPERTY_HARD_RT_CAPABLE)

/* Parameters glide towards their targets once per tick, so moving a control never
 * zippers and the output does not depend on how the host splits blocks. */
#define RD_TICK 32

#define RD_IN_AUDIO (LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO)
#define RD_OUT_AUDIO (LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO)
#define RD_IN_CONTROL (LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL)
#define RD_OUT_CONTROL (LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL)
#define RD_BOUNDED (LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE)
#define RD_TOGGLE_ON (LADSPA_HINT_TOGGLED | LADSPA_HINT_DEFAULT_1)

static inline float rd_clamp(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float rd_db_to_gain(float db)
{
    return powf(10.0f, db * 0.05f);
}

static inline float rd_gain_to_db(float gain)
{
    return 20.0f * log10f(gain > 1e-9f ? gain : 1e-9f);
}

static inline float rd_flush(float v)
{
    return fabsf(v) < 1e-20f ? 0.0f : v;
}

/* Reads a control port, falling back to the default for unconnected or non-finite input. */
static inline float rd_control(const LADSPA_Data *port, float def, float lo, float hi)
{
    float v = port ? *port : def;
    if (!isfinite(v))
        v = def;
    return rd_clamp(v, lo, hi);
}

static inline int rd_switch(const LADSPA_Data *port)
{
    return port ? (isfinite(*port) && *port > 0.5f) : 1;
}

static inline void rd_set_output(LADSPA_Data *port, float value)
{
    if (port)
        *port = value;
}

/* One-pole coefficient: the state covers 63% of a step in `ms` milliseconds. */
static inline float rd_time_coef(float ms, float rate)
{
    if (ms <= 0.0f || rate <= 0.0f)
        return 0.0f;
    return expf(-1000.0f / (ms * rate));
}

static inline float rd_glide(float current, float target, float coef)
{
    float v = target + (current - target) * coef;
    return fabsf(v - target) < 1e-6f ? target : v;
}

/* A short linear crossfade between the dry and processed signal. */
typedef struct {
    float mix;
    float step;
} rd_bypass;

static inline void rd_bypass_init(rd_bypass *b, float rate, int enabled)
{
    b->step = 1.0f / (0.02f * rate);
    b->mix = enabled ? 1.0f : 0.0f;
}

static inline float rd_bypass_next(rd_bypass *b, int enabled)
{
    if (enabled) {
        b->mix += b->step;
        if (b->mix > 1.0f)
            b->mix = 1.0f;
    } else {
        b->mix -= b->step;
        if (b->mix < 0.0f)
            b->mix = 0.0f;
    }
    return b->mix;
}

static inline int rd_bypass_idle(const rd_bypass *b, int enabled)
{
    return !enabled && b->mix <= 0.0f;
}

/* A fixed delay, sized at instantiate time. */
typedef struct {
    float *buf;
    unsigned len;
    unsigned pos;
} rd_delay;

static inline int rd_delay_alloc(rd_delay *d, unsigned len)
{
    d->len = len;
    d->pos = 0;
    d->buf = len ? (float *)calloc(len, sizeof(float)) : NULL;
    return len == 0 || d->buf != NULL;
}

static inline void rd_delay_clear(rd_delay *d)
{
    d->pos = 0;
    if (d->buf)
        memset(d->buf, 0, d->len * sizeof(float));
}

static inline float rd_delay_push(rd_delay *d, float x)
{
    if (!d->len)
        return x;
    float y = d->buf[d->pos];
    d->buf[d->pos] = x;
    if (++d->pos == d->len)
        d->pos = 0;
    return y;
}

static inline void rd_delay_free(rd_delay *d)
{
    free(d->buf);
    d->buf = NULL;
}

/* Linear trapezoidal state variable filter (Simper), stable under modulation. */
typedef enum {
    RD_SVF_HIGHPASS,
    RD_SVF_LOWSHELF,
    RD_SVF_HIGHSHELF,
    RD_SVF_BELL,
} rd_svf_type;

typedef struct {
    float ic1, ic2;
    float a1, a2, a3;
    float m0, m1, m2;
} rd_svf;

static inline void rd_svf_reset(rd_svf *f)
{
    f->ic1 = f->ic2 = 0.0f;
}

static inline void rd_svf_set(rd_svf *f, rd_svf_type type, float freq, float q, float gain_db, float rate)
{
    freq = rd_clamp(freq, 10.0f, 0.45f * rate);
    const float A = powf(10.0f, gain_db / 40.0f);
    float g = tanf((float)M_PI * freq / rate);
    float k = 1.0f / q;

    switch (type) {
    case RD_SVF_HIGHPASS:
        f->m0 = 1.0f;
        f->m1 = -k;
        f->m2 = -1.0f;
        break;
    case RD_SVF_LOWSHELF:
        g /= sqrtf(A);
        f->m0 = 1.0f;
        f->m1 = k * (A - 1.0f);
        f->m2 = A * A - 1.0f;
        break;
    case RD_SVF_HIGHSHELF:
        g *= sqrtf(A);
        f->m0 = A * A;
        f->m1 = k * (1.0f - A) * A;
        f->m2 = 1.0f - A * A;
        break;
    case RD_SVF_BELL:
        k = 1.0f / (q * A);
        f->m0 = 1.0f;
        f->m1 = k * (A * A - 1.0f);
        f->m2 = 0.0f;
        break;
    }
    f->a1 = 1.0f / (1.0f + g * (g + k));
    f->a2 = g * f->a1;
    f->a3 = g * f->a2;
}

static inline float rd_svf_run(rd_svf *f, float v0)
{
    const float v3 = v0 - f->ic2;
    const float v1 = f->a1 * f->ic1 + f->a2 * v3;
    const float v2 = f->ic2 + f->a2 * f->ic1 + f->a3 * v3;
    f->ic1 = 2.0f * v1 - f->ic1;
    f->ic2 = 2.0f * v2 - f->ic2;
    return f->m0 * v0 + f->m1 * v1 + f->m2 * v2;
}

static inline void rd_svf_flush(rd_svf *f)
{
    f->ic1 = rd_flush(f->ic1);
    f->ic2 = rd_flush(f->ic2);
}

extern const LADSPA_Descriptor rd_highpass_descriptor;
extern const LADSPA_Descriptor rd_eq_descriptor;
extern const LADSPA_Descriptor rd_gate_descriptor;
extern const LADSPA_Descriptor rd_compressor_descriptor;
extern const LADSPA_Descriptor rd_limiter_descriptor;
#ifdef ROSTRUM_DSP_DENOISE
extern const LADSPA_Descriptor rd_denoise_descriptor;
#endif

#endif
