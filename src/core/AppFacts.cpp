#include "core/AppFacts.h"

#include "core/AppClassifier.h"
#include "core/JavaApps.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

#include <unistd.h>

namespace rostrum {

namespace {

QString readSmall(const QString &path, qint64 limit = 256 * 1024)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(f.read(limit));
}

QString statusField(const QString &status, QLatin1String name)
{
    for (const QStringView line : QStringView(status).split(QLatin1Char('\n'))) {
        if (line.startsWith(name) && line.mid(name.size()).startsWith(QLatin1Char(':'))) {
            return line.mid(name.size() + 1).trimmed().toString();
        }
    }
    return {};
}

bool isFlatpakApp(const QString &dir, const QString &appId)
{
    bool inApplication = false;
    for (const QStringView raw : QStringView(readSmall(dir + QStringLiteral("/root/.flatpak-info"), 16 * 1024))
                                     .split(QLatin1Char('\n'))) {
        const QStringView line = raw.trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            inApplication = line == QLatin1String("[Application]");
        } else if (inApplication && line.startsWith(QLatin1String("name="))) {
            return line.mid(5).trimmed() == appId;
        }
    }
    // Without access to the sandbox's root, the systemd scope Flatpak starts each app in.
    return readSmall(dir + QStringLiteral("/cgroup"), 16 * 1024)
        .contains(QStringLiteral("/app-flatpak-%1-").arg(appId));
}

// The host pid of the process a Flatpak sandbox calls `sandboxPid`: this user's process whose
// innermost pid is `sandboxPid` and that runs in that Flatpak. 0 for none, or for more than one
// (two sandboxes of one app can each have the same pid).
qint64 hostPidOf(qint64 sandboxPid, const QString &appId, const QString &procRoot)
{
    static const QRegularExpression digits(QStringLiteral("^\\d+$"));
    const QString uid = QString::number(::getuid());
    qint64 found = 0;
    for (const QString &name : QDir(procRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!digits.match(name).hasMatch()) {
            continue;
        }
        const QString dir = procRoot + QLatin1Char('/') + name;
        const QString status = readSmall(dir + QStringLiteral("/status"), 16 * 1024);
        const QStringList nsPids = statusField(status, QLatin1String("NSpid")).split(QLatin1Char('\t'), Qt::SkipEmptyParts);
        if (nsPids.size() < 2 || nsPids.last().toLongLong() != sandboxPid) {
            continue;
        }
        if (statusField(status, QLatin1String("Uid")).section(QLatin1Char('\t'), 0, 0) != uid ||
            !isFlatpakApp(dir, appId)) {
            continue;
        }
        if (found) {
            return 0;
        }
        found = name.toLongLong();
    }
    return found;
}

} // namespace

ProcessFacts readProcessFacts(qint64 pid, const QString &binary, const QString &procRoot, const QString &flatpakAppId)
{
    ProcessFacts out;
    const QString want = cleanBinary(binary);
    if (pid <= 0 || want.isEmpty()) {
        return out;
    }
    if (!flatpakAppId.isEmpty()) {
        pid = hostPidOf(pid, flatpakAppId, procRoot);
        if (pid <= 0) {
            return out;
        }
    }
    const QString dir = QStringLiteral("%1/%2").arg(procRoot).arg(pid);
    const QString exe = cleanBinary(QFileInfo(dir + QStringLiteral("/exe")).symLinkTarget());
    // comm is the kernel's 15-character copy of the name; it covers a missing exe link.
    const QString comm = readSmall(dir + QStringLiteral("/comm")).trimmed();
    const bool same = exe.compare(want, Qt::CaseInsensitive) == 0 ||
                      (!comm.isEmpty() && comm.compare(want.left(15), Qt::CaseInsensitive) == 0);
    if (!same) {
        return out;
    }
    const QString environ = readSmall(dir + QStringLiteral("/environ"));
    for (const QStringView var : QStringView(environ).split(QChar(u'\0'), Qt::SkipEmptyParts)) {
        const auto eq = var.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            continue;
        }
        const QStringView key = var.left(eq);
        const QString value = var.mid(eq + 1).trimmed().toString();
        if ((key == QLatin1String("SteamAppId") || key == QLatin1String("SteamGameId") ||
             key == QLatin1String("STEAM_COMPAT_APP_ID")) &&
            out.steamAppId.isEmpty() && !value.isEmpty() && value != QLatin1String("0")) {
            out.steamAppId = value;
        } else if (key == QLatin1String("FLATPAK_ID")) {
            out.flatpakId = value;
        } else if (key == QLatin1String("SNAP_NAME")) {
            out.snapName = value;
        }
    }
    out.commandLine = readSmall(dir + QStringLiteral("/cmdline")).split(QChar(u'\0'), Qt::SkipEmptyParts);
    return out;
}

QString steamGameName(const QString &appId, const QStringList &steamRoots)
{
    static QHash<QString, QString> cache;
    static const QRegularExpression digits(QStringLiteral("^\\d+$"));
    if (!digits.match(appId).hasMatch()) {
        return {};
    }
    const bool defaultRoots = steamRoots.isEmpty();
    if (defaultRoots) {
        if (auto it = cache.constFind(appId); it != cache.cend()) {
            return it.value();
        }
    }
    QStringList roots = steamRoots;
    if (defaultRoots) {
        const QString home = QDir::homePath();
        roots = {home + QStringLiteral("/.local/share/Steam"), home + QStringLiteral("/.steam/steam"),
                 home + QStringLiteral("/.var/app/com.valvesoftware.Steam/.local/share/Steam")};
    }
    static const QRegularExpression pathRe(QStringLiteral("\"path\"\\s+\"([^\"]+)\""));
    static const QRegularExpression nameRe(QStringLiteral("\"name\"\\s+\"([^\"]+)\""));
    QStringList libraries;
    for (const QString &root : std::as_const(roots)) {
        libraries << root;
        const QString vdf = readSmall(root + QStringLiteral("/steamapps/libraryfolders.vdf"));
        for (auto it = pathRe.globalMatch(vdf); it.hasNext();) {
            libraries << it.next().captured(1).replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
        }
    }
    QString name;
    for (const QString &lib : std::as_const(libraries)) {
        const QString acf = readSmall(QStringLiteral("%1/steamapps/appmanifest_%2.acf").arg(lib, appId));
        if (const auto m = nameRe.match(acf); m.hasMatch()) {
            name = m.captured(1).trimmed();
            break;
        }
    }
    if (defaultRoots) {
        cache.insert(appId, name);
    }
    return name;
}

AppFacts collectFacts(const StreamProps &props, const QMap<QString, QString> &nodeProps, qint64 pid,
                      DesktopIndex &desktop, const std::function<bool(const QString &)> &isPlainTarget,
                      const QString &procRoot)
{
    AppFacts f;
    f.props = props;
    f.mediaRole = nodeProps.value(QStringLiteral("media.role"));
    f.iconName = nodeProps.value(QStringLiteral("application.icon-name"));
    f.dontMove = nodeProps.value(QStringLiteral("node.dont-move")) == QLatin1String("true");
    for (const char *key : {"target.object", "node.target"}) {
        const QString target = nodeProps.value(QString::fromLatin1(key));
        if (!target.isEmpty() && target != QLatin1String("-1") && !isPlainTarget(target)) {
            f.ownOutputChoice = true;
        }
    }

    f.appId = nodeProps.value(QStringLiteral("pipewire.access.portal.app_id"));
    const ProcessFacts proc = readProcessFacts(pid, props.binary, procRoot, f.appId);
    f.steamAppId = proc.steamAppId;
    if (f.appId.isEmpty()) {
        f.appId = !proc.flatpakId.isEmpty() ? proc.flatpakId
                : !proc.snapName.isEmpty()  ? proc.snapName
                                            : nodeProps.value(QStringLiteral("application.id"));
    }
    if (!f.steamAppId.isEmpty()) {
        f.steamGameName = steamGameName(f.steamAppId);
    }

    // A generic name ("Chromium" from any Electron app) would find the wrong entry, and so would
    // "java", the runtime every Java app runs in (and the name OpenAL Soft gives its streams).
    const QString name = isGenericAppName(props.appName) || isJavaBinary(props.appName) ? QString() : props.appName;
    const QString binary = isJavaBinary(props.binary) ? QString() : cleanBinary(props.binary);
    if (const auto entry = desktop.find({f.appId, binary, name, f.iconName})) {
        f.desktopId = entry->id;
        f.desktopIcon = entry->icon;
        f.desktopCategories = entry->categories;
    }
    return f;
}

QStringList iconCandidates(const AppFacts &f)
{
    QStringList out;
    // Choosing an output prevents routing, but does not turn an app into an audio tool.
    AppFacts iconFacts = f;
    iconFacts.dontMove = false;
    iconFacts.ownOutputChoice = false;
    if (classify(iconFacts).excluded) {
        return out;
    }
    if (!f.steamAppId.isEmpty()) {
        out << QStringLiteral("steam_icon_") + f.steamAppId;
    }
    out << f.desktopIcon << f.iconName;
    out.removeAll(QString());
    out.removeDuplicates();
    return out;
}

} // namespace rostrum
