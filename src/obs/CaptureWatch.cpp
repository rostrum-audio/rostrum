#include "obs/CaptureWatch.h"

namespace rostrum::obs {

namespace {
const QString kDevicePrefix = QStringLiteral("rostrum.");

QList<CaptureKey> keysOf(const QList<Recording> &recordings)
{
    QList<CaptureKey> keys;
    for (const Recording &r : recordings) {
        if (!r.device.startsWith(kDevicePrefix)) {
            continue;
        }
        const CaptureKey key{r.source, r.device};
        if (!keys.contains(key)) {
            keys.append(key);
        }
    }
    return keys;
}

bool sameObs(const ObsProcess &a, const ObsProcess &b)
{
    return a.pid == b.pid && a.startTime == b.startTime;
}

bool sameSet(const QList<CaptureKey> &a, const QList<CaptureKey> &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (const CaptureKey &k : a) {
        if (!b.contains(k)) {
            return false;
        }
    }
    return true;
}
} // namespace

void CaptureWatch::setNowMs(qint64 ms)
{
    m_now = ms;
}

void CaptureWatch::advance(qint64 nowMs)
{
    m_now = nowMs;
}

void CaptureWatch::resetBaseline()
{
    m_candidate.clear();
    m_committed.clear();
    m_labels.clear();
    m_committedObs.reset();
    m_epoch = false;
    m_alert = false;
}

void CaptureWatch::observe(const QList<Recording> &recordings, const std::optional<ObsProcess> &obs,
                           qint64 nowMs)
{
    advance(nowMs);
    if (m_disconnected) {
        return;
    }
    m_obs = obs;
    if (!obs) {
        resetBaseline();
        m_watching = false;
        m_haveCurrent = false;
        m_current.clear();
        return;
    }
    if (m_committedObs && !sameObs(*m_committedObs, *obs)) { // OBS restarted: that baseline is void
        resetBaseline();
    }

    m_current = keysOf(recordings);
    m_haveCurrent = true;
    for (const Recording &r : recordings) {
        if (r.device.startsWith(kDevicePrefix)) {
            m_labels.insert(r.device, r.what.isEmpty() ? r.device : r.what);
        }
    }
    if (!m_watching || !sameSet(m_candidate, m_current)) {
        m_candidate = m_current;
        m_candidateSince = nowMs;
    }
    m_watching = true;
    promoteCandidate();
    evaluate();
}

void CaptureWatch::promoteCandidate()
{
    // No new baseline while an epoch is pending: a partial return must not become the truth.
    if (!m_watching || m_epoch || m_now - m_candidateSince < kBaselineStableMs) {
        return;
    }
    m_committed = m_candidate;
    m_committedObs = m_obs;
}

void CaptureWatch::onDisconnected(qint64 nowMs)
{
    advance(nowMs);
    m_disconnected = true;
    m_watching = false;
    m_haveCurrent = false;
    m_current.clear();
    m_candidate.clear();
    m_epoch = false;
    m_alert = false;
    m_mixReady = false; // a stream mix cannot be ready without PipeWire
    m_mixReadySince = -1;
}

void CaptureWatch::onReconnected(qint64 nowMs)
{
    advance(nowMs);
    m_disconnected = false;
    m_epoch = true;
    m_epochStart = nowMs;
    m_dismissed = false;
    m_alert = false;
    m_haveCurrent = false;
}

void CaptureWatch::setMix(bool enabled, bool ready, bool error)
{
    m_mixEnabled = enabled;
    m_mixError = error;
    const bool wasReady = m_mixReady;
    m_mixReady = ready;
    if (!(enabled && ready && !error)) {
        m_mixReadySince = -1;
    } else if (!wasReady || m_mixReadySince < 0) {
        m_mixReadySince = m_now;
    }
    evaluate();
}

void CaptureWatch::setObsLive(bool streamingOrRecording)
{
    m_obsLive = streamingOrRecording;
}

void CaptureWatch::tick(qint64 nowMs)
{
    advance(nowMs);
    promoteCandidate();
    evaluate();
}

bool CaptureWatch::decisionDue() const
{
    if (!m_mixEnabled || m_mixError) {
        return false;
    }
    if (m_now - m_epochStart >= kEpochCapAfterReadyMs) {
        return true;
    }
    return m_mixReady && m_mixReadySince >= 0 && m_now - m_mixReadySince >= kGraceAfterMixReadyMs;
}

QList<CaptureKey> CaptureWatch::missing() const
{
    QList<CaptureKey> out;
    for (const CaptureKey &k : m_committed) {
        if (!m_current.contains(k)) {
            out.append(k);
        }
    }
    return out;
}

void CaptureWatch::evaluate()
{
    if (!m_epoch || !m_watching || !m_haveCurrent || !m_obs || !m_committedObs
        || !sameObs(*m_committedObs, *m_obs) || m_committed.isEmpty()) {
        return;
    }
    if (missing().isEmpty()) { // everything is back: the epoch is resolved
        m_alert = false;
        m_epoch = false;
        return;
    }
    if (!m_alert && !m_dismissed && decisionDue()) {
        m_alert = true;
        m_liveSeverity = m_obsLive;
    }
}

QStringList CaptureWatch::missingDevices() const
{
    QStringList names;
    if (!m_alert) {
        return names;
    }
    for (const CaptureKey &k : missing()) {
        const QString name = m_labels.value(k.device, k.device);
        if (!names.contains(name)) {
            names.append(name);
        }
    }
    return names;
}

void CaptureWatch::dismiss()
{
    m_alert = false;
    m_dismissed = true;
}

} // namespace rostrum::obs
