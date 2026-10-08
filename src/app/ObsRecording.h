#pragma once
#include "obs/RecordingLive.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

namespace rostrum::app {
class AppController;
class Obs;

class ObsRecording : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    Q_PROPERTY(QVariantList choices READ choices NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString collection READ collection NOTIFY changed)
    Q_PROPERTY(QStringList previewItems READ previewItems NOTIFY changed)
    Q_PROPERTY(bool canPreview READ canPreview NOTIFY changed)
    Q_PROPERTY(bool canApply READ canApply NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
public:
    ObsRecording(AppController *app, Obs *obs, obs::Client *client);
    QVariantList rows() const;
    QVariantList choices() const;
    QString status() const;
    QString collection() const { return m_snapshot.collection; }
    QStringList previewItems() const;
    bool canPreview() const;
    bool canApply() const;
    bool canUndo() const;
    QSet<QString> intendedDevices() const;
    quint32 assignedMicTracks() const;
    QVariantMap readiness() const;
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void choose(int track, const QString &busId);
    Q_INVOKABLE void preview();
    Q_INVOKABLE void apply();
    Q_INVOKABLE void undo();
Q_SIGNALS:
    void changed();
    void previewReady();

private:
    void acceptSnapshot(const obs::RecordingSnapshot &snapshot);
    void setBusy(bool busy);
    void saveAssignments(const obs::RecordingAssignments &assignments, bool present = true);
    QString undoPath() const;
    QJsonObject readUndo() const;
    bool writeUndo(const QJsonObject &record) const;
    void runUndo(QJsonObject record, const obs::RecordingSnapshot &scope, bool rollback);
    void notify(const QString &text);
    AppController *m_app;
    Obs *m_obs;
    obs::Client *m_client;
    obs::RecordingSnapshot m_snapshot;
    obs::RecordingAssignments m_draft;
    obs::RecordingPlan m_preview;
    QByteArray m_previewFingerprint;
    QString m_scope, m_error;
    bool m_fetching = false, m_havePreview = false, m_draftEdited = false;
    int m_generation = 0;
    QElapsedTimer m_age;
    QTimer m_refetch, m_snapshotRefresh;
};
} // namespace rostrum::app
