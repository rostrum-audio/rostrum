#include "obs/Readiness.h"

#include <QTest>
using namespace rostrum;
using namespace rostrum::obs;
namespace {
ReadinessResult row(const QList<ReadinessResult> &rows, const QString &id)
{
    for (const auto &r : rows)
        if (r.id == id)
            return r;
    return {};
}
pw::Graph graph()
{
    pw::Graph g;
    uint32_t id = 1, port = 100;
    for (const QString &name : {QStringLiteral("rostrum.desktop"), QStringLiteral("rostrum.phones"),
                                QStringLiteral("rostrum.stream"), QStringLiteral("hardware")}) {
        pw::Node n;
        n.id = id++;
        n.name = name;
        n.mediaClass = QStringLiteral("Audio/Sink");
        n.channels = 2;
        n.volumes = {1, 1};
        g.nodes.insert(n.id, n);
        for (const auto &channel : {QStringLiteral("FL"), QStringLiteral("FR")}) {
            pw::Port out;
            out.id = port++;
            out.nodeId = n.id;
            out.output = true;
            out.monitor = true;
            out.channel = channel;
            g.ports.insert(out.id, out);
            pw::Port in = out;
            in.id = port++;
            in.output = false;
            in.monitor = false;
            g.ports.insert(in.id, in);
        }
    }
    return g;
}
void link(pw::Graph &g, uint32_t from, uint32_t to)
{
    const auto outputs = g.outputPorts(from), inputs = g.inputPorts(to);
    for (int i = 0; i < 2; ++i) {
        pw::Link l;
        l.id = 200 + g.links.size();
        l.outNode = from;
        l.inNode = to;
        l.outPort = outputs[i].id;
        l.inPort = inputs[i].id;
        g.links.insert(l.id, l);
    }
}
ReadinessInput input(const pw::Graph &g)
{
    ReadinessInput i;
    i.scene = defaults::scene();
    i.graph = &g;
    i.resolvedPhones = QStringLiteral("hardware");
    return i;
}
} // namespace
class TestReadiness : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void intendedRecordingIsNotABroadcastBypass()
    {
        auto g = graph();
        auto i = input(g);
        State state;
        state.scopeKnown = true;
        state.programInputs.insert(QStringLiteral("Recording Desktop"));
        Input in;
        in.name = QStringLiteral("Recording Desktop");
        in.kind = QLatin1String(kPulseOutput);
        in.settings = {{QStringLiteral("device_id"), QStringLiteral("rostrum.desktop.monitor")}};
        in.settingsKnown = in.muteKnown = in.tracksKnown = in.gainKnown = true;
        in.tracks = 8;
        state.inputs << in;
        i.obsState = &state;
        i.obsFresh = true;
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("obs-Recording Desktop")).status,
                 ReadinessStatus::Attention);
        i.recordingDevices.insert(QStringLiteral("rostrum.desktop.monitor"));
        // Configuration is intentional, but no capture stream was observed: never Verified.
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("obs-Recording Desktop")).status,
                 ReadinessStatus::Unknown);
        state.inputs[0].tracks |= 1;
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("obs-Recording Desktop")).status,
                 ReadinessStatus::Attention);
    }

    void effectiveMuteAndIntent()
    {
        ReadinessInput i;
        i.scene = defaults::scene();
        i.inspectRouting = false;
        i.mutes = {true, true};
        auto warnings = readinessWarnings(evaluateReadiness(i));
        QVERIFY(warnings.contains(GoLiveProblem::MicMuted));
        QVERIFY(warnings.contains(GoLiveProblem::StreamMixSilent));
        i.scene.micBus()->muted = true;
        i.mutes = {false, false};
        QVERIFY(!readinessWarnings(evaluateReadiness(i)).contains(GoLiveProblem::MicMuted));
        for (auto &b : i.scene.buses)
            if (!b.isInput())
                b.destination = Destination::Phones;
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("stream-level")).status, ReadinessStatus::Excluded);
        QVERIFY(goLiveProblems(i.scene, {}, nullptr).contains(GoLiveProblem::StreamMixSilent));
    }
    void exclusionChecksEveryLink()
    {
        auto g = graph();
        link(g, 1, 2);
        link(g, 2, 4);
        link(g, 1, 3);
        auto i = input(g);
        i.scene.bus(QStringLiteral("desktop"))->destination = Destination::Phones;
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("desktop-stream")).status,
                 ReadinessStatus::Attention);
        g.links.remove(204);
        g.links.remove(205);
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("desktop-stream")).status,
                 ReadinessStatus::Excluded);
        i.scene.bus(QStringLiteral("desktop"))->destination = Destination::Stream;
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("desktop-phones")).status,
                 ReadinessStatus::Attention);
    }
    void missingDeviceAndIdleApp()
    {
        auto g = graph();
        auto i = input(g);
        i.selectedMic = QStringLiteral("saved");
        i.micMissing = true;
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("mic-device")).status, ReadinessStatus::Attention);
        i.micFallback = true;
        i.resolvedMic = QStringLiteral("other");
        QVERIFY(
            row(evaluateReadiness(i), QStringLiteral("mic-device")).detail.contains(QStringLiteral("other")));
        pw::Node n;
        n.id = 5;
        n.name = QStringLiteral("idle");
        n.mediaClass = QStringLiteral("Stream/Output/Audio");
        g.nodes.insert(5, n);
        i.apps.append({5, QStringLiteral("desktop")});
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("app-5")).status, ReadinessStatus::Excluded);
        g.nodes[5].running = true;
        QVERIFY(row(evaluateReadiness(i), QStringLiteral("app-5")).status != ReadinessStatus::Verified);
    }
    void channelStateAndMissingChannels()
    {
        auto g = graph();
        link(g, 1, 3);
        auto i = input(g);
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("desktop-stream")).status,
                 ReadinessStatus::Unknown);
        for (auto &l : g.links)
            l.state = QStringLiteral("active");
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("desktop-stream")).status,
                 ReadinessStatus::Verified);
        auto partial = g;
        for (auto &p : partial.ports)
            if (p.nodeId == 3 && !p.output && p.channel == QLatin1String("FR"))
                p.channel = QStringLiteral("AUX1");
        QCOMPARE(row(evaluateReadiness(input(partial)), QStringLiteral("desktop-stream")).status,
                 ReadinessStatus::Unknown);
        g.links.remove(201);
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("desktop-stream")).status,
                 ReadinessStatus::Attention);
        link(g, 1, 3);
        for (auto &l : g.links)
            l.state = QStringLiteral("paused");
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("desktop-stream")).status,
                 ReadinessStatus::Excluded);
        g.links.begin()->state = QStringLiteral("error");
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("desktop-stream")).status,
                 ReadinessStatus::Attention);
    }
    void obsConfigurationAndObservedTarget()
    {
        auto g = graph();
        auto i = input(g);
        State state;
        state.scopeKnown = true;
        Input in;
        in.name = QStringLiteral("Mix");
        in.kind = QString::fromLatin1(kPulseOutput);
        in.channel = QStringLiteral("desktop1");
        in.settings = {{QStringLiteral("device_id"), QString::fromLatin1(kStreamDevice)}};
        in.settingsKnown = in.muteKnown = in.tracksKnown = in.gainKnown = true;
        in.tracks = 1;
        state.inputs.append(in);
        i.obsState = &state;
        i.obsFresh = true;
        pw::Node capture;
        capture.id = 5;
        capture.name = QStringLiteral("OBS: Mix");
        capture.mediaClass = QStringLiteral("Stream/Input/Audio");
        capture.channels = 2;
        g.nodes.insert(5, capture);
        const auto ports = g.inputPorts(4);
        for (auto p : ports) {
            p.id += 20;
            p.nodeId = 5;
            g.ports.insert(p.id, p);
        }
        link(g, 3, 5);
        for (auto &l : g.links)
            l.state = QStringLiteral("active");
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("obs-Mix")).status, ReadinessStatus::Verified);
        i.facts.defaultSink = QStringLiteral("rostrum.stream");
        state.inputs[0].settings = {{QStringLiteral("device_id"), 42}};
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("obs-Mix")).status, ReadinessStatus::Unknown);
        state.inputs[0].settings = {{QStringLiteral("device_id"), QString::fromLatin1(kStreamDevice)}};
        i.obsFresh = false;
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("obs")).status, ReadinessStatus::Unknown);
        i.obsFresh = true;
        state.inputs[0].muteKnown = false;
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("obs-Mix")).status, ReadinessStatus::Unknown);
        QVERIFY(!readinessWarnings(evaluateReadiness(i)).contains(GoLiveProblem::NoStreamMixCapture));
        state.inputs[0].muteKnown = true;
        state.inputs[0].muted = true;
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("obs-Mix")).status, ReadinessStatus::Attention);
        state.inputs[0].settings = {{QStringLiteral("device_id"), QStringLiteral("hardware.monitor")}};
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("obs-Mix")).status, ReadinessStatus::Excluded);
        state.inputs[0].kind = QString::fromLatin1(kPwInput);
        state.inputs[0].settings = {{QStringLiteral("TargetId"), 987654}};
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("obs-Mix")).status, ReadinessStatus::Unknown);
        QVERIFY(!readinessWarnings(evaluateReadiness(i)).contains(GoLiveProblem::NoStreamMixCapture));
        state.inputs[0].kind = QString::fromLatin1(kPulseInput);
        state.inputs[0].settings = {{QStringLiteral("device_id"), QString::fromLatin1(kStreamDevice)}};
        state.inputs[0].muted = false;
        for (auto &l : g.links)
            l.outNode = 2;
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("obs-Mix")).status, ReadinessStatus::Attention);
        state.inputs[0].settings = {{QStringLiteral("device_id"), QStringLiteral("hardware.monitor")}};
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("obs-Mix")).status, ReadinessStatus::Attention);
        state.scopeKnown = false;
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("obs")).status, ReadinessStatus::Unknown);
    }
    void unavailableObsCannotPass()
    {
        ReadinessInput i;
        i.scene = defaults::scene();
        i.inspectRouting = false;
        QCOMPARE(row(evaluateReadiness(i), QStringLiteral("obs")).status, ReadinessStatus::Unknown);
        i.obsError = QStringLiteral("timed out");
        QVERIFY(
            row(evaluateReadiness(i), QStringLiteral("obs")).detail.contains(QStringLiteral("timed out")));
    }
};
QTEST_GUILESS_MAIN(TestReadiness)
#include "tst_readiness.moc"
