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

    void unnamedStreamMatchesTheRuleMadeFromIt()
    {
        // No app name and no binary: the app is known by its stream name, and "Always" writes
        // a rule with that name.
        const StreamProps props{{}, {}, QStringLiteral("Game audio"), QStringLiteral("alsa_playback.x")};
        const AppIdentity id = identify(props);
        const AppRule rule{id.key.match, id.key.key, QStringLiteral("game")};
        QVERIFY(ruleMatches(rule, props));
        QCOMPARE(matchRule({rule}, props), 0);
        const StreamProps nodeOnly{{}, {}, {}, QStringLiteral("alsa_playback.x")};
        const AppIdentity byNode = identify(nodeOnly);
        QVERIFY(ruleMatches({byNode.key.match, byNode.key.key, QStringLiteral("game")}, nodeOnly));
        // A named app is never matched through its stream name.
        QVERIFY(!ruleMatches(rule, {QStringLiteral("Firefox"), {}, QStringLiteral("Game audio"), {}}));
        QVERIFY(!ruleMatches(rule, {{}, QStringLiteral("mpv"), QStringLiteral("Game audio"), {}}));
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

    void javaAppsAreKnownByTheirProgram()
    {
        StreamProps p{QStringLiteral("ALSA plug-in [java]"), QStringLiteral("java"), QStringLiteral("ALSA Playback"),
                      QStringLiteral("ALSA plug-in [java]"), QStringLiteral("RuneLite")};
        AppIdentity id = identify(p);
        QCOMPARE(id.displayName, QStringLiteral("RuneLite"));
        QVERIFY(!id.unnamed);
        QCOMPARE(id.key.toString(), QStringLiteral("java:RuneLite"));
        QCOMPARE(id.javaProgram, QStringLiteral("RuneLite"));

        p.javaProgram = QStringLiteral("com.example.Game");
        id = identify(p);
        QCOMPARE(id.displayName, QStringLiteral("Java"));
        QVERIFY(id.unnamed); // Name It… names this program only
        QCOMPARE(id.key.toString(), QStringLiteral("java:com.example.Game"));

        p.javaProgram.clear(); // the command line could not be read: as before
        QCOMPARE(identify(p).key.toString(), QStringLiteral("binary:java"));
        QVERIFY(identify(p).javaProgram.isEmpty());

        // OpenAL Soft names a stream after the process, which is no name either.
        const StreamProps openAl{QStringLiteral("java"), QStringLiteral("java"), {}, {}, QStringLiteral("Minecraft")};
        QCOMPARE(identify(openAl).displayName, QStringLiteral("Minecraft"));
        // A Java app that names itself keeps that name.
        const StreamProps named{QStringLiteral("Some Game"), QStringLiteral("java"), {}, {}, QStringLiteral("com.example.Game")};
        QCOMPARE(identify(named).key.toString(), QStringLiteral("name:Some Game"));
    }

    void aStreamNamedJavaKeepsItsNameWithoutAProgram()
    {
        // OpenAL Soft names a stream "java". Without a program (a recording stream, or a command
        // line that cannot be read) it keeps the key it had, so saved choices for it still apply.
        const StreamProps capture{QStringLiteral("java"), QStringLiteral("java"), {}, {}};
        const AppIdentity id = identify(capture);
        QCOMPARE(id.key.toString(), QStringLiteral("name:java"));
        QCOMPARE(id.displayName, QStringLiteral("java"));
        QVERIFY(!id.unnamed);
        // A program says which Java app it is, whatever the binary's case.
        const StreamProps game{QStringLiteral("java"), QStringLiteral("/usr/bin/Java"), {}, {}, QStringLiteral("Minecraft")};
        QCOMPARE(identify(game).key.toString(), QStringLiteral("java:Minecraft"));
        // A name that only matches the binary of something else stays a name.
        const StreamProps other{QStringLiteral("java"), QStringLiteral("python3"), {}, {}, QStringLiteral("Minecraft")};
        QCOMPARE(identify(other).key.toString(), QStringLiteral("name:java"));
    }

    void javaRulesComeBetweenNameAndBinary()
    {
        const AppRule anyJava{QStringLiteral("java"), MatchKey::Binary, QStringLiteral("desktop")};
        const AppRule runeLite{QStringLiteral("runelite"), MatchKey::Java, QStringLiteral("game")};
        const StreamProps rl{QStringLiteral("ALSA plug-in [java]"), QStringLiteral("java"), {}, {}, QStringLiteral("RuneLite")};
        const StreamProps mc{QStringLiteral("ALSA plug-in [java]"), QStringLiteral("java"), {}, {}, QStringLiteral("Minecraft")};
        QCOMPARE(matchRule({anyJava, runeLite}, rl), 1);
        QCOMPARE(matchRule({anyJava, runeLite}, mc), 0); // the older java rule still catches the rest
        // A name rule still wins over a Java rule.
        const AppRule byName{QStringLiteral("ALSA plug-in [java]"), MatchKey::Name, QStringLiteral("music")};
        QCOMPARE(matchRule({anyJava, runeLite, byName}, rl), 2);
        // Only a Java app's program matches: not an app named RuneLite, nor one with no program.
        QVERIFY(!ruleMatches(runeLite, StreamProps{QStringLiteral("RuneLite"), QStringLiteral("runelite"), {}, {}}));
        QVERIFY(!ruleMatches(AppRule{QString(), MatchKey::Java, QStringLiteral("game")}, mc));
    }
};

QTEST_GUILESS_MAIN(TestAppIdentity)
#include "tst_appidentity.moc"
