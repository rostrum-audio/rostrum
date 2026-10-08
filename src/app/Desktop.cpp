#include "app/Desktop.h"

#include "app/AppController.h"
#include "app/Cli.h"
#include "app/Hotkeys.h"
#include "app/Preferences.h"
#include "app/Tray.h"
#include "core/Autostart.h"
#include "core/Paths.h"

#include <KGlobalAccel>
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


// The app id before Rostrum had its own domain. Its global shortcuts would hold the same keys
// and make every new binding look taken, so they go before Hotkeys registers anything.
const QString kOldAppId = QStringLiteral("io.github.rostrum_audio.Rostrum");

void dropOldShortcuts()
{
    const auto *bus = QDBusConnection::sessionBus().interface();
    if (bus && bus->isServiceRegistered(QStringLiteral("org.kde.kglobalaccel")) &&
        KGlobalAccel::cleanComponent(kOldAppId)) {
        qCInfo(lcDesktop) << "Removed global shortcuts left under" << kOldAppId;
    }
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

    if (m_enabled) {
        dropOldShortcuts();
    }
    m_hotkeys = new Hotkeys(this);
    connect(m_hotkeys, &Hotkeys::triggered, m_app,
            [this](const QString &id) { m_app->runAction(id, AppController::Origin::Hotkey); });
    connect(m_hotkeys, &Hotkeys::released, m_app,
            [this](const QString &id) { m_app->releaseAction(id, AppController::Origin::Hotkey); });
    connect(m_hotkeys, &Hotkeys::changedExternally, this, [this](const QString &id, const QString &portable) {
        m_app->settings().hotkeys.insert(id, portable);
        m_app->saveSettingsSoon();
        Q_EMIT m_app->settingsChanged();
    });
    connect(m_app, &AppController::settingsChanged, this, &Desktop::applyHotkeys);
    connect(m_app, &AppController::actionsChanged, this, &Desktop::applyHotkeys);
    connect(m_app, &AppController::headphonesLost, this, &Desktop::notifyHeadphonesLost);
    connect(m_app, &AppController::feedbackRequested, this, &Desktop::showFeedback);
    connect(m_app, &AppController::micLost, this, &Desktop::notifyMicLost);
    connect(m_app, &AppController::micFiltersTripped, this, &Desktop::notifyMicFiltersTripped);
    applyHotkeys();

    if (m_enabled) {
        // Plasma can restart its panel; pick the tray up again when a host comes back.
        QDBusConnection::sessionBus().connect(kWatcherService, kWatcherPath, kWatcherService,
                                              QStringLiteral("StatusNotifierHostRegistered"), this,
                                              SLOT(checkTray()));
        checkTray();
    }
    const QStringList args = QCoreApplication::arguments();
    m_startHidden = m_app->settings().wizardDone &&
                    (args.contains(QStringLiteral("--") + QLatin1String(cli::kStartHidden)) ||
                     (m_app->settings().startInTray && args.contains(QStringLiteral("--autostart"))));

    const QString oldAutostart = QFileInfo(paths::autostartFile()).dir().filePath(kOldAppId + QStringLiteral(".desktop"));
    if (m_enabled && QFileInfo::exists(oldAutostart) && QFile::remove(oldAutostart)) {
        qCInfo(lcDesktop) << "Moved the autostart entry to" << paths::autostartFile();
        setLaunchAtLogin(true);
    }

    // The autostart file is the truth: the user may have removed it in System Settings.
    const bool exists = QFileInfo::exists(paths::autostartFile());
    if (m_app->settings().launchAtLogin != exists) {
        m_app->settings().launchAtLogin = exists;
        m_app->saveSettingsSoon();
    }
    if (m_enabled && exists) {
        ensureAutostartCurrent();
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
    if (window) {
        window->installEventFilter(this);
    }
}

bool Desktop::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_window && m_tray && m_app->settings().minimizeToTray && m_window->isVisible()) {
        // X11 reports minimizing as a window state. Wayland does not: the compositor only stops
        // showing the window, as it also does on a switch to another virtual desktop. Minimizing
        // happens to the active window, while a desktop switch takes focus away first.
        const bool minimized =
            (event->type() == QEvent::WindowStateChange && (m_window->windowStates() & Qt::WindowMinimized)) ||
            (event->type() == QEvent::Expose && !m_window->isExposed() && m_window->isActive() &&
             QGuiApplication::platformName() == QLatin1String("wayland"));
        if (minimized) {
            QMetaObject::invokeMethod(this, &Desktop::hideMinimized, Qt::QueuedConnection);
        }
    }
    return QObject::eventFilter(watched, event);
}

void Desktop::hideMinimized()
{
    if (!m_window || !m_window->isVisible()) {
        return;
    }
    qCDebug(lcDesktop) << "minimized: hiding to the tray";
    m_app->saveSettingsNow();
    // Shown again from the tray, the window comes back as it was before, not minimized.
    m_window->setWindowStates(m_window->windowStates() & ~Qt::WindowMinimized);
    m_window->hide();
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

bool Desktop::writeAutostartFile()
{
    const QString launcher = autostart::launcherPath(QCoreApplication::applicationFilePath(),
                                                     qEnvironmentVariable("APPIMAGE"));
    if (launcher.isEmpty()) {
        qCWarning(lcDesktop) << "Cannot determine stable launcher for autostart";
        return false;
    }
    const QString path = paths::autostartFile();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        Q_EMIT m_app->toast(i18n("Could not write %1", path));
        return false;
    }
    const QString exec = autostart::execLine(launcher);
    f.write(autostart::entry(exec, i18n("A stream mix console for Linux"), launcher).toUtf8());
    return true;
}

void Desktop::ensureAutostartCurrent()
{
    const QString currentLauncher = autostart::launcherPath(QCoreApplication::applicationFilePath(),
                                                            qEnvironmentVariable("APPIMAGE"));
    if (currentLauncher.isEmpty()) {
        return;
    }
    const QString path = paths::autostartFile();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    const QString existing = QString::fromUtf8(f.readAll());
    f.close();
    const QString expectedExec = autostart::execLine(currentLauncher);
    if (!existing.contains(QStringLiteral("Exec=") + expectedExec) ||
        !existing.contains(QStringLiteral("TryExec=") + currentLauncher) ||
        !existing.contains(QStringLiteral("StartupWMClass=rostrum"))) {
        qCInfo(lcDesktop) << "Updating autostart entry for binary:" << currentLauncher;
        writeAutostartFile();
    }
}

void Desktop::setLaunchAtLogin(bool on)
{
    const QString path = paths::autostartFile();
    if (on) {
        if (!writeAutostartFile()) {
            Q_EMIT changed();
            return;
        }
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

bool Desktop::closeToTray() const
{
    return m_app->settings().closeToTray;
}

void Desktop::setCloseToTray(bool on)
{
    if (m_app->settings().closeToTray == on) {
        return;
    }
    m_app->settings().closeToTray = on;
    m_app->saveSettingsSoon();
    Q_EMIT changed();
}

bool Desktop::minimizeToTray() const
{
    return m_app->settings().minimizeToTray;
}

void Desktop::setMinimizeToTray(bool on)
{
    if (m_app->settings().minimizeToTray == on) {
        return;
    }
    m_app->settings().minimizeToTray = on;
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
    for (const QString &id : m_app->actionIds()) {
        bindings.insert(id, keys.value(id, actions::defaultShortcut(id)));
        labels.insert(id, Preferences::actionLabel(id));
    }
    m_hotkeys->apply(bindings, labels);
}

void Desktop::showFeedback(const QString &iconName, const QString &text)
{
    if (!m_enabled || !m_app->settings().osdFeedback ||
        (m_window && m_window->isVisible() && m_window->isActive())) {
        return;
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.kde.plasmashell"), QStringLiteral("/org/kde/osdService"),
        QStringLiteral("org.kde.osdService"), QStringLiteral("showText"));
    msg << iconName << text;
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg, 2000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, iconName, text](QDBusPendingCallWatcher *w) {
                if (QDBusPendingReply<> reply = *w; reply.isError()) {
                    notifyFeedback(iconName, text);
                }
                w->deleteLater();
            });
}

// Without Plasma's OSD: a short transient notification that replaces the previous one.
void Desktop::notifyFeedback(const QString &iconName, const QString &text)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("Notify"));
    msg << i18n("Rostrum") << m_feedbackNotificationId << iconName << text << QString() << QStringList()
        << QVariantMap{{QStringLiteral("desktop-entry"), QStringLiteral(ROSTRUM_APP_ID)},
                       {QStringLiteral("transient"), true},
                       {QStringLiteral("urgency"), QVariant::fromValue<uchar>(0)}}
        << 2000;
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        if (QDBusPendingReply<uint> reply = *w; reply.isValid()) {
            m_feedbackNotificationId = reply.value();
        }
        w->deleteLater();
    });
}

void Desktop::notifyHeadphonesLost(const QString &description)
{
    notify(i18n("Headphones disconnected, scene held."),
           i18n("%1 went away. Plug it back in and routes come back on their own.", description), 1,
           &m_notificationId);
}

void Desktop::notifyGoLive(const QString &body)
{
    notify(i18n("Check your stream audio"), body, 2, &m_goLiveNotificationId);
}

void Desktop::notifyMicLost(const QString &description)
{
    if (m_app->engine()->micSilenced()) {
        notify(i18n("Mic disconnected, stream mic silent."),
               i18n("%1 went away. Your stream mic stays silent until it comes back.", description), 1,
               &m_micNotificationId);
    } else {
        notify(i18n("Mic disconnected."),
               i18n("%1 went away. Using %2 until it comes back.", description, m_app->micText()), 1,
               &m_micNotificationId);
    }
}

void Desktop::notifyMicFiltersTripped()
{
    notify(i18n("Mic filters turned off"),
           i18n("PipeWire stopped twice right after they started. Your mic works without them; you can "
                "turn them on again on the Mic Filters page."),
           1, &m_micFiltersNotificationId);
}

void Desktop::notify(const QString &summary, const QString &body, uchar urgency, uint *id)
{
    if (!m_enabled) {
        return;
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("Notify"));
    msg << i18n("Rostrum") << *id << QStringLiteral(ROSTRUM_APP_ID) << summary << body << QStringList()
        << QVariantMap{{QStringLiteral("desktop-entry"), QStringLiteral(ROSTRUM_APP_ID)},
                       {QStringLiteral("urgency"), QVariant::fromValue<uchar>(urgency)}}
        << -1;
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [id](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<uint> reply = *w;
        if (reply.isValid()) {
            *id = reply.value();
        } else {
            qCWarning(lcDesktop) << "Notification failed:" << reply.error().message();
        }
        w->deleteLater();
    });
}

} // namespace rostrum::app
