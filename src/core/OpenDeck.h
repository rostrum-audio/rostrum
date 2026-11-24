#pragma once

#include <QList>
#include <QString>
#include <functional>

namespace rostrum::opendeck {

struct Target
{
    QString id; // native, flatpak or custom
    QString plugins;
};
enum class State
{
    NotFound,
    Absent,
    Matches,
    Newer,
    InstalledNewer,
    Incomplete
};
enum class Error
{
    None,
    ChooseTarget,
    MissingFolder,
    NoPython,
    InvalidBundle,
    UnsafeTarget,
    StageFailed,
    ReplaceFailed,
    RestoreFailed
};
struct Result
{
    Error error = Error::None;
    QString path;
    bool changed = false;
};
using Rename = std::function<bool(const QString &, const QString &)>;

QString pluginId();
// Custom always wins, even when missing. Tests supply home/configHome; defaults use host XDG.
QList<Target> findTargets(const QString &home = {}, const QString &configHome = {},
                          const QString &custom = {});
Target selectTarget(const QList<Target> &targets, const QString &preferred);
State inspect(const Target &target);
// The manifest Version of the bundled plugin ("1.4.0") and of the one installed in target (empty
// when it is absent or unreadable).
QString bundledVersion();
QString installedVersion(const Target &target);

// Whether to tell the user that this Rostrum bundles a newer plugin than the installed one. Only
// for an older installed copy, on a resolved target with Python 3, once per bundled version.
struct NoticeFacts
{
    State state = State::NotFound;
    QString bundled;
    QString installed;
    QString notified; // the bundled version a notice was last shown for
    bool targetResolved = false;
    bool python = false;
};
bool shouldNotify(const NoticeFacts &facts);

// Extracts only :/opendeck resources. Injected rename allows a real rollback failure test.
Result install(const Target &target, bool pythonAvailable, const Rename &rename = {});

} // namespace rostrum::opendeck
