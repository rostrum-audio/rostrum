#pragma once
#include "obs/ObsCaptures.h"
#include "obs/ObsConfig.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace rostrum::obs {

// One Rostrum capture inside OBS: which OBS source records which "rostrum.*" node.
struct CaptureKey
{
    QString source;
    QString device; // must start with "rostrum."
    bool operator==(const CaptureKey &) const = default;
};

// Decides whether OBS lost its Rostrum captures across a PipeWire restart. Pure logic: time and
// all inputs are injected, nothing here touches PipeWire, OBS or the UI.
class CaptureWatch
{
public:
    static constexpr qint64 kBaselineStableMs = 2000;
    static constexpr qint64 kGraceAfterMixReadyMs = 8000;
    static constexpr qint64 kEpochCapAfterReadyMs = 20000;

    void setNowMs(qint64 ms);

    // While PipeWire is Ready: the current recordings and the OBS process (nullopt: no OBS).
    void observe(const QList<Recording> &recordings, const std::optional<ObsProcess> &obs,
                 qint64 nowMs);
    // PipeWire left Ready: freeze the baseline, drop the unconfirmed candidate.
    void onDisconnected(qint64 nowMs);
    // PipeWire is Ready again: start an epoch.
    void onReconnected(qint64 nowMs);
    void setMix(bool enabled, bool ready, bool error);
    // Whether OBS is streaming or recording (only affects liveSeverity()).
    void setObsLive(bool streamingOrRecording);
    // Advance debounce and grace timers.
    void tick(qint64 nowMs);

    bool alertActive() const { return m_alert; }
    QStringList missingDevices() const; // empty unless the alert is active
    bool liveSeverity() const { return m_liveSeverity; } // OBS live when the alert was raised
    void dismiss();                                       // quiet until the next epoch

private:
    void advance(qint64 nowMs);
    void resetBaseline();
    void promoteCandidate();
    void evaluate();
    bool decisionDue() const;
    QList<CaptureKey> missing() const;

    // Baseline: candidate is being debounced, committed is the confirmed set.
    QList<CaptureKey> m_candidate;
    qint64 m_candidateSince = 0;
    QList<CaptureKey> m_committed;
    QHash<QString, QString> m_labels; // device -> human-facing name
    std::optional<ObsProcess> m_committedObs;

    // Latest observation while Ready.
    std::optional<ObsProcess> m_obs;
    QList<CaptureKey> m_current;
    bool m_watching = false; // Ready, with OBS present, and observed
    bool m_haveCurrent = false;
    bool m_disconnected = false;

    // Epoch (after a reconnect).
    bool m_epoch = false;
    qint64 m_epochStart = 0;
    bool m_dismissed = false;
    bool m_alert = false;
    bool m_liveSeverity = false;

    bool m_mixEnabled = false;
    bool m_mixReady = false;
    bool m_mixError = false;
    qint64 m_mixReadySince = -1;
    bool m_obsLive = false;

    qint64 m_now = 0;
};

} // namespace rostrum::obs
