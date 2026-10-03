#include "core/Requirements.h"

#include <QTest>

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
        const QString hint = installHint();
        QVERIFY(hint.contains(QStringLiteral("pipewire-pulse")));
        QVERIFY(hint.contains(QStringLiteral("wireplumber")));
        QVERIFY(hint.contains(QStringLiteral("dnf")));
        QVERIFY(hint.contains(QStringLiteral("pacman")));
    }
};

QTEST_GUILESS_MAIN(TestRequirements)
#include "tst_requirements.moc"
