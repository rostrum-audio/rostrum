/* RNNoise noise suppression. RNNoise works on 10 ms frames at 48 kHz, so the plugin
 * buffers one frame (reported as latency) and keeps the dry path aligned with it.
 * At any other rate it passes audio through and reports itself inactive. */
#include "dsp_common.h"

#include <rnnoise.h>

enum { P_IN, P_OUT, P_ENABLED, P_STRENGTH, P_VOICE_THRESHOLD, P_VOICE, P_ACTIVE, P_LATENCY, P_COUNT };

#define FRAME 480
#define RNNOISE_RATE 48000
#define VOICE_HOLD_FRAMES 20
#define PCM_SCALE 32768.0f

typedef struct {
    LADSPA_Data *port[P_COUNT];
    float rate;
    int active;
    DenoiseState *state;
    float input[FRAME];
    float dry[FRAME];
    float wet[FRAME];
    float scratch[FRAME];
    unsigned pos;
    float voice;
    unsigned voice_hold;
    float voice_gain;
    float voice_open;
    float voice_close;
    float strength;
    float strength_step;
    rd_bypass bypass;
} Denoise;

static LADSPA_Handle instantiate(const LADSPA_Descriptor *d, unsigned long rate)
{
    (void)d;
    Denoise *dn = (Denoise *)calloc(1, sizeof(Denoise));
    if (!dn)
        return NULL;
    dn->rate = (float)rate;
    dn->active = rate == RNNOISE_RATE;
    if (dn->active) {
        dn->state = rnnoise_create(NULL);
        if (!dn->state) {
            free(dn);
            return NULL;
        }
    }
    dn->voice_open = rd_time_coef(2.0f, dn->rate);
    dn->voice_close = rd_time_coef(40.0f, dn->rate);
    dn->strength_step = 1.0f / (0.05f * dn->rate);
    return dn;
}

static void connect_port(LADSPA_Handle handle, unsigned long port, LADSPA_Data *data)
{
    if (port < P_COUNT)
        ((Denoise *)handle)->port[port] = data;
}

static void report(Denoise *dn)
{
    rd_set_output(dn->port[P_VOICE], dn->voice);
    rd_set_output(dn->port[P_ACTIVE], dn->active ? 1.0f : 0.0f);
    rd_set_output(dn->port[P_LATENCY], dn->active ? (float)FRAME : 0.0f);
}

static void activate(LADSPA_Handle handle)
{
    Denoise *dn = (Denoise *)handle;
    if (dn->state)
        rnnoise_init(dn->state, NULL);
    memset(dn->input, 0, sizeof(dn->input));
    memset(dn->dry, 0, sizeof(dn->dry));
    memset(dn->wet, 0, sizeof(dn->wet));
    dn->pos = 0;
    dn->voice = 0.0f;
    dn->voice_hold = 0;
    dn->voice_gain = 1.0f;
    dn->strength = rd_control(dn->port[P_STRENGTH], 1.0f, 0.0f, 1.0f);
    rd_bypass_init(&dn->bypass, dn->rate, rd_switch(dn->port[P_ENABLED]));
    report(dn);
}

static void process_frame(Denoise *dn, int enabled)
{
    memcpy(dn->dry, dn->input, sizeof(dn->dry));
    if (!enabled && dn->bypass.mix <= 0.0f) {
        memcpy(dn->wet, dn->input, sizeof(dn->wet));
        dn->voice = 0.0f;
        return;
    }
    for (unsigned i = 0; i < FRAME; i++)
        dn->scratch[i] = dn->input[i] * PCM_SCALE;
    const float voice = rnnoise_process_frame(dn->state, dn->wet, dn->scratch);
    for (unsigned i = 0; i < FRAME; i++)
        dn->wet[i] = dn->wet[i] / PCM_SCALE;
    dn->voice = isfinite(voice) ? rd_clamp(voice, 0.0f, 1.0f) : 0.0f;
}

static void run(LADSPA_Handle handle, unsigned long n)
{
    Denoise *dn = (Denoise *)handle;
    const LADSPA_Data *in = dn->port[P_IN];
    LADSPA_Data *out = dn->port[P_OUT];

    if (!dn->active) {
        if (out != in)
            memmove(out, in, n * sizeof(LADSPA_Data));
        report(dn);
        return;
    }

    const int enabled = rd_switch(dn->port[P_ENABLED]);
    const float strength = rd_control(dn->port[P_STRENGTH], 1.0f, 0.0f, 1.0f);
    const float threshold = rd_control(dn->port[P_VOICE_THRESHOLD], 0.0f, 0.0f, 0.95f);

    for (unsigned long i = 0; i < n; i++) {
        const float x = in[i];
        const float dry = dn->dry[dn->pos];
        const float denoised = dn->wet[dn->pos];
        dn->input[dn->pos] = x;

        if (dn->strength < strength)
            dn->strength = fminf(strength, dn->strength + dn->strength_step);
        else if (dn->strength > strength)
            dn->strength = fmaxf(strength, dn->strength - dn->strength_step);

        const float target = (threshold <= 0.0f || dn->voice_hold > 0) ? 1.0f : 0.0f;
        const float coef = target > dn->voice_gain ? dn->voice_open : dn->voice_close;
        dn->voice_gain = rd_flush(target + (dn->voice_gain - target) * coef);

        const float wet = (dry + (denoised - dry) * dn->strength) * dn->voice_gain;
        const float mix = rd_bypass_next(&dn->bypass, enabled);
        out[i] = dry + (wet - dry) * mix;

        if (++dn->pos == FRAME) {
            dn->pos = 0;
            process_frame(dn, enabled);
            if (dn->voice >= threshold && threshold > 0.0f)
                dn->voice_hold = VOICE_HOLD_FRAMES;
            else if (dn->voice_hold > 0)
                dn->voice_hold--;
        }
    }
    report(dn);
}

static void cleanup(LADSPA_Handle handle)
{
    Denoise *dn = (Denoise *)handle;
    if (dn->state)
        rnnoise_destroy(dn->state);
    free(dn);
}

static const LADSPA_PortDescriptor port_desc[P_COUNT] = {
    RD_IN_AUDIO, RD_OUT_AUDIO, RD_IN_CONTROL, RD_IN_CONTROL, RD_IN_CONTROL,
    RD_OUT_CONTROL, RD_OUT_CONTROL, RD_OUT_CONTROL,
};

static const char *const port_names[P_COUNT] = {
    "In", "Out", "Enabled", "Strength", "Voice threshold", "Voice", "Active", "latency",
};

static const LADSPA_PortRangeHint port_hints[P_COUNT] = {
    { 0, 0, 0 },
    { 0, 0, 0 },
    { RD_TOGGLE_ON, 0, 1 },
    { RD_BOUNDED | LADSPA_HINT_DEFAULT_1, 0.0f, 1.0f },
    { RD_BOUNDED | LADSPA_HINT_DEFAULT_0, 0.0f, 0.95f },
    { RD_BOUNDED, 0.0f, 1.0f },
    { RD_BOUNDED, 0.0f, 1.0f },
    { 0, 0, 0 },
};

const LADSPA_Descriptor rd_denoise_descriptor = {
    .UniqueID = 0x52440006,
    .Label = "rostrum_denoise",
    .Properties = RD_PROPERTIES,
    .Name = "Rostrum noise suppression (RNNoise)",
    .Maker = RD_MAKER,
    .Copyright = RD_COPYRIGHT,
    .PortCount = P_COUNT,
    .PortDescriptors = port_desc,
    .PortNames = port_names,
    .PortRangeHints = port_hints,
    .ImplementationData = NULL,
    .instantiate = instantiate,
    .connect_port = connect_port,
    .activate = activate,
    .run = run,
    .run_adding = NULL,
    .set_run_adding_gain = NULL,
    .deactivate = NULL,
    .cleanup = cleanup,
};
