// Levels, solo, bus structure, devices, destination links and node volumes.

#include "core/Volume.h"
#include "engine/Engine.h"
#include "pw/PwContext.h"

#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(lcEngine)

namespace rostrum::engine {

namespace {
constexpr qint64 kLinkRetryMs = 3000;
constexpr qint64 kVolumeResendMs = 1000;
constexpr double kMicMaxGain = 1.5;
} // namespace

// ---- levels -----------------------------------------------------------------------------

void Engine::levelChanged()
{
    Q_EMIT sceneChanged();
    Q_EMIT levelsChanged();
    scheduleReconcile();
}

void Engine::setBusVolume(const QString &id, double volume)
{
    Bus *b = m_scene.bus(id);
    if (!b) {
        return;
    }
    cancelFade(b->nodeName());
    const double v = std::clamp(volume, 0.0, b->isInput() ? kMicMaxGain : 1.0);
    if (qFuzzyCompare(b->volume + 1.0, v + 1.0)) {
        return;
    }
    b->volume = v;
    levelChanged();
}

void Engine::setBusMuted(const QString &id, bool muted)
{
    Bus *b = m_scene.bus(id);
    if (!b) {
        return;
    }
    cancelFade(b->nodeName());
    bool released = false;
    if (b->isInput()) {
        bool &contrary = muted ? m_pushToTalk : m_pushToMute;
        released = contrary || (!muted && m_panicMic);
        contrary = false;
        m_panicMic = m_panicMic && muted;
    }
    if (b->muted == muted) {
        if (released) {
            holdsChanged();
        }
        return;
    }
    b->muted = muted;
    levelChanged();
}

void Engine::setBusBalance(const QString &id, double balance)
{
    Bus *b = m_scene.bus(id);
    if (!b || b->isInput()) {
        return;
    }
    cancelFade(b->nodeName());
    const double v = volume::clampBalance(balance);
    if (qFuzzyCompare(b->balance + 1.0, v + 1.0)) {
        return;
    }
    b->balance = v;
    levelChanged();
}

void Engine::setBusDestination(const QString &id, Destination d)
{
    Bus *b = m_scene.bus(id);
    if (!b || b->destination == d) {
        return;
    }
    b->destination = d;
    levelChanged();
}

void Engine::setBusVod(const QString &id, bool on)
{
    Bus *b = m_scene.bus(id);
    if (!b || b->isInput() || b->vod == on) {
        return;
    }
    b->vod = on;
    levelChanged();
}

void Engine::setMasterPhones(double volume)
{
    cancelFade(QString::fromLatin1(kPhonesNode));
    m_scene.masterPhones = std::clamp(volume, 0.0, 1.0);
    levelChanged();
}

void Engine::setMasterPhonesMuted(bool muted)
{
    cancelFade(QString::fromLatin1(kPhonesNode));
    m_scene.masterPhonesMuted = muted;
    levelChanged();
}

void Engine::setMasterStream(double volume)
{
    cancelFade(QString::fromLatin1(kStreamNode));
    cancelFade(QString::fromLatin1(kVodNode));
    m_scene.masterStream = std::clamp(volume, 0.0, 1.0);
    levelChanged();
}

void Engine::setMasterStreamMuted(bool muted)
{
    m_panicStream = m_panicStream && muted;
    cancelFade(QString::fromLatin1(kStreamNode));
    cancelFade(QString::fromLatin1(kVodNode));
    m_scene.masterStreamMuted = muted;
    levelChanged();
}

void Engine::setMicMuted(bool muted)
{
    setBusMuted(QString::fromLatin1(kMicBusId), muted);
}

bool Engine::micMuted() const
{
    const Bus *mic = m_scene.micBus();
    return mic && mic->muted;
}

void Engine::setSidetoneEnabled(bool on)
{
    Bus *mic = m_scene.micBus();
    if (!mic) {
        return;
    }
    const bool stream = feedsStream(mic->destination);
    // On at zero volume would sound like it is broken.
    if (on && m_scene.sidetoneVolume <= 0.0) {
        m_scene.sidetoneVolume = kDefaultSidetoneVolume;
    }
    // Turning sidetone off never leaves the mic going nowhere: it falls back to Stream.
    setBusDestination(mic->id, on ? (stream ? Destination::Both : Destination::Phones) : Destination::Stream);
}

bool Engine::sidetoneEnabled() const
{
    const Bus *mic = m_scene.micBus();
    return mic && feedsPhones(mic->destination);
}

void Engine::setSidetoneVolume(double volume)
{
    m_scene.sidetoneVolume = std::clamp(volume, 0.0, 1.0);
    levelChanged();
}

// ---- solo -------------------------------------------------------------------------------

void Engine::setSolo(const QString &id, bool soloed)
{
    const Bus *b = m_scene.bus(id);
    if (!b || b->isInput()) {
        return;
    }
    if (soloed) {
        m_soloed.insert(id);
    } else {
        m_soloed.remove(id);
    }
    // A fade would hold dimmed buses unmuted until it ends.
    m_fades.clear();
    m_fadeTimer.stop();
    // Solo changes the live mix only; it never dirties or alters the saved scene.
    Q_EMIT soloChanged();
    Q_EMIT levelsChanged();
    scheduleReconcile();
}

bool Engine::dimmedBySolo(const QString &id) const
{
    const Bus *b = m_scene.bus(id);
    return b && !b->isInput() && anySolo() && !m_soloed.contains(id);
}

void Engine::clearSolo()
{
    if (m_soloed.isEmpty()) {
        return;
    }
    m_soloed.clear();
    m_fades.clear();
    m_fadeTimer.stop();
    Q_EMIT soloChanged();
    Q_EMIT levelsChanged();
    scheduleReconcile();
}

// ---- holds ------------------------------------------------------------------------------

void Engine::holdsChanged()
{
    Q_EMIT levelsChanged();
    scheduleReconcile();
}

void Engine::setPushToTalk(bool held)
{
    if (m_pushToTalk != held) {
        m_pushToTalk = held;
        holdsChanged();
    }
}

void Engine::setPushToMute(bool held)
{
    if (m_pushToMute != held) {
        m_pushToMute = held;
        holdsChanged();
    }
}

void Engine::setPanic(bool on)
{
    if (m_panicMic == on && m_panicStream == on) {
        return;
    }
    m_panicMic = on;
    m_panicStream = on;
    holdsChanged();
}

bool Engine::effectiveMicMuted() const
{
    return m_panicMic || m_pushToMute || (micMuted() && !m_pushToTalk);
}

bool Engine::effectiveStreamMuted() const
{
    return m_panicStream || m_scene.masterStreamMuted;
}

void Engine::releaseHolds()
{
    if (!m_pushToTalk && !m_pushToMute && !panic()) {
        return;
    }
    m_pushToTalk = m_pushToMute = m_panicMic = m_panicStream = false;
    Q_EMIT levelsChanged();
    if (m_pw->state() == pw::PwContext::State::Ready) {
        reconcileVolumes();
    }
}

// ---- structure --------------------------------------------------------------------------

QString Engine::addBus(const QString &name, const QString &color)
{
    if (!canAddBus()) {
        return {};
    }
    QStringList taken;
    for (const auto &b : m_scene.buses) {
        taken << b.id;
    }
    // Reserved node names.
    taken << QStringLiteral("phones") << QStringLiteral("stream") << QStringLiteral("sidetone")
          << QStringLiteral("mic") << QStringLiteral("micfx") << QStringLiteral("filtered");
    Bus b;
    b.name = name.trimmed().isEmpty() ? QStringLiteral("Bus") : name.trimmed();
    b.id = makeSlug(b.name, taken);
    if (color.isEmpty()) {
        const auto &pal = defaults::palette();
        b.color = QString::fromLatin1(pal.at(m_scene.buses.size() % pal.size()).hex);
    } else {
        b.color = color;
    }
    b.destination = Destination::Both;
    m_scene.buses.append(b);
    Q_EMIT structureChanged();
    levelChanged();
    return b.id;
}

bool Engine::removeBus(const QString &id)
{
    const Bus *b = m_scene.bus(id);
    if (!b || b->isInput()) {
        return false;
    }
    m_scene.buses.removeIf([&](const Bus &x) { return x.id == id; });
    m_scene.removeRulesForBus(id);
    for (auto it = m_sessionAssign.begin(); it != m_sessionAssign.end();) {
        it = it.value() == id ? m_sessionAssign.erase(it) : std::next(it);
    }
    m_soloed.remove(id);
    Q_EMIT structureChanged();
    Q_EMIT appsChanged();
    levelChanged();
    return true;
}

void Engine::renameBus(const QString &id, const QString &name)
{
    Bus *b = m_scene.bus(id);
    const QString n = name.trimmed();
    if (!b || n.isEmpty() || b->name == n) {
        return;
    }
    b->name = n;
    Q_EMIT structureChanged();
    levelChanged();
}

void Engine::recolorBus(const QString &id, const QString &color)
{
    Bus *b = m_scene.bus(id);
    if (!b || b->color == color) {
        return;
    }
    b->color = color;
    Q_EMIT structureChanged();
    levelChanged();
}

QString Engine::duplicateBus(const QString &id)
{
    const Bus *src = m_scene.bus(id);
    if (!src || src->isInput() || !canAddBus()) {
        return {};
    }
    const Bus copy = *src;
    const QString newId = addBus(copy.name + QStringLiteral(" copy"), copy.color);
    if (Bus *b = m_scene.bus(newId)) {
        b->volume = copy.volume;
        b->muted = copy.muted;
        b->balance = copy.balance;
        b->destination = copy.destination;
        levelChanged();
    }
    return newId;
}

// ---- devices ----------------------------------------------------------------------------

void Engine::setHeadphoneDevice(const QString &nodeName)
{
    if (m_headphones == nodeName) {
        return;
    }
    m_headphones = nodeName;
    m_headphonesWereMissing = false;
    Q_EMIT devicesChanged();
    scheduleReconcile();
}

void Engine::setMicDevice(const QString &nodeName)
{
    if (m_micDevice == nodeName) {
        return;
    }
    m_micDevice = nodeName;
    m_micWasMissing = false;
    m_lastSourceDescription.clear();
    Q_EMIT devicesChanged();
    scheduleReconcile();
}

void Engine::setMicFallback(bool on)
{
    if (m_micFallback == on) {
        return;
    }
    m_micFallback = on;
    Q_EMIT devicesChanged();
    scheduleReconcile();
}

namespace {

bool usableSink(const pw::Node &n)
{
    return n.isSink() && !n.isRostrum();
}

bool usableSource(const pw::Node &n)
{
    return n.isSource() && !n.isRostrum();
}

} // namespace

const pw::Node *resolveDevice(const pw::Graph &g, const QString &saved, const QString &systemDefault,
                              bool sink, bool fallback)
{
    auto usable = [sink](const pw::Node &n) { return sink ? usableSink(n) : usableSource(n); };
    if (const pw::Node *n = g.nodeByName(saved); n && usable(*n)) {
        return n;
    }
    if (!saved.isEmpty() && !fallback) {
        return nullptr;
    }
    if (const pw::Node *n = g.nodeByName(systemDefault); n && usable(*n)) {
        return n;
    }
    const pw::Node *best = nullptr;
    for (const auto &n : g.nodes) {
        if (usable(n) &&
            (!best || n.prop("priority.session").toInt() > best->prop("priority.session").toInt())) {
            best = &n;
        }
    }
    return best;
}

const pw::Node *Engine::resolveSink() const
{
    return resolveDevice(m_pw->graph(), m_headphones, m_pw->defaultSinkName(), true, true);
}

// A missing saved mic leaves rostrum.mic unlinked unless the user opted into a fallback: a webcam
// or laptop mic going live on stream by surprise is worse than silence.
const pw::Node *Engine::resolveSource() const
{
    return resolveDevice(m_pw->graph(), m_micDevice, m_pw->defaultSourceName(), false, m_micFallback);
}

QString Engine::resolvedSinkName() const
{
    const pw::Node *n = resolveSink();
    return n ? n->name : QString();
}

QString Engine::resolvedSourceName() const
{
    const pw::Node *n = resolveSource();
    return n ? n->name : QString();
}

bool Engine::headphonesMissing() const
{
    if (m_headphones.isEmpty() || m_pw->state() != pw::PwContext::State::Ready) {
        return false;
    }
    const pw::Node *n = m_pw->graph().nodeByName(m_headphones);
    return !n || !usableSink(*n);
}

bool Engine::micMissing() const
{
    if (m_micDevice.isEmpty() || m_pw->state() != pw::PwContext::State::Ready) {
        return false;
    }
    const pw::Node *n = m_pw->graph().nodeByName(m_micDevice);
    return !n || !usableSource(*n);
}

bool Engine::micSilenced() const
{
    return !m_micFallback && micMissing();
}

void Engine::setMonoHeadphones(bool on)
{
    if (m_monoHeadphones == on) {
        return;
    }
    m_monoHeadphones = on;
    qCInfo(lcEngine) << "mono headphones" << on;
    scheduleReconcile();
}

QString Engine::missingMicLabel() const
{
    return m_lastSourceDescription.isEmpty() ? m_micDevice : m_lastSourceDescription;
}

void Engine::reconcileDevices()
{
    const bool missing = headphonesMissing();
    if (const pw::Node *n = m_pw->graph().nodeByName(m_headphones)) {
        m_lastSinkDescription = n->label();
    }
    if (missing && !m_headphonesWereMissing) {
        m_headphonesWereMissing = true;
        qCInfo(lcEngine) << "headphones missing:" << m_headphones << "falling back to" << resolvedSinkName();
        Q_EMIT headphonesLost(m_lastSinkDescription.isEmpty() ? m_headphones : m_lastSinkDescription);
    } else if (!missing && m_headphonesWereMissing) {
        m_headphonesWereMissing = false;
        qCInfo(lcEngine) << "headphones back:" << m_headphones;
        Q_EMIT headphonesRestored();
    }
    const bool micGone = micMissing();
    if (const pw::Node *n = m_pw->graph().nodeByName(m_micDevice)) {
        m_lastSourceDescription = n->label();
    }
    if (micGone && !m_micWasMissing) {
        m_micWasMissing = true;
        if (m_micFallback) {
            qCInfo(lcEngine) << "mic missing:" << m_micDevice << "falling back to" << resolvedSourceName();
        } else {
            qCInfo(lcEngine) << "mic missing:" << m_micDevice << "stream mic silent until it returns";
        }
        Q_EMIT micLost(missingMicLabel());
    } else if (!micGone && m_micWasMissing) {
        m_micWasMissing = false;
        qCInfo(lcEngine) << "mic back:" << m_micDevice;
        Q_EMIT micRestored();
    }
    const QString sig = QStringList{resolvedSinkName(), resolvedSourceName(), missing ? QStringLiteral("1") : QString(),
                                    micGone ? QStringLiteral("1") : QString(), micMeterNode()}
                            .join(QLatin1Char('|'));
    if (sig != m_devicesSignature) {
        m_devicesSignature = sig;
        Q_EMIT devicesChanged();
    }
}

// ---- ducking ----------------------------------------------------------------------------

void Engine::setDucking(const ducking::Settings &settings)
{
    const ducking::Settings s = ducking::sanitize(settings);
    if (s == m_ducking) {
        return;
    }
    if (s.enabled != m_ducking.enabled) {
        qCInfo(lcEngine) << "ducking" << s.enabled;
    }
    m_ducking = s;
    reconcileDucking();
    Q_EMIT duckedChanged();
    scheduleReconcile();
}

QString Engine::duckTriggerBus() const
{
    if (m_ducking.trigger == ducking::Trigger::Mic) {
        return {};
    }
    const Bus *b = m_scene.busFor(AppCategory::Voice);
    return b ? b->id : QString();
}

bool Engine::isDuckTarget(const QString &busId) const
{
    const Bus *b = m_scene.bus(busId);
    return m_ducking.enabled && b && !b->isInput() && m_ducking.buses.contains(busId) &&
           busId != duckTriggerBus();
}

bool Engine::isDucked(const QString &busId) const
{
    return m_duckEnvelope.ducked() && isDuckTarget(busId);
}

void Engine::reconcileDucking()
{
    const bool on = m_ducking.enabled && m_mixEnabled;
    QString mic;
    QString voice;
    if (on && m_ducking.trigger != ducking::Trigger::Voice) {
        // A source meter keeps the mic open, so it runs only while the mic can be heard on stream.
        const Bus *micBus = m_scene.micBus();
        if (micBus && !effectiveMicMuted() && feedsStream(micBus->destination) && !micSilenced()) {
            mic = micMeterNode();
        }
    }
    if (on && m_ducking.trigger != ducking::Trigger::Mic) {
        if (const Bus *b = m_scene.busFor(AppCategory::Voice)) {
            voice = b->nodeName();
        }
    }
    m_duckMicNode = mic;
    m_duckVoiceNode = voice;
    QStringList targets;
    if (!mic.isEmpty()) {
        targets << mic;
    }
    if (!voice.isEmpty()) {
        targets << voice;
    }
    m_duckMeters.setTargets(targets, mic.isEmpty() ? QStringList() : QStringList{mic});
    m_duckMeters.setActive(on);
    if (on && !m_duckTimer.isActive()) {
        m_duckClock.start();
        m_duckTimer.start();
    } else if (!on && m_duckTimer.isActive()) {
        m_duckTimer.stop();
        const bool was = m_duckEnvelope.ducked();
        m_duckEnvelope.reset();
        if (was) {
            Q_EMIT duckedChanged();
        }
    }
}

void Engine::duckTick()
{
    const double dt = double(m_duckClock.restart());
    double peak = 0.0;
    if (!m_duckMicNode.isEmpty()) {
        const Bus *mic = m_scene.micBus();
        peak = m_duckMeters.takePeak(m_duckMicNode) * volume::faderToLinear(mic ? mic->volume : 1.0);
    }
    if (!m_duckVoiceNode.isEmpty()) {
        peak = std::max(peak, double(m_duckMeters.takePeak(m_duckVoiceNode)));
    }
    const bool was = m_duckEnvelope.ducked();
    m_duckEnvelope.advance(peak, dt, m_ducking);
    if (was != m_duckEnvelope.ducked()) {
        Q_EMIT duckedChanged();
    }
    if (m_duckEnvelope.gain() != m_duckGainSent && m_pw->state() == pw::PwContext::State::Ready) {
        reconcileVolumes();
    }
}

bool Engine::releaseTransientLevels()
{
    m_duckTimer.stop();
    m_duckMeters.setActive(false);
    m_fadeTimer.stop();
    if (m_duckEnvelope.gain() >= 1.0 && m_fades.isEmpty()) {
        return false;
    }
    m_duckEnvelope.reset();
    m_fades.clear();
    if (m_pw->state() == pw::PwContext::State::Ready) {
        reconcileVolumes();
    }
    return true;
}

// ---- links ------------------------------------------------------------------------------

void Engine::reconcileLinks()
{
    if (!m_mixEnabled) {
        return;
    }
    const auto &g = m_pw->graph();
    const pw::Node *phones = g.nodeByName(QString::fromLatin1(kPhonesNode));
    const pw::Node *stream = g.nodeByName(QString::fromLatin1(kStreamNode));
    const pw::Node *vod = g.nodeByName(QString::fromLatin1(kVodNode));
    const pw::Node *mic = g.nodeByName(QString::fromLatin1(kMicNode));
    const pw::Node *sidetone = g.nodeByName(QString::fromLatin1(kSidetoneNode));
    const pw::Node *hwSink = resolveSink();
    const pw::Node *hwSource = resolveSource();

    QList<QPair<const pw::Node *, const pw::Node *>> nodePairs;
    for (const auto &b : m_scene.buses) {
        if (b.isInput()) {
            continue;
        }
        const pw::Node *bus = g.nodeByName(b.nodeName());
        if (!bus) {
            continue;
        }
        if (feedsPhones(b.destination) && phones) {
            nodePairs.append(qMakePair(bus, phones));
        }
        if (feedsStream(b.destination) && stream) {
            nodePairs.append(qMakePair(bus, stream));
        }
        if (feedsVod(b) && vod) {
            nodePairs.append(qMakePair(bus, vod));
        }
    }
    if (phones && hwSink) {
        nodePairs.append(qMakePair(phones, hwSink));
    }
    // With filters running the stream mic and sidetone hear the filtered signal. hwSource, when
    // there is one, is the mic apps use too; without it the stream mic stays silent (no fallback)
    // while apps still get the filtered default mic.
    const pw::Node *micFx = micFxRunning() ? micFxNode() : nullptr;
    const pw::Node *filtered = micFx ? filteredNode() : nullptr;
    const pw::Node *appsSource = micFx ? appsMic() : nullptr;
    if (micFx && appsSource) {
        nodePairs.append(qMakePair(appsSource, micFx));
        if (filtered) {
            nodePairs.append(qMakePair(micFx, filtered));
        }
    }
    const pw::Node *voice = micFx && appsSource ? micFx : hwSource;
    if (hwSource && mic) {
        nodePairs.append(qMakePair(voice, mic));
    }
    if (hwSource && sidetone) {
        nodePairs.append(qMakePair(voice, sidetone));
    }
    if (sidetone && phones) {
        nodePairs.append(qMakePair(sidetone, phones));
    }

    // Ports of a new node arrive one by one. Linking a half-enumerated stereo device as if it were
    // mono would create links that get torn down a moment later, so wait for the full set.
    auto complete = [](const pw::Node *n, const QList<pw::Port> &ports) {
        const int expect = n->channels > 0 ? n->channels : n->prop("audio.channels").toInt();
        return !ports.isEmpty() && (expect <= 0 || ports.size() >= expect);
    };

    QSet<QPair<uint32_t, uint32_t>> wanted;
    m_phonesCrossLinkWanted = false;
    for (const auto &[out, in] : nodePairs) {
        const auto outPorts = g.outputPorts(out->id);
        const auto inPorts = g.inputPorts(in->id);
        if (!complete(out, outPorts) || !complete(in, inPorts)) {
            continue;
        }
        const bool mono = m_monoHeadphones && out == phones && in == hwSink;
        const auto pairs = mono ? pw::monoPorts(outPorts, inPorts) : pw::matchPorts(outPorts, inPorts);
        if (mono) {
            m_phonesCrossLinkWanted = pairs.size() > outPorts.size();
        }
        for (const auto &p : pairs) {
            wanted.insert(p);
        }
    }

    // A link is Rostrum's to manage if it starts at a Rostrum node and ends at a Rostrum node or a
    // hardware sink, or if it feeds the mic, sidetone or mic filter node. App streams into buses
    // and OBS capturing a Rostrum monitor are never touched.
    const pw::Node *micFxAny = g.nodeByName(QString::fromLatin1(kMicFxNode));
    auto managed = [&](const pw::Link &l) {
        const pw::Node *out = g.node(l.outNode);
        const pw::Node *in = g.node(l.inNode);
        if (!out || !in) {
            return false;
        }
        // PipeWire drops a node's links with it; destroying them as well only produces errors.
        if (m_pendingDestroy.contains(out->id) || m_pendingDestroy.contains(in->id)) {
            return false;
        }
        if (in == mic || in == sidetone || (in == micFxAny && isOwnedNode(*in))) {
            return true;
        }
        if (!isOwnedNode(*out)) {
            return false;
        }
        return isOwnedNode(*in) || (in->isSink() && !in->isRostrum());
    };

    for (const auto &l : g.links) {
        if (managed(l) && !wanted.contains({l.outPort, l.inPort}) && !m_pendingDestroy.contains(l.id)) {
            qCInfo(lcEngine) << "unlink" << g.node(l.outNode)->name << "->" << g.node(l.inNode)->name;
            destroyOnce(l.id);
        }
    }
    for (const auto &p : std::as_const(wanted)) {
        const QString key = QStringLiteral("%1:%2").arg(p.first).arg(p.second);
        if (g.hasLink(p.first, p.second)) {
            m_pendingLinks.remove(key);
            continue;
        }
        auto it = m_pendingLinks.find(key);
        if (it != m_pendingLinks.end() && it->elapsed() < kLinkRetryMs) {
            continue;
        }
        m_pw->createLink(p.first, p.second);
        QElapsedTimer t;
        t.start();
        m_pendingLinks.insert(key, t);
    }
}

// ---- volumes ----------------------------------------------------------------------------

bool Engine::phonesCrossLinked() const
{
    const auto &g = m_pw->graph();
    const pw::Node *phones = g.nodeByName(QString::fromLatin1(kPhonesNode));
    const pw::Node *hwSink = resolveSink();
    if (!phones || !hwSink) {
        return false;
    }
    for (const auto &l : g.links) {
        if (l.outNode != phones->id || l.inNode != hwSink->id) {
            continue;
        }
        const QString out = g.ports.value(l.outPort).channel;
        const QString in = g.ports.value(l.inPort).channel;
        if (!out.isEmpty() && out != in && (in == QLatin1String("FL") || in == QLatin1String("FR"))) {
            return true;
        }
    }
    return false;
}

void Engine::reconcileVolumes()
{
    if (!m_mixEnabled) {
        return;
    }
    const auto &g = m_pw->graph();
    // gain multiplies the linear volume for session-only effects that never touch the scene.
    auto apply = [&](const QString &nodeName, double fader, bool mute, double balance = 0.0,
                     double gain = 1.0) {
        const pw::Node *n = g.nodeByName(nodeName);
        if (!n) {
            return;
        }
        QList<float> want;
        if (balance == 0.0) {
            want = {float(volume::faderToLinear(fader) * gain)};
        } else {
            // Bus nodes are FL, FR in that order.
            want = {float(volume::faderToLinear(volume::balancedPosition(fader, balance, false)) * gain),
                    float(volume::faderToLinear(volume::balancedPosition(fader, balance, true)) * gain)};
        }
        auto &sent = m_sentVolume[n->id];
        bool sentSame = sent.mute == mute && sent.volumes.size() == want.size();
        for (qsizetype i = 0; sentSame && i < want.size(); ++i) {
            sentSame = qFuzzyCompare(sent.volumes.at(i) + 1.0f, want.at(i) + 1.0f);
        }
        // Something else (WirePlumber's state restore, another mixer) may change our nodes;
        // the scene wins, but at most once per second so two tools cannot spin.
        bool reportedDiffers = !n->volumes.isEmpty() && n->muted != mute;
        for (qsizetype i = 0; i < n->volumes.size(); ++i) {
            reportedDiffers = reportedDiffers || qAbs(n->volumes.at(i) - want.value(i, want.last())) > 0.001f;
        }
        if (sentSame && !(reportedDiffers && sent.when.elapsed() >= kVolumeResendMs)) {
            return;
        }
        sent.volumes = want;
        sent.mute = mute;
        sent.when.start();
        m_pw->setNodeVolumes(n->id, want, mute);
    };

    // Mid-fade a node is unmuted and ramps from its old level; a target mute lands when it ends.
    auto level = [&](const QString &nodeName, double fader, bool mute, double balance = 0.0,
                     double gain = 1.0) {
        if (const auto f = fadingLevel(nodeName)) {
            apply(nodeName, f->position, false, f->balance, gain);
        } else {
            apply(nodeName, fader, mute, balance, gain);
        }
    };

    m_duckGainSent = m_duckEnvelope.gain();
    for (const auto &b : m_scene.buses) {
        if (b.isInput()) {
            continue;
        }
        level(b.nodeName(), b.volume, b.muted || dimmedBySolo(b.id), b.balance,
              isDuckTarget(b.id) ? m_duckGainSent : 1.0);
    }
    // Halve while mono is wanted or any cross-link is still up, so links and volume changing at
    // slightly different moments can only make it briefly quieter, never 6 dB louder.
    const double phonesGain = (m_phonesCrossLinkWanted || phonesCrossLinked()) ? 0.5 : 1.0;
    level(QString::fromLatin1(kPhonesNode), m_scene.masterPhones, m_scene.masterPhonesMuted, 0.0, phonesGain);
    if (m_panicStream) {
        apply(QString::fromLatin1(kStreamNode), m_scene.masterStream, true);
        apply(QString::fromLatin1(kVodNode), m_scene.masterStream, true);
    } else {
        level(QString::fromLatin1(kStreamNode), m_scene.masterStream, m_scene.masterStreamMuted);
        level(QString::fromLatin1(kVodNode), m_scene.masterStream, m_scene.masterStreamMuted);
    }
    if (const Bus *mic = m_scene.micBus()) {
        const bool micMuted = micSilenced() || effectiveMicMuted();
        apply(QString::fromLatin1(kMicNode), mic->volume, micMuted || !feedsStream(mic->destination));
        apply(QString::fromLatin1(kSidetoneNode), m_scene.sidetoneVolume,
              micMuted || !feedsPhones(mic->destination) || m_scene.sidetoneVolume <= 0.0);
    }
    for (auto it = m_sentVolume.begin(); it != m_sentVolume.end();) {
        it = g.node(it.key()) ? std::next(it) : m_sentVolume.erase(it);
    }
}

} // namespace rostrum::engine
