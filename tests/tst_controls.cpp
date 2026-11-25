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

#include <cmath>

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

    void takeShortcutClearsTheOtherAction()
    {
        using namespace actions;
        const QString mic = QString::fromLatin1(kMuteMic), stream = QString::fromLatin1(kMuteStream),
                      talk = QString::fromLatin1(kPushToTalk), game = QStringLiteral("mute_bus_game");
        const QStringList ids = all() + QStringList{game};
        QMap<QString, QString> keys = defaultSettings().hotkeys;
        keys.insert(game, QStringLiteral("Meta+Alt+G"));

        // A free shortcut: only the action itself changes.
        auto free = keys;
        QCOMPARE(takeShortcut(free, ids, talk, QStringLiteral("Meta+Alt+T")), QStringList{});
        QCOMPARE(free.value(talk), QStringLiteral("Meta+Alt+T"));
        QCOMPARE(free.value(mic), QStringLiteral("Meta+Alt+M"));

        // Replace: the action gets the shortcut, the one that had it is unbound, the rest stay.
        auto taken = keys;
        QCOMPARE(takeShortcut(taken, ids, talk, QStringLiteral("Meta+Alt+M")), QStringList{mic});
        QCOMPARE(taken.value(talk), QStringLiteral("Meta+Alt+M"));
        QVERIFY(taken.contains(mic));
        QVERIFY(taken.value(mic).isEmpty());
        QCOMPARE(taken.value(stream), QStringLiteral("Meta+Alt+S"));
        QCOMPARE(taken.value(game), QStringLiteral("Meta+Alt+G"));
        int holders = 0;
        for (const QString &id : ids) {
            holders += taken.value(id, defaultShortcut(id)) == QStringLiteral("Meta+Alt+M");
        }
        QCOMPARE(holders, 1);

        // A bus action loses it too, and so does an action still on its default with no entry.
        auto bus = keys;
        QCOMPARE(takeShortcut(bus, ids, mic, QStringLiteral("Meta+Alt+G")), QStringList{game});
        QVERIFY(bus.value(game).isEmpty());
        auto implicit = keys;
        implicit.remove(stream);
        QCOMPARE(takeShortcut(implicit, ids, talk, QStringLiteral("Meta+Alt+S")), QStringList{stream});
        QVERIFY(implicit.contains(stream));
        QVERIFY(implicit.value(stream).isEmpty());

        // Unbinding never touches the other unbound actions, and the result survives a save.
        auto none = taken;
        QCOMPARE(takeShortcut(none, ids, talk, QString()), QStringList{});
        QVERIFY(none.value(talk).isEmpty());
        QVERIFY(none.value(mic).isEmpty());
        Settings s = defaultSettings();
        s.hotkeys = taken;
        QCOMPARE(parseSettings(serializeSettings(s)).hotkeys, taken);
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

    void mixTargets()
    {
        auto t = remote::splitMixTarget(QStringLiteral("game.phones"));
        QCOMPARE(t.bus, QStringLiteral("game"));
        QCOMPARE(t.mix, std::optional(Mix::Phones));
        t = remote::splitMixTarget(QStringLiteral("Game.Stream"));
        QCOMPARE(t.bus, QStringLiteral("Game"));
        QCOMPARE(t.mix, std::optional(Mix::Stream));
        for (const char *whole : {"stream", "game", "x.vod", ".stream", "game."}) {
            t = remote::splitMixTarget(QString::fromLatin1(whole));
            QCOMPARE(t.bus, QString::fromLatin1(whole));
            QVERIFY(!t.mix);
        }
    }

    void micHasOneLevel()
    {
        Scene scene = defaults::scene();
        auto r = remote::resolveVolumeTarget(scene, QStringLiteral("mic.phones"));
        QCOMPARE(r.error, QString::fromLatin1(remote::kErrorInvalidValue));
        r = remote::resolveVolumeTarget(scene, QStringLiteral("Mic.Stream"));
        QCOMPARE(r.error, QString::fromLatin1(remote::kErrorInvalidValue));
        r = remote::resolveVolumeTarget(scene, QStringLiteral("phones.stream")); // a master
        QCOMPARE(r.error, QString::fromLatin1(remote::kErrorInvalidValue));

        r = remote::resolveVolumeTarget(scene, QStringLiteral("Game.Phones"));
        QVERIFY(r.error.isEmpty());
        QCOMPARE(r.busId, QStringLiteral("game"));
        QCOMPARE(r.mix, std::optional(Mix::Phones));
        r = remote::resolveVolumeTarget(scene, QStringLiteral("game"));
        QCOMPARE(r.busId, QStringLiteral("game"));
        QVERIFY(!r.mix);
        r = remote::resolveVolumeTarget(scene, QStringLiteral("Stream"));
        QCOMPARE(r.busId, QString::fromLatin1(remote::kStreamId));
        r = remote::resolveVolumeTarget(scene, QStringLiteral("nowhere.stream"));
        QCOMPARE(r.error, QString::fromLatin1(remote::kErrorNoSuchBus));

        // The whole text wins over a mix suffix: a bus can be called "Chat.Stream".
        Bus chat;
        chat.id = QStringLiteral("chat");
        chat.name = QStringLiteral("Chat.Stream");
        scene.buses.append(chat);
        r = remote::resolveVolumeTarget(scene, QStringLiteral("chat.stream"));
        QCOMPARE(r.busId, QStringLiteral("chat"));
        QVERIFY(!r.mix);
    }

    void sides()
    {
        QCOMPARE(remote::parseSide(QStringLiteral("stream")), std::optional(remote::Side::Stream));
        QCOMPARE(remote::parseSide(QStringLiteral("Phones")), std::optional(remote::Side::Phones));
        QCOMPARE(remote::parseSide(QStringLiteral("both")), std::optional(remote::Side::Both));
        QVERIFY(!remote::parseSide(QString()));
        QVERIFY(!remote::parseSide(QStringLiteral("vod")));
        QCOMPARE(remote::sideName(remote::Side::Phones), QStringLiteral("phones"));
    }

    void stepRequests()
    {
        using P = remote::StepProblem;
        const Scene scene = defaults::scene();
        const auto step = [&](const char *bus, const char *side, double delta, bool perMix = true) {
            return remote::resolveStep(scene, QString::fromLatin1(bus), QString::fromLatin1(side), delta, perMix);
        };
        auto t = step("Game", "stream", -0.05);
        QCOMPARE(t.problem, P::None);
        QCOMPARE(t.busId, QStringLiteral("game"));
        QCOMPARE(t.side, remote::Side::Stream);
        QCOMPARE(step("nowhere", "both", 0.1).problem, P::NoSuchBus);
        QCOMPARE(step("game", "sideways", 0.1).problem, P::BadSide);
        QCOMPARE(step("mic", "stream", 0.1).problem, P::OneLevelBus);
        QCOMPARE(step("stream", "phones", 0.1).problem, P::OneLevelBus);
        QCOMPARE(step("mic", "both", 0.1).problem, P::None);
        QCOMPARE(step("game", "both", 1.5).problem, P::BadDelta);
        QCOMPARE(step("game", "both", std::nan("")).problem, P::BadDelta);
        QCOMPARE(step("game", "both", -1.0).problem, P::None);
        QCOMPARE(step("game", "stream", 0.1, false).problem, P::NoPerMixLevels);
        QCOMPARE(step("game", "both", 0.1, false).problem, P::None);
        QCOMPARE(step("mic", "both", 0.1, false).problem, P::None);
    }

    void inMixRequests()
    {
        using P = remote::InMixProblem;
        const Scene scene = defaults::scene();
        const auto in = [&](const char *bus, const char *side) {
            return remote::resolveInMix(scene, QString::fromLatin1(bus), QString::fromLatin1(side));
        };
        QCOMPARE(in("Game", "stream").problem, P::None);
        QCOMPARE(in("Game", "stream").busId, QStringLiteral("game"));
        QCOMPARE(in("game", "phones").mix, Mix::Phones);
        QCOMPARE(in("stream", "stream").problem, P::NotPlaybackBus);
        QCOMPARE(in("mic", "phones").problem, P::NotPlaybackBus);
        QCOMPARE(in("game", "both").problem, P::BadSide);
        QCOMPARE(in("nowhere", "stream").problem, P::NoSuchBus);
    }

    void parseSideChat()
    {
        QCOMPARE(remote::parseSide(QStringLiteral("chat")), std::optional(remote::Side::Chat));
        QCOMPARE(remote::parseSide(QStringLiteral("Chat")), std::optional(remote::Side::Chat));
        QCOMPARE(remote::sideName(remote::Side::Chat), QStringLiteral("chat"));
    }

    void inMixChatOnSoundsBus()
    {
        using P = remote::InMixProblem;
        const Scene scene = defaults::scene();
        const auto t = remote::resolveInMix(scene, QStringLiteral("Game"), QStringLiteral("chat"));
        QCOMPARE(t.problem, P::None);
        QCOMPARE(t.busId, QStringLiteral("game"));
        QVERIFY(t.chat);
        QVERIFY(!remote::resolveInMix(scene, QStringLiteral("game"), QStringLiteral("stream")).chat);
        QCOMPARE(remote::resolveInMix(scene, QStringLiteral("mic"), QStringLiteral("chat")).problem,
                 P::NotPlaybackBus);
        QCOMPARE(remote::resolveInMix(scene, QStringLiteral("stream"), QStringLiteral("chat")).problem,
                 P::NotPlaybackBus);
    }

    void inMixChatRefusedOnVoiceBus()
    {
        const Scene scene = defaults::scene();
        QVERIFY(scene.bus(QStringLiteral("voice")));
        QCOMPARE(remote::resolveInMix(scene, QStringLiteral("voice"), QStringLiteral("chat")).problem,
                 remote::InMixProblem::BadSide);
    }

    void stepChatMovesOnlyChatLevel()
    {
        using P = remote::StepProblem;
        const Scene scene = defaults::scene();
        auto t = remote::resolveStep(scene, QStringLiteral("game"), QStringLiteral("chat"), 0.05, true);
        QCOMPARE(t.problem, P::None);
        QCOMPARE(t.side, remote::Side::Chat);
        QCOMPARE(remote::resolveStep(scene, QStringLiteral("voice"), QStringLiteral("chat"), 0.05, true).problem,
                 P::BadSide);
        QCOMPARE(remote::resolveStep(scene, QStringLiteral("mic"), QStringLiteral("chat"), 0.05, true).problem,
                 P::OneLevelBus);
        QCOMPARE(remote::resolveStep(scene, QStringLiteral("game"), QStringLiteral("chat"), 0.05, false).problem,
                 P::NoPerMixLevels);
        QCOMPARE(remote::resolveStep(scene, QStringLiteral("game"), QStringLiteral("chat"), 2.0, true).problem,
                 P::BadDelta);
    }

    void volumeTargetChatSuffix()
    {
        const Scene scene = defaults::scene();
        auto t = remote::splitMixTarget(QStringLiteral("game.chat"));
        QCOMPARE(t.bus, QStringLiteral("game"));
        QVERIFY(t.chat);
        QVERIFY(!t.mix);
        auto r = remote::resolveVolumeTarget(scene, QStringLiteral("Game.Chat"));
        QVERIFY(r.error.isEmpty());
        QCOMPARE(r.busId, QStringLiteral("game"));
        QVERIFY(r.chat);
        QVERIFY(!r.mix);
        QCOMPARE(remote::resolveVolumeTarget(scene, QStringLiteral("mic.chat")).error,
                 QString::fromLatin1(remote::kErrorInvalidValue));
        QCOMPARE(remote::resolveVolumeTarget(scene, QStringLiteral("voice.chat")).error,
                 QString::fromLatin1(remote::kErrorInvalidValue));
        QCOMPARE(remote::resolveVolumeTarget(scene, QStringLiteral("nowhere.chat")).error,
                 QString::fromLatin1(remote::kErrorNoSuchBus));
        QVERIFY(!remote::resolveVolumeTarget(scene, QStringLiteral("game")).chat);
    }

    void toggleChatFirstPressSetsUp()
    {
        const auto to = remote::toggledChat(false, true); // a stale muted flag does not matter
        QVERIFY(to.chat);
        QVERIFY(!to.muted);
    }

    void toggleChatThenFlipsMuteOnly()
    {
        auto to = remote::toggledChat(true, false);
        QVERIFY(to.chat);
        QVERIFY(to.muted);
        to = remote::toggledChat(true, true);
        QVERIFY(to.chat);
        QVERIFY(!to.muted);
    }

    void toggleInMixTruthTable()
    {
        using D = Destination;
        struct Row { D d; bool muted; Mix mix; D toD; bool toMuted; };
        const Row rows[] = {
            {D::Both, false, Mix::Stream, D::Phones, false}, {D::Stream, false, Mix::Stream, D::Stream, true},
            {D::Phones, false, Mix::Stream, D::Both, false}, {D::Both, true, Mix::Stream, D::Stream, false},
            {D::Stream, true, Mix::Stream, D::Stream, false}, {D::Phones, true, Mix::Stream, D::Stream, false},
            {D::Both, false, Mix::Phones, D::Stream, false}, {D::Phones, false, Mix::Phones, D::Phones, true},
            {D::Stream, false, Mix::Phones, D::Both, false}, {D::Both, true, Mix::Phones, D::Phones, false},
            {D::Phones, true, Mix::Phones, D::Phones, false}, {D::Stream, true, Mix::Phones, D::Phones, false},
        };
        for (const Row &r : rows) {
            const auto after = remote::toggledInMix(r.d, r.muted, r.mix);
            QCOMPARE(after, (remote::InMix{r.toD, r.toMuted}));
            const Mix other = r.mix == Mix::Stream ? Mix::Phones : Mix::Stream;
            QCOMPARE(remote::playsIn(after.destination, after.muted, other), remote::playsIn(r.d, r.muted, other));
            QCOMPARE(remote::playsIn(after.destination, after.muted, r.mix), !remote::playsIn(r.d, r.muted, r.mix));
        }
    }

    void stepMovesOneLevelAlone()
    {
        Rig rig;
        auto &e = rig.engine;
        const auto game = [&] { return *e.scene().bus(QStringLiteral("game")); };
        QVERIFY(game().linked);
        e.setBusVolume(QStringLiteral("game"), 0.5);
        e.setBusMixVolumeAlone(QStringLiteral("game"), Mix::Stream, 0.3);
        QCOMPARE(game().volume, 0.3);
        QCOMPARE(game().phonesVolume, 0.5); // linked, yet Headphones stayed
        QVERIFY(game().linked);
        e.setBusMixVolumeTogether(QStringLiteral("game"), Mix::Stream, 0.4); // a Both step keeps the gap
        QCOMPARE(game().volume, 0.4);
        QCOMPARE(game().phonesVolume, 0.6);
        e.setBusMixVolumeAlone(QStringLiteral("game"), Mix::Stream, 1.0);
        e.setBusMixVolumeAlone(QStringLiteral("game"), Mix::Phones, 0.5);
        e.setBusMixVolumeTogether(QStringLiteral("game"), Mix::Stream, 1.1); // stops at the top
        QCOMPARE(game().volume, 1.0);
        QCOMPARE(game().phonesVolume, 0.5);
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
