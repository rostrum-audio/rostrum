/* Rumble filter: a Butterworth high-pass, 12 dB/octave or 24 dB/octave when steep. */
#include "dsp_common.h"

enum { P_IN, P_OUT, P_ENABLED, P_FREQ, P_STEEP, P_COUNT };

#define FREQ_DEF 80.0f
#define FREQ_MIN 20.0f
#define FREQ_MAX 300.0f

/* Butterworth pole Qs: one stage for 2nd order, two stages for 4th order. */
#define Q_2ND 0.70710678f
#define Q_4TH_A 0.54119610f
#define Q_4TH_B 1.30656296f

typedef struct {
    LADSPA_Data *port[P_COUNT];
    float rate;
    float glide;
    float freq;
    float steep;
    unsigned tick;
    rd_svf stage[2];
    rd_bypass bypass;
} Highpass;

static void update(Highpass *h, int jump)
{
    const float freq = rd_control(h->port[P_FREQ], FREQ_DEF, FREQ_MIN, FREQ_MAX);
    const float steep = rd_switch(h->port[P_STEEP]) ? 1.0f : 0.0f;
    h->freq = jump ? freq : rd_glide(h->freq, freq, h->glide);
    h->steep = jump ? steep : rd_glide(h->steep, steep, h->glide);
    const float qa = Q_2ND + (Q_4TH_A - Q_2ND) * h->steep;
    rd_svf_set(&h->stage[0], RD_SVF_HIGHPASS, h->freq, qa, 0.0f, h->rate);
    rd_svf_set(&h->stage[1], RD_SVF_HIGHPASS, h->freq, Q_4TH_B, 0.0f, h->rate);
}

static LADSPA_Handle instantiate(const LADSPA_Descriptor *d, unsigned long rate)
{
    (void)d;
    Highpass *h = (Highpass *)calloc(1, sizeof(Highpass));
    if (!h)
        return NULL;
    h->rate = (float)rate;
    h->glide = rd_time_coef(30.0f, h->rate / RD_TICK);
    return h;
}

static void connect_port(LADSPA_Handle handle, unsigned long port, LADSPA_Data *data)
{
    if (port < P_COUNT)
        ((Highpass *)handle)->port[port] = data;
}

static void activate(LADSPA_Handle handle)
{
    Highpass *h = (Highpass *)handle;
    rd_svf_reset(&h->stage[0]);
    rd_svf_reset(&h->stage[1]);
    h->tick = 0;
    update(h, 1);
    rd_bypass_init(&h->bypass, h->rate, rd_switch(h->port[P_ENABLED]));
}

static void run(LADSPA_Handle handle, unsigned long n)
{
    Highpass *h = (Highpass *)handle;
    const LADSPA_Data *in = h->port[P_IN];
    LADSPA_Data *out = h->port[P_OUT];
    const int enabled = rd_switch(h->port[P_ENABLED]);

    for (unsigned long i = 0; i < n; i++) {
        if (h->tick == 0) {
            update(h, 0);
            rd_svf_flush(&h->stage[0]);
            rd_svf_flush(&h->stage[1]);
        }
        h->tick = (h->tick + 1) % RD_TICK;

        const float x = in[i];
        const float a = rd_svf_run(&h->stage[0], x);
        const float b = rd_svf_run(&h->stage[1], a);
        const float wet = a + (b - a) * h->steep;
        const float mix = rd_bypass_next(&h->bypass, enabled);
        out[i] = x + (wet - x) * mix;
    }
}

static void cleanup(LADSPA_Handle handle)
{
    free(handle);
}

static const LADSPA_PortDescriptor port_desc[P_COUNT] = {
    RD_IN_AUDIO, RD_OUT_AUDIO, RD_IN_CONTROL, RD_IN_CONTROL, RD_IN_CONTROL,
};

static const char *const port_names[P_COUNT] = {
    "In", "Out", "Enabled", "Frequency", "Steep",
};

static const LADSPA_PortRangeHint port_hints[P_COUNT] = {
    { 0, 0, 0 },
    { 0, 0, 0 },
    { RD_TOGGLE_ON, 0, 1 },
    { RD_BOUNDED | LADSPA_HINT_LOGARITHMIC | LADSPA_HINT_DEFAULT_LOW, FREQ_MIN, FREQ_MAX },
    { LADSPA_HINT_TOGGLED | LADSPA_HINT_DEFAULT_0, 0, 1 },
};

const LADSPA_Descriptor rd_highpass_descriptor = {
    .UniqueID = 0x52440001,
    .Label = "rostrum_highpass",
    .Properties = RD_PROPERTIES,
    .Name = "Rostrum rumble filter",
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
