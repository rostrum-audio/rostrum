#include "engine/Engine.h"
#include "obs/ObsClient.h"
#include "obs/ObsConfig.h"
#include "obs/ObsLive.h"
#include "obs/ObsPlan.h"
#include "obs/ObsStatus.h"
#include "obs/Readiness.h"
#include "obs/RecordingLive.h"
#include "obs/SceneCollection.h"
#include "pw/PwContext.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWebSocket>
#include <QWebSocketServer>

#include <cmath>
#include <limits>

using namespace rostrum::obs;
using rostrum::Scene;

namespace {

const QString kHwMic = QStringLiteral("alsa_input.usb-test_mic.analog-stereo");
const QString kHwOut = QStringLiteral("alsa_output.usb-test_out.analog-stereo");

QJsonObject fixture()
{
    QFile f(QStringLiteral(ROSTRUM_TEST_DATA "/obs-collection.json"));
    if (!f.open(QIODevice::ReadOnly)) {
        qFatal("missing fixture");
    }
    return QJsonDocument::fromJson(f.readAll()).object();
}

Facts facts()
{
    Facts f;
    f.defaultSource = kHwMic;
    f.defaultSink = kHwOut;
    f.serials.insert(81, QStringLiteral("rostrum.mic"));
    f.serials.insert(82, QStringLiteral("rostrum.stream"));
    f.serials.insert(83, QStringLiteral("rostrum.vod"));
    return f;
}

// What a complete live read adds to a scene collection's state: the profile and the streaming
// settings, here not Twitch. The inputs keep the known flags stateFromCollection gave them.
State liveRead(State s)
{
    if (s.collection.isEmpty()) {
        s.collection = QStringLiteral("Test");
    }
    s.profile = QStringLiteral("Test profile");
    s.collectionKnown = s.profileKnown = s.streamServiceKnown = s.vodKnown = true;
    return s;
}

// An input as a complete read returns it.
Input readInput(const QString &name, const char *kind, const QString &device, bool muted = false)
{
    Input in{name, QString::fromLatin1(kind), {{QStringLiteral("device_id"), device}}, muted};
    in.settingsKnown = in.muteKnown = in.tracksKnown = true;
    return in;
}

// Set up, rather than empty because something could not be read.
bool setUp(const Plan &plan)
{
    return !plan.incomplete() && plan.isEmpty();
}

QStringList describe(const Plan &plan)
{
    QStringList out;
    for (const auto &a : plan.actions) {
        switch (a.type) {
        case Action::Type::SetDevice:
            out << QStringLiteral("set %1 %2").arg(a.input, a.device);
            break;
        case Action::Type::Unmute:
            out << QStringLiteral("unmute %1").arg(a.input);
            break;
        case Action::Type::Mute:
            out << QStringLiteral("mute %1").arg(a.input);
            break;
        case Action::Type::CreateInput:
            out << QStringLiteral("create %1 %2 %3 in %4 tracks %5")
                       .arg(a.input, a.kind, a.device, a.scenes.join(QLatin1Char(',')))
                       .arg(a.tracks);
            break;
        case Action::Type::CreateGlobal:
            out << QStringLiteral("global %1 %2 %3 %4 tracks %5").arg(a.channel, a.input, a.kind, a.device).arg(a.tracks);
            break;
        case Action::Type::SetTwitchVodTrack: {
            QStringList values;
            for (const auto &p : a.profile) {
                values << p.key() + QLatin1Char('=') + p.value;
            }
            out << QStringLiteral("twitch-vod %1").arg(values.join(QLatin1Char(' ')));
            break;
        }
        }
    }
    return out;
}

// Just enough of obs-websocket 5 to exercise the client: auth, the requests Rostrum sends,
// and an in-memory set of inputs and scenes.
class FakeObs : public QObject
{
public:
    explicit FakeObs(const QString &password)
        : m_server(QStringLiteral("fake-obs"), QWebSocketServer::NonSecureMode)
        , m_password(password)
    {
        m_server.listen(QHostAddress::LocalHost, 0);
        connect(&m_server, &QWebSocketServer::newConnection, this, [this] {
            QWebSocket *s = m_server.nextPendingConnection();
            m_sockets << s;
            connect(s, &QWebSocket::disconnected, this, [this, s] { m_sockets.removeAll(s); });
            connect(s, &QWebSocket::textMessageReceived, this, [this, s](const QString &t) { onMessage(s, t); });
            send(s, 0, {{QStringLiteral("obsWebSocketVersion"), QStringLiteral("5.7.4")},
                        {QStringLiteral("rpcVersion"), 1},
                        {QStringLiteral("authentication"),
                         QJsonObject{{QStringLiteral("salt"), m_salt}, {QStringLiteral("challenge"), m_challenge}}}});
        });
    }

    quint16 port() const { return m_server.serverPort(); }

    struct In
    {
        QString kind;
        QJsonObject settings;
        bool muted = false;
        QJsonObject tracks;
    };
    QMap<QString, In> inputs;
    QMap<QString, QStringList> scenes;
    QHash<QString, QString> special;
    QStringList ignored, requests;
    std::function<void(const QString &type)> onRequest; // before the request is handled
    bool nested = false;
    QStringList refuse; // request types to fail
    bool streaming = false;
    bool recording = false;
    bool recordPaused = false;
    qint64 outputDuration = 0;
    QString programScene;
    QString streamService;
    // The profile by "Category/Name", as OBS keeps it: a key the profile never set reads as OBS's
    // default, or null. Rostrum may write only keys OBS reads; every write is recorded.
    QHash<QString, QString> profileValues{{QStringLiteral("Output/Mode"), QStringLiteral("Advanced")}};
    const QHash<QString, QString> profileDefaults{
        {QStringLiteral("Output/Mode"), QStringLiteral("Simple")},
        {QStringLiteral("AdvOut/TrackIndex"), QStringLiteral("1")},
        {QStringLiteral("AdvOut/VodTrackIndex"), QStringLiteral("2")},
        {QStringLiteral("SimpleOutput/UseAdvanced"), QStringLiteral("false")}};
    QStringList profileWrites; // "Category/Name=value"
    int recordingTracks = 3;
    QStringList trackNamesRead;
    QString gameTrackName = QStringLiteral("Game");
    QString profile = QStringLiteral("Test profile"), collection = QStringLiteral("Test collection");

    void emitEvent(const QString &type, const QJsonObject &data)
    {
        for (QWebSocket *s : std::as_const(m_sockets)) {
            send(s, 5, {{QStringLiteral("eventType"), type}, {QStringLiteral("eventData"), data}});
        }
    }
    void dropClients()
    {
        for (QWebSocket *s : std::as_const(m_sockets)) {
            s->close();
        }
    }

private:
    void send(QWebSocket *s, int op, const QJsonObject &d)
    {
        s->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("op"), op}, {QStringLiteral("d"), d}}).toJson()));
    }

    void onMessage(QWebSocket *s, const QString &text)
    {
        const QJsonObject msg = QJsonDocument::fromJson(text.toUtf8()).object();
        const QJsonObject d = msg.value(QLatin1String("d")).toObject();
        const int op = msg.value(QLatin1String("op")).toInt();
        if (op == 1) {
            if (d.value(QLatin1String("authentication")).toString() != Client::authResponse(m_password, m_salt, m_challenge)) {
                s->close(QWebSocketProtocol::CloseCode(4009), QStringLiteral("Authentication failed."));
                return;
            }
            send(s, 2, {{QStringLiteral("negotiatedRpcVersion"), 1}});
            return;
        }
        if (op != 6) {
            return;
        }
        const QString type = d.value(QLatin1String("requestType")).toString();
        requests << type;
        if (onRequest)
            onRequest(type);
        if (ignored.contains(type))
            return;
        const QJsonObject rd = d.value(QLatin1String("requestData")).toObject();
        QJsonObject out;
        const bool ok = !refuse.contains(type) && handle(type, rd, out);
        send(s, 7, {{QStringLiteral("requestType"), type},
                    {QStringLiteral("requestId"), d.value(QLatin1String("requestId"))},
                    {QStringLiteral("requestStatus"), QJsonObject{{QStringLiteral("result"), ok},
                                                                  {QStringLiteral("code"), ok ? 100 : 600},
                                                                  {QStringLiteral("comment"), ok ? QString() : QStringLiteral("refused")}}},
                    {QStringLiteral("responseData"), out}});
    }

    bool handle(const QString &type, const QJsonObject &rd, QJsonObject &out)
    {
        const QString name = rd.value(QLatin1String("inputName")).toString();
        if (type == QLatin1String("GetVersion")) {
            out.insert(QStringLiteral("obsVersion"), QStringLiteral("32.2.2"));
        } else if (type == QLatin1String("GetSceneCollectionList")) {
            out.insert(QStringLiteral("currentSceneCollectionName"), collection);
        } else if (type == QLatin1String("GetProfileList")) {
            out.insert(QStringLiteral("currentProfileName"), profile);
        } else if (type == QLatin1String("GetSpecialInputs")) {
            for (const char *ch : {"desktop1", "desktop2", "mic1", "mic2", "mic3", "mic4"}) {
                const QString n = special.value(QLatin1String(ch));
                out.insert(QLatin1String(ch), n.isEmpty() ? QJsonValue() : QJsonValue(n));
            }
        } else if (type == QLatin1String("GetSceneList")) {
            QJsonArray list;
            const QStringList names = scenes.keys();
            for (auto it = names.crbegin(); it != names.crend(); ++it) {
                list.append(QJsonObject{{QStringLiteral("sceneName"), *it}});
            }
            out.insert(QStringLiteral("scenes"), list);
        } else if (type == QLatin1String("GetStreamStatus")) {
            out.insert(QStringLiteral("outputActive"), streaming);
            out.insert(QStringLiteral("outputDuration"), double(streaming ? outputDuration : 0));
        } else if (type == QLatin1String("GetRecordStatus")) {
            out.insert(QStringLiteral("outputActive"), recording);
            out.insert(QStringLiteral("outputPaused"), recordPaused);
            out.insert(QStringLiteral("outputDuration"), double(recording ? outputDuration : 0));
        } else if (type == QLatin1String("GetCurrentProgramScene")) {
            out.insert(QStringLiteral("sceneName"), programScene);
            out.insert(QStringLiteral("currentProgramSceneName"), programScene);
        } else if (type == QLatin1String("GetInputList")) {
            QJsonArray list;
            for (auto it = inputs.cbegin(); it != inputs.cend(); ++it) {
                list.append(QJsonObject{{QStringLiteral("inputName"), it.key()},
                                        {QStringLiteral("inputKind"), it->kind},
                                        {QStringLiteral("unversionedInputKind"), it->kind}});
            }
            out.insert(QStringLiteral("inputs"), list);
        } else if (type == QLatin1String("GetSceneItemList")) {
            QJsonArray items;
            int id = 0;
            const auto scene = rd.value(QLatin1String("sceneName")).toString(programScene);
            for (const auto &name : scenes.value(scene))
                items.append(QJsonObject{{QStringLiteral("sourceName"), name},
                                         {QStringLiteral("sceneItemId"), ++id},
                                         {QStringLiteral("sceneItemEnabled"), true},
                                         {QStringLiteral("isGroup"), nested}});
            out.insert(QStringLiteral("sceneItems"), items);
        } else if (type == QLatin1String("GetInputVolume")) {
            out.insert(QStringLiteral("inputVolumeMul"), 1.0);
        } else if (type == QLatin1String("GetInputSettings")) {
            if (!inputs.contains(name)) {
                return false;
            }
            out.insert(QStringLiteral("inputSettings"), inputs[name].settings);
        } else if (type == QLatin1String("GetInputMute")) {
            if (!inputs.contains(name)) {
                return false;
            }
            out.insert(QStringLiteral("inputMuted"), inputs[name].muted);
        } else if (type == QLatin1String("GetInputAudioTracks")) {
            if (!inputs.contains(name)) {
                return false;
            }
            QJsonObject tracks;
            for (int track = 1; track <= 6; ++track)
                tracks.insert(QString::number(track),
                              inputs[name].tracks.value(QString::number(track)).toBool());
            out.insert(QStringLiteral("inputAudioTracks"), tracks);
        } else if (type == QLatin1String("SetInputSettings")) {
            if (!inputs.contains(name)) {
                return false;
            }
            const QJsonObject s = rd.value(QLatin1String("inputSettings")).toObject();
            for (auto it = s.begin(); it != s.end(); ++it) {
                inputs[name].settings.insert(it.key(), it.value());
            }
        } else if (type == QLatin1String("SetInputMute")) {
            if (!inputs.contains(name)) {
                return false;
            }
            inputs[name].muted = rd.value(QLatin1String("inputMuted")).toBool();
        } else if (type == QLatin1String("SetInputAudioTracks")) {
            if (!inputs.contains(name)) {
                return false;
            }
            inputs[name].tracks = rd.value(QLatin1String("inputAudioTracks")).toObject();
        } else if (type == QLatin1String("CreateInput")) {
            const QString scene = rd.value(QLatin1String("sceneName")).toString();
            if (inputs.contains(name) || !scenes.contains(scene)) {
                return false;
            }
            inputs.insert(name, In{rd.value(QLatin1String("inputKind")).toString(),
                                   rd.value(QLatin1String("inputSettings")).toObject(), false, {}});
            scenes[scene] << name;
            out.insert(QStringLiteral("sceneItemId"), scenes[scene].size());
        } else if (type == QLatin1String("CreateSceneItem")) {
            const QString scene = rd.value(QLatin1String("sceneName")).toString();
            const QString source = rd.value(QLatin1String("sourceName")).toString();
            if (!inputs.contains(source) || !scenes.contains(scene)) {
                return false;
            }
            scenes[scene] << source;
            out.insert(QStringLiteral("sceneItemId"), scenes[scene].size());
        } else if (type == QLatin1String("SetSceneItemEnabled")) {
            // These tests model placement, not rendering. The request is still validated.
            const auto scene = rd.value(QLatin1String("sceneName")).toString();
            const int id = rd.value(QLatin1String("sceneItemId")).toInt();
            if (!scenes.contains(scene) || id < 1 || id > scenes[scene].size())
                return false;
        } else if (type == QLatin1String("RemoveSceneItem")) {
            const auto scene = rd.value(QLatin1String("sceneName")).toString();
            const int id = rd.value(QLatin1String("sceneItemId")).toInt();
            if (!scenes.contains(scene) || id < 1 || id > scenes[scene].size())
                return false;
            scenes[scene].removeAt(id - 1);
        } else if (type == QLatin1String("RemoveInput")) {
            if (!inputs.remove(name)) {
                return false;
            }
            for (auto &items : scenes) {
                items.removeAll(name);
            }
        } else if (type == QLatin1String("GetStreamServiceSettings")) {
            out.insert(QStringLiteral("streamServiceType"), QStringLiteral("rtmp_common"));
            out.insert(QStringLiteral("streamServiceSettings"),
                       QJsonObject{{QStringLiteral("service"), streamService}});
        } else if (type == QLatin1String("GetProfileParameter")) {
            const QString param = rd.value(QLatin1String("parameterName")).toString();
            const QString key =
                rd.value(QLatin1String("parameterCategory")).toString() + QLatin1Char('/') + param;
            if (key == QLatin1String("Output/Mode") || key.contains(QLatin1String("Vod")) ||
                key == QLatin1String("AdvOut/TrackIndex") ||
                key == QLatin1String("SimpleOutput/UseAdvanced")) {
                const auto value = [](const QHash<QString, QString> &values, const QString &key) {
                    return values.contains(key) ? QJsonValue(values.value(key))
                                                : QJsonValue(QJsonValue::Null);
                };
                const QJsonValue fallback = value(profileDefaults, key);
                out.insert(QStringLiteral("parameterValue"),
                           profileValues.contains(key) ? value(profileValues, key) : fallback);
                out.insert(QStringLiteral("defaultParameterValue"), fallback);
            } else if (param == QLatin1String("RecTracks")) {
                out.insert(QStringLiteral("parameterValue"), QString::number(recordingTracks));
            } else if (param.startsWith(QLatin1String("Track")) && param.endsWith(QLatin1String("Name"))) {
                trackNamesRead << param;
                out.insert(QStringLiteral("parameterValue"),
                           param == QLatin1String("Track3Name") ? QJsonValue(gameTrackName)
                                                               : QJsonValue(QJsonValue::Null));
            } else if (param == QLatin1String("RecType")) {
                out.insert(QStringLiteral("parameterValue"), QStringLiteral("Standard"));
            }
        } else if (type == QLatin1String("SetProfileParameter")) {
            const QString key = rd.value(QLatin1String("parameterCategory")).toString() + QLatin1Char('/') +
                                rd.value(QLatin1String("parameterName")).toString();
            const QJsonValue value = rd.value(QLatin1String("parameterValue"));
            static const QSet<QString> writable{
                QStringLiteral("AdvOut/RecTracks"), QStringLiteral("AdvOut/VodTrackEnabled"),
                QStringLiteral("AdvOut/VodTrackIndex"), QStringLiteral("SimpleOutput/VodTrackEnabled")};
            if (!writable.contains(key) || !value.isString()) {
                return false;
            }
            profileWrites << key + QLatin1Char('=') + value.toString();
            if (key == QLatin1String("AdvOut/RecTracks")) {
                recordingTracks = value.toString().toInt();
            } else {
                profileValues.insert(key, value.toString());
            }
        } else {
            return false;
        }
        return true;
    }

    // Before the server: its sockets report disconnected while it is destroyed.
    QList<QWebSocket *> m_sockets;
    QWebSocketServer m_server;
    QString m_password;
    const QString m_salt = QStringLiteral("c2FsdA==");
    const QString m_challenge = QStringLiteral("Y2hhbGxlbmdl");
};

QJsonObject tracks(std::initializer_list<int> on)
{
    QJsonObject o;
    for (int n = 1; n <= 6; ++n) {
        o.insert(QString::number(n), std::find(on.begin(), on.end(), n) != on.end());
    }
    return o;
}

void populate(FakeObs &obs)
{
    obs.inputs.insert(QStringLiteral("Mic/Aux"),
                      {QString::fromLatin1(kPulseInput), {{QStringLiteral("device_id"), kHwMic}}, false, tracks({1, 2})});
    obs.inputs.insert(QStringLiteral("Game Audio"), {QString::fromLatin1(kPulseOutput), {}, false, tracks({1, 3})});
    obs.inputs.insert(QStringLiteral("Discord Audio"),
                      {QString::fromLatin1(kPwApp), {{QStringLiteral("TargetName"), QStringLiteral("Discord")}}, false, tracks({5})});
    obs.special.insert(QStringLiteral("mic1"), QStringLiteral("Mic/Aux"));
    obs.scenes.insert(QStringLiteral("BRB"), {});
    obs.scenes.insert(QStringLiteral("Game"), {QStringLiteral("Game Audio")});
    obs.scenes.insert(QStringLiteral("Overlays"), {QStringLiteral("Discord Audio")});
}

bool connectClient(Client &client, quint16 port, const QString &password)
{
    client.open(port, password);
    return QTest::qWaitFor([&] { return client.status() != Client::Status::Connecting; }, 5000) &&
           client.status() == Client::Status::Connected;
}

// A complete live read, or an empty state.
State fetched(Client &client)
{
    State got;
    bool done = false;
    QString error;
    fetchState(&client, [&](const State &s, const QString &e) {
        got = s;
        error = e;
        done = true;
    });
    if (!QTest::qWaitFor([&] { return done; }, 5000) || !error.isEmpty()) {
        return {};
    }
    return got;
}

} // namespace

Q_DECLARE_METATYPE(rostrum::obs::Capture)
Q_DECLARE_METATYPE(rostrum::obs::CaptureStatus)

class TestObs : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void recordingSetupReadsAppliesAndUndoes_data()
    {
        QTest::addColumn<bool>("filteredMic");
        QTest::newRow("plain mic") << false;
        QTest::newRow("filtered mic") << true;
    }

    void recordingSetupReadsAppliesAndUndoes()
    {
        QFETCH(bool, filteredMic);
        FakeObs server(QStringLiteral("pw"));
        populate(server);
        server.inputs[QStringLiteral("Mic/Aux")].settings[QStringLiteral("device_id")] =
            QLatin1String(kMicDevice);
        server.inputs[QStringLiteral("Mic/Aux")].tracks = tracks({1, 2});
        Client client;
        QVERIFY(connectClient(client, server.port(), QStringLiteral("pw")));
        RecordingSnapshot before;
        bool done = false;
        fetchRecordingSnapshot(&client, [&](const auto &snapshot, const QString &error) {
            QVERIFY2(error.isEmpty(), qPrintable(error));
            before = snapshot;
            done = true;
        });
        QTRY_VERIFY(done);
        QVERIFY(before.known);
        QCOMPARE(server.trackNamesRead.size(), 6);
        QCOMPARE(before.trackNames.value(3), QStringLiteral("Game"));
        QCOMPARE(before.trackNames.value(4), QString());
        for (const auto &request : server.requests)
            QVERIFY(request.startsWith(QLatin1String("Get")));
        const auto plan = recordingPlan(
            before, rostrum::defaults::scene(),
            {{3, QStringLiteral("mic")}, {4, QStringLiteral("game")}, {6, QStringLiteral("music")}},
            filteredMic);
        QVERIFY(plan.problems.isEmpty());
        QList<RecordingChange> journal;
        done = false;
        applyRecordingChanges(
            &client, before, plan.changes,
            [&](const auto &applied) {
                journal = applied;
                return true;
            },
            [&](const auto &, const QString &error) {
                QVERIFY2(error.isEmpty(), qPrintable(error));
                done = true;
            });
        QTRY_VERIFY(done);
        QCOMPARE(server.inputs[QStringLiteral("Rostrum Game (Recording)")].tracks, tracks({4}));
        QCOMPARE(server.inputs[QStringLiteral("Rostrum Music (Recording)")].tracks, tracks({6}));
        QCOMPARE(server.inputs[QStringLiteral("Mic/Aux")].tracks,
                 filteredMic ? tracks({1, 2}) : tracks({1, 2, 3}));
        if (filteredMic) {
            const auto mic = server.inputs.value(QStringLiteral("Rostrum Mic (Recording)"));
            QCOMPARE(mic.settings.value(QStringLiteral("device_id")).toString(),
                     QStringLiteral("rostrum.filtered"));
            QCOMPARE(mic.tracks, tracks({3}));
        }
        QCOMPARE(server.recordingTracks, 47);
        QCOMPARE(server.inputs[QStringLiteral("Discord Audio")].tracks, tracks({5}));
        server.gameTrackName = QStringLiteral("Gameplay");
        RecordingSnapshot after;
        done = false;
        fetchRecordingSnapshot(&client, [&](const auto &snapshot, const QString &error) {
            QVERIFY2(error.isEmpty(), qPrintable(error));
            after = snapshot;
            done = true;
        });
        QTRY_VERIFY(done);
        QCOMPARE(after.trackNames.value(3), QStringLiteral("Gameplay"));
        auto renamed = before;
        renamed.trackNames[3] = QStringLiteral("Gameplay");
        QVERIFY(recordingFingerprint(renamed) != recordingFingerprint(before));
        const auto repeated = recordingPlan(
            after, rostrum::defaults::scene(),
            {{3, QStringLiteral("mic")}, {4, QStringLiteral("game")}, {6, QStringLiteral("music")}},
            filteredMic);
        QVERIFY(repeated.changes.isEmpty());
        RecordingPlan completed;
        completed.changes = recordingChangesFromJson(recordingChangesJson(journal));
        done = false;
        applyRecordingChanges(
            &client, after, recordingUndo(completed).changes, [](const auto &) { return true; },
            [&](const auto &, const QString &error) {
                QVERIFY2(error.isEmpty(), qPrintable(error));
                done = true;
            });
        QTRY_VERIFY(done);
        QVERIFY(!server.inputs.contains(QStringLiteral("Rostrum Game (Recording)")));
        QVERIFY(!server.inputs.contains(QStringLiteral("Rostrum Music (Recording)")));
        QCOMPARE(server.inputs[QStringLiteral("Mic/Aux")].tracks, tracks({1, 2}));
        QVERIFY(!server.inputs.contains(QStringLiteral("Rostrum Mic (Recording)")));
        QCOMPARE(server.recordingTracks, 3);
    }

    void recordingWritesStopIfOutputStarts()
    {
        FakeObs server(QStringLiteral("pw"));
        populate(server);
        Client client;
        QVERIFY(connectClient(client, server.port(), QStringLiteral("pw")));
        RecordingSnapshot scope;
        scope.collection = server.collection;
        scope.profile = server.profile;
        const QList<RecordingChange> changes{{QStringLiteral("SetInputAudioTracks"),
                                              {{QStringLiteral("inputName"), QStringLiteral("Game Audio")},
                                               {QStringLiteral("inputAudioTracks"), tracks({4})}},
                                              {},
                                              {}},
                                             {QStringLiteral("SetInputAudioTracks"),
                                              {{QStringLiteral("inputName"), QStringLiteral("Discord Audio")},
                                               {QStringLiteral("inputAudioTracks"), tracks({5})}},
                                              {},
                                              {}}};
        const auto voiceBefore = server.inputs[QStringLiteral("Discord Audio")].tracks;
        bool done = false;
        applyRecordingChanges(
            &client, scope, changes,
            [&](const auto &) {
                server.streaming = true;
                return true;
            },
            [&](const auto &applied, const QString &error) {
                QCOMPARE(applied.size(), 1);
                QCOMPARE(error, QStringLiteral("activeOutput"));
                done = true;
            });
        QTRY_VERIFY(done);
        QCOMPARE(server.inputs[QStringLiteral("Discord Audio")].tracks, voiceBefore);
    }

    void recordingRefusesUnknownSnapshotAndChangedCollection()
    {
        FakeObs server(QStringLiteral("pw"));
        populate(server);
        server.refuse << QStringLiteral("GetInputAudioTracks");
        Client client;
        QVERIFY(connectClient(client, server.port(), QStringLiteral("pw")));
        bool done = false;
        fetchRecordingSnapshot(&client, [&](const auto &snapshot, const QString &error) {
            QVERIFY(!snapshot.known);
            QVERIFY(!error.isEmpty());
            done = true;
        });
        QTRY_VERIFY(done);
        RecordingSnapshot scope;
        scope.collection = QStringLiteral("Old collection");
        scope.profile = server.profile;
        done = false;
        applyRecordingChanges(
            &client, scope,
            {{QStringLiteral("SetInputMute"),
              {{QStringLiteral("inputName"), QStringLiteral("Mic/Aux")},
               {QStringLiteral("inputMuted"), true}},
              {},
              {}}},
            [](const auto &) { return true; },
            [&](const auto &applied, const QString &error) {
                QVERIFY(applied.isEmpty());
                QCOMPARE(error, QStringLiteral("scopeChanged"));
                done = true;
            });
        QTRY_VERIFY(done);
        QVERIFY(!server.requests.contains(QStringLiteral("SetInputMute")));
    }

    void authMatchesProtocolExample()
    {
        QCOMPARE(Client::authResponse(QStringLiteral("supersecretpassword"),
                                      QStringLiteral("lM1GncleQOaCu9lT1yeUZhFYnqhsLLP1G5lAGo3ixaI="),
                                      QStringLiteral("+IxH4CnCiqpX1rM9scsNynZzbOe4KhDeYcTNS3PDaeY=")),
                 QStringLiteral("1Ct943GAT+6YQUUX47Ia/ncufilbe6+oD6lY+5kaCu4="));
    }

    void classifiesCaptures()
    {
        const Facts f = facts();
        auto cap = [&](const char *kind, QJsonObject settings) {
            return classify(Input{QStringLiteral("x"), QString::fromLatin1(kind), settings}, f);
        };
        QCOMPARE(cap(kPulseInput, {}), Capture::Mic);
        QCOMPARE(cap(kPulseInput, {{QStringLiteral("device_id"), QStringLiteral("rostrum.mic")}}), Capture::RostrumMic);
        // The filtered mic is the voice again, so it doubles with Rostrum Mic like the hardware.
        QCOMPARE(cap(kPulseInput, {{QStringLiteral("device_id"), QStringLiteral("rostrum.filtered")}}), Capture::Mic);
        QCOMPARE(cap(kPulseOutput, {}), Capture::Output);
        QCOMPARE(cap(kPulseOutput, {{QStringLiteral("device_id"), QStringLiteral("rostrum.stream.monitor")}}),
                 Capture::RostrumStream);
        QCOMPARE(cap(kPulseOutput, {{QStringLiteral("device_id"), QStringLiteral("rostrum.game.monitor")}}),
                 Capture::RostrumBus);
        QCOMPARE(cap(kPulseInput, {{QStringLiteral("device_id"), QStringLiteral("rostrum.stream.monitor")}}),
                 Capture::RostrumStream);
        QCOMPARE(cap(kPwInput, {{QStringLiteral("TargetId"), 81}}), Capture::RostrumMic);
        QCOMPARE(cap(kPwOutput, {{QStringLiteral("TargetId"), 82}}), Capture::RostrumStream);
        QCOMPARE(cap(kPwOutput, {{QStringLiteral("TargetId"), 4294967295.0}}), Capture::Output);
        QCOMPARE(cap(kPwInput, {{QStringLiteral("TargetId"), 999}}), Capture::None);
        QCOMPARE(cap(kPwApp, {{QStringLiteral("TargetName"), QStringLiteral("Discord")}}), Capture::App);
        QCOMPARE(cap(kPwApp, {}), Capture::None);
        QCOMPARE(cap("browser_source", {}), Capture::None);
    }

    void readsCollection()
    {
        const State s = stateFromCollection(fixture());
        QCOMPARE(s.scenes, (QStringList{QStringLiteral("Game"), QStringLiteral("Desktop"), QStringLiteral("BRB"),
                                        QStringLiteral("Overlays")}));
        QVERIFY(s.channel(QStringLiteral("mic1")));
        QCOMPARE(s.channel(QStringLiteral("mic1"))->name, QStringLiteral("Mic/Aux"));
        QVERIFY(!s.input(QStringLiteral("Alerts")));
        QVERIFY(!s.input(QStringLiteral("Display Capture")));
        QVERIFY(s.input(QStringLiteral("Browser Audio"))->muted);
        QCOMPARE(s.freeChannel(QStringLiteral("desktop")), QStringLiteral("desktop1"));
        QCOMPARE(s.freeChannel(QStringLiteral("mic")), QStringLiteral("mic2"));
    }

    void livePlanReusesMicAndAddsStreamMixEverywhere()
    {
        const Plan plan = makePlan(liveRead(stateFromCollection(fixture())), facts(), Mode::Live);
        QCOMPARE(describe(plan),
                 (QStringList{
                     QStringLiteral("set Mic/Aux rostrum.mic"),
                     QStringLiteral("create Rostrum Stream Mix pulse_output_capture rostrum.stream.monitor in "
                                    "Game,Desktop,BRB,Overlays tracks 1"),
                     QStringLiteral("create Rostrum VOD Mix pulse_output_capture rostrum.vod.monitor in "
                                    "Game,Desktop,BRB,Overlays tracks 2"),
                     QStringLiteral("mute Game Audio"),
                     QStringLiteral("mute Discord Audio"),
                     QStringLiteral("mute Spotify"),
                 }));
        QCOMPARE(plan.actions.at(3).capture, Capture::Output);
        QCOMPARE(plan.actions.at(4).capture, Capture::App);
    }

    void offlinePlanUsesGlobalDesktopAudio()
    {
        const Plan plan = makePlan(stateFromCollection(fixture()), facts(), Mode::Offline);
        QCOMPARE(describe(plan),
                 (QStringList{
                     QStringLiteral("set Mic/Aux rostrum.mic"),
                     QStringLiteral("global desktop1 Desktop Audio pulse_output_capture rostrum.stream.monitor tracks 1"),
                     QStringLiteral("create Rostrum VOD Mix pulse_output_capture rostrum.vod.monitor in Game,Desktop,BRB,Overlays tracks 2"),
                     QStringLiteral("mute Game Audio"),
                     QStringLiteral("mute Discord Audio"),
                     QStringLiteral("mute Spotify"),
                 }));
    }

    void offlineApplyIsIdempotentAndUndoes()
    {
        const QJsonObject original = fixture();
        const State before = stateFromCollection(original);
        const Plan plan = makePlan(before, facts(), Mode::Offline);
        const QJsonObject after = applyToCollection(original, plan.actions);

        const State s = stateFromCollection(after);
        QCOMPARE(s.channel(QStringLiteral("desktop1"))->settings.value(QLatin1String("device_id")).toString(),
                 QString::fromLatin1(kStreamDevice));
        QCOMPARE(s.channel(QStringLiteral("desktop1"))->tracks, 1u);
        QVERIFY(!s.channel(QStringLiteral("desktop2")));
        QCOMPARE(s.input(QStringLiteral("Rostrum VOD Mix"))->settings.value(QLatin1String("device_id")).toString(),
                 QString::fromLatin1(kVodDevice));
        QCOMPARE(s.input(QStringLiteral("Rostrum VOD Mix"))->tracks, 2u);
        QCOMPARE(s.input(QStringLiteral("Mic/Aux"))->settings.value(QLatin1String("device_id")).toString(),
                 QString::fromLatin1(kMicDevice));
        QCOMPARE(s.input(QStringLiteral("Mic/Aux"))->tracks, 3u);
        QVERIFY(s.input(QStringLiteral("Game Audio"))->muted);
        QVERIFY(setUp(makePlan(s, facts(), Mode::Offline)));
        QVERIFY(setUp(makePlan(liveRead(s), facts(), Mode::Live)));
        // OBS loads a scene item without "visible" hidden, which silences an audio source there,
        // and numbers one without an "id" itself.
        int placed = 0;
        for (const auto &v : after.value(QLatin1String("sources")).toArray()) {
            const QJsonObject source = v.toObject();
            if (source.value(QLatin1String("id")).toString() != QLatin1String("scene")) {
                continue;
            }
            for (const auto &iv :
                 source.value(QLatin1String("settings")).toObject().value(QLatin1String("items")).toArray()) {
                const QJsonObject item = iv.toObject();
                if (item.value(QLatin1String("name")).toString() == QLatin1String("Rostrum VOD Mix")) {
                    ++placed;
                    QCOMPARE(item.value(QLatin1String("visible")), QJsonValue(true));
                    QVERIFY(!item.contains(QLatin1String("id")));
                }
            }
        }
        QCOMPARE(placed, 4);

        Undo undo;
        for (const auto &a : plan.actions) {
            undo.ops << undoFor(a, before);
        }
        QCOMPARE(undoInCollection(after, Undo::fromJson(undo.toJson())), original);
    }

    void untickedConflictsStay()
    {
        Plan plan = makePlan(stateFromCollection(fixture()), facts(), Mode::Offline);
        for (auto &a : plan.actions) {
            if (a.input == QLatin1String("Spotify")) {
                a.enabled = false;
            }
        }
        const State s = stateFromCollection(applyToCollection(fixture(), plan.enabledActions()));
        QVERIFY(!s.input(QStringLiteral("Spotify"))->muted);
        QVERIFY(s.input(QStringLiteral("Discord Audio"))->muted);
    }

    void mutedRostrumInputIsUnmutedNotDuplicated()
    {
        State s = liveRead(stateFromCollection(fixture()));
        s.inputs << readInput(QStringLiteral("Rostrum Stream Mix"), kPulseOutput, QString::fromLatin1(kStreamDevice), true);
        const QStringList d = describe(makePlan(s, facts(), Mode::Live));
        QVERIFY(d.contains(QStringLiteral("unmute Rostrum Stream Mix")));
        QVERIFY(!d.join(QLatin1Char('\n')).contains(QLatin1String("create Rostrum Stream Mix")));
    }

    void secondCopyOfRostrumMicIsMuted()
    {
        State s = liveRead({});
        s.scenes = {QStringLiteral("A")};
        s.inputs << readInput(QStringLiteral("One"), kPulseInput, QString::fromLatin1(kMicDevice))
                 << readInput(QStringLiteral("Two"), kPulseInput, QString::fromLatin1(kMicDevice))
                 << readInput(QStringLiteral("Mix"), kPulseOutput, QString::fromLatin1(kStreamDevice))
                 << readInput(QStringLiteral("Vod"), kPulseOutput, QString::fromLatin1(kVodDevice));
        QCOMPARE(describe(makePlan(s, facts(), Mode::Live)), QStringList{QStringLiteral("mute Two")});
    }

    void unreadAudioInputsBlockThePlan()
    {
        // Rostrum Mic is already in OBS, but one of its fields was not read. Its device would read
        // as "default", so it would look like the hardware mic and be retargeted or doubled; its
        // mute would read as unmuted, so a Mute's undo would unmute it later.
        const State full = liveRead(stateFromCollection(fixture()));
        for (int field = 0; field < 3; ++field) {
            State s = full;
            Input mic = readInput(QStringLiteral("Rostrum Mic"), kPulseInput, QString::fromLatin1(kMicDevice), true);
            if (field == 0) {
                mic.settingsKnown = false;
                mic.settings = {};
            } else if (field == 1) {
                mic.muteKnown = false;
                mic.muted = false;
            } else {
                mic.tracksKnown = false;
            }
            s.inputs << mic;
            const Plan plan = makePlan(s, facts(), Mode::Live);
            QVERIFY2(plan.incomplete(), qPrintable(QString::number(field)));
            QCOMPARE(plan.unreadInputs, QStringList{QStringLiteral("Rostrum Mic")});
            QVERIFY(!plan.streamSettingsUnread);
            QCOMPARE(describe(plan), QStringList{});
            QVERIFY(!setUp(plan));
        }
        // Sources that record no audio don't matter.
        State s = full;
        Input browser;
        browser.name = QStringLiteral("Alerts");
        browser.kind = QStringLiteral("browser_source");
        s.inputs << browser;
        QVERIFY(!makePlan(s, facts(), Mode::Live).incomplete());
    }

    void unreadStreamSettingsBlockLivePlans()
    {
        State s = stateFromCollection(fixture());
        s.profile = QStringLiteral("Test profile"); // read; only the streaming settings were not
        s.profileKnown = true;
        const Plan unread = makePlan(s, facts(), Mode::Live);
        QVERIFY(unread.streamSettingsUnread);
        QVERIFY(unread.unreadInputs.isEmpty());
        QVERIFY(unread.isEmpty());
        // Offline Rostrum never reads them or sets the VOD track.
        QVERIFY(!makePlan(s, facts(), Mode::Offline).incomplete());

        s.streamServiceKnown = true;
        s.streamService = QStringLiteral("Twitch");
        QVERIFY(makePlan(s, facts(), Mode::Live).streamSettingsUnread); // VOD track unknown
        s.vodKnown = true;
        QVERIFY(!makePlan(s, facts(), Mode::Live).incomplete());
        s.streamService = QStringLiteral("YouTube - RTMPS");
        s.vodKnown = false; // irrelevant away from Twitch
        QVERIFY(!makePlan(s, facts(), Mode::Live).incomplete());
    }

    void offlineSourcesWithMissingFieldsAreUnread()
    {
        for (const auto &in : stateFromCollection(fixture()).inputs) {
            QVERIFY2(in.settingsKnown && in.muteKnown && in.tracksKnown, qPrintable(in.name));
        }
        for (const char *key : {"settings", "muted", "mixers"}) {
            QJsonObject doc = fixture();
            QJsonArray sources = doc.value(QLatin1String("sources")).toArray();
            for (qsizetype n = 0; n < sources.size(); ++n) {
                QJsonObject o = sources.at(n).toObject();
                if (o.value(QLatin1String("name")).toString() == QLatin1String("Game Audio")) {
                    o.insert(QLatin1String(key), QStringLiteral("unexpected"));
                    sources.replace(n, o);
                }
            }
            doc.insert(QStringLiteral("sources"), sources);
            const Plan plan = makePlan(stateFromCollection(doc), facts(), Mode::Offline);
            QCOMPARE(plan.unreadInputs, QStringList{QStringLiteral("Game Audio")});
            QVERIFY2(plan.isEmpty(), key);

            QJsonObject global = fixture();
            QJsonObject mic = global.value(QLatin1String("AuxAudioDevice1")).toObject();
            mic.remove(QLatin1String(key));
            global.insert(QStringLiteral("AuxAudioDevice1"), mic);
            QCOMPARE(makePlan(stateFromCollection(global), facts(), Mode::Offline).unreadInputs,
                     QStringList{QStringLiteral("Mic/Aux")});
        }
    }

    void findsConfigAndCollection()
    {
        QTemporaryDir home;
        const QString dir = home.path() + QStringLiteral("/.var/app/com.obsproject.Studio/config/obs-studio");
        QVERIFY(QDir().mkpath(dir + QStringLiteral("/basic/scenes")));
        QVERIFY(QDir().mkpath(dir + QStringLiteral("/plugin_config/obs-websocket")));
        auto write = [](const QString &path, const QByteArray &data) {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(data);
        };
        write(dir + QStringLiteral("/user.ini"), "[General]\nFirstRun=true\n\n[Basic]\nProfile=Me\nSceneCollectionFile=config\n");
        write(dir + QStringLiteral("/basic/scenes/config.json"), "{}");
        write(dir + QStringLiteral("/plugin_config/obs-websocket/config.json"),
              R"({"server_enabled":true,"server_port":4466,"auth_required":true,"server_password":"pw"})");

        const QList<Install> installs = findInstalls(home.path());
        QCOMPARE(installs.size(), 1);
        QCOMPARE(installs.first().flavor, QStringLiteral("flatpak"));
        QCOMPARE(sceneCollectionFile(dir), dir + QStringLiteral("/basic/scenes/config.json"));
        const WebSocketConfig ws = readWebSocketConfig(dir);
        QVERIFY(ws.found && ws.enabled && ws.authRequired);
        QCOMPARE(ws.port, quint16(4466));
        QCOMPARE(ws.password, QStringLiteral("pw"));

        QFile::remove(dir + QStringLiteral("/plugin_config/obs-websocket/config.json"));
        write(dir + QStringLiteral("/global.ini"), "[OBSWebSocket]\nServerEnabled=false\nServerPort=4455\nAuthRequired=true\nServerPassword=old\n");
        const WebSocketConfig old = readWebSocketConfig(dir);
        QVERIFY(old.found && !old.enabled);
        QCOMPARE(old.password, QStringLiteral("old"));
    }

    void wrongPasswordIsReported()
    {
        FakeObs obs(QStringLiteral("right"));
        Client client;
        QVERIFY(!connectClient(client, obs.port(), QStringLiteral("wrong")));
        QTRY_COMPARE(client.status(), Client::Status::AuthFailed);
    }

    void liveSetupAndUndo()
    {
        FakeObs obs(QStringLiteral("pw"));
        populate(obs);
        Client client;
        QVERIFY(connectClient(client, obs.port(), QStringLiteral("pw")));

        State before;
        bool fetched = false;
        fetchState(&client, [&](const State &s, const QString &error) {
            QVERIFY(error.isEmpty());
            before = s;
            fetched = true;
        });
        QTRY_VERIFY(fetched);
        QCOMPARE(before.channel(QStringLiteral("mic1"))->name, QStringLiteral("Mic/Aux"));
        QCOMPARE(before.input(QStringLiteral("Game Audio"))->tracks, 0b101u);
        QCOMPARE(before.scenes, (QStringList{QStringLiteral("BRB"), QStringLiteral("Game"), QStringLiteral("Overlays")}));

        const Plan plan = makePlan(before, facts(), Mode::Live);
        QVERIFY(!plan.incomplete());
        Undo undo;
        bool applied = false;
        applyPlan(&client, plan.enabledActions(), before, [&](const QList<UndoOp> &ops, const QString &error) {
            QVERIFY2(error.isEmpty(), qPrintable(error));
            undo.ops = ops;
            applied = true;
        });
        QTRY_VERIFY(applied);
        QCOMPARE(obs.inputs[QStringLiteral("Mic/Aux")].settings.value(QLatin1String("device_id")).toString(),
                 QString::fromLatin1(kMicDevice));
        QVERIFY(obs.inputs.contains(QStringLiteral("Rostrum Stream Mix")));
        QCOMPARE(obs.inputs[QStringLiteral("Rostrum Stream Mix")].tracks, tracks({1}));
        QVERIFY(obs.inputs.contains(QStringLiteral("Rostrum VOD Mix")));
        QCOMPARE(obs.inputs[QStringLiteral("Rostrum VOD Mix")].tracks, tracks({2}));
        for (const QString &scene : obs.scenes.keys()) {
            QVERIFY2(obs.scenes[scene].contains(QStringLiteral("Rostrum Stream Mix")), qPrintable(scene));
            QVERIFY2(obs.scenes[scene].contains(QStringLiteral("Rostrum VOD Mix")), qPrintable(scene));
        }
        QVERIFY(obs.inputs[QStringLiteral("Game Audio")].muted);
        QVERIFY(obs.inputs[QStringLiteral("Discord Audio")].muted);

        bool refetched = false;
        fetchState(&client, [&](const State &s, const QString &) {
            QVERIFY(setUp(makePlan(s, facts(), Mode::Live)));
            refetched = true;
        });
        QTRY_VERIFY(refetched);

        bool undone = false;
        UndoResult result;
        applyUndo(&client, undo, {QString(), obs.collection, obs.profile}, [&](const UndoResult &r) {
            result = r;
            undone = true;
        });
        QTRY_VERIFY(undone);
        QCOMPARE(result.failed, 0);
        QVERIFY(result.stopped.isEmpty());
        QCOMPARE(result.settled.size(), undo.ops.size());
        QCOMPARE(obs.inputs[QStringLiteral("Mic/Aux")].settings.value(QLatin1String("device_id")).toString(), kHwMic);
        QVERIFY(!obs.inputs.contains(QStringLiteral("Rostrum Stream Mix")));
        QVERIFY(!obs.inputs.contains(QStringLiteral("Rostrum VOD Mix")));
        QVERIFY(!obs.inputs[QStringLiteral("Game Audio")].muted);
        QVERIFY(!obs.inputs[QStringLiteral("Discord Audio")].muted);
    }

    void refusedStepStopsAndKeepsPartialUndo()
    {
        FakeObs obs(QStringLiteral("pw"));
        populate(obs);
        obs.refuse << QStringLiteral("CreateInput");
        Client client;
        QVERIFY(connectClient(client, obs.port(), QStringLiteral("pw")));
        State before;
        bool fetched = false;
        fetchState(&client, [&](const State &s, const QString &) {
            before = s;
            fetched = true;
        });
        QTRY_VERIFY(fetched);
        bool done = false;
        applyPlan(&client, makePlan(before, facts(), Mode::Live).actions, before,
                  [&](const QList<UndoOp> &ops, const QString &error) {
                      QVERIFY(!error.isEmpty());
                      QCOMPARE(ops.size(), 1); // only the Mic/Aux change happened
                      QCOMPARE(ops.first().type, UndoOp::Type::RestoreDevice);
                      done = true;
                  });
        QTRY_VERIFY(done);
        QVERIFY(!obs.inputs[QStringLiteral("Game Audio")].muted);
    }

    void liveStatusFollowsOutputsAndScenes()
    {
        FakeObs obs(QStringLiteral("pw"));
        populate(obs);
        obs.streaming = true;
        obs.outputDuration = 65000;
        obs.programScene = QStringLiteral("Game");
        Client client;
        LiveStatus live(&client);
        QSignalSpy started(&live, &LiveStatus::streamStarted);
        QSignalSpy switched(&live, &LiveStatus::programSceneChanged);
        QVERIFY(connectClient(client, obs.port(), QStringLiteral("pw")));
        QTRY_VERIFY(live.known());

        // Already live at connect: reported once, with the start worked out from OBS's duration.
        QVERIFY(live.streaming());
        QCOMPARE(started.count(), 1);
        QVERIFY(qAbs(QDateTime::currentMSecsSinceEpoch() - 65000 - live.streamStartMs()) < 5000);
        QVERIFY(!live.recording());
        QCOMPARE(live.programScene(), QStringLiteral("Game"));
        QCOMPARE(live.scenes(),
                 (QStringList{QStringLiteral("BRB"), QStringLiteral("Game"), QStringLiteral("Overlays")}));
        QCOMPARE(switched.count(), 0);

        obs.emitEvent(QStringLiteral("StreamStateChanged"),
                      {{QStringLiteral("outputActive"), false},
                       {QStringLiteral("outputState"), QStringLiteral("OBS_WEBSOCKET_OUTPUT_STOPPED")}});
        QTRY_VERIFY(!live.streaming());
        QCOMPARE(live.streamStartMs(), 0);
        obs.emitEvent(QStringLiteral("StreamStateChanged"),
                      {{QStringLiteral("outputActive"), true},
                       {QStringLiteral("outputState"), QStringLiteral("OBS_WEBSOCKET_OUTPUT_STARTED")}});
        QTRY_COMPARE(started.count(), 2);
        QVERIFY(live.streaming());

        obs.recording = true;
        obs.outputDuration = 0;
        obs.emitEvent(QStringLiteral("RecordStateChanged"),
                      {{QStringLiteral("outputActive"), true},
                       {QStringLiteral("outputState"), QStringLiteral("OBS_WEBSOCKET_OUTPUT_STARTED")}});
        QTRY_VERIFY(live.recording());
        obs.recordPaused = true;
        obs.outputDuration = 42000;
        obs.emitEvent(QStringLiteral("RecordStateChanged"),
                      {{QStringLiteral("outputActive"), true},
                       {QStringLiteral("outputState"), QStringLiteral("OBS_WEBSOCKET_OUTPUT_PAUSED")}});
        QTRY_COMPARE(live.recordPausedElapsedMs(), 42000);
        QVERIFY(live.recordPaused());

        obs.emitEvent(QStringLiteral("CurrentProgramSceneChanged"),
                      {{QStringLiteral("sceneName"), QStringLiteral("BRB")}});
        QTRY_COMPARE(switched.count(), 1);
        QCOMPARE(switched.first().first().toString(), QStringLiteral("BRB"));
        QCOMPARE(live.programScene(), QStringLiteral("BRB"));

        obs.scenes.insert(QStringLiteral("Just Chatting"), {});
        obs.emitEvent(QStringLiteral("SceneCreated"),
                      {{QStringLiteral("sceneName"), QStringLiteral("Just Chatting")}});
        QTRY_VERIFY(live.scenes().contains(QStringLiteral("Just Chatting")));

        // OBS quitting must never leave a stale LIVE badge behind.
        obs.dropClients();
        QTRY_VERIFY(!live.known());
        QVERIFY(!live.streaming());
        QVERIFY(!live.recording());
        QVERIFY(live.programScene().isEmpty());
    }

    void readinessSnapshotIsReadOnlyAndRetainsFailures()
    {
        FakeObs obs(QStringLiteral("pw"));
        populate(obs);
        obs.programScene = QStringLiteral("Game");
        Client client;
        QVERIFY(connectClient(client, obs.port(), QStringLiteral("pw")));
        State snapshot;
        bool done = false;
        auto fetch = [&] {
            done = false;
            fetchReadinessState(&client, [&](const State &s, const QString &error) {
                QVERIFY2(error.isEmpty(), qPrintable(error));
                snapshot = s;
                done = true;
            });
        };
        fetch();
        QTRY_VERIFY(done);
        QVERIFY(snapshot.scopeKnown);
        QVERIFY(snapshot.input(QStringLiteral("Mic/Aux"))->gainKnown);
        QVERIFY(snapshot.input(QStringLiteral("Mic/Aux"))->tracksKnown);
        for (const auto &type : obs.requests)
            QVERIFY(type.startsWith(QLatin1String("Get")));
        obs.refuse << QStringLiteral("GetInputMute");
        fetch();
        QTRY_VERIFY(done);
        QVERIFY(!snapshot.input(QStringLiteral("Mic/Aux"))->muteKnown);
        obs.refuse.clear();
        obs.nested = true;
        fetch();
        QTRY_VERIFY(done);
        QVERIFY(!snapshot.scopeKnown);
        obs.nested = false;
        obs.ignored << QStringLiteral("GetInputSettings");
        fetch();
        QTRY_VERIFY_WITH_TIMEOUT(done, 7000);
        QVERIFY(!snapshot.input(QStringLiteral("Mic/Aux"))->settingsKnown);
    }

    void readinessDisconnectCannotProduceKnownScope()
    {
        FakeObs obs(QStringLiteral("pw"));
        populate(obs);
        Client client;
        QVERIFY(connectClient(client, obs.port(), QStringLiteral("pw")));
        bool done = false;
        State snapshot;
        QString error;
        fetchReadinessState(&client, [&](const State &state, const QString &message) {
            snapshot = state;
            error = message;
            done = true;
        });
        client.close();
        QTRY_VERIFY(done);
        QVERIFY(!snapshot.scopeKnown);
        QVERIFY(!error.isEmpty());
    }

    void refusedReadsMakeTheLivePlanIncomplete()
    {
        FakeObs obs(QStringLiteral("pw"));
        populate(obs);
        Client client;
        QVERIFY(connectClient(client, obs.port(), QStringLiteral("pw")));
        auto plan = [&] {
            Plan out;
            bool done = false;
            fetchState(&client, [&](const State &s, const QString &error) {
                QVERIFY2(error.isEmpty(), qPrintable(error)); // OBS is still there
                out = makePlan(s, facts(), Mode::Live);
                done = true;
            });
            [&] { QTRY_VERIFY(done); }();
            return out;
        };
        for (const QString &type : {QStringLiteral("GetInputSettings"), QStringLiteral("GetInputMute"),
                                    QStringLiteral("GetInputAudioTracks")}) {
            obs.refuse = {type};
            const Plan p = plan();
            QCOMPARE(QSet<QString>(p.unreadInputs.cbegin(), p.unreadInputs.cend()),
                     (QSet<QString>{QStringLiteral("Mic/Aux"), QStringLiteral("Game Audio"),
                                    QStringLiteral("Discord Audio")}));
            QVERIFY2(p.isEmpty(), qPrintable(type));
        }
        obs.refuse = {QStringLiteral("GetStreamServiceSettings")};
        QVERIFY(plan().streamSettingsUnread);
        obs.streamService = QStringLiteral("Twitch");
        obs.refuse = {QStringLiteral("GetProfileParameter")};
        QVERIFY(plan().streamSettingsUnread);
        obs.refuse.clear();
        const Plan p = plan();
        QVERIFY(!p.incomplete());
        QVERIFY(!p.isEmpty());
    }

    void disconnectWhileReadingIsAnError()
    {
        for (const bool obsQuits : {false, true}) {
            FakeObs obs(QStringLiteral("pw"));
            populate(obs);
            obs.ignored << QStringLiteral("GetInputMute");
            Client client;
            QVERIFY(connectClient(client, obs.port(), QStringLiteral("pw")));
            int calls = 0;
            QString error;
            State got;
            fetchState(&client, [&](const State &s, const QString &message) {
                ++calls;
                got = s;
                error = message;
            });
            QTRY_VERIFY(obs.requests.contains(QStringLiteral("GetInputMute")));
            QTRY_VERIFY(obs.requests.contains(QStringLiteral("GetInputAudioTracks")));
            if (obsQuits) {
                obs.dropClients();
            } else {
                client.close();
            }
            QTRY_COMPARE(calls, 1);
            QVERIFY2(!error.isEmpty(), obsQuits ? "OBS quit" : "client closed");
            QVERIFY(got.inputs.isEmpty());
            QTest::qWait(200);
            QCOMPARE(calls, 1);
        }
    }

    void effectiveMuteWarnings()
    {
        rostrum::pw::PwContext pw; // Never started: no live audio connection.
        rostrum::engine::Engine engine(&pw);
        engine.setScene(rostrum::defaults::scene(), false);
        engine.setPanic(true);
        QVERIFY(engine.effectiveMicMuted());
        QVERIFY(engine.effectiveStreamMuted());
        const EffectiveMutes mutes{engine.effectiveMicMuted(), engine.effectiveStreamMuted()};
        const auto problems = goLiveProblems(engine.scene(), {}, nullptr, &mutes);
        QVERIFY(problems.contains(GoLiveProblem::MicMuted));
        QVERIFY(problems.contains(GoLiveProblem::StreamMixSilent));
        engine.setPanic(false);
        engine.setPushToMute(true);
        const EffectiveMutes heldMute{engine.effectiveMicMuted(), engine.effectiveStreamMuted()};
        QVERIFY(goLiveProblems(engine.scene(), {}, nullptr, &heldMute).contains(GoLiveProblem::MicMuted));
        engine.setPushToMute(false);
        engine.setMicMuted(true);
        engine.setPushToTalk(true);
        const EffectiveMutes heldTalk{engine.effectiveMicMuted(), engine.effectiveStreamMuted()};
        QVERIFY(!goLiveProblems(engine.scene(), {}, nullptr, &heldTalk).contains(GoLiveProblem::MicMuted));
    }

    void vodCaptureStatus_data()
    {
        QTest::addColumn<quint32>("trackMask");
        QTest::addColumn<bool>("muted");
        QTest::addColumn<double>("gain");
        QTest::addColumn<VodCaptureStatus>("expected");
        QTest::newRow("VOD track 2 only") << 2u << false << 1.0 << VodCaptureStatus::Configured;
        QTest::newRow("wrong track") << 1u << false << 1.0 << VodCaptureStatus::WrongTracks;
        QTest::newRow("also on stream") << 3u << false << 1.0 << VodCaptureStatus::WrongTracks;
        QTest::newRow("no tracks") << 0u << false << 1.0 << VodCaptureStatus::WrongTracks;
        QTest::newRow("muted VOD") << 2u << true << 1.0 << VodCaptureStatus::Muted;
        QTest::newRow("zero-volume VOD") << 2u << false << 0.0 << VodCaptureStatus::Silent;
        QTest::newRow("simultaneous mute and silent gain") << 2u << true << 0.0 << VodCaptureStatus::Silent;
    }

    void vodCaptureStatus()
    {
        QFETCH(quint32, trackMask);
        QFETCH(bool, muted);
        QFETCH(double, gain);
        QFETCH(VodCaptureStatus, expected);
        Input input;
        input.name = QStringLiteral("Rostrum VOD Mix");
        input.tracks = trackMask;
        input.muted = muted;
        input.gain = gain;
        input.tracksKnown = input.muteKnown = input.gainKnown = true;
        QCOMPARE(rostrum::obs::vodCaptureStatus(&input), expected);
        QCOMPARE(rostrum::obs::vodCaptureStatus(nullptr), VodCaptureStatus::Unknown);
        input.tracksKnown = false;
        QCOMPARE(rostrum::obs::vodCaptureStatus(&input), VodCaptureStatus::Unknown);
        input.tracksKnown = true;
        input.muteKnown = false;
        QCOMPARE(rostrum::obs::vodCaptureStatus(&input), VodCaptureStatus::Unknown);
        input.muteKnown = true;
        input.gainKnown = false;
        // The recording panel snapshot reads tracks and mute, not volume.
        QCOMPARE(rostrum::obs::vodCaptureStatus(&input),
                 expected == VodCaptureStatus::Silent ? (muted ? VodCaptureStatus::Muted : VodCaptureStatus::Configured) : expected);
    }

    void captureStatus_data()
    {
        QTest::addColumn<Capture>("capture");
        QTest::addColumn<quint32>("trackMask");
        QTest::addColumn<bool>("muted");
        QTest::addColumn<double>("gain");
        QTest::addColumn<CaptureStatus>("expected");

        // RostrumMic: requires tracks 1 and 2 (track 3u). Tracks 3-6 allowed.
        QTest::newRow("Mic: valid tracks 1 and 2") << Capture::RostrumMic << 3u << false << 1.0 << CaptureStatus::Configured;
        QTest::newRow("Mic: valid with recording tracks") << Capture::RostrumMic << 7u << false << 1.0 << CaptureStatus::Configured;
        QTest::newRow("Mic: missing track 1") << Capture::RostrumMic << 2u << false << 1.0 << CaptureStatus::WrongTracks;
        QTest::newRow("Mic: missing track 2") << Capture::RostrumMic << 1u << false << 1.0 << CaptureStatus::WrongTracks;
        QTest::newRow("Mic: no tracks") << Capture::RostrumMic << 0u << false << 1.0 << CaptureStatus::WrongTracks;
        QTest::newRow("Mic: muted") << Capture::RostrumMic << 3u << true << 1.0 << CaptureStatus::Muted;
        QTest::newRow("Mic: silent gain") << Capture::RostrumMic << 3u << false << 0.0 << CaptureStatus::Silent;
        QTest::newRow("Mic: simultaneous mute and wrong tracks") << Capture::RostrumMic << 1u << true << 1.0 << CaptureStatus::WrongTracks;
        QTest::newRow("Mic: simultaneous mute and no tracks") << Capture::RostrumMic << 0u << true << 1.0 << CaptureStatus::WrongTracks;
        QTest::newRow("Mic: simultaneous mute and silent gain") << Capture::RostrumMic << 3u << true << 0.0 << CaptureStatus::Silent;

        // RostrumStream: requires track 1, forbids track 2.
        QTest::newRow("Stream: valid track 1") << Capture::RostrumStream << 1u << false << 1.0 << CaptureStatus::Configured;
        QTest::newRow("Stream: conflict on VOD track 2") << Capture::RostrumStream << 3u << false << 1.0 << CaptureStatus::WrongTracks;
        QTest::newRow("Stream: missing track 1") << Capture::RostrumStream << 2u << false << 1.0 << CaptureStatus::WrongTracks;
        QTest::newRow("Stream: no tracks") << Capture::RostrumStream << 0u << false << 1.0 << CaptureStatus::WrongTracks;
        QTest::newRow("Stream: muted") << Capture::RostrumStream << 1u << true << 1.0 << CaptureStatus::Muted;
        QTest::newRow("Stream: silent gain") << Capture::RostrumStream << 1u << false << 0.0 << CaptureStatus::Silent;
        QTest::newRow("Stream: negative gain") << Capture::RostrumStream << 1u << false << -0.5 << CaptureStatus::Silent;
        QTest::newRow("Stream: simultaneous mute and track 2 conflict") << Capture::RostrumStream << 3u << true << 1.0 << CaptureStatus::WrongTracks;
        QTest::newRow("Stream: simultaneous mute and missing track 1") << Capture::RostrumStream << 0u << true << 1.0 << CaptureStatus::WrongTracks;
        QTest::newRow("Stream: simultaneous mute and silent gain") << Capture::RostrumStream << 1u << true << 0.0 << CaptureStatus::Silent;

        // RostrumVod: requires track 2 only.
        QTest::newRow("VOD: valid track 2 only") << Capture::RostrumVod << 2u << false << 1.0 << CaptureStatus::Configured;
        QTest::newRow("VOD: wrong track (track 1)") << Capture::RostrumVod << 1u << false << 1.0 << CaptureStatus::WrongTracks;
        QTest::newRow("VOD: conflict on stream track 1") << Capture::RostrumVod << 3u << false << 1.0 << CaptureStatus::WrongTracks;
        QTest::newRow("VOD: no tracks") << Capture::RostrumVod << 0u << false << 1.0 << CaptureStatus::WrongTracks;
        QTest::newRow("VOD: muted") << Capture::RostrumVod << 2u << true << 1.0 << CaptureStatus::Muted;
        QTest::newRow("VOD: zero-volume") << Capture::RostrumVod << 2u << false << 0.0 << CaptureStatus::Silent;
        QTest::newRow("VOD: simultaneous mute and track 1 conflict") << Capture::RostrumVod << 1u << true << 1.0 << CaptureStatus::WrongTracks;
        QTest::newRow("VOD: simultaneous mute and stream conflict") << Capture::RostrumVod << 3u << true << 1.0 << CaptureStatus::WrongTracks;
        QTest::newRow("VOD: simultaneous mute and no tracks") << Capture::RostrumVod << 0u << true << 1.0 << CaptureStatus::WrongTracks;
        QTest::newRow("VOD: simultaneous mute and silent gain") << Capture::RostrumVod << 2u << true << 0.0 << CaptureStatus::Silent;
    }

    void captureStatus()
    {
        QFETCH(Capture, capture);
        QFETCH(quint32, trackMask);
        QFETCH(bool, muted);
        QFETCH(double, gain);
        QFETCH(CaptureStatus, expected);
        Input input;
        input.name = QStringLiteral("ObsCapture");
        input.tracks = trackMask;
        input.muted = muted;
        input.gain = gain;
        input.tracksKnown = input.muteKnown = input.gainKnown = true;
        QCOMPARE(rostrum::obs::captureStatus(capture, &input), expected);
        QCOMPARE(rostrum::obs::captureStatus(capture, nullptr), CaptureStatus::Unknown);
        input.tracksKnown = false;
        QCOMPARE(rostrum::obs::captureStatus(capture, &input), CaptureStatus::Unknown);
        input.tracksKnown = true;
        input.muteKnown = false;
        QCOMPARE(rostrum::obs::captureStatus(capture, &input), CaptureStatus::Unknown);
        input.muteKnown = true;
        input.gain = std::numeric_limits<double>::quiet_NaN();
        QCOMPARE(rostrum::obs::captureStatus(capture, &input), CaptureStatus::Unknown);
        input.gain = std::numeric_limits<double>::infinity();
        QCOMPARE(rostrum::obs::captureStatus(capture, &input), CaptureStatus::Unknown);
        input.gain = gain;
        input.gainKnown = false;
        QCOMPARE(rostrum::obs::captureStatus(capture, &input),
                 expected == CaptureStatus::Silent ? (muted ? CaptureStatus::Muted : CaptureStatus::Configured) : expected);
    }

    void rostrumCaptureChecks()
    {
        const QList<Recording> recordings{
            {QStringLiteral("Mic/Aux"), QStringLiteral("Rostrum Mic"), Capture::RostrumMic},
            {QStringLiteral("Desktop Audio"), QStringLiteral("Rostrum Stream Mix"), Capture::RostrumStream},
            {QStringLiteral("Rostrum VOD Mix"), QStringLiteral("Rostrum VOD Mix"), Capture::RostrumVod}};

        // 1. Valid configuration
        State state;
        Input mic{QStringLiteral("Mic/Aux")};
        mic.tracks = 3u;
        mic.muted = false;
        mic.gain = 1.0;
        mic.tracksKnown = mic.muteKnown = mic.gainKnown = true;

        Input stream{QStringLiteral("Desktop Audio")};
        stream.tracks = 1u;
        stream.muted = false;
        stream.gain = 1.0;
        stream.tracksKnown = stream.muteKnown = stream.gainKnown = true;

        Input vod{QStringLiteral("Rostrum VOD Mix")};
        vod.tracks = 2u;
        vod.muted = false;
        vod.gain = 1.0;
        vod.tracksKnown = vod.muteKnown = vod.gainKnown = true;

        state.inputs << mic << stream << vod;

        auto checks = rostrum::obs::rostrumCaptureChecks(recordings, &state, {});
        QCOMPARE(checks.size(), 3);
        QCOMPARE(checks[0].status, CaptureStatus::Configured);
        QCOMPARE(checks[0].kind, CaptureKind::Ok);
        QCOMPARE(checks[1].status, CaptureStatus::Configured);
        QCOMPARE(checks[1].kind, CaptureKind::Ok);
        QCOMPARE(checks[2].status, CaptureStatus::Configured);
        QCOMPARE(checks[2].kind, CaptureKind::Ok);

        // 2. Simultaneous mute and track conflicts: mute does NOT hide track conflict!
        state.inputs[0].tracks = 1u; // mic missing track 2
        state.inputs[0].muted = true;
        state.inputs[1].tracks = 3u; // stream also on track 2
        state.inputs[1].muted = true;
        state.inputs[2].tracks = 1u; // vod on track 1 instead of 2
        state.inputs[2].muted = true;

        checks = rostrum::obs::rostrumCaptureChecks(recordings, &state, {});
        QCOMPARE(checks[0].status, CaptureStatus::WrongTracks);
        QCOMPARE(checks[0].kind, CaptureKind::Warning);
        QCOMPARE(checks[1].status, CaptureStatus::WrongTracks);
        QCOMPARE(checks[1].kind, CaptureKind::Warning);
        QCOMPARE(checks[2].status, CaptureStatus::WrongTracks);
        QCOMPARE(checks[2].kind, CaptureKind::Warning);

        // 3. Valid tracks but muted -> CaptureKind::Muted
        state.inputs[0].tracks = 3u;
        state.inputs[0].muted = true;
        state.inputs[1].tracks = 1u;
        state.inputs[1].muted = true;
        state.inputs[2].tracks = 2u;
        state.inputs[2].muted = true;

        checks = rostrum::obs::rostrumCaptureChecks(recordings, &state, {});
        QCOMPARE(checks[0].status, CaptureStatus::Muted);
        QCOMPARE(checks[0].kind, CaptureKind::Muted);
        QCOMPARE(checks[1].status, CaptureStatus::Muted);
        QCOMPARE(checks[1].kind, CaptureKind::Muted);
        QCOMPARE(checks[2].status, CaptureStatus::Muted);
        QCOMPARE(checks[2].kind, CaptureKind::Muted);

        // 4. Silent gain -> CaptureStatus::Silent and CaptureKind::Warning
        state.inputs[0].muted = false;
        state.inputs[0].gain = 0.0;
        state.inputs[1].muted = false;
        state.inputs[1].gain = 0.0;
        state.inputs[2].muted = false;
        state.inputs[2].gain = 0.0;

        checks = rostrum::obs::rostrumCaptureChecks(recordings, &state, {});
        QCOMPARE(checks[0].status, CaptureStatus::Silent);
        QCOMPARE(checks[0].kind, CaptureKind::Warning);
        QCOMPARE(checks[1].status, CaptureStatus::Silent);
        QCOMPARE(checks[1].kind, CaptureKind::Warning);
        QCOMPARE(checks[2].status, CaptureStatus::Silent);
        QCOMPARE(checks[2].kind, CaptureKind::Warning);

        // 4b. Simultaneous mute and silent gain -> CaptureStatus::Silent and CaptureKind::Warning
        state.inputs[0].muted = true;
        state.inputs[0].gain = 0.0;
        state.inputs[1].muted = true;
        state.inputs[1].gain = 0.0;
        state.inputs[2].muted = true;
        state.inputs[2].gain = 0.0;

        checks = rostrum::obs::rostrumCaptureChecks(recordings, &state, {});
        QCOMPARE(checks[0].status, CaptureStatus::Silent);
        QCOMPARE(checks[0].kind, CaptureKind::Warning);
        QCOMPARE(checks[1].status, CaptureStatus::Silent);
        QCOMPARE(checks[1].kind, CaptureKind::Warning);
        QCOMPARE(checks[2].status, CaptureStatus::Silent);
        QCOMPARE(checks[2].kind, CaptureKind::Warning);

        // 5. Unknown state: null state or unverified tracks
        checks = rostrum::obs::rostrumCaptureChecks(recordings, nullptr, {});
        for (const auto &c : checks) {
            QCOMPARE(c.status, CaptureStatus::Unknown);
            QCOMPARE(c.kind, CaptureKind::Warning);
        }

        state.inputs[0].tracksKnown = false;
        state.inputs[1].muteKnown = false;
        state.inputs[2].gain = std::numeric_limits<double>::quiet_NaN();
        checks = rostrum::obs::rostrumCaptureChecks(recordings, &state, {});
        for (const auto &c : checks) {
            QCOMPARE(c.status, CaptureStatus::Unknown);
            QCOMPARE(c.kind, CaptureKind::Warning);
        }

        // 6. Isolated recording mic on tracks 3-6
        QList<Recording> isolatedRec{
            {QStringLiteral("Rostrum Mic (Recording)"), QStringLiteral("Rostrum Mic (Recording)"), Capture::RostrumMic}};
        State isoState;
        Input isoMic{QStringLiteral("Rostrum Mic (Recording)")};
        isoMic.kind = QLatin1String(kPulseInput);
        isoMic.settings = {{QStringLiteral("device_id"), QStringLiteral("rostrum.filtered")}};
        isoMic.tracks = 4u; // track 3
        isoMic.gain = 1.0;
        isoMic.tracksKnown = isoMic.settingsKnown = isoMic.muteKnown = isoMic.gainKnown = true;
        isoState.inputs << isoMic;
        const QSet<QString> recDevices{QStringLiteral("rostrum.filtered")};

        checks = rostrum::obs::rostrumCaptureChecks(isolatedRec, &isoState, recDevices);
        QCOMPARE(checks.size(), 1);
        QCOMPARE(checks[0].status, CaptureStatus::Isolated);
        QCOMPARE(checks[0].kind, CaptureKind::Ok);

        isoState.inputs[0].muted = true;
        checks = rostrum::obs::rostrumCaptureChecks(isolatedRec, &isoState, recDevices);
        QCOMPARE(checks[0].status, CaptureStatus::Isolated);
        QCOMPARE(checks[0].kind, CaptureKind::Muted);

        // Silent gain on isolated mic -> Silent and Warning
        isoState.inputs[0].muted = false;
        isoState.inputs[0].gain = 0.0;
        checks = rostrum::obs::rostrumCaptureChecks(isolatedRec, &isoState, recDevices);
        QCOMPARE(checks[0].status, CaptureStatus::Silent);
        QCOMPARE(checks[0].kind, CaptureKind::Warning);

        // Simultaneous mute and silent gain on isolated mic -> Silent and Warning
        isoState.inputs[0].muted = true;
        isoState.inputs[0].gain = 0.0;
        checks = rostrum::obs::rostrumCaptureChecks(isolatedRec, &isoState, recDevices);
        QCOMPARE(checks[0].status, CaptureStatus::Silent);
        QCOMPARE(checks[0].kind, CaptureKind::Warning);

        // Non-finite gain on isolated mic -> Unknown and Warning
        isoState.inputs[0].muted = false;
        isoState.inputs[0].gain = std::numeric_limits<double>::quiet_NaN();
        checks = rostrum::obs::rostrumCaptureChecks(isolatedRec, &isoState, recDevices);
        QCOMPARE(checks[0].status, CaptureStatus::Unknown);
        QCOMPARE(checks[0].kind, CaptureKind::Warning);

        isoState.inputs[0].gain = std::numeric_limits<double>::infinity();
        checks = rostrum::obs::rostrumCaptureChecks(isolatedRec, &isoState, recDevices);
        QCOMPARE(checks[0].status, CaptureStatus::Unknown);
        QCOMPARE(checks[0].kind, CaptureKind::Warning);

        // Reset gain to valid
        isoState.inputs[0].gain = 1.0;

        // If also on track 1, not isolated! Evaluated as stream mic: missing track 2 -> Warning
        isoState.inputs[0].tracks = 5u; // tracks 1 and 3
        isoState.inputs[0].muted = false;
        checks = rostrum::obs::rostrumCaptureChecks(isolatedRec, &isoState, recDevices);
        QCOMPARE(checks[0].status, CaptureStatus::WrongTracks);
        QCOMPARE(checks[0].kind, CaptureKind::Warning);
    }

    void assignedFilteredMicWarning_data()
    {
        QTest::addColumn<QString>("configuredDevice");
        QTest::addColumn<QString>("observedDevice");
        QTest::addColumn<quint32>("tracks");
        QTest::addColumn<quint32>("assignedTracks");
        QTest::addColumn<bool>("intended");
        QTest::newRow("assigned filtered mic") << QStringLiteral("rostrum.filtered")
            << QStringLiteral("rostrum.filtered") << 4u << 4u << true;
        QTest::newRow("unassigned filtered mic") << QStringLiteral("rostrum.filtered")
            << QStringLiteral("rostrum.filtered") << 4u << 0u << false;
        QTest::newRow("other recording track") << QStringLiteral("rostrum.filtered")
            << QStringLiteral("rostrum.filtered") << 8u << 4u << false;
        QTest::newRow("also reaches stream") << QStringLiteral("rostrum.filtered")
            << QStringLiteral("rostrum.filtered") << 5u << 4u << false;
        QTest::newRow("hardware mic") << QStringLiteral("alsa_input.hardware")
            << QStringLiteral("alsa_input.hardware") << 4u << 4u << false;
        QTest::newRow("moved to hardware") << QStringLiteral("rostrum.filtered")
            << QStringLiteral("alsa_input.hardware") << 4u << 4u << false;
    }

    void assignedFilteredMicWarning()
    {
        QFETCH(QString, configuredDevice);
        QFETCH(QString, observedDevice);
        QFETCH(quint32, tracks);
        QFETCH(quint32, assignedTracks);
        QFETCH(bool, intended);
        rostrum::pw::Graph graph;
        rostrum::pw::Node source;
        source.id = 1;
        source.name = observedDevice;
        source.mediaClass = QStringLiteral("Audio/Source");
        graph.nodes.insert(source.id, source);
        rostrum::pw::Node capture;
        capture.id = 2;
        capture.name = QStringLiteral("OBS: Rostrum Mic (Recording)");
        capture.appName = QStringLiteral("OBS");
        capture.mediaClass = QStringLiteral("Stream/Input/Audio");
        graph.nodes.insert(capture.id, capture);
        rostrum::pw::Link link;
        link.id = 3;
        link.outNode = source.id;
        link.inNode = capture.id;
        graph.links.insert(link.id, link);
        const auto observed = obsRecordings(graph);
        QCOMPARE(observed.size(), 1);
        // Graph classification stays conservative for unassigned filtered sources.
        QCOMPARE(observed.first().capture, Capture::Mic);
        Input input;
        input.name = QStringLiteral("Rostrum Mic (Recording)");
        input.kind = QLatin1String(kPulseInput);
        input.settings = {{QStringLiteral("device_id"), configuredDevice}};
        input.settingsKnown = input.tracksKnown = true;
        input.tracks = tracks;
        QCOMPARE(assignedRecordingMic(observed.first(), input, assignedTracks), intended);
        input.settingsKnown = false;
        QVERIFY(!assignedRecordingMic(observed.first(), input, assignedTracks));
    }

    void goLiveProblemsAreFound()
    {
        Scene scene = rostrum::defaults::scene();
        QVERIFY(goLiveProblems(scene, {}, nullptr).isEmpty());

        scene.micBus()->muted = true;
        QCOMPARE(goLiveProblems(scene, {}, nullptr), QList<GoLiveProblem>{GoLiveProblem::MicMuted});
        scene.micBus()->muted = false;
        scene.micBus()->destination = rostrum::Destination::Phones;
        QCOMPARE(goLiveProblems(scene, {}, nullptr), QList<GoLiveProblem>{GoLiveProblem::MicMuted});
        scene.micBus()->destination = rostrum::Destination::Stream;

        scene.masterStreamMuted = true;
        QCOMPARE(goLiveProblems(scene, {}, nullptr), QList<GoLiveProblem>{GoLiveProblem::StreamMixSilent});
        scene.masterStreamMuted = false;
        for (auto &bus : scene.buses) {
            if (!bus.isInput() && rostrum::feedsStream(bus.destination) && bus.id != QLatin1String("music")) {
                bus.muted = true;
            }
        }
        QVERIFY(goLiveProblems(scene, {}, nullptr).isEmpty()); // Music still reaches the stream
        // Solo dims the stream too: soloing a muted bus leaves nothing.
        QCOMPARE(goLiveProblems(scene, {QStringLiteral("game")}, nullptr),
                 QList<GoLiveProblem>{GoLiveProblem::StreamMixSilent});
        scene.bus(QStringLiteral("music"))->volume = 0.0;
        QCOMPARE(goLiveProblems(scene, {}, nullptr), QList<GoLiveProblem>{GoLiveProblem::StreamMixSilent});

        const Scene fine = rostrum::defaults::scene();
        const QList<Recording> none;
        QCOMPARE(goLiveProblems(fine, {}, &none),
                 (QList<GoLiveProblem>{GoLiveProblem::NoStreamMixCapture, GoLiveProblem::NoMicCapture}));
        const QList<Recording> both{
            {QStringLiteral("Mic/Aux"), QStringLiteral("Rostrum Mic"), Capture::RostrumMic},
            {QStringLiteral("Desktop Audio"), QStringLiteral("Rostrum Stream Mix"), Capture::RostrumStream}};
        QVERIFY(goLiveProblems(fine, {}, &both).isEmpty());
        const QList<Recording> desktop{
            {QStringLiteral("Mic/Aux"), QStringLiteral("Rostrum Mic"), Capture::RostrumMic},
            {QStringLiteral("Desktop Audio"), QStringLiteral("Speakers"), Capture::Output}};
        QCOMPARE(goLiveProblems(fine, {}, &desktop), QList<GoLiveProblem>{GoLiveProblem::NoStreamMixCapture});
    }

    void sceneMapPicksKnownScenes()
    {
        const QMap<QString, QString> map{{QStringLiteral("BRB"), QStringLiteral("Be Right Back")},
                                         {QStringLiteral("Game"), QStringLiteral("Deleted")}};
        const QStringList ours{QStringLiteral("Live"), QStringLiteral("Be Right Back")};
        QCOMPARE(mappedScene(map, QStringLiteral("BRB"), ours), QStringLiteral("Be Right Back"));
        QVERIFY(mappedScene(map, QStringLiteral("Game"), ours).isEmpty());
        QVERIFY(mappedScene(map, QStringLiteral("Overlays"), ours).isEmpty());
        QVERIFY(mappedScene(map, QString(), ours).isEmpty());
    }

    void twitchVodTrackPlanAndReadiness_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("advEnabled");
        QTest::addColumn<QString>("advTrack");
        QTest::addColumn<QString>("advStreamTrack");
        QTest::addColumn<QString>("simpleEnabled");
        QTest::addColumn<QString>("simpleAdvanced");
        QTest::addColumn<QStringList>("writes");
        QTest::addColumn<int>("status");
        const int attention = int(ReadinessStatus::Attention), verified = int(ReadinessStatus::Verified);
        // OBS's defaults read back as values: stream track 1, VOD track 2, custom encoder off.
        QTest::newRow("advanced, off on the default track")
            << "Advanced" << "" << "2" << "1" << "" << "false"
            << QStringList{QStringLiteral("AdvOut/VodTrackEnabled=true")} << attention;
        QTest::newRow("advanced, on track 3")
            << "Advanced" << "true" << "3" << "1" << "" << "false"
            << QStringList{QStringLiteral("AdvOut/VodTrackIndex=2")} << attention;
        QTest::newRow("advanced, off on track 4")
            << "Advanced" << "false" << "4" << "1" << "" << "false"
            << QStringList{QStringLiteral("AdvOut/VodTrackEnabled=true"),
                           QStringLiteral("AdvOut/VodTrackIndex=2")}
            << attention;
        QTest::newRow("advanced, sends track 2")
            << "Advanced" << "true" << "2" << "1" << "" << "false" << QStringList{} << verified;
        QTest::newRow("advanced, on written as a number")
            << "Advanced" << "1" << "2" << "1" << "" << "false" << QStringList{} << verified;
        // OBS sends no VOD track when it is the stream track; Rostrum leaves the stream track alone.
        QTest::newRow("advanced, streams track 2")
            << "Advanced" << "true" << "2" << "2" << "" << "false" << QStringList{} << attention;
        // Simple output: always track 2, and only with custom encoder settings on.
        QTest::newRow("simple, custom encoder, off")
            << "Simple" << "true" << "2" << "1" << "false" << "true"
            << QStringList{QStringLiteral("SimpleOutput/VodTrackEnabled=true")} << attention;
        QTest::newRow("simple, custom encoder, on")
            << "Simple" << "" << "" << "1" << "true" << "true" << QStringList{} << verified;
        QTest::newRow("simple, no custom encoder")
            << "Simple" << "true" << "2" << "1" << "true" << "false" << QStringList{} << attention;
        QTest::newRow("mode unset") << "" << "true" << "2" << "1" << "true" << "" << QStringList{}
                                    << attention;
    }

    void twitchVodTrackPlanAndReadiness()
    {
        QFETCH(QString, mode);
        QFETCH(QString, advEnabled);
        QFETCH(QString, advTrack);
        QFETCH(QString, advStreamTrack);
        QFETCH(QString, simpleEnabled);
        QFETCH(QString, simpleAdvanced);
        QFETCH(QStringList, writes);
        QFETCH(int, status);
        State state = liveRead(stateFromCollection(fixture()));
        state.streamService = QStringLiteral("Twitch");
        state.vod = {mode, advEnabled, advTrack, advStreamTrack, simpleEnabled, simpleAdvanced};
        const Plan plan = makePlan(state, facts(), Mode::Live);
        QVERIFY(!plan.incomplete());
        QStringList planned;
        for (const auto &d : describe(plan)) {
            if (d.startsWith(QLatin1String("twitch-vod"))) {
                planned << d;
            }
        }
        QCOMPARE(planned, writes.isEmpty()
                              ? QStringList{}
                              : QStringList{QStringLiteral("twitch-vod ") + writes.join(QLatin1Char(' '))});

        rostrum::pw::Graph g;
        ReadinessInput ri;
        ri.graph = &g;
        ri.obsFresh = true;
        state.scopeKnown = true;
        for (auto &in : state.inputs) {
            in.gainKnown = in.settingsKnown = in.tracksKnown = in.muteKnown = true;
            in.gain = 1.0;
        }
        ri.obsState = &state;
        ri.facts = facts();
        ri.scene = rostrum::defaults::scene();
        const auto vodStatus = [&] {
            int found = -1;
            for (const auto &r : evaluateReadiness(ri)) {
                if (r.id == QLatin1String("obs-twitch-vod")) {
                    found = int(r.status);
                }
            }
            return found;
        };
        QCOMPARE(vodStatus(), status);
        state.vodKnown = false;
        QCOMPARE(vodStatus(), int(ReadinessStatus::Unknown));
    }

    void twitchVodTrackLiveWritesOnlyKeysObsReads_data()
    {
        QTest::addColumn<QStringList>("profile"); // "Category/Name=value" set before
        QTest::addColumn<QStringList>("writes");
        QTest::addColumn<QStringList>("undoWrites");
        QTest::newRow("advanced, never set") << QStringList{QStringLiteral("Output/Mode=Advanced")}
                                             << QStringList{QStringLiteral("AdvOut/VodTrackEnabled=true")}
                                             << QStringList{QStringLiteral("AdvOut/VodTrackEnabled=false")};
        QTest::newRow("advanced, track 3") << QStringList{QStringLiteral("Output/Mode=Advanced"),
                                                          QStringLiteral("AdvOut/VodTrackEnabled=true"),
                                                          QStringLiteral("AdvOut/VodTrackIndex=3")}
                                           << QStringList{QStringLiteral("AdvOut/VodTrackIndex=2")}
                                           << QStringList{QStringLiteral("AdvOut/VodTrackIndex=3")};
        QTest::newRow("simple with custom encoder settings")
            << QStringList{QStringLiteral("Output/Mode=Simple"),
                           QStringLiteral("SimpleOutput/UseAdvanced=true")}
            << QStringList{QStringLiteral("SimpleOutput/VodTrackEnabled=true")}
            << QStringList{QStringLiteral("SimpleOutput/VodTrackEnabled=false")};
    }

    void twitchVodTrackLiveWritesOnlyKeysObsReads()
    {
        QFETCH(QStringList, profile);
        QFETCH(QStringList, writes);
        QFETCH(QStringList, undoWrites);
        FakeObs obs(QStringLiteral("pw"));
        populate(obs);
        obs.streamService = QStringLiteral("Twitch");
        obs.profileValues.clear();
        for (const QString &entry : profile) {
            obs.profileValues.insert(entry.section(QLatin1Char('='), 0, 0),
                                     entry.section(QLatin1Char('='), 1));
        }
        Client client;
        QVERIFY(connectClient(client, obs.port(), QStringLiteral("pw")));
        const State before = fetched(client);
        QVERIFY(before.vodKnown);
        QCOMPARE(twitchVod(before.vod), TwitchVod::Off);
        QList<Action> vod;
        for (const auto &a : makePlan(before, facts(), Mode::Live).actions) {
            if (a.type == Action::Type::SetTwitchVodTrack) {
                vod << a;
            }
        }
        QCOMPARE(vod.size(), 1);

        bool done = false;
        QList<UndoOp> applied;
        QString error;
        applyPlan(&client, vod, before, [&](const QList<UndoOp> &ops, const QString &e) {
            applied = ops;
            error = e;
            done = true;
        });
        QTRY_VERIFY(done);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(obs.profileWrites, writes);
        const State after = fetched(client);
        QCOMPARE(twitchVod(after.vod), TwitchVod::Track2);
        for (const auto &a : makePlan(after, facts(), Mode::Live).actions) {
            QVERIFY(a.type != Action::Type::SetTwitchVodTrack);
        }

        // Undo goes through the journal, as after a restart.
        obs.profileWrites.clear();
        Undo undo;
        undo.ops = applied;
        undo = Undo::fromJson(undo.toJson());
        done = false;
        UndoResult result;
        applyUndo(&client, undo, {QString(), obs.collection, obs.profile}, [&](const UndoResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY(done);
        QCOMPARE(result.failed, 0);
        QVERIFY(result.stopped.isEmpty());
        QCOMPARE(obs.profileWrites, undoWrites);
        QCOMPARE(twitchVod(fetched(client).vod), TwitchVod::Off);
    }

    void twitchVodUndoJournal()
    {
        const QString input = QStringLiteral("Twitch VOD Track");
        const auto op = [&](const char *type) {
            return QJsonObject{{QStringLiteral("type"), QLatin1String(type)},
                               {QStringLiteral("input"), input}};
        };
        QJsonObject legacy = op("twitchVodTrack");
        legacy.insert(QStringLiteral("track"), 3);
        QJsonObject legacyUnset = op("twitchVodTrack");
        legacyUnset.insert(QStringLiteral("track"), 0);
        QJsonObject foreign = op("profile");
        foreign.insert(QStringLiteral("parameters"),
                       QJsonArray{QJsonObject{{QStringLiteral("category"), QStringLiteral("Output")},
                                              {QStringLiteral("name"), QStringLiteral("Mode")},
                                              {QStringLiteral("value"), QStringLiteral("Simple")}}});
        const Undo undo = Undo::fromJson({{QStringLiteral("format"), 1},
                                          {QStringLiteral("ops"), QJsonArray{legacy, legacyUnset, foreign}}});
        // Rostrum 0.2.0's op puts back the index it changed; one that read no index has nothing to
        // put back, and undo never writes keys Set Up OBS does not.
        QCOMPARE(undo.ops.size(), 1);
        QCOMPARE(undo.ops.first().type, UndoOp::Type::RestoreProfile);
        QCOMPARE(undo.ops.first().profile,
                 (QList<ProfileParameter>{
                     {QStringLiteral("AdvOut"), QStringLiteral("VodTrackIndex"), QStringLiteral("3")}}));
        QCOMPARE(Undo::fromJson(undo.toJson()).ops.first().profile, undo.ops.first().profile);
    }

    void undoRunsOnlyWhereItWasMade()
    {
        const auto op = [](UndoOp::Type type, const QString &input, const QString &dir,
                           const QString &scope) {
            UndoOp o;
            o.type = type;
            o.input = input;
            o.configDir = dir;
            o.scope = scope;
            if (type == UndoOp::Type::RestoreProfile) {
                o.profile = {
                    {QStringLiteral("AdvOut"), QStringLiteral("VodTrackEnabled"), QStringLiteral("false")}};
            }
            return o;
        };
        const QString dir = QStringLiteral("/obs"), flatpak = QStringLiteral("/flatpak/obs");
        Undo undo;
        undo.ops << op(UndoOp::Type::RestoreMute, QStringLiteral("Game Audio"), dir,
                       QStringLiteral("Main")) // 0
                 << op(UndoOp::Type::RemoveInput, QStringLiteral("Rostrum VOD Mix"), dir,
                       QStringLiteral("Other")) // 1
                 << op(UndoOp::Type::RestoreProfile, QStringLiteral("Twitch VOD Track"), dir,
                       QStringLiteral("Live")) // 2
                 << op(UndoOp::Type::RestoreMute, QStringLiteral("Game Audio"), flatpak,
                       QStringLiteral("Main")); // 3
        State now = stateFromCollection(fixture());
        QCOMPARE(undo.runnable({dir, QStringLiteral("Main"), QStringLiteral("Live")}, now),
                 (QList<qsizetype>{0, 2}));
        QCOMPARE(undo.runnable({dir, QStringLiteral("Main"), QStringLiteral("Practice")}, now),
                 (QList<qsizetype>{0}));
        QCOMPARE(undo.runnable({dir, QStringLiteral("Other"), QStringLiteral("Live")}, now),
                 (QList<qsizetype>{1, 2}));
        // Offline: profile values wait for OBS to run.
        QCOMPARE(undo.runnable({dir, QStringLiteral("Main"), QString()}, now), (QList<qsizetype>{0}));
        QCOMPARE(undo.runnable({flatpak, QStringLiteral("Main"), QString()}, now), (QList<qsizetype>{3}));
        // An unread collection runs nothing.
        QVERIFY(undo.runnable({dir, QString(), QString()}, now).isEmpty());
        QCOMPARE(undo.without({0, 2}).ops.size(), 2);
        QCOMPARE(undo.only({1}).ops.first().scope, QStringLiteral("Other"));

        // The record keeps install and scope; format 1 had one install and no scope.
        const Undo back = Undo::fromJson(undo.toJson());
        QCOMPARE(back.ops.size(), 4);
        for (qsizetype n = 0; n < 4; ++n) {
            QCOMPARE(back.ops.at(n).configDir, undo.ops.at(n).configDir);
            QCOMPARE(back.ops.at(n).scope, undo.ops.at(n).scope);
        }
        QJsonObject v1{{QStringLiteral("format"), 1},
                       {QStringLiteral("configDir"), dir},
                       {QStringLiteral("ops"),
                        QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("mute")},
                                               {QStringLiteral("input"), QStringLiteral("Game Audio")},
                                               {QStringLiteral("muted"), false}},
                                   QJsonObject{{QStringLiteral("type"), QStringLiteral("remove")},
                                               {QStringLiteral("input"), QStringLiteral("Rostrum Mic")}}}}};
        const Undo legacy = Undo::fromJson(v1);
        QCOMPARE(legacy.ops.size(), 2);
        QCOMPARE(legacy.ops.first().configDir, dir);
        QVERIFY(legacy.ops.first().scope.isEmpty());
        // Its inputs are put back only in a collection that has every one of them.
        QVERIFY(legacy.runnable({dir, QStringLiteral("Test"), QString()}, now).isEmpty());
        now.inputs << readInput(QStringLiteral("Rostrum Mic"), kPulseInput, QString::fromLatin1(kMicDevice));
        QCOMPARE(legacy.runnable({dir, QStringLiteral("Test"), QString()}, now), (QList<qsizetype>{0, 1}));
        QVERIFY(legacy.runnable({flatpak, QStringLiteral("Test"), QString()}, now).isEmpty());
        QVERIFY(Undo::fromJson(
                    {{QStringLiteral("format"), 3}, {QStringLiteral("ops"), v1.value(QLatin1String("ops"))}})
                    .isEmpty());
    }

    void liveReadKnowsItsScope()
    {
        FakeObs obs(QStringLiteral("pw"));
        populate(obs);
        Client client;
        QVERIFY(connectClient(client, obs.port(), QStringLiteral("pw")));
        State s = fetched(client);
        QVERIFY(s.collectionKnown && s.profileKnown);
        QCOMPARE(s.collection, obs.collection);
        QCOMPARE(s.profile, obs.profile);
        QVERIFY(!makePlan(s, facts(), Mode::Live).incomplete());

        // A switch while reading: the inputs may be either collection's.
        int lists = 0;
        obs.onRequest = [&](const QString &type) {
            if (type == QLatin1String("GetSceneCollectionList") && ++lists == 2) {
                obs.collection = QStringLiteral("Switched");
            }
        };
        s = fetched(client);
        QVERIFY(!s.collectionKnown);
        QVERIFY(makePlan(s, facts(), Mode::Live).scopeUnread);
        QVERIFY(makePlan(s, facts(), Mode::Live).isEmpty());
        obs.onRequest = {};
        obs.refuse = {QStringLiteral("GetProfileList")};
        s = fetched(client);
        QVERIFY(s.collectionKnown && !s.profileKnown);
        QVERIFY(makePlan(s, facts(), Mode::Live).incomplete());
        // Offline needs only the collection, which its file names.
        s = stateFromCollection(fixture());
        QVERIFY(s.collectionKnown && !s.profileKnown);
        QCOMPARE(s.collection, QStringLiteral("Test"));
        QVERIFY(!makePlan(s, facts(), Mode::Offline).incomplete());
    }

    void applyAndUndoStopWhenObsSwitched()
    {
        FakeObs obs(QStringLiteral("pw"));
        populate(obs);
        Client client;
        QVERIFY(connectClient(client, obs.port(), QStringLiteral("pw")));
        const State before = fetched(client);
        const Plan plan = makePlan(before, facts(), Mode::Live);
        QVERIFY(!plan.isEmpty());
        const auto writes = [&] {
            return std::count_if(obs.requests.cbegin(), obs.requests.cend(),
                                 [](const QString &r) { return !r.startsWith(QLatin1String("Get")); });
        };
        for (const bool profile : {false, true}) {
            (profile ? obs.profile : obs.collection) = QStringLiteral("Switched");
            obs.requests.clear();
            bool done = false;
            QString error;
            QList<UndoOp> ops{UndoOp{}};
            applyPlan(&client, plan.enabledActions(), before,
                      [&](const QList<UndoOp> &applied, const QString &e) {
                          ops = applied;
                          error = e;
                          done = true;
                      });
            QTRY_VERIFY(done);
            QVERIFY(error.contains(QLatin1String("switched")));
            QVERIFY(ops.isEmpty());
            QCOMPARE(writes(), 0);

            Undo undo;
            undo.ops << undoFor(plan.enabledActions().first(), before);
            done = false;
            UndoResult result;
            applyUndo(&client, undo, {QString(), before.collection, before.profile},
                      [&](const UndoResult &r) {
                          result = r;
                          done = true;
                      });
            QTRY_VERIFY(done);
            QVERIFY(result.stopped.contains(QLatin1String("switched")));
            QVERIFY(result.settled.isEmpty());
            QCOMPARE(writes(), 0);
            obs.collection = QStringLiteral("Test collection");
            obs.profile = QStringLiteral("Test profile");
        }
    }

    void undoKeepsWhatItDidNotTry()
    {
        FakeObs obs(QStringLiteral("pw"));
        populate(obs);
        Client client;
        QVERIFY(connectClient(client, obs.port(), QStringLiteral("pw")));
        Undo undo;
        UndoOp unmute; // oldest: tried last
        unmute.type = UndoOp::Type::RestoreMute;
        unmute.input = QStringLiteral("Game Audio");
        UndoOp remove;
        remove.type = UndoOp::Type::RemoveInput;
        remove.input = QStringLiteral("Discord Audio");
        undo.ops << unmute << remove;
        const UndoScope scope{QString(), obs.collection, obs.profile};

        // A refusal while OBS is there is reported and done with.
        obs.refuse = {QStringLiteral("RemoveInput")};
        bool done = false;
        UndoResult result;
        applyUndo(&client, undo, scope, [&](const UndoResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY(done);
        QVERIFY(result.stopped.isEmpty());
        QCOMPARE(result.failed, 1);
        QCOMPARE(result.settled, (QList<qsizetype>{0, 1}));

        // OBS going away part way: the op it was on and the ones after stay.
        obs.refuse.clear();
        obs.ignored = {QStringLiteral("RemoveInput")};
        obs.requests.clear();
        done = false;
        applyUndo(&client, undo, scope, [&](const UndoResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY(obs.requests.contains(QStringLiteral("RemoveInput")));
        obs.dropClients();
        QTRY_VERIFY(done);
        QVERIFY(!result.stopped.isEmpty());
        QVERIFY(result.settled.isEmpty());
        QCOMPARE(result.failed, 0);
        QVERIFY(obs.inputs.contains(QStringLiteral("Discord Audio")));
    }

    void unansweredWritesTimeOutAndStayUndoable()
    {
        FakeObs obs(QStringLiteral("pw"));
        populate(obs);
        Client client;
        client.setWriteTimeoutMs(300);
        QVERIFY(connectClient(client, obs.port(), QStringLiteral("pw")));
        const State before = fetched(client);
        const Plan plan = makePlan(before, facts(), Mode::Live);

        // OBS stops answering at the first mute. Setup ends, and what it did, the unanswered
        // mute included, was reported step by step for the undo record.
        obs.ignored = {QStringLiteral("SetInputMute")};
        bool done = false;
        QString error;
        QList<UndoOp> applied, asTheyHappened;
        applyPlan(
            &client, plan.enabledActions(), before,
            [&](const QList<UndoOp> &ops, const QString &e) {
                applied = ops;
                error = e;
                done = true;
            },
            [&](const UndoOp &op) {
                QVERIFY(!done);
                asTheyHappened << op;
            });
        QTRY_VERIFY(done);
        QVERIFY2(Client::timedOut(error), qPrintable(error));
        QVERIFY(!applied.isEmpty());
        QCOMPARE(applied.last().type, UndoOp::Type::RestoreMute);
        QCOMPARE(asTheyHappened.size(), applied.size());
        QCOMPARE(obs.requests.count(QStringLiteral("SetInputMute")), 1);

        // Undo is not settled by silence either: the unanswered op and the ones after it stay.
        Undo undo;
        undo.ops = applied;
        done = false;
        UndoResult result;
        applyUndo(&client, undo, {QString(), obs.collection, obs.profile}, [&](const UndoResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY(done);
        QVERIFY(Client::timedOut(result.stopped));
        QVERIFY(result.settled.isEmpty()); // the mute is newest, so it was tried first
        obs.ignored.clear();
        done = false;
        applyUndo(&client, undo, {QString(), obs.collection, obs.profile}, [&](const UndoResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY(done);
        QVERIFY(result.stopped.isEmpty());
        QCOMPARE(result.settled.size(), undo.ops.size());
        QVERIFY(!obs.inputs.contains(QStringLiteral("Rostrum Stream Mix")));
    }

    void undoPutsBackAnInputWithNoTracks()
    {
        // The mic source Rostrum takes over is given tracks 1 and 2. One that was on no track
        // (kept for monitoring, say) goes back to none, live and offline, through the record.
        FakeObs obs(QStringLiteral("pw"));
        populate(obs);
        obs.inputs[QStringLiteral("Mic/Aux")].tracks = tracks({});
        Client client;
        QVERIFY(connectClient(client, obs.port(), QStringLiteral("pw")));
        const State before = fetched(client);
        QCOMPARE(before.input(QStringLiteral("Mic/Aux"))->tracks, 0u);
        const Plan plan = makePlan(before, facts(), Mode::Live);
        bool done = false;
        Undo undo;
        applyPlan(&client, plan.enabledActions(), before,
                  [&](const QList<UndoOp> &ops, const QString &error) {
                      QVERIFY2(error.isEmpty(), qPrintable(error));
                      undo.ops = ops;
                      done = true;
                  });
        QTRY_VERIFY(done);
        QCOMPARE(obs.inputs[QStringLiteral("Mic/Aux")].tracks, tracks({1, 2}));
        undo = Undo::fromJson(undo.toJson());
        done = false;
        applyUndo(&client, undo, {QString(), obs.collection, obs.profile},
                  [&](const UndoResult &) { done = true; });
        QTRY_VERIFY(done);
        QCOMPARE(obs.inputs[QStringLiteral("Mic/Aux")].tracks, tracks({}));

        QJsonObject doc = fixture();
        QJsonObject mic = doc.value(QLatin1String("AuxAudioDevice1")).toObject();
        QVERIFY(!mic.isEmpty());
        mic.insert(QStringLiteral("mixers"), 0);
        doc.insert(QStringLiteral("AuxAudioDevice1"), mic);
        const State offline = stateFromCollection(doc);
        const Plan offlinePlan = makePlan(offline, facts(), Mode::Offline);
        Undo offlineUndo;
        for (const auto &a : offlinePlan.actions) {
            offlineUndo.ops << undoFor(a, offline);
        }
        const QJsonObject changed = applyToCollection(doc, offlinePlan.actions);
        QCOMPARE(stateFromCollection(changed).input(QStringLiteral("Mic/Aux"))->tracks, 3u);
        QCOMPARE(undoInCollection(changed, Undo::fromJson(offlineUndo.toJson())), doc);
    }
};

QTEST_MAIN(TestObs)
#include "tst_obs.moc"
