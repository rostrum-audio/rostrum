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
// Extracts only :/opendeck resources. Injected rename allows a real rollback failure test.
Result install(const Target &target, bool pythonAvailable, const Rename &rename = {});

} // namespace rostrum::opendeck
