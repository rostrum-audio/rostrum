#include "core/Requirements.h"

#include <QTest>

#include <cerrno>

using namespace rostrum::requirements;

class TestRequirements : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void parses()
    {
        auto v = parseVersion(QStringLiteral("Linked with libpipewire 1.6.2"));
        QVERIFY(v.valid);
        QCOMPARE(v.major, 1);
        QCOMPARE(v.minor, 6);
        QCOMPARE(v.micro, 2);
        QVERIFY(!parseVersion(QStringLiteral("garbage")).valid);
    }

    void minimums()
    {
        QVERIFY(pipewireOk(QStringLiteral("1.0.0")));
        QVERIFY(pipewireOk(QStringLiteral("1.6.2")));
        QVERIFY(!pipewireOk(QStringLiteral("0.3.85")));
        QVERIFY(!pipewireOk(QString()));
        QVERIFY(wireplumberOk(QStringLiteral("0.5.13")));
        QVERIFY(!wireplumberOk(QStringLiteral("0.4.17")));
    }

    void hintNamesPackages()
    {
        const auto cmds = installCommands();
        QCOMPARE(cmds.size(), 3);
        QVERIFY(cmds.at(0).command.startsWith(QStringLiteral("sudo apt ")));
        QVERIFY(cmds.at(1).command.contains(QStringLiteral("pipewire-pulseaudio")));
        QVERIFY(cmds.at(2).command.startsWith(QStringLiteral("sudo pacman ")));
        for (const auto &c : cmds) {
            QVERIFY(c.command.contains(QStringLiteral("wireplumber")));
            QVERIFY(!c.command.contains(QLatin1Char('\n')));
        }
        QVERIFY(startCommand().startsWith(QStringLiteral("systemctl --user ")));
        QVERIFY(!startCommand().contains(QStringLiteral("sudo")));
        QVERIFY(installHint().contains(startCommand()));
        QVERIFY(installHint().contains(cmds.at(1).command));
    }

    void connectProblems()
    {
        QCOMPARE(connectProblem(EHOSTDOWN), ConnectProblem::NotRunning);
        QCOMPARE(connectProblem(ENOENT), ConnectProblem::NotRunning);
        QCOMPARE(connectProblem(ECONNREFUSED), ConnectProblem::NotAnswering);
        QCOMPARE(connectProblem(EACCES), ConnectProblem::NotAllowed);
        QCOMPARE(connectProblem(EIO), ConnectProblem::Other);
        QCOMPARE(connectProblem(0), ConnectProblem::Other);
    }
};

QTEST_GUILESS_MAIN(TestRequirements)
#include "tst_requirements.moc"
