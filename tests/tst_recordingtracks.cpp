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

    void removesOtherAudioFromIsolatedTracks()
    {
        auto s = snapshot();
        Input browser = s.state.inputs.first();
        browser.name = QStringLiteral("Alerts");
        browser.kind = QStringLiteral("browser_source");
        browser.tracks = 63;
        browser.settings = {};
        s.state.inputs << browser;
        const auto p = recordingPlan(s, defaults::scene(), {{4, QStringLiteral("game")}});
        QCOMPARE(p.inputTracks.value(browser.name), 3u);
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
        QCOMPARE(parseSettings(serializeSettings(settings)), settings);
    }
};

QTEST_GUILESS_MAIN(TestRecordingTracks)
#include "tst_recordingtracks.moc"
