#pragma once

#include "core/Paths.h"

#include <QDir>
#include <QTemporaryDir>

namespace rostrum::test {

// One fresh folder standing in for HOME and the XDG base directories for a whole test run. App
// objects save settings, scenes and OBS state there, and read OBS's own config from there, never
// the person's real ones. Make it a member of the test class, then check leak() first thing in
// initTestCase(), before any app object exists.
class IsolatedHome
{
public:
    IsolatedHome()
    {
        if (!m_dir.isValid())
            return;
        const QByteArray root = m_dir.path().toUtf8();
        qputenv("HOME", root);
        qputenv("XDG_CONFIG_HOME", root + "/.config");
        qputenv("XDG_STATE_HOME", root + "/.local/state");
        qputenv("XDG_DATA_HOME", root + "/.local/share");
        qputenv("XDG_CACHE_HOME", root + "/.cache");
        // Nor the person's audio session.
        qputenv("PIPEWIRE_REMOTE", "rostrum-test-none");
    }
    QString path() const { return m_dir.path(); }

    // The first place app code would save to that lies outside this folder, or empty when none.
    QString leak() const
    {
        if (!m_dir.isValid())
            return QStringLiteral("(no temporary folder)");
        const QString root = m_dir.path() + QLatin1Char('/');
        for (const QString &place : {paths::settingsFile(), paths::scenesDir(), paths::stateDir(), paths::dspDir(),
                                     paths::pipewirePulseFragment(), QDir::homePath() + QLatin1Char('/')}) {
            if (!place.startsWith(root))
                return place;
        }
        return {};
    }

private:
    QTemporaryDir m_dir;
};

} // namespace rostrum::test
