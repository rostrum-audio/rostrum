#include "engine/Engine.h"
#include "obs/ObsClient.h"
#include "obs/ObsConfig.h"
#include "obs/ObsLive.h"
#include "obs/ObsPlan.h"
#include "obs/ObsStatus.h"
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
    return f;
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
    bool nested = false;
    QStringList refuse; // request types to fail
    bool streaming = false;
    bool recording = false;
    bool recordPaused = false;
    qint64 outputDuration = 0;
    QString programScene;

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
            for (const auto &name : scenes.value(programScene))
                items.append(QJsonObject{{QStringLiteral("sourceName"), name},
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
        } else if (type == QLatin1String("CreateSceneItem")) {
            const QString scene = rd.value(QLatin1String("sceneName")).toString();
            const QString source = rd.value(QLatin1String("sourceName")).toString();
            if (!inputs.contains(source) || !scenes.contains(scene)) {
                return false;
            }
            scenes[scene] << source;
        } else if (type == QLatin1String("RemoveInput")) {
            if (!inputs.remove(name)) {
                return false;
            }
            for (auto &items : scenes) {
                items.removeAll(name);
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

} // namespace

class TestObs : public QObject
{
    Q_OBJECT

private Q_SLOTS:
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
        const Plan plan = makePlan(stateFromCollection(fixture()), facts(), Mode::Live);
        QCOMPARE(describe(plan),
                 (QStringList{
                     QStringLiteral("set Mic/Aux rostrum.mic"),
                     QStringLiteral("create Rostrum Stream Mix pulse_output_capture rostrum.stream.monitor in "
                                    "Game,Desktop,BRB,Overlays tracks 5"),
                     QStringLiteral("mute Game Audio"),
                     QStringLiteral("mute Discord Audio"),
                     QStringLiteral("mute Spotify"),
                 }));
        QCOMPARE(plan.actions.at(2).capture, Capture::Output);
        QCOMPARE(plan.actions.at(3).capture, Capture::App);
    }

    void offlinePlanUsesGlobalDesktopAudio()
    {
        const Plan plan = makePlan(stateFromCollection(fixture()), facts(), Mode::Offline);
        QCOMPARE(describe(plan),
                 (QStringList{
                     QStringLiteral("set Mic/Aux rostrum.mic"),
                     QStringLiteral("global desktop1 Desktop Audio pulse_output_capture rostrum.stream.monitor tracks 5"),
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
        QCOMPARE(s.channel(QStringLiteral("desktop1"))->tracks, 5u);
        QCOMPARE(s.input(QStringLiteral("Mic/Aux"))->settings.value(QLatin1String("device_id")).toString(),
                 QString::fromLatin1(kMicDevice));
        QVERIFY(s.input(QStringLiteral("Game Audio"))->muted);
        QVERIFY(makePlan(s, facts(), Mode::Offline).isEmpty());
        QVERIFY(makePlan(s, facts(), Mode::Live).isEmpty());

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
        State s = stateFromCollection(fixture());
        Input stream{QStringLiteral("Rostrum Stream Mix"), QString::fromLatin1(kPulseOutput),
                     {{QStringLiteral("device_id"), QString::fromLatin1(kStreamDevice)}}, true};
        s.inputs << stream;
        const QStringList d = describe(makePlan(s, facts(), Mode::Live));
        QVERIFY(d.contains(QStringLiteral("unmute Rostrum Stream Mix")));
        QVERIFY(!d.join(QLatin1Char('\n')).contains(QLatin1String("create")));
    }

    void secondCopyOfRostrumMicIsMuted()
    {
        State s;
        s.scenes = {QStringLiteral("A")};
        s.inputs << Input{QStringLiteral("One"), QString::fromLatin1(kPulseInput), {{QStringLiteral("device_id"), QString::fromLatin1(kMicDevice)}}}
                 << Input{QStringLiteral("Two"), QString::fromLatin1(kPulseInput), {{QStringLiteral("device_id"), QString::fromLatin1(kMicDevice)}}}
                 << Input{QStringLiteral("Mix"), QString::fromLatin1(kPulseOutput), {{QStringLiteral("device_id"), QString::fromLatin1(kStreamDevice)}}};
        QCOMPARE(describe(makePlan(s, facts(), Mode::Live)), QStringList{QStringLiteral("mute Two")});
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
        QCOMPARE(obs.inputs[QStringLiteral("Rostrum Stream Mix")].tracks, tracks({1, 3}));
        for (const QString &scene : obs.scenes.keys()) {
            QVERIFY2(obs.scenes[scene].contains(QStringLiteral("Rostrum Stream Mix")), qPrintable(scene));
        }
        QVERIFY(obs.inputs[QStringLiteral("Game Audio")].muted);
        QVERIFY(obs.inputs[QStringLiteral("Discord Audio")].muted);

        bool refetched = false;
        fetchState(&client, [&](const State &s, const QString &) {
            QVERIFY(makePlan(s, facts(), Mode::Live).isEmpty());
            refetched = true;
        });
        QTRY_VERIFY(refetched);

        bool undone = false;
        applyUndo(&client, undo, [&](int failed, const QString &) {
            QCOMPARE(failed, 0);
            undone = true;
        });
        QTRY_VERIFY(undone);
        QCOMPARE(obs.inputs[QStringLiteral("Mic/Aux")].settings.value(QLatin1String("device_id")).toString(), kHwMic);
        QVERIFY(!obs.inputs.contains(QStringLiteral("Rostrum Stream Mix")));
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
};

QTEST_MAIN(TestObs)
#include "tst_obs.moc"
