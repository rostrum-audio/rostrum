/* Feed-forward soft-knee compressor with a smooth decoupled peak detector
 * (Giannoulis, Massberg and Reiss, 2012) and make-up gain. */
#include "dsp_common.h"

enum { P_IN, P_OUT, P_ENABLED, P_THRESHOLD, P_RATIO, P_KNEE, P_ATTACK, P_RELEASE, P_MAKEUP, P_REDUCTION, P_COUNT };

typedef struct {
    LADSPA_Data *port[P_COUNT];
    float rate;
    float glide;
    float reduction;
    float makeup_db;
    float applied_db;
    float applied;
    unsigned tick;
    rd_bypass bypass;
} Compressor;

/* Gain reduction in dB (>= 0) for an input level in dB. */
static inline float curve(float level_db, float threshold, float slope, float knee)
{
    const float over = level_db - threshold;
    if (2.0f * over <= -knee)
        return 0.0f;
    if (knee > 0.0f && 2.0f * over < knee) {
        const float t = over + knee * 0.5f;
        return -slope * t * t / (2.0f * knee);
    }
    return -slope * over;
}

static LADSPA_Handle instantiate(const LADSPA_Descriptor *d, unsigned long rate)
{
    (void)d;
    Compressor *c = (Compressor *)calloc(1, sizeof(Compressor));
    if (!c)
        return NULL;
    c->rate = (float)rate;
    c->glide = rd_time_coef(30.0f, c->rate / RD_TICK);
    return c;
}

static void connect_port(LADSPA_Handle handle, unsigned long port, LADSPA_Data *data)
{
    if (port < P_COUNT)
        ((Compressor *)handle)->port[port] = data;
}

static void activate(LADSPA_Handle handle)
{
    Compressor *c = (Compressor *)handle;
    c->reduction = 0.0f;
    c->tick = 0;
    c->makeup_db = rd_control(c->port[P_MAKEUP], 0.0f, 0.0f, 24.0f);
    c->applied_db = c->makeup_db;
    c->applied = rd_db_to_gain(c->applied_db);
    rd_bypass_init(&c->bypass, c->rate, rd_switch(c->port[P_ENABLED]));
}

static void run(LADSPA_Handle handle, unsigned long n)
{
    Compressor *c = (Compressor *)handle;
    const LADSPA_Data *in = c->port[P_IN];
    LADSPA_Data *out = c->port[P_OUT];
    const int enabled = rd_switch(c->port[P_ENABLED]);
    const float threshold = rd_control(c->port[P_THRESHOLD], -20.0f, -60.0f, 0.0f);
    const float ratio = rd_control(c->port[P_RATIO], 3.0f, 1.0f, 20.0f);
    const float knee = rd_control(c->port[P_KNEE], 6.0f, 0.0f, 24.0f);
    const float attack = rd_time_coef(rd_control(c->port[P_ATTACK], 8.0f, 0.1f, 200.0f), c->rate);
    const float release = rd_time_coef(rd_control(c->port[P_RELEASE], 150.0f, 10.0f, 2000.0f), c->rate);
    const float makeup = rd_control(c->port[P_MAKEUP], 0.0f, 0.0f, 24.0f);
    const float slope = 1.0f / ratio - 1.0f;
    /* Below the knee there is nothing to compute, so skip the log. */
    const float knee_start = rd_db_to_gain(threshold - knee * 0.5f);
    float deepest = 0.0f;

    for (unsigned long i = 0; i < n; i++) {
        if (c->tick == 0) {
            c->makeup_db = rd_glide(c->makeup_db, makeup, c->glide);
            c->reduction = rd_flush(c->reduction);
        }
        c->tick = (c->tick + 1) % RD_TICK;

        const float x = in[i];
        const float level = fabsf(x);
        const float target = level > knee_start ? curve(rd_gain_to_db(level), threshold, slope, knee) : 0.0f;
        const float coef = target > c->reduction ? attack : release;
        c->reduction = target + (c->reduction - target) * coef;
        if (c->reduction < 1e-5f)
            c->reduction = 0.0f;
        if (c->reduction > deepest)
            deepest = c->reduction;

        const float gain_db = c->makeup_db - c->reduction;
        if (gain_db != c->applied_db) {
            c->applied_db = gain_db;
            c->applied = rd_db_to_gain(gain_db);
        }
        const float mix = rd_bypass_next(&c->bypass, enabled);
        out[i] = x * (1.0f + (c->applied - 1.0f) * mix);
    }
    rd_set_output(c->port[P_REDUCTION], rd_bypass_idle(&c->bypass, enabled) ? 0.0f : deepest);
}

static void cleanup(LADSPA_Handle handle)
{
    free(handle);
}

static const LADSPA_PortDescriptor port_desc[P_COUNT] = {
    RD_IN_AUDIO, RD_OUT_AUDIO, RD_IN_CONTROL, RD_IN_CONTROL, RD_IN_CONTROL,
    RD_IN_CONTROL, RD_IN_CONTROL, RD_IN_CONTROL, RD_IN_CONTROL, RD_OUT_CONTROL,
};

static const char *const port_names[P_COUNT] = {
    "In", "Out", "Enabled", "Threshold", "Ratio", "Knee", "Attack", "Release", "Makeup", "Reduction",
};

static const LADSPA_PortRangeHint port_hints[P_COUNT] = {
    { 0, 0, 0 },
    { 0, 0, 0 },
    { RD_TOGGLE_ON, 0, 1 },
    { RD_BOUNDED | LADSPA_HINT_DEFAULT_HIGH, -60.0f, 0.0f },
    { RD_BOUNDED | LADSPA_HINT_LOGARITHMIC | LADSPA_HINT_DEFAULT_LOW, 1.0f, 20.0f },
    { RD_BOUNDED | LADSPA_HINT_DEFAULT_LOW, 0.0f, 24.0f },
    { RD_BOUNDED | LADSPA_HINT_LOGARITHMIC | LADSPA_HINT_DEFAULT_LOW, 0.1f, 200.0f },
    { RD_BOUNDED | LADSPA_HINT_LOGARITHMIC | LADSPA_HINT_DEFAULT_LOW, 10.0f, 2000.0f },
    { RD_BOUNDED | LADSPA_HINT_DEFAULT_0, 0.0f, 24.0f },
    { RD_BOUNDED, 0.0f, 60.0f },
};

const LADSPA_Descriptor rd_compressor_descriptor = {
    .UniqueID = 0x52440004,
    .Label = "rostrum_compressor",
    .Properties = RD_PROPERTIES,
    .Name = "Rostrum compressor",
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
