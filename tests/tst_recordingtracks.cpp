#include "RecordingFixture.h"
#include "core/Model.h"
#include "core/Settings.h"
#include "obs/RecordingTracks.h"

#include <QTest>

using namespace rostrum;
using namespace rostrum::obs;

class TestRecordingTracks : public QObject
{
    Q_OBJECT
    RecordingSnapshot snapshot() const
    {
        RecordingSnapshot s;
        s.collection = QStringLiteral("Test");
        s.profile = QStringLiteral("Test");
        s.outputMode = QStringLiteral("Advanced");
        s.recordingTracks = 3;
        s.known = true;
        s.state.scenes = {QStringLiteral("Game"), QStringLiteral("BRB")};
        for (const auto &name : s.state.scenes)
            s.sceneItems.insert(name, {});
        Input mic;
        mic.name = QStringLiteral("Mic/Aux");
        mic.kind = QLatin1String(kPulseInput);
        mic.settings = {{QStringLiteral("device_id"), QLatin1String(kMicDevice)}};
        mic.tracks = 3;
        mic.settingsKnown = mic.tracksKnown = mic.muteKnown = true;
        mic.channel = QStringLiteral("mic1");
        s.state.inputs << mic;
        return s;
    }

private Q_SLOTS:
    void suggestionsKeepOccupiedTracksEvenWhenMuted()
    {
        auto s = snapshot();
        auto capture = s.state.inputs.first();
        capture.name = QStringLiteral("My audio");
        capture.tracks = 8; // track 4
        capture.muted = true;
        s.state.inputs << capture;
        const auto suggestion = recordingSuggestion(defaults::scene(), s);
        QCOMPARE(suggestion.value(3), QStringLiteral("mic"));
        QVERIFY(!suggestion.contains(4));
        QCOMPARE(suggestion.value(5), QStringLiteral("voice"));
        QCOMPARE(suggestion.value(6), QStringLiteral("music"));
        s.known = false;
        QVERIFY(recordingSuggestion(defaults::scene(), s).isEmpty());
    }

    void omittedTracksKeepObsAssignments()
    {
        auto s = snapshot();
        s.recordingTracks = 63;
        s.state.inputs.first().tracks = 63;
        s.state.inputs.first().muted = true;
        const auto p = recordingPlan(s, defaults::scene(), {});
        QVERIFY(p.problems.isEmpty());
        QVERIFY(p.changes.isEmpty());
        QCOMPARE(p.recordingTracks, 63u);
    }

    void replacementPreservesOtherTracksAndUndo()
    {
        auto s = snapshot();
        s.recordingTracks = 63;
        auto browser = s.state.inputs.first();
        browser.name = QStringLiteral("Kick Chat");
        browser.kind = QStringLiteral("browser_source");
        browser.settings = {};
        browser.tracks = 63;
        browser.muted = true;
        s.state.inputs << browser;
        const RecordingAssignments a{{4, QStringLiteral("game")}};
        const auto p = recordingPlan(s, defaults::scene(), a);
        QVERIFY(p.problems.isEmpty());
        QCOMPARE(p.inputTracks.value(browser.name), 55u);
        QCOMPARE(p.recordingTracks, 63u);
        const auto after = recordingResult(s, p);
        QVERIFY(after.state.input(browser.name)->muted);
        QVERIFY(recordingPlan(after, defaults::scene(), a).changes.isEmpty());
        const auto restored = recordingResult(after, recordingUndo(p));
        QCOMPARE(restored.state.input(browser.name)->tracks, browser.tracks);
        QVERIFY(restored.state.input(browser.name)->muted);
        QCOMPARE(restored.recordingTracks, s.recordingTracks);
    }

    void unusedClearsOnlyChosenTrack()
    {
        auto s = snapshot();
        s.recordingTracks = 63;
        s.state.inputs.first().tracks = 63;
        const auto p = recordingPlan(s, defaults::scene(), {{5, QString()}});
        QVERIFY(p.problems.isEmpty());
        QCOMPARE(p.inputTracks.value(QStringLiteral("Mic/Aux")), 47u);
        QCOMPARE(p.recordingTracks, 47u);
        const auto after = recordingResult(s, p);
        QVERIFY(recordingPlan(after, defaults::scene(), {{5, QString()}}).changes.isEmpty());
        const auto restored = recordingResult(after, recordingUndo(p));
        QCOMPARE(restored.state.inputs.first().tracks, 63u);
        QCOMPARE(restored.recordingTracks, 63u);
    }

    void sharedCaptureIsCopiedWithoutChangingKeptTracks()
    {
        auto s = snapshot();
        s.recordingTracks = 63;
        auto capture = s.state.inputs.first();
        capture.name = QStringLiteral("My Game Capture");
        capture.kind = QLatin1String(kPulseOutput);
        capture.settings = {{QStringLiteral("device_id"), QStringLiteral("rostrum.game.monitor")}};
        capture.tracks = 17; // kept tracks 1 and 5
        capture.muted = true;
        capture.channel.clear();
        s.state.inputs << capture;
        const RecordingAssignments a{{4, QStringLiteral("game")}};
        const auto p = recordingPlan(s, defaults::scene(), a);
        QVERIFY(p.problems.isEmpty());
        const auto after = recordingResult(s, p);
        QCOMPARE(after.state.input(capture.name)->tracks, capture.tracks);
        QVERIFY(after.state.input(capture.name)->muted);
        QCOMPARE(after.state.input(QStringLiteral("Rostrum Game (Recording)"))->tracks, 8u);
        QVERIFY(recordingPlan(after, defaults::scene(), a).changes.isEmpty());
    }

    void keptMicMuteIsNotChangedByNewMicTrack()
    {
        auto s = snapshot();
        s.state.inputs.first().muted = true;
        const auto p = recordingPlan(s, defaults::scene(), {{3, QStringLiteral("mic")}});
        QVERIFY(p.problems.isEmpty());
        const auto after = recordingResult(s, p);
        QVERIFY(after.state.input(QStringLiteral("Mic/Aux"))->muted);
        QCOMPARE(after.state.input(QStringLiteral("Mic/Aux"))->tracks, 3u);
        QCOMPARE(after.state.input(QStringLiteral("Rostrum Mic (Recording)"))->tracks, 4u);
        QVERIFY(recordingPlan(after, defaults::scene(), {{3, QStringLiteral("mic")}}).changes.isEmpty());
        const auto restored = recordingResult(after, recordingUndo(p));
        QCOMPARE(restored.state.inputs.size(), s.state.inputs.size());
        QVERIFY(restored.state.inputs.first().muted);
    }

    void isolatesGameFromMusic()
    {
        auto s = snapshot();
        auto scene = defaults::scene();
        scene.bus(QStringLiteral("game"))->destination = Destination::Phones;
        const auto p = recordingPlan(
            s, scene,
            {{3, QStringLiteral("mic")}, {4, QStringLiteral("game")}, {6, QStringLiteral("music")}});
        QVERIFY(p.problems.isEmpty());
        QCOMPARE(p.recordingTracks, 47u); // tracks 1, 2, 3, 4, 6; unused track 5 stays off
        QCOMPARE(p.inputTracks.value(QStringLiteral("Mic/Aux")), 7u);
        QCOMPARE(p.inputTracks.value(QStringLiteral("Rostrum Game (Recording)")), 8u);
        QCOMPARE(p.inputTracks.value(QStringLiteral("Rostrum Music (Recording)")), 32u);
        QCOMPARE(scene.bus(QStringLiteral("game"))->destination, Destination::Phones);
    }

    void filteredMicRecording()
    {
        const auto before = snapshot();
        const auto scene = defaults::scene();
        const RecordingAssignments a{{3, QStringLiteral("mic")}};
        const auto plan = recordingPlan(before, scene, a, true);
        QVERIFY(plan.problems.isEmpty());
        const auto after = recordingResult(before, plan);
        const auto *mic = after.state.input(QStringLiteral("Rostrum Mic (Recording)"));
        QVERIFY(mic);
        QCOMPARE(mic->settings.value(QStringLiteral("device_id")).toString(),
                 QStringLiteral("rostrum.filtered"));
        QCOMPARE(mic->tracks, 4u);
        QCOMPARE(after.state.input(QStringLiteral("Mic/Aux"))->tracks, 3u);
        QVERIFY(recordingPlan(after, scene, a, true).changes.isEmpty());
        const auto restored = recordingResult(after, recordingUndo(plan));
        QCOMPARE(restored.state.inputs.size(), before.state.inputs.size());
        QCOMPARE(restored.state.inputs.first().tracks, before.state.inputs.first().tracks);
        QCOMPARE(restored.recordingTracks, before.recordingTracks);
        QCOMPARE(restored.sceneItems, before.sceneItems);
        const auto plain = recordingResult(after, recordingPlan(after, scene, a, false));
        QCOMPARE(plain.state.input(QStringLiteral("Mic/Aux"))->tracks, 7u);
        QCOMPARE(plain.state.input(mic->name)->tracks, 0u);
        const auto filteredAgain = recordingResult(plain, recordingPlan(plain, scene, a, true));
        QCOMPARE(filteredAgain.state.inputs.size(), after.state.inputs.size());
        QCOMPARE(filteredAgain.state.input(mic->name)->tracks, 4u);
        QCOMPARE(filteredAgain.state.input(QStringLiteral("Mic/Aux"))->tracks, 3u);
        for (const bool filters : {false, true}) {
            const auto devices = recordingDevices(scene, a, filters);
            QVERIFY(intendedRecording(*mic, devices));
            const auto stream = makePlan(after.state, {}, Mode::Live, devices);
            for (const auto &action : stream.actions)
                QVERIFY(action.type != Action::Type::Mute || action.input != mic->name);
        }
    }

    void secondApplyIsNoOp()
    {
        auto s = snapshot();
        const auto scene = defaults::scene();
        const RecordingAssignments a{{3, QStringLiteral("mic")}, {4, QStringLiteral("game")}};
        const auto p = recordingPlan(s, scene, a);
        QVERIFY(p.problems.isEmpty());
        auto after = recordingResult(s, p);
        const auto second = recordingPlan(after, scene, a);
        QVERIFY(second.problems.isEmpty());
        QVERIFY(second.changes.isEmpty());
    }

    void recordingOnlyMicDoesNotReplaceStreamMic()
    {
        auto s = snapshot();
        s.state.inputs.clear();
        const RecordingAssignments a{{3, QStringLiteral("mic")}};
        const auto scene = defaults::scene();
        const auto p = recordingPlan(s, scene, a);
        QVERIFY(p.problems.isEmpty());
        QCOMPARE(p.inputTracks.value(QStringLiteral("Rostrum Mic (Recording)")), 4u);
        const auto after = recordingResult(s, p);
        const auto stream = makePlan(after.state, {}, Mode::Live, recordingDevices(scene, a));
        bool createsStreamMic = false;
        for (const auto &action : stream.actions) {
            if (action.role == Action::Role::Mic && action.type == Action::Type::CreateInput) {
                QCOMPARE(action.tracks, 3u);
                createsStreamMic = true;
            }
            QVERIFY(action.type != Action::Type::Mute ||
                    action.input != QStringLiteral("Rostrum Mic (Recording)"));
        }
        QVERIFY(createsStreamMic);
    }

    void undoRestoresOldTracksIncludingZero()
    {
        auto s = snapshot();
        s.state.inputs.first().tracks = 0;
        const auto p =
            recordingPlan(s, defaults::scene(), {{3, QStringLiteral("mic")}, {4, QStringLiteral("game")}});
        const auto after = recordingResult(s, p);
        const auto restored = recordingResult(after, recordingUndo(p));
        QCOMPARE(restored.state.inputs.size(), s.state.inputs.size());
        QCOMPARE(restored.state.inputs.first().tracks, 0u);
        QCOMPARE(restored.recordingTracks, s.recordingTracks);
        QCOMPARE(restored.sceneItems, s.sceneItems);
    }

    void unknownBusIsReported()
    {
        const auto p = recordingPlan(snapshot(), defaults::scene(), {{4, QStringLiteral("deleted")}});
        QVERIFY(!p.problems.isEmpty());
        QVERIFY(p.changes.isEmpty());
    }

    void missingAssignedBusStillIsNotConflictMuted()
    {
        const RecordingAssignments a{{4, QStringLiteral("game")}};
        const auto after = recordingResult(snapshot(), recordingPlan(snapshot(), defaults::scene(), a));
        auto scene = defaults::scene();
        scene.buses.removeIf([](const Bus &bus) { return bus.id == QLatin1String("game"); });
        QVERIFY(!recordingPlan(after, scene, a).problems.isEmpty());
        const auto stream = makePlan(after.state, {}, Mode::Live, recordingDevices(scene, a));
        for (const auto &action : stream.actions)
            QVERIFY(action.type != Action::Type::Mute ||
                    action.input != QStringLiteral("Rostrum Game (Recording)"));
    }

    void assignedCaptureSurvivesConflictMuter()
    {
        const auto scene = defaults::scene();
        const RecordingAssignments a{{4, QStringLiteral("game")}};
        const auto after = recordingResult(snapshot(), recordingPlan(snapshot(), scene, a));
        const auto stream = makePlan(after.state, {}, Mode::Live, recordingDevices(scene, a));
        for (const auto &action : stream.actions)
            QVERIFY(action.type != Action::Type::Mute ||
                    action.input != QStringLiteral("Rostrum Game (Recording)"));
        const auto unassigned = makePlan(after.state, {}, Mode::Live);
        bool muted = false;
        for (const auto &action : unassigned.actions)
            muted |= action.type == Action::Type::Mute &&
                     action.input == QStringLiteral("Rostrum Game (Recording)");
        QVERIFY(muted);
    }

    void refusesActiveOutputsAndDuplicateBus()
    {
        auto s = snapshot();
        s.streaming = true;
        QVERIFY(!recordingPlan(s, defaults::scene(), {{4, QStringLiteral("game")}}).problems.isEmpty());
        s.streaming = false;
        s.recording = true;
        QVERIFY(!recordingPlan(s, defaults::scene(), {{4, QStringLiteral("game")}}).problems.isEmpty());
        s.recording = false;
        QVERIFY(
            !recordingPlan(s, defaults::scene(), {{4, QStringLiteral("game")}, {5, QStringLiteral("game")}})
                 .problems.isEmpty());
        s.recordingType = QStringLiteral("FFmpeg");
        QVERIFY(!recordingPlan(s, defaults::scene(), {{4, QStringLiteral("game")}}).problems.isEmpty());
    }

    void removesOtherAudioOnlyFromChosenTrack()
    {
        auto s = snapshot();
        Input browser = s.state.inputs.first();
        browser.name = QStringLiteral("Alerts");
        browser.kind = QStringLiteral("browser_source");
        browser.tracks = 63;
        browser.settings = {};
        s.state.inputs << browser;
        const auto p = recordingPlan(s, defaults::scene(), {{4, QStringLiteral("game")}});
        QCOMPARE(p.inputTracks.value(browser.name), 55u);
    }

    void reusesDisabledPlacementAndUndoRestoresIt()
    {
        const RecordingAssignments a{{4, QStringLiteral("game")}};
        auto s = recordingResult(snapshot(), recordingPlan(snapshot(), defaults::scene(), a));
        auto item = s.sceneItems[QStringLiteral("Game")].first().toObject();
        item[QStringLiteral("sceneItemEnabled")] = false;
        item[QStringLiteral("sceneItemId")] = 42;
        s.sceneItems[QStringLiteral("Game")][0] = item;
        const auto p = recordingPlan(s, defaults::scene(), a);
        QVERIFY(p.problems.isEmpty());
        QVERIFY(!p.changes.isEmpty());
        for (const auto &c : p.changes)
            QVERIFY(c.method != QLatin1String("CreateSceneItem"));
        QCOMPARE(recordingResult(recordingResult(s, p), recordingUndo(p)).sceneItems, s.sceneItems);
    }

    void settingsKeepCollectionAndBusIds()
    {
        Settings settings = defaultSettings();
        settings.obsRecordingTracks[QStringLiteral("collection-a")] = {
            {QStringLiteral("3"), QStringLiteral("mic")},
            {QStringLiteral("4"), QStringLiteral("custom-game")},
            {QStringLiteral("5"), QString()}};
        settings.obsRecordingTracks[QStringLiteral("collection-b")] = {
            {QStringLiteral("6"), QStringLiteral("music")}};
        settings.obsRecordingTracks[QStringLiteral("keep-all")] = {};
        const auto restored = parseSettings(serializeSettings(settings));
        QCOMPARE(restored, settings);
        QVERIFY(!restored.obsRecordingTracks.value(QStringLiteral("collection-a")).contains(QStringLiteral("6")));
        QVERIFY(restored.obsRecordingTracks.value(QStringLiteral("collection-a")).contains(QStringLiteral("5")));
        QVERIFY(restored.obsRecordingTracks.contains(QStringLiteral("keep-all")));
    }
};

QTEST_GUILESS_MAIN(TestRecordingTracks)
#include "tst_recordingtracks.moc"
