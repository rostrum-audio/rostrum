#include "core/Remote.h"
#include "core/SceneStore.h"
#include "core/SceneToml.h"
#include "core/Settings.h"
#include "engine/Controls.h"
#include "engine/Engine.h"
#include "engine/SceneManager.h"
#include "pw/PwContext.h"

#include <QTemporaryDir>
#include <QTest>

using namespace rostrum;
using Result = engine::Controls::Result;

namespace {

// A scene manager over a temporary folder with "Live" and "Ranked"; "Ranked" has an extra bus.
struct Rig
{
    QTemporaryDir dir;
    pw::PwContext pw; // never started: no audio server needed
    engine::Engine engine{&pw};
    engine::SceneManager scenes{&engine, dir.path()};
    engine::Controls controls{&engine, &scenes};

    Rig()
    {
        scenes.load(QStringLiteral("Live"));
        Scene ranked = defaults::scene(QStringLiteral("Ranked"));
        Bus extra;
        extra.id = QStringLiteral("soundboard");
        extra.name = QStringLiteral("Soundboard");
        ranked.buses.append(extra);
        scenes.create(ranked);
    }

    QString fileText(const QString &name) const
    {
        return SceneStore::readFile(SceneStore(dir.path()).fileFor(name), nullptr);
    }
};

} // namespace

class TestControls : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void actionIds()
    {
        const QStringList all = actions::all();
        // The wizard shows the first three, and old ids keep their place.
        QCOMPARE(all.mid(0, 4),
                 (QStringList{QStringLiteral("mute_mic"), QStringLiteral("mute_stream"),
                              QStringLiteral("previous_scene"), QStringLiteral("next_scene")}));
        for (const char *id :
             {"push_to_talk", "push_to_mute", "panic_mute", "toggle_sidetone", "mute_headphones",
              "stream_volume_up", "stream_volume_down", "scene_5", "scene_8"}) {
            QVERIFY2(all.contains(QLatin1String(id)), id);
        }
        QCOMPARE(all.size(), int(QSet<QString>(all.begin(), all.end()).size()));

        QVERIFY(actions::isHold(QStringLiteral("push_to_talk")));
        QVERIFY(actions::isHold(QStringLiteral("push_to_mute")));
        QVERIFY(!actions::isHold(QStringLiteral("panic_mute")));
        QCOMPARE(actions::group(QStringLiteral("panic_mute")), actions::Group::Mic);
        QCOMPARE(actions::group(QStringLiteral("mute_headphones")), actions::Group::Stream);
        QCOMPARE(actions::group(QStringLiteral("scene_7")), actions::Group::Scenes);
        QCOMPARE(actions::group(QStringLiteral("mute_bus_game")), actions::Group::Buses);

        QCOMPARE(actions::muteBusAction(QStringLiteral("game")), QStringLiteral("mute_bus_game"));
        QCOMPARE(actions::busOfAction(QStringLiteral("mute_bus_extra-2")), QStringLiteral("extra-2"));
        QVERIFY(
            actions::busOfAction(QStringLiteral("mute_bus_mic")).isEmpty()); // the mic has its own actions
        QVERIFY(actions::busOfAction(QStringLiteral("mute_bus_")).isEmpty());
        QVERIFY(actions::busOfAction(QStringLiteral("mute_bus_Bad Id")).isEmpty());
        QVERIFY(actions::busOfAction(QStringLiteral("mute_mic")).isEmpty());

        QCOMPARE(actions::sceneSlot(QStringLiteral("scene_3")), 3);
        QCOMPARE(actions::sceneSlot(QStringLiteral("scene_0")), 0);
        QCOMPARE(actions::sceneSlot(QStringLiteral("scene_x")), 0);
        QVERIFY(actions::isKnown(QStringLiteral("mute_bus_voice")));
        QVERIFY(!actions::isKnown(QStringLiteral("launch_rockets")));
    }

    void settingsKeepNewActions()
    {
        const Settings d = defaultSettings();
        QVERIFY(d.osdFeedback);
        QVERIFY(d.closeToTray);
        QVERIFY(!d.minimizeToTray);
        // New actions start unbound; the old defaults stay.
        QCOMPARE(d.hotkeys.value(QStringLiteral("mute_mic")), QStringLiteral("Meta+Alt+M"));
        QCOMPARE(d.hotkeys.value(QStringLiteral("scene_4")), QStringLiteral("Meta+Alt+4"));
        for (const char *id : {"push_to_talk", "push_to_mute", "panic_mute", "scene_5", "stream_volume_up"}) {
            QVERIFY2(d.hotkeys.contains(QLatin1String(id)), id);
            QVERIFY2(d.hotkeys.value(QLatin1String(id)).isEmpty(), id);
        }

        Settings s = d;
        s.osdFeedback = false;
        s.closeToTray = false;
        s.minimizeToTray = true;
        s.hotkeys[QStringLiteral("push_to_talk")] = QStringLiteral("Meta+Alt+T");
        s.hotkeys[QStringLiteral("panic_mute")] = QStringLiteral("Meta+Alt+Esc");
        s.hotkeys[QStringLiteral("mute_bus_game")] = QStringLiteral("Meta+Alt+G");
        s.hotkeys[QStringLiteral("mute_bus_soundboard")] = QStringLiteral("Meta+Alt+B");
        const QString text = serializeSettings(s);
        QVERIFY(text.contains(QStringLiteral("osd_feedback = false")));
        QVERIFY(text.contains(QStringLiteral("close_to_tray = false")));
        QVERIFY(text.contains(QStringLiteral("minimize_to_tray = true")));
        QCOMPARE(parseSettings(text), s);

        // Unknown ids and malformed bus ids are dropped.
        const Settings parsed =
            parseSettings(QStringLiteral("[hotkeys]\nlaunch_rockets = 'Meta+R'\n"
                                         "mute_bus_mic = 'Meta+X'\n'mute_bus_Bad Id' = 'Meta+Y'\n"));
        QCOMPARE(parsed.hotkeys, d.hotkeys);
    }

    void micAndStreamToggles()
    {
        Rig r;
        auto &e = r.engine;
        QCOMPARE(r.controls.press(QStringLiteral("mute_mic")).result, Result::Done);
        QVERIFY(e.micMuted());
        r.controls.press(QStringLiteral("mute_mic"));
        QVERIFY(!e.micMuted());

        r.controls.press(QStringLiteral("mute_stream"));
        QVERIFY(e.scene().masterStreamMuted);
        r.controls.press(QStringLiteral("mute_headphones"));
        QVERIFY(e.scene().masterPhonesMuted);
        QVERIFY(!e.sidetoneEnabled());
        r.controls.press(QStringLiteral("toggle_sidetone"));
        QVERIFY(e.sidetoneEnabled());
        r.controls.press(QStringLiteral("toggle_sidetone"));
        QVERIFY(!e.sidetoneEnabled());
        QCOMPARE(e.scene().micBus()->destination, Destination::Stream); // never left going nowhere

        e.setMasterStream(0.5);
        r.controls.press(QStringLiteral("stream_volume_up"));
        QCOMPARE(e.scene().masterStream, 0.55);
        r.controls.press(QStringLiteral("stream_volume_down"));
        r.controls.press(QStringLiteral("stream_volume_down"));
        QCOMPARE(e.scene().masterStream, 0.45);
        e.setMasterStream(0.98);
        r.controls.press(QStringLiteral("stream_volume_up"));
        QCOMPARE(e.scene().masterStream, 1.0);

        QCOMPARE(r.controls.press(QStringLiteral("launch_rockets")).result, Result::UnknownAction);
    }

    void holdsAreSessionOnly()
    {
        Rig r;
        auto &e = r.engine;
        e.setMicMuted(true);
        QVERIFY(r.scenes.save());
        const QString before = r.fileText(QStringLiteral("Live"));

        QCOMPARE(r.controls.press(QStringLiteral("push_to_talk")).result, Result::Done);
        QVERIFY(r.controls.isHeld(QStringLiteral("push_to_talk")));
        QVERIFY(!e.effectiveMicMuted());
        QVERIFY(e.micMuted()); // the scene keeps the user's own mute
        QVERIFY(!r.scenes.dirty());
        r.controls.release(QStringLiteral("push_to_talk"));
        QVERIFY(e.effectiveMicMuted());

        e.setMicMuted(false);
        r.controls.press(QStringLiteral("push_to_mute"));
        QVERIFY(e.effectiveMicMuted());
        // Push to mute wins over push to talk.
        r.controls.press(QStringLiteral("push_to_talk"));
        QVERIFY(e.effectiveMicMuted());
        r.controls.release(QStringLiteral("push_to_mute"));
        r.controls.release(QStringLiteral("push_to_talk"));
        QVERIFY(!e.effectiveMicMuted());

        // Saving while a hold is on writes the user's own mute, and nothing about the hold.
        e.setMicMuted(true);
        r.controls.press(QStringLiteral("push_to_talk"));
        QVERIFY(r.scenes.save());
        const QString after = r.fileText(QStringLiteral("Live"));
        QCOMPARE(after, before);
        QVERIFY(toml_io::parseScene(after)->micBus()->muted);

        // Clicking the mic button while holding push to talk mutes for real and ends the hold.
        e.setMicMuted(true);
        QVERIFY(!e.pushToTalk());
        QVERIFY(e.effectiveMicMuted());

        r.controls.press(QStringLiteral("push_to_mute"));
        e.releaseHolds();
        QVERIFY(!e.pushToMute());
    }

    void panicRestoresAndSurvivesSwitch()
    {
        Rig r;
        auto &e = r.engine;
        r.scenes.setAutoSave(true);
        e.setMicMuted(false);
        r.scenes.save();
        r.controls.press(QStringLiteral("panic_mute"));
        QVERIFY(e.panic());
        QVERIFY(e.effectiveMicMuted());
        QVERIFY(e.effectiveStreamMuted());
        QVERIFY(!e.micMuted());
        QVERIFY(!e.scene().masterStreamMuted);
        QVERIFY(!r.scenes.dirty());

        // A scene switch does not lift panic.
        QVERIFY(r.scenes.switchTo(QStringLiteral("Ranked")));
        QVERIFY(e.effectiveMicMuted());
        QVERIFY(e.effectiveStreamMuted());
        QVERIFY(!r.fileText(QStringLiteral("Ranked")).contains(QStringLiteral("panic"), Qt::CaseInsensitive));

        // Pressing again brings both back as the scene has them.
        r.controls.press(QStringLiteral("panic_mute"));
        QVERIFY(!e.panic());
        QVERIFY(!e.effectiveMicMuted());
        QVERIFY(!e.effectiveStreamMuted());

        // Unmuting the mic by hand lifts only the mic half.
        r.controls.press(QStringLiteral("panic_mute"));
        e.setMicMuted(false);
        QVERIFY(!e.effectiveMicMuted());
        QVERIFY(e.effectiveStreamMuted());
        QVERIFY(e.panic());
        r.controls.press(QStringLiteral("mute_stream")); // toggles what is heard: unmutes
        QVERIFY(!e.effectiveStreamMuted());
        QVERIFY(!e.panic());
    }

    void busMuteActions()
    {
        Rig r;
        auto &e = r.engine;
        QCOMPARE(r.controls.press(QStringLiteral("mute_bus_game")).result, Result::Done);
        QVERIFY(e.scene().bus(QStringLiteral("game"))->muted);
        r.controls.press(QStringLiteral("mute_bus_game"));
        QVERIFY(!e.scene().bus(QStringLiteral("game"))->muted);
        // A bus that only another scene has.
        QCOMPARE(r.controls.press(QStringLiteral("mute_bus_soundboard")).result, Result::NoSuchBus);
        QCOMPARE(r.controls.press(QStringLiteral("mute_bus_mic")).result, Result::UnknownAction);

        const auto buses = r.controls.buses();
        QCOMPARE(buses.first().first, QStringLiteral("game"));
        QCOMPARE(buses.last(), qMakePair(QStringLiteral("soundboard"), QStringLiteral("Soundboard")));
        QVERIFY(std::none_of(buses.begin(), buses.end(),
                             [](const auto &b) { return b.first == QLatin1String("mic"); }));
        const QStringList ids = r.controls.actionIds();
        QVERIFY(ids.startsWith(actions::all().first()));
        QVERIFY(ids.contains(QStringLiteral("mute_bus_soundboard")));
        QVERIFY(ids.contains(QStringLiteral("mute_bus_desktop")));
        QVERIFY(!ids.contains(QStringLiteral("mute_bus_mic")));
    }

    void sceneActionsPickAScene()
    {
        Rig r;
        QCOMPARE(r.scenes.names(), (QStringList{QStringLiteral("Live"), QStringLiteral("Ranked")}));
        auto next = r.controls.press(QStringLiteral("next_scene"));
        QCOMPARE(next.result, Result::SwitchScene);
        QCOMPARE(next.scene, QStringLiteral("Ranked"));
        QCOMPARE(r.scenes.currentName(), QStringLiteral("Live")); // the caller switches
        QCOMPARE(r.controls.press(QStringLiteral("previous_scene")).scene, QStringLiteral("Ranked"));
        QCOMPARE(r.controls.press(QStringLiteral("scene_1")).scene, QStringLiteral("Live"));
        QCOMPARE(r.controls.press(QStringLiteral("scene_5")).result, Result::NoSuchScene);
    }

    void volumeArguments()
    {
        auto v = remote::parseVolumeArg(QStringLiteral("game=0.8"));
        QVERIFY(v);
        QCOMPARE(v->busId, QStringLiteral("game"));
        QCOMPARE(v->position, 0.8);
        v = remote::parseVolumeArg(QStringLiteral(" Music = 45% "));
        QVERIFY(v);
        QCOMPARE(v->busId, QStringLiteral("Music"));
        QCOMPARE(v->position, 0.45);
        QVERIFY(!remote::parseVolumeArg(QStringLiteral("game")));
        QVERIFY(!remote::parseVolumeArg(QStringLiteral("=0.5")));
        QVERIFY(!remote::parseVolumeArg(QStringLiteral("game=loud")));
        QVERIFY(!remote::parseVolumeArg(QStringLiteral("game=nan")));
        QCOMPARE(remote::maxPosition(QStringLiteral("mic")), 1.5);
        QCOMPARE(remote::maxPosition(QStringLiteral("stream")), 1.0);
    }
};

QTEST_GUILESS_MAIN(TestControls)
#include "tst_controls.moc"
