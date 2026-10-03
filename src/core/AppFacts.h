#pragma once

#include "core/AppClassifier.h"
#include "core/DesktopEntries.h"

#include <QMap>
#include <QString>
#include <QStringList>

#include <functional>

namespace rostrum {

struct ProcessFacts
{
    QString steamAppId; // SteamAppId / SteamGameId / STEAM_COMPAT_APP_ID, never "0"
    QString flatpakId;  // FLATPAK_ID
    QString snapName;   // SNAP_NAME
};

// Reads the environment of `pid`, but only once its executable is confirmed to be `binary`:
// sandboxed apps report a pid from inside their sandbox, which on the host is another process.
ProcessFacts readProcessFacts(qint64 pid, const QString &binary, const QString &procRoot = QStringLiteral("/proc"));

// The game's name from the Steam library ("Hades II" for 1145350), or empty.
// Empty roots = the native and Flatpak Steam installs in the home folder.
QString steamGameName(const QString &appId, const QStringList &steamRoots = {});

// Gathers AppFacts for one playback stream. `isRostrumTarget` says whether a target.object
// value names one of Rostrum's own nodes (exported rules point streams there).
AppFacts collectFacts(const StreamProps &props, const QMap<QString, QString> &nodeProps, qint64 pid,
                      DesktopIndex &desktop, const std::function<bool(const QString &)> &isRostrumTarget,
                      const QString &procRoot = QStringLiteral("/proc"));

// Icons that may show the app, best first: theme icon names or absolute paths. The Steam game's
// own icon, then the app menu entry's, then what the stream reports, then its ids and binary.
QStringList iconCandidates(const AppFacts &facts);

} // namespace rostrum
