#include "app/Preferences.h"

#include "app/AppController.h"
#include "app/Desktop.h"
#include "app/Hotkeys.h"
#include "app/Obs.h"
#include "core/Paths.h"

#include <KLocalizedString>

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJSEngine>
#include <QKeySequence>

#include <algorithm>
#include <iterator>

namespace rostrum::app {

Preferences *Preferences::s_instance = nullptr;

Preferences::Preferences(AppController *app, QObject *parent)
    : QObject(parent)
    , m_app(app)
{
    Q_ASSERT(!s_instance);
    s_instance = this;
    connect(app, &AppController::settingsChanged, this, &Preferences::changed);
    connect(app, &AppController::actionsChanged, this, &Preferences::changed);
    if (Desktop::instance()) {
        connect(Desktop::instance()->hotkeys(), &Hotkeys::statusChanged, this, &Preferences::changed);
    }
}

Preferences::~Preferences()
{
    s_instance = nullptr;
}

Preferences *Preferences::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

template<typename T>
void Preferences::update(T &field, const T &value)
{
    if (field == value) {
        return;
    }
    field = value;
    m_app->saveSettingsSoon();
    Q_EMIT m_app->settingsChanged();
}

bool Preferences::autoSaveScenes() const { return m_app->settings().autoSaveScenes; }

void Preferences::setAutoSaveScenes(bool on)
{
    update(m_app->settings().autoSaveScenes, on);
    m_app->scenes()->setAutoSave(on);
}

bool Preferences::confirmSceneSwitch() const { return m_app->settings().confirmSceneSwitch; }
void Preferences::setConfirmSceneSwitch(bool on) { update(m_app->settings().confirmSceneSwitch, on); }
bool Preferences::scrollToAdjust() const { return m_app->settings().scrollToAdjust; }
void Preferences::setScrollToAdjust(bool on) { update(m_app->settings().scrollToAdjust, on); }
bool Preferences::osdFeedback() const { return m_app->settings().osdFeedback; }
void Preferences::setOsdFeedback(bool on) { update(m_app->settings().osdFeedback, on); }
int Preferences::sceneFadeMs() const { return m_app->settings().sceneFadeMs; }

void Preferences::setSceneFadeMs(int ms)
{
    if (std::find(std::begin(kSceneFadeChoicesMs), std::end(kSceneFadeChoicesMs), ms) ==
        std::end(kSceneFadeChoicesMs)) {
        return;
    }
    update(m_app->settings().sceneFadeMs, ms);
    m_app->engine()->setSceneFadeMs(ms);
}

bool Preferences::lowMeterSpeed() const { return m_app->settings().meterSpeed == QLatin1String("low"); }

void Preferences::setLowMeterSpeed(bool on)
{
    update(m_app->settings().meterSpeed, on ? QStringLiteral("low") : QStringLiteral("normal"));
}

bool Preferences::showDb() const { return m_app->settings().showDb; }
void Preferences::setShowDb(bool on) { update(m_app->settings().showDb, on); }
bool Preferences::showNodeIds() const { return m_app->settings().showNodeIds; }
void Preferences::setShowNodeIds(bool on) { update(m_app->settings().showNodeIds, on); }
bool Preferences::autoAssign() const { return m_app->settings().autoAssign; }

void Preferences::setAutoAssign(bool on)
{
    update(m_app->settings().autoAssign, on);
    m_app->engine()->setAutoAssign(on);
}

void Preferences::updateDucking(const std::function<void(ducking::Settings &)> &change)
{
    ducking::Settings d = m_app->settings().ducking;
    change(d);
    update(m_app->settings().ducking, ducking::sanitize(d));
    m_app->engine()->setDucking(m_app->settings().ducking);
}

bool Preferences::duckingEnabled() const { return m_app->settings().ducking.enabled; }
void Preferences::setDuckingEnabled(bool on)
{
    updateDucking([on](ducking::Settings &d) { d.enabled = on; });
}

QString Preferences::duckingTrigger() const
{
    return ducking::triggerName(m_app->settings().ducking.trigger);
}

void Preferences::setDuckingTrigger(const QString &trigger)
{
    if (const auto t = ducking::triggerFromString(trigger)) {
        updateDucking([t](ducking::Settings &d) { d.trigger = *t; });
    }
}

QStringList Preferences::duckingBuses() const { return m_app->settings().ducking.buses; }
void Preferences::setDuckingBus(const QString &busId, bool ducked)
{
    updateDucking([&](ducking::Settings &d) {
        d.buses.removeAll(busId);
        if (ducked) {
            d.buses << busId;
        }
    });
}

int Preferences::duckingAmountDb() const { return m_app->settings().ducking.amountDb; }
void Preferences::setDuckingAmountDb(int db)
{
    updateDucking([db](ducking::Settings &d) { d.amountDb = db; });
}

int Preferences::duckingAttackMs() const { return m_app->settings().ducking.attackMs; }
void Preferences::setDuckingAttackMs(int ms)
{
    updateDucking([ms](ducking::Settings &d) { d.attackMs = ms; });
}

int Preferences::duckingReleaseMs() const { return m_app->settings().ducking.releaseMs; }
void Preferences::setDuckingReleaseMs(int ms)
{
    updateDucking([ms](ducking::Settings &d) { d.releaseMs = ms; });
}

int Preferences::skippedApps() const { return int(m_app->settings().autoSkip.size()); }
void Preferences::forgetSkippedApps() { m_app->engine()->forgetAutoSkip(); }

QString Preferences::actionLabel(const QString &id, const QString &busName)
{
    // Labels come from core in English; translate them here.
    static const QMap<QString, KLocalizedString> labels = {
        {QString::fromLatin1(actions::kMuteMic), ki18nc("@label shortcut action", "Mute mic")},
        {QString::fromLatin1(actions::kMuteStream), ki18nc("@label shortcut action", "Mute all playback to stream")},
        {QString::fromLatin1(actions::kPrevScene), ki18nc("@label shortcut action", "Previous scene")},
        {QString::fromLatin1(actions::kNextScene), ki18nc("@label shortcut action", "Next scene")},
        {QString::fromLatin1(actions::kPushToTalk), ki18nc("@label shortcut action", "Push to talk")},
        {QString::fromLatin1(actions::kPushToMute), ki18nc("@label shortcut action", "Push to mute")},
        {QString::fromLatin1(actions::kPanicMute), ki18nc("@label shortcut action", "Panic mute")},
        {QString::fromLatin1(actions::kToggleSidetone),
         ki18nc("@label shortcut action", "Sidetone on or off")},
        {QString::fromLatin1(actions::kToggleMicFilters),
         ki18nc("@label shortcut action", "Mic filters on or off")},
        {QString::fromLatin1(actions::kMuteHeadphones), ki18nc("@label shortcut action", "Mute headphones")},
        {QString::fromLatin1(actions::kStreamVolumeUp), ki18nc("@label shortcut action", "Stream volume up")},
        {QString::fromLatin1(actions::kStreamVolumeDown),
         ki18nc("@label shortcut action", "Stream volume down")},
    };
    if (labels.contains(id)) {
        return labels.value(id).toString();
    }
    if (const int slot = actions::sceneSlot(id)) {
        return i18nc("@label shortcut action", "Load scene %1", slot);
    }
    if (const QString bus = actions::busOfAction(id); !bus.isEmpty()) {
        const auto *app = AppController::instance();
        const QString name = !busName.isEmpty() ? busName : app ? app->busName(bus) : bus;
        return i18nc("@label shortcut action, bus name", "Mute %1 bus", name);
    }
    return actions::label(id);
}

QString Preferences::actionDescription(const QString &id)
{
    static const QMap<QString, KLocalizedString> descriptions = {
        {QString::fromLatin1(actions::kMuteMic),
         ki18nc("@info shortcut action", "Mutes or unmutes the mic.")},
        {QString::fromLatin1(actions::kPushToTalk),
         ki18nc("@info shortcut action", "Hold to talk while the mic is muted. Letting go mutes it again.")},
        {QString::fromLatin1(actions::kPushToMute),
         ki18nc("@info shortcut action",
                "Hold to mute the mic, for a cough or a sip. Letting go brings it back.")},
        {QString::fromLatin1(actions::kPanicMute),
         ki18nc("@info shortcut action", "Mutes the mic and everything going to stream at once. Press again "
                                         "to bring both back as they were.")},
        {QString::fromLatin1(actions::kToggleSidetone),
         ki18nc("@info shortcut action", "Turns hearing your own mic in the headphones on or off.")},
        {QString::fromLatin1(actions::kToggleMicFilters),
         ki18nc("@info shortcut action",
                "Turns noise removal and the other mic filters on or off, for the stream and for apps.")},
        {QString::fromLatin1(actions::kMuteStream),
         ki18nc("@info shortcut action",
                "Mutes or unmutes the Stream master. Your headphones are not affected.")},
        {QString::fromLatin1(actions::kMuteHeadphones),
         ki18nc("@info shortcut action",
                "Mutes or unmutes the Headphones master. The stream is not affected.")},
        {QString::fromLatin1(actions::kStreamVolumeUp),
         ki18nc("@info shortcut action", "Raises the Stream master by 5 %.")},
        {QString::fromLatin1(actions::kStreamVolumeDown),
         ki18nc("@info shortcut action", "Lowers the Stream master by 5 %.")},
    };
    if (descriptions.contains(id)) {
        return descriptions.value(id).toString();
    }
    if (actions::sceneSlot(id) > 0) {
        return i18nc("@info shortcut action", "Counted in the order of the Scenes page.");
    }
    if (!actions::busOfAction(id).isEmpty()) {
        return i18nc("@info shortcut action",
                     "Mutes or unmutes this bus in the live scene, where it has one.");
    }
    return {};
}

QVariantList Preferences::hotkeys() const
{
    auto groupName = [](actions::Group g) {
        switch (g) {
        case actions::Group::Mic:
            return QStringLiteral("mic");
        case actions::Group::Stream:
            return QStringLiteral("stream");
        case actions::Group::Scenes:
            return QStringLiteral("scenes");
        case actions::Group::Buses:
            break;
        }
        return QStringLiteral("buses");
    };
    QVariantList rows;
    const auto &keys = m_app->settings().hotkeys;
    const Hotkeys *global = Desktop::instance() ? Desktop::instance()->hotkeys() : nullptr;
    for (const QString &id : m_app->actionIds()) {
        rows << QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("label"), actionLabel(id)},
            {QStringLiteral("description"), actionDescription(id)},
            {QStringLiteral("group"), groupName(actions::group(id))},
            {QStringLiteral("hold"), actions::isHold(id)},
            {QStringLiteral("shortcut"), keys.value(id, actions::defaultShortcut(id))},
            {QStringLiteral("defaultShortcut"), actions::defaultShortcut(id)},
            {QStringLiteral("global"), global && global->isGlobal(id)},
            {QStringLiteral("problem"), global ? global->problem(id) : QString()},
        };
    }
    return rows;
}

void Preferences::setHotkey(const QString &actionId, const QString &sequence)
{
    if (!actions::isKnown(actionId)) {
        return;
    }
    const QString portable =
        QKeySequence(sequence, QKeySequence::PortableText).toString(QKeySequence::PortableText);
    auto &keys = m_app->settings().hotkeys;
    if (keys.contains(actionId) && keys.value(actionId) == portable) {
        return;
    }
    keys.insert(actionId, portable);
    m_app->saveSettingsSoon();
    Q_EMIT m_app->settingsChanged();
}

void Preferences::setHotkeySequence(const QString &actionId, const QKeySequence &sequence)
{
    setHotkey(actionId, sequence.toString(QKeySequence::PortableText));
}

void Preferences::resetHotkey(const QString &actionId)
{
    setHotkey(actionId, actions::defaultShortcut(actionId));
}

QString Preferences::hotkeyConflict(const QString &actionId, const QString &sequence) const
{
    const QKeySequence wanted(sequence, QKeySequence::PortableText);
    if (wanted.isEmpty()) {
        return {};
    }
    for (const auto &row : hotkeys()) {
        const QVariantMap m = row.toMap();
        if (m.value(QStringLiteral("id")).toString() != actionId &&
            QKeySequence(m.value(QStringLiteral("shortcut")).toString(), QKeySequence::PortableText) == wanted) {
            return m.value(QStringLiteral("label")).toString();
        }
    }
    return {};
}

QString Preferences::configFolder() const { return paths::configDir(); }
QString Preferences::logFile() const { return paths::logFile(); }

QStringList Preferences::ruleFiles() const
{
    return {paths::pipewirePulseFragment(), paths::pipewireClientFragment()};
}

QUrl Preferences::readmeUrl() const
{
    const QString installed = QStringLiteral(ROSTRUM_DOC_DIR "/README.md");
    if (QFileInfo::exists(installed)) {
        return QUrl::fromLocalFile(installed);
    }
    const QString source = QStringLiteral(ROSTRUM_SOURCE_DIR "/README.md");
    return QFileInfo::exists(source) ? QUrl::fromLocalFile(source) : QUrl();
}

void Preferences::openConfigFolder()
{
    QDir().mkpath(paths::configDir());
    QDesktopServices::openUrl(QUrl::fromLocalFile(paths::configDir()));
}

bool Preferences::backUpTo(const QUrl &file)
{
    if (!file.isLocalFile()) {
        Q_EMIT m_app->toast(i18n("Choose a file on this computer."));
        return false;
    }
    QString err;
    if (!m_app->writeBackup(file.toLocalFile(), &err)) {
        Q_EMIT m_app->toast(i18n("Could not back up settings. %1", err));
        return false;
    }
    Q_EMIT m_app->toast(i18n("Settings and scenes backed up"));
    return true;
}

bool Preferences::restoreFrom(const QUrl &file)
{
    if (!file.isLocalFile()) {
        Q_EMIT m_app->toast(i18n("Choose a file on this computer."));
        return false;
    }
    QString safety;
    QString err;
    bool launchAtLogin = false;
    if (!m_app->restoreBackup(file.toLocalFile(), &safety, &err, &launchAtLogin)) {
        Q_EMIT m_app->toast(i18n("Nothing was restored. %1", err));
        return false;
    }
    if (Desktop::instance()) {
        Desktop::instance()->setLaunchAtLogin(launchAtLogin);
    }
    if (Obs::instance()) {
        Obs::instance()->settingsRestored();
    }
    Q_EMIT m_app->toast(i18n("Settings and scenes restored. The previous setup was saved as %1.",
                             QFileInfo(safety).fileName()));
    return true;
}

QString Preferences::backupFileName() const
{
    return QStringLiteral("rostrum-backup-%1.toml").arg(QDate::currentDate().toString(Qt::ISODate));
}

void Preferences::rebuildMix()
{
    m_app->engine()->rebuildMix();
    Q_EMIT m_app->toast(i18n("Rebuilding the virtual devices…"));
}

void Preferences::exportRulesNow()
{
    m_app->scenes()->exportRules();
    Q_EMIT m_app->toast(i18n("App rules written for the next login"));
}

} // namespace rostrum::app
