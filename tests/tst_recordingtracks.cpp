#include "RecordingFixture.h"
#include "app/AppController.h"
#include "app/Desktop.h"
#include "app/Obs.h"
#include "app/ObsRecording.h"
#include "core/Model.h"
#include "core/Settings.h"
#include "obs/ObsPlan.h"
#include "obs/RecordingTracks.h"
#include "obs/SceneCollection.h"

#include <KLocalizedString>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QWebSocket>
#include <QWebSocketServer>
#include <sys/prctl.h>
#include <utility>

namespace rostrum::app {
Desktop *Desktop::s_instance = nullptr;
void Desktop::notifyGoLive(const QString &) {}
} // namespace rostrum::app

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

    void restoredSettingsSynchronizeDraftAndFallBackWhenCleared()
    {
        const auto scene = defaults::scene();
        const auto snap = snapshot();
        const QString configDir = QStringLiteral("/tmp/obs-studio");
        const QString scope = recordingScopeKey(configDir, snap.collection);
        QVERIFY(!scope.isEmpty());
        QVERIFY(scope != recordingScopeKey(configDir, QStringLiteral("Other")));

        const RecordingAssignments first{{3, QStringLiteral("mic")},
                                         {4, QString()},
                                         {6, QStringLiteral("desktop")}};
        QMap<QString, QMap<QString, QString>> saved{{scope, savedRecordingAssignments(first)}};
        QCOMPARE(recordingDraft(saved, configDir, scene, snap), first);

        // Restoring different assignments for the same active OBS collection replaces the draft.
        const RecordingAssignments second{{5, QStringLiteral("voice")}};
        saved[scope] = savedRecordingAssignments(second);
        QCOMPARE(recordingDraft(saved, configDir, scene, snap), second);

        // Restoring an explicit "keep all OBS assignments" record keeps an empty draft
        // rather than re-populating first-time bus suggestions.
        saved[scope] = {};
        QVERIFY(recordingDraft(saved, configDir, scene, snap).isEmpty());

        // Restoring a backup with no saved record for the current collection falls back
        // to the live snapshot's suggestion when known, or an empty draft when unknown.
        saved.remove(scope);
        QCOMPARE(recordingDraft(saved, configDir, scene, snap), recordingSuggestion(scene, snap));
        auto unknown = snap;
        unknown.known = false;
        QVERIFY(recordingDraft(saved, configDir, scene, unknown).isEmpty());
    }
    void initTestCase()
    {
        KLocalizedString::setApplicationDomain("rostrum");
        QStandardPaths::setTestModeEnabled(true);
    }

    void previewAndRefreshIgnoreStaleCallbacksAcrossInvalidationAndRestore()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        QWebSocketServer server(QStringLiteral("obs-test"), QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QList<QWebSocket *> sockets;
        QObject::connect(&server, &QWebSocketServer::newConnection, &server, [&] {
            while (auto *sock = server.nextPendingConnection()) {
                sockets << sock;
                QObject::connect(sock, &QWebSocket::textMessageReceived, sock, [sock](const QString &text) {
                    const auto msg = QJsonDocument::fromJson(text.toUtf8()).object();
                    if (msg.value(QLatin1String("op")).toInt() == 1) {
                        sock->sendTextMessage(QString::fromUtf8(
                            QJsonDocument(QJsonObject{{QStringLiteral("op"), 2},
                                                      {QStringLiteral("d"),
                                                       QJsonObject{{QStringLiteral("negotiatedRpcVersion"), 1}}}})
                                .toJson(QJsonDocument::Compact)));
                    }
                });
                sock->sendTextMessage(QString::fromUtf8(
                    QJsonDocument(QJsonObject{{QStringLiteral("op"), 0},
                                              {QStringLiteral("d"),
                                               QJsonObject{{QStringLiteral("obsWebSocketVersion"),
                                                            QStringLiteral("5.5.0")},
                                                           {QStringLiteral("rpcVersion"), 1}}}})
                        .toJson(QJsonDocument::Compact)));
            }
        });

        app::AppController app(nullptr);
        app::Obs obs(&app, &app);
        app::ObsRecording *rec = obs.recordingTracks();
        QVERIFY(rec);

        QList<app::ObsRecording::SnapshotCallback> pending;
        rec->setSnapshotProvider(
            [&](obs::Client *, app::ObsRecording::SnapshotCallback cb) { pending << std::move(cb); },
            tempDir.path());

        rec->client()->open(server.serverPort(), {});
        QTRY_COMPARE(rec->client()->status(), obs::Client::Status::Connected);

        const auto snap = snapshot();
        const QString scope = recordingScopeKey(tempDir.path(), snap.collection);
        auto trackBus = [rec](int track) {
            for (const auto &v : rec->rows()) {
                const auto row = v.toMap();
                if (row.value(QStringLiteral("track")).toInt() == track)
                    return row.value(QStringLiteral("busId")).toString();
            }
            return QString();
        };

        // Initial connect triggered refresh(); complete it with a valid snapshot.
        QCOMPARE(pending.size(), 1);
        QVERIFY(rec->fetching());
        pending.takeFirst()(snap, {});
        QVERIFY(!rec->fetching());
        QVERIFY(rec->canPreview());
        QVERIFY(!rec->canApply());
        QCOMPARE(trackBus(3), QStringLiteral("mic"));
        QCOMPARE(trackBus(4), QStringLiteral("game"));

        QSignalSpy previewReadySpy(rec, &app::ObsRecording::previewReady);

        // 1. Settings restore while preview() is pending (delayed preview callback arrives before
        //    the post-restore refresh() callback completes).
        rec->choose(4, QStringLiteral("desktop"));
        QCOMPARE(trackBus(4), QStringLiteral("desktop"));
        rec->preview();
        QCOMPARE(pending.size(), 1);
        QVERIFY(rec->fetching());
        auto stalePreviewBeforeRefresh = pending.takeFirst();

        app.settings().obsRecordingTracks[scope] = {{QStringLiteral("5"), QStringLiteral("voice")}};
        obs.settingsRestored();
        QCOMPARE(pending.size(), 1);
        QVERIFY(rec->fetching());
        QVERIFY(!rec->canApply());
        QCOMPARE(trackBus(3), QStringLiteral(":keep-obs:"));
        QCOMPARE(trackBus(4), QStringLiteral(":keep-obs:"));
        QCOMPARE(trackBus(5), QStringLiteral("voice"));
        auto restoreRefresh1 = pending.takeFirst();

        // Delayed pre-restore preview callback arrives first: must be ignored and keep fetching=true.
        stalePreviewBeforeRefresh(snap, {});
        QCOMPARE(previewReadySpy.count(), 0);
        QVERIFY(!rec->canApply());
        QVERIFY(rec->fetching());
        QCOMPARE(trackBus(4), QStringLiteral(":keep-obs:"));
        QCOMPARE(trackBus(5), QStringLiteral("voice"));

        // Post-restore refresh callback arrives: completes fetch and preserves restored assignments.
        restoreRefresh1(snap, {});
        QVERIFY(!rec->fetching());
        QVERIFY(rec->canPreview());
        QVERIFY(!rec->canApply());
        QCOMPARE(trackBus(4), QStringLiteral(":keep-obs:"));
        QCOMPARE(trackBus(5), QStringLiteral("voice"));

        // 2. Settings restore while preview() is pending (out-of-order completion: post-restore
        //    refresh() completes first, then stale preview() completes second with snapshot or error).
        rec->choose(4, QStringLiteral("game"));
        QCOMPARE(trackBus(4), QStringLiteral("game"));
        rec->preview();
        QCOMPARE(pending.size(), 1);
        auto stalePreviewAfterRefresh = pending.takeFirst();

        app.settings().obsRecordingTracks[scope] = {{QStringLiteral("6"), QStringLiteral("music")}};
        obs.settingsRestored();
        QCOMPARE(pending.size(), 1);
        auto restoreRefresh2 = pending.takeFirst();

        // Post-restore refresh completes first.
        restoreRefresh2(snap, {});
        QVERIFY(!rec->fetching());
        QVERIFY(rec->canPreview());
        QVERIFY(!rec->canApply());
        QCOMPARE(trackBus(4), QStringLiteral(":keep-obs:"));
        QCOMPARE(trackBus(5), QStringLiteral(":keep-obs:"));
        QCOMPARE(trackBus(6), QStringLiteral("music"));
        const QString statusAfterRestore = rec->status();

        // Stale pre-restore preview completes second (both valid snapshot and error): must be a no-op.
        stalePreviewAfterRefresh(snap, {});
        stalePreviewAfterRefresh({}, QStringLiteral("scopeChanged"));
        QCOMPARE(previewReadySpy.count(), 0);
        QVERIFY(!rec->fetching());
        QVERIFY(rec->canPreview());
        QVERIFY(!rec->canApply());
        QCOMPARE(rec->status(), statusAfterRestore);
        QCOMPARE(trackBus(4), QStringLiteral(":keep-obs:"));
        QCOMPARE(trackBus(6), QStringLiteral("music"));

        // 3. Out-of-order completion between background refresh(), choose(), and preview().
        rec->refresh();
        QCOMPARE(pending.size(), 1);
        auto staleRefresh = pending.takeFirst();

        // preview() supersedes an in-flight background refresh().
        rec->preview();
        QCOMPARE(pending.size(), 1);
        auto activePreview = pending.takeFirst();

        activePreview(snap, {});
        QCOMPARE(previewReadySpy.count(), 1);
        QVERIFY(!rec->fetching());
        QVERIFY(rec->canApply());

        // Stale refresh completing out-of-order with an error must not clobber the valid preview.
        staleRefresh({}, QStringLiteral("scopeChanged"));
        QVERIFY(rec->canPreview());
        QVERIFY(rec->canApply());

        // Preview #1 starts, then choose() modifies the draft and Preview #2 starts; Preview #2
        // completes first, then stale Preview #1 completes second with an error.
        rec->preview();
        QCOMPARE(pending.size(), 1);
        auto preview1 = pending.takeFirst();

        rec->choose(3, QStringLiteral("mic"));
        QVERIFY(!rec->fetching());
        QVERIFY(!rec->canApply());

        rec->preview();
        QCOMPARE(pending.size(), 1);
        auto preview2 = pending.takeFirst();

        preview2(snap, {});
        QCOMPARE(previewReadySpy.count(), 2);
        QVERIFY(rec->canApply());
        const QString validPreviewStatus = rec->status();

        preview1({}, QStringLiteral("activeOutput"));
        QVERIFY(rec->canPreview());
        QVERIFY(rec->canApply());
        QCOMPARE(rec->status(), validPreviewStatus);
        QCOMPARE(trackBus(3), QStringLiteral("mic"));
        QCOMPARE(trackBus(6), QStringLiteral("music"));

        // 4. Production signal paths: Engine::micFiltersChanged and obs::Client::event.
        //    micFiltersChanged invalidates an in-flight preview(), but does not cancel an in-flight refresh().
        rec->preview();
        QCOMPARE(pending.size(), 1);
        auto previewBeforeMicFilter = pending.takeFirst();

        auto filters = app.engine()->micFilters();
        filters.enabled = !filters.enabled;
        app.engine()->setMicFilters(filters);
        QVERIFY(!rec->fetching());
        QVERIFY(!rec->canApply());
        previewBeforeMicFilter(snap, {});
        QCOMPARE(previewReadySpy.count(), 2);
        QVERIFY(!rec->canApply());

        rec->refresh();
        QCOMPARE(pending.size(), 1);
        auto refreshDuringMicFilter = pending.takeFirst();
        filters.enabled = !filters.enabled;
        app.engine()->setMicFilters(filters);
        QVERIFY(rec->fetching());
        refreshDuringMicFilter(snap, {});
        QVERIFY(!rec->fetching());
        QVERIFY(rec->canPreview());

        //    obs::Client::event (InputAudioTracksChanged) invalidates an in-flight preview() and schedules refetch.
        rec->preview();
        QCOMPARE(pending.size(), 1);
        auto previewBeforeObsEvent = pending.takeFirst();
        for (auto *sock : std::as_const(sockets)) {
            sock->sendTextMessage(QString::fromUtf8(
                QJsonDocument(QJsonObject{{QStringLiteral("op"), 5},
                                          {QStringLiteral("d"),
                                           QJsonObject{{QStringLiteral("eventType"),
                                                        QStringLiteral("InputAudioTracksChanged")},
                                                       {QStringLiteral("eventData"), QJsonObject{}}}}})
                    .toJson(QJsonDocument::Compact)));
        }
        QTRY_VERIFY(!rec->canPreview());
        QVERIFY(!rec->fetching());
        previewBeforeObsEvent(snap, {});
        QCOMPARE(previewReadySpy.count(), 2);
        QVERIFY(!rec->canPreview());

        //    Wait for the debounced m_refetch (250ms) scheduled by the OBS event to fire and restore snapshot.
        QTRY_COMPARE(pending.size(), 1);
        pending.takeFirst()(snap, {});
        QVERIFY(rec->canPreview());

        // 5. Slow snapshots: repeated preview() / choose() / settingsRestored() calls must not
        //    accumulate unbounded in-flight requests, and the coalesced latest request must win.
        rec->preview();
        QCOMPARE(pending.size(), 1);
        // Calling preview() again while a preview for the current state is already pending is a no-op.
        rec->preview();
        rec->preview();
        QCOMPARE(pending.size(), 1);

        for (const QString &bus : {QStringLiteral("mic"), QStringLiteral("voice"), QStringLiteral("game"),
                                   QStringLiteral("music"), QStringLiteral("desktop")}) {
            rec->choose(3, bus);
            rec->preview();
            QVERIFY2(pending.size() <= 2, qPrintable(QString::number(pending.size())));
        }
        QCOMPARE(pending.size(), 2);

        // Drain the two slow in-flight requests; draining the first dispatches the coalesced latest preview.
        pending.takeFirst()(snap, {});
        QCOMPARE(pending.size(), 2);
        QCOMPARE(previewReadySpy.count(), 2);
        pending.takeFirst()(snap, {});
        QCOMPARE(pending.size(), 1);
        QCOMPARE(previewReadySpy.count(), 2);
        pending.takeFirst()(snap, {});
        QCOMPARE(pending.size(), 0);
        QVERIFY(!rec->fetching());
        QCOMPARE(previewReadySpy.count(), 3);
        QVERIFY(rec->canApply());
        QCOMPARE(trackBus(3), QStringLiteral("desktop"));

        // 6. Delayed preview() callback after disconnect.
        rec->preview();
        QCOMPARE(pending.size(), 1);
        auto previewBeforeDisconnect = pending.takeFirst();

        rec->client()->close();
        QCOMPARE(rec->client()->status(), obs::Client::Status::Disconnected);
        QVERIFY(!rec->fetching());
        QVERIFY(!rec->canPreview());
        QVERIFY(!rec->canApply());

        previewBeforeDisconnect(snap, {});
        QCOMPARE(previewReadySpy.count(), 3);
        QVERIFY(!rec->fetching());
        QVERIFY(!rec->canPreview());
        QVERIFY(!rec->canApply());

        qDeleteAll(sockets);
        sockets.clear();
    }

    void applyAndUndoCompleteAndHandleFailuresAcrossSignals()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        QWebSocketServer server(QStringLiteral("obs-apply-test"), QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QList<QWebSocket *> sockets;
        const auto baseSnap = snapshot();
        QString currentCollection = baseSnap.collection;
        QString currentProfile = baseSnap.profile;
        bool serverStreaming = false;
        bool serverRecording = false;
        QString refuseMethod;
        int nextSceneItemId = 10;

        auto sendEvent = [&](const QString &eventType, const QJsonObject &eventData) {
            for (auto *sock : std::as_const(sockets)) {
                sock->sendTextMessage(QString::fromUtf8(
                    QJsonDocument(QJsonObject{{QStringLiteral("op"), 5},
                                              {QStringLiteral("d"),
                                               QJsonObject{{QStringLiteral("eventType"), eventType},
                                                           {QStringLiteral("eventData"), eventData}}}})
                        .toJson(QJsonDocument::Compact)));
            }
        };

        QObject::connect(&server, &QWebSocketServer::newConnection, &server, [&] {
            while (auto *sock = server.nextPendingConnection()) {
                sockets << sock;
                QObject::connect(sock, &QWebSocket::textMessageReceived, sock, [&, sock](const QString &text) {
                    const auto msg = QJsonDocument::fromJson(text.toUtf8()).object();
                    const int op = msg.value(QLatin1String("op")).toInt();
                    const auto d = msg.value(QLatin1String("d")).toObject();
                    if (op == 1) {
                        sock->sendTextMessage(QString::fromUtf8(
                            QJsonDocument(QJsonObject{{QStringLiteral("op"), 2},
                                                      {QStringLiteral("d"),
                                                       QJsonObject{{QStringLiteral("negotiatedRpcVersion"), 1}}}})
                                .toJson(QJsonDocument::Compact)));
                        return;
                    }
                    if (op != 6)
                        return;
                    const QString type = d.value(QLatin1String("requestType")).toString();
                    const QString reqId = d.value(QLatin1String("requestId")).toString();
                    QJsonObject out;
                    bool ok = (type != refuseMethod);
                    if (ok) {
                        if (type == QLatin1String("GetVersion"))
                            out.insert(QStringLiteral("obsVersion"), QStringLiteral("32.2.2"));
                        else if (type == QLatin1String("GetSceneCollectionList"))
                            out.insert(QStringLiteral("currentSceneCollectionName"), currentCollection);
                        else if (type == QLatin1String("GetProfileList"))
                            out.insert(QStringLiteral("currentProfileName"), currentProfile);
                        else if (type == QLatin1String("GetStreamStatus"))
                            out.insert(QStringLiteral("outputActive"), serverStreaming);
                        else if (type == QLatin1String("GetRecordStatus"))
                            out.insert(QStringLiteral("outputActive"), serverRecording);
                        else if (type == QLatin1String("CreateInput") ||
                                 type == QLatin1String("CreateSceneItem"))
                            out.insert(QStringLiteral("sceneItemId"), ++nextSceneItemId);
                    }
                    sock->sendTextMessage(QString::fromUtf8(
                        QJsonDocument(
                            QJsonObject{{QStringLiteral("op"), 7},
                                        {QStringLiteral("d"),
                                         QJsonObject{{QStringLiteral("requestType"), type},
                                                     {QStringLiteral("requestId"), reqId},
                                                     {QStringLiteral("requestStatus"),
                                                      QJsonObject{{QStringLiteral("result"), ok},
                                                                  {QStringLiteral("code"), ok ? 100 : 600},
                                                                  {QStringLiteral("comment"),
                                                                   ok ? QString()
                                                                      : QStringLiteral("refused")}}},
                                                     {QStringLiteral("responseData"), out}}}})
                            .toJson(QJsonDocument::Compact)));
                });
                sock->sendTextMessage(QString::fromUtf8(
                    QJsonDocument(QJsonObject{{QStringLiteral("op"), 0},
                                              {QStringLiteral("d"),
                                               QJsonObject{{QStringLiteral("obsWebSocketVersion"),
                                                            QStringLiteral("5.5.0")},
                                                           {QStringLiteral("rpcVersion"), 1}}}})
                        .toJson(QJsonDocument::Compact)));
            }
        });

        app::AppController app(nullptr);
        QSignalSpy toastSpy(&app, &app::AppController::toast);
        QList<app::ObsRecording::SnapshotCallback> pending;

        {
            app::Obs obs(&app, &app);
            app::ObsRecording *rec = obs.recordingTracks();
            rec->setSnapshotProvider(
                [&](obs::Client *, app::ObsRecording::SnapshotCallback cb) { pending << std::move(cb); },
                tempDir.path());

            rec->client()->open(server.serverPort(), {});
            QTRY_COMPARE(rec->client()->status(), obs::Client::Status::Connected);

            const QString scope = recordingScopeKey(tempDir.path(), baseSnap.collection);
            app.settings().obsRecordingTracks[scope] = {};
            QCOMPARE(pending.size(), 1);
            pending.takeFirst()(baseSnap, {});
            QVERIFY(rec->canPreview());

            // 1. Preview changes for track 4 -> "game", start a background refresh(), then call
            //    production apply(). Verify apply() enters busy state, invalidates the pending
            //    refresh(), and is NOT aborted by Obs::changed or obs::Client::event.
            rec->choose(4, QStringLiteral("game"));
            rec->preview();
            QCOMPARE(pending.size(), 1);
            pending.takeFirst()(baseSnap, {});
            QVERIFY(rec->canApply());

            rec->refresh();
            QCOMPARE(pending.size(), 1);
            auto staleRefreshBeforeApply = pending.takeFirst();

            rec->apply();
            QVERIFY(obs.busy());
            QVERIFY(!rec->fetching());
            QVERIFY(!rec->canPreview());
            QVERIFY(!rec->canApply());
            QCOMPARE(pending.size(), 1);
            auto applySnapshotCb = pending.takeFirst();

            // Stale refresh arriving while apply() has obs.busy() == true must be ignored.
            staleRefreshBeforeApply({}, QStringLiteral("scopeChanged"));
            // Irrelevant signals while apply()'s snapshot fetch is in flight must not abort apply().
            Q_EMIT obs.changed();
            sendEvent(QStringLiteral("InputMuteStateChanged"),
                      {{QStringLiteral("inputName"), QStringLiteral("Mic/Aux")},
                       {QStringLiteral("inputMuted"), false}});
            QTest::qWait(10);
            QVERIFY(obs.busy());

            applySnapshotCb(baseSnap, {});
            QTRY_VERIFY(!obs.busy());
            QVERIFY(!toastSpy.isEmpty());
            QCOMPARE(toastSpy.last().first().toString(),
                     QStringLiteral("Recording tracks configured. Recording contents and sound have not been tested."));
            QCOMPARE(app.settings().obsRecordingTracks.value(scope).value(QStringLiteral("4")),
                     QStringLiteral("game"));

            // Complete the post-apply refresh() with the resulting OBS state.
            const auto appliedSnap =
                recordingResult(baseSnap, recordingPlan(baseSnap, defaults::scene(), {{4, QStringLiteral("game")}}));
            QCOMPARE(pending.size(), 1);
            pending.takeFirst()(appliedSnap, {});
            QVERIFY(rec->canUndo());

            // Stale refresh arriving after apply() finished must still be ignored.
            staleRefreshBeforeApply({}, QStringLiteral("scopeChanged"));
            QVERIFY(rec->canPreview());
            QVERIFY(rec->canUndo());

            // 2. Failed undo() when output is active, followed by successful undo() while
            //    Obs::changed, Engine::micFiltersChanged, and obs::Client::event fire.
            toastSpy.clear();
            rec->undo();
            QVERIFY(obs.busy());
            QCOMPARE(pending.size(), 1);
            auto failedUndoSnapshotCb = pending.takeFirst();
            auto streamingSnap = appliedSnap;
            streamingSnap.streaming = true;
            failedUndoSnapshotCb(streamingSnap, {});
            QVERIFY(!obs.busy());
            QVERIFY(!toastSpy.isEmpty());
            QCOMPARE(toastSpy.last().first().toString(),
                     QStringLiteral("Recording Undo requires the original OBS profile and scene collection with outputs stopped."));
            QCOMPARE(pending.size(), 1);
            pending.takeFirst()(appliedSnap, {});
            QVERIFY(rec->canUndo());

            toastSpy.clear();
            rec->undo();
            QVERIFY(obs.busy());
            QCOMPARE(pending.size(), 1);
            auto undoSnapshotCb = pending.takeFirst();
            Q_EMIT obs.changed();
            auto filters = app.engine()->micFilters();
            filters.enabled = !filters.enabled;
            app.engine()->setMicFilters(filters);
            sendEvent(QStringLiteral("InputMuteStateChanged"),
                      {{QStringLiteral("inputName"), QStringLiteral("Mic/Aux")},
                       {QStringLiteral("inputMuted"), true}});
            QTest::qWait(10);
            QVERIFY(obs.busy());

            undoSnapshotCb(appliedSnap, {});
            QTRY_VERIFY(!obs.busy());
            QVERIFY(!toastSpy.isEmpty());
            QCOMPARE(toastSpy.last().first().toString(),
                     QStringLiteral("The previous recording assignments were restored."));
            QVERIFY(app.settings().obsRecordingTracks.value(scope).isEmpty());
            QCOMPARE(pending.size(), 1);
            pending.takeFirst()(baseSnap, {});
            QVERIFY(!rec->canUndo());

            // 3. Failed apply() when streaming starts via LiveStatus (Obs::liveChanged), and
            //    failed apply() mid-write triggering rollback runUndo.
            rec->choose(4, QStringLiteral("game"));
            rec->preview();
            QCOMPARE(pending.size(), 1);
            pending.takeFirst()(baseSnap, {});
            QVERIFY(rec->canApply());

            toastSpy.clear();
            rec->apply();
            QVERIFY(obs.busy());
            QCOMPARE(pending.size(), 1);
            auto applyDuringStreamCb = pending.takeFirst();
            serverStreaming = true;
            sendEvent(QStringLiteral("StreamStateChanged"), {{QStringLiteral("outputActive"), true}});
            QTRY_VERIFY(obs.streaming());

            applyDuringStreamCb(baseSnap, {});
            QVERIFY(!obs.busy());
            QVERIFY(!toastSpy.isEmpty());
            QCOMPARE(toastSpy.last().first().toString(),
                     QStringLiteral("Stop streaming and recording before changing recording tracks."));
            serverStreaming = false;
            sendEvent(QStringLiteral("StreamStateChanged"), {{QStringLiteral("outputActive"), false}});
            QTRY_VERIFY(!obs.streaming());
            QCOMPARE(pending.size(), 1);
            pending.takeFirst()(baseSnap, {});

            rec->choose(4, QStringLiteral("game"));
            rec->preview();
            QCOMPARE(pending.size(), 1);
            pending.takeFirst()(baseSnap, {});
            QVERIFY(rec->canApply());

            toastSpy.clear();
            refuseMethod = QStringLiteral("SetProfileParameter");
            rec->apply();
            QVERIFY(obs.busy());
            QCOMPARE(pending.size(), 1);
            pending.takeFirst()(baseSnap, {});
            QTRY_VERIFY(!obs.busy());
            refuseMethod.clear();
            QVERIFY(!toastSpy.isEmpty());
            QCOMPARE(toastSpy.last().first().toString(),
                     QStringLiteral("Recording setup failed. The previous OBS assignments were restored."));
            QVERIFY(app.settings().obsRecordingTracks.value(scope).isEmpty());
            QCOMPARE(pending.size(), 1);
            pending.takeFirst()(baseSnap, {});
            QVERIFY(!rec->canUndo());

            // 4. Queue both a refresh() and a preview() callback, then destroy Obs / ObsRecording
            //    before invoking them to verify QPointer callback lifetime safety.
            rec->refresh();
            rec->preview();
            QCOMPARE(pending.size(), 2);
        }

        // Obs and ObsRecording are now destroyed; invoking the remaining callbacks must be a safe no-op.
        while (!pending.isEmpty())
            pending.takeFirst()(baseSnap, {});

        qDeleteAll(sockets);
    }

    void obsSummaryAndDetailDescribesVodMix_data()
    {
        QTest::addColumn<bool>("screenshot");
        QTest::newRow("offscreen") << false;
        QTest::newRow("screenshot") << true;
    }

    void obsSummaryAndDetailDescribesVodMix()
    {
        QFETCH(bool, screenshot);
        QTemporaryDir tempDir;
        char oldName[16]{};
        QVERIFY(::prctl(PR_GET_NAME, oldName) == 0);
        const auto restoreName = qScopeGuard([&] { ::prctl(PR_SET_NAME, oldName); });
        QVERIFY(::prctl(PR_SET_NAME, "obs") == 0);
        QVERIFY(isObsRunning());
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const QString configDir = tempDir.path() + QStringLiteral("/obs-studio");
        QVERIFY(QDir().mkpath(configDir + QStringLiteral("/basic/scenes")));
        QVERIFY(QDir().mkpath(configDir + QStringLiteral("/plugin_config/obs-websocket")));
        auto write = [](const QString &path, const QByteArray &data) {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(data);
        };
        write(configDir + QStringLiteral("/user.ini"), "[Basic]\nProfile=Test\nSceneCollectionFile=config\n");
        write(configDir + QStringLiteral("/plugin_config/obs-websocket/config.json"),
              QJsonDocument(QJsonObject{{QStringLiteral("server_enabled"), true},
                                       {QStringLiteral("server_port"), server.serverPort()},
                                       {QStringLiteral("auth_required"), false}}).toJson());

        // Complete collection with Rostrum Mic, Rostrum Stream Mix, Rostrum VOD Mix
        QFile fixtureFile(QStringLiteral(ROSTRUM_TEST_DATA "/obs-collection.json"));
        QVERIFY(fixtureFile.open(QIODevice::ReadOnly));
        const QJsonObject original = QJsonDocument::fromJson(fixtureFile.readAll()).object();

        app::AppController app(nullptr);
        const auto plan = makePlan(stateFromCollection(original), factsFrom(*app.pw()), Mode::Offline);
        const QJsonObject configured = applyToCollection(original, plan.actions);
        write(configDir + QStringLiteral("/basic/scenes/config.json"), QJsonDocument(configured).toJson());

        const QByteArray oldConfigHome = qgetenv("XDG_CONFIG_HOME");
        const QByteArray oldPlatform = qgetenv("QT_QPA_PLATFORM");
        const QByteArray oldScreenshot = qgetenv("ROSTRUM_SCREENSHOT");
        const auto restoreEnvironment = qScopeGuard([&] {
            for (const auto &[name, value] :
                 {std::pair{"XDG_CONFIG_HOME", oldConfigHome},
                  std::pair{"QT_QPA_PLATFORM", oldPlatform},
                  std::pair{"ROSTRUM_SCREENSHOT", oldScreenshot}}) {
                if (value.isNull()) {
                    qunsetenv(name);
                } else {
                    qputenv(name, value);
                }
            }
        });
        qputenv("XDG_CONFIG_HOME", tempDir.path().toUtf8());
        if (screenshot) {
            qunsetenv("QT_QPA_PLATFORM");
            qputenv("ROSTRUM_SCREENSHOT", (tempDir.path() + QStringLiteral("/unused.png")).toUtf8());
        } else {
            qputenv("QT_QPA_PLATFORM", "offscreen");
            qunsetenv("ROSTRUM_SCREENSHOT");
        }

        app::Obs obs(&app, &app);
        obs.setActive(true);

        QVERIFY(obs.setUp());
        QCOMPARE(obs.state(), QStringLiteral("closed"));
        QTest::qWait(100);
        QVERIFY(!server.hasPendingConnections());
        QCOMPARE(obs.summary(),
                 QStringLiteral("OBS is set up: it records Rostrum Mic, Rostrum Stream Mix and Rostrum VOD Mix."));

        // When there is an unmuted duplicate/conflict (e.g. Spotify left unmuted):
        Plan doublingPlan = makePlan(stateFromCollection(original), factsFrom(*app.pw()), Mode::Offline);
        for (auto &a : doublingPlan.actions) {
            if (a.input == QLatin1String("Spotify")) {
                a.enabled = false;
            }
        }
        const QJsonObject doublingDoc = applyToCollection(original, doublingPlan.enabledActions());
        write(configDir + QStringLiteral("/basic/scenes/config.json"), QJsonDocument(doublingDoc).toJson());

        obs.refresh();
        QVERIFY(obs.onlyDoubling());
        QCOMPARE(obs.detail(),
                 QStringLiteral("Rostrum Mic, Rostrum Stream Mix and Rostrum VOD Mix are in place, but the sources marked below play on top of them. Fix OBS mutes them."));
    }
};

QTEST_GUILESS_MAIN(TestRecordingTracks)
#include "tst_recordingtracks.moc"
