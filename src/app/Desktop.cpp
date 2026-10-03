#include "app/Desktop.h"

#include "app/AppController.h"
#include "app/Hotkeys.h"
#include "app/Preferences.h"
#include "app/Tray.h"
#include "core/Autostart.h"
#include "core/Paths.h"

#include <KLocalizedString>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJSEngine>
#include <QLoggingCategory>
#include <QQuickWindow>
#include <QStandardPaths>

Q_LOGGING_CATEGORY(lcDesktop, "rostrum.desktop", QtInfoMsg)

namespace rostrum::app {

namespace {

const QString kWatcherService = QStringLiteral("org.kde.StatusNotifierWatcher");
const QString kWatcherPath = QStringLiteral("/StatusNotifierWatcher");

bool statusNotifierHostRegistered()
{
    QDBusMessage msg = QDBusMessage::createMethodCall(kWatcherService, kWatcherPath,
                                                      QStringLiteral("org.freedesktop.DBus.Properties"),
                                                      QStringLiteral("Get"));
    msg << kWatcherService << QStringLiteral("IsStatusNotifierHostRegistered");
    const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 2000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return false;
    }
    return reply.arguments().constFirst().value<QDBusVariant>().variant().toBool();
}

QString autostartExec()
{
    const QString self = QCoreApplication::applicationFilePath();
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("rostrum"));
    const QString program =
        (!onPath.isEmpty() && QFileInfo(onPath).canonicalFilePath() == QFileInfo(self).canonicalFilePath())
            ? QStringLiteral("rostrum")
            : autostart::execQuote(self);
    return program + QStringLiteral(" --autostart");
}

} // namespace

Desktop *Desktop::s_instance = nullptr;

Desktop::Desktop(AppController *app, QObject *parent) : QObject(parent), m_app(app)
{
    Q_ASSERT(!s_instance);
    s_instance = this;

    // Screenshot and test runs must leave the user's tray, shortcuts and notifications alone.
    m_enabled = QGuiApplication::platformName() != QLatin1String("offscreen") &&
                qEnvironmentVariableIsEmpty("ROSTRUM_SCREENSHOT");

    m_hotkeys = new Hotkeys(this);
    connect(m_hotkeys, &Hotkeys::triggered, m_app, &AppController::triggerAction);
    connect(m_hotkeys, &Hotkeys::changedExternally, this, [this](const QString &id, const QString &portable) {
        m_app->settings().hotkeys.insert(id, portable);
        m_app->saveSettingsSoon();
        Q_EMIT m_app->settingsChanged();
    });
    connect(m_app, &AppController::settingsChanged, this, &Desktop::applyHotkeys);
    connect(m_app, &AppController::headphonesLost, this, &Desktop::notifyHeadphonesLost);
    applyHotkeys();

    if (m_enabled) {
        // Plasma can restart its panel; pick the tray up again when a host comes back.
        QDBusConnection::sessionBus().connect(kWatcherService, kWatcherPath, kWatcherService,
                                              QStringLiteral("StatusNotifierHostRegistered"), this,
                                              SLOT(checkTray()));
        checkTray();
    }
    m_startHidden = m_tray && m_app->settings().startInTray && m_app->settings().wizardDone &&
                    QCoreApplication::arguments().contains(QStringLiteral("--autostart"));

    // The autostart file is the truth: the user may have removed it in System Settings.
    const bool exists = QFileInfo::exists(paths::autostartFile());
    if (m_app->settings().launchAtLogin != exists) {
        m_app->settings().launchAtLogin = exists;
        m_app->saveSettingsSoon();
    }
}

Desktop::~Desktop()
{
    s_instance = nullptr;
}

Desktop *Desktop::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

void Desktop::setWindow(QQuickWindow *window)
{
    m_window = window;
    if (m_tray) {
        m_tray->setWindow(window);
    }
}

void Desktop::checkTray()
{
    if (m_tray || !m_enabled || !statusNotifierHostRegistered()) {
        return;
    }
    m_tray = new Tray(m_app, this);
    if (m_window) {
        m_tray->setWindow(m_window);
    }
    qCInfo(lcDesktop) << "Tray icon added";
    Q_EMIT trayChanged();
}

bool Desktop::launchAtLogin() const
{
    return m_app->settings().launchAtLogin;
}

void Desktop::setLaunchAtLogin(bool on)
{
    const QString path = paths::autostartFile();
    if (on) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            Q_EMIT m_app->toast(i18n("Could not write %1", path));
            Q_EMIT changed();
            return;
        }
        f.write(autostart::entry(autostartExec(), i18n("A stream mix console for Linux")).toUtf8());
    } else if (QFileInfo::exists(path) && !QFile::remove(path)) {
        Q_EMIT m_app->toast(i18n("Could not remove %1", path));
        Q_EMIT changed();
        return;
    }
    if (m_app->settings().launchAtLogin != on) {
        m_app->settings().launchAtLogin = on;
        m_app->saveSettingsSoon();
    }
    Q_EMIT changed();
}

bool Desktop::startInTray() const
{
    return m_app->settings().startInTray;
}

void Desktop::setStartInTray(bool on)
{
    if (m_app->settings().startInTray == on) {
        return;
    }
    m_app->settings().startInTray = on;
    m_app->saveSettingsSoon();
    Q_EMIT changed();
}

QString Desktop::shortcutBackend() const
{
    switch (m_hotkeys->backend()) {
    case Hotkeys::Backend::KGlobalAccel:
        return QStringLiteral("kglobalaccel");
    case Hotkeys::Backend::Portal:
        return QStringLiteral("portal");
    case Hotkeys::Backend::None:
        break;
    }
    return QStringLiteral("none");
}

void Desktop::applyHotkeys()
{
    QMap<QString, QString> bindings;
    QMap<QString, QString> labels;
    const auto &keys = m_app->settings().hotkeys;
    for (const QString &id : actions::all()) {
        bindings.insert(id, keys.value(id, actions::defaultShortcut(id)));
        labels.insert(id, Preferences::actionLabel(id));
    }
    m_hotkeys->apply(bindings, labels);
}

void Desktop::notifyHeadphonesLost(const QString &description)
{
    if (!m_enabled) {
        return;
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("Notify"));
    msg << i18n("Rostrum") << m_notificationId << QStringLiteral(ROSTRUM_APP_ID)
        << i18n("Headphones disconnected, scene held.")
        << i18n("%1 went away. Plug it back in and routes come back on their own.", description)
        << QStringList()
        << QVariantMap{{QStringLiteral("desktop-entry"), QStringLiteral(ROSTRUM_APP_ID)},
                       {QStringLiteral("urgency"), QVariant::fromValue<uchar>(1)}}
        << -1;
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<uint> reply = *w;
        if (reply.isValid()) {
            m_notificationId = reply.value();
        } else {
            qCWarning(lcDesktop) << "Notification failed:" << reply.error().message();
        }
        w->deleteLater();
    });
}

} // namespace rostrum::app
