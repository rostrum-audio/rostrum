#include "app/Preferences.h"

#include "app/AppController.h"
#include "app/Desktop.h"
#include "app/Hotkeys.h"
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

int Preferences::skippedApps() const { return int(m_app->settings().autoSkip.size()); }
void Preferences::forgetSkippedApps() { m_app->engine()->forgetAutoSkip(); }

QString Preferences::actionLabel(const QString &id)
{
    // Labels come from core in English; translate them here.
    static const QMap<QString, KLocalizedString> labels = {
        {QString::fromLatin1(actions::kMuteMic), ki18nc("@label shortcut action", "Mute mic")},
        {QString::fromLatin1(actions::kMuteStream), ki18nc("@label shortcut action", "Mute all playback to stream")},
        {QString::fromLatin1(actions::kPrevScene), ki18nc("@label shortcut action", "Previous scene")},
        {QString::fromLatin1(actions::kNextScene), ki18nc("@label shortcut action", "Next scene")},
        {QString::fromLatin1(actions::kScene1), ki18nc("@label shortcut action", "Load scene 1")},
        {QString::fromLatin1(actions::kScene2), ki18nc("@label shortcut action", "Load scene 2")},
        {QString::fromLatin1(actions::kScene3), ki18nc("@label shortcut action", "Load scene 3")},
        {QString::fromLatin1(actions::kScene4), ki18nc("@label shortcut action", "Load scene 4")},
    };
    return labels.contains(id) ? labels.value(id).toString() : actions::label(id);
}

QVariantList Preferences::hotkeys() const
{
    QVariantList rows;
    const auto &keys = m_app->settings().hotkeys;
    const Hotkeys *global = Desktop::instance() ? Desktop::instance()->hotkeys() : nullptr;
    for (const QString &id : actions::all()) {
        rows << QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("label"), actionLabel(id)},
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
    if (!actions::all().contains(actionId)) {
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
