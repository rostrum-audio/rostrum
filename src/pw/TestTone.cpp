#include "pw/TestTone.h"

#include "pw/PwContext.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace rostrum::pw {

namespace {
constexpr int kRate = 48000;
constexpr int kChannels = 2;

struct Note
{
    double start; // seconds
    double frequency;
    double pan; // -1 left .. +1 right
};
constexpr Note kNotes[] = {
    {0.00, 523.25, -0.55}, // C5
    {0.16, 659.25, 0.55},  // E5
    {0.32, 783.99, 0.0},   // G5
    {0.48, 1046.50, 0.0},  // C6
};
constexpr double kNoteGain = 0.09;
constexpr double kAttack = 0.004;      // seconds; soft enough not to click
constexpr double kDecay = 0.32;        // fundamental decay time constant
constexpr double kOvertoneDecay = 0.1; // overtones fade faster, like a struck bar
constexpr double kLength = 1.4;
constexpr double kTailFade = 0.06; // forces silence at the very end
constexpr int kStopAfterMs = 1600;

double noteSample(const Note &n, double t)
{
    const double local = t - n.start;
    if (local < 0.0) {
        return 0.0;
    }
    const double attack = std::min(1.0, local / kAttack);
    const double w = 2.0 * std::numbers::pi * n.frequency * local;
    const double body = std::exp(-local / kDecay) * std::sin(w);
    const double overtones = std::exp(-local / kOvertoneDecay) * (0.35 * std::sin(2.0 * w) + 0.12 * std::sin(3.0 * w));
    return kNoteGain * attack * (body + overtones);
}
} // namespace

namespace chime {

int totalFrames() { return int(kLength * kRate); }
int sampleRate() { return kRate; }

void sample(int frame, float &left, float &right)
{
    left = right = 0.0f;
    if (frame < 0 || frame >= totalFrames()) {
        return;
    }
    const double t = double(frame) / kRate;
    const double tail = std::clamp((kLength - t) / kTailFade, 0.0, 1.0);
    double l = 0.0;
    double r = 0.0;
    for (const Note &n : kNotes) {
        const double v = noteSample(n, t) * tail;
        // Equal-power pan: centre is -3 dB per side.
        const double angle = (n.pan + 1.0) * std::numbers::pi / 4.0;
        l += v * std::cos(angle);
        r += v * std::sin(angle);
    }
    left = float(l);
    right = float(r);
}

} // namespace chime

struct TestTone::State
{
    pw_stream *stream = nullptr;
    spa_hook listener{};
    int frame = 0; // touched only on the realtime thread
};

namespace {

// Realtime thread: no locks, no allocation.
void onProcess(void *data)
{
    auto *s = static_cast<TestTone::State *>(data);
    pw_buffer *b = pw_stream_dequeue_buffer(s->stream);
    if (!b) {
        return;
    }
    spa_data &d = b->buffer->datas[0];
    if (!d.data) {
        pw_stream_queue_buffer(s->stream, b);
        return;
    }
    const uint32_t stride = sizeof(float) * kChannels;
    uint32_t frames = d.maxsize / stride;
    if (b->requested > 0) {
        frames = std::min<uint32_t>(frames, b->requested);
    }
    auto *out = static_cast<float *>(d.data);
    for (uint32_t i = 0; i < frames; ++i) {
        chime::sample(s->frame, out[i * kChannels], out[i * kChannels + 1]);
        if (s->frame < chime::totalFrames()) {
            ++s->frame;
        }
    }
    d.chunk->offset = 0;
    d.chunk->stride = stride;
    d.chunk->size = frames * stride;
    pw_stream_queue_buffer(s->stream, b);
}

pw_stream_events makeEvents()
{
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.process = onProcess;
    return e;
}

const pw_stream_events kEvents = makeEvents();

} // namespace

TestTone::TestTone(PwContext *pw, QObject *parent)
    : QObject(parent)
    , m_pw(pw)
{
    m_stopTimer.setSingleShot(true);
    m_stopTimer.setInterval(kStopAfterMs);
    connect(&m_stopTimer, &QTimer::timeout, this, &TestTone::stop);
    connect(m_pw, &PwContext::aboutToStop, this, &TestTone::stop);
}

TestTone::~TestTone()
{
    stop();
}

bool TestTone::play(const QString &nodeName)
{
    stop();
    if (m_pw->state() != PwContext::State::Ready || !m_pw->threadLoop() || !m_pw->core()) {
        return false;
    }
    QByteArray target;
    if (!nodeName.isEmpty()) {
        const Node *n = m_pw->graph().nodeByName(nodeName);
        if (!n) {
            return false;
        }
        target = (n->serial.isEmpty() ? n->name : n->serial).toUtf8();
    }

    auto *s = new State;
    pw_thread_loop_lock(m_pw->threadLoop());
    pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Notification",
        PW_KEY_NODE_NAME, "rostrum-test-tone", PW_KEY_NODE_DESCRIPTION, "Rostrum test tone",
        PW_KEY_APP_NAME, "Rostrum", "state.restore-props", "false", "state.restore-target", "false",
        "rostrum.internal", "true", nullptr);
    if (!target.isEmpty()) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, target.constData());
    }
    s->stream = pw_stream_new(m_pw->core(), "rostrum-test-tone", props);
    if (s->stream) {
        pw_stream_add_listener(s->stream, &s->listener, &kEvents, s);
        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32;
        info.rate = kRate;
        info.channels = kChannels;
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
        const spa_pod *params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
        pw_stream_connect(s->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                                       PW_STREAM_FLAG_RT_PROCESS),
                          params, 1);
    }
    pw_thread_loop_unlock(m_pw->threadLoop());
    if (!s->stream) {
        delete s;
        return false;
    }
    m_state = s;
    m_target = nodeName;
    m_stopTimer.start();
    Q_EMIT playingChanged();
    return true;
}

void TestTone::stop()
{
    m_stopTimer.stop();
    if (!m_state) {
        return;
    }
    if (pw_thread_loop *loop = m_pw->threadLoop()) {
        pw_thread_loop_lock(loop);
        spa_hook_remove(&m_state->listener);
        pw_stream_destroy(m_state->stream);
        pw_thread_loop_unlock(loop);
    }
    delete m_state;
    m_state = nullptr;
    m_target.clear();
    Q_EMIT playingChanged();
}

} // namespace rostrum::pw
