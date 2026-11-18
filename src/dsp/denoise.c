/* RNNoise noise suppression. RNNoise works on 10 ms frames at 48 kHz.
 *
 * RNNoise runs on a worker thread, not in run(). run() is called from the PipeWire daemon's
 * realtime thread, which runs every other node in the graph one after another, headphones
 * included. On a busy desktop one RNNoise frame can take several milliseconds (its model is
 * about 1 MB and falls out of the CPU cache), and a cycle that waited for it made every device
 * crackle. run() hands each finished frame to the worker and plays out the frame before it, so
 * the plugin reports two frames of latency and the dry path stays aligned with it. At cycles of
 * up to 512 samples every frame is queued at least one cycle before it is played. A frame the
 * worker has not finished by then goes out without noise removal, and the worker logs how many
 * did, at most once every 10 s. Where a frame is due in the same run() call that queued it
 * (cycles above 512 samples), run() waits for the worker, as it did when RNNoise ran inline.
 *
 * At any other rate it passes audio through and reports itself inactive.
 *
 * Tests only: ROSTRUM_DSP_TEST_DENOISE_WAIT=1 makes run() always wait for the worker, and
 * ROSTRUM_DSP_TEST_DENOISE_STALL=<ms>:<every> makes the worker sleep <ms> before every
 * <every>th frame. */
#define _GNU_SOURCE /* pthread_setname_np */
#include "dsp_common.h"

#include <rnnoise.h>

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

enum { P_IN, P_OUT, P_ENABLED, P_STRENGTH, P_VOICE_THRESHOLD, P_VOICE, P_ACTIVE, P_LATENCY, P_COUNT };

#define FRAME 480
#define LATENCY_FRAMES 2
#define HISTORY (LATENCY_FRAMES + 1)
#define SLOTS 16
#define RNNOISE_RATE 48000
#define VOICE_HOLD_FRAMES 20
#define PCM_SCALE 32768.0f
#define LATE_REPORT_NS 10000000000LL

/* A slot moves FREE -> QUEUED (run) -> BUSY (worker) -> DONE (worker). run() takes a frame back
 * from QUEUED to FREE when it is due before the worker started it. */
enum { SLOT_FREE, SLOT_QUEUED, SLOT_BUSY, SLOT_DONE };
/* What run() did with each input frame, kept until the frame is played out. */
enum { FRAME_PASS, FRAME_QUEUED, FRAME_DROPPED };

typedef struct {
    _Atomic int state;
    _Atomic uint64_t frame;
    uint64_t call;     /* the run() call that queued it; only run() reads it */
    float input[FRAME];
    float wet[FRAME];  /* written by the worker */
    float voice;
} Slot;

typedef struct {
    LADSPA_Data *port[P_COUNT];
    float rate;
    int active;
    DenoiseState *state;

    /* Used by run() only. */
    float history[HISTORY][FRAME]; /* input frames; the oldest is played out as the dry path */
    float *filling;                /* the history row the current input frame goes into */
    const float *dry;              /* the history row being played out */
    float wet[FRAME];              /* the denoised frame being played out */
    unsigned char kind[4];
    uint64_t frame;                /* the input frame being filled */
    uint64_t calls;
    unsigned pos;
    int due;                       /* the next sample starts a new output frame */
    float voice;
    unsigned voice_hold;
    float voice_gain;
    float voice_open;
    float voice_close;
    float strength;
    float strength_step;
    rd_bypass bypass;

    /* Shared with the worker. */
    Slot slots[SLOTS];
    sem_t wake;
    pthread_t thread;
    int threaded;
    _Atomic int stop;
    _Atomic unsigned late;
    int wait;
    unsigned stall_ms;
    unsigned stall_every;
} Denoise;

static const float silence[FRAME];

static void pause_briefly(long ns)
{
    struct timespec t = { ns / 1000000000L, ns % 1000000000L };
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {
    }
}

static int64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* Runs RNNoise on a slot the caller has claimed (BUSY) and publishes the result. */
static void denoise_slot(Denoise *dn, Slot *s)
{
    float scratch[FRAME];
    for (unsigned i = 0; i < FRAME; i++)
        scratch[i] = s->input[i] * PCM_SCALE;
    const float voice = rnnoise_process_frame(dn->state, s->wet, scratch);
    for (unsigned i = 0; i < FRAME; i++)
        s->wet[i] = s->wet[i] / PCM_SCALE;
    s->voice = isfinite(voice) ? rd_clamp(voice, 0.0f, 1.0f) : 0.0f;
    atomic_store_explicit(&s->state, SLOT_DONE, memory_order_release);
}

static int claim(Slot *s)
{
    int expected = SLOT_QUEUED;
    return atomic_compare_exchange_strong_explicit(&s->state, &expected, SLOT_BUSY, memory_order_acq_rel,
                                                   memory_order_acquire);
}

/* The oldest queued frame, so RNNoise sees frames in order. */
static Slot *next_queued(Denoise *dn)
{
    Slot *best = NULL;
    uint64_t best_frame = 0;
    for (unsigned i = 0; i < SLOTS; i++) {
        Slot *s = &dn->slots[i];
        if (atomic_load_explicit(&s->state, memory_order_acquire) != SLOT_QUEUED)
            continue;
        const uint64_t f = atomic_load_explicit(&s->frame, memory_order_relaxed);
        if (!best || f < best_frame) {
            best = s;
            best_frame = f;
        }
    }
    return best;
}

static void *worker(void *arg)
{
    Denoise *dn = (Denoise *)arg;
    unsigned reported = 0;
    int64_t last_report = now_ns() - LATE_REPORT_NS;
    for (;;) {
        while (sem_wait(&dn->wake) != 0 && errno == EINTR) {
        }
        if (atomic_load_explicit(&dn->stop, memory_order_acquire))
            break;
        Slot *s;
        while ((s = next_queued(dn)) != NULL) {
            if (!claim(s))
                continue;
            const uint64_t f = atomic_load_explicit(&s->frame, memory_order_relaxed);
            if (dn->stall_ms && dn->stall_every && f % dn->stall_every == dn->stall_every - 1)
                pause_briefly((long)dn->stall_ms * 1000000L);
            denoise_slot(dn, s);
        }
        const unsigned late = atomic_load_explicit(&dn->late, memory_order_relaxed);
        if (late != reported) {
            const int64_t now = now_ns();
            if (now - last_report >= LATE_REPORT_NS) {
                fprintf(stderr,
                        "rostrum_denoise: %u frame(s) went out without noise removal in the last %lld s: "
                        "the worker thread did not finish them in time\n",
                        late - reported, (long long)((now - last_report) / 1000000000LL));
                reported = late;
                last_report = now;
            }
        }
    }
    return NULL;
}

static void start_worker(Denoise *dn)
{
    if (sem_init(&dn->wake, 0, 0) != 0)
        return;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        sem_destroy(&dn->wake);
        return;
    }
    /* Never inherit the realtime class: RNNoise must not compete with the graph. */
    struct sched_param param = { .sched_priority = 0 };
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
    pthread_attr_setschedparam(&attr, &param);
    if (pthread_create(&dn->thread, &attr, worker, dn) == 0) {
        dn->threaded = 1;
        pthread_setname_np(dn->thread, "rostrum-denoise");
    } else {
        sem_destroy(&dn->wake);
    }
    pthread_attr_destroy(&attr);
}

static unsigned env_unsigned(const char *text, const char **end)
{
    char *stop = NULL;
    const unsigned long v = strtoul(text, &stop, 10);
    *end = stop;
    return v > 60000 ? 60000 : (unsigned)v;
}

static void read_test_switches(Denoise *dn)
{
    const char *wait = getenv("ROSTRUM_DSP_TEST_DENOISE_WAIT");
    dn->wait = wait && strcmp(wait, "1") == 0;
    const char *stall = getenv("ROSTRUM_DSP_TEST_DENOISE_STALL");
    if (stall && *stall) {
        const char *end = NULL;
        dn->stall_ms = env_unsigned(stall, &end);
        dn->stall_every = *end == ':' ? env_unsigned(end + 1, &end) : 1;
    }
}

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
        read_test_switches(dn);
        /* Without a thread, run() denoises inline as it used to. */
        start_worker(dn);
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
    rd_set_output(dn->port[P_LATENCY], dn->active ? (float)(FRAME * LATENCY_FRAMES) : 0.0f);
}

/* Takes back queued frames and waits for one the worker is on. Not called from run(). */
static int quiesce(Denoise *dn)
{
    for (unsigned i = 0; i < SLOTS; i++) {
        int expected = SLOT_QUEUED;
        atomic_compare_exchange_strong(&dn->slots[i].state, &expected, SLOT_FREE);
    }
    for (int tries = 0; tries < 5000; tries++) {
        int busy = 0;
        for (unsigned i = 0; i < SLOTS; i++)
            busy |= atomic_load(&dn->slots[i].state) == SLOT_BUSY;
        if (!busy)
            return 1;
        pause_briefly(1000000L);
    }
    return 0;
}

static void activate(LADSPA_Handle handle)
{
    Denoise *dn = (Denoise *)handle;
    if (dn->state && quiesce(dn)) {
        rnnoise_init(dn->state, NULL);
        for (unsigned i = 0; i < SLOTS; i++)
            atomic_store(&dn->slots[i].state, SLOT_FREE);
    }
    memset(dn->history, 0, sizeof(dn->history));
    memset(dn->wet, 0, sizeof(dn->wet));
    memset(dn->kind, FRAME_PASS, sizeof(dn->kind));
    dn->filling = dn->history[0];
    dn->dry = silence;
    dn->frame = 0;
    dn->calls = 0;
    dn->pos = 0;
    dn->due = 0;
    dn->voice = 0.0f;
    dn->voice_hold = 0;
    dn->voice_gain = 1.0f;
    dn->strength = rd_control(dn->port[P_STRENGTH], 1.0f, 0.0f, 1.0f);
    rd_bypass_init(&dn->bypass, dn->rate, rd_switch(dn->port[P_ENABLED]));
    report(dn);
}

/* The input frame just filled: hand it to the worker, or mark it as passed through. */
static void queue_frame(Denoise *dn, int enabled)
{
    const uint64_t f = dn->frame;
    unsigned char *kind = &dn->kind[f & 3];
    if (rd_bypass_idle(&dn->bypass, enabled)) {
        *kind = FRAME_PASS;
        return;
    }
    Slot *s = &dn->slots[f % SLOTS];
    const int state = atomic_load_explicit(&s->state, memory_order_acquire);
    if (state == SLOT_QUEUED || state == SLOT_BUSY) {
        /* The worker is a whole ring behind; this frame goes out dry. */
        *kind = FRAME_DROPPED;
        return;
    }
    memcpy(s->input, dn->filling, sizeof(s->input));
    s->call = dn->calls;
    atomic_store_explicit(&s->frame, f, memory_order_relaxed);
    atomic_store_explicit(&s->state, SLOT_QUEUED, memory_order_release);
    *kind = FRAME_QUEUED;
    if (dn->threaded) {
        sem_post(&dn->wake);
    } else if (claim(s)) {
        denoise_slot(dn, s);
    }
}

/* Called at the first sample of an output frame: picks up its denoised audio. */
static void play_frame(Denoise *dn, float threshold)
{
    if (dn->frame >= LATENCY_FRAMES) {
        const uint64_t f = dn->frame - LATENCY_FRAMES;
        dn->dry = dn->history[f % HISTORY];
        int ready = 0;
        if (dn->kind[f & 3] == FRAME_PASS) {
            memcpy(dn->wet, dn->dry, sizeof(dn->wet));
            dn->voice = 0.0f;
            ready = 1;
        } else if (dn->kind[f & 3] == FRAME_QUEUED) {
            Slot *s = &dn->slots[f % SLOTS];
            int state = atomic_load_explicit(&s->state, memory_order_acquire);
            if (state != SLOT_DONE && (dn->wait || s->call == dn->calls)) {
                while ((state = atomic_load_explicit(&s->state, memory_order_acquire)) != SLOT_DONE)
                    pause_briefly(50000L);
            }
            if (state == SLOT_DONE) {
                memcpy(dn->wet, s->wet, sizeof(dn->wet));
                dn->voice = s->voice;
                ready = 1;
            } else {
                /* Not started: the worker skips it. Started: it finishes into a slot nobody reads. */
                int expected = SLOT_QUEUED;
                atomic_compare_exchange_strong_explicit(&s->state, &expected, SLOT_FREE, memory_order_acq_rel,
                                                        memory_order_relaxed);
            }
        }
        if (!ready) {
            /* Late: play the frame dry and keep the last voice reading. */
            memcpy(dn->wet, dn->dry, sizeof(dn->wet));
            atomic_fetch_add_explicit(&dn->late, 1, memory_order_relaxed);
        }
    }
    if (dn->voice >= threshold && threshold > 0.0f)
        dn->voice_hold = VOICE_HOLD_FRAMES;
    else if (dn->voice_hold > 0)
        dn->voice_hold--;
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

    dn->calls++;
    const int enabled = rd_switch(dn->port[P_ENABLED]);
    const float strength = rd_control(dn->port[P_STRENGTH], 1.0f, 0.0f, 1.0f);
    const float threshold = rd_control(dn->port[P_VOICE_THRESHOLD], 0.0f, 0.0f, 0.95f);

    for (unsigned long i = 0; i < n; i++) {
        if (dn->due) {
            dn->due = 0;
            play_frame(dn, threshold);
        }
        const float x = in[i];
        const float dry = dn->dry[dn->pos];
        const float denoised = dn->wet[dn->pos];
        dn->filling[dn->pos] = x;

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
            queue_frame(dn, enabled);
            dn->frame++;
            dn->filling = dn->history[dn->frame % HISTORY];
            dn->due = 1;
        }
    }
    report(dn);
}

static void cleanup(LADSPA_Handle handle)
{
    Denoise *dn = (Denoise *)handle;
    if (dn->threaded) {
        atomic_store_explicit(&dn->stop, 1, memory_order_release);
        sem_post(&dn->wake);
        pthread_join(dn->thread, NULL);
        sem_destroy(&dn->wake);
    }
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
