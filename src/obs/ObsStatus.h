#pragma once

#include "core/Model.h"
#include "obs/ObsCaptures.h"
#include "obs/ObsClient.h"

#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>

namespace rostrum::obs {

// Whether OBS streams or records, and which scene is on program, kept up to date from
// obs-websocket events. Read-only: it never asks OBS to change anything.
class LiveStatus : public QObject
{
    Q_OBJECT
public:
    explicit LiveStatus(Client *client, QObject *parent = nullptr);

    // Connected, and OBS has answered the first status requests.
    bool known() const { return m_known; }
    bool streaming() const { return m_streaming; }
    bool recording() const { return m_recording; }
    bool recordPaused() const { return m_recordPaused; }
    // When the output started, in ms since the epoch, worked out from OBS's own duration; 0 when off.
    qint64 streamStartMs() const { return m_streamStart; }
    qint64 recordStartMs() const { return m_recordStart; }
    // How long the recording ran before it was paused.
    qint64 recordPausedElapsedMs() const { return m_recordPausedElapsed; }
    QString programScene() const { return m_programScene; }
    QStringList scenes() const { return m_scenes; } // top first, as OBS shows them

Q_SIGNALS:
    void changed();
    // Streaming went from off to on, or was already on when Rostrum connected.
    void streamStarted();
    // OBS put another scene on program. Not emitted for the scene found at connect.
    void programSceneChanged(const QString &name);

private:
    void onStatus();
    void onEvent(const QString &type, const QJsonObject &data);
    void reset();
    void fetchStream();
    void fetchRecord();
    void fetchScenes();
    void setStreaming(bool on, qint64 durationMs);
    void finishOne();

    QPointer<Client> m_client;
    int m_generation = 0;
    int m_pending = 0;
    bool m_known = false;
    bool m_streaming = false;
    bool m_recording = false;
    bool m_recordPaused = false;
    qint64 m_streamStart = 0;
    qint64 m_recordStart = 0;
    qint64 m_recordPausedElapsed = 0;
    QString m_programScene;
    QStringList m_scenes;
};

enum class GoLiveProblem
{
    MicMuted,           // muted, at zero, or kept off the stream
    StreamMixSilent,    // Master Stream muted or at zero, or no bus reaches the stream
    NoStreamMixCapture, // OBS records nothing from Rostrum Stream Mix
    NoMicCapture,       // OBS records nothing from Rostrum Mic
};

// What would make a stream start badly. `soloed` is the engine's live solo set, which dims the
// other buses on the stream too. `recordings` is what OBS records according to PipeWire, or null
// when that isn't known.
QList<GoLiveProblem> goLiveProblems(const Scene &scene, const QSet<QString> &soloed,
                                    const QList<Recording> *recordings);

// The Rostrum scene to switch to when OBS puts `obsScene` on program; empty for no change.
QString mappedScene(const QMap<QString, QString> &map, const QString &obsScene,
                    const QStringList &rostrumScenes);

} // namespace rostrum::obs
