#include "core/Model.h"
#include "core/Volume.h"
#include "engine/NodeSpecs.h"
#include "pw/Graph.h"

#include <QTest>

#include <cmath>
#include <limits>

using namespace rostrum;

class TestModel : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void defaultScene()
    {
        const Scene s = defaults::scene();
        QCOMPARE(s.name, QStringLiteral("Live"));
        QCOMPARE(s.buses.size(), 6);
        const QStringList ids = {QStringLiteral("mic"),   QStringLiteral("game"),   QStringLiteral("voice"),
                                 QStringLiteral("music"), QStringLiteral("alerts"), QStringLiteral("desktop")};
        for (int i = 0; i < ids.size(); ++i) {
            QCOMPARE(s.buses.at(i).id, ids.at(i));
            QCOMPARE(s.buses.at(i).color, QString::fromLatin1(defaults::palette().at(i).hex));
        }
        QVERIFY(s.micBus()->isInput());
        QCOMPARE(s.micBus()->destination, Destination::Stream);
        QCOMPARE(s.bus(QStringLiteral("music"))->destination, Destination::Stream);
        QCOMPARE(s.bus(QStringLiteral("game"))->destination, Destination::Both);
        QCOMPARE(s.bus(QStringLiteral("desktop"))->destination, Destination::Both);
        QCOMPARE(s.sidetoneVolume, 0.0);
        QCOMPARE(defaults::palette().size(), 12);
    }

    void slugs()
    {
        QCOMPARE(makeSlug(QStringLiteral("Just Chatting"), {}), QStringLiteral("just-chatting"));
        QCOMPARE(makeSlug(QStringLiteral("Game"), {QStringLiteral("game")}), QStringLiteral("game-2"));
        QCOMPARE(makeSlug(QStringLiteral("!!!"), {}), QStringLiteral("bus"));
        QCOMPARE(makeSlug(QStringLiteral("Café"), {}), QStringLiteral("cafe"));
    }

    void summary()
    {
        Scene s = defaults::scene();
        s.micBus()->muted = true;
        QCOMPARE(s.summary(), QStringLiteral("6 buses, Music → Stream, mic muted"));
    }

    void mergeKeepsSavedLevelsButTakesStructure()
    {
        Scene saved = defaults::scene();
        Scene current = saved;
        current.bus(QStringLiteral("game"))->volume = 0.3;
        current.bus(QStringLiteral("game"))->name = QStringLiteral("Ranked");
        current.bus(QStringLiteral("game"))->color = QStringLiteral("#123456");
        current.masterStream = 0.5;
        current.rules.append({QStringLiteral("Discord"), MatchKey::Name, QStringLiteral("voice")});
        const Scene merged = mergeStructure(saved, current);
        QCOMPARE(merged.bus(QStringLiteral("game"))->volume, 1.0);
        QCOMPARE(merged.bus(QStringLiteral("game"))->name, QStringLiteral("Ranked"));
        QCOMPARE(merged.bus(QStringLiteral("game"))->color, QStringLiteral("#123456"));
        QCOMPARE(merged.masterStream, 1.0);
        QCOMPARE(merged.rules.size(), 1);
    }

    void volumeCurve()
    {
        QCOMPARE(volume::faderToLinear(1.0), 1.0);
        QCOMPARE(volume::faderToLinear(0.0), 0.0);
        QVERIFY(qAbs(volume::faderToLinear(0.5) - 0.125) < 1e-9);
        QVERIFY(qAbs(volume::linearToFader(0.125) - 0.5) < 1e-9);
        QCOMPARE(volume::meterFraction(1.0), 1.0);
        QCOMPARE(volume::meterFraction(0.0), 0.0);
    }

    void nodeSpecs()
    {
        const auto specs = engine::desiredNodes(defaults::scene());
        // phones, stream, mic, sidetone + 5 playback buses
        QCOMPARE(specs.size(), 9);
        const auto stream = std::find_if(specs.cbegin(), specs.cend(),
                                         [](const auto &s) { return s.name == QLatin1String("rostrum.stream"); });
        QVERIFY(stream != specs.cend());
        QCOMPARE(stream->description, QStringLiteral("Rostrum Stream Mix"));
        const auto mic = std::find_if(specs.cbegin(), specs.cend(),
                                      [](const auto &s) { return s.name == QLatin1String("rostrum.mic"); });
        QCOMPARE(mic->description, QStringLiteral("Rostrum Mic"));
        const auto props = mic->properties();
        QCOMPARE(props.value(QStringLiteral("media.class")), QStringLiteral("Audio/Source/Virtual"));
        QCOMPARE(props.value(QStringLiteral("object.linger")), QStringLiteral("true"));
        QCOMPARE(props.value(QStringLiteral("monitor.channel-volumes")), QStringLiteral("true"));
        QVERIFY(std::any_of(specs.cbegin(), specs.cend(),
                            [](const auto &s) { return s.name == QLatin1String("rostrum.game"); }));
    }

    void portMatching()
    {
        using pw::Port;
        auto port = [](uint32_t id, const char *ch) {
            Port p;
            p.id = id;
            p.channel = QString::fromLatin1(ch);
            return p;
        };
        // stereo -> stereo by channel
        auto pairs = pw::matchPorts({port(1, "FL"), port(2, "FR")}, {port(11, "FR"), port(10, "FL")});
        QCOMPARE(pairs.size(), 2);
        QVERIFY(pairs.contains(qMakePair(1u, 10u)));
        QVERIFY(pairs.contains(qMakePair(2u, 11u)));
        // mono -> stereo spreads
        pairs = pw::matchPorts({port(1, "MONO")}, {port(10, "FL"), port(11, "FR")});
        QCOMPARE(pairs.size(), 2);
        // stereo -> mono sums both
        pairs = pw::matchPorts({port(1, "FL"), port(2, "FR")}, {port(10, "MONO")});
        QCOMPARE(pairs.size(), 2);
        // unnamed channels fall back to index order
        pairs = pw::matchPorts({port(1, "AUX0"), port(2, "AUX1")}, {port(10, "FL"), port(11, "FR")});
        QCOMPARE(pairs, (QList<QPair<uint32_t, uint32_t>>{{1, 10}, {2, 11}}));
    }

    void monoPortMatching()
    {
        using pw::Port;
        auto port = [](uint32_t id, const char *ch) {
            Port p;
            p.id = id;
            p.channel = QString::fromLatin1(ch);
            return p;
        };
        // stereo -> stereo: each side into both
        auto pairs = pw::monoPorts({port(1, "FL"), port(2, "FR")}, {port(10, "FL"), port(11, "FR")});
        QCOMPARE(pairs.size(), 4);
        for (const auto &p :
             {qMakePair(1u, 10u), qMakePair(1u, 11u), qMakePair(2u, 10u), qMakePair(2u, 11u)}) {
            QVERIFY(pairs.contains(p));
        }
        // surround sink: only the front pair, never the centre or rears
        pairs = pw::monoPorts({port(1, "FL"), port(2, "FR")},
                              {port(10, "FL"), port(11, "FR"), port(12, "FC"), port(13, "RL")});
        QCOMPARE(pairs.size(), 4);
        QVERIFY(!pairs.contains(qMakePair(1u, 12u)));
        QVERIFY(!pairs.contains(qMakePair(2u, 13u)));
        // a mono sink already sums, and unnamed channels cannot be cross-linked safely
        QCOMPARE(pw::monoPorts({port(1, "FL"), port(2, "FR")}, {port(10, "MONO")}),
                 pw::matchPorts({port(1, "FL"), port(2, "FR")}, {port(10, "MONO")}));
        QCOMPARE(pw::monoPorts({port(1, "AUX0"), port(2, "AUX1")}, {port(10, "AUX0"), port(11, "AUX1")}),
                 (QList<QPair<uint32_t, uint32_t>>{{1, 10}, {2, 11}}));
    }

    void balance()
    {
        QCOMPARE(volume::clampBalance(0.0), 0.0);
        QCOMPARE(volume::clampBalance(-3.0), -1.0);
        QCOMPARE(volume::clampBalance(2.0), 1.0);
        QCOMPARE(volume::clampBalance(0.004), 0.0);
        QCOMPARE(volume::clampBalance(std::nan("")), 0.0);
        QCOMPARE(volume::clampBalance(std::numeric_limits<double>::infinity()), 0.0);
        // Centre leaves both sides at the fader.
        QCOMPARE(volume::balancedPosition(0.8, 0.0, false), 0.8);
        QCOMPARE(volume::balancedPosition(0.8, 0.0, true), 0.8);
        // Right turns the left side down and keeps the right side at the fader; left mirrors it.
        QVERIFY(qAbs(volume::balancedPosition(0.8, 0.5, false) - 0.4) < 1e-9);
        QCOMPARE(volume::balancedPosition(0.8, 0.5, true), 0.8);
        QCOMPARE(volume::balancedPosition(0.8, -0.5, false), 0.8);
        QVERIFY(qAbs(volume::balancedPosition(0.8, -0.5, true) - 0.4) < 1e-9);
        QCOMPARE(volume::balancedPosition(0.8, 1.0, false), 0.0);
        QCOMPARE(volume::balancedPosition(0.8, -7.0, true), 0.0);
    }

    void mergeKeepsSavedBalance()
    {
        Scene saved = defaults::scene();
        saved.bus(QStringLiteral("music"))->balance = -0.25;
        Scene current = saved;
        current.bus(QStringLiteral("music"))->balance = 0.5;
        current.bus(QStringLiteral("music"))->name = QStringLiteral("Tunes");
        const Scene merged = mergeStructure(saved, current);
        QCOMPARE(merged.bus(QStringLiteral("music"))->balance, -0.25);
        QCOMPARE(merged.bus(QStringLiteral("music"))->name, QStringLiteral("Tunes"));
    }
};

QTEST_GUILESS_MAIN(TestModel)
#include "tst_model.moc"
