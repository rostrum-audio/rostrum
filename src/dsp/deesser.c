/* Soft de-esser: a bandpass detector drives a dynamic high shelf that cuts only
 * while sibilance is loud, so presence EQ stays clear without a permanent lisp. */
#include "dsp_common.h"

enum {
    P_IN,
    P_OUT,
    P_ENABLED,
    P_THRESHOLD,
    P_FREQUENCY,
    P_RANGE,
    P_ATTACK,
    P_RELEASE,
    P_REDUCTION,
    P_COUNT
};

#define DETECTOR_Q 2.5f
#define SHELF_Q 0.70710678f

typedef struct {
    LADSPA_Data *port[P_COUNT];
    float rate;
    float glide;
    float freq;
    float cut_db;
    float envelope;
    unsigned tick;
    rd_svf detector;
    rd_svf shelf;
    rd_bypass bypass;
} Deesser;

static void update_filters(Deesser *d, float freq, float cut_db, int jump)
{
    d->freq = jump ? freq : rd_glide(d->freq, freq, d->glide);
    d->cut_db = jump ? cut_db : rd_glide(d->cut_db, cut_db, d->glide);
    rd_svf_set(&d->detector, RD_SVF_BANDPASS, d->freq, DETECTOR_Q, 0.0f, d->rate);
    rd_svf_set(&d->shelf, RD_SVF_HIGHSHELF, d->freq, SHELF_Q, d->cut_db, d->rate);
}

static LADSPA_Handle instantiate(const LADSPA_Descriptor *desc, unsigned long rate)
{
    (void)desc;
    Deesser *d = (Deesser *)calloc(1, sizeof(Deesser));
    if (!d)
        return NULL;
    d->rate = (float)rate;
    d->glide = rd_time_coef(15.0f, d->rate / RD_TICK);
    return d;
}

static void connect_port(LADSPA_Handle handle, unsigned long port, LADSPA_Data *data)
{
    if (port < P_COUNT)
        ((Deesser *)handle)->port[port] = data;
}

static void activate(LADSPA_Handle handle)
{
    Deesser *d = (Deesser *)handle;
    rd_svf_reset(&d->detector);
    rd_svf_reset(&d->shelf);
    d->envelope = 0.0f;
    d->tick = 0;
    const float freq = rd_control(d->port[P_FREQUENCY], 6500.0f, 4000.0f, 12000.0f);
    update_filters(d, freq, 0.0f, 1);
    rd_bypass_init(&d->bypass, d->rate, rd_switch(d->port[P_ENABLED]));
    rd_set_output(d->port[P_REDUCTION], 0.0f);
}

static void run(LADSPA_Handle handle, unsigned long n)
{
    Deesser *d = (Deesser *)handle;
    const LADSPA_Data *in = d->port[P_IN];
    LADSPA_Data *out = d->port[P_OUT];
    const int enabled = rd_switch(d->port[P_ENABLED]);
    const float threshold = rd_control(d->port[P_THRESHOLD], -28.0f, -60.0f, 0.0f);
    const float freq = rd_control(d->port[P_FREQUENCY], 6500.0f, 4000.0f, 12000.0f);
    const float range = rd_control(d->port[P_RANGE], -10.0f, -24.0f, 0.0f);
    const float attack = rd_time_coef(rd_control(d->port[P_ATTACK], 1.0f, 0.5f, 50.0f), d->rate);
    const float release = rd_time_coef(rd_control(d->port[P_RELEASE], 50.0f, 10.0f, 500.0f), d->rate);
    float deepest = 0.0f;

    for (unsigned long i = 0; i < n; i++) {
        const float x = in[i];
        const float band = fabsf(rd_svf_run(&d->detector, x));
        const float coef = band > d->envelope ? attack : release;
        d->envelope = band + (d->envelope - band) * coef;
        d->envelope = rd_flush(d->envelope);

        const float level_db = rd_gain_to_db(d->envelope);
        const float over = level_db - threshold;
        float target_cut = 0.0f;
        if (over > 0.0f && range < 0.0f) {
            /* Soft knee into the max cut: 12 dB of overshoot reaches full range. */
            const float t = over < 12.0f ? over / 12.0f : 1.0f;
            target_cut = range * t * t;
        }
        if (target_cut < deepest)
            deepest = target_cut;

        if (d->tick == 0) {
            update_filters(d, freq, target_cut, 0);
            rd_svf_flush(&d->detector);
            rd_svf_flush(&d->shelf);
        }
        d->tick = (d->tick + 1) % RD_TICK;

        const float wet = rd_svf_run(&d->shelf, x);
        const float mix = rd_bypass_next(&d->bypass, enabled);
        out[i] = x + (wet - x) * mix;
    }
    rd_set_output(d->port[P_REDUCTION], rd_bypass_idle(&d->bypass, enabled) ? 0.0f : -deepest);
}

static void cleanup(LADSPA_Handle handle)
{
    free(handle);
}

static const LADSPA_PortDescriptor port_desc[P_COUNT] = {
    RD_IN_AUDIO, RD_OUT_AUDIO, RD_IN_CONTROL, RD_IN_CONTROL, RD_IN_CONTROL, RD_IN_CONTROL,
    RD_IN_CONTROL, RD_IN_CONTROL, RD_OUT_CONTROL,
};

static const char *const port_names[P_COUNT] = {
    "In", "Out", "Enabled", "Threshold", "Frequency", "Range", "Attack", "Release", "Reduction",
};

static const LADSPA_PortRangeHint port_hints[P_COUNT] = {
    { 0, 0, 0 },
    { 0, 0, 0 },
    { RD_TOGGLE_ON, 0, 1 },
    { RD_BOUNDED | LADSPA_HINT_DEFAULT_MIDDLE, -60.0f, 0.0f },
    { RD_BOUNDED | LADSPA_HINT_LOGARITHMIC | LADSPA_HINT_DEFAULT_MIDDLE, 4000.0f, 12000.0f },
    { RD_BOUNDED | LADSPA_HINT_DEFAULT_HIGH, -24.0f, 0.0f },
    { RD_BOUNDED | LADSPA_HINT_LOGARITHMIC | LADSPA_HINT_DEFAULT_LOW, 0.5f, 50.0f },
    { RD_BOUNDED | LADSPA_HINT_LOGARITHMIC | LADSPA_HINT_DEFAULT_LOW, 10.0f, 500.0f },
    { RD_BOUNDED, 0.0f, 24.0f },
};

const LADSPA_Descriptor rd_deesser_descriptor = {
    .UniqueID = 0x52440007,
    .Label = "rostrum_deesser",
    .Properties = RD_PROPERTIES,
    .Name = "Rostrum de-esser",
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
