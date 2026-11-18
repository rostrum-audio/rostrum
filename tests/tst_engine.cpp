#include "core/Ducking.h"
#include "core/SceneStore.h"
#include "core/SceneToml.h"
#include "core/Settings.h"
#include "engine/Engine.h"
#include "engine/MixPlan.h"
#include "engine/NodeSpecs.h"
#include "engine/SceneManager.h"
#include "pw/Graph.h"
#include "pw/MicCheck.h"
#include "pw/PortConfig.h"
#include "pw/PwContext.h"

#include <QCoreApplication>
#include <QEvent>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>

#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <spa/pod/parser.h>

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

QStringList linkNames(const QList<engine::NodeLink> &links)
{
    QStringList out;
    for (const auto &l : links) {
        out << l.from + QStringLiteral(">") + l.to;
    }
    out.sort();
    return out;
}

pw::Port port(uint32_t id, uint32_t node, bool output, const char *channel)
{
    pw::Port p;
    p.id = id;
    p.nodeId = node;
    p.output = output;
    p.channel = QString::fromLatin1(channel);
    return p;
}

} // namespace

class TestEngine : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    // PipeWire 1.4 refuses a PortConfig format without a rate (1.6 dropped the check), and then
    // Studio Voice never gets its ports. Both versions ignore the rate in DSP mode.
    void dspPortConfigPassesPipeWire14Checks()
    {
        for (const int channels : {1, 2}) {
            for (const bool output : {false, true}) {
                uint8_t buffer[1024];
                spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
                const spa_pod *param = pw::buildDspPortConfig(&b, output, channels);
                QVERIFY(param);
                uint32_t direction = 0, mode = 0;
                const spa_pod *format = nullptr;
                // The same checks, in the same terms, as audioconvert's PortConfig handler.
                QVERIFY(spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamPortConfig, nullptr,
                                             SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(&direction),
                                             SPA_PARAM_PORT_CONFIG_mode, SPA_POD_Id(&mode),
                                             SPA_PARAM_PORT_CONFIG_format, SPA_POD_Pod(&format)) >= 0);
                QCOMPARE(direction, uint32_t(output ? SPA_DIRECTION_OUTPUT : SPA_DIRECTION_INPUT));
                QCOMPARE(mode, uint32_t(SPA_PARAM_PORT_CONFIG_MODE_dsp));
                spa_audio_info_raw info{};
                QVERIFY(spa_format_audio_raw_parse(format, &info) >= 0);
                QVERIFY(info.format != 0);
                QVERIFY(info.rate != 0);
                QCOMPARE(info.channels, uint32_t(channels));
                if (channels == 1) {
                    QCOMPARE(info.position[0], uint32_t(SPA_AUDIO_CHANNEL_MONO));
                } else {
                    QCOMPARE(info.position[0], uint32_t(SPA_AUDIO_CHANNEL_FL));
                    QCOMPARE(info.position[1], uint32_t(SPA_AUDIO_CHANNEL_FR));
                }
            }
        }
    }

    void sendNamesAndProperties()
    {
        QCOMPARE(engine::sendNodeName(QStringLiteral("game"), Mix::Phones), QStringLiteral("rostrum.game.phones"));
        QCOMPARE(engine::sendNodeName(QStringLiteral("game"), Mix::Stream), QStringLiteral("rostrum.game.stream"));
        const Scene scene = defaults::scene();
        const auto p = engine::sendProperties(*scene.bus(QStringLiteral("game")), Mix::Stream);
        QCOMPARE(p.value(QStringLiteral("node.name")), QStringLiteral("rostrum.game.stream"));
        QCOMPARE(p.value(QStringLiteral("factory.name")), QStringLiteral("audio.convert"));
        QCOMPARE(p.value(QStringLiteral("convert.direction")), QStringLiteral("output"));
        QCOMPARE(p.value(QStringLiteral("audio.channels")), QStringLiteral("2"));
        QCOMPARE(p.value(QStringLiteral("audio.position")), QStringLiteral("[ FL FR ]"));
        QCOMPARE(p.value(QStringLiteral("rostrum.role")), QStringLiteral("send"));
        QCOMPARE(p.value(QStringLiteral("rostrum.bus")), QStringLiteral("game"));
        QCOMPARE(p.value(QStringLiteral("rostrum.mix")), QStringLiteral("stream"));
        QCOMPARE(p.value(QStringLiteral("object.linger")), QStringLiteral("true"));
        QCOMPARE(p.value(QStringLiteral("node.virtual")), QStringLiteral("true"));
        QVERIFY(!p.contains(QStringLiteral("media.class")));
        QVERIFY(!p.contains(QStringLiteral("node.force-quantum")));
    }

    void desiredSendsSkipMic()
    {
        const auto sends = engine::desiredSends(defaults::scene());
        QCOMPARE(sends.size(), 10);
        QCOMPARE(sends.first().name, QStringLiteral("rostrum.game.phones"));
        QCOMPARE(sends.at(1).name, QStringLiteral("rostrum.game.stream"));
        for (const auto &spec : sends) {
            QVERIFY(spec.busId != QStringLiteral("mic"));
            QCOMPARE(spec.name, engine::sendNodeName(spec.busId, spec.mix));
        }
    }

    void busLinksPerMix()
    {
        const QStringList links = linkNames(engine::busLinks(defaults::scene(), true));
        for (const char *l : {"rostrum.game>rostrum.game.phones", "rostrum.game>rostrum.game.stream",
                              "rostrum.game.phones>rostrum.phones", "rostrum.game.stream>rostrum.stream",
                              "rostrum.game.stream>rostrum.vod", "rostrum.music.stream>rostrum.stream"}) {
            QVERIFY2(links.contains(QString::fromLatin1(l)), l);
        }
        // Music goes to the stream only, and stays off the VOD by default.
        QVERIFY(!links.contains(QStringLiteral("rostrum.music.phones>rostrum.phones")));
        QVERIFY(!links.contains(QStringLiteral("rostrum.music.stream>rostrum.vod")));
        for (const QString &l : links) {
            for (const char *master : {"rostrum.phones", "rostrum.stream", "rostrum.vod"}) {
                const QString to = QStringLiteral(">") + QString::fromLatin1(master);
                QVERIFY2(!l.endsWith(to) || l.section(QLatin1Char('>'), 0, 0).count(QLatin1Char('.')) == 2,
                         qPrintable(l)); // only sends ("rostrum.<bus>.<mix>") feed a master
            }
        }
    }

    void busLinksSingleLevel()
    {
        const Scene scene = defaults::scene();
        QStringList expected;
        for (const auto &b : scene.buses) {
            if (b.isInput()) {
                continue;
            }
            if (feedsPhones(b.destination)) {
                expected << b.nodeName() + QStringLiteral(">rostrum.phones");
            }
            if (feedsStream(b.destination)) {
                expected << b.nodeName() + QStringLiteral(">rostrum.stream");
            }
            if (feedsVod(b)) {
                expected << b.nodeName() + QStringLiteral(">rostrum.vod");
            }
        }
        expected.sort();
        QCOMPARE(linkNames(engine::busLinks(scene, false)), expected);
    }

    void unwantedSendsAreRemoved()
    {
        pw::PwContext pw;
        engine::Engine engine(&pw);
        const QString copy = engine.duplicateBus(QStringLiteral("game"));
        QVERIFY(engine::sendWanted(engine.scene(), engine::sendNodeName(copy, Mix::Phones)));
        QVERIFY(engine::sendWanted(engine.scene(), engine::sendNodeName(copy, Mix::Stream)));
        QVERIFY(engine.removeBus(QStringLiteral("game")));
        QVERIFY(!engine::sendWanted(engine.scene(), QStringLiteral("rostrum.game.phones")));
        QVERIFY(!engine::sendWanted(engine.scene(), QStringLiteral("rostrum.game.stream")));
        QVERIFY(!engine::sendWanted(engine.scene(), QStringLiteral("rostrum.mic.phones")));
        QVERIFY(!engine::sendWanted(engine.scene(), QStringLiteral("rostrum.voice")));
    }

    void sendsWithPortsAreNotReconfigured()
    {
        pw::Graph g;
        g.nodes.insert(40, node(40, "rostrum.game.phones", ""));
        g.nodes.insert(41, node(41, "rostrum.game.stream", ""));
        for (const auto &p : {port(400, 40, false, "FL"), port(401, 40, false, "FR"), port(402, 40, true, "FL"),
                              port(403, 40, true, "FR")}) {
            g.ports.insert(p.id, p);
        }
        QVERIFY(!engine::sendNeedsPortConfig(g, 40, {}));
        QVERIFY(engine::sendNeedsPortConfig(g, 41, {}));
        QVERIFY(!engine::sendNeedsPortConfig(g, 41, {41}));
    }

    void duplicateKeepsMixLevels()
    {
        pw::PwContext pw;
        engine::Engine engine(&pw);
        Scene s = engine.scene();
        s.bus(QStringLiteral("game"))->volume = 0.4;
        s.bus(QStringLiteral("game"))->phonesVolume = 0.9;
        s.bus(QStringLiteral("game"))->linked = false;
        engine.setScene(s);
        const QString copy = engine.duplicateBus(QStringLiteral("game"));
        const Bus *b = engine.scene().bus(copy);
        QVERIFY(b);
        QCOMPARE(b->volume, 0.4);
        QCOMPARE(b->phonesVolume, 0.9);
        QCOMPARE(b->linked, false);
    }

    void newBusCannotUseVodMasterName()
    {
        pw::PwContext pw;
        engine::Engine engine(&pw);
        const QString id = engine.addBus(QStringLiteral("VOD"));
        QVERIFY(!id.isEmpty());
        const Bus *bus = engine.scene().bus(id);
        QVERIFY(bus);
        QVERIFY2(bus->nodeName() != QString::fromLatin1(engine::kVodNode),
                 "A playback bus must not reuse the VOD master node");
        QSet<QString> names;
        for (const auto &spec : engine::desiredNodes(engine.scene())) {
            QVERIFY2(!names.contains(spec.name), qPrintable(spec.name));
            names.insert(spec.name);
        }
        const auto restored = toml_io::parseScene(toml_io::serializeScene(engine.scene()));
        QVERIFY(restored);
        QVERIFY2(restored->bus(id), "The added bus must survive saving and loading");
    }

    void ruleIconsSurviveRestartWithoutStreams()
    {
        Scene scene = defaults::scene();
        AppRule discord;
        discord.match = QStringLiteral("WEBRTC VoiceEngine");
        discord.busId = QStringLiteral("voice");
        discord.iconNames = {QStringLiteral("missing-desktop-icon"), QStringLiteral("discord")};
        scene.rules.append(discord);
        const auto restored = toml_io::parseScene(toml_io::serializeScene(scene));
        QVERIFY(restored);
        QCOMPARE(restored->rules.first().iconNames, (QStringList{"missing-desktop-icon", "discord"}));
        pw::PwContext pw;
        engine::Engine engine(&pw);
        QCOMPARE(engine.ruleIconCandidates(restored->rules.first()),
                 (QStringList{"missing-desktop-icon", "discord"}));
        AppRule java;
        java.match = QStringLiteral("java");
        QVERIFY(engine.ruleIconCandidates(java).isEmpty());
        AppRule obs;
        obs.match = QStringLiteral("OBS");
        obs.iconNames = {QStringLiteral("obs")};
        QVERIFY(engine.ruleIconCandidates(obs).isEmpty());
    }

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
        // Labels are empty unless the chosen device is actually missing.
        QVERIFY(engine.missingMicLabel().isEmpty());
        QVERIFY(engine.missingHeadphonesLabel().isEmpty());
    }

    void missingDeviceLabelsTrackPresence()
    {
        pw::PwContext pw; // never started: stand-in graph only
        engine::Engine engine(&pw);
        QSignalSpy micSpy(&engine, &engine::Engine::missingMicLabelChanged);
        QSignalSpy phonesSpy(&engine, &engine::Engine::missingHeadphonesLabelChanged);

        auto flush = [] {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::Timer);
            QCoreApplication::processEvents();
        };

        pw::Graph present = devices();
        present.nodes[1].description = QStringLiteral("USB Headset Mic");
        present.nodes[5].description = QStringLiteral("USB Headset");
        pw.testingInstallGraph(present);
        engine.setMicDevice(QStringLiteral("alsa_input.usb-headset"));
        engine.setHeadphoneDevice(QStringLiteral("alsa_output.usb-headset"));
        flush();

        QVERIFY(!engine.micMissing());
        QVERIFY(!engine.headphonesMissing());
        QVERIFY(engine.missingMicLabel().isEmpty());
        QVERIFY(engine.missingHeadphonesLabel().isEmpty());
        const int micAfterPresent = micSpy.count();
        const int phonesAfterPresent = phonesSpy.count();

        pw::Graph gone = present;
        gone.nodes.remove(1); // mic
        gone.nodes.remove(5); // headphones
        pw.testingInstallGraph(gone);
        flush();

        QVERIFY(engine.micMissing());
        QVERIFY(engine.headphonesMissing());
        QCOMPARE(engine.missingMicLabel(), QStringLiteral("USB Headset Mic"));
        QCOMPARE(engine.missingHeadphonesLabel(), QStringLiteral("USB Headset"));
        QVERIFY(micSpy.count() > micAfterPresent);
        QVERIFY(phonesSpy.count() > phonesAfterPresent);
        const int micAfterGone = micSpy.count();
        const int phonesAfterGone = phonesSpy.count();

        pw.testingInstallGraph(present);
        flush();

        QVERIFY(!engine.micMissing());
        QVERIFY(!engine.headphonesMissing());
        QVERIFY(engine.missingMicLabel().isEmpty());
        QVERIFY(engine.missingHeadphonesLabel().isEmpty());
        QVERIFY(micSpy.count() > micAfterGone);
        QVERIFY(phonesSpy.count() > phonesAfterGone);
    }

    // WirePlumber reconfigures every node when it starts, so a bus loses its ports one by one and
    // gets new ones. PipeWire removes the links on a removed port itself and hands their IDs to
    // new objects at once; asking for such a link to be destroyed would remove whatever has the ID
    // now (in practice: the bus's new ports).
    void linksOfABusThatIsLosingItsPortsAreLeftAlone()
    {
        pw::PwContext pw; // never started: stand-in graph only
        engine::Engine engine(&pw);
        auto flush = [] {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::Timer);
            QCoreApplication::processEvents();
        };
        auto port = [](uint32_t id, uint32_t nodeId, bool output, const char *channel) {
            pw::Port p;
            p.id = id;
            p.nodeId = nodeId;
            p.output = output;
            p.monitor = output;
            p.channel = QString::fromLatin1(channel);
            p.name = QStringLiteral("%1_%2").arg(output ? QStringLiteral("monitor") : QStringLiteral("playback"), p.channel);
            p.serial = QString::number(1000 + id);
            return p;
        };
        auto link = [](uint32_t id, uint32_t outNode, uint32_t outPort, uint32_t inNode, uint32_t inPort) {
            pw::Link l;
            l.id = id;
            l.outNode = outNode;
            l.outPort = outPort;
            l.inNode = inNode;
            l.inPort = inPort;
            return l;
        };

        pw::Graph settled;
        for (auto n : {node(100, "rostrum.game", "Audio/Sink", 9000), node(200, "rostrum.stream", "Audio/Sink", 9000)}) {
            n.channels = 2;
            settled.nodes.insert(n.id, n);
        }
        for (const auto &p : {port(110, 100, false, "FL"), port(111, 100, false, "FR"), port(112, 100, true, "FL"),
                              port(113, 100, true, "FR"), port(210, 200, false, "FL"), port(211, 200, false, "FR"),
                              port(212, 200, true, "FL"), port(213, 200, true, "FR")}) {
            settled.ports.insert(p.id, p);
        }
        settled.links.insert(930, link(930, 100, 112, 200, 210));
        settled.links.insert(931, link(931, 100, 113, 200, 211));

        engine.setBusDestination(QStringLiteral("game"), Destination::Stream);
        engine.createMix();
        pw.testingInstallGraph(settled);
        flush();
        // Both links are wanted as they are.
        QVERIFY(!pw.testingDestroyRequests().contains(930));
        QVERIFY(!pw.testingDestroyRequests().contains(931));

        // Half way through: the left ports and their link are gone, the right ones not yet.
        pw::Graph losing = settled;
        for (uint32_t id : {110u, 111u, 112u}) {
            losing.ports.remove(id);
        }
        losing.links.remove(930);
        pw.testingInstallGraph(losing);
        flush();
        QVERIFY2(!pw.testingDestroyRequests().contains(931),
                 "The link on a bus that is losing its ports must be left to PipeWire");

        // A link that is really unwanted still goes: the bus stops feeding the stream.
        pw.testingInstallGraph(settled);
        flush();
        engine.setBusDestination(QStringLiteral("game"), Destination::Phones);
        flush();
        QVERIFY(pw.testingDestroyRequests().contains(930));
        QVERIFY(pw.testingDestroyRequests().contains(931));
    }

    void micFxQuantumFollowsSettings()
    {
        using engine::micFxQuantum;
        QCOMPARE(micFxQuantum(QString(), QString()), engine::kMicFxQuantum);
        QCOMPARE(micFxQuantum(QStringLiteral("32"), QStringLiteral("2048")), 512u);
        // The user's own floor wins when it is higher; their ceiling when it is lower.
        QCOMPARE(micFxQuantum(QStringLiteral("1024"), QStringLiteral("2048")), 1024u);
        QCOMPARE(micFxQuantum(QString(), QStringLiteral("256")), 256u);
        QCOMPARE(micFxQuantum(QStringLiteral("1024"), QStringLiteral("512")), 512u);
        QCOMPARE(micFxQuantum(QStringLiteral("junk"), QStringLiteral("0")), 512u);

        QCOMPARE(engine::micFxProperties(512).value(QStringLiteral("node.force-quantum")), QStringLiteral("512"));
        QVERIFY(!engine::micFxProperties(0).contains(QStringLiteral("node.force-quantum")));
    }

    void micFxNodeIsReplacedWhenItsQuantumIsStale()
    {
        auto flush = [] {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::Timer);
            QCoreApplication::processEvents();
        };
        auto graphWith = [](const QString &forceQuantum) {
            pw::Graph g = devices();
            g.factories.insert(900, QStringLiteral("spa-node-factory"));
            pw::Node fx = node(150, "rostrum.micfx", "");
            fx.props.insert(QStringLiteral("rostrum.role"), QStringLiteral("micfx"));
            fx.props.insert(QStringLiteral("factory.name"), QStringLiteral("audio.convert"));
            fx.props.insert(QStringLiteral("convert.direction"), QStringLiteral("output"));
            if (!forceQuantum.isEmpty()) {
                fx.props.insert(QStringLiteral("node.force-quantum"), forceQuantum);
            }
            g.nodes.insert(fx.id, fx);
            return g;
        };

        pw::PwContext pw; // never started
        engine::Engine engine(&pw);
        engine.setMicFilterPlugin(QStringLiteral("/opt/rostrum/lib/rostrum/librostrum-dsp.so"), true, QString());
        micfx::Settings s;
        s.enabled = true;
        engine.setMicFilters(s);
        engine.createMix();

        // A node from 0.3.0 forces nothing: it is replaced.
        pw.testingInstallGraph(graphWith(QString()));
        flush();
        QVERIFY(pw.testingDestroyRequests().contains(150));

        // One that forces the wanted cycle is kept.
        pw::PwContext current;
        engine::Engine kept(&current);
        kept.setMicFilterPlugin(QStringLiteral("/opt/rostrum/lib/rostrum/librostrum-dsp.so"), true, QString());
        kept.setMicFilters(s);
        kept.createMix();
        current.testingInstallGraph(graphWith(QStringLiteral("512")));
        flush();
        QVERIFY(!current.testingDestroyRequests().contains(150));

        // Raising clock.min-quantum above it makes it stale.
        current.testingSetSetting(QStringLiteral("clock.min-quantum"), QStringLiteral("1024"));
        flush();
        QVERIFY(current.testingDestroyRequests().contains(150));

        // Without noise removal nothing is forced, so a forcing node is replaced too.
        pw::PwContext plain;
        engine::Engine noDenoise(&plain);
        noDenoise.setMicFilterPlugin(QStringLiteral("/opt/rostrum/lib/rostrum/librostrum-dsp.so"), false, QString());
        noDenoise.setMicFilters(s);
        noDenoise.createMix();
        plain.testingInstallGraph(graphWith(QStringLiteral("512")));
        flush();
        QVERIFY(plain.testingDestroyRequests().contains(150));
    }

    void micFilterSettingsAndAppChoices()
    {
        pw::PwContext pw; // never started
        engine::Engine engine(&pw);
        QSignalSpy changed(&engine, &engine::Engine::micFiltersChanged);

        // Without a plugin there is a reason to show instead of the feature.
        QVERIFY(!engine.micFiltersUnavailable().isEmpty());
        engine.setMicFilterPlugin(QString(), true, QStringLiteral("The plugin is missing."));
        QCOMPARE(engine.micFiltersUnavailable(), QStringLiteral("The plugin is missing."));
        QVERIFY(!engine.micFiltersHaveDenoise());
        engine.setMicFilterPlugin(QStringLiteral("/opt/rostrum/lib/rostrum/librostrum-dsp.so"), true, QString());
        QVERIFY(engine.micFiltersUnavailable().isEmpty());
        QVERIFY(engine.micFiltersHaveDenoise());

        // Settings are sanitized, and only a real change is announced.
        micfx::Settings s;
        s.enabled = true;
        s.compRatio = 500;
        engine.setMicFilters(s);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(engine.micFilters(), micfx::sanitize(s));
        QVERIFY(engine.micFilters().compRatio < 500);
        engine.setMicFilters(s);
        QCOMPARE(changed.count(), 1);

        const AppKey discord{MatchKey::Binary, QStringLiteral("Discord")};
        engine.setMicAppChoice(discord, micfx::AppChoice::Raw);
        QCOMPARE(changed.count(), 2);
        QCOMPARE(micfx::appChoice(engine.micFilters(), discord.toString()), micfx::AppChoice::Raw);
        engine.setMicAppChoice(discord, micfx::AppChoice::Raw);
        QCOMPARE(changed.count(), 2);
        engine.setMicAppChoice(discord, micfx::AppChoice::Default);
        QCOMPARE(micfx::appChoice(engine.micFilters(), discord.toString()), micfx::AppChoice::Default);
        QVERIFY(engine.micFilters().rawApps.isEmpty());

        // Nothing runs without an audio server or a mix: the meter reads the plain mic.
        QCOMPARE(engine.micFiltersState(), engine::Engine::MicFxState::Off);
        QVERIFY(engine.micApps().isEmpty());
        QCOMPARE(engine.micMeterNode(), engine.resolvedSourceName());
    }

    void micCheckVerdict()
    {
        using namespace pw::miccheck;
        QCOMPARE(verdict(toDb(0.0f)), Verdict::Silent);
        QCOMPARE(verdict(toDb(0.0005f)), Verdict::Silent); // about -66 dBFS: room noise
        QCOMPARE(verdict(toDb(0.03f)), Verdict::Quiet);    // about -30 dBFS
        QCOMPARE(verdict(toDb(0.5f)), Verdict::Good);      // about -6 dBFS
        // The mic limiter's default -1 dB ceiling is a good level, not clipping.
        QCOMPARE(verdict(-1.0), Verdict::Good);
        QCOMPARE(verdict(toDb(1.0f)), Verdict::Loud);
        QCOMPARE(verdict(toDb(1.4f)), Verdict::Loud);
        QVERIFY(std::isinf(toDb(0.0f)));
    }

    void sidetoneTurnedOnIsAudible()
    {
        pw::PwContext pw;
        engine::Engine engine(&pw);
        Scene scene = engine.scene();
        scene.sidetoneVolume = 0.0;
        engine.setScene(scene);
        engine.setSidetoneEnabled(true);
        QVERIFY(engine.sidetoneEnabled());
        QCOMPARE(engine.scene().sidetoneVolume, engine::Engine::kDefaultSidetoneVolume);
        // A level the user chose is kept.
        engine.setSidetoneVolume(0.8);
        engine.setSidetoneEnabled(false);
        engine.setSidetoneEnabled(true);
        QCOMPARE(engine.scene().sidetoneVolume, 0.8);
    }

    void linkErrorNotClearedWithoutAReconcile()
    {
        // Never connected: no pass can see the link resolved, so the error must stay, and
        // Try Again (createMix) still clears it.
        pw::PwContext pw;
        engine::Engine engine(&pw);
        engine.createMix();
        const QString msg = QStringLiteral("Invalid argument link-factory: can't link ports 94 and 108");
        Q_EMIT pw.createFailed(msg, QString());
        QCoreApplication::processEvents(); // the reconcile the failure schedules
        QCOMPARE(engine.mixError(), msg);
        engine.createMix();
        QVERIFY(engine.mixError().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestEngine)
#include "tst_engine.moc"
