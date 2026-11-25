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

QDBusArgument &operator<<(QDBusArgument &arg, const BusLevels &b)
{
    arg.beginStructure();
    arg << b.id << b.name << b.stream << b.phones << b.muted << b.destination;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, BusLevels &b)
{
    arg.beginStructure();
    arg >> b.id >> b.name >> b.stream >> b.phones >> b.muted >> b.destination;
    arg.endStructure();
    return arg;
}

QDBusArgument &operator<<(QDBusArgument &arg, const BusChat &b)
{
    arg.beginStructure();
    arg << b.id << b.chat << b.muted << b.volume;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, BusChat &b)
{
    arg.beginStructure();
    arg >> b.id >> b.chat >> b.muted >> b.volume;
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
    qDBusRegisterMetaType<BusLevels>();
    qDBusRegisterMetaType<BusLevelsList>();
    qDBusRegisterMetaType<BusChat>();
    qDBusRegisterMetaType<BusChatList>();
    qDBusRegisterMetaType<ActionInfo>();
    qDBusRegisterMetaType<ActionInfoList>();
    for (auto sig : {&AppController::levelsChanged, &AppController::scenesChanged, &AppController::statusChanged,
                     &AppController::settingsChanged}) {
        connect(m_app, sig, this, &DBusControl::publishChanges);
    }
    connect(m_app->engine(), &engine::Engine::perMixLevelsChanged, this, &DBusControl::publishChanges);
    connect(m_app->engine(), &engine::Engine::chatMicChanged, this, &DBusControl::publishChanges);
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
bool DBusControl::micFilters() const
{
    return m_app->engine()->micFilters().enabled;
}

bool DBusControl::perMixLevels() const
{
    return m_app->engine()->perMixUnavailable().isEmpty();
}

bool DBusControl::chatMic() const
{
    return m_app->engine()->chatMicActive();
}

// The PropertiesChanged signal for the properties above, sent only when one changed.
void DBusControl::publishChanges()
{
    const QVariantMap now{{QStringLiteral("MicMuted"), micMuted()},
                          {QStringLiteral("StreamMuted"), streamMuted()},
                          {QStringLiteral("Panic"), panic()},
                          {QStringLiteral("CurrentScene"), currentScene()},
                          {QStringLiteral("Connected"), connected()},
                          {QStringLiteral("MicFilters"), micFilters()},
                          {QStringLiteral("PerMixLevels"), perMixLevels()},
                          {QStringLiteral("ChatMic"), chatMic()}};
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
    return remote::resolveBusId(m_app->engine()->scene(), text);
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
    const auto target = remote::resolveVolumeTarget(m_app->engine()->scene(), busText);
    const QString &busId = target.busId;
    if (target.error == QLatin1String(remote::kErrorNoSuchBus)) {
        return error(remote::kErrorNoSuchBus, i18n("The live scene has no bus “%1”.", busText));
    }
    if (target.error == QLatin1String(remote::kErrorInvalidValue)) {
        if (target.chat && busId != QLatin1String(kMicBusId) && busId != QLatin1String(remote::kStreamId) &&
            busId != QLatin1String(remote::kPhonesId)) {
            return error(remote::kErrorInvalidValue, i18n("“%1” cannot send to voice chat.", busText));
        }
        return error(remote::kErrorInvalidValue, busId == QLatin1String(kMicBusId)
                                                     ? i18n("The mic has one level.")
                                                     : i18n("The %1 master has one level.", busId));
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
    } else if (target.chat) {
        // The mixer refuses to turn voice chat on where it cannot run. Setting the level of a
        // bus that is not already sending there is the same switch.
        const Bus *bus = engine->scene().bus(busId);
        const QString why = engine->chatUnavailable();
        if (bus && !bus->chat && !why.isEmpty()) {
            return error(remote::kErrorInvalidValue, why);
        }
        engine->setBusChatVolume(busId, position);
    } else if (target.mix) {
        engine->setBusMixVolume(busId, *target.mix, position); // a linked bus moves both, as in the UI
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

DBusControl::Error DBusControl::stepBusVolume(const QString &busText, const QString &mix, double delta,
                                              double *position)
{
    auto *engine = m_app->engine();
    const auto target = remote::resolveStep(engine->scene(), busText, mix, delta, perMixLevels());
    const QString &busId = target.busId;
    switch (target.problem) {
    case remote::StepProblem::None:
        break;
    case remote::StepProblem::NoSuchBus:
        return error(remote::kErrorNoSuchBus, i18n("The live scene has no bus “%1”.", busText));
    case remote::StepProblem::BadSide:
        if (remote::parseSide(mix) == remote::Side::Chat) {
            return error(remote::kErrorInvalidValue, i18n("“%1” cannot send to voice chat.", busText));
        }
        return error(remote::kErrorInvalidValue,
                     i18n("The mix must be “stream”, “phones”, “chat” or “both”, not “%1”.", mix));
    case remote::StepProblem::OneLevelBus:
        return error(remote::kErrorInvalidValue, busId == QLatin1String(kMicBusId)
                                                     ? i18n("The mic has one level.")
                                                     : i18n("The %1 master has one level.", busId));
    case remote::StepProblem::BadDelta:
        return error(remote::kErrorInvalidValue, i18n("The step must be between -1 and 1."));
    case remote::StepProblem::NoPerMixLevels:
        return error(remote::kErrorInvalidValue,
                     i18n("This system has one level per bus, so “%1” cannot move one mix alone.", busText));
    }
    const Scene &s = engine->scene();
    if (busId == QLatin1String(remote::kStreamId)) {
        engine->setMasterStream(s.masterStream + delta);
        *position = engine->scene().masterStream;
    } else if (busId == QLatin1String(remote::kPhonesId)) {
        engine->setMasterPhones(s.masterPhones + delta);
        *position = engine->scene().masterPhones;
    } else {
        const Bus &b = *s.bus(busId);
        if (b.isInput()) {
            engine->setBusVolume(busId, b.volume + delta);
        } else if (target.side == remote::Side::Chat) {
            const QString why = engine->chatUnavailable();
            if (!b.chat && !why.isEmpty()) {
                return error(remote::kErrorInvalidValue, why);
            }
            engine->setBusChatVolume(busId, b.chatVolume + delta);
        } else if (target.side == remote::Side::Both) {
            engine->setBusMixVolumeTogether(busId, Mix::Stream, b.volume + delta);
        } else if (target.side == remote::Side::Phones) {
            engine->setBusMixVolumeAlone(busId, Mix::Phones, b.phonesVolume + delta);
        } else {
            engine->setBusMixVolumeAlone(busId, Mix::Stream, b.volume + delta);
        }
        const Bus &after = *engine->scene().bus(busId);
        *position = target.side == remote::Side::Phones ? after.phonesVolume
                    : target.side == remote::Side::Chat ? after.chatVolume
                                                        : after.volume;
    }
    return {};
}

DBusControl::Error DBusControl::toggleBusInMix(const QString &busText, const QString &mix)
{
    auto *engine = m_app->engine();
    const auto target = remote::resolveInMix(engine->scene(), busText, mix);
    switch (target.problem) {
    case remote::InMixProblem::None:
        break;
    case remote::InMixProblem::NoSuchBus:
        return error(remote::kErrorNoSuchBus, i18n("The live scene has no bus “%1”.", busText));
    case remote::InMixProblem::BadSide:
        if (remote::parseSide(mix) == remote::Side::Chat) {
            return error(remote::kErrorInvalidValue, i18n("“%1” cannot send to voice chat.", busText));
        }
        return error(remote::kErrorInvalidValue,
                     i18n("The mix must be “stream”, “phones” or “chat”, not “%1”.", mix));
    case remote::InMixProblem::NotPlaybackBus:
        return error(remote::kErrorInvalidValue,
                     i18n("“%1” is not a playback bus, so it has no destination.", busText));
    }
    const Bus &b = *engine->scene().bus(target.busId);
    if (target.chat) {
        // The first press sets the destination up; after that it only switches it off and on.
        // The mixer leaves the checkbox off, unless it is already on, while voice chat cannot run.
        const QString why = engine->chatUnavailable();
        if (!b.chat && !why.isEmpty()) {
            return error(remote::kErrorInvalidValue, why);
        }
        const auto before = m_app->snapshot();
        const remote::ChatState to = remote::toggledChat(b.chat, b.chatMuted);
        if (!b.chat) {
            engine->setBusChat(target.busId, true);
        } else if (to.muted != b.chatMuted) {
            engine->setBusChatMuted(target.busId, to.muted);
        }
        m_app->reportChange(before);
        return {};
    }
    const remote::InMix to = remote::toggledInMix(b.destination, b.muted, target.mix);
    const bool destinationChanges = to.destination != b.destination;
    const bool muteChanges = to.muted != b.muted;
    const auto before = m_app->snapshot();
    if (destinationChanges) {
        engine->setBusDestination(target.busId, to.destination);
    }
    if (muteChanges) {
        engine->setBusMuted(target.busId, to.muted);
    }
    m_app->reportChange(before);
    return {};
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

void DBusControl::setMicFilters(bool on)
{
    const auto before = m_app->snapshot();
    micfx::Settings fx = m_app->engine()->micFilters();
    fx.enabled = on;
    m_app->engine()->setMicFilters(fx);
    m_app->reportChange(before);
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

BusLevelsList DBusControl::busLevels() const
{
    const Scene &s = m_app->engine()->scene();
    BusLevelsList out;
    for (const BusInfo &row : buses()) {
        BusLevels levels{row.id, row.name, row.position, row.position, row.muted, QString()};
        if (const Bus *b = s.bus(row.id); b && !b->isInput()) {
            levels.phones = b->phonesVolume;
            levels.destination = destinationName(b->destination);
        }
        out.append(levels);
    }
    return out;
}

BusChatList DBusControl::busChat() const
{
    BusChatList out;
    for (const Bus &b : m_app->engine()->scene().buses) {
        if (chatAllowed(b)) {
            out.append({b.id, b.chat, b.chatMuted, b.chatVolume});
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
double DBusControl::StepBusVolume(const QString &busId, const QString &mix, double delta)
{
    double position = 0.0;
    reply(stepBusVolume(busId, mix, delta, &position));
    return position;
}
void DBusControl::ToggleBusInMix(const QString &busId, const QString &mix)
{
    reply(toggleBusInMix(busId, mix));
}
void DBusControl::SetMicMuted(bool muted)
{
    setMicMuted(muted);
}
void DBusControl::ToggleMicMute()
{
    toggleMicMute();
}
void DBusControl::SetMicFilters(bool on)
{
    setMicFilters(on);
}
QStringList DBusControl::ListScenes()
{
    return m_app->sceneNames();
}
BusInfoList DBusControl::ListBuses()
{
    return buses();
}
BusLevelsList DBusControl::ListBusLevels()
{
    return busLevels();
}
BusChatList DBusControl::ListBusChat()
{
    return busChat();
}
ActionInfoList DBusControl::ListActions()
{
    return actionList();
}

} // namespace rostrum::app
