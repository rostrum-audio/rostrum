#include "core/Paths.h"

#include <QDir>
#include <QTest>

using namespace rostrum;

class TestPaths : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void honoursXdgVariables()
    {
        qputenv("XDG_CONFIG_HOME", "/tmp/rostrum-cfg");
        qputenv("XDG_STATE_HOME", "/tmp/rostrum-state");
        QCOMPARE(paths::configDir(), QStringLiteral("/tmp/rostrum-cfg/rostrum"));
        QCOMPARE(paths::scenesDir(), QStringLiteral("/tmp/rostrum-cfg/rostrum/scenes"));
        QCOMPARE(paths::logFile(), QStringLiteral("/tmp/rostrum-state/rostrum/rostrum.log"));
        QCOMPARE(paths::pipewirePulseFragment(),
                 QStringLiteral("/tmp/rostrum-cfg/pipewire/pipewire-pulse.conf.d/50-rostrum.conf"));
        QCOMPARE(paths::pipewireClientFragment(),
                 QStringLiteral("/tmp/rostrum-cfg/pipewire/client.conf.d/50-rostrum.conf"));
    }

    void ignoresRelativeXdgValues()
    {
        qputenv("XDG_CONFIG_HOME", "relative/path");
        QVERIFY(paths::configHome().startsWith(QDir::homePath()));
    }
};

QTEST_GUILESS_MAIN(TestPaths)
#include "tst_paths.moc"
