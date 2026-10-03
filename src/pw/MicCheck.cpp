#include "pw/MicCheck.h"

#include "pw/PwContext.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

#include <QElapsedTimer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <vector>

namespace rostrum::pw {

namespace miccheck {

double toDb(float peak)
{
    return peak > 0.0f ? 20.0 * std::log10(double(peak)) : -std::numeric_limits<double>::infinity();
}

Verdict verdict(double peakDb)
{
    if (peakDb < kSilentBelowDb) {
        return Verdict::Silent;
    }
    if (peakDb < kQuietBelowDb) {
        return Verdict::Quiet;
    }
    if (peakDb >= kLoudFromDb) {
        return Verdict::Loud;
    }
    return Verdict::Good;
}

} // namespace miccheck

namespace {
using miccheck::kFrames;
using miccheck::kRate;
constexpr int kPollMs = 50;
// A source that never runs (nothing drives it) or a sink that stalls must not hang the check.
constexpr qint64 kGraceMs = 3000;
// Let the last buffers reach the ears before the stream goes.
constexpr int kTailFrames = kRate / 5;
} // namespace

struct MicCheck::State
{
    pw_stream *stream = nullptr;
    spa_hook listener{};
    std::vector<float> samples = std::vector<float>(kFrames); // mono, allocated before any stream
    std::atomic<int> recorded{0};
    std::atomic<int> played{0};
    std::atomic<float> peak{0.0f};
    QElapsedTimer since;
};

namespace {

// Realtime thread: no locks, no allocation.
void onCapture(void *data)
{
    auto *s = static_cast<MicCheck::State *>(data);
    pw_buffer *b = pw_stream_dequeue_buffer(s->stream);
    if (!b) {
        return;
    }
    const spa_data &d = b->buffer->datas[0];
    const int at = s->recorded.load(std::memory_order_relaxed);
    if (d.data && d.chunk && at < kFrames) {
        const uint32_t offset = std::min(d.chunk->offset, d.maxsize);
        const uint32_t size = std::min(d.chunk->size, d.maxsize - offset);
        const auto *in = static_cast<const float *>(SPA_PTROFF(d.data, offset, void));
        const int n = std::min<int>(int(size / sizeof(float)), kFrames - at);
        float peak = s->peak.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) {
            const float v = in[i];
            s->samples[size_t(at + i)] = v;
            peak = std::max(peak, std::fabs(v));
        }
        s->peak.store(peak, std::memory_order_relaxed);
        s->recorded.store(at + n, std::memory_order_release);
    }
    pw_stream_queue_buffer(s->stream, b);
}

// Realtime thread: no locks, no allocation.
void onPlayback(void *data)
{
    auto *s = static_cast<MicCheck::State *>(data);
    pw_buffer *b = pw_stream_dequeue_buffer(s->stream);
    if (!b) {
        return;
    }
    spa_data &d = b->buffer->datas[0];
    if (!d.data) {
        pw_stream_queue_buffer(s->stream, b);
        return;
    }
    constexpr uint32_t stride = sizeof(float) * 2;
    uint32_t frames = d.maxsize / stride;
    if (b->requested > 0) {
        frames = std::min<uint32_t>(frames, b->requested);
    }
    const int end = s->recorded.load(std::memory_order_acquire);
    int at = s->played.load(std::memory_order_relaxed);
    auto *out = static_cast<float *>(d.data);
    for (uint32_t i = 0; i < frames; ++i) {
        const float v = at < end ? s->samples[size_t(at)] : 0.0f;
        out[i * 2] = v;
        out[i * 2 + 1] = v;
        ++at;
    }
    s->played.store(std::min(at, end + kTailFrames), std::memory_order_relaxed);
    d.chunk->offset = 0;
    d.chunk->stride = stride;
    d.chunk->size = frames * stride;
    pw_stream_queue_buffer(s->stream, b);
}

pw_stream_events makeEvents(void (*process)(void *))
{
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.process = process;
    return e;
}

const pw_stream_events kCaptureEvents = makeEvents(onCapture);
const pw_stream_events kPlaybackEvents = makeEvents(onPlayback);

QByteArray targetOf(const Node &n)
{
    return (n.serial.isEmpty() ? n.name : n.serial).toUtf8();
}

pw_stream *connectStream(pw_core *core, const char *name, const char *description, bool capture,
                         const QByteArray &target, spa_hook *listener, MicCheck::State *s)
{
    pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, capture ? "Capture" : "Playback",
        PW_KEY_NODE_NAME, name, PW_KEY_NODE_DESCRIPTION, description, PW_KEY_APP_NAME, "Rostrum",
        PW_KEY_TARGET_OBJECT, target.constData(), "node.dont-fallback", "true", "node.dont-reconnect", "true",
        "node.dont-move", "true", "state.restore-props", "false", "state.restore-target", "false",
        "rostrum.internal", "true", nullptr);
    pw_stream *stream = pw_stream_new(core, name, props);
    if (!stream) {
        return nullptr;
    }
    pw_stream_add_listener(stream, listener, capture ? &kCaptureEvents : &kPlaybackEvents, s);
    uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = kRate;
    if (capture) {
        info.channels = 1;
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else {
        info.channels = 2;
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
    }
    const spa_pod *params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
    pw_stream_connect(stream, capture ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT, PW_ID_ANY,
                      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                                   PW_STREAM_FLAG_RT_PROCESS),
                      params, 1);
    return stream;
}

} // namespace

MicCheck::MicCheck(PwContext *pw, QObject *parent)
    : QObject(parent)
    , m_pw(pw)
{
    m_poll.setInterval(kPollMs);
    connect(&m_poll, &QTimer::timeout, this, &MicCheck::poll);
    connect(m_pw, &PwContext::aboutToStop, this, &MicCheck::stop);
}

MicCheck::~MicCheck()
{
    stop();
}

bool MicCheck::start(const QString &source, const QString &sink)
{
    stop();
    if (m_pw->state() != PwContext::State::Ready || !m_pw->threadLoop() || !m_pw->core() || sink.isEmpty()) {
        return false;
    }
    const Node *in = m_pw->graph().nodeByName(source);
    if (!in || !m_pw->graph().nodeByName(sink)) {
        return false;
    }
    auto *s = new State;
    pw_thread_loop_lock(m_pw->threadLoop());
    s->stream = connectStream(m_pw->core(), "rostrum-mic-check-record", "Rostrum mic check", true, targetOf(*in),
                              &s->listener, s);
    pw_thread_loop_unlock(m_pw->threadLoop());
    if (!s->stream) {
        delete s;
        return false;
    }
    s->since.start();
    m_state = s;
    m_sink = sink;
    m_poll.start();
    setPhase(Phase::Recording);
    return true;
}

bool MicCheck::startPlayback()
{
    destroyStream();
    const Node *out = m_pw->graph().nodeByName(m_sink);
    if (!out || !m_pw->threadLoop() || !m_pw->core()) {
        return false;
    }
    pw_thread_loop_lock(m_pw->threadLoop());
    m_state->stream = connectStream(m_pw->core(), "rostrum-mic-check-play", "Rostrum mic check playback", false,
                                    targetOf(*out), &m_state->listener, m_state);
    pw_thread_loop_unlock(m_pw->threadLoop());
    if (!m_state->stream) {
        return false;
    }
    m_state->since.start();
    setPhase(Phase::Playing);
    return true;
}

void MicCheck::poll()
{
    if (!m_state) {
        return;
    }
    const int frames = m_state->recorded.load(std::memory_order_acquire);
    const qint64 elapsed = m_state->since.elapsed();
    const qint64 limit = qint64(miccheck::kSeconds) * 1000 + kGraceMs;
    if (m_phase == Phase::Recording && (frames >= kFrames || elapsed > limit)) {
        Q_EMIT recorded();
        if (frames == 0 || !startPlayback()) {
            stop();
            return;
        }
    } else if (m_phase == Phase::Playing &&
               (m_state->played.load(std::memory_order_relaxed) >= frames + kTailFrames || elapsed > limit)) {
        stop();
        return;
    }
    Q_EMIT progressChanged();
}

double MicCheck::progress() const
{
    if (!m_state) {
        return 0.0;
    }
    const int done = m_phase == Phase::Recording ? m_state->recorded.load(std::memory_order_relaxed)
                                                 : m_state->played.load(std::memory_order_relaxed);
    const int total = m_phase == Phase::Recording ? kFrames : m_state->recorded.load(std::memory_order_relaxed);
    return total > 0 ? std::clamp(double(done) / total, 0.0, 1.0) : 0.0;
}

float MicCheck::peak() const
{
    return m_state ? m_state->peak.load(std::memory_order_relaxed) : 0.0f;
}

void MicCheck::destroyStream()
{
    if (!m_state || !m_state->stream) {
        return;
    }
    if (pw_thread_loop *loop = m_pw->threadLoop()) {
        pw_thread_loop_lock(loop);
        spa_hook_remove(&m_state->listener);
        pw_stream_destroy(m_state->stream);
        pw_thread_loop_unlock(loop);
    }
    m_state->stream = nullptr;
    m_state->listener = {};
}

void MicCheck::stop()
{
    m_poll.stop();
    if (!m_state) {
        return;
    }
    destroyStream();
    delete m_state;
    m_state = nullptr;
    m_sink.clear();
    setPhase(Phase::Idle);
}

void MicCheck::setPhase(Phase phase)
{
    if (phase == m_phase) {
        return;
    }
    m_phase = phase;
    Q_EMIT phaseChanged();
    Q_EMIT progressChanged();
}

} // namespace rostrum::pw
