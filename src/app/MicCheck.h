#pragma once

#include "pw/MicCheck.h"

#include <QObject>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

namespace rostrum::app {

class AppController;

// "How do I sound?": records a few seconds of the stream mic, after gain and mic filters, and
// plays them back in the headphones only, then says whether the level looks right.
class MicCheck : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // "idle", "recording" or "playing"
    Q_PROPERTY(QString phase READ phase NOTIFY phaseChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(int seconds READ seconds CONSTANT)
    // After a check: "silent", "quiet", "good" or "loud"; empty before the first one.
    Q_PROPERTY(QString result READ result NOTIFY resultChanged)
    Q_PROPERTY(double peakDb READ peakDb NOTIFY resultChanged)

public:
    MicCheck(AppController *app, QObject *parent);
    ~MicCheck() override;

    static MicCheck *create(QQmlEngine *, QJSEngine *);

    QString phase() const;
    double progress() const { return m_check.progress(); }
    int seconds() const { return pw::miccheck::kSeconds; }
    QString result() const { return m_result; }
    double peakDb() const { return m_peakDb; }

    // Says why in a toast when it cannot start: no mic, the mic is muted or no headphones.
    Q_INVOKABLE bool start();
    Q_INVOKABLE void stop();

Q_SIGNALS:
    void phaseChanged();
    void progressChanged();
    void resultChanged();

private:
    static MicCheck *s_instance;
    AppController *m_app = nullptr;
    pw::MicCheck m_check;
    QString m_result;
    double m_peakDb = 0.0;
    bool m_fromAction = false;
};

} // namespace rostrum::app
