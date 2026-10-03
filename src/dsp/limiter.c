/* Lookahead peak limiter. The gain needed by each sample is held over the lookahead
 * window (sliding minimum), released smoothly, then averaged over the same window,
 * so the gain has fully arrived by the time the delayed peak is played. */
#include "dsp_common.h"

enum { P_IN, P_OUT, P_ENABLED, P_CEILING, P_RELEASE, P_REDUCTION, P_LATENCY, P_COUNT };

#define LOOKAHEAD_MS 1.5f
#define LOOKAHEAD_MAX 1024u

typedef struct {
    LADSPA_Data *port[P_COUNT];
    float rate;
    unsigned lookahead;
    unsigned window;
    rd_delay audio;
    /* Monotonic queue for the sliding minimum over `window` samples. */
    float *queue_gain;
    unsigned long long *queue_at;
    unsigned queue_head;
    unsigned queue_count;
    unsigned long long now;
    /* Box filter over `window` samples. */
    float *box;
    unsigned box_pos;
    double box_sum;
    float released;
    rd_bypass bypass;
} Limiter;

static LADSPA_Handle instantiate(const LADSPA_Descriptor *d, unsigned long rate)
{
    (void)d;
    Limiter *l = (Limiter *)calloc(1, sizeof(Limiter));
    if (!l)
        return NULL;
    l->rate = (float)rate;
    unsigned lookahead = (unsigned)lroundf(LOOKAHEAD_MS * l->rate / 1000.0f);
    if (lookahead < 1)
        lookahead = 1;
    if (lookahead > LOOKAHEAD_MAX)
        lookahead = LOOKAHEAD_MAX;
    l->lookahead = lookahead;
    l->window = lookahead + 1;
    l->queue_gain = (float *)calloc(l->window, sizeof(float));
    l->queue_at = (unsigned long long *)calloc(l->window, sizeof(unsigned long long));
    l->box = (float *)calloc(l->window, sizeof(float));
    if (!rd_delay_alloc(&l->audio, lookahead) || !l->queue_gain || !l->queue_at || !l->box) {
        rd_delay_free(&l->audio);
        free(l->queue_gain);
        free(l->queue_at);
        free(l->box);
        free(l);
        return NULL;
    }
    return l;
}

static void connect_port(LADSPA_Handle handle, unsigned long port, LADSPA_Data *data)
{
    if (port < P_COUNT)
        ((Limiter *)handle)->port[port] = data;
}

static void activate(LADSPA_Handle handle)
{
    Limiter *l = (Limiter *)handle;
    rd_delay_clear(&l->audio);
    l->queue_head = 0;
    l->queue_count = 0;
    l->now = 0;
    for (unsigned i = 0; i < l->window; i++)
        l->box[i] = 1.0f;
    l->box_pos = 0;
    l->box_sum = l->window;
    l->released = 1.0f;
    rd_bypass_init(&l->bypass, l->rate, rd_switch(l->port[P_ENABLED]));
    rd_set_output(l->port[P_LATENCY], (float)l->lookahead);
}

static inline float sliding_min(Limiter *l, float gain)
{
    const unsigned cap = l->window;
    while (l->queue_count && l->queue_at[l->queue_head] + l->lookahead < l->now) {
        l->queue_head = (l->queue_head + 1) % cap;
        l->queue_count--;
    }
    while (l->queue_count) {
        const unsigned back = (l->queue_head + l->queue_count - 1) % cap;
        if (l->queue_gain[back] < gain)
            break;
        l->queue_count--;
    }
    const unsigned slot = (l->queue_head + l->queue_count) % cap;
    l->queue_gain[slot] = gain;
    l->queue_at[slot] = l->now;
    l->queue_count++;
    return l->queue_gain[l->queue_head];
}

static void run(LADSPA_Handle handle, unsigned long n)
{
    Limiter *l = (Limiter *)handle;
    const LADSPA_Data *in = l->port[P_IN];
    LADSPA_Data *out = l->port[P_OUT];
    const int enabled = rd_switch(l->port[P_ENABLED]);
    const float ceiling = rd_db_to_gain(rd_control(l->port[P_CEILING], -1.0f, -12.0f, 0.0f));
    const float release = rd_time_coef(rd_control(l->port[P_RELEASE], 60.0f, 10.0f, 1000.0f), l->rate);
    float lowest = 1.0f;

    for (unsigned long i = 0; i < n; i++) {
        const float x = in[i];
        const float level = fabsf(x);
        const float needed = level > ceiling ? ceiling / level : 1.0f;

        const float held = sliding_min(l, needed);
        l->now++;
        if (held < l->released)
            l->released = held;
        else
            l->released = held + (l->released - held) * release;

        l->box_sum += (double)l->released - (double)l->box[l->box_pos];
        l->box[l->box_pos] = l->released;
        l->box_pos = (l->box_pos + 1) % l->window;
        float gain = (float)(l->box_sum / l->window);
        if (gain > 1.0f)
            gain = 1.0f;
        if (gain < lowest)
            lowest = gain;

        const float delayed = rd_delay_push(&l->audio, x);
        const float mix = rd_bypass_next(&l->bypass, enabled);
        float y = delayed * (1.0f + (gain - 1.0f) * mix);
        if (mix >= 1.0f)
            y = rd_clamp(y, -ceiling, ceiling);
        out[i] = y;
    }
    rd_set_output(l->port[P_REDUCTION], rd_bypass_idle(&l->bypass, enabled) ? 0.0f : -rd_gain_to_db(lowest));
    rd_set_output(l->port[P_LATENCY], (float)l->lookahead);
}

static void cleanup(LADSPA_Handle handle)
{
    Limiter *l = (Limiter *)handle;
    rd_delay_free(&l->audio);
    free(l->queue_gain);
    free(l->queue_at);
    free(l->box);
    free(l);
}

static const LADSPA_PortDescriptor port_desc[P_COUNT] = {
    RD_IN_AUDIO, RD_OUT_AUDIO, RD_IN_CONTROL, RD_IN_CONTROL, RD_IN_CONTROL, RD_OUT_CONTROL, RD_OUT_CONTROL,
};

static const char *const port_names[P_COUNT] = {
    "In", "Out", "Enabled", "Ceiling", "Release", "Reduction", "latency",
};

static const LADSPA_PortRangeHint port_hints[P_COUNT] = {
    { 0, 0, 0 },
    { 0, 0, 0 },
    { RD_TOGGLE_ON, 0, 1 },
    { RD_BOUNDED | LADSPA_HINT_DEFAULT_MAXIMUM, -12.0f, 0.0f },
    { RD_BOUNDED | LADSPA_HINT_LOGARITHMIC | LADSPA_HINT_DEFAULT_LOW, 10.0f, 1000.0f },
    { RD_BOUNDED, 0.0f, 60.0f },
    { 0, 0, 0 },
};

const LADSPA_Descriptor rd_limiter_descriptor = {
    .UniqueID = 0x52440005,
    .Label = "rostrum_limiter",
    .Properties = RD_PROPERTIES,
    .Name = "Rostrum limiter",
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
