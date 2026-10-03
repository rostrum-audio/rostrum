#include "core/AppIdentity.h"

#include <QTest>

using namespace rostrum;

class TestAppIdentity : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void namedApp()
    {
        const auto id = identify({QStringLiteral("Firefox"), QStringLiteral("firefox"), {}, {}});
        QCOMPARE(id.displayName, QStringLiteral("Firefox"));
        QCOMPARE(id.key.key, MatchKey::Name);
        QCOMPARE(id.key.match, QStringLiteral("Firefox"));
        QVERIFY(!id.unnamed);
    }

    void discordWebRtcFallsBackToBinary()
    {
        const auto id = identify({QStringLiteral("WEBRTC VoiceEngine"), QStringLiteral("Discord"), {}, {}});
        QCOMPARE(id.displayName, QStringLiteral("Discord"));
        QCOMPARE(id.key.key, MatchKey::Binary);
        QCOMPARE(id.key.match, QStringLiteral("Discord"));
        QVERIFY(!id.unnamed);
    }

    void genericNameWithUnknownBinaryIsUnnamed()
    {
        const auto id = identify({QStringLiteral("ALSA plug-in [wine64-preloader]"),
                                  QStringLiteral("wine64-preloader"), {}, {}});
        QVERIFY(id.unnamed);
        QCOMPARE(id.key.key, MatchKey::Binary);
        QCOMPARE(id.key.match, QStringLiteral("wine64-preloader"));
    }

    void deletedBinaryIsCleaned()
    {
        QCOMPARE(cleanBinary(QStringLiteral("brave (deleted)")), QStringLiteral("brave"));
        QCOMPARE(cleanBinary(QStringLiteral("/usr/lib/firefox/firefox")), QStringLiteral("firefox"));
    }

    void unnamedStream()
    {
        const auto id = identify({{}, {}, QStringLiteral("Game audio"), QStringLiteral("alsa_playback.x")});
        QVERIFY(id.unnamed);
        QCOMPARE(id.displayName, QStringLiteral("Game audio"));
    }

    void ruleMatching()
    {
        QList<AppRule> rules;
        rules.append({QStringLiteral("discord"), MatchKey::Binary, QStringLiteral("voice")});
        rules.append({QStringLiteral("Firefox"), MatchKey::Name, QStringLiteral("music")});
        // Name rules win over binary rules.
        QCOMPARE(matchRule(rules, {QStringLiteral("firefox"), QStringLiteral("discord"), {}, {}}), 1);
        // Binary fallback, case-insensitive, " (deleted)" ignored.
        QCOMPARE(matchRule(rules, {QStringLiteral("WEBRTC VoiceEngine"), QStringLiteral("Discord (deleted)"), {}, {}}),
                 0);
        QCOMPARE(matchRule(rules, {QStringLiteral("Spotify"), QStringLiteral("spotify"), {}, {}}), -1);
    }

    void keyRoundTrip()
    {
        const AppKey k{MatchKey::Binary, QStringLiteral("discord")};
        QCOMPARE(AppKey::fromString(k.toString()), k);
        QCOMPARE(AppKey::fromString(QStringLiteral("name:A:B")).match, QStringLiteral("A:B"));
    }
};

QTEST_GUILESS_MAIN(TestAppIdentity)
#include "tst_appidentity.moc"
