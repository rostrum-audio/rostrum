#pragma once

#include "core/UpdateFeed.h"

#include <QCryptographicHash>
#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

class QNetworkReply;
class QQmlEngine;
class QJSEngine;

namespace rostrum::app {

class AppController;

// Asks the release feed once a day whether a newer Rostrum exists. An AppImage copy downloads the
// new file, checks its SHA-256 against the feed and swaps it in for the next start. Other copies
// are updated by whatever installed them, so Rostrum only says that a new version is out.
class Updater : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(Updates)
    QML_SINGLETON

    Q_PROPERTY(bool checkEnabled READ checkEnabled WRITE setCheckEnabled NOTIFY changed)
    Q_PROPERTY(bool autoInstall READ autoInstall WRITE setAutoInstall NOTIFY changed)
    // "source", "package", "flatpak" or "appimage"
    Q_PROPERTY(QString installKind READ installKindName CONSTANT)
    Q_PROPERTY(bool canCheck READ canCheck CONSTANT)
    Q_PROPERTY(bool canInstall READ canInstall CONSTANT)
    // "idle", "checking", "upToDate", "available", "downloading", "ready" or "error"
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY changed)
    Q_PROPERTY(QString releaseNotes READ releaseNotes NOTIFY changed)
    Q_PROPERTY(QUrl releaseUrl READ releaseUrl NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QString lastChecked READ lastChecked NOTIFY changed)
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
    // A new version the user has not skipped, or an installed one waiting for a restart.
    Q_PROPERTY(bool showBanner READ showBanner NOTIFY changed)
    Q_PROPERTY(QString feedHost READ feedHost CONSTANT)

public:
    Updater(AppController *app, QObject *parent);
    ~Updater() override;

    static Updater *create(QQmlEngine *, QJSEngine *);
    static updates::InstallKind installKind();

    // Call once settings are loaded.
    void start();

    bool checkEnabled() const;
    void setCheckEnabled(bool on);
    bool autoInstall() const;
    void setAutoInstall(bool on);
    QString installKindName() const { return updates::kindName(m_kind); }
    bool canCheck() const { return m_kind != updates::InstallKind::Flatpak; }
    bool canInstall() const { return m_canInstall; }
    QString state() const { return m_state; }
    QString latestVersion() const { return m_release.version; }
    QString releaseNotes() const { return m_release.notes; }
    QUrl releaseUrl() const { return m_release.page; }
    double progress() const { return m_progress; }
    QString lastChecked() const;
    QString errorText() const { return m_error; }
    bool showBanner() const;
    QString feedHost() const { return m_feed.host(); }

    Q_INVOKABLE void checkNow();
    Q_INVOKABLE void install();
    Q_INVOKABLE void skipThisVersion();
    // Starts the installed AppImage and quits this instance.
    Q_INVOKABLE void restart();

Q_SIGNALS:
    void changed();
    void restartRequested(const QString &program);

private:
    void maybeCheck();
    void setState(const QString &state, const QString &error = QString());
    void finishCheck(QNetworkReply *reply, bool manual);
    void finishDownload();
    void failDownload(const QString &error);

    static Updater *s_instance;
    AppController *m_app = nullptr;
    QNetworkAccessManager m_net;
    QUrl m_feed;
    updates::InstallKind m_kind = updates::InstallKind::Source;
    QString m_appImage;
    bool m_canInstall = false;
    bool m_networkAllowed = false;
    bool m_checkSoon = false;
    QTimer m_timer;
    QString m_state = QStringLiteral("idle");
    QString m_error;
    updates::Release m_release;
    double m_progress = 0.0;
    QPointer<QNetworkReply> m_reply;
    QFile m_part;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
};

} // namespace rostrum::app
