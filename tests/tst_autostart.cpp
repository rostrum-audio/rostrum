#include "core/Autostart.h"
#include "core/Paths.h"

#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace rostrum;

class TestAutostart : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void plainPathIsUnquoted()
    {
        QCOMPARE(autostart::execQuote(QStringLiteral("/usr/bin/rostrum")),
                 QStringLiteral("/usr/bin/rostrum"));
        QCOMPARE(autostart::execQuote(QStringLiteral("rostrum")), QStringLiteral("rostrum"));
    }

    void reservedCharactersAreQuoted()
    {
        QCOMPARE(autostart::execQuote(QStringLiteral("/home/w/New Folder/rostrum")),
                 QStringLiteral("\"/home/w/New Folder/rostrum\""));
        // " becomes \" (Exec rule), then every \ is doubled (string rule).
        QCOMPARE(autostart::execQuote(QStringLiteral("/a\"b")), QStringLiteral("\"/a\\\\\"b\""));
        QCOMPARE(autostart::execQuote(QStringLiteral("/a$b")), QStringLiteral("\"/a\\\\$b\""));
        QCOMPARE(autostart::execQuote(QString()), QStringLiteral("\"\""));
    }

    void execLineForAppImage()
    {
        const QString appImage = QStringLiteral("/home/w/Downloads/Rostrum-0.1.0-x86_64.AppImage");
        QCOMPARE(autostart::execLine(appImage),
                 QStringLiteral("\"/home/w/Downloads/Rostrum-0.1.0-x86_64.AppImage\" --autostart"));

        const QString appImageWithSpaces = QStringLiteral("/home/w/Applications/Rostrum Mix.AppImage");
        QCOMPARE(autostart::execLine(appImageWithSpaces),
                 QStringLiteral("\"/home/w/Applications/Rostrum Mix.AppImage\" --autostart"));
    }

    void execLineForLocalInstall()
    {
        const QString localBin = QStringLiteral("/home/w/.local/bin/rostrum");
        QCOMPARE(autostart::execLine(localBin),
                 QStringLiteral("/home/w/.local/bin/rostrum --autostart"));

        const QString localBinWithSpaces = QStringLiteral("/home/w/test dir/.local/bin/rostrum");
        QCOMPARE(autostart::execLine(localBinWithSpaces),
                 QStringLiteral("\"/home/w/test dir/.local/bin/rostrum\" --autostart"));
    }

    void launcherPathResolution()
    {
        // When running an AppImage, APPIMAGE environment variable is preserved
        QCOMPARE(autostart::launcherPath(QStringLiteral("/tmp/.mount_Rostru123/usr/bin/rostrum"),
                                         QStringLiteral("/home/w/Rostrum-0.1.0-x86_64.AppImage")),
                 QStringLiteral("/home/w/Rostrum-0.1.0-x86_64.AppImage"));

        // When running a local install under ~/.local/bin
        QCOMPARE(autostart::launcherPath(QStringLiteral("/home/w/.local/bin/rostrum")),
                 QStringLiteral("/home/w/.local/bin/rostrum"));

        // Temporary mount path is rejected if APPIMAGE is empty
        const QString mountPath = QStringLiteral("/tmp/.mount_Rostru123/usr/bin/rostrum");
        QVERIFY(!autostart::launcherPath(mountPath).startsWith(QStringLiteral("/tmp/")));
    }

    void entryUsesAppIdAndExec()
    {
        const QString e = autostart::entry(QStringLiteral("rostrum --autostart"), QStringLiteral("Mix"),
                                           QStringLiteral("/usr/bin/rostrum"));
        QVERIFY(e.startsWith(QStringLiteral("[Desktop Entry]\n")));
        QVERIFY(e.contains(QStringLiteral("\nType=Application\n")));
        QVERIFY(e.contains(QStringLiteral("\nName=Rostrum\n")));
        QVERIFY(e.contains(QStringLiteral("\nExec=rostrum --autostart\n")));
        QVERIFY(e.contains(QStringLiteral("\nTryExec=/usr/bin/rostrum\n")));
        QVERIFY(e.contains(QStringLiteral("\nIcon=" ROSTRUM_APP_ID "\n")));
        QVERIFY(e.contains(QStringLiteral("\nTerminal=false\n")));
        QVERIFY(e.contains(QStringLiteral("\nStartupWMClass=rostrum\n")));
        QVERIFY(e.contains(QStringLiteral("\nX-GNOME-Autostart-enabled=true\n")));
        QVERIFY(!e.contains(QStringLiteral("Hidden=true")));
        QVERIFY(!e.contains(QStringLiteral("OnlyShowIn")));
    }

    void autostartFileIsNamedForTheAppId()
    {
        qputenv("XDG_CONFIG_HOME", "/tmp/rostrum-cfg");
        QCOMPARE(paths::autostartFile(),
                 QStringLiteral("/tmp/rostrum-cfg/autostart/" ROSTRUM_APP_ID ".desktop"));
    }

    void entryValidates()
    {
        const QString validator = QStandardPaths::findExecutable(QStringLiteral("desktop-file-validate"));
        if (validator.isEmpty()) {
            QSKIP("desktop-file-validate is not installed");
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        // Test AppImage entry validation
        const QString appImagePath = QStringLiteral("/home/w/Rostrum-0.1.0-x86_64.AppImage");
        const QString appImageDesktop = dir.filePath(QStringLiteral("appimage.desktop"));
        QFile f1(appImageDesktop);
        QVERIFY(f1.open(QIODevice::WriteOnly));
        f1.write(autostart::entry(autostart::execLine(appImagePath),
                                  QStringLiteral("A stream mix console for Linux"),
                                  appImagePath).toUtf8());
        f1.close();
        QProcess p1;
        p1.start(validator, {appImageDesktop});
        QVERIFY(p1.waitForFinished());
        QVERIFY2(p1.exitCode() == 0, p1.readAllStandardOutput().constData());

        // Test ~/.local install entry validation
        const QString localBinPath = QStringLiteral("/home/w/.local/bin/rostrum");
        const QString localDesktop = dir.filePath(QStringLiteral("local.desktop"));
        QFile f2(localDesktop);
        QVERIFY(f2.open(QIODevice::WriteOnly));
        f2.write(autostart::entry(autostart::execLine(localBinPath),
                                  QStringLiteral("A stream mix console for Linux"),
                                  localBinPath).toUtf8());
        f2.close();
        QProcess p2;
        p2.start(validator, {localDesktop});
        QVERIFY(p2.waitForFinished());
        QVERIFY2(p2.exitCode() == 0, p2.readAllStandardOutput().constData());
    }
};

QTEST_GUILESS_MAIN(TestAutostart)
#include "tst_autostart.moc"
