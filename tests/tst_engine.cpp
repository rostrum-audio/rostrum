#include "core/Ducking.h"
#include "core/SceneStore.h"
#include "core/SceneToml.h"
#include "core/Settings.h"
#include "engine/Engine.h"
#include "engine/MixPlan.h"
#include "engine/NodeSpecs.h"
#include "engine/SceneManager.h"
#include "pw/BluetoothProfiles.h"
#include "pw/Graph.h"
#include "pw/MicCheck.h"
#include "pw/PortConfig.h"
#include "pw/PwContext.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QEvent>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <unistd.h>

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

// A fake /proc/<pid> for a java process of this user running `mainClass`.
void writeJavaProcess(const QString &procRoot, qint64 pid, const QString &nsPids, const QByteArray &mainClass,
                      const QString &flatpak = {})
{
    const auto write = [](const QString &path, const QByteArray &contents) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(contents);
    };
    const QString d = QStringLiteral("%1/%2").arg(procRoot).arg(pid);
    write(d + QStringLiteral("/status"),
          QStringLiteral("Name:\tjava\nUid:\t%1\t%1\t%1\t%1\nNSpid:\t%2\n").arg(::getuid()).arg(nsPids).toUtf8());
    write(d + QStringLiteral("/comm"), "java\n");
    write(d + QStringLiteral("/cmdline"), QByteArray("java\0-cp\0game.jar\0", 18) + mainClass + '\0');
    if (!flatpak.isEmpty()) {
        write(d + QStringLiteral("/root/.flatpak-info"), "[Application]\nname=" + flatpak.toUtf8() + '\n');
    }
}

// A Java app's playback stream, named the way the ALSA plug-in names it.
pw::Node javaStream(uint32_t id, qint64 pid)
{
    pw::Node n = node(id, "ALSA plug-in [java]", "Stream/Output/Audio");
    n.appName = QStringLiteral("ALSA plug-in [java]");
    n.binary = QStringLiteral("java");
    n.pid = pid;
    n.serial = QString::number(1000 + id);
    return n;
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

    void desiredSendsAddChatSend()
    {
        Scene scene = defaults::scene();
        scene.bus(QStringLiteral("alerts"))->chat = true;
        const auto sends = engine::desiredSends(scene);
        QCOMPARE(sends.size(), 11);
        int chat = 0;
        for (qsizetype i = 0; i < sends.size(); ++i) {
            if (sends.at(i).name == QStringLiteral("rostrum.alerts.chat")) {
                ++chat;
                QVERIFY(sends.at(i).chat);
                QCOMPARE(sends.at(i).busId, QStringLiteral("alerts"));
                QCOMPARE(sends.at(i - 1).name, QStringLiteral("rostrum.alerts.stream"));
                QCOMPARE(sends.at(i - 2).name, QStringLiteral("rostrum.alerts.phones"));
            } else {
                QVERIFY(!sends.at(i).chat);
            }
            QVERIFY(sends.at(i).busId != QStringLiteral("mic"));
        }
        QCOMPARE(chat, 1);
    }

    void chatSendRefusedOnVoiceBus()
    {
        Scene scene = defaults::scene();
        scene.bus(QStringLiteral("voice"))->chat = true; // a stale saved flag
        QCOMPARE(engine::desiredSends(scene).size(), 10);
        QVERIFY(!linkNames(engine::busLinks(scene, true)).join(QLatin1Char(' ')).contains(QStringLiteral(".chat")));
        QVERIFY(!engine::chatSendLevels(scene, {}).contains(QStringLiteral("rostrum.voice.chat")));
    }

    void busLinksChat()
    {
        Scene scene = defaults::scene();
        scene.bus(QStringLiteral("alerts"))->chat = true;
        const QStringList links = linkNames(engine::busLinks(scene, true));
        QVERIFY(links.contains(QStringLiteral("rostrum.alerts>rostrum.alerts.chat")));
        QVERIFY(links.contains(QStringLiteral("rostrum.alerts.chat>rostrum.chatmic")));
        for (const QString &l : links) {
            if (l.endsWith(QStringLiteral(">rostrum.chatmic"))) {
                QCOMPARE(l, QStringLiteral("rostrum.alerts.chat>rostrum.chatmic"));
            }
            QVERIFY(!l.endsWith(QStringLiteral(">rostrum.mic")));
            QVERIFY(!l.endsWith(QStringLiteral(">rostrum.filtered")));
        }
    }

    void busLinksChatSingleLevel()
    {
        Scene scene = defaults::scene();
        scene.bus(QStringLiteral("alerts"))->chat = true;
        for (const QString &l : linkNames(engine::busLinks(scene, false))) {
            QVERIFY2(!l.contains(QStringLiteral("chat")), qPrintable(l));
        }
    }

    void chatSendLevelsHalfGainAndBlock()
    {
        Scene scene = defaults::scene();
        Bus *alerts = scene.bus(QStringLiteral("alerts"));
        alerts->chat = true;
        alerts->chatVolume = 0.4;
        const QString name = QStringLiteral("rostrum.alerts.chat");
        auto lv = engine::chatSendLevels(scene, {});
        QCOMPARE(lv.size(), 1);
        QCOMPARE(lv.value(name).position, 0.4);
        QCOMPARE(lv.value(name).gain, 0.5);
        QCOMPARE(lv.value(name).balance, 0.0);
        QVERIFY(!lv.value(name).mute);
        QVERIFY(engine::chatSendLevels(scene, {QStringLiteral("alerts")}).value(name).mute);
        QVERIFY(!engine::chatSendLevels(scene, {QStringLiteral("game")}).value(name).mute);
        // Muted keeps the send and its links, only the level changes.
        alerts->chatMuted = true;
        QVERIFY(engine::chatSendLevels(scene, {}).value(name).mute);
        QVERIFY(engine::sendWanted(scene, name));
        QVERIFY(linkNames(engine::busLinks(scene, true)).contains(QStringLiteral("rostrum.alerts.chat>rostrum.chatmic")));
        // Solo never touches it, and busNodeLevels does not carry it.
        alerts->chatMuted = false;
        const auto bl = engine::busNodeLevels(scene, {QStringLiteral("game")}, {}, 1.0, true);
        QVERIFY(!bl.contains(name));
        QVERIFY(!engine::chatSendLevels(scene, {}).value(name).mute);
    }

    void chatLoopFound()
    {
        Scene scene = defaults::scene();
        scene.bus(QStringLiteral("alerts"))->chat = true;
        QMultiHash<QString, QString> byBus;
        byBus.insert(QStringLiteral("alerts"), QStringLiteral("discord"));
        byBus.insert(QStringLiteral("game"), QStringLiteral("quake"));
        const QSet<QString> got = engine::chatLoopBuses(scene, byBus, {QStringLiteral("discord")});
        QCOMPARE(got, QSet<QString>{QStringLiteral("alerts")});
    }

    void chatBlockedBusNamesAreDisplayNames()
    {
        Scene scene = defaults::scene();
        scene.bus(QStringLiteral("alerts"))->name = QStringLiteral("Stream Alerts");
        const QStringList got = engine::busDisplayNames(
            scene, {QStringLiteral("alerts"), QStringLiteral("game"), QStringLiteral("nosuchbus")});
        QCOMPARE(got, (QStringList{QStringLiteral("Game"), QStringLiteral("Stream Alerts")}));
        QVERIFY(engine::busDisplayNames(scene, {}).isEmpty());
    }

    void chatLoopIgnoresBusWithoutChat()
    {
        Scene scene = defaults::scene();
        scene.bus(QStringLiteral("alerts"))->chat = false;
        QMultiHash<QString, QString> byBus;
        byBus.insert(QStringLiteral("alerts"), QStringLiteral("discord"));
        QVERIFY(engine::chatLoopBuses(scene, byBus, {QStringLiteral("discord")}).isEmpty());
    }

    void chatLoopEmptyWhenAppNotOnChatMic()
    {
        Scene scene = defaults::scene();
        scene.bus(QStringLiteral("alerts"))->chat = true;
        QMultiHash<QString, QString> byBus;
        byBus.insert(QStringLiteral("alerts"), QStringLiteral("discord"));
        QVERIFY(engine::chatLoopBuses(scene, byBus, {QStringLiteral("obs")}).isEmpty());
        QVERIFY(engine::chatLoopBuses(scene, byBus, {}).isEmpty());
    }

    void sendWantedKnowsChatSend()
    {
        Scene scene = defaults::scene();
        const QString name = QStringLiteral("rostrum.alerts.chat");
        QVERIFY(!engine::sendWanted(scene, name));
        scene.bus(QStringLiteral("alerts"))->chat = true;
        QVERIFY(engine::sendWanted(scene, name));
        const auto p = engine::chatSendProperties(*scene.bus(QStringLiteral("alerts")));
        QCOMPARE(p.value(QStringLiteral("node.name")), name);
        QCOMPARE(p.value(QStringLiteral("rostrum.mix")), QStringLiteral("chat"));
        QCOMPARE(p.value(QStringLiteral("rostrum.role")), QStringLiteral("send"));
        QCOMPARE(p.value(QStringLiteral("node.description")),
                 QStringLiteral("Rostrum %1 → Voice chat").arg(scene.bus(QStringLiteral("alerts"))->name));
    }

    void roleNameChatMic()
    {
        QCOMPARE(engine::roleName(engine::NodeRole::ChatMic), QStringLiteral("chatmic"));
        // Kept apart from the sends, whose rostrum.mix is "chat".
        QVERIFY(engine::roleName(engine::NodeRole::ChatMic) != engine::roleName(engine::NodeRole::Send));
    }

    void chatMicSpecIsMonoSource()
    {
        const engine::NodeSpec spec = engine::chatMicSpec();
        QCOMPARE(spec.role, engine::NodeRole::ChatMic);
        const auto p = spec.properties();
        QCOMPARE(p.value(QStringLiteral("node.name")), QStringLiteral("rostrum.chatmic"));
        QCOMPARE(p.value(QStringLiteral("node.description")), QStringLiteral("Rostrum Chat Mic"));
        QCOMPARE(p.value(QStringLiteral("media.class")), QStringLiteral("Audio/Source/Virtual"));
        QCOMPARE(p.value(QStringLiteral("audio.position")), QStringLiteral("[ MONO ]"));
        QCOMPARE(p.value(QStringLiteral("audio.channels")), QStringLiteral("1"));
        QCOMPARE(p.value(QStringLiteral("object.linger")), QStringLiteral("true"));
        QCOMPARE(p.value(QStringLiteral("priority.session")), QStringLiteral("0"));
        QCOMPARE(p.value(QStringLiteral("rostrum.role")), QStringLiteral("chatmic"));
        for (auto it = p.cbegin(); it != p.cend(); ++it) {
            QVERIFY2(!it.key().startsWith(QStringLiteral("node.dont-")), qPrintable(it.key()));
        }
    }

    void setBusChatRefusedOnVoiceBus()
    {
        pw::PwContext pw; // never started
        engine::Engine engine(&pw);
        QSignalSpy changed(&engine, &engine::Engine::sceneChanged);
        // Voice chat would hear itself from the bus that plays it.
        QVERIFY(!engine.setBusChat(QStringLiteral("voice"), true));
        QVERIFY(!engine.scene().bus(QStringLiteral("voice"))->chat);
        QVERIFY(!engine.setBusChat(QStringLiteral("mic"), true));
        QVERIFY(!engine.scene().micBus()->chat);
        QVERIFY(!engine.setBusChat(QStringLiteral("nosuchbus"), true));
        QCOMPARE(changed.count(), 0);
        QVERIFY(engine.setBusChat(QStringLiteral("alerts"), true));
        QVERIFY(engine.scene().bus(QStringLiteral("alerts"))->chat);
        QCOMPARE(changed.count(), 1);
        // A bus that starts receiving Voice is refused from then on.
        engine.setBusAutoCategory(QStringLiteral("music"), AppCategory::Voice);
        QVERIFY(!engine.setBusChat(QStringLiteral("music"), true));
        QVERIFY(!engine.scene().bus(QStringLiteral("music"))->chat);
    }

    void setBusChatOffAllowedOnVoiceBus()
    {
        pw::PwContext pw; // never started
        engine::Engine engine(&pw);
        const QString music = QStringLiteral("music");
        QVERIFY(engine.setBusChat(music, true));
        // The bus starts receiving Voice after chat was on: it must still be able to drop it.
        engine.setBusAutoCategory(music, AppCategory::Voice);
        QVERIFY(engine.scene().bus(music)->chat);
        QVERIFY(engine.setBusChat(music, false));
        QVERIFY(!engine.scene().bus(music)->chat);
        QVERIFY(!engine.setBusChat(music, true));
    }

    void setBusChatMakesSceneDirty()
    {
        QTemporaryDir dir;
        pw::PwContext pw; // never started
        engine::Engine engine(&pw);
        engine::SceneManager scenes(&engine, dir.path());
        scenes.load(QStringLiteral("Live"));
        const QString alerts = QStringLiteral("alerts");
        QVERIFY(!scenes.dirty());
        QVERIFY(engine.setBusChat(alerts, true));
        QVERIFY(scenes.dirty());
        QVERIFY(engine.setBusChat(alerts, false));
        QVERIFY(!scenes.dirty());

        // The live switch is saved too, and turning the destination on starts it audible.
        engine.setBusChatMuted(alerts, true);
        QVERIFY(engine.scene().bus(alerts)->chatMuted);
        QVERIFY(scenes.dirty());
        QVERIFY(engine.setBusChat(alerts, true));
        QVERIFY(!engine.scene().bus(alerts)->chatMuted);
        engine.setBusChatMuted(alerts, true);
        QVERIFY(engine.scene().bus(alerts)->chat);
        QVERIFY(engine.scene().bus(alerts)->chatMuted);

        // Turning the destination off clears the live mute, so a file never carries chat_muted alone.
        QVERIFY(engine.setBusChat(alerts, false));
        QVERIFY(!engine.scene().bus(alerts)->chatMuted);
        QVERIFY(!scenes.dirty());
        // The level is clamped like the others.
        engine.setBusChatVolume(alerts, 3.0);
        QCOMPARE(engine.scene().bus(alerts)->chatVolume, 1.0);
        QVERIFY(scenes.dirty());
        engine.setBusChatVolume(alerts, -1.0);
        QCOMPARE(engine.scene().bus(alerts)->chatVolume, 0.0);
        engine.setBusChatVolume(alerts, kDefaultChatVolume);
        QVERIFY(!scenes.dirty());

        // The mic has no voice chat destination.
        engine.setBusChatMuted(QStringLiteral("mic"), true);
        engine.setBusChatVolume(QStringLiteral("mic"), 0.2);
        QVERIFY(!engine.scene().micBus()->chatMuted);
        QCOMPARE(engine.scene().micBus()->chatVolume, kDefaultChatVolume);
        QVERIFY(!scenes.dirty());
    }

    void newBusCannotUseChatMicName()
    {
        pw::PwContext pw;
        engine::Engine engine(&pw);
        const QString id = engine.addBus(QStringLiteral("Chatmic"));
        QVERIFY(!id.isEmpty());
        QVERIFY(id != QString::fromLatin1(kChatMicBusId));
        QVERIFY(engine.scene().bus(id)->nodeName() != QString::fromLatin1(engine::kChatMicNode));
    }

    // The chat mic is one lingering node: a duplicate from a create race goes, the oldest stays.
    // Voice reaches it from rostrum.filtered; any other link between Rostrum nodes into it goes.
    void chatMicIsFedFromFilteredAndKeepsTheOldest()
    {
        auto flush = [] {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::Timer);
            QCoreApplication::processEvents();
        };
        auto monoSource = [](uint32_t id, const char *name, const char *role) {
            pw::Node n = node(id, name, "Audio/Source/Virtual");
            n.channels = 1;
            n.props.insert(QStringLiteral("rostrum.role"), QString::fromLatin1(role));
            n.serial = QString::number(5000 + id);
            return n;
        };
        auto link = [](uint32_t id, uint32_t outNode, uint32_t outPort, uint32_t inNode, uint32_t inPort) {
            pw::Link l;
            l.id = id;
            l.outNode = outNode;
            l.outPort = outPort;
            l.inNode = inNode;
            l.inPort = inPort;
            l.serial = QString::number(6000 + id);
            return l;
        };
        pw::Graph g = devices();
        g.factories.insert(900, QStringLiteral("spa-node-factory"));
        for (const auto &n : {monoSource(160, "rostrum.filtered", "filtered"),
                              monoSource(170, "rostrum.chatmic", "chatmic"),
                              monoSource(171, "rostrum.chatmic", "chatmic")}) {
            g.nodes.insert(n.id, n);
        }
        pw::Node vod = node(180, "rostrum.vod", "Audio/Sink");
        vod.channels = 2;
        g.nodes.insert(vod.id, vod);
        for (auto p : {port(1600, 160, false, "MONO"), port(1601, 160, true, "MONO"),
                       port(1700, 170, false, "MONO"), port(1701, 170, true, "MONO"),
                       port(1710, 171, false, "MONO"), port(1711, 171, true, "MONO"),
                       port(1800, 180, false, "FL"), port(1801, 180, false, "FR"),
                       port(1802, 180, true, "FL"), port(1803, 180, true, "FR")}) {
            p.monitor = p.nodeId == 180 && p.output;
            g.ports.insert(p.id, p);
        }

        pw::PwContext pw; // never started
        engine::Engine engine(&pw);
        QSignalSpy changed(&engine, &engine::Engine::chatMicChanged);
        engine.setChatMicWanted(true);
        engine.createMix();
        pw.testingInstallGraph(g);
        flush();
        QVERIFY(pw.testingDestroyRequests().contains(171));
        QVERIFY(!pw.testingDestroyRequests().contains(170));
        QVERIFY(engine.chatMicNode());
        QCOMPARE(engine.chatMicNode()->id, 170u);
        // Not in use until it carries the voice.
        QVERIFY(!engine.chatMicActive());
        QCOMPARE(changed.count(), 0);

        g.nodes.remove(171);
        g.ports.remove(1710);
        g.ports.remove(1711);
        g.links.insert(950, link(950, 160, 1601, 170, 1700));
        g.links.insert(951, link(951, 180, 1802, 170, 1700)); // the VOD master: never voice chat
        pw.testingInstallGraph(g);
        flush();
        QVERIFY(engine.chatMicActive());
        QCOMPARE(changed.count(), 1);
        QVERIFY(!pw.testingDestroyRequests().contains(950));
        QVERIFY(pw.testingDestroyRequests().contains(951));

        // No longer wanted, with no app recording it: it goes at once, and its links with it.
        engine.setChatMicWanted(false);
        flush();
        QVERIFY(pw.testingDestroyRequests().contains(170));
        QVERIFY(!pw.testingDestroyRequests().contains(950));
        QVERIFY(!engine.chatMicNode());
    }

    // Once no saved scene wants the chat mic, apps are moved off it first: it stays, carrying the
    // voice, until none is left on it, or for 3 s at most.
    void chatMicWaitsForAppsBeforeItGoes()
    {
        auto flush = [] {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::Timer);
            QCoreApplication::processEvents();
        };
        auto monoSource = [](uint32_t id, const char *name, const char *role) {
            pw::Node n = node(id, name, "Audio/Source/Virtual");
            n.channels = 1;
            n.props.insert(QStringLiteral("rostrum.role"), QString::fromLatin1(role));
            n.serial = QString::number(5000 + id);
            return n;
        };
        auto link = [](uint32_t id, uint32_t outNode, uint32_t outPort, uint32_t inNode, uint32_t inPort) {
            pw::Link l;
            l.id = id;
            l.outNode = outNode;
            l.outPort = outPort;
            l.inNode = inNode;
            l.inPort = inPort;
            l.serial = QString::number(6000 + id);
            return l;
        };
        pw::Graph g = devices();
        g.factories.insert(900, QStringLiteral("spa-node-factory"));
        for (const auto &n : {monoSource(160, "rostrum.filtered", "filtered"),
                              monoSource(170, "rostrum.chatmic", "chatmic")}) {
            g.nodes.insert(n.id, n);
        }
        pw::Node discord = node(200, "Discord", "Stream/Input/Audio");
        discord.appName = QStringLiteral("Discord");
        discord.binary = QStringLiteral("Discord");
        discord.serial = QStringLiteral("7200");
        g.nodes.insert(discord.id, discord);
        for (auto p : {port(1600, 160, false, "MONO"), port(1601, 160, true, "MONO"),
                       port(1700, 170, false, "MONO"), port(1701, 170, true, "MONO"),
                       port(2000, 200, false, "MONO")}) {
            g.ports.insert(p.id, p);
        }
        g.links.insert(950, link(950, 160, 1601, 170, 1700));
        g.links.insert(952, link(952, 170, 1701, 200, 2000)); // Discord records the chat mic

        // Held while the app is on it; gone once it has moved.
        {
            pw::PwContext pw; // never started
            engine::Engine engine(&pw);
            engine.setChatMicWanted(true);
            engine.createMix();
            pw.testingInstallGraph(g);
            flush();
            QVERIFY(engine.chatMicActive());
            // With Studio Voice off it is listed, not as filtered.
            const QList<engine::MicApp> apps = engine.micApps();
            QCOMPARE(apps.size(), 1);
            QCOMPARE(apps.first().nodeId, 200u);
            QVERIFY(!apps.first().filtered);

            engine.setChatMicWanted(false);
            flush();
            QVERIFY(!pw.testingDestroyRequests().contains(170));
            QVERIFY(engine.chatMicNode());

            pw::Graph moved = g;
            moved.links.remove(952);
            pw.testingInstallGraph(moved);
            flush();
            QVERIFY(pw.testingDestroyRequests().contains(170));
        }
        // An app that stays does not keep it past the wait.
        {
            pw::PwContext pw; // never started
            engine::Engine engine(&pw);
            engine.setChatMicWanted(true);
            engine.createMix();
            pw.testingInstallGraph(g);
            flush();
            engine.setChatMicWanted(false);
            flush();
            QVERIFY(!pw.testingDestroyRequests().contains(170));
            QTRY_VERIFY_WITH_TIMEOUT(pw.testingDestroyRequests().contains(170), 5000);
        }
    }

    // An app that plays on a bus with voice chat and records the chat mic would hear itself.
    // The engine mutes that bus's chat send, and clears it when the recording stops.
    void chatLoopMutesTheBusWhileTheAppRecordsTheChatMic()
    {
        auto flush = [] {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::Timer);
            QCoreApplication::processEvents();
        };
        auto monoSource = [](uint32_t id, const char *name, const char *role) {
            pw::Node n = node(id, name, "Audio/Source/Virtual");
            n.channels = 1;
            n.props.insert(QStringLiteral("rostrum.role"), QString::fromLatin1(role));
            n.serial = QString::number(5000 + id);
            return n;
        };
        auto link = [](uint32_t id, uint32_t outNode, uint32_t outPort, uint32_t inNode, uint32_t inPort) {
            pw::Link l;
            l.id = id;
            l.outNode = outNode;
            l.outPort = outPort;
            l.inNode = inNode;
            l.inPort = inPort;
            l.serial = QString::number(6000 + id);
            return l;
        };
        pw::Graph g = devices();
        g.factories.insert(900, QStringLiteral("spa-node-factory"));
        for (const auto &n : {monoSource(160, "rostrum.filtered", "filtered"),
                              monoSource(170, "rostrum.chatmic", "chatmic")}) {
            g.nodes.insert(n.id, n);
        }
        pw::Node capture = node(200, "Discord", "Stream/Input/Audio");
        capture.appName = QStringLiteral("Discord");
        capture.binary = QStringLiteral("Discord");
        capture.serial = QStringLiteral("7200");
        g.nodes.insert(capture.id, capture);
        pw::Node playback = node(210, "Discord", "Stream/Output/Audio");
        playback.appName = QStringLiteral("Discord");
        playback.binary = QStringLiteral("Discord");
        playback.serial = QStringLiteral("7210");
        g.nodes.insert(playback.id, playback);
        for (auto p : {port(1600, 160, false, "MONO"), port(1601, 160, true, "MONO"),
                       port(1700, 170, false, "MONO"), port(1701, 170, true, "MONO"),
                       port(2000, 200, false, "MONO")}) {
            g.ports.insert(p.id, p);
        }
        g.links.insert(950, link(950, 160, 1601, 170, 1700));
        g.links.insert(952, link(952, 170, 1701, 200, 2000));

        pw::PwContext pw;
        engine::Engine engine(&pw);
        QSignalSpy looped(&engine, &engine::Engine::chatLoopChanged);
        engine.setChatMicWanted(true);
        engine.createMix();
        QVERIFY(engine.setBusChat(QStringLiteral("alerts"), true));
        engine.assignApp({MatchKey::Name, QStringLiteral("Discord")}, QStringLiteral("alerts"), true);
        pw.testingInstallGraph(g);
        flush();
        QVERIFY(engine.chatBlocked(QStringLiteral("alerts")));
        QCOMPARE(engine.chatLoopApps(), QStringList{QStringLiteral("Discord")});
        QCOMPARE(engine.chatBlockedBusNames(), QStringList{QStringLiteral("Alerts")});
        QVERIFY(looped.count() >= 1);
        const int blocked = looped.count();

        g.nodes.remove(200);
        g.ports.remove(2000);
        g.links.remove(952);
        pw.testingInstallGraph(g);
        flush();
        QVERIFY(!engine.chatBlocked(QStringLiteral("alerts")));
        QVERIFY(engine.chatLoopApps().isEmpty());
        QVERIFY(looped.count() > blocked);
    }

    // A failed create is asked for again after the retry time, without waiting for a graph event.
    void chatMicCreateIsRetriedAfterAFailure()
    {
        struct CountingEngine : engine::Engine
        {
            using engine::Engine::Engine;
            int passes = 0;
            void reconcile() override
            {
                ++passes;
                engine::Engine::reconcile();
            }
        };
        pw::PwContext pw; // never started
        CountingEngine engine(&pw);
        engine.createMix();
        QTRY_VERIFY(engine.passes >= 1);
        const int before = engine.passes;
        Q_EMIT pw.createFailed(QStringLiteral("No memory"), QString::fromLatin1(engine::kChatMicNode));
        QTest::qWait(1000);
        QCOMPARE(engine.passes, before);
        QTRY_VERIFY_WITH_TIMEOUT(engine.passes > before, 4000);
    }

    void chatNeedsPerMixLevels()
    {
        struct ChatEngine : engine::Engine
        {
            using engine::Engine::Engine;
            using engine::Engine::chatMicNeeded; // whether the chat mic is created
        };
        pw::PwContext pw; // never started
        ChatEngine engine(&pw);
        QSignalSpy changed(&engine, &engine::Engine::chatMicChanged);
        // Unknown until PipeWire is connected.
        QVERIFY(engine.chatUnavailable().isEmpty());
        engine.setChatMicWanted(true);
        engine.createMix();
        QVERIFY(!engine.chatMicNeeded());
        pw.testingInstallGraph(devices()); // lists no spa-node-factory
        QTRY_VERIFY(changed.count() >= 1);
        QVERIFY(engine.chatUnavailable().contains(engine.perMixUnavailable()));
        QVERIFY(!engine.chatMicNeeded());
        QVERIFY(!engine.chatMicNode());
        QVERIFY(!engine.chatMicActive());
        QCOMPARE(changed.count(), 1);

        pw::Graph g = devices();
        g.factories.insert(900, QStringLiteral("spa-node-factory"));
        pw.testingInstallGraph(g);
        QTRY_COMPARE(changed.count(), 2);
        QVERIFY(engine.chatUnavailable().isEmpty());
        QVERIFY(engine.chatMicNeeded());
        // Only while a saved scene has voice chat, and only while the mix runs.
        engine.setChatMicWanted(false);
        QVERIFY(!engine.chatMicNeeded());
        engine.setChatMicWanted(true);
        engine.destroyMix();
        QVERIFY(!engine.chatMicNeeded());
    }

    void chatMicCreateFailureIsNotAMixError()
    {
        pw::PwContext pw; // never started
        engine::Engine engine(&pw);
        engine.createMix();
        Q_EMIT pw.createFailed(QStringLiteral("No memory"), QString::fromLatin1(engine::kChatMicNode));
        QCoreApplication::processEvents();
        QVERIFY(engine.mixError().isEmpty());
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

    void busNodeLevelsPerMix()
    {
        Scene s = defaults::scene();
        Bus *game = s.bus(QStringLiteral("game"));
        game->volume = 0.5;
        game->phonesVolume = 0.8;
        game->balance = 0.4;
        const auto levels = engine::busNodeLevels(s, {}, {}, 1.0, true);
        const auto bus = levels.value(QStringLiteral("rostrum.game"));
        QCOMPARE(bus.position, 1.0);
        QCOMPARE(bus.balance, 0.4);
        QCOMPARE(bus.mute, false);
        QCOMPARE(levels.value(QStringLiteral("rostrum.game.phones")).position, 0.8);
        QCOMPARE(levels.value(QStringLiteral("rostrum.game.stream")).position, 0.5);
        QCOMPARE(levels.value(QStringLiteral("rostrum.game.phones")).balance, 0.0);
        QVERIFY(!levels.contains(QStringLiteral("rostrum.mic")));
    }

    void soloDimsHeadphonesOnly()
    {
        Scene s = defaults::scene();
        s.bus(QStringLiteral("music"))->destination = Destination::Both; // Stream only by default
        const auto levels = engine::busNodeLevels(s, {QStringLiteral("music")}, {}, 1.0, true);
        QCOMPARE(levels.value(QStringLiteral("rostrum.game.phones")).mute, true);
        QCOMPARE(levels.value(QStringLiteral("rostrum.game.stream")).mute, false);
        QCOMPARE(levels.value(QStringLiteral("rostrum.game")).mute, false);
        QCOMPARE(levels.value(QStringLiteral("rostrum.music.phones")).mute, false);
    }

    void soloSingleLevelUnchanged()
    {
        Scene s = defaults::scene();
        s.bus(QStringLiteral("game"))->volume = 0.5;
        const auto levels = engine::busNodeLevels(s, {QStringLiteral("music")}, {}, 1.0, false);
        QCOMPARE(levels.value(QStringLiteral("rostrum.game")).mute, true);
        QCOMPARE(levels.value(QStringLiteral("rostrum.game")).position, 0.5);
        QCOMPARE(levels.value(QStringLiteral("rostrum.music")).mute, false);
        QVERIFY(!levels.contains(QStringLiteral("rostrum.game.phones")));
        QVERIFY(!levels.contains(QStringLiteral("rostrum.game.stream")));
    }

    void sendsMutedWhereTheBusDoesNotGo()
    {
        Scene s = defaults::scene();
        s.bus(QStringLiteral("game"))->destination = Destination::Phones;
        s.bus(QStringLiteral("music"))->destination = Destination::Stream;
        const auto levels = engine::busNodeLevels(s, {}, {}, 1.0, true);
        QCOMPARE(levels.value(QStringLiteral("rostrum.game.phones")).mute, false);
        QCOMPARE(levels.value(QStringLiteral("rostrum.game.stream")).mute, true);
        QCOMPARE(levels.value(QStringLiteral("rostrum.music.phones")).mute, true);
        QCOMPARE(levels.value(QStringLiteral("rostrum.music.stream")).mute, false);
    }

    void duckGainOnBusNode()
    {
        const Scene s = defaults::scene();
        const auto levels = engine::busNodeLevels(s, {}, {QStringLiteral("music")}, 0.25, true);
        QCOMPARE(levels.value(QStringLiteral("rostrum.music")).gain, 0.25);
        QCOMPARE(levels.value(QStringLiteral("rostrum.music.phones")).gain, 1.0);
        QCOMPARE(levels.value(QStringLiteral("rostrum.music.stream")).gain, 1.0);
        QCOMPARE(levels.value(QStringLiteral("rostrum.game")).gain, 1.0);
        const auto single = engine::busNodeLevels(s, {}, {QStringLiteral("music")}, 0.25, false);
        QCOMPARE(single.value(QStringLiteral("rostrum.music")).gain, 0.25);
    }

    void voiceTriggerScaledByLouderLevel()
    {
        Bus voice;
        voice.id = QStringLiteral("voice");
        voice.destination = Destination::Both;
        voice.volume = 0.5;
        voice.phonesVolume = 1.0;
        QVERIFY(qAbs(engine::voiceTriggerGain(voice, false, true) - 1.0) < 1e-9);
        QVERIFY(qAbs(engine::voiceTriggerGain(voice, true, true) - 0.125) < 1e-9);
        voice.destination = Destination::Stream;
        QVERIFY(qAbs(engine::voiceTriggerGain(voice, true, true) - 0.125) < 1e-9);
        voice.destination = Destination::Phones;
        QCOMPARE(engine::voiceTriggerGain(voice, true, true), 0.0);
        // Single level: the bus monitor is already after the fader and mute.
        voice.destination = Destination::Both;
        QCOMPARE(engine::voiceTriggerGain(voice, false, false), 1.0);
    }

    void setBusMixVolumeLinked()
    {
        pw::PwContext pw;
        engine::Engine engine(&pw);
        Scene s = engine.scene();
        Bus *game = s.bus(QStringLiteral("game"));
        game->phonesVolume = 0.6;
        game->volume = 0.2;
        game->linked = true;
        engine.setScene(s);
        QSignalSpy changed(&engine, &engine::Engine::sceneChanged);
        engine.setBusMixVolume(QStringLiteral("game"), Mix::Phones, 0.5);
        QCOMPARE(changed.count(), 1);
        QVERIFY(qAbs(engine.scene().bus(QStringLiteral("game"))->phonesVolume - 0.5) < 1e-9);
        QVERIFY(qAbs(engine.scene().bus(QStringLiteral("game"))->volume - 0.1) < 1e-9);

        engine.setScene(s);
        engine.setBusLinked(QStringLiteral("game"), false);
        QCOMPARE(engine.scene().bus(QStringLiteral("game"))->linked, false);
        engine.setBusMixVolume(QStringLiteral("game"), Mix::Phones, 0.5);
        QVERIFY(qAbs(engine.scene().bus(QStringLiteral("game"))->phonesVolume - 0.5) < 1e-9);
        QVERIFY(qAbs(engine.scene().bus(QStringLiteral("game"))->volume - 0.2) < 1e-9);

        engine.setBusVolume(QStringLiteral("game"), 0.7);
        QCOMPARE(engine.scene().bus(QStringLiteral("game"))->phonesVolume, 0.7);
        QCOMPARE(engine.scene().bus(QStringLiteral("game"))->volume, 0.7);

        // The mic has one level: the Mix variants leave it alone, setBusVolume is its gain.
        const double micGain = engine.scene().micBus()->volume;
        engine.setBusMixVolume(QStringLiteral("mic"), Mix::Phones, 0.3);
        engine.setBusLinked(QStringLiteral("mic"), false);
        QCOMPARE(engine.scene().micBus()->volume, micGain);
        QCOMPARE(engine.scene().micBus()->phonesVolume, 1.0);
        QCOMPARE(engine.scene().micBus()->linked, true);
        engine.setBusVolume(QStringLiteral("mic"), 1.2);
        QCOMPARE(engine.scene().micBus()->volume, 1.2);
        QCOMPARE(engine.scene().micBus()->phonesVolume, 1.0);
    }

    void setBusMixVolumeTogetherIgnoresLink()
    {
        pw::PwContext pw;
        engine::Engine engine(&pw);
        Scene s = engine.scene();
        Bus *game = s.bus(QStringLiteral("game"));
        game->phonesVolume = 0.6;
        game->volume = 0.2;
        game->linked = false;
        engine.setScene(s);
        engine.setBusMixVolumeTogether(QStringLiteral("game"), Mix::Stream, 0.4);
        QVERIFY(qAbs(engine.scene().bus(QStringLiteral("game"))->phonesVolume - 0.8) < 1e-9);
        QVERIFY(qAbs(engine.scene().bus(QStringLiteral("game"))->volume - 0.4) < 1e-9);
        QCOMPARE(engine.scene().bus(QStringLiteral("game"))->linked, false);
    }

    void fadeIncludesSends()
    {
        Scene s = defaults::scene();
        Bus *game = s.bus(QStringLiteral("game"));
        game->volume = 0.5;
        game->phonesVolume = 0.8;
        game->balance = -0.2;
        s.bus(QStringLiteral("music"))->muted = true;
        const auto levels = engine::busFadeLevels(s, {}, true);
        QCOMPARE(levels.value(QStringLiteral("rostrum.game")), (engine::FadeLevel{1.0, -0.2}));
        QCOMPARE(levels.value(QStringLiteral("rostrum.game.phones")), (engine::FadeLevel{0.8, 0.0}));
        QCOMPARE(levels.value(QStringLiteral("rostrum.game.stream")), (engine::FadeLevel{0.5, 0.0}));
        QCOMPARE(levels.value(QStringLiteral("rostrum.music")).position, 0.0);
        // Solo dims the headphones send to 0, nothing else.
        const auto solo = engine::busFadeLevels(s, {QStringLiteral("game")}, true);
        QCOMPARE(solo.value(QStringLiteral("rostrum.desktop.phones")).position, 0.0);
        QVERIFY(solo.value(QStringLiteral("rostrum.desktop.stream")).position > 0.0);
        QVERIFY(solo.value(QStringLiteral("rostrum.desktop")).position > 0.0);
        // Single level is today's: the bus node at its volume, 0 when muted or dimmed.
        const auto single = engine::busFadeLevels(s, {QStringLiteral("music")}, false);
        QCOMPARE(single.value(QStringLiteral("rostrum.game")), (engine::FadeLevel{0.0, -0.2}));
        QVERIFY(!single.contains(QStringLiteral("rostrum.game.phones")));
        // Voice chat sends fade like the mix sends: the fader position, or 0 when muted or blocked.
        // Solo never changes them. Engine::fadeLevels folds these into the scene ramp.
        game->chat = true;
        game->chatVolume = 0.4;
        game->chatMuted = false;
        const engine::NodeLevel chat = engine::chatSendLevels(s, {}).value(QStringLiteral("rostrum.game.chat"));
        QCOMPARE(chat.position, 0.4);
        QCOMPARE(chat.mute, false);
        QCOMPARE(chat.gain, 0.5);
        game->chatMuted = true;
        QCOMPARE(engine::chatSendLevels(s, {}).value(QStringLiteral("rostrum.game.chat")).mute, true);
        game->chatMuted = false;
        QCOMPARE(engine::chatSendLevels(s, {QStringLiteral("game")}).value(QStringLiteral("rostrum.game.chat")).mute,
                 true);
        QCOMPARE(engine::chatSendLevels(s, {}).value(QStringLiteral("rostrum.game.chat")).mute, false);
    }

    void perMixNeedsFactory()
    {
        pw::PwContext pw; // never started
        engine::Engine engine(&pw);
        QSignalSpy changed(&engine, &engine::Engine::perMixLevelsChanged);
        engine.createMix();
        pw.testingInstallGraph(devices()); // lists no spa-node-factory
        QTRY_VERIFY(changed.count() >= 1);
        QCOMPARE(engine.perMixLevels(), false);
        QCOMPARE(engine.perMixUnavailable(), QStringLiteral("PipeWire's spa-node-factory module is not loaded."));
        QCOMPARE(changed.count(), 1);
    }

    void perMixFallsBackWhenPortsNeverCome()
    {
        pw::Graph g = devices();
        g.factories.insert(90, QStringLiteral("spa-node-factory"));
        uint32_t id = 60;
        for (const char *name : {"rostrum.game.phones", "rostrum.game.stream"}) {
            pw::Node send = node(id, name, "");
            send.props.insert(QStringLiteral("rostrum.role"), QStringLiteral("send"));
            g.nodes.insert(send.id, send);
            ++id;
        }
        pw::PwContext pw; // never started: setPortConfig does nothing, so the ports never come
        engine::Engine engine(&pw);
        Scene scene = engine.scene();
        scene.bus(QStringLiteral("game"))->phonesVolume = 0.3;
        engine.setScene(scene);
        QVERIFY(!engine.perMixLevels()); // not connected yet
        engine.createMix();
        pw.testingInstallGraph(g);
        QCoreApplication::processEvents();
        QVERIFY(engine.perMixLevels());
        QVERIFY(engine.perMixUnavailable().isEmpty());
        QSignalSpy changed(&engine, &engine::Engine::perMixLevelsChanged);
        QTRY_VERIFY_WITH_TIMEOUT(!engine.perMixLevels(), 6000);
        QVERIFY(engine.perMixUnavailable().contains(QStringLiteral("Game")));
        QCOMPARE(engine.perMixUnavailable(), QStringLiteral("PipeWire did not set up the level controls for Game."));
        QCOMPARE(changed.count(), 1);
        // The sends it made are removed; the fallback holds for the session.
        QVERIFY(pw.testingDestroyRequests().contains(60));
        QVERIFY(pw.testingDestroyRequests().contains(61));
        pw.testingInstallGraph(g);
        QCoreApplication::processEvents();
        QVERIFY(!engine.perMixLevels());

        // fallbackKeepsHeadphonesLevels: the Headphones level is kept, the links are single-level.
        QCOMPARE(engine.scene().bus(QStringLiteral("game"))->phonesVolume, 0.3);
        QCOMPARE(linkNames(engine::busLinks(engine.scene(), engine.perMixLevels())),
                 linkNames(engine::busLinks(engine.scene(), false)));
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

    void bluetoothHeadphonesPreferA2dpByAddress()
    {
        pw::Graph g = devices();
        const QString addr = QStringLiteral("AA:BB:CC:DD:EE:FF");
        auto bluez = [&](uint32_t id, const char *name, const char *profile, int priority) {
            pw::Node n = node(id, name, "Audio/Sink", priority);
            n.props.insert(QStringLiteral("device.api"), QStringLiteral("bluez5"));
            n.props.insert(QStringLiteral("api.bluez5.address"), addr);
            n.props.insert(QStringLiteral("api.bluez5.profile"), QString::fromLatin1(profile));
            return n;
        };
        g.nodes.insert(20, bluez(20, "bluez_output.AA_BB_CC_DD_EE_FF.headset-head-unit", "headset-head-unit", 2000));
        g.nodes.insert(21, bluez(21, "bluez_output.AA_BB_CC_DD_EE_FF.a2dp-sink", "a2dp-sink", 1500));
        // Saved A2DP name is gone; address still finds A2DP over HFP even when HFP has higher priority.
        QCOMPARE(nameOf(engine::resolveDevice(g, QStringLiteral("bluez_output.AA_BB_CC_DD_EE_FF.a2dp-sink"),
                                              QStringLiteral("alsa_output.speakers"), true, true, addr)),
                 QStringLiteral("bluez_output.AA_BB_CC_DD_EE_FF.a2dp-sink"));
        g.nodes.remove(21);
        QCOMPARE(nameOf(engine::resolveDevice(g, QStringLiteral("bluez_output.AA_BB_CC_DD_EE_FF.a2dp-sink"),
                                              QStringLiteral("alsa_output.speakers"), true, true, addr)),
                 QStringLiteral("bluez_output.AA_BB_CC_DD_EE_FF.headset-head-unit"));
        g.nodes.remove(20);
        QCOMPARE(nameOf(engine::resolveDevice(g, QStringLiteral("bluez_output.AA_BB_CC_DD_EE_FF.a2dp-sink"),
                                              QStringLiteral("alsa_output.speakers"), true, true, addr)),
                 QStringLiteral("alsa_output.speakers"));
    }

    void bluetoothHeadphonesMigrateNodeName()
    {
        pw::PwContext pw;
        engine::Engine engine(&pw);
        auto flush = [] {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::Timer);
            QCoreApplication::processEvents();
        };
        const QString addr = QStringLiteral("11:22:33:44:55:66");
        pw::Graph g = devices();
        pw::Node a2dp = node(30, "bluez_output.old.a2dp-sink", "Audio/Sink", 1500);
        a2dp.description = QStringLiteral("TW 5");
        a2dp.props.insert(QStringLiteral("device.api"), QStringLiteral("bluez5"));
        a2dp.props.insert(QStringLiteral("api.bluez5.address"), addr);
        a2dp.props.insert(QStringLiteral("api.bluez5.profile"), QStringLiteral("a2dp-sink"));
        g.nodes.insert(30, a2dp);
        pw.testingInstallGraph(g);
        engine.setHeadphoneDevice(QStringLiteral("bluez_output.old.a2dp-sink"));
        engine.setHeadphonesBtAddress(addr);
        flush();
        QVERIFY(!engine.headphonesMissing());

        pw::Graph flipped = devices();
        pw::Node hfp = node(31, "bluez_output.new.headset-head-unit", "Audio/Sink", 2000);
        hfp.description = QStringLiteral("TW 5");
        hfp.props.insert(QStringLiteral("device.api"), QStringLiteral("bluez5"));
        hfp.props.insert(QStringLiteral("api.bluez5.address"), addr);
        hfp.props.insert(QStringLiteral("api.bluez5.profile"), QStringLiteral("headset-head-unit"));
        flipped.nodes.insert(31, hfp);
        pw.testingInstallGraph(flipped);
        flush();

        QVERIFY(!engine.headphonesMissing());
        QCOMPARE(engine.headphoneDevice(), QStringLiteral("bluez_output.new.headset-head-unit"));
        QCOMPARE(engine.resolvedSinkName(), QStringLiteral("bluez_output.new.headset-head-unit"));
        QCOMPARE(engine.headphonesBtAddress(), addr);
    }

    void headphonesBtAddressRoundTripsInSettings()
    {
        Settings s = defaultSettings();
        s.headphones = QStringLiteral("bluez_output.AA_BB.a2dp-sink");
        s.headphonesBtAddress = QStringLiteral("AA:BB:CC:DD:EE:FF");
        QCOMPARE(parseSettings(serializeSettings(s)), s);
        QVERIFY(serializeSettings(s).contains(QStringLiteral("headphones_bt_address")));
        QVERIFY(parseSettings(QStringLiteral("[devices]\nheadphones = 'x'\n")).headphonesBtAddress.isEmpty());
    }

    void followDefaultLearnsAndClearsHeadphonesBtAddress()
    {
        pw::PwContext pw;
        engine::Engine engine(&pw);
        auto flush = [] {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::Timer);
            QCoreApplication::processEvents();
        };
        const QString addr = QStringLiteral("80:C3:BA:BB:34:03");
        pw::Graph g = devices();
        pw::Node buds = node(40, "bluez_output.buds.a2dp-sink", "Audio/Sink", 2000);
        buds.props.insert(QStringLiteral("device.api"), QStringLiteral("bluez5"));
        buds.props.insert(QStringLiteral("api.bluez5.address"), addr);
        buds.props.insert(QStringLiteral("api.bluez5.profile"), QStringLiteral("a2dp-sink"));
        g.nodes.insert(40, buds);
        pw.testingInstallGraph(g);
        pw.testingSetDefaults(QStringLiteral("bluez_output.buds.a2dp-sink"), QStringLiteral("alsa_input.webcam"));
        engine.setHeadphoneDevice(QString());
        engine.setHeadphonesBtAddress(QString());
        flush();
        QCOMPARE(engine.headphonesBtAddress(), addr);
        QVERIFY(engine.headphoneDevice().isEmpty());
        QCOMPARE(engine.resolvedSinkName(), QStringLiteral("bluez_output.buds.a2dp-sink"));

        pw.testingSetDefaults(QStringLiteral("alsa_output.speakers"), QStringLiteral("alsa_input.webcam"));
        flush();
        QVERIFY(engine.headphonesBtAddress().isEmpty());
        QVERIFY(engine.headphoneDevice().isEmpty());
        QCOMPARE(engine.resolvedSinkName(), QStringLiteral("alsa_output.speakers"));
    }

    void highQualityProfileScorePrefersAacOverAptx()
    {
        QCOMPARE(pw::highQualityProfileScore(QStringLiteral("a2dp-sink-aac")), 50);
        QCOMPARE(pw::highQualityProfileScore(QStringLiteral("a2dp-sink-sbc_xq")), 40);
        QCOMPARE(pw::highQualityProfileScore(QStringLiteral("a2dp-sink-sbc")), 30);
        QCOMPARE(pw::highQualityProfileScore(QStringLiteral("a2dp-sink")), 20);
        QCOMPARE(pw::highQualityProfileScore(QStringLiteral("a2dp-sink-aptx")), 20);
        QCOMPARE(pw::highQualityProfileScore(QStringLiteral("bap-sink")), 10);
        QCOMPARE(pw::highQualityProfileScore(QStringLiteral("headset-head-unit")), 0);

        pw::Device device;
        device.profiles = {
            {0, QStringLiteral("off"), QString(), 0, true},
            {1, QStringLiteral("a2dp-sink"), QStringLiteral("aptX"), 0, true},
            {2, QStringLiteral("a2dp-sink-aac"), QStringLiteral("AAC"), 0, true},
            {3, QStringLiteral("headset-head-unit"), QString(), 0, true},
            {4, QStringLiteral("a2dp-sink-sbc"), QStringLiteral("SBC"), 0, false},
        };
        QCOMPARE(pw::rankedHighQualityProfiles(device), (QList<uint32_t>{2, 1}));

        pw::Device aptxOnly;
        aptxOnly.profiles = {{1, QStringLiteral("a2dp-sink"), QStringLiteral("High Fidelity Playback (aptX)"), 0, true}};
        QCOMPARE(pw::rankedHighQualityProfiles(aptxOnly), (QList<uint32_t>{1}));

        pw::Device sbcOnly;
        sbcOnly.profiles = {{1, QStringLiteral("a2dp-sink-sbc"), QStringLiteral("SBC"), 0, true}};
        QCOMPARE(pw::rankedHighQualityProfiles(sbcOnly), (QList<uint32_t>{1}));
    }

    void streamOnlyPlaybackBusNamesListsMusic()
    {
        Scene scene = defaults::scene();
        Bus *music = scene.bus(QStringLiteral("music"));
        QVERIFY(music);
        music->destination = Destination::Stream;
        QCOMPARE(engine::streamOnlyPlaybackBusNames(scene), QStringList{music->name});
        music->destination = Destination::Both;
        QVERIFY(engine::streamOnlyPlaybackBusNames(scene).isEmpty());
    }

    void micBtAddressResolvesAndMigrates()
    {
        pw::PwContext pw;
        engine::Engine engine(&pw);
        auto flush = [] {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::Timer);
            QCoreApplication::processEvents();
        };
        const QString addr = QStringLiteral("AA:BB:CC:DD:EE:01");
        pw::Graph g = devices();
        pw::Node mic = node(50, "bluez_input.old.headset-head-unit", "Audio/Source", 1800);
        mic.props.insert(QStringLiteral("device.api"), QStringLiteral("bluez5"));
        mic.props.insert(QStringLiteral("api.bluez5.address"), addr);
        mic.props.insert(QStringLiteral("api.bluez5.profile"), QStringLiteral("headset-head-unit"));
        g.nodes.insert(50, mic);
        pw.testingInstallGraph(g);
        engine.setMicDevice(QStringLiteral("bluez_input.old.headset-head-unit"));
        engine.setMicBtAddress(addr);
        engine.setMicFallback(false);
        flush();
        QVERIFY(!engine.micMissing());

        pw::Graph flipped = devices();
        pw::Node renamed = node(51, "bluez_input.new.headset-head-unit", "Audio/Source", 1800);
        renamed.props.insert(QStringLiteral("device.api"), QStringLiteral("bluez5"));
        renamed.props.insert(QStringLiteral("api.bluez5.address"), addr);
        renamed.props.insert(QStringLiteral("api.bluez5.profile"), QStringLiteral("headset-head-unit"));
        flipped.nodes.insert(51, renamed);
        pw.testingInstallGraph(flipped);
        flush();
        QVERIFY(!engine.micMissing());
        QCOMPARE(engine.micDevice(), QStringLiteral("bluez_input.new.headset-head-unit"));
        QCOMPARE(engine.resolvedSourceName(), QStringLiteral("bluez_input.new.headset-head-unit"));

        pw::Graph gone = devices();
        pw.testingInstallGraph(gone);
        flush();
        QVERIFY(engine.micMissing());
        QVERIFY(engine.micSilenced());
        QVERIFY(engine.resolvedSourceName().isEmpty());
    }

    void micBtAddressNotLearnedFromSystemDefault()
    {
        pw::PwContext pw;
        engine::Engine engine(&pw);
        auto flush = [] {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::Timer);
            QCoreApplication::processEvents();
        };
        const QString addr = QStringLiteral("11:22:33:44:55:66");
        pw::Graph g = devices();
        pw::Node mic = node(52, "bluez_input.buds.headset-head-unit", "Audio/Source", 2500);
        mic.props.insert(QStringLiteral("device.api"), QStringLiteral("bluez5"));
        mic.props.insert(QStringLiteral("api.bluez5.address"), addr);
        g.nodes.insert(52, mic);
        pw.testingInstallGraph(g);
        pw.testingSetDefaults(QStringLiteral("alsa_output.speakers"),
                              QStringLiteral("bluez_input.buds.headset-head-unit"));
        engine.setMicDevice(QString());
        engine.setMicBtAddress(QString());
        flush();
        QVERIFY(engine.micBtAddress().isEmpty());
        QCOMPARE(engine.resolvedSourceName(), QStringLiteral("bluez_input.buds.headset-head-unit"));
    }

    void micBtAddressRoundTripsInSettings()
    {
        Settings s = defaultSettings();
        s.mic = QStringLiteral("bluez_input.AA_BB.headset-head-unit");
        s.micBtAddress = QStringLiteral("AA:BB:CC:DD:EE:FF");
        QCOMPARE(parseSettings(serializeSettings(s)), s);
        QVERIFY(serializeSettings(s).contains(QStringLiteral("mic_bt_address")));
    }

    void headphonesTransportUnhealthyAfterErrorDebounce()
    {
        pw::PwContext pw;
        engine::Engine engine(&pw);
        auto flush = [] {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::Timer);
            QCoreApplication::processEvents();
        };
        const QString addr = QStringLiteral("DE:AD:BE:EF:00:01");
        pw::Graph g = devices();
        pw::Node buds = node(60, "bluez_output.dead.a2dp-sink", "Audio/Sink", 2000);
        buds.props.insert(QStringLiteral("device.api"), QStringLiteral("bluez5"));
        buds.props.insert(QStringLiteral("api.bluez5.address"), addr);
        buds.props.insert(QStringLiteral("api.bluez5.profile"), QStringLiteral("a2dp-sink"));
        buds.error = true;
        g.nodes.insert(60, buds);
        pw.testingInstallGraph(g);
        pw.testingSetDefaults(QStringLiteral("bluez_output.dead.a2dp-sink"), QStringLiteral("alsa_input.webcam"));
        engine.setHeadphoneDevice(QStringLiteral("bluez_output.dead.a2dp-sink"));
        engine.setHeadphonesBtAddress(addr);
        flush();
        QVERIFY(!engine.headphonesTransportUnhealthy());
        QTest::qWait(2200);
        flush();
        QVERIFY(engine.headphonesTransportUnhealthy());

        buds.error = false;
        buds.running = true;
        g.nodes.insert(60, buds);
        pw.testingInstallGraph(g);
        pw.testingSetDefaults(QStringLiteral("bluez_output.dead.a2dp-sink"), QStringLiteral("alsa_input.webcam"));
        flush();
        QVERIFY(!engine.headphonesTransportUnhealthy());
    }

    void javaAppsGetTheirOwnRulesAndLevels()
    {
        QTemporaryDir dir;
        QTemporaryDir proc;
        // A fake /proc: native RuneLite (500), Minecraft (600), and RuneLite in Bolt's sandbox,
        // which the stream knows as 406 (host 700).
        writeJavaProcess(proc.path(), 500, QStringLiteral("500"), "net.runelite.client.RuneLite");
        writeJavaProcess(proc.path(), 600, QStringLiteral("600"), "net.minecraft.client.main.Main");
        writeJavaProcess(proc.path(), 700, QStringLiteral("700\t406"), "net.runelite.client.RuneLite",
                         QStringLiteral("com.adamcake.Bolt"));

        pw::Graph g;
        for (const auto &[id, pid] : {std::pair<uint32_t, qint64>{50, 500}, {60, 600}, {70, 406}, {80, 999}}) {
            pw::Node n = javaStream(id, pid);
            if (id == 70) {
                n.props.insert(QStringLiteral("pipewire.access.portal.app_id"), QStringLiteral("com.adamcake.Bolt"));
            }
            g.nodes.insert(n.id, n);
        }

        pw::PwContext pw; // never started: stand-in graph only
        engine::Engine engine(&pw);
        engine.testingSetProcRoot(proc.path());
        engine::SceneManager scenes(&engine, dir.path());
        scenes.load(QStringLiteral("Live"));
        pw.testingInstallGraph(g);

        QHash<uint32_t, engine::AppStream> by;
        const auto refresh = [&] {
            by.clear();
            for (const auto &s : engine.appStreams()) {
                by.insert(s.nodeId, s);
            }
        };
        refresh();
        QCOMPARE(by.size(), 4);
        QCOMPARE(by[50].identity.key.toString(), QStringLiteral("java:RuneLite"));
        QCOMPARE(by[50].identity.displayName, QStringLiteral("RuneLite"));
        QCOMPARE(by[60].identity.displayName, QStringLiteral("Minecraft"));
        QCOMPARE(by[70].identity.key.toString(), QStringLiteral("java:RuneLite")); // Flatpak, same app
        QCOMPARE(by[80].identity.key.toString(), QStringLiteral("binary:java"));  // unreadable: as before

        const AppKey runeLite{MatchKey::Java, QStringLiteral("RuneLite")};
        const AppKey minecraft{MatchKey::Java, QStringLiteral("Minecraft")};
        engine.setAppMuted(minecraft, true);
        QVERIFY(engine.appMuted(minecraft));
        QVERIFY(!engine.appMuted(runeLite));

        // A rule from before Java programs were known still catches every other Java app.
        engine.assignApp({MatchKey::Binary, QStringLiteral("java")}, QStringLiteral("desktop"), true);
        engine.assignApp(runeLite, QStringLiteral("game"), true);
        refresh();
        QCOMPARE(by[50].busId, QStringLiteral("game"));
        QCOMPARE(by[70].busId, QStringLiteral("game"));
        QCOMPARE(by[60].busId, QStringLiteral("desktop"));
        QCOMPARE(by[80].busId, QStringLiteral("desktop"));
        QVERIFY(engine.scene().rule(MatchKey::Java, QStringLiteral("runelite")));

        // Name It… labels that program's rule only.
        engine.setRuleLabel(runeLite, QStringLiteral("OSRS"));
        refresh();
        QCOMPARE(by[50].identity.displayName, QStringLiteral("OSRS"));
        QCOMPARE(by[60].identity.displayName, QStringLiteral("Minecraft"));
    }

    void olderJavaRuleLeavesEachJavaAppItsOwnRow()
    {
        QTemporaryDir dir;
        QTemporaryDir proc;
        writeJavaProcess(proc.path(), 500, QStringLiteral("500"), "net.runelite.client.RuneLite");
        writeJavaProcess(proc.path(), 600, QStringLiteral("600"), "net.minecraft.client.main.Main");
        writeJavaProcess(proc.path(), 700, QStringLiteral("700"), "com.example.Game");
        pw::Graph g;
        for (const auto &[id, pid] : {std::pair<uint32_t, qint64>{50, 500}, {60, 600}, {70, 700}}) {
            const pw::Node n = javaStream(id, pid);
            g.nodes.insert(n.id, n);
        }
        pw::PwContext pw; // never started: stand-in graph only
        engine::Engine engine(&pw);
        engine.testingSetProcRoot(proc.path());
        engine::SceneManager scenes(&engine, dir.path());
        scenes.load(QStringLiteral("Live"));

        // From before Java programs were known: Name It… on the one "Java" row, at half volume.
        const AppKey anyJava{MatchKey::Binary, QStringLiteral("java")};
        engine.assignApp(anyJava, QStringLiteral("game"), true);
        engine.setRuleLabel(anyJava, QStringLiteral("RuneLite"));
        engine.setAppVolume(anyJava, 0.5);
        pw.testingInstallGraph(g);

        QHash<uint32_t, engine::AppStream> by;
        const auto refresh = [&] {
            by.clear();
            for (const auto &s : engine.appStreams()) {
                by.insert(s.nodeId, s);
            }
        };
        refresh();
        QCOMPARE(by.size(), 3);
        // The older rule still places every Java app, but each keeps its own name and row.
        QCOMPARE(by[50].identity.displayName, QStringLiteral("RuneLite"));
        QCOMPARE(by[60].identity.displayName, QStringLiteral("Minecraft"));
        QCOMPARE(by[70].identity.displayName, QStringLiteral("Java"));
        QVERIFY(by[70].identity.unnamed);
        for (const uint32_t id : {50u, 60u, 70u}) {
            QCOMPARE(by[id].busId, QStringLiteral("game"));
            QVERIFY(!by[id].ruleKey.isValid());
            QCOMPARE(by[id].volume, 0.5); // the level the older rule set, until the app gets its own
        }

        // Mute one: the other keeps playing, and the older rule is untouched.
        const AppKey runeLite{MatchKey::Java, QStringLiteral("RuneLite")};
        const AppKey minecraft{MatchKey::Java, QStringLiteral("Minecraft")};
        engine.setAppMuted(minecraft, true);
        engine.setAppVolume(runeLite, 0.8);
        refresh();
        QVERIFY(by[60].muted);
        QVERIFY(!by[50].muted);
        QCOMPARE(by[50].volume, 0.8);
        QCOMPARE(by[60].volume, 0.5);
        QVERIFY(!engine.scene().rule(MatchKey::Binary, QStringLiteral("java"))->muted);
        QCOMPARE(engine.scene().rule(MatchKey::Binary, QStringLiteral("java"))->volume, 0.5);
        // Unmuting an app sticks even while the older rule is muted.
        engine.setAppMuted(anyJava, true);
        engine.setAppMuted(runeLite, false);
        refresh();
        QVERIFY(!by[50].muted);
        QVERIFY(by[70].muted);
        engine.setAppMuted(anyJava, false);

        // An unknown Java app, which nothing recognises, can be taken off its bus too.
        engine.unassignStream(70);
        refresh();
        QVERIFY(by[70].busId.isEmpty());
        QCOMPARE(by[60].busId, QStringLiteral("game"));

        // Name It… on the unknown one gives it a rule of its own, at the level it plays at,
        // and leaves the other Java apps and the older rule alone.
        const AppKey other{MatchKey::Java, QStringLiteral("com.example.Game")};
        engine.assignApp(other, QStringLiteral("music"), true);
        engine.setRuleLabel(other, QStringLiteral("Pixel Game"));
        refresh();
        QCOMPARE(by[70].identity.displayName, QStringLiteral("Pixel Game"));
        QCOMPARE(by[70].busId, QStringLiteral("music"));
        QCOMPARE(by[50].identity.displayName, QStringLiteral("RuneLite"));
        QCOMPARE(by[50].busId, QStringLiteral("game"));
        QCOMPARE(engine.scene().rule(MatchKey::Java, QStringLiteral("com.example.Game"))->volume, 0.5);
        const AppRule *older = engine.scene().rule(MatchKey::Binary, QStringLiteral("java"));
        QVERIFY(older);
        QCOMPARE(older->label, QStringLiteral("RuneLite"));
        QCOMPARE(older->busId, QStringLiteral("game"));

        // Taking one off its bus leaves the older rule for the rest.
        engine.unassignStream(50);
        refresh();
        QVERIFY(by[50].busId.isEmpty());
        QCOMPARE(by[60].busId, QStringLiteral("game"));
        QVERIFY(engine.scene().rule(MatchKey::Binary, QStringLiteral("java")));
        // Assigning it again brings it back.
        engine.assignApp(runeLite, QStringLiteral("game"), false);
        refresh();
        QCOMPARE(by[50].busId, QStringLiteral("game"));
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

    // PipeWire hands a removed link's ID to the next object at once (packet #3: a new DSP port).
    // Rostrum's graph copy can still show the old link when it decides to unlink; the destroy must
    // only be sent while the ID still names that very link (same object.serial).
    void unlinkSkipsALinkWhoseIdWasReused()
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
            p.name = QStringLiteral("%1_%2").arg(
                output ? QStringLiteral("monitor") : QStringLiteral("playback"), p.channel);
            p.serial = QString::number(1000 + id);
            return p;
        };
        auto link = [](uint32_t id, uint32_t outNode, uint32_t outPort, uint32_t inNode, uint32_t inPort,
                       const char *serial) {
            pw::Link l;
            l.id = id;
            l.outNode = outNode;
            l.outPort = outPort;
            l.inNode = inNode;
            l.inPort = inPort;
            l.serial = QString::fromLatin1(serial);
            return l;
        };

        pw::Graph g;
        for (auto n : {node(100, "rostrum.game", "Audio/Sink", 9000),
                       node(200, "rostrum.stream", "Audio/Sink", 9000)}) {
            n.channels = 2;
            g.nodes.insert(n.id, n);
        }
        for (const auto &p :
             {port(110, 100, false, "FL"), port(111, 100, false, "FR"), port(112, 100, true, "FL"),
              port(113, 100, true, "FR"), port(210, 200, false, "FL"), port(211, 200, false, "FR"),
              port(212, 200, true, "FL"), port(213, 200, true, "FR")}) {
            g.ports.insert(p.id, p);
        }
        g.links.insert(930, link(930, 100, 112, 200, 210, "5930"));
        g.links.insert(931, link(931, 100, 113, 200, 211, "5931"));

        engine.setBusDestination(QStringLiteral("game"), Destination::Stream);
        engine.createMix();
        pw.testingInstallGraph(g);
        flush();
        QVERIFY(!pw.testingDestroyRequests().contains(930));
        QVERIFY(!pw.testingDestroyRequests().contains(931));

        // Where PipeWire's events arrive first, 930 is already a port and 931 a newer link; the
        // copy has not caught up. The bus stops feeding the stream, so both look unwanted.
        QHash<uint32_t, pw::Link> loop;
        loop.insert(931, link(931, 100, 113, 200, 211, "6931"));
        pw.testingSetLoopLinks(loop);
        engine.setBusDestination(QStringLiteral("game"), Destination::Phones);
        flush();
        QVERIFY2(!pw.testingDestroyRequests().contains(930), "930 now names a port");
        QVERIFY2(!pw.testingDestroyRequests().contains(931), "931 now names a different link");

        // Once the IDs name the same links again, the unwanted links do go.
        loop.insert(930, g.links.value(930));
        loop.insert(931, g.links.value(931));
        pw.testingSetLoopLinks(loop);
        pw.testingInstallGraph(g);
        flush();
        QVERIFY(pw.testingDestroyRequests().contains(930));
        QVERIFY(pw.testingDestroyRequests().contains(931));
    }

    // A send gets DSP ports by a port config that replaces the ports it starts with. PipeWire drops
    // the links on the old ports itself and reuses their IDs for the new ports at once, so a destroy
    // sent for such a link removed a new DSP port (PipeWire 1.4: the send then fell back for good).
    void linksOfASendWaitingForItsPortsAreLeftAlone()
    {
        pw::PwContext pw; // never started: stand-in graph only
        engine::Engine engine(&pw);
        auto flush = [] {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::Timer);
            QCoreApplication::processEvents();
        };
        auto port = [](uint32_t id, uint32_t nodeId, bool output, const char *channel, const char *name) {
            pw::Port p;
            p.id = id;
            p.nodeId = nodeId;
            p.output = output;
            p.monitor = output && nodeId != 300; // the buses' outputs are their monitors
            p.channel = QString::fromLatin1(channel);
            p.name = QString::fromLatin1(name);
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

        pw::Graph g;
        g.factories.insert(90, QStringLiteral("spa-node-factory"));
        for (auto n : {node(100, "rostrum.game", "Audio/Sink", 9000),
                       node(200, "rostrum.stream", "Audio/Sink", 9000)}) {
            n.channels = 2;
            g.nodes.insert(n.id, n);
        }
        pw::Node send = node(300, "rostrum.game.stream", "");
        send.props.insert(QStringLiteral("rostrum.role"), QStringLiteral("send"));
        g.nodes.insert(send.id, send);
        for (const auto &p :
             {port(110, 100, false, "FL", "playback_FL"), port(111, 100, false, "FR", "playback_FR"),
              port(112, 100, true, "FL", "monitor_FL"), port(113, 100, true, "FR", "monitor_FR"),
              port(210, 200, false, "FL", "playback_FL"), port(211, 200, false, "FR", "playback_FR"),
              port(310, 300, false, "", "input_1"), port(311, 300, true, "", "output_1")}) {
            g.ports.insert(p.id, p);
        }
        // Linked before its port config, e.g. while its node info was still on the way.
        g.links.insert(960, link(960, 100, 112, 300, 310));

        engine.setBusDestination(QStringLiteral("game"), Destination::Stream);
        engine.createMix();
        pw.testingInstallGraph(g);
        flush();
        QVERIFY(engine.perMixLevels());
        QVERIFY2(!pw.testingDestroyRequests().contains(960),
                 "A link on a send waiting for its DSP ports must be left to PipeWire");

        // Once the DSP ports are there, a link that is really unwanted still goes.
        g.ports.remove(310);
        g.ports.remove(311);
        for (const auto &p :
             {port(320, 300, false, "FL", "input_FL"), port(321, 300, false, "FR", "input_FR"),
              port(322, 300, true, "FL", "output_FL"), port(323, 300, true, "FR", "output_FR")}) {
            g.ports.insert(p.id, p);
        }
        g.links.remove(960);
        g.links.insert(961, link(961, 100, 112, 300, 321)); // left into right
        pw.testingInstallGraph(g);
        flush();
        QVERIFY(pw.testingDestroyRequests().contains(961));
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
