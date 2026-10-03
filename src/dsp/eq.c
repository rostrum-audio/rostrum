/* Voice EQ: low shelf (warmth), mud bell, presence bell and high shelf (air). */
#include "dsp_common.h"

enum {
    P_IN,
    P_OUT,
    P_ENABLED,
    P_LOW_GAIN,
    P_LOW_FREQ,
    P_MUD_GAIN,
    P_MUD_FREQ,
    P_PRESENCE_GAIN,
    P_PRESENCE_FREQ,
    P_AIR_GAIN,
    P_AIR_FREQ,
    P_COUNT
};

#define BANDS 4

typedef struct {
    rd_svf_type type;
    float q;
    int gain_port, freq_port;
    float gain_min, gain_max;
    float freq_def, freq_min, freq_max;
} Band;

static const Band bands[BANDS] = {
    { RD_SVF_LOWSHELF, 0.70710678f, P_LOW_GAIN, P_LOW_FREQ, -12.0f, 12.0f, 120.0f, 40.0f, 400.0f },
    { RD_SVF_BELL, 1.0f, P_MUD_GAIN, P_MUD_FREQ, -12.0f, 6.0f, 350.0f, 150.0f, 1000.0f },
    { RD_SVF_BELL, 0.9f, P_PRESENCE_GAIN, P_PRESENCE_FREQ, -6.0f, 12.0f, 4000.0f, 1500.0f, 8000.0f },
    { RD_SVF_HIGHSHELF, 0.70710678f, P_AIR_GAIN, P_AIR_FREQ, -6.0f, 12.0f, 10000.0f, 5000.0f, 16000.0f },
};

typedef struct {
    LADSPA_Data *port[P_COUNT];
    float rate;
    float glide;
    float gain[BANDS];
    float freq[BANDS];
    unsigned tick;
    rd_svf filter[BANDS];
    rd_bypass bypass;
} Eq;

static void update(Eq *e, int jump)
{
    for (int b = 0; b < BANDS; b++) {
        const Band *band = &bands[b];
        const float gain = rd_control(e->port[band->gain_port], 0.0f, band->gain_min, band->gain_max);
        const float freq = rd_control(e->port[band->freq_port], band->freq_def, band->freq_min, band->freq_max);
        e->gain[b] = jump ? gain : rd_glide(e->gain[b], gain, e->glide);
        e->freq[b] = jump ? freq : rd_glide(e->freq[b], freq, e->glide);
        rd_svf_set(&e->filter[b], band->type, e->freq[b], band->q, e->gain[b], e->rate);
    }
}

static LADSPA_Handle instantiate(const LADSPA_Descriptor *d, unsigned long rate)
{
    (void)d;
    Eq *e = (Eq *)calloc(1, sizeof(Eq));
    if (!e)
        return NULL;
    e->rate = (float)rate;
    e->glide = rd_time_coef(30.0f, e->rate / RD_TICK);
    return e;
}

static void connect_port(LADSPA_Handle handle, unsigned long port, LADSPA_Data *data)
{
    if (port < P_COUNT)
        ((Eq *)handle)->port[port] = data;
}

static void activate(LADSPA_Handle handle)
{
    Eq *e = (Eq *)handle;
    for (int b = 0; b < BANDS; b++)
        rd_svf_reset(&e->filter[b]);
    e->tick = 0;
    update(e, 1);
    rd_bypass_init(&e->bypass, e->rate, rd_switch(e->port[P_ENABLED]));
}

static void run(LADSPA_Handle handle, unsigned long n)
{
    Eq *e = (Eq *)handle;
    const LADSPA_Data *in = e->port[P_IN];
    LADSPA_Data *out = e->port[P_OUT];
    const int enabled = rd_switch(e->port[P_ENABLED]);

    for (unsigned long i = 0; i < n; i++) {
        if (e->tick == 0) {
            update(e, 0);
            for (int b = 0; b < BANDS; b++)
                rd_svf_flush(&e->filter[b]);
        }
        e->tick = (e->tick + 1) % RD_TICK;

        const float x = in[i];
        float wet = x;
        for (int b = 0; b < BANDS; b++)
            wet = rd_svf_run(&e->filter[b], wet);
        const float mix = rd_bypass_next(&e->bypass, enabled);
        out[i] = x + (wet - x) * mix;
    }
}

static void cleanup(LADSPA_Handle handle)
{
    free(handle);
}

static const LADSPA_PortDescriptor port_desc[P_COUNT] = {
    RD_IN_AUDIO, RD_OUT_AUDIO, RD_IN_CONTROL, RD_IN_CONTROL, RD_IN_CONTROL, RD_IN_CONTROL,
    RD_IN_CONTROL, RD_IN_CONTROL, RD_IN_CONTROL, RD_IN_CONTROL, RD_IN_CONTROL,
};

static const char *const port_names[P_COUNT] = {
    "In", "Out", "Enabled",
    "Low gain", "Low frequency",
    "Mud gain", "Mud frequency",
    "Presence gain", "Presence frequency",
    "Air gain", "Air frequency",
};

#define GAIN_HINT (RD_BOUNDED | LADSPA_HINT_DEFAULT_0)
#define FREQ_HINT (RD_BOUNDED | LADSPA_HINT_LOGARITHMIC | LADSPA_HINT_DEFAULT_MIDDLE)

static const LADSPA_PortRangeHint port_hints[P_COUNT] = {
    { 0, 0, 0 },
    { 0, 0, 0 },
    { RD_TOGGLE_ON, 0, 1 },
    { GAIN_HINT, -12.0f, 12.0f },
    { FREQ_HINT, 40.0f, 400.0f },
    { GAIN_HINT, -12.0f, 6.0f },
    { FREQ_HINT, 150.0f, 1000.0f },
    { GAIN_HINT, -6.0f, 12.0f },
    { FREQ_HINT, 1500.0f, 8000.0f },
    { GAIN_HINT, -6.0f, 12.0f },
    { FREQ_HINT, 5000.0f, 16000.0f },
};

const LADSPA_Descriptor rd_eq_descriptor = {
    .UniqueID = 0x52440002,
    .Label = "rostrum_eq",
    .Properties = RD_PROPERTIES,
    .Name = "Rostrum voice EQ",
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
