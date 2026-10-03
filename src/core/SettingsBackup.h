#pragma once

#include "core/Model.h"
#include "core/Settings.h"

#include <QDateTime>
#include <QList>

#include <optional>

namespace rostrum::backup {

// One TOML file with the settings a person sets up by hand (options, devices, hotkeys, scene
// order) and every scene. It leaves out the crash report and update choices, update and
// first-run bookkeeping, window geometry and when each app was last seen, so a backup can be
// shared or moved to another machine without carrying consent or usage history with it.
struct Bundle
{
    Settings settings; // excluded fields hold their defaults
    QList<Scene> scenes;
    QDateTime created;
};

QString serialize(const Settings &settings, const QList<Scene> &scenes, const QDateTime &now);
std::optional<Bundle> parse(const QString &text, QString *error = nullptr);

// `current` with every field a backup carries taken from `backup`. Launch at login is left as is
// too: it lives in the autostart file, which the caller sets up.
Settings restoredSettings(const Settings &current, const Settings &backup);

} // namespace rostrum::backup
