/* Noise gate: turns the mic down by `Range` while the level stays under `Threshold`.
 * Hysteresis and a hold time keep it from chattering on word endings. */
#include "dsp_common.h"

enum { P_IN, P_OUT, P_ENABLED, P_THRESHOLD, P_RANGE, P_ATTACK, P_HOLD, P_RELEASE, P_REDUCTION, P_COUNT };

#define HYSTERESIS_DB 4.0f
#define DETECTOR_MS 15.0f

typedef struct {
    LADSPA_Data *port[P_COUNT];
    float rate;
    float detector;
    float envelope;
    float gain_db;
    float gain;
    float gain_cached_db;
    unsigned long hold_left;
    int open;
    rd_bypass bypass;
} Gate;

static LADSPA_Handle instantiate(const LADSPA_Descriptor *d, unsigned long rate)
{
    (void)d;
    Gate *g = (Gate *)calloc(1, sizeof(Gate));
    if (!g)
        return NULL;
    g->rate = (float)rate;
    g->detector = rd_time_coef(DETECTOR_MS, g->rate);
    return g;
}

static void connect_port(LADSPA_Handle handle, unsigned long port, LADSPA_Data *data)
{
    if (port < P_COUNT)
        ((Gate *)handle)->port[port] = data;
}

static void activate(LADSPA_Handle handle)
{
    Gate *g = (Gate *)handle;
    g->envelope = 0.0f;
    g->open = 0;
    g->hold_left = 0;
    g->gain_db = rd_control(g->port[P_RANGE], -20.0f, -80.0f, 0.0f);
    g->gain_cached_db = g->gain_db;
    g->gain = rd_db_to_gain(g->gain_db);
    rd_bypass_init(&g->bypass, g->rate, rd_switch(g->port[P_ENABLED]));
}

static void run(LADSPA_Handle handle, unsigned long n)
{
    Gate *g = (Gate *)handle;
    const LADSPA_Data *in = g->port[P_IN];
    LADSPA_Data *out = g->port[P_OUT];
    const int enabled = rd_switch(g->port[P_ENABLED]);
    const float threshold = rd_db_to_gain(rd_control(g->port[P_THRESHOLD], -50.0f, -80.0f, -10.0f));
    const float close_below = threshold * rd_db_to_gain(-HYSTERESIS_DB);
    const float range_db = rd_control(g->port[P_RANGE], -20.0f, -80.0f, 0.0f);
    const float attack = rd_time_coef(rd_control(g->port[P_ATTACK], 2.0f, 0.5f, 50.0f), g->rate);
    const float release = rd_time_coef(rd_control(g->port[P_RELEASE], 150.0f, 20.0f, 2000.0f), g->rate);
    const unsigned long hold = (unsigned long)(rd_control(g->port[P_HOLD], 200.0f, 0.0f, 1000.0f) * g->rate / 1000.0f);
    float deepest = 0.0f;

    for (unsigned long i = 0; i < n; i++) {
        const float x = in[i];
        const float level = fabsf(x);
        g->envelope = level > g->envelope ? level : rd_flush(g->envelope * g->detector);

        if (g->envelope >= threshold) {
            g->open = 1;
            g->hold_left = hold;
        } else if (g->envelope < close_below) {
            if (g->hold_left > 0)
                g->hold_left--;
            else
                g->open = 0;
        }

        const float target = g->open ? 0.0f : range_db;
        const float coef = target > g->gain_db ? attack : release;
        g->gain_db = rd_glide(g->gain_db, target, coef);
        if (g->gain_db != g->gain_cached_db) {
            g->gain_cached_db = g->gain_db;
            g->gain = rd_db_to_gain(g->gain_db);
        }
        if (g->gain_db < deepest)
            deepest = g->gain_db;

        const float mix = rd_bypass_next(&g->bypass, enabled);
        out[i] = x * (1.0f + (g->gain - 1.0f) * mix);
    }
    rd_set_output(g->port[P_REDUCTION], rd_bypass_idle(&g->bypass, enabled) ? 0.0f : -deepest);
}

static void cleanup(LADSPA_Handle handle)
{
    free(handle);
}

static const LADSPA_PortDescriptor port_desc[P_COUNT] = {
    RD_IN_AUDIO, RD_OUT_AUDIO, RD_IN_CONTROL, RD_IN_CONTROL, RD_IN_CONTROL,
    RD_IN_CONTROL, RD_IN_CONTROL, RD_IN_CONTROL, RD_OUT_CONTROL,
};

static const char *const port_names[P_COUNT] = {
    "In", "Out", "Enabled", "Threshold", "Range", "Attack", "Hold", "Release", "Reduction",
};

static const LADSPA_PortRangeHint port_hints[P_COUNT] = {
    { 0, 0, 0 },
    { 0, 0, 0 },
    { RD_TOGGLE_ON, 0, 1 },
    { RD_BOUNDED | LADSPA_HINT_DEFAULT_MIDDLE, -80.0f, -10.0f },
    { RD_BOUNDED | LADSPA_HINT_DEFAULT_HIGH, -80.0f, 0.0f },
    { RD_BOUNDED | LADSPA_HINT_LOGARITHMIC | LADSPA_HINT_DEFAULT_LOW, 0.5f, 50.0f },
    { RD_BOUNDED | LADSPA_HINT_DEFAULT_LOW, 0.0f, 1000.0f },
    { RD_BOUNDED | LADSPA_HINT_LOGARITHMIC | LADSPA_HINT_DEFAULT_LOW, 20.0f, 2000.0f },
    { RD_BOUNDED, 0.0f, 80.0f },
};

const LADSPA_Descriptor rd_gate_descriptor = {
    .UniqueID = 0x52440003,
    .Label = "rostrum_gate",
    .Properties = RD_PROPERTIES,
    .Name = "Rostrum noise gate",
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
