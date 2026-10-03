#include "core/RuleExport.h"
#include "core/SceneStore.h"
#include "engine/Engine.h"
#include "engine/SceneManager.h"
#include "pw/PwContext.h"

#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

using namespace rostrum;

namespace {

Scene goldenScene()
{
    Scene s = defaults::scene(QStringLiteral("Live"));
    s.rules = {
        {QStringLiteral("Firefox"), MatchKey::Name, QStringLiteral("music")},
        {QStringLiteral("discord"), MatchKey::Binary, QStringLiteral("voice")},
        {QStringLiteral("Half-Life 2 (x64)"), MatchKey::Name, QStringLiteral("game")},
        {QStringLiteral("Bob's \"Radio\" 1.0"), MatchKey::Name, QStringLiteral("music")},
        {QStringLiteral("Ghost"), MatchKey::Name, QStringLiteral("gone")},
        {QStringLiteral("obs"), MatchKey::Binary, QStringLiteral("mic")},
    };
    return s;
}

QString readGolden(const char *name)
{
    QFile f(QStringLiteral(ROSTRUM_TEST_DATA "/") + QLatin1String(name));
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(f.readAll());
}

// Top-level "name = [" arrays in a PipeWire config file.
QStringList topLevelArrays(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    QStringList out;
    static const QRegularExpression re(QStringLiteral(R"(^([a-z.]+)\s*=\s*\[)"), QRegularExpression::MultilineOption);
    auto it = re.globalMatch(QString::fromUtf8(f.readAll()));
    while (it.hasNext()) {
        out << it.next().captured(1);
    }
    return out;
}

} // namespace

class TestRuleExport : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void regex()
    {
        QCOMPARE(rule_export::caseInsensitiveRegex(QStringLiteral("Discord")), QStringLiteral("^[Dd][Ii][Ss][Cc][Oo][Rr][Dd]$"));
        QCOMPARE(rule_export::caseInsensitiveRegex(QStringLiteral("a.b(1)")), QStringLiteral("^[Aa]\\.[Bb]\\(1\\)$"));
        // The generated pattern means the same thing to Qt's regex engine.
        const QRegularExpression re(rule_export::caseInsensitiveRegex(QStringLiteral("Half-Life 2 (x64)")));
        QVERIFY(re.match(QStringLiteral("half-life 2 (X64)")).hasMatch());
        QVERIFY(!re.match(QStringLiteral("Half-Life 2 (x64) extra")).hasMatch());
    }

    void orderAndFiltering()
    {
        const auto rules = rule_export::exportableRules(goldenScene());
        QCOMPARE(rules.size(), 4); // unknown bus and the mic bus are skipped
        QCOMPARE(rules.first().key, MatchKey::Binary); // binary first, so name rules win
        QCOMPARE(rules.at(1).match, QStringLiteral("Firefox"));
    }

    void golden_data()
    {
        QTest::addColumn<QString>("actual");
        QTest::addColumn<QString>("expected");
        QTest::newRow("pulse") << rule_export::pulseFragment(goldenScene()) << readGolden("pulse-rules.conf");
        QTest::newRow("client") << rule_export::clientFragment(goldenScene()) << readGolden("client-rules.conf");
    }

    void golden()
    {
        QFETCH(QString, actual);
        QFETCH(QString, expected);
        QVERIFY(!expected.isEmpty());
        QCOMPARE(actual, expected);
    }

    void neverPinsStreams()
    {
        for (const QString &text : {rule_export::pulseFragment(goldenScene()), rule_export::clientFragment(goldenScene())}) {
            QVERIFY(!text.contains(QStringLiteral("dont-fallback")));
            QVERIFY(!text.contains(QStringLiteral("dont-move")));
            QVERIFY(!text.contains(QStringLiteral("dont-reconnect")));
            QVERIFY(!text.contains(QStringLiteral("dont_fallback")));
            // target.object is the only property ever set.
            static const QRegularExpression props(QStringLiteral(R"(update-props = \{ ([^}]*) \})"));
            auto it = props.globalMatch(text);
            int n = 0;
            while (it.hasNext()) {
                QVERIFY(it.next().captured(1).startsWith(QStringLiteral("target.object = ")));
                ++n;
            }
            QCOMPARE(n, 4);
        }
    }

    void emptyWhenNoRules()
    {
        QVERIFY(rule_export::pulseFragment(defaults::scene()).isEmpty());
        QVERIFY(rule_export::clientFragment(defaults::scene()).isEmpty());
    }

    void arrayNamesMatchThisMachine()
    {
        const QString pulse = QStringLiteral("/usr/share/pipewire/pipewire-pulse.conf");
        const QString client = QStringLiteral("/usr/share/pipewire/client.conf");
        if (!QFile::exists(pulse) || !QFile::exists(client)) {
            QSKIP("PipeWire system config is not installed");
        }
        QVERIFY(topLevelArrays(pulse).contains(QLatin1String(rule_export::kPulseRulesArray)));
        QVERIFY(topLevelArrays(client).contains(QLatin1String(rule_export::kClientRulesArray)));
    }

    void managerExportsDefaultScene()
    {
        QTemporaryDir dir;
        const QString pulse = dir.path() + QStringLiteral("/pipewire/pipewire-pulse.conf.d/50-rostrum.conf");
        const QString client = dir.path() + QStringLiteral("/pipewire/client.conf.d/50-rostrum.conf");
        pw::PwContext pw;
        engine::Engine engine(&pw);
        engine::SceneManager scenes(&engine, dir.path() + QStringLiteral("/scenes"));
        scenes.load(QStringLiteral("Live"));
        scenes.enableRuleExport(pulse, client);
        QVERIFY(!QFile::exists(pulse));

        engine.assignApp({MatchKey::Binary, QStringLiteral("discord")}, QStringLiteral("voice"), true);
        QVERIFY(QFile::exists(pulse) && QFile::exists(client));
        QVERIFY(SceneStore::readFile(pulse, nullptr).contains(QStringLiteral("\"rostrum.voice\"")));

        // Rules of a scene that is not the default are not exported.
        QVERIFY(scenes.saveAs(QStringLiteral("Other")));
        engine.assignApp({MatchKey::Name, QStringLiteral("Spotify")}, QStringLiteral("music"), true);
        QVERIFY(!SceneStore::readFile(pulse, nullptr).contains(QStringLiteral("music")));
        scenes.setDefault(QStringLiteral("Other"));
        QVERIFY(SceneStore::readFile(pulse, nullptr).contains(QStringLiteral("\"rostrum.music\"")));

        // Removing the last rule removes the fragments.
        scenes.setDefault(QStringLiteral("Live"));
        QVERIFY(scenes.switchTo(QStringLiteral("Live")));
        engine.removeRule({MatchKey::Binary, QStringLiteral("discord")});
        QVERIFY(!QFile::exists(pulse));
        QVERIFY(!QFile::exists(client));
    }
};

QTEST_GUILESS_MAIN(TestRuleExport)
#include "tst_ruleexport.moc"
