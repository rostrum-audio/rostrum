#include "app/DBusControl.h"

#include "app/AppController.h"
#include "app/Preferences.h"
#include "core/Remote.h"

#include <KLocalizedString>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusServiceWatcher>
#include <QLoggingCategory>
#include <cmath>

Q_LOGGING_CATEGORY(lcDBus, "rostrum.dbus", QtInfoMsg)

namespace rostrum::app {

QDBusArgument &operator<<(QDBusArgument &arg, const BusInfo &b)
{
    arg.beginStructure();
    arg << b.id << b.name << b.position << b.muted;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, BusInfo &b)
{
    arg.beginStructure();
    arg >> b.id >> b.name >> b.position >> b.muted;
    arg.endStructure();
    return arg;
}

QDBusArgument &operator<<(QDBusArgument &arg, const ActionInfo &a)
{
    arg.beginStructure();
    arg << a.id << a.label;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, ActionInfo &a)
{
    arg.beginStructure();
    arg >> a.id >> a.label;
    arg.endStructure();
    return arg;
}

namespace {
DBusControl::Error error(const char *name, const QString &message)
{
    return {QString::fromLatin1(name), message};
}
} // namespace

DBusControl::DBusControl(AppController *app, QObject *parent) : QObject(parent), m_app(app), m_bus(QString())
{
    qDBusRegisterMetaType<BusInfo>();
    qDBusRegisterMetaType<BusInfoList>();
    qDBusRegisterMetaType<ActionInfo>();
    qDBusRegisterMetaType<ActionInfoList>();
    for (auto sig :
         {&AppController::levelsChanged, &AppController::scenesChanged, &AppController::statusChanged}) {
        connect(m_app, sig, this, &DBusControl::publishChanges);
    }
}

DBusControl::~DBusControl() = default;

bool DBusControl::registerOn(QDBusConnection bus)
{
    m_bus = bus;
    if (!m_bus.registerObject(QString::fromLatin1(remote::kPath), this,
                              QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties)) {
        qCWarning(lcDBus) << "Could not register" << remote::kPath << m_bus.lastError().message();
        return false;
    }
    // A client that pressed a hold and went away must not leave the mic live.
    m_watcher = new QDBusServiceWatcher(this);
    m_watcher->setConnection(m_bus);
    m_watcher->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered, this, &DBusControl::releaseHoldsOf);
    publishChanges();
    return true;
}

bool DBusControl::micMuted() const
{
    return m_app->engine()->effectiveMicMuted();
}
bool DBusControl::streamMuted() const
{
    return m_app->engine()->effectiveStreamMuted();
}
bool DBusControl::panic() const
{
    return m_app->engine()->panic();
}
QString DBusControl::currentScene() const
{
    return m_app->currentScene();
}
bool DBusControl::connected() const
{
    return m_app->connected();
}

// The PropertiesChanged signal for the properties above, sent only when one changed.
void DBusControl::publishChanges()
{
    const QVariantMap now{{QStringLiteral("MicMuted"), micMuted()},
                          {QStringLiteral("StreamMuted"), streamMuted()},
                          {QStringLiteral("Panic"), panic()},
                          {QStringLiteral("CurrentScene"), currentScene()},
                          {QStringLiteral("Connected"), connected()}};
    QVariantMap changed;
    for (auto it = now.cbegin(); it != now.cend(); ++it) {
        if (m_published.value(it.key()) != it.value()) {
            changed.insert(it.key(), it.value());
        }
    }
    const bool first = m_published.isEmpty();
    m_published = now;
    if (changed.isEmpty() || first || !m_bus.isConnected()) {
        return;
    }
    QDBusMessage signal = QDBusMessage::createSignal(QString::fromLatin1(remote::kPath),
                                                     QStringLiteral("org.freedesktop.DBus.Properties"),
                                                     QStringLiteral("PropertiesChanged"));
    signal << QString::fromLatin1(remote::kInterface) << changed << QStringList();
    m_bus.send(signal);
}

void DBusControl::reply(const Error &e)
{
    if (e && calledFromDBus()) {
        sendErrorReply(e.name, e.message);
    }
}

// A bus id, or failing that a bus name in any case. Also "stream" and "phones" for the masters.
QString DBusControl::resolveBus(const QString &text) const
{
    const QString t = text.trimmed();
    for (const char *master : {remote::kStreamId, remote::kPhonesId}) {
        if (t.compare(QLatin1String(master), Qt::CaseInsensitive) == 0) {
            return QString::fromLatin1(master);
        }
    }
    const Scene &scene = m_app->engine()->scene();
    for (const auto &b : scene.buses) {
        if (b.id.compare(t, Qt::CaseInsensitive) == 0) {
            return b.id;
        }
    }
    for (const auto &b : scene.buses) {
        if (b.name.compare(t, Qt::CaseInsensitive) == 0) {
            return b.id;
        }
    }
    return {};
}

DBusControl::Error DBusControl::checkKnown(const QString &id) const
{
    if (!actions::isKnown(id)) {
        return error(remote::kErrorUnknownAction, i18n("There is no action “%1”.", id));
    }
    return {};
}

DBusControl::Error DBusControl::triggerAction(const QString &id)
{
    if (const Error e = checkKnown(id)) {
        return e;
    }
    if (actions::isHold(id)) {
        return error(remote::kErrorHoldAction,
                     i18n("“%1” acts while held. Use PressAction and ReleaseAction instead.", id));
    }
    using Result = engine::Controls::Result;
    switch (m_app->runAction(id, AppController::Origin::Remote).result) {
    case Result::NoSuchScene:
        return error(remote::kErrorNoSuchScene, i18n("There is no scene %1.", actions::sceneSlot(id)));
    case Result::NoSuchBus:
        return error(remote::kErrorNoSuchBus,
                     i18n("The live scene has no bus “%1”.", actions::busOfAction(id)));
    case Result::UnknownAction:
        return error(remote::kErrorUnknownAction, i18n("There is no action “%1”.", id));
    case Result::Done:
    case Result::SwitchScene:
        break;
    }
    return {};
}

DBusControl::Error DBusControl::switchScene(const QString &name)
{
    const QStringList names = m_app->sceneNames();
    QString target;
    for (const QString &n : names) {
        if (n == name || (target.isEmpty() && n.compare(name, Qt::CaseInsensitive) == 0)) {
            target = n;
        }
    }
    if (target.isEmpty()) {
        return error(remote::kErrorNoSuchScene, i18n("There is no scene called “%1”.", name));
    }
    if (target != m_app->currentScene()) {
        const auto before = m_app->snapshot();
        m_app->switchScene(target);
        m_app->reportChange(before);
    }
    return {};
}

DBusControl::Error DBusControl::setBusVolume(const QString &busText, double position)
{
    const QString busId = resolveBus(busText);
    if (busId.isEmpty()) {
        return error(remote::kErrorNoSuchBus, i18n("The live scene has no bus “%1”.", busText));
    }
    const double max = remote::maxPosition(busId);
    if (!std::isfinite(position) || position < 0.0 || position > max) {
        return error(remote::kErrorInvalidValue,
                     i18n("The volume for “%1” must be between 0 and %2.", busText, QString::number(max)));
    }
    auto *engine = m_app->engine();
    if (busId == QLatin1String(remote::kStreamId)) {
        engine->setMasterStream(position);
    } else if (busId == QLatin1String(remote::kPhonesId)) {
        engine->setMasterPhones(position);
    } else {
        engine->setBusVolume(busId, position);
    }
    return {};
}

DBusControl::Error DBusControl::setBusMuted(const QString &busText, bool muted)
{
    const QString busId = resolveBus(busText);
    if (busId.isEmpty()) {
        return error(remote::kErrorNoSuchBus, i18n("The live scene has no bus “%1”.", busText));
    }
    const auto before = m_app->snapshot();
    auto *engine = m_app->engine();
    if (busId == QLatin1String(remote::kStreamId)) {
        engine->setMasterStreamMuted(muted);
    } else if (busId == QLatin1String(remote::kPhonesId)) {
        engine->setMasterPhonesMuted(muted);
    } else {
        engine->setBusMuted(busId, muted);
    }
    m_app->reportChange(before);
    return {};
}

DBusControl::Error DBusControl::toggleBusMuted(const QString &busText)
{
    const QString busId = resolveBus(busText);
    if (busId.isEmpty()) {
        return error(remote::kErrorNoSuchBus, i18n("The live scene has no bus “%1”.", busText));
    }
    const auto *engine = m_app->engine();
    const Scene &s = engine->scene();
    const bool muted = busId == QLatin1String(remote::kStreamId)   ? engine->effectiveStreamMuted()
                       : busId == QLatin1String(remote::kPhonesId) ? s.masterPhonesMuted
                       : busId == QLatin1String(kMicBusId)         ? engine->effectiveMicMuted()
                                                                   : s.bus(busId)->muted;
    return setBusMuted(busId, !muted);
}

void DBusControl::setMicMuted(bool muted)
{
    const auto before = m_app->snapshot();
    m_app->setMicMuted(muted);
    m_app->reportChange(before);
}

void DBusControl::toggleMicMute()
{
    setMicMuted(!micMuted());
}

BusInfoList DBusControl::busesOf(const Scene &s)
{
    BusInfoList out{
        {QString::fromLatin1(remote::kStreamId), i18nc("@label master fader", "Stream"), s.masterStream,
         s.masterStreamMuted},
        {QString::fromLatin1(remote::kPhonesId), i18nc("@label master fader", "Headphones"), s.masterPhones,
         s.masterPhonesMuted},
    };
    if (const Bus *mic = s.micBus()) {
        out.append({mic->id, mic->name, mic->volume, mic->muted});
    }
    for (const auto &b : s.buses) {
        if (!b.isInput()) {
            out.append({b.id, b.name, b.volume, b.muted});
        }
    }
    return out;
}

BusInfoList DBusControl::buses() const
{
    const auto *engine = m_app->engine();
    BusInfoList out = busesOf(engine->scene());
    for (auto &b : out) {
        if (b.id == QLatin1String(remote::kStreamId)) {
            b.muted = engine->effectiveStreamMuted();
        } else if (b.id == QLatin1String(kMicBusId)) {
            b.muted = engine->effectiveMicMuted();
        }
    }
    return out;
}

ActionInfoList DBusControl::actionList() const
{
    ActionInfoList out;
    for (const QString &id : m_app->actionIds()) {
        out.append({id, Preferences::actionLabel(id)});
    }
    return out;
}

void DBusControl::TriggerAction(const QString &id)
{
    reply(triggerAction(id));
}

void DBusControl::PressAction(const QString &id)
{
    if (const Error e = checkKnown(id)) {
        reply(e);
        return;
    }
    if (!actions::isHold(id)) {
        reply(triggerAction(id));
        return;
    }
    if (calledFromDBus() && m_watcher) {
        m_holds[message().service()].insert(id);
        m_watcher->addWatchedService(message().service());
    }
    m_app->runAction(id, AppController::Origin::Remote);
}

void DBusControl::ReleaseAction(const QString &id)
{
    if (const Error e = checkKnown(id)) {
        reply(e);
        return;
    }
    if (calledFromDBus()) {
        m_holds[message().service()].remove(id);
    }
    m_app->releaseAction(id, AppController::Origin::Remote);
}

void DBusControl::releaseHoldsOf(const QString &service)
{
    const QSet<QString> ids = m_holds.take(service);
    m_watcher->removeWatchedService(service);
    for (const QString &id : ids) {
        qCInfo(lcDBus) << "Releasing" << id << "held by a D-Bus client that went away";
        m_app->releaseAction(id, AppController::Origin::Remote);
    }
}

void DBusControl::SwitchScene(const QString &name)
{
    reply(switchScene(name));
}
void DBusControl::SetBusVolume(const QString &busId, double position)
{
    reply(setBusVolume(busId, position));
}
void DBusControl::SetBusMuted(const QString &busId, bool muted)
{
    reply(setBusMuted(busId, muted));
}
void DBusControl::ToggleBusMuted(const QString &busId)
{
    reply(toggleBusMuted(busId));
}
void DBusControl::SetMicMuted(bool muted)
{
    setMicMuted(muted);
}
void DBusControl::ToggleMicMute()
{
    toggleMicMute();
}
QStringList DBusControl::ListScenes()
{
    return m_app->sceneNames();
}
BusInfoList DBusControl::ListBuses()
{
    return buses();
}
ActionInfoList DBusControl::ListActions()
{
    return actionList();
}

} // namespace rostrum::app
