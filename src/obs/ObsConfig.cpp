#include "obs/ObsConfig.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <algorithm>
#include <unistd.h>

namespace rostrum::obs {

namespace {

QString homeOr(const QString &home)
{
    return home.isEmpty() ? QDir::homePath() : home;
}

// OBS's ini files are plain key=value lines under [Section] headers.
QString iniValue(const QString &file, const QString &section, const QString &key)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    QString current;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            current = line.mid(1, line.size() - 2);
            continue;
        }
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (current == section && eq > 0 && line.left(eq).trimmed() == key) {
            return line.mid(eq + 1).trimmed();
        }
    }
    return {};
}

QDateTime touched(const QString &configDir)
{
    QDateTime newest;
    for (const char *name : {"user.ini", "global.ini"}) {
        const QFileInfo fi(configDir + QLatin1Char('/') + QLatin1String(name));
        if (fi.exists() && (!newest.isValid() || fi.lastModified() > newest)) {
            newest = fi.lastModified();
        }
    }
    return newest;
}

} // namespace

QList<Install> findInstalls(const QString &home)
{
    const QString h = homeOr(home);
    QString configHome = home.isEmpty() ? qEnvironmentVariable("XDG_CONFIG_HOME") : QString();
    if (configHome.isEmpty()) {
        configHome = h + QLatin1String("/.config");
    }
    const QList<Install> candidates{
        {configHome + QLatin1String("/obs-studio"), QStringLiteral("native")},
        {h + QLatin1String("/.var/app/com.obsproject.Studio/config/obs-studio"), QStringLiteral("flatpak")},
        {h + QLatin1String("/snap/obs-studio/current/.config/obs-studio"), QStringLiteral("snap")},
    };
    QList<Install> found;
    for (const auto &c : candidates) {
        if (touched(c.configDir).isValid()) {
            found << c;
        }
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const Install &a, const Install &b) { return touched(a.configDir) > touched(b.configDir); });
    return found;
}

bool obsBinaryInstalled(const QString &home)
{
    const QString h = homeOr(home);
    if (home.isEmpty() && !QStandardPaths::findExecutable(QStringLiteral("obs")).isEmpty()) {
        return true;
    }
    for (const QString &dir : {QStringLiteral("/var/lib/flatpak/app/com.obsproject.Studio"),
                               h + QLatin1String("/.local/share/flatpak/app/com.obsproject.Studio"),
                               QStringLiteral("/snap/obs-studio")}) {
        if (QFileInfo(dir).isDir()) {
            return true;
        }
    }
    return false;
}

WebSocketConfig readWebSocketConfig(const QString &configDir)
{
    WebSocketConfig cfg;
    QFile f(configDir + QLatin1String("/plugin_config/obs-websocket/config.json"));
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        if (!o.isEmpty()) {
            cfg.found = true;
            cfg.enabled = o.value(QLatin1String("server_enabled")).toBool();
            cfg.port = quint16(o.value(QLatin1String("server_port")).toInt(4455));
            cfg.authRequired = o.value(QLatin1String("auth_required")).toBool(true);
            cfg.password = o.value(QLatin1String("server_password")).toString();
            return cfg;
        }
    }
    const QString global = configDir + QLatin1String("/global.ini");
    const QString enabled = iniValue(global, QStringLiteral("OBSWebSocket"), QStringLiteral("ServerEnabled"));
    if (!enabled.isEmpty()) {
        cfg.found = true;
        cfg.enabled = enabled == QLatin1String("true");
        const int port = iniValue(global, QStringLiteral("OBSWebSocket"), QStringLiteral("ServerPort")).toInt();
        cfg.port = port > 0 ? quint16(port) : 4455;
        cfg.authRequired = iniValue(global, QStringLiteral("OBSWebSocket"), QStringLiteral("AuthRequired")) != QLatin1String("false");
        cfg.password = iniValue(global, QStringLiteral("OBSWebSocket"), QStringLiteral("ServerPassword"));
    }
    return cfg;
}

QString sceneCollectionFile(const QString &configDir)
{
    QString name;
    for (const char *ini : {"user.ini", "global.ini"}) {
        name = iniValue(configDir + QLatin1Char('/') + QLatin1String(ini), QStringLiteral("Basic"),
                        QStringLiteral("SceneCollectionFile"));
        if (!name.isEmpty()) {
            break;
        }
    }
    if (name.isEmpty() || name.contains(QLatin1Char('/'))) {
        return {};
    }
    const QString path = configDir + QLatin1String("/basic/scenes/") + name + QLatin1String(".json");
    return QFileInfo::exists(path) ? path : QString();
}

bool isObsRunning()
{
    const uint uid = ::getuid();
    const QStringList pids = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &pid : pids) {
        if (pid.isEmpty() || !pid.at(0).isDigit()) {
            continue;
        }
        QFile comm(QLatin1String("/proc/") + pid + QLatin1String("/comm"));
        if (!comm.open(QIODevice::ReadOnly) || comm.readAll().trimmed() != "obs") {
            continue;
        }
        if (QFileInfo(QLatin1String("/proc/") + pid).ownerId() == uid) {
            return true;
        }
    }
    return false;
}

} // namespace rostrum::obs
