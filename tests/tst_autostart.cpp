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

    void entryUsesAppIdAndExec()
    {
        const QString e = autostart::entry(QStringLiteral("rostrum --autostart"), QStringLiteral("Mix"));
        QVERIFY(e.startsWith(QStringLiteral("[Desktop Entry]\n")));
        QVERIFY(e.contains(QStringLiteral("\nExec=rostrum --autostart\n")));
        QVERIFY(e.contains(QStringLiteral("\nIcon=" ROSTRUM_APP_ID "\n")));
        QVERIFY(e.contains(QStringLiteral("\nComment=Mix\n")));
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
        const QString path = dir.filePath(QStringLiteral(ROSTRUM_APP_ID ".desktop"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        const QString exec = autostart::execQuote(QStringLiteral("/home/w/New Folder/build/rostrum")) +
                             QStringLiteral(" --autostart");
        f.write(autostart::entry(exec, QStringLiteral("A stream mix console for Linux")).toUtf8());
        f.close();
        QProcess p;
        p.start(validator, {path});
        QVERIFY(p.waitForFinished());
        QVERIFY2(p.exitCode() == 0, p.readAllStandardOutput().constData());
    }
};

QTEST_GUILESS_MAIN(TestAutostart)
#include "tst_autostart.moc"
