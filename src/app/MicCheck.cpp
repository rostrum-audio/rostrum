#include "app/MicCheck.h"

#include "app/AppController.h"
#include "engine/NodeSpecs.h"

#include <KLocalizedString>

#include <QJSEngine>
#include <QLoggingCategory>

namespace rostrum::app {

Q_LOGGING_CATEGORY(lcMicCheck, "rostrum.miccheck", QtInfoMsg)

MicCheck *MicCheck::s_instance = nullptr;

MicCheck::MicCheck(AppController *app, QObject *parent)
    : QObject(parent)
    , m_app(app)
    , m_check(app->pw())
{
    Q_ASSERT(!s_instance);
    s_instance = this;
    // From a hotkey or the command line the window may be hidden, so the steps show as feedback.
    connect(app, &AppController::micCheckRequested, this, [this] {
        if (m_check.phase() != pw::MicCheck::Phase::Idle) {
            stop();
        } else if (start()) {
            m_fromAction = true;
            Q_EMIT m_app->feedbackRequested(QStringLiteral("media-record"), i18n("Mic check: talk now"));
        }
    });
    connect(this, &MicCheck::resultChanged, this, [this] {
        if (!m_fromAction) {
            return;
        }
        m_fromAction = false;
        const QString level = i18nc("@info level in decibels full scale", "%1 dBFS", QString::number(m_peakDb, 'f', 1));
        const QString text = m_result == QLatin1String("good")    ? i18n("Mic check: good level (%1)", level)
                             : m_result == QLatin1String("quiet") ? i18n("Mic check: too quiet (%1)", level)
                             : m_result == QLatin1String("loud")  ? i18n("Mic check: too loud (%1)", level)
                                                                  : i18n("Mic check: nothing heard");
        Q_EMIT m_app->feedbackRequested(QStringLiteral("audio-input-microphone"), text);
    });
    connect(&m_check, &pw::MicCheck::phaseChanged, this, &MicCheck::phaseChanged);
    connect(&m_check, &pw::MicCheck::progressChanged, this, &MicCheck::progressChanged);
    connect(&m_check, &pw::MicCheck::recorded, this, [this] {
        m_peakDb = pw::miccheck::toDb(m_check.peak());
        switch (pw::miccheck::verdict(m_peakDb)) {
        case pw::miccheck::Verdict::Silent:
            m_result = QStringLiteral("silent");
            break;
        case pw::miccheck::Verdict::Quiet:
            m_result = QStringLiteral("quiet");
            break;
        case pw::miccheck::Verdict::Good:
            m_result = QStringLiteral("good");
            break;
        case pw::miccheck::Verdict::Loud:
            m_result = QStringLiteral("loud");
            break;
        }
        qCInfo(lcMicCheck) << "recorded, peak" << m_peakDb << "dBFS:" << m_result;
        Q_EMIT resultChanged();
    });
}

MicCheck::~MicCheck()
{
    s_instance = nullptr;
}

MicCheck *MicCheck::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

QString MicCheck::phase() const
{
    switch (m_check.phase()) {
    case pw::MicCheck::Phase::Recording:
        return QStringLiteral("recording");
    case pw::MicCheck::Phase::Playing:
        return QStringLiteral("playing");
    case pw::MicCheck::Phase::Idle:
        break;
    }
    return QStringLiteral("idle");
}

bool MicCheck::start()
{
    engine::Engine *engine = m_app->engine();
    QString problem;
    if (!m_app->connected() || !engine->hasMic()) {
        problem = i18n("No mic to check. Choose one on the Devices page.");
    } else if (engine->effectiveMicMuted()) {
        problem = i18n("Your mic is muted. Unmute it to check how you sound.");
    } else if (engine->resolvedSinkName().isEmpty()) {
        problem = i18n("No headphones to play the check back on. Choose them on the Devices page.");
    }
    if (problem.isEmpty() &&
        m_check.start(QString::fromLatin1(engine::kMicNode), engine->resolvedSinkName())) {
        qCInfo(lcMicCheck) << "recording" << engine::kMicNode << "to play back on" << engine->resolvedSinkName();
        return true;
    }
    Q_EMIT m_app->toast(problem.isEmpty() ? i18n("Could not start the mic check.") : problem);
    return false;
}

void MicCheck::stop()
{
    m_check.stop();
}

} // namespace rostrum::app
