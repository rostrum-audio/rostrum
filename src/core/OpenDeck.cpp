#include "core/OpenDeck.h"

#include "core/Paths.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QVersionNumber>

namespace rostrum::opendeck {
namespace {
const QString resource = QStringLiteral(":/opendeck");
QStringList files()
{
    QStringList result;
    QDirIterator it(resource, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        result << it.next().mid(resource.size() + 1);
    return result;
}
QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool identityValid(const QString &root)
{
    const auto manifest = QJsonDocument::fromJson(read(root + QStringLiteral("/manifest.json"))).object();
    return manifest.value(QStringLiteral("Name")).toString() == QLatin1String("Rostrum")
        && manifest.value(QStringLiteral("CodePathLin")).toString() == QLatin1String("plugin.sh")
        && !QVersionNumber::fromString(manifest.value(QStringLiteral("Version")).toString()).isNull()
        && read(root + QStringLiteral("/plugin.sh")).startsWith("#!/bin/sh\n");
}

bool treeComplete(const QString &root)
{
    for (const auto &name : files()) {
        const QFileInfo file(root + QLatin1Char('/') + name);
        if (file.isSymLink() || !file.isFile() || !file.isReadable() || file.size() == 0)
            return false;
    }
    return true;
}

bool valid(const QString &root)
{
    // Bundle / stage gate: identity + every embedded file present (no action count).
    return identityValid(root) && treeComplete(root);
}
QVersionNumber versionOf(const QString &root)
{
    return QVersionNumber::fromString(QJsonDocument::fromJson(read(root + QStringLiteral("/manifest.json")))
                                          .object()
                                          .value(QStringLiteral("Version"))
                                          .toString());
}
bool matches(const QString &root)
{
    for (const auto &name : files()) {
        const QString path = root + QLatin1Char('/') + name;
        if (QFileInfo(path).isSymLink() || read(path) != read(resource + QLatin1Char('/') + name))
            return false;
    }
    return QFileInfo(root + QStringLiteral("/plugin.sh")).isExecutable();
}
} // namespace

QString pluginId()
{
    return QStringLiteral("dev.getrostrum.Rostrum.sdPlugin");
}

QList<Target> findTargets(const QString &home, const QString &configHome, const QString &custom)
{
    if (!custom.isEmpty())
        return {{QStringLiteral("custom"), custom}};
    const QString h = home.isEmpty() ? QDir::homePath() : home;
    const QString config = configHome.isEmpty()
                               ? (home.isEmpty() ? paths::configHome() : h + QStringLiteral("/.config"))
                               : configHome;
    const QList<Target> candidates{
        {QStringLiteral("native"), config + QStringLiteral("/opendeck/plugins")},
        {QStringLiteral("flatpak"),
         h + QStringLiteral("/.var/app/me.amankhanna.opendeck/config/opendeck/plugins")},
    };
    QList<Target> result;
    for (const auto &candidate : candidates) {
        if (QFileInfo(QFileInfo(candidate.plugins).absolutePath()).isDir())
            result << candidate;
    }
    return result;
}

Target selectTarget(const QList<Target> &targets, const QString &preferred)
{
    if (!preferred.isEmpty()) {
        for (const auto &target : targets)
            if (target.id == preferred || target.id == QLatin1String("custom"))
                return target;
        return {};
    }
    return targets.size() == 1 ? targets.first() : Target{};
}

QString bundledVersion()
{
    return versionOf(resource).toString();
}
QString installedVersion(const Target &target)
{
    if (target.plugins.isEmpty())
        return {};
    const QString root = target.plugins + QLatin1Char('/') + pluginId();
    if (QFileInfo(root).isSymLink() || !QFileInfo(root).isDir())
        return {};
    const QVersionNumber version = versionOf(root);
    return version.isNull() ? QString() : version.toString();
}
bool shouldNotify(const NoticeFacts &facts)
{
    return facts.state == State::Newer && facts.targetResolved && facts.python && !facts.bundled.isEmpty() &&
           facts.notified != facts.bundled;
}
State inspect(const Target &target)
{
    if (target.plugins.isEmpty())
        return State::NotFound;
    const QString root = target.plugins + QLatin1Char('/') + pluginId();
    if (QFileInfo(root).isSymLink())
        return State::Incomplete;
    if (!QFileInfo::exists(root))
        return State::Absent;
    if (!identityValid(root))
        return State::Incomplete;
    const auto installed = versionOf(root);
    const auto bundled = versionOf(resource);
    if (installed > bundled)
        return State::InstalledNewer; // before file match / Missing asset checks
    if (matches(root))
        return State::Matches;
    if (bundled > installed)
        return State::Newer;
    return State::Incomplete;
}

Result install(const Target &target, bool pythonAvailable, const Rename &operation)
{
    if (target.plugins.isEmpty())
        return {Error::ChooseTarget};
    if (!QDir::isAbsolutePath(target.plugins) ||
        (target.id == QLatin1String("custom") && !QFileInfo(target.plugins).isDir()))
        return {Error::MissingFolder, target.plugins};
    if (!pythonAvailable)
        return {Error::NoPython};
    if (files().isEmpty() || !valid(resource))
        return {Error::InvalidBundle};
    const QString root = target.plugins + QLatin1Char('/') + pluginId();
    if (QFileInfo(root).isSymLink() || (QFileInfo::exists(root) && !QFileInfo(root).isDir()))
        return {Error::UnsafeTarget, root};
    // Nothing to do, and never a downgrade: the page disables Install for a newer copy, but
    // a stale page or a race must not replace it either.
    if (const State state = inspect(target); state == State::Matches || state == State::InstalledNewer)
        return {};
    // Detection must still hold when clicked, including after an external uninstall.
    if (!QFileInfo(QFileInfo(target.plugins).absolutePath()).isDir() || !QDir().mkpath(target.plugins))
        return {Error::MissingFolder, target.plugins};
    QTemporaryDir transaction(target.plugins + QStringLiteral("/.rostrum-install-XXXXXX"));
    if (!transaction.isValid())
        return {Error::StageFailed, target.plugins};
    const QString stage = transaction.path() + QStringLiteral("/new");
    for (const auto &name : files()) {
        const QString destination = stage + QLatin1Char('/') + name;
        if (!QDir().mkpath(QFileInfo(destination).absolutePath()))
            return {Error::StageFailed, destination};
        QFile out(destination);
        const auto bytes = read(resource + QLatin1Char('/') + name);
        if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.flush())
            return {Error::StageFailed, destination};
        out.close();
        auto permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup |
                           QFileDevice::ReadOther;
        if (name == QLatin1String("plugin.sh"))
            permissions |= QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther;
        if (!QFile::setPermissions(destination, permissions))
            return {Error::StageFailed, destination};
    }
    if (!valid(stage) || !matches(stage))
        return {Error::InvalidBundle, stage};
    const auto rename =
        operation ? operation
                  : Rename([](const QString &from, const QString &to) { return QDir().rename(from, to); });
    const QString previous = transaction.path() + QStringLiteral("/previous");
    const bool hadPrevious = QFileInfo::exists(root);
    if (hadPrevious && !rename(root, previous))
        return {Error::ReplaceFailed, root};
    if (!rename(stage, root)) {
        if (hadPrevious && !rename(previous, root)) {
            transaction.setAutoRemove(false); // Preserve the original even if restoring its name fails.
            return {Error::RestoreFailed, previous};
        }
        return {Error::ReplaceFailed, root};
    }
    return {Error::None, root, true};
}
} // namespace rostrum::opendeck
