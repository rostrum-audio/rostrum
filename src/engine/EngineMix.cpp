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
    if (!b || b->muted == muted) {
        return;
    }
    b->muted = muted;
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

void Engine::setMasterPhones(double volume)
{
    m_scene.masterPhones = std::clamp(volume, 0.0, 1.0);
    levelChanged();
}

void Engine::setMasterPhonesMuted(bool muted)
{
    m_scene.masterPhonesMuted = muted;
    levelChanged();
}

void Engine::setMasterStream(double volume)
{
    m_scene.masterStream = std::clamp(volume, 0.0, 1.0);
    levelChanged();
}

void Engine::setMasterStreamMuted(bool muted)
{
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
    Q_EMIT soloChanged();
    Q_EMIT levelsChanged();
    scheduleReconcile();
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
          << QStringLiteral("mic");
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

const pw::Node *Engine::resolveSink() const
{
    const auto &g = m_pw->graph();
    if (const pw::Node *n = g.nodeByName(m_headphones); n && usableSink(*n)) {
        return n;
    }
    if (const pw::Node *n = g.nodeByName(m_pw->defaultSinkName()); n && usableSink(*n)) {
        return n;
    }
    const pw::Node *best = nullptr;
    for (const auto &n : g.nodes) {
        if (usableSink(n) && (!best || n.prop("priority.session").toInt() > best->prop("priority.session").toInt())) {
            best = &n;
        }
    }
    return best;
}

const pw::Node *Engine::resolveSource() const
{
    const auto &g = m_pw->graph();
    if (const pw::Node *n = g.nodeByName(m_micDevice); n && usableSource(*n)) {
        return n;
    }
    if (const pw::Node *n = g.nodeByName(m_pw->defaultSourceName()); n && usableSource(*n)) {
        return n;
    }
    const pw::Node *best = nullptr;
    for (const auto &n : g.nodes) {
        if (usableSource(n) &&
            (!best || n.prop("priority.session").toInt() > best->prop("priority.session").toInt())) {
            best = &n;
        }
    }
    return best;
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
    const QString sig = QStringList{resolvedSinkName(), resolvedSourceName(), missing ? QStringLiteral("1") : QString(),
                                    micMissing() ? QStringLiteral("1") : QString()}
                            .join(QLatin1Char('|'));
    if (sig != m_devicesSignature) {
        m_devicesSignature = sig;
        Q_EMIT devicesChanged();
    }
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
    }
    if (phones && hwSink) {
        nodePairs.append(qMakePair(phones, hwSink));
    }
    if (hwSource && mic) {
        nodePairs.append(qMakePair(hwSource, mic));
    }
    if (hwSource && sidetone) {
        nodePairs.append(qMakePair(hwSource, sidetone));
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
    for (const auto &[out, in] : nodePairs) {
        const auto outPorts = g.outputPorts(out->id);
        const auto inPorts = g.inputPorts(in->id);
        if (!complete(out, outPorts) || !complete(in, inPorts)) {
            continue;
        }
        const auto pairs = pw::matchPorts(outPorts, inPorts);
        for (const auto &p : pairs) {
            wanted.insert(p);
        }
    }

    // A link is Rostrum's to manage if it starts at a Rostrum node and ends at a Rostrum node or a
    // hardware sink, or if it feeds the mic or sidetone node. App streams into buses and OBS
    // capturing a Rostrum monitor are never touched.
    auto managed = [&](const pw::Link &l) {
        const pw::Node *out = g.node(l.outNode);
        const pw::Node *in = g.node(l.inNode);
        if (!out || !in) {
            return false;
        }
        if (in == mic || in == sidetone) {
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

void Engine::reconcileVolumes()
{
    if (!m_mixEnabled) {
        return;
    }
    const auto &g = m_pw->graph();
    auto apply = [&](const QString &nodeName, double fader, bool mute) {
        const pw::Node *n = g.nodeByName(nodeName);
        if (!n) {
            return;
        }
        const float linear = float(volume::faderToLinear(fader));
        auto &sent = m_sentVolume[n->id];
        const bool sentSame = qFuzzyCompare(sent.linear + 1.0f, linear + 1.0f) && sent.mute == mute;
        // Something else (WirePlumber's state restore, another mixer) may change our nodes;
        // the scene wins, but at most once per second so two tools cannot spin.
        const bool reportedDiffers = !n->volumes.isEmpty() &&
                                     (qAbs(n->volumes.first() - linear) > 0.001f || n->muted != mute);
        if (sentSame && !(reportedDiffers && sent.when.elapsed() >= kVolumeResendMs)) {
            return;
        }
        sent.linear = linear;
        sent.mute = mute;
        sent.when.start();
        m_pw->setNodeVolume(n->id, linear, mute);
    };

    for (const auto &b : m_scene.buses) {
        if (b.isInput()) {
            continue;
        }
        apply(b.nodeName(), b.volume, b.muted || dimmedBySolo(b.id));
    }
    apply(QString::fromLatin1(kPhonesNode), m_scene.masterPhones, m_scene.masterPhonesMuted);
    apply(QString::fromLatin1(kStreamNode), m_scene.masterStream, m_scene.masterStreamMuted);
    if (const Bus *mic = m_scene.micBus()) {
        apply(QString::fromLatin1(kMicNode), mic->volume, mic->muted || !feedsStream(mic->destination));
        apply(QString::fromLatin1(kSidetoneNode), m_scene.sidetoneVolume,
              mic->muted || !feedsPhones(mic->destination) || m_scene.sidetoneVolume <= 0.0);
    }
    for (auto it = m_sentVolume.begin(); it != m_sentVolume.end();) {
        it = g.node(it.key()) ? std::next(it) : m_sentVolume.erase(it);
    }
}

} // namespace rostrum::engine
