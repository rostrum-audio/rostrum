#include "core/Ducking.h"
#include "core/SceneStore.h"
#include "core/SceneToml.h"
#include "core/Settings.h"
#include "engine/Engine.h"
#include "engine/SceneManager.h"
#include "pw/Graph.h"
#include "pw/PwContext.h"

#include <QTemporaryDir>
#include <QTest>

#include <cmath>

using namespace rostrum;

namespace {

pw::Node node(uint32_t id, const char *name, const char *mediaClass, int priority = 0)
{
    pw::Node n;
    n.id = id;
    n.name = QString::fromLatin1(name);
    n.mediaClass = QString::fromLatin1(mediaClass);
    n.props.insert(QStringLiteral("priority.session"), QString::number(priority));
    return n;
}

// A headset mic, a webcam (the system default), a laptop mic, Rostrum's own nodes and two sinks.
pw::Graph devices()
{
    pw::Graph g;
    for (const auto &n : {node(1, "alsa_input.usb-headset", "Audio/Source", 1500),
                          node(2, "alsa_input.webcam", "Audio/Source", 1000),
                          node(3, "alsa_input.laptop", "Audio/Source", 2000),
                          node(4, "rostrum.mic", "Audio/Source/Virtual", 9000),
                          node(5, "alsa_output.usb-headset", "Audio/Sink", 1500),
                          node(6, "alsa_output.speakers", "Audio/Sink", 1000),
                          node(7, "rostrum.phones", "Audio/Sink", 9000)}) {
        g.nodes.insert(n.id, n);
    }
    return g;
}

QString nameOf(const pw::Node *n)
{
    return n ? n->name : QString();
}

} // namespace

class TestEngine : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void savedMicIsUsedWhenPresent()
    {
        const pw::Graph g = devices();
        const QString headset = QStringLiteral("alsa_input.usb-headset");
        QCOMPARE(nameOf(engine::resolveDevice(g, headset, QStringLiteral("alsa_input.webcam"), false, false)),
                 headset);
        QCOMPARE(nameOf(engine::resolveDevice(g, headset, QStringLiteral("alsa_input.webcam"), false, true)),
                 headset);
    }

    void missingMicStaysSilentByDefault()
    {
        pw::Graph g = devices();
        g.nodes.remove(1); // the headset is unplugged
        const QString headset = QStringLiteral("alsa_input.usb-headset");
        const QString webcam = QStringLiteral("alsa_input.webcam");
        // No fallback: nothing is linked, so the webcam never goes live on stream.
        QCOMPARE(engine::resolveDevice(g, headset, webcam, false, false), nullptr);
        // With the fallback chosen, the system default stands in.
        QCOMPARE(nameOf(engine::resolveDevice(g, headset, webcam, false, true)), webcam);
        // Without a usable default, the highest priority wins, never a Rostrum node.
        QCOMPARE(nameOf(engine::resolveDevice(g, headset, QString(), false, true)),
                 QStringLiteral("alsa_input.laptop"));
        QCOMPARE(engine::resolveDevice(g, headset, QString(), false, false), nullptr);
    }

    void unsavedMicFollowsTheDefault()
    {
        const pw::Graph g = devices();
        const QString webcam = QStringLiteral("alsa_input.webcam");
        QCOMPARE(nameOf(engine::resolveDevice(g, QString(), webcam, false, false)), webcam);
        QCOMPARE(nameOf(engine::resolveDevice(g, QString(), QString(), false, false)),
                 QStringLiteral("alsa_input.laptop"));
    }

    void savedNameOfTheWrongKindIsMissing()
    {
        const pw::Graph g = devices();
        // A sink name saved as the mic, or a Rostrum node, is never linked as the mic.
        QCOMPARE(engine::resolveDevice(g, QStringLiteral("alsa_output.speakers"), QString(), false, false),
                 nullptr);
        QCOMPARE(engine::resolveDevice(g, QStringLiteral("rostrum.mic"), QString(), false, false), nullptr);
    }

    void headphonesStillFallBack()
    {
        pw::Graph g = devices();
        g.nodes.remove(5);
        QCOMPARE(nameOf(engine::resolveDevice(g, QStringLiteral("alsa_output.usb-headset"),
                                              QStringLiteral("alsa_output.speakers"), true, true)),
                 QStringLiteral("alsa_output.speakers"));
        QCOMPARE(nameOf(engine::resolveDevice(g, QString(), QString(), true, true)),
                 QStringLiteral("alsa_output.speakers"));
    }

    void appMuteIsSavedLikeTheVolume()
    {
        QTemporaryDir dir;
        pw::PwContext pw;
        engine::Engine engine(&pw);
        engine::SceneManager scenes(&engine, dir.path());
        scenes.load(QStringLiteral("Live"));

        // Without a rule, the mute lasts for this run only and never touches the scene.
        const AppKey discord{MatchKey::Binary, QStringLiteral("Discord")};
        engine.setAppMuted(discord, true);
        QVERIFY(engine.appMuted(discord));
        QVERIFY(!scenes.dirty());

        // Making it a rule carries the mute over, and the rule saves it.
        engine.assignApp(discord, QStringLiteral("voice"), true);
        QVERIFY(engine.scene().rule(MatchKey::Binary, QStringLiteral("discord"))->muted);
        QVERIFY(scenes.save());
        const QString text =
            SceneStore::readFile(SceneStore(dir.path()).fileFor(QStringLiteral("Live")), nullptr);
        QVERIFY2(text.contains(QStringLiteral("muted = true")), qPrintable(text));
        QVERIFY(toml_io::parseScene(text)->rule(MatchKey::Binary, QStringLiteral("Discord"))->muted);

        engine.setAppMuted(discord, false);
        QVERIFY(scenes.dirty());
        QVERIFY(!engine.appMuted(discord));
        // An unmuted rule writes no mute line at all.
        const QString rules = toml_io::serializeScene(engine.scene()).section(QStringLiteral("[[rule]]"), 1);
        QVERIFY(!rules.isEmpty());
        QVERIFY2(!rules.contains(QStringLiteral("muted")), qPrintable(rules));
    }

    void mergeKeepsSavedAppMute()
    {
        Scene saved = defaults::scene();
        AppRule r;
        r.match = QStringLiteral("Spotify");
        r.busId = QStringLiteral("music");
        r.muted = true;
        saved.rules.append(r);
        Scene current = saved;
        current.rules.first().muted = false;
        current.rules.first().busId = QStringLiteral("desktop");
        const Scene merged = mergeStructure(saved, current);
        QVERIFY(merged.rules.first().muted);
        QCOMPARE(merged.rules.first().busId, QStringLiteral("desktop"));
    }

    void balanceRoundTripsAndIsClamped()
    {
        Scene s = defaults::scene();
        s.bus(QStringLiteral("music"))->balance = -0.4;
        const QString text = toml_io::serializeScene(s);
        QVERIFY2(text.contains(QStringLiteral("balance = -0.4")), qPrintable(text));
        // Only the bus with a balance writes one.
        QCOMPARE(text.count(QStringLiteral("balance")), 1);
        QCOMPARE(toml_io::parseScene(text)->bus(QStringLiteral("music"))->balance, -0.4);
        QCOMPARE(*toml_io::parseScene(text), s);

        QString edited = text;
        edited.replace(QStringLiteral("balance = -0.4"), QStringLiteral("balance = 7.5"));
        QCOMPARE(toml_io::parseScene(edited)->bus(QStringLiteral("music"))->balance, 1.0);
        edited.replace(QStringLiteral("balance = 7.5"), QStringLiteral("balance = nan"));
        QCOMPARE(toml_io::parseScene(edited)->bus(QStringLiteral("music"))->balance, 0.0);
        edited.replace(QStringLiteral("balance = nan"), QStringLiteral("balance = 'left'"));
        QCOMPARE(toml_io::parseScene(edited)->bus(QStringLiteral("music"))->balance, 0.0);

        // The mic is mono: a hand-edited balance on it is dropped.
        Scene mic = defaults::scene();
        mic.micBus()->balance = 0.5;
        QCOMPARE(toml_io::sanitize(mic).micBus()->balance, 0.0);
    }

    void busBalanceMarksTheSceneDirty()
    {
        QTemporaryDir dir;
        pw::PwContext pw;
        engine::Engine engine(&pw);
        engine::SceneManager scenes(&engine, dir.path());
        scenes.load(QStringLiteral("Live"));
        engine.setBusBalance(QStringLiteral("music"), -2.0);
        QCOMPARE(engine.scene().bus(QStringLiteral("music"))->balance, -1.0);
        QVERIFY(scenes.dirty());
        engine.setBusBalance(QStringLiteral("music"), 0.001);
        QCOMPARE(engine.scene().bus(QStringLiteral("music"))->balance, 0.0);
        QVERIFY(!scenes.dirty());
        engine.setBusBalance(QStringLiteral("mic"), 0.5);
        QCOMPARE(engine.scene().micBus()->balance, 0.0);
    }

    void monoHeadphonesIsOffByDefault()
    {
        QVERIFY(!defaultSettings().monoHeadphones);
        Settings s = defaultSettings();
        s.monoHeadphones = true;
        QCOMPARE(parseSettings(serializeSettings(s)), s);
        QVERIFY(serializeSettings(s).contains(QStringLiteral("mono_headphones = true")));
        pw::PwContext pw;
        engine::Engine engine(&pw);
        QVERIFY(!engine.monoHeadphones());
        engine.setMonoHeadphones(true);
        QVERIFY(engine.monoHeadphones());
        // A setting, not a level: the scene never carries it.
        QVERIFY(!toml_io::serializeScene(engine.scene()).contains(QStringLiteral("mono")));
    }

    void sceneFadeRampsWithoutTouchingTheScene()
    {
        pw::PwContext pw;
        engine::Engine engine(&pw);
        const QString music = QStringLiteral("rostrum.music");
        Scene a = defaults::scene();
        a.bus(QStringLiteral("music"))->volume = 0.8;
        a.bus(QStringLiteral("game"))->muted = true;
        a.bus(QStringLiteral("voice"))->volume = 0.5;
        engine.setScene(a);
        QVERIFY(!engine.fading());

        Scene b = a;
        b.name = QStringLiteral("Chatting");
        b.bus(QStringLiteral("music"))->volume = 0.2;
        b.bus(QStringLiteral("game"))->muted = false;
        b.bus(QStringLiteral("game"))->volume = 0.6;
        b.bus(QStringLiteral("voice"))->muted = true;
        b.micBus()->muted = true;
        b.masterStream = 0.5;

        // Off by default: the switch is instant.
        engine.setScene(b, true);
        QVERIFY(!engine.fading());
        engine.setScene(a);

        engine.setSceneFadeMs(300);
        engine.setScene(b, true);
        // The scene holds the target at once, so saving mid-fade saves the target.
        QCOMPARE(engine.scene(), b);
        QVERIFY(engine.fading());
        QVERIFY(qAbs(*engine.fadingPosition(music) - 0.8) < 0.1);
        // A muted bus starts from silence; a bus being muted ramps down to it.
        QVERIFY(*engine.fadingPosition(QStringLiteral("rostrum.game")) < 0.1);
        QVERIFY(qAbs(*engine.fadingPosition(QStringLiteral("rostrum.voice")) - 0.5) < 0.1);
        QVERIFY(engine.fadingPosition(QStringLiteral("rostrum.stream")).has_value());
        // Unchanged buses and the mic never fade.
        QVERIFY(!engine.fadingPosition(QStringLiteral("rostrum.alerts")).has_value());
        QVERIFY(!engine.fadingPosition(QStringLiteral("rostrum.mic")).has_value());
        QVERIFY(!engine.fadingPosition(QStringLiteral("rostrum.sidetone")).has_value());

        QTRY_VERIFY_WITH_TIMEOUT(!engine.fading(), 2000);
        QVERIFY(!engine.fadingPosition(music).has_value());
        QCOMPARE(engine.scene(), b);
    }

    void switchMidFadeContinuesFromTheCurrentLevel()
    {
        pw::PwContext pw;
        engine::Engine engine(&pw);
        const QString music = QStringLiteral("rostrum.music");
        Scene loud = defaults::scene();
        Scene quiet = loud;
        quiet.bus(QStringLiteral("music"))->volume = 0.0;
        engine.setScene(loud);
        engine.setSceneFadeMs(1000);
        engine.setScene(quiet, true);
        QTest::qWait(400);
        const double reached = *engine.fadingPosition(music);
        QVERIFY2(reached < 0.9 && reached > 0.1, qPrintable(QString::number(reached)));
        engine.setScene(loud, true);
        QVERIFY(qAbs(*engine.fadingPosition(music) - reached) < 0.1);

        // Moving a fader takes that bus out of the fade; the rest carry on.
        Scene other = quiet;
        other.bus(QStringLiteral("game"))->volume = 0.1;
        engine.setScene(other, true);
        engine.setBusVolume(QStringLiteral("music"), 0.7);
        QVERIFY(!engine.fadingPosition(music).has_value());
        QVERIFY(engine.fadingPosition(QStringLiteral("rostrum.game")).has_value());
        // Solo mutes at once, so it ends the fade.
        engine.setSolo(QStringLiteral("game"), true);
        QVERIFY(!engine.fading());
    }

    void fadesNeverDirtyTheSceneOrRunOnLoad()
    {
        QTemporaryDir dir;
        pw::PwContext pw;
        engine::Engine engine(&pw);
        engine.setSceneFadeMs(600);
        engine::SceneManager scenes(&engine, dir.path());
        scenes.load(QStringLiteral("Live"));
        QVERIFY(!engine.fading());
        Scene brb = defaults::scene(QStringLiteral("BRB"));
        brb.bus(QStringLiteral("game"))->volume = 0.1;
        QVERIFY(scenes.create(brb));

        QVERIFY(scenes.switchTo(QStringLiteral("BRB")));
        QVERIFY(engine.fading());
        QVERIFY(!scenes.dirty());
        QTRY_VERIFY_WITH_TIMEOUT(!engine.fading(), 2000);
        QVERIFY(!scenes.dirty());

        // Loading at startup applies the saved scene at once.
        engine::Engine fresh(&pw);
        fresh.setSceneFadeMs(600);
        engine::SceneManager again(&fresh, dir.path());
        again.load(QStringLiteral("BRB"));
        QVERIFY(!fresh.fading());
    }

    void sceneFadeSettingSnapsToAChoice()
    {
        QCOMPARE(defaultSettings().sceneFadeMs, 0);
        Settings s = defaultSettings();
        s.sceneFadeMs = 300;
        QCOMPARE(parseSettings(serializeSettings(s)), s);
        auto parsed = [](const char *v) {
            return parseSettings(QStringLiteral("[general]\nscene_fade_ms = %1\n").arg(QLatin1String(v)))
                .sceneFadeMs;
        };
        QCOMPARE(parsed("600"), 600);
        QCOMPARE(parsed("450"), 300);
        QCOMPARE(parsed("-20"), 0);
        QCOMPARE(parsed("99999"), 1000);
        QCOMPARE(parsed("'slow'"), 0);
    }

    void duckingEnvelope()
    {
        ducking::Settings s; // -12 dB, 100 ms attack, 800 ms release
        ducking::Envelope env;
        const double speech = 0.1; // -20 dBFS
        const double hiss = 0.005; // -46 dBFS, below the threshold
        auto near = [](double a, double b) { return qAbs(a - b) < 1e-6; };

        for (int i = 0; i < 100; ++i) {
            env.advance(hiss, 20, s);
        }
        QCOMPARE(env.gainDb(), 0.0);
        QCOMPARE(env.gain(), 1.0);
        QVERIFY(!env.ducked());

        // Attack: 12 dB over 100 ms.
        env.advance(speech, 50, s);
        QVERIFY(near(env.gainDb(), -6.0));
        QVERIFY(env.ducked());
        env.advance(speech, 50, s);
        QVERIFY(near(env.gainDb(), -12.0));
        // Never past the amount, however late the tick.
        env.advance(speech, 1000, s);
        QVERIFY(near(env.gainDb(), -12.0));
        QVERIFY(qAbs(env.gain() - std::pow(10.0, -12.0 / 20.0)) < 1e-9);

        // Hold: a pause between words keeps it down.
        env.advance(hiss, 300, s);
        QVERIFY(near(env.gainDb(), -12.0));
        env.advance(speech, 20, s);
        env.advance(hiss, ducking::kHoldMs - 20, s);
        QVERIFY(near(env.gainDb(), -12.0));
        // Release: back over 800 ms once the hold has run out.
        env.advance(hiss, 20, s);
        env.advance(hiss, 400, s);
        QVERIFY2(env.gainDb() > -6.5 && env.gainDb() < -5.5, qPrintable(QString::number(env.gainDb())));
        env.advance(hiss, 5000, s);
        QCOMPARE(env.gainDb(), 0.0);
        QVERIFY(!env.ducked());

        env.advance(speech, 20, s);
        QVERIFY(env.gainDb() < 0.0);
        env.reset();
        QCOMPARE(env.gainDb(), 0.0);
        QCOMPARE(env.gain(), 1.0);
    }

    void duckingSettings()
    {
        const Settings d = defaultSettings();
        QVERIFY(!d.ducking.enabled);
        QCOMPARE(d.ducking.trigger, ducking::Trigger::Mic);
        QCOMPARE(d.ducking.buses, QStringList{QStringLiteral("music")});
        QCOMPARE(d.ducking.amountDb, -12);
        QCOMPARE(d.ducking.attackMs, 100);
        QCOMPARE(d.ducking.releaseMs, 800);

        Settings s = d;
        s.ducking.enabled = true;
        s.ducking.trigger = ducking::Trigger::Either;
        s.ducking.buses = {QStringLiteral("music"), QStringLiteral("alerts")};
        s.ducking.amountDb = -18;
        s.ducking.attackMs = 50;
        s.ducking.releaseMs = 1500;
        QCOMPARE(parseSettings(serializeSettings(s)), s);

        const Settings odd = parseSettings(QStringLiteral(
            "[ducking]\nenabled = true\ntrigger = 'loud'\n"
            "buses = ['music', 'mic', 'music', 'Bad Id', 'game']\n"
            "amount_db = -100\nattack_ms = 'fast'\nrelease_ms = 1e9\n"));
        QVERIFY(odd.ducking.enabled);
        QCOMPARE(odd.ducking.trigger, ducking::Trigger::Mic);
        QCOMPARE(odd.ducking.buses, (QStringList{QStringLiteral("music"), QStringLiteral("game")}));
        QCOMPARE(odd.ducking.amountDb, -24);
        QCOMPARE(odd.ducking.attackMs, 100);
        QCOMPARE(odd.ducking.releaseMs, 3000);
        QCOMPARE(parseSettings(QStringLiteral("[ducking]\namount_db = -10.6\n")).ducking.amountDb, -9);
        QCOMPARE(parseSettings(QStringLiteral("[ducking]\nbuses = []\n")).ducking.buses, QStringList());
    }

    void duckingTargetsAndSessionOnly()
    {
        QTemporaryDir dir;
        pw::PwContext pw;
        engine::Engine engine(&pw);
        engine::SceneManager scenes(&engine, dir.path());
        scenes.load(QStringLiteral("Live"));
        const QString music = QStringLiteral("music");
        const QString voice = QStringLiteral("voice");

        QVERIFY(!engine.isDuckTarget(music)); // off by default
        ducking::Settings s;
        s.enabled = true;
        s.buses = {music, voice, QStringLiteral("mic"), QStringLiteral("gone")};
        engine.setDucking(s);
        QCOMPARE(engine.ducking().buses, (QStringList{music, voice, QStringLiteral("gone")}));
        QVERIFY(engine.isDuckTarget(music));
        QVERIFY(engine.isDuckTarget(voice));
        QVERIFY(!engine.isDuckTarget(QStringLiteral("mic")));
        QVERIFY(!engine.isDuckTarget(QStringLiteral("gone")));
        QVERIFY(!engine.isDuckTarget(QStringLiteral("game")));
        // The bus that triggers is never ducked by itself.
        s.trigger = ducking::Trigger::Voice;
        engine.setDucking(s);
        QVERIFY(!engine.isDuckTarget(voice));
        s.trigger = ducking::Trigger::Either;
        engine.setDucking(s);
        QVERIFY(!engine.isDuckTarget(voice));
        QVERIFY(engine.isDuckTarget(music));

        // Nothing is heard without an audio server, so nothing is ducked or released.
        QVERIFY(!engine.isDucked(music));
        QVERIFY(!engine.releaseTransientLevels());
        // Ducking is a setting, never a level.
        QVERIFY(!scenes.dirty());
        QVERIFY(!toml_io::serializeScene(engine.scene()).contains(QStringLiteral("duck")));
    }

    void micFallbackIsOffByDefault()
    {
        QVERIFY(!defaultSettings().micFallback);
        Settings s = defaultSettings();
        s.mic = QStringLiteral("alsa_input.usb-headset");
        s.micFallback = true;
        QCOMPARE(parseSettings(serializeSettings(s)), s);
        QVERIFY(!parseSettings(QStringLiteral("[devices]\nmic = 'x'\n")).micFallback);

        pw::PwContext pw; // never started: no audio server needed
        engine::Engine engine(&pw);
        QVERIFY(!engine.micFallback());
        engine.setMicDevice(QStringLiteral("alsa_input.usb-headset"));
        // Not connected: nothing is reported missing, and the saved choice is kept as given.
        QVERIFY(!engine.micSilenced());
        QCOMPARE(engine.micDevice(), QStringLiteral("alsa_input.usb-headset"));
        QCOMPARE(engine.missingMicLabel(), QStringLiteral("alsa_input.usb-headset"));
    }
};

QTEST_GUILESS_MAIN(TestEngine)
#include "tst_engine.moc"
