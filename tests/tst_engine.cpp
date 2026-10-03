#include "core/SceneStore.h"
#include "core/SceneToml.h"
#include "core/Settings.h"
#include "engine/Engine.h"
#include "engine/SceneManager.h"
#include "pw/Graph.h"
#include "pw/PwContext.h"

#include <QTemporaryDir>
#include <QTest>

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
