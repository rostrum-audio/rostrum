#include "core/Settings.h"

#include "core/SceneStore.h"

#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <toml++/toml.hpp>

namespace rostrum {

namespace actions {

QStringList all()
{
    return {QString::fromLatin1(kMuteMic),  QString::fromLatin1(kMuteStream), QString::fromLatin1(kPrevScene),
            QString::fromLatin1(kNextScene), QString::fromLatin1(kScene1),     QString::fromLatin1(kScene2),
            QString::fromLatin1(kScene3),    QString::fromLatin1(kScene4)};
}

QString label(const QString &id)
{
    static const QMap<QString, QString> labels = {
        {QString::fromLatin1(kMuteMic), QStringLiteral("Mute mic")},
        {QString::fromLatin1(kMuteStream), QStringLiteral("Mute all playback to stream")},
        {QString::fromLatin1(kPrevScene), QStringLiteral("Previous scene")},
        {QString::fromLatin1(kNextScene), QStringLiteral("Next scene")},
        {QString::fromLatin1(kScene1), QStringLiteral("Load scene 1")},
        {QString::fromLatin1(kScene2), QStringLiteral("Load scene 2")},
        {QString::fromLatin1(kScene3), QStringLiteral("Load scene 3")},
        {QString::fromLatin1(kScene4), QStringLiteral("Load scene 4")},
    };
    return labels.value(id, id);
}

QString defaultShortcut(const QString &id)
{
    static const QMap<QString, QString> keys = {
        {QString::fromLatin1(kMuteMic), QStringLiteral("Meta+Alt+M")},
        {QString::fromLatin1(kMuteStream), QStringLiteral("Meta+Alt+S")},
        {QString::fromLatin1(kPrevScene), QStringLiteral("Meta+Alt+PgUp")},
        {QString::fromLatin1(kNextScene), QStringLiteral("Meta+Alt+PgDown")},
        {QString::fromLatin1(kScene1), QStringLiteral("Meta+Alt+1")},
        {QString::fromLatin1(kScene2), QStringLiteral("Meta+Alt+2")},
        {QString::fromLatin1(kScene3), QStringLiteral("Meta+Alt+3")},
        {QString::fromLatin1(kScene4), QStringLiteral("Meta+Alt+4")},
    };
    return keys.value(id);
}

} // namespace actions

Settings defaultSettings()
{
    Settings s;
    for (const auto &id : actions::all()) {
        s.hotkeys.insert(id, actions::defaultShortcut(id));
    }
    return s;
}

namespace {

template<typename T>
T get(const toml::table &t, std::string_view section, std::string_view key, T fallback)
{
    if (auto v = t[section][key].value<T>()) {
        return *v;
    }
    return fallback;
}

QString getStr(const toml::table &t, std::string_view section, std::string_view key, const QString &fallback)
{
    if (auto v = t[section][key].value<std::string>()) {
        return QString::fromStdString(*v);
    }
    return fallback;
}

} // namespace

QString serializeSettings(const Settings &s)
{
    toml::table hotkeys;
    for (auto it = s.hotkeys.cbegin(); it != s.hotkeys.cend(); ++it) {
        hotkeys.insert(it.key().toStdString(), it.value().toStdString());
    }
    toml::array skip;
    for (const auto &key : s.autoSkip) {
        skip.push_back(key.toStdString());
    }
    toml::array duckBuses;
    for (const auto &id : s.ducking.buses) {
        duckBuses.push_back(id.toStdString());
    }
    toml::table t{
        {"format", 1},
        {"general",
         toml::table{
             {"wizard_done", s.wizardDone},
             {"setup_version", s.setupVersion},
             {"launch_at_login", s.launchAtLogin},
             {"start_in_tray", s.startInTray},
             {"auto_save_scenes", s.autoSaveScenes},
             {"confirm_scene_switch", s.confirmSceneSwitch},
             {"scroll_to_adjust", s.scrollToAdjust},
             {"scene_fade_ms", s.sceneFadeMs},
         }},
        {"mixer", toml::table{{"meter_speed", s.meterSpeed.toStdString()}, {"show_db", s.showDb}}},
        {"ducking",
         toml::table{
             {"enabled", s.ducking.enabled},
             {"trigger", ducking::triggerName(s.ducking.trigger).toStdString()},
             {"buses", duckBuses},
             {"amount_db", s.ducking.amountDb},
             {"attack_ms", s.ducking.attackMs},
             {"release_ms", s.ducking.releaseMs},
         }},
        {"apps", toml::table{{"auto_assign", s.autoAssign}, {"auto_skip", skip}}},
        {"privacy", toml::table{{"crash_reports", s.crashReports.toStdString()}}},
        {"updates",
         toml::table{
             {"check", s.checkUpdates},
             {"install", s.installUpdates},
             {"skipped_version", s.skippedVersion.toStdString()},
             {"last_check", int64_t(s.lastUpdateCheck)},
         }},
        {"advanced", toml::table{{"show_node_ids", s.showNodeIds}}},
        {"scenes", toml::table{{"default", s.defaultScene.toStdString()}}},
        {"devices",
         toml::table{
             {"headphones", s.headphones.toStdString()},
             {"mic", s.mic.toStdString()},
             {"mic_fallback", s.micFallback},
             {"mono_headphones", s.monoHeadphones},
         }},
        {"hotkeys", hotkeys},
        {"window",
         toml::table{
             {"width", s.windowWidth},
             {"height", s.windowHeight},
             {"page", s.lastPage.toStdString()},
             {"sidebar_collapsed", s.sidebarCollapsed},
         }},
    };
    std::ostringstream out;
    out << t << '\n';
    return QString::fromStdString(out.str());
}

Settings parseSettings(const QString &text, QString *error)
{
    Settings s = defaultSettings();
    toml::table t;
    try {
        t = toml::parse(text.toStdString());
    } catch (const toml::parse_error &e) {
        if (error) {
            *error = QString::fromStdString(std::string(e.description()));
        }
        return s;
    }
    s.wizardDone = get(t, "general", "wizard_done", s.wizardDone);
    // Files from before setup was versioned saw only the device steps.
    s.setupVersion = int(get<int64_t>(t, "general", "setup_version", s.wizardDone ? 1 : 0));
    s.launchAtLogin = get(t, "general", "launch_at_login", s.launchAtLogin);
    s.startInTray = get(t, "general", "start_in_tray", s.startInTray);
    s.autoSaveScenes = get(t, "general", "auto_save_scenes", s.autoSaveScenes);
    s.confirmSceneSwitch = get(t, "general", "confirm_scene_switch", s.confirmSceneSwitch);
    s.scrollToAdjust = get(t, "general", "scroll_to_adjust", s.scrollToAdjust);
    // Hand-edited lengths snap to the nearest offered one.
    const auto fade = get<int64_t>(t, "general", "scene_fade_ms", s.sceneFadeMs);
    for (const int choice : kSceneFadeChoicesMs) {
        if (std::abs(fade - choice) < std::abs(fade - s.sceneFadeMs)) {
            s.sceneFadeMs = choice;
        }
    }
    s.meterSpeed = getStr(t, "mixer", "meter_speed", s.meterSpeed);
    if (s.meterSpeed != QLatin1String("low")) {
        s.meterSpeed = QStringLiteral("normal");
    }
    s.showDb = get(t, "mixer", "show_db", s.showDb);
    s.ducking.enabled = get(t, "ducking", "enabled", s.ducking.enabled);
    s.ducking.trigger = ducking::triggerFromString(getStr(t, "ducking", "trigger", QString()))
                            .value_or(s.ducking.trigger);
    if (const auto *buses = t["ducking"]["buses"].as_array()) {
        s.ducking.buses.clear();
        for (const auto &v : *buses) {
            if (auto id = v.value<std::string>()) {
                s.ducking.buses << QString::fromStdString(*id);
            }
        }
    }
    // Clamped before the int cast; sanitize() then snaps to an offered value.
    auto number = [&t](std::string_view key, int fallback) {
        const double v = get<double>(t, "ducking", key, fallback);
        return std::isfinite(v) ? int(std::clamp(v, -60000.0, 60000.0)) : fallback;
    };
    s.ducking.amountDb = number("amount_db", s.ducking.amountDb);
    s.ducking.attackMs = number("attack_ms", s.ducking.attackMs);
    s.ducking.releaseMs = number("release_ms", s.ducking.releaseMs);
    s.ducking = ducking::sanitize(s.ducking);
    s.autoAssign = get(t, "apps", "auto_assign", s.autoAssign);
    if (const auto *skip = t["apps"]["auto_skip"].as_array()) {
        for (const auto &v : *skip) {
            if (auto key = v.value<std::string>(); key && !key->empty()) {
                s.autoSkip << QString::fromStdString(*key);
            }
        }
        s.autoSkip.removeDuplicates();
    }
    s.crashReports = getStr(t, "privacy", "crash_reports", s.crashReports);
    if (s.crashReports != QLatin1String(crashmode::kSend) && s.crashReports != QLatin1String(crashmode::kNever)) {
        s.crashReports = QString::fromLatin1(crashmode::kAsk);
    }
    s.checkUpdates = get(t, "updates", "check", s.checkUpdates);
    s.installUpdates = get(t, "updates", "install", s.installUpdates);
    s.skippedVersion = getStr(t, "updates", "skipped_version", s.skippedVersion);
    s.lastUpdateCheck = get<int64_t>(t, "updates", "last_check", s.lastUpdateCheck);
    s.showNodeIds = get(t, "advanced", "show_node_ids", s.showNodeIds);
    s.defaultScene = getStr(t, "scenes", "default", s.defaultScene);
    s.headphones = getStr(t, "devices", "headphones", s.headphones);
    s.mic = getStr(t, "devices", "mic", s.mic);
    s.micFallback = get(t, "devices", "mic_fallback", s.micFallback);
    s.monoHeadphones = get(t, "devices", "mono_headphones", s.monoHeadphones);
    if (const auto *hk = t["hotkeys"].as_table()) {
        for (auto &&[k, v] : *hk) {
            const QString id = QString::fromStdString(std::string(k.str()));
            if (actions::all().contains(id)) {
                s.hotkeys.insert(id, QString::fromStdString(v.value_or(std::string())));
            }
        }
    }
    s.windowWidth = std::max(960, int(get<int64_t>(t, "window", "width", s.windowWidth)));
    s.windowHeight = std::max(600, int(get<int64_t>(t, "window", "height", s.windowHeight)));
    s.lastPage = getStr(t, "window", "page", s.lastPage);
    s.sidebarCollapsed = get(t, "window", "sidebar_collapsed", s.sidebarCollapsed);
    return s;
}

Settings loadSettings(const QString &path, QString *error)
{
    if (!QFileInfo::exists(path)) {
        return defaultSettings();
    }
    QString err;
    const QString text = SceneStore::readFile(path, &err);
    if (!err.isEmpty()) {
        if (error) {
            *error = err;
        }
        return defaultSettings();
    }
    return parseSettings(text, error);
}

bool saveSettings(const QString &path, const Settings &settings, QString *error)
{
    return SceneStore::writeFile(path, serializeSettings(settings), error);
}

} // namespace rostrum
