#include "core/Paths.h"

#include <QDir>

namespace rostrum::paths {

namespace {

QString fromEnv(const char *var, const QString &fallbackUnderHome)
{
    const QString value = qEnvironmentVariable(var);
    if (!value.isEmpty() && QDir::isAbsolutePath(value)) {
        return QDir::cleanPath(value);
    }
    return QDir::homePath() + QLatin1Char('/') + fallbackUnderHome;
}

} // namespace

QString configHome() { return fromEnv("XDG_CONFIG_HOME", QStringLiteral(".config")); }
QString stateHome() { return fromEnv("XDG_STATE_HOME", QStringLiteral(".local/state")); }

QString configDir() { return configHome() + QStringLiteral("/rostrum"); }
QString scenesDir() { return configDir() + QStringLiteral("/scenes"); }
QString settingsFile() { return configDir() + QStringLiteral("/settings.toml"); }
QString stateDir() { return stateHome() + QStringLiteral("/rostrum"); }
QString logFile() { return stateDir() + QStringLiteral("/rostrum.log"); }
QString crashDir() { return stateDir() + QStringLiteral("/crashes"); }
QString sentryDir() { return stateDir() + QStringLiteral("/sentry"); }

QString autostartFile()
{
    return configHome() + QStringLiteral("/autostart/") + QStringLiteral(ROSTRUM_APP_ID) +
           QStringLiteral(".desktop");
}

QString pipewirePulseFragment()
{
    return configHome() + QStringLiteral("/pipewire/pipewire-pulse.conf.d/50-rostrum.conf");
}

QString pipewireClientFragment()
{
    return configHome() + QStringLiteral("/pipewire/client.conf.d/50-rostrum.conf");
}

} // namespace rostrum::paths
