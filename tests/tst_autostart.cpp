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
private:
    QTemporaryDir m_envDir;

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_envDir.isValid());
        const QString home = m_envDir.filePath(QStringLiteral("home"));
        const QString cfg = m_envDir.filePath(QStringLiteral("config"));
        QVERIFY(QDir().mkpath(home));
        QVERIFY(QDir().mkpath(cfg));
        qputenv("HOME", home.toUtf8());
        qputenv("XDG_CONFIG_HOME", cfg.toUtf8());
        qputenv("PATH", "/usr/local/bin:/usr/bin:/bin");
        qunsetenv("APPIMAGE");
        qunsetenv("ARGV0");
    }

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

        // AppImage with spaces is preserved
        QCOMPARE(autostart::launcherPath(QStringLiteral("/tmp/.mount_Rostru123/usr/bin/rostrum"),
                                         QStringLiteral("/home/w/Applications/Rostrum Mix.AppImage")),
                 QStringLiteral("/home/w/Applications/Rostrum Mix.AppImage"));

        // Paths containing literal backslashes are preserved
        QCOMPARE(autostart::launcherPath(QStringLiteral("/home/w/a\\b/rostrum")),
                 QStringLiteral("/home/w/a\\b/rostrum"));

        // When running a local install under ~/.local/bin
        QCOMPARE(autostart::launcherPath(QStringLiteral("/home/w/.local/bin/rostrum")),
                 QStringLiteral("/home/w/.local/bin/rostrum"));

        // Temporary mount path is rejected if APPIMAGE is empty
        const QString mountPath = QStringLiteral("/tmp/.mount_Rostru123/usr/bin/rostrum");
        QVERIFY(!autostart::launcherPath(mountPath).startsWith(QStringLiteral("/tmp/")));
    }

    void unrepresentablePathsAreRejected()
    {
        const QString mountPath = QStringLiteral("/tmp/.mount_Rostru123/usr/bin/rostrum");
        const QStringList unsupported = {
            QStringLiteral("/home/w/100%Mix/Rostrum.AppImage"),
            QStringLiteral("/home/w/100%fMix/rostrum"),
            QStringLiteral("/home/w/100%20Mix/.local/bin/rostrum"),
            QStringLiteral("/home/w/line\nbreak/rostrum"),
            QStringLiteral("/home/w/tab\tchar/rostrum"),
            QStringLiteral("/home/w/cr\rchar/rostrum"),
        };

        for (const QString &badPath : unsupported) {
            QVERIFY2(autostart::launcherPath(badPath).isEmpty(), qPrintable(badPath));
            QVERIFY2(autostart::launcherPath(mountPath, badPath).isEmpty(), qPrintable(badPath));
            QVERIFY2(autostart::execLine(badPath).isEmpty(), qPrintable(badPath));
            QVERIFY2(autostart::entry(autostart::execLine(badPath), QStringLiteral("Mix"), badPath).isEmpty(),
                     qPrintable(badPath));
            QVERIFY2(autostart::entry(QStringLiteral("rostrum --autostart"), QStringLiteral("Mix"), badPath).isEmpty(),
                     qPrintable(badPath));
            QVERIFY2(!autostart::isCurrent(QStringLiteral("[Desktop Entry]\n"), badPath),
                     qPrintable(badPath));
        }
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

        // Paths with spaces are quoted in Exec= and unquoted in TryExec= per the Desktop Entry spec
        const QString spacedLauncher = QStringLiteral("/home/w/Applications/Rostrum Mix.AppImage");
        const QString spacedEntry = autostart::entry(autostart::execLine(spacedLauncher),
                                                     QStringLiteral("Mix"), spacedLauncher);
        QVERIFY(spacedEntry.contains(
            QStringLiteral("\nExec=\"/home/w/Applications/Rostrum Mix.AppImage\" --autostart\n")));
        QVERIFY(spacedEntry.contains(
            QStringLiteral("\nTryExec=/home/w/Applications/Rostrum Mix.AppImage\n")));

        // Literal backslashes are escaped fourfold in Exec= ("..." + string) and doubled in TryExec= (string)
        const QString backslashLauncher = QStringLiteral("/home/w/a\\b\\s/rostrum");
        const QString backslashEntry = autostart::entry(autostart::execLine(backslashLauncher),
                                                        QStringLiteral("Mix"), backslashLauncher);
        QVERIFY(backslashEntry.contains(
            QStringLiteral("\nExec=\"/home/w/a\\\\\\\\b\\\\\\\\s/rostrum\" --autostart\n")));
        QVERIFY(backslashEntry.contains(
            QStringLiteral("\nTryExec=/home/w/a\\\\b\\\\s/rostrum\n")));
    }

    void autostartFileIsNamedForTheAppId()
    {
        const QByteArray prev = qgetenv("XDG_CONFIG_HOME");
        const QString customCfg = m_envDir.filePath(QStringLiteral("rostrum-cfg"));
        qputenv("XDG_CONFIG_HOME", customCfg.toUtf8());
        QCOMPARE(paths::autostartFile(),
                 customCfg + QStringLiteral("/autostart/" ROSTRUM_APP_ID ".desktop"));
        qputenv("XDG_CONFIG_HOME", prev);
    }

    void ensureCurrentMatchesExactKeysAndGroup()
    {
        const QStringList launchers = {
            QStringLiteral("/usr/bin/rostrum"),
            QStringLiteral("/home/w/.local/bin/rostrum"),
            QStringLiteral("/home/w/test dir/.local/bin/rostrum"),
            QStringLiteral("/home/w/Rostrum-0.1.0-x86_64.AppImage"),
            QStringLiteral("/home/w/Applications/Rostrum Mix.AppImage"),
            QStringLiteral("/home/w/Apps (1) & $Tools/Rostrum's \"Mix\".AppImage"),
            QStringLiteral("/home/w/a\\b\\s\\n\\t\\r\\x/rostrum"),
        };
        for (const QString &launcher : launchers) {
            const QString valid = autostart::entry(autostart::execLine(launcher), QStringLiteral("Mix"), launcher);
            QVERIFY2(autostart::isCurrent(valid, launcher), qPrintable(launcher));
        }

        // Unescaped backslash in TryExec= is not current and must be rewritten
        const QString bsLauncher = QStringLiteral("/home/w/a\\b/rostrum");
        QString unescapedTryExec = autostart::entry(autostart::execLine(bsLauncher), QStringLiteral("Mix"), bsLauncher);
        unescapedTryExec.replace(QStringLiteral("TryExec=/home/w/a\\\\b/rostrum\n"),
                                 QStringLiteral("TryExec=/home/w/a\\b/rostrum\n"));
        QVERIFY(!autostart::isCurrent(unescapedTryExec, bsLauncher));

        const QString localBin = QStringLiteral("/home/w/.local/bin/rostrum");
        const QString appImage = QStringLiteral("/home/w/Rostrum-0.1.0-x86_64.AppImage");
        const QString validLocal = autostart::entry(autostart::execLine(localBin), QStringLiteral("Mix"), localBin);

        QVERIFY(!autostart::isCurrent(validLocal, QString()));
        QVERIFY(!autostart::isCurrent(validLocal, appImage));

        // Comments, blank lines, spaces around '=', and a trailing Desktop Action group remain valid
        const QString validWithExtras = QStringLiteral(
            "# Managed autostart entry\n"
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Rostrum\n"
            "Exec = /home/w/.local/bin/rostrum --autostart\n"
            "TryExec = /home/w/.local/bin/rostrum\n"
            "StartupWMClass = rostrum\n"
            "\n"
            "[Desktop Action Other]\n"
            "Name=Other\n"
            "Exec=/usr/bin/true\n");
        QVERIFY(autostart::isCurrent(validWithExtras, localBin));

        // Misleading value prefixes (substring match on TryExec, Exec, or StartupWMClass)
        QString tryExecSuffix = validLocal;
        tryExecSuffix.replace(QStringLiteral("TryExec=/home/w/.local/bin/rostrum\n"),
                              QStringLiteral("TryExec=/home/w/.local/bin/rostrum-old\n"));
        QVERIFY(!autostart::isCurrent(tryExecSuffix, localBin));

        QString execSuffix = validLocal;
        execSuffix.replace(QStringLiteral("Exec=/home/w/.local/bin/rostrum --autostart\n"),
                           QStringLiteral("Exec=/home/w/.local/bin/rostrum --autostart --debug\n"));
        QVERIFY(!autostart::isCurrent(execSuffix, localBin));

        QString wmClassSuffix = validLocal;
        wmClassSuffix.replace(QStringLiteral("StartupWMClass=rostrum\n"),
                              QStringLiteral("StartupWMClass=rostrum-old\n"));
        QVERIFY(!autostart::isCurrent(wmClassSuffix, localBin));

        // TryExec vs Exec: "TryExec=/home/w/.local/bin/rostrum --autostart" contains both
        // "TryExec=/home/w/.local/bin/rostrum" and "Exec=/home/w/.local/bin/rostrum --autostart"
        // as substrings, but neither Exec nor TryExec actually matches.
        const QString tryExecMasqueradingAsExec = QStringLiteral(
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Rostrum\n"
            "Exec=rostrum --autostart\n"
            "TryExec=/home/w/.local/bin/rostrum --autostart\n"
            "StartupWMClass=rostrum\n");
        QVERIFY(!autostart::isCurrent(tryExecMasqueradingAsExec, localBin));

        // Misleading key prefixes, comments, and localized keys
        const QString prefixedKeys = QStringLiteral(
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Rostrum\n"
            "X-Exec=/home/w/.local/bin/rostrum --autostart\n"
            "X-TryExec=/home/w/.local/bin/rostrum\n"
            "X-StartupWMClass=rostrum\n"
            "#Exec=/home/w/.local/bin/rostrum --autostart\n"
            "Exec[en_US]=/home/w/.local/bin/rostrum --autostart\n");
        QVERIFY(!autostart::isCurrent(prefixedKeys, localBin));

        // Duplicate keys in [Desktop Entry] (one stale, one matching)
        const QString duplicateExec = validLocal + QStringLiteral("Exec=rostrum --autostart\n");
        QVERIFY(!autostart::isCurrent(duplicateExec, localBin));

        // Unrelated desktop-entry groups must not satisfy [Desktop Entry] checks
        const QString wrongGroupAfter = QStringLiteral(
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Rostrum\n"
            "Exec=rostrum --autostart\n"
            "\n"
            "[Desktop Action Autostart]\n"
            "Exec=/home/w/.local/bin/rostrum --autostart\n"
            "TryExec=/home/w/.local/bin/rostrum\n"
            "StartupWMClass=rostrum\n");
        QVERIFY(!autostart::isCurrent(wrongGroupAfter, localBin));

        const QString wrongGroupBefore = QStringLiteral(
            "[X-Rostrum]\n"
            "Exec=/home/w/.local/bin/rostrum --autostart\n"
            "TryExec=/home/w/.local/bin/rostrum\n"
            "StartupWMClass=rostrum\n"
            "\n"
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Rostrum\n"
            "Exec=/home/w/.local/bin/rostrum --autostart\n"
            "TryExec=/home/w/.local/bin/rostrum\n"
            "StartupWMClass=rostrum\n");
        QVERIFY(!autostart::isCurrent(wrongGroupBefore, localBin));
    }

    void entryValidates()
    {
        const QString validator = QStandardPaths::findExecutable(QStringLiteral("desktop-file-validate"));
        if (validator.isEmpty()) {
            QSKIP("desktop-file-validate is not installed");
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QList<QPair<QString, QString>> cases = {
            {QStringLiteral("appimage.desktop"),
             QStringLiteral("/home/w/Rostrum-0.1.0-x86_64.AppImage")},
            {QStringLiteral("appimage-spaces.desktop"),
             QStringLiteral("/home/w/Applications/Rostrum Mix.AppImage")},
            {QStringLiteral("local.desktop"),
             QStringLiteral("/home/w/.local/bin/rostrum")},
            {QStringLiteral("local-spaces.desktop"),
             QStringLiteral("/home/w/test dir/.local/bin/rostrum")},
            {QStringLiteral("special-chars.desktop"),
             QStringLiteral("/home/w/Apps (1) & $Tools/Rostrum's \"Mix\".AppImage")},
            {QStringLiteral("backslash.desktop"),
             QStringLiteral("/home/w/a\\b\\s\\n\\t\\r\\x/rostrum")},
        };

        for (const auto &[fileName, launcherPath] : cases) {
            const QString desktopPath = dir.filePath(fileName);
            QFile f(desktopPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(autostart::entry(autostart::execLine(launcherPath),
                                     QStringLiteral("A stream mix console for Linux"),
                                     launcherPath)
                        .toUtf8());
            f.close();
            QProcess p;
            p.start(validator, {desktopPath});
            QVERIFY(p.waitForFinished());
            QVERIFY2(p.exitCode() == 0, p.readAllStandardOutput().constData());
        }
    }

    void glibDesktopAppInfoResolvesSupportedLaunchers()
    {
        const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
        if (python.isEmpty()) {
            QSKIP("python3 is not installed");
        }
        {
            QProcess probe;
            probe.start(python, {QStringLiteral("-c"),
                                 QStringLiteral("import gi; gi.require_version('GioUnix', '2.0'); "
                                                "from gi.repository import GLib, GioUnix")});
            QVERIFY(probe.waitForFinished());
            if (probe.exitCode() != 0) {
                QSKIP("PyGObject (GLib/GioUnix) is not installed");
            }
        }

        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QStringList relPaths = {
            QStringLiteral("local/bin/rostrum"),
            QStringLiteral("Downloads/Rostrum-0.1.0-x86_64.AppImage"),
            QStringLiteral("Applications/Rostrum Mix.AppImage"),
            QStringLiteral("test dir/.local/bin/rostrum"),
            QStringLiteral("Apps (1) & $Tools/Rostrum's \"Mix\".AppImage"),
            QStringLiteral("a\\b/rostrum"),
            QStringLiteral("a\\s\\n\\t\\r\\x/Rostrum.AppImage"),
        };

        const QString script = QStringLiteral(
            "import sys\n"
            "import gi\n"
            "gi.require_version('GioUnix', '2.0')\n"
            "from gi.repository import GLib, GioUnix\n"
            "desktop_path, expected_exe = sys.argv[1], sys.argv[2]\n"
            "kf = GLib.KeyFile()\n"
            "kf.load_from_file(desktop_path, GLib.KeyFileFlags.NONE)\n"
            "try_exec = kf.get_string('Desktop Entry', 'TryExec')\n"
            "assert try_exec == expected_exe, f'TryExec mismatch: {try_exec!r} != {expected_exe!r}'\n"
            "exec_str = kf.get_string('Desktop Entry', 'Exec')\n"
            "ok, argv = GLib.shell_parse_argv(exec_str)\n"
            "assert ok and argv == [expected_exe, '--autostart'], f'Exec argv mismatch: {argv!r}'\n"
            "info = GioUnix.DesktopAppInfo.new_from_filename(desktop_path)\n"
            "assert info is not None, 'DesktopAppInfo failed to resolve executable'\n");

        int index = 0;
        for (const QString &rel : relPaths) {
            const QString exePath = dir.filePath(rel);
            QVERIFY(QDir().mkpath(QFileInfo(exePath).absolutePath()));
            QFile exe(exePath);
            QVERIFY(exe.open(QIODevice::WriteOnly | QIODevice::Truncate));
            exe.write("#!/bin/sh\nexit 0\n");
            exe.close();
            QVERIFY(exe.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                       QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                       QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                       QFileDevice::ExeOther));

            const QString desktopPath = dir.filePath(QStringLiteral("case-%1.desktop").arg(index++));
            const QString content = autostart::entry(
                autostart::execLine(exePath), QStringLiteral("A stream mix console for Linux"), exePath);
            QVERIFY2(!content.isEmpty(), qPrintable(exePath));

            QFile df(desktopPath);
            QVERIFY(df.open(QIODevice::WriteOnly | QIODevice::Truncate));
            df.write(content.toUtf8());
            df.close();

            QProcess p;
            p.start(python, {QStringLiteral("-c"), script, desktopPath, exePath});
            QVERIFY(p.waitForFinished());
            QVERIFY2(p.exitCode() == 0,
                     (p.readAllStandardOutput() + p.readAllStandardError()).constData());
        }

        // Negative controls proving GDesktopAppInfo checks on-disk executable resolution
        // and rejects unescaped TryExec backslashes and doubled-percent Exec paths.
        const QString pctExePath = dir.filePath(QStringLiteral("100%Mix/rostrum"));
        QVERIFY(QDir().mkpath(QFileInfo(pctExePath).absolutePath()));
        QFile pctExe(pctExePath);
        QVERIFY(pctExe.open(QIODevice::WriteOnly | QIODevice::Truncate));
        pctExe.write("#!/bin/sh\nexit 0\n");
        pctExe.close();
        QVERIFY(pctExe.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                      QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                      QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                      QFileDevice::ExeOther));

        QString doubledPctExec = pctExePath;
        doubledPctExec.replace(QLatin1String("%"), QLatin1String("%%"));
        const QString pctDesktopPath = dir.filePath(QStringLiteral("pct-doubled.desktop"));
        {
            QFile df(pctDesktopPath);
            QVERIFY(df.open(QIODevice::WriteOnly | QIODevice::Truncate));
            df.write(QStringLiteral("[Desktop Entry]\n"
                                    "Type=Application\n"
                                    "Name=Rostrum\n"
                                    "Exec=\"%1\" --autostart\n"
                                    "TryExec=%2\n")
                         .arg(doubledPctExec, pctExePath)
                         .toUtf8());
        }

        const QString bsExePath = dir.filePath(QStringLiteral("a\\b/rostrum"));
        const QString rawBsDesktopPath = dir.filePath(QStringLiteral("bs-unescaped-tryexec.desktop"));
        {
            QString rawBsContent = autostart::entry(
                autostart::execLine(bsExePath), QStringLiteral("Mix"), bsExePath);
            QString escapedBs = bsExePath;
            escapedBs.replace(QLatin1String("\\"), QLatin1String("\\\\"));
            rawBsContent.replace(QStringLiteral("TryExec=") + escapedBs + QLatin1Char('\n'),
                                 QStringLiteral("TryExec=") + bsExePath + QLatin1Char('\n'));
            QFile df(rawBsDesktopPath);
            QVERIFY(df.open(QIODevice::WriteOnly | QIODevice::Truncate));
            df.write(rawBsContent.toUtf8());
        }

        const QString negativeScript = QStringLiteral(
            "import sys\n"
            "import gi\n"
            "gi.require_version('GioUnix', '2.0')\n"
            "from gi.repository import GioUnix\n"
            "for path in sys.argv[1:]:\n"
            "    loaded = False\n"
            "    try:\n"
            "        loaded = GioUnix.DesktopAppInfo.new_from_filename(path) is not None\n"
            "    except Exception:\n"
            "        loaded = False\n"
            "    assert not loaded, f'Expected DesktopAppInfo to reject {path}'\n");

        QProcess neg;
        neg.start(python, {QStringLiteral("-c"), negativeScript, pctDesktopPath, rawBsDesktopPath});
        QVERIFY(neg.waitForFinished());
        QVERIFY2(neg.exitCode() == 0,
                 (neg.readAllStandardOutput() + neg.readAllStandardError()).constData());
    }
};

QTEST_GUILESS_MAIN(TestAutostart)
#include "tst_autostart.moc"
