#include "obs/ObsStatus.h"

#include "obs/Readiness.h"

#include <QDateTime>
#include <QJsonArray>
#include <algorithm>

namespace rostrum::obs {

namespace {

const QString kPaused = QStringLiteral("OBS_WEBSOCKET_OUTPUT_PAUSED");
const QString kResumed = QStringLiteral("OBS_WEBSOCKET_OUTPUT_RESUMED");

qint64 now()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QStringList sceneNames(const QJsonArray &list)
{
    QStringList out;
    // obs-websocket lists scenes bottom first.
    for (qsizetype n = list.size() - 1; n >= 0; --n) {
        const QString name = list.at(n).toObject().value(QLatin1String("sceneName")).toString();
        if (!name.isEmpty()) {
            out << name;
        }
    }
    return out;
}

} // namespace

LiveStatus::LiveStatus(Client *client, QObject *parent)
    : QObject(parent)
    , m_client(client)
{
    connect(client, &Client::statusChanged, this, &LiveStatus::onStatus);
    connect(client, &Client::event, this, &LiveStatus::onEvent);
    onStatus();
}

void LiveStatus::onStatus()
{
    if (!m_client || m_client->status() != Client::Status::Connected) {
        reset();
        return;
    }
    if (m_known || m_pending > 0) {
        return;
    }
    m_pending = 4;
    fetchStream();
    fetchRecord();
    fetchScenes();
    const int generation = m_generation;
    m_client->request(QStringLiteral("GetCurrentProgramScene"), {},
                      [this, generation](bool ok, const QJsonObject &d, const QString &) {
                          if (generation != m_generation) {
                              return;
                          }
                          if (ok) {
                              m_programScene = d.value(QLatin1String("sceneName")).toString();
                              if (m_programScene.isEmpty()) {
                                  m_programScene =
                                      d.value(QLatin1String("currentProgramSceneName")).toString();
                              }
                          }
                          finishOne();
                      });
}

void LiveStatus::reset()
{
    ++m_generation;
    m_pending = 0;
    const bool had =
        m_known || m_streaming || m_recording || !m_programScene.isEmpty() || !m_scenes.isEmpty();
    m_known = false;
    m_streaming = false;
    m_recording = false;
    m_recordPaused = false;
    m_streamStart = 0;
    m_recordStart = 0;
    m_recordPausedElapsed = 0;
    m_programScene.clear();
    m_scenes.clear();
    if (had) {
        Q_EMIT changed();
    }
}

void LiveStatus::setStreaming(bool on, qint64 durationMs)
{
    const bool started = on && !m_streaming;
    m_streaming = on;
    m_streamStart = on ? now() - std::max<qint64>(0, durationMs) : 0;
    // Before the first answers are all in, finishOne() reports the state once.
    if (!m_known) {
        return;
    }
    Q_EMIT changed();
    if (started) {
        Q_EMIT streamStarted();
    }
}

void LiveStatus::finishOne()
{
    if (m_pending == 0 || --m_pending > 0) {
        return;
    }
    m_known = true;
    Q_EMIT changed();
    if (m_streaming) {
        Q_EMIT streamStarted();
    }
}

void LiveStatus::fetchStream()
{
    const int generation = m_generation;
    m_client->request(QStringLiteral("GetStreamStatus"), {},
                      [this, generation](bool ok, const QJsonObject &d, const QString &) {
                          if (generation != m_generation) {
                              return;
                          }
                          if (ok) {
                              setStreaming(d.value(QLatin1String("outputActive")).toBool(),
                                           qint64(d.value(QLatin1String("outputDuration")).toDouble()));
                          }
                          finishOne();
                      });
}

void LiveStatus::fetchRecord()
{
    const int generation = m_generation;
    m_client->request(QStringLiteral("GetRecordStatus"), {},
                      [this, generation](bool ok, const QJsonObject &d, const QString &) {
                          if (generation != m_generation) {
                              return;
                          }
                          if (ok) {
                              const qint64 duration =
                                  qint64(d.value(QLatin1String("outputDuration")).toDouble());
                              m_recording = d.value(QLatin1String("outputActive")).toBool();
                              m_recordPaused = m_recording && d.value(QLatin1String("outputPaused")).toBool();
                              m_recordStart = m_recording ? now() - duration : 0;
                              m_recordPausedElapsed = m_recordPaused ? duration : 0;
                              if (m_known) {
                                  Q_EMIT changed();
                              }
                          }
                          finishOne();
                      });
}

void LiveStatus::fetchScenes()
{
    const int generation = m_generation;
    m_client->request(QStringLiteral("GetSceneList"), {},
                      [this, generation](bool ok, const QJsonObject &d, const QString &) {
                          if (generation != m_generation) {
                              return;
                          }
                          if (ok) {
                              m_scenes = sceneNames(d.value(QLatin1String("scenes")).toArray());
                              if (m_known) {
                                  Q_EMIT changed();
                              }
                          }
                          finishOne();
                      });
}

void LiveStatus::onEvent(const QString &type, const QJsonObject &data)
{
    if (type == QLatin1String("StreamStateChanged")) {
        const bool active = data.value(QLatin1String("outputActive")).toBool();
        if (active != m_streaming) {
            setStreaming(active, 0);
        }
    } else if (type == QLatin1String("RecordStateChanged")) {
        const bool active = data.value(QLatin1String("outputActive")).toBool();
        const QString state = data.value(QLatin1String("outputState")).toString();
        if (state == kPaused || state == kResumed) {
            // Ask OBS for the duration so far rather than keeping a second clock.
            m_recordPaused = state == kPaused;
            Q_EMIT changed();
            fetchRecord();
        } else if (active != m_recording) {
            m_recording = active;
            m_recordPaused = false;
            m_recordStart = active ? now() : 0;
            m_recordPausedElapsed = 0;
            Q_EMIT changed();
        }
    } else if (type == QLatin1String("CurrentProgramSceneChanged")) {
        const QString name = data.value(QLatin1String("sceneName")).toString();
        if (name != m_programScene) {
            m_programScene = name;
            Q_EMIT changed();
            Q_EMIT programSceneChanged(name);
        }
    } else if (type == QLatin1String("SceneListChanged")) {
        m_scenes = sceneNames(data.value(QLatin1String("scenes")).toArray());
        Q_EMIT changed();
    } else if (type == QLatin1String("SceneCreated") || type == QLatin1String("SceneRemoved") ||
               type == QLatin1String("SceneNameChanged")) {
        if (type == QLatin1String("SceneNameChanged") &&
            data.value(QLatin1String("oldSceneName")).toString() == m_programScene) {
            m_programScene = data.value(QLatin1String("sceneName")).toString();
        }
        fetchScenes();
    }
}

QList<GoLiveProblem> goLiveProblems(const Scene &scene, const QSet<QString> &soloed,
                                    const QList<Recording> *recordings, const EffectiveMutes *mutes)
{
    ReadinessInput input;
    input.scene = scene;
    input.soloed = soloed;
    input.mutes =
        mutes ? *mutes : EffectiveMutes{scene.micBus() && scene.micBus()->muted, scene.masterStreamMuted};
    input.inspectRouting = false;
    input.legacyRecordings = recordings;
    return readinessWarnings(evaluateReadiness(input));
}

QString mappedScene(const QMap<QString, QString> &map, const QString &obsScene,
                    const QStringList &rostrumScenes)
{
    const QString target = map.value(obsScene);
    return rostrumScenes.contains(target) ? target : QString();
}

} // namespace rostrum::obs
