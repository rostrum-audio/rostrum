#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace rostrum {

// The parts of a .desktop file that say what an app is.
struct DesktopEntry
{
    QString id;   // "org.kde.elisa" for org.kde.elisa.desktop
    QString name; // Name=
    QStringList categories;
    QStringList tokens; // lower-case ways a running app can point at this entry
};

// Parses the [Desktop Entry] group. Returns nothing for hidden entries and non-applications.
std::optional<DesktopEntry> parseDesktopEntry(const QString &text, const QString &id);

// The program a desktop Exec= line starts, seen through env, flatpak run and shell wrappers:
// "env FOO=1 /usr/bin/spotify %U" -> "spotify"; "flatpak run com.spotify.Client" -> "com.spotify.client".
QStringList execTokens(const QString &exec);

// Every installed .desktop file, looked up by the ids, binaries and window classes PipeWire
// reports. Built on first use; a miss rescans at most once a minute, for apps installed since.
class DesktopIndex
{
public:
    // Empty = the XDG data dirs plus the Flatpak and Snap export dirs.
    explicit DesktopIndex(const QStringList &dataDirs = {});

    // First entry that any candidate (any case) points at.
    std::optional<DesktopEntry> find(const QStringList &candidates);

    static QStringList defaultDataDirs();

private:
    void scan();

    QStringList m_dirs;
    QList<DesktopEntry> m_entries;
    QHash<QString, int> m_byToken;
    QElapsedTimer m_scanned;
};

} // namespace rostrum
