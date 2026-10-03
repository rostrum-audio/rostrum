#include "pw/MeterBank.h"

#include "pw/PwContext.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/format-utils.h>
#include <spa/pod/builder.h>

#include <QLoggingCategory>

#include <algorithm>
#include <atomic>
#include <cmath>

Q_DECLARE_LOGGING_CATEGORY(lcPw)

namespace rostrum::pw {

namespace {
constexpr int kWatchdogMs = 1000;
// Source meters are active streams, so the graph must run them every quantum. Three silent
// checks in a row (2 to 3 s) means the stream is stuck and is rebuilt.
constexpr int kStuckChecks = 3;
} // namespace

struct MeterBank::Meter
{
    pw_stream *stream = nullptr;
    spa_hook listener{};
    std::atomic<float> peak{0.0f};
    std::atomic<uint32_t> channels{1};
    std::atomic<uint64_t> cycles{0};
    std::atomic<bool> failed{false};
    uint32_t nodeId = 0;
    bool sum = false;
    bool expectsCycles = false;
    uint64_t seenCycles = 0;
    int silentChecks = 0;
};

namespace {

// Realtime thread: no locks, no allocation.
void onProcess(void *data)
{
    auto *m = static_cast<MeterBank::Meter *>(data);
    pw_buffer *b = pw_stream_dequeue_buffer(m->stream);
    if (!b) {
        return;
    }
    const uint32_t frameSize = m->sum ? std::max<uint32_t>(1, m->channels.load(std::memory_order_relaxed)) : 1;
    float loudest = 0.0f;
    const spa_buffer *buf = b->buffer;
    for (uint32_t i = 0; i < buf->n_datas; ++i) {
        const spa_data &d = buf->datas[i];
        if (!d.data || !d.chunk) {
            continue;
        }
        const uint32_t offset = std::min(d.chunk->offset, d.maxsize);
        const uint32_t size = std::min(d.chunk->size, d.maxsize - offset);
        const auto *samples = reinterpret_cast<const float *>(static_cast<const uint8_t *>(d.data) + offset);
        const uint32_t count = size / sizeof(float);
        for (uint32_t n = 0; n + frameSize <= count; n += frameSize) {
            float s = samples[n];
            for (uint32_t c = 1; c < frameSize; ++c) {
                s += samples[n + c];
            }
            loudest = std::max(loudest, std::fabs(s));
        }
    }
    float prev = m->peak.load(std::memory_order_relaxed);
    while (loudest > prev && !m->peak.compare_exchange_weak(prev, loudest, std::memory_order_relaxed)) {
    }
    m->cycles.fetch_add(1, std::memory_order_relaxed);
    pw_stream_queue_buffer(m->stream, b);
}

void onParamChanged(void *data, uint32_t id, const spa_pod *param)
{
    auto *m = static_cast<MeterBank::Meter *>(data);
    if (!param || id != SPA_PARAM_Format) {
        return;
    }
    uint32_t mediaType = 0;
    uint32_t mediaSubtype = 0;
    if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0 || mediaType != SPA_MEDIA_TYPE_audio ||
        mediaSubtype != SPA_MEDIA_SUBTYPE_raw) {
        return;
    }
    spa_audio_info_raw info{};
    if (spa_format_audio_raw_parse(param, &info) >= 0 && info.channels > 0) {
        m->channels.store(info.channels, std::memory_order_relaxed);
    }
}

void onStateChanged(void *data, pw_stream_state old, pw_stream_state state, const char *)
{
    auto *m = static_cast<MeterBank::Meter *>(data);
    if (state == PW_STREAM_STATE_ERROR || (state == PW_STREAM_STATE_UNCONNECTED && old != PW_STREAM_STATE_UNCONNECTED)) {
        m->failed.store(true, std::memory_order_relaxed);
    }
}

pw_stream_events makeEvents()
{
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = onStateChanged;
    e.param_changed = onParamChanged;
    e.process = onProcess;
    return e;
}

const pw_stream_events kEvents = makeEvents();

} // namespace

MeterBank::MeterBank(PwContext *pw, QObject *parent)
    : QObject(parent)
    , m_pw(pw)
{
    connect(m_pw, &PwContext::graphChanged, this, &MeterBank::sync);
    connect(m_pw, &PwContext::stateChanged, this, &MeterBank::sync);
    connect(m_pw, &PwContext::aboutToStop, this, &MeterBank::destroyAll);
    m_watchdog.setInterval(kWatchdogMs);
    connect(&m_watchdog, &QTimer::timeout, this, &MeterBank::checkHealth);
}

MeterBank::~MeterBank()
{
    destroyAll();
}

void MeterBank::setTargets(const QStringList &nodeNames, const QStringList &summed)
{
    if (nodeNames == m_targets && summed == m_summed) {
        return;
    }
    if (summed != m_summed) {
        // A meter's summing is fixed when it is created.
        for (const QString &name : std::as_const(m_summed) + summed) {
            if (m_summed.contains(name) != summed.contains(name)) {
                destroyMeter(name);
            }
        }
    }
    m_targets = nodeNames;
    m_summed = summed;
    sync();
}

void MeterBank::setActive(bool active)
{
    if (active == m_active) {
        return;
    }
    m_active = active;
    if (active) {
        m_watchdog.start();
    } else {
        m_watchdog.stop();
    }
    sync();
}

void MeterBank::checkHealth()
{
    QStringList stuck;
    for (auto it = m_meters.cbegin(); it != m_meters.cend(); ++it) {
        Meter *m = it.value();
        const uint64_t cycles = m->cycles.load(std::memory_order_relaxed);
        m->silentChecks = (m->expectsCycles && cycles == m->seenCycles) ? m->silentChecks + 1 : 0;
        m->seenCycles = cycles;
        if (m->failed.load(std::memory_order_relaxed) || m->silentChecks >= kStuckChecks) {
            stuck << it.key();
        }
    }
    for (const QString &name : std::as_const(stuck)) {
        if (!m_reported.contains(name)) {
            m_reported.insert(name);
            qCWarning(lcPw) << "meter for" << name << "stopped receiving audio; reconnecting";
        }
        destroyMeter(name);
    }
    if (!stuck.isEmpty()) {
        sync();
    }
}

float MeterBank::takePeak(const QString &nodeName)
{
    Meter *m = m_meters.value(nodeName);
    return m ? m->peak.exchange(0.0f, std::memory_order_relaxed) : 0.0f;
}

void MeterBank::sync()
{
    const bool ready = m_pw->state() == PwContext::State::Ready && m_pw->threadLoop() && m_pw->core();
    if (!ready || !m_active) {
        destroyAll();
        return;
    }
    const Graph &g = m_pw->graph();
    auto resolve = [&g](const QString &key) -> const Node * {
        if (key.startsWith(QLatin1Char('#'))) {
            return g.node(key.mid(1).toUInt());
        }
        return g.nodeByName(key);
    };
    const QStringList existing = m_meters.keys();
    for (const QString &name : existing) {
        const Node *n = resolve(name);
        if (!m_targets.contains(name) || !n || n->id != m_meters.value(name)->nodeId) {
            destroyMeter(name);
        }
    }
    for (const QString &name : std::as_const(m_targets)) {
        if (m_meters.contains(name)) {
            continue;
        }
        const Node *n = resolve(name);
        if (!n || !(n->isSink() || n->isSource() || n->isPlaybackStream())) {
            continue;
        }
        auto *m = new Meter;
        m->nodeId = n->id;
        m->sum = m_summed.contains(name);
        m->expectsCycles = n->isSource();
        const QByteArray streamName = "rostrum-meter." + n->name.toUtf8();
        const QByteArray target = (n->serial.isEmpty() ? n->name : n->serial).toUtf8();

        pw_thread_loop_lock(m_pw->threadLoop());
        pw_properties *props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "DSP",
            PW_KEY_NODE_NAME, streamName.constData(), PW_KEY_NODE_DESCRIPTION, "Rostrum meter",
            PW_KEY_TARGET_OBJECT, target.constData(),
            PW_KEY_STREAM_DONT_REMIX, "true", PW_KEY_NODE_LATENCY, "1024/48000",
            "node.dont-fallback", "true", "node.dont-reconnect", "true", "node.dont-move", "true",
            "state.restore-props", "false", "state.restore-target", "false", "rostrum.internal", "true",
            nullptr);
        if (n->isSink()) {
            pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
        }
        // A passive link never wakes a suspended source, and nothing else may be recording the
        // mic yet, so source meters must be active. Sink and stream meters stay passive: the
        // audio playing through them already keeps them running.
        if (!n->isSource()) {
            pw_properties_set(props, PW_KEY_NODE_PASSIVE, "true");
        }
        m->stream = pw_stream_new(m_pw->core(), streamName.constData(), props);
        if (m->stream) {
            pw_stream_add_listener(m->stream, &m->listener, &kEvents, m);
            uint8_t buffer[1024];
            spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
            // Only the sample format is fixed; rate and channels follow the node.
            spa_audio_info_raw info{};
            info.format = SPA_AUDIO_FORMAT_F32;
            const spa_pod *params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
            pw_stream_connect(m->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                              static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                                           PW_STREAM_FLAG_RT_PROCESS),
                              params, 1);
        }
        pw_thread_loop_unlock(m_pw->threadLoop());

        if (!m->stream) {
            delete m;
            continue;
        }
        m_meters.insert(name, m);
    }
}

void MeterBank::destroyMeter(const QString &name)
{
    Meter *m = m_meters.take(name);
    if (!m) {
        return;
    }
    if (pw_thread_loop *loop = m_pw->threadLoop()) {
        pw_thread_loop_lock(loop);
        spa_hook_remove(&m->listener);
        pw_stream_destroy(m->stream);
        pw_thread_loop_unlock(loop);
    }
    delete m;
}

void MeterBank::destroyAll()
{
    const QStringList names = m_meters.keys();
    for (const QString &name : names) {
        destroyMeter(name);
    }
}

} // namespace rostrum::pw
