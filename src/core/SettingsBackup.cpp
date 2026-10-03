#include "core/SettingsBackup.h"

#include "core/SceneToml.h"

#include <toml++/toml.hpp>

#include <sstream>

namespace rostrum::backup {

namespace {

constexpr int kFormatVersion = 1;
constexpr const char *kKind = "rostrum-backup";

QString toText(const toml::table &t)
{
    std::ostringstream out;
    out << toml::toml_formatter{t, toml::toml_formatter::default_flags |
                                       toml::format_flags::relaxed_float_precision}
        << '\n';
    return QString::fromStdString(out.str());
}

} // namespace

QString serialize(const Settings &settings, const QList<Scene> &scenes, const QDateTime &now)
{
    toml::table s = toml::parse(serializeSettings(settings).toStdString());
    for (const char *key : {"format", "privacy", "updates", "window"}) {
        s.erase(key);
    }
    if (auto *general = s["general"].as_table()) {
        general->erase("wizard_done");
        general->erase("setup_version");
    }

    QList<Scene> clean = scenes;
    for (auto &scene : clean) {
        for (auto &rule : scene.rules) {
            rule.lastSeen = {};
        }
    }
    toml::table bundle = toml::parse(toml_io::serializeBundle(clean).toStdString());

    toml::table root;
    root.insert("format", kFormatVersion);
    root.insert("kind", kKind);
    root.insert("created", now.toUTC().toString(Qt::ISODate).toStdString());
    root.insert("settings", std::move(s));
    if (auto *arr = bundle["scene"].as_array()) {
        root.insert("scene", std::move(*arr));
    }
    return toText(root);
}

std::optional<Bundle> parse(const QString &text, QString *error)
{
    toml::table t;
    try {
        t = toml::parse(text.toStdString());
    } catch (const toml::parse_error &e) {
        if (error) {
            *error = QString::fromStdString(std::string(e.description()));
        }
        return std::nullopt;
    }
    if (t["kind"].value_or(std::string()) != kKind) {
        if (error) {
            *error = QStringLiteral("The file is not a Rostrum backup.");
        }
        return std::nullopt;
    }
    if (t["format"].value_or(0) > kFormatVersion) {
        if (error) {
            *error = QStringLiteral("The backup is from a newer Rostrum.");
        }
        return std::nullopt;
    }
    Bundle b;
    b.created =
        QDateTime::fromString(QString::fromStdString(t["created"].value_or(std::string())), Qt::ISODate);
    if (const auto *s = t["settings"].as_table()) {
        b.settings = parseSettings(toText(*s));
    }
    if (const auto *arr = t["scene"].as_array()) {
        toml::table scenes;
        scenes.insert("scene", *arr);
        b.scenes = toml_io::parseBundle(toText(scenes));
    }
    return b;
}

Settings restoredSettings(const Settings &current, const Settings &backup)
{
    Settings r = backup;
    r.wizardDone = current.wizardDone;
    r.setupVersion = current.setupVersion;
    r.launchAtLogin = current.launchAtLogin;
    r.crashReports = current.crashReports;
    r.checkUpdates = current.checkUpdates;
    r.installUpdates = current.installUpdates;
    r.skippedVersion = current.skippedVersion;
    r.lastUpdateCheck = current.lastUpdateCheck;
    r.windowWidth = current.windowWidth;
    r.windowHeight = current.windowHeight;
    r.lastPage = current.lastPage;
    r.sidebarCollapsed = current.sidebarCollapsed;
    r.compactWindow = current.compactWindow;
    r.compactWidth = current.compactWidth;
    r.compactHeight = current.compactHeight;
    r.keepOnTop = current.keepOnTop;
    return r;
}

} // namespace rostrum::backup
