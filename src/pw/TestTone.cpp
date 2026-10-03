#include "pw/TestTone.h"

#include "pw/PwContext.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

#include <cmath>
#include <numbers>

namespace rostrum::pw {

namespace {
constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr double kFrequency = 440.0;
constexpr double kAmplitude = 0.2; // about −14 dBFS
constexpr int kToneFrames = kRate;  // one second
constexpr int kFadeFrames = kRate / 50;
constexpr int kStopAfterMs = 1300;
} // namespace

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
        float v = 0.0f;
        if (s->frame < kToneFrames) {
            const double fade = std::min({1.0, double(s->frame) / kFadeFrames,
                                          double(kToneFrames - s->frame) / kFadeFrames});
            v = float(kAmplitude * fade * std::sin(2.0 * std::numbers::pi * kFrequency * s->frame / kRate));
            ++s->frame;
        }
        for (int c = 0; c < kChannels; ++c) {
            out[i * kChannels + c] = v;
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
