#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

namespace rostrum::app {

class AppController;

// Crash files from earlier runs and what happens to them: sent in the background, offered to the
// user after a crash, or deleted. Only crash::buildReport() output leaves the computer.
class CrashReports : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // "send", "ask" or "never"
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY changed)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY changed)
    // Ask now: there are reports, the user wants to be asked, and setup is finished.
    Q_PROPERTY(bool askNow READ askNow NOTIFY changed)
    Q_PROPERTY(QString lastCrashDate READ lastCrashDate NOTIFY changed)
    Q_PROPERTY(QString destination READ destination CONSTANT)

public:
    CrashReports(AppController *app, QObject *parent);
    ~CrashReports() override;

    static CrashReports *create(QQmlEngine *, QJSEngine *);

    // Call once settings are loaded.
    void start();

    QString mode() const;
    void setMode(const QString &mode);
    int pendingCount() const { return int(m_pending.size()); }
    bool askNow() const;
    QString lastCrashDate() const;
    QString destination() const { return m_endpoint.host(); }

    // Exactly what would be sent for the pending reports, as indented JSON.
    Q_INVOKABLE QString pendingReportText() const;
    // A report built from this system with made-up frames, for the privacy explanation.
    Q_INVOKABLE QString exampleReportText() const;
    Q_INVOKABLE void sendPending();
    Q_INVOKABLE void discardPending();

Q_SIGNALS:
    void changed();

private:
    void scan();
    void upload(const QString &path);
    void updateSystemContext();

    static CrashReports *s_instance;
    AppController *m_app = nullptr;
    QNetworkAccessManager m_net;
    QUrl m_endpoint;
    bool m_networkAllowed = false;
    QStringList m_pending; // crash file paths, oldest first
    QStringList m_inFlight;
};

} // namespace rostrum::app
