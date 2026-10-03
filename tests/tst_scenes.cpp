#include "core/ScenePresets.h"
#include "core/SceneStore.h"
#include "core/SceneToml.h"
#include "core/Settings.h"
#include "engine/Engine.h"
#include "engine/SceneManager.h"
#include "pw/PwContext.h"

#include <QTemporaryDir>
#include <QTest>

using namespace rostrum;

namespace {

Scene sample()
{
    Scene s = defaults::scene(QStringLiteral("Ranked"));
    s.masterPhones = 0.8;
    s.masterStreamMuted = true;
    s.sidetoneVolume = 0.25;
    s.bus(QStringLiteral("game"))->volume = 0.42;
    s.bus(QStringLiteral("voice"))->muted = true;
    s.bus(QStringLiteral("music"))->destination = Destination::Phones;
    s.bus(QStringLiteral("alerts"))->name = QStringLiteral("Alerts \"loud\" = yes");
    s.bus(QStringLiteral("alerts"))->color = QStringLiteral("#e93a9a");
    AppRule r;
    r.match = QStringLiteral("Discord");
    r.key = MatchKey::Binary;
    r.busId = QStringLiteral("voice");
    r.volume = 0.7;
    r.muted = true;
    r.lastSeen = QDateTime(QDate(2026, 10, 2), QTime(21, 30, 5), QTimeZone::UTC);
    s.rules.append(r);
    AppRule wine;
    wine.match = QStringLiteral("wine64-preloader");
    wine.key = MatchKey::Binary;
    wine.busId = QStringLiteral("game");
    wine.label = QStringLiteral("Elden Ring");
    s.rules.append(wine);
    s.rules.append({QStringLiteral("Firefox"), MatchKey::Name, QStringLiteral("music")});
    return s;
}

} // namespace

class TestScenes : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void roundTrip()
    {
        const Scene s = sample();
        const QString text = toml_io::serializeScene(s);
        QString err;
        const auto back = toml_io::parseScene(text, &err);
        QVERIFY2(back.has_value(), qPrintable(err));
        QCOMPARE(*back, s);
        QCOMPARE(back->rules.first().lastSeen, s.rules.first().lastSeen);
        QCOMPARE(back->rules.at(1).label, QStringLiteral("Elden Ring"));
        QVERIFY(!text.contains(QStringLiteral("label = \"\"")));
        QCOMPARE(back->bus(QStringLiteral("alerts"))->name, QStringLiteral("Alerts \"loud\" = yes"));
        // Serializing the parsed scene gives identical text (stable output).
        QCOMPARE(toml_io::serializeScene(*back), text);
    }

    void levelsAreWrittenShort()
    {
        Scene s = defaults::scene();
        s.bus(QStringLiteral("game"))->volume = 0.8;
        const QString text = toml_io::serializeScene(s);
        QVERIFY2(text.contains(QStringLiteral("volume = 0.8\n")), qPrintable(text));
        QCOMPARE(toml_io::parseScene(text)->bus(QStringLiteral("game"))->volume, 0.8);
    }

    void neverWritesSolo()
    {
        const QString text = toml_io::serializeScene(sample());
        QVERIFY(!text.contains(QStringLiteral("solo"), Qt::CaseInsensitive));
        const QString bundle = toml_io::serializeBundle({sample(), defaults::scene()});
        QVERIFY(!bundle.contains(QStringLiteral("solo"), Qt::CaseInsensitive));
    }

    void soloDoesNotChangeSavedScene()
    {
        QTemporaryDir dir;
        pw::PwContext pw; // never started: no audio server needed
        engine::Engine engine(&pw);
        engine::SceneManager scenes(&engine, dir.path());
        scenes.load(QStringLiteral("Live"));
        QVERIFY(scenes.save());
        const QString file = SceneStore(dir.path()).fileFor(QStringLiteral("Live"));
        const QString before = SceneStore::readFile(file, nullptr);

        engine.setSolo(QStringLiteral("game"), true);
        engine.setSolo(QStringLiteral("music"), true);
        QVERIFY(engine.dimmedBySolo(QStringLiteral("voice")));
        QVERIFY(!engine.dimmedBySolo(QStringLiteral("game")));
        QVERIFY(!engine.dimmedBySolo(QStringLiteral("mic")));
        QVERIFY(!scenes.dirty());

        QVERIFY(scenes.save());
        const QString after = SceneStore::readFile(file, nullptr);
        QCOMPARE(after, before);
        QVERIFY(!after.contains(QStringLiteral("solo"), Qt::CaseInsensitive));
        // Every bus keeps the user's own mute flag; solo did not turn into saved mutes.
        const auto parsed = toml_io::parseScene(after);
        for (const auto &b : parsed->buses) {
            QVERIFY(!b.muted);
        }
    }

    void sanitizeRepairs()
    {
        const QString text = QStringLiteral(R"(
name = "Broken"
[[bus]]
id = "game"
name = "Game"
color = "not-a-color"
volume = 4.0
destination = "sideways"
[[bus]]
id = "game"
name = "Duplicate"
[[bus]]
id = "stream"
name = "Reserved id"
[[rule]]
match = "Ghost"
bus = "missing"
[[rule]]
match = "Discord"
key = "binary"
bus = "game"
[[rule]]
match = "discord"
key = "binary"
bus = "game"
)");
        const auto s = toml_io::parseScene(text);
        QVERIFY(s.has_value());
        QCOMPARE(s->buses.size(), 2); // mic inserted first, duplicate and reserved ids dropped
        QCOMPARE(s->buses.first().id, QStringLiteral("mic"));
        QVERIFY(s->buses.first().isInput());
        const Bus *game = s->bus(QStringLiteral("game"));
        QCOMPARE(game->volume, 1.0);
        QCOMPARE(game->destination, Destination::Both);
        QVERIFY(game->color.startsWith(QLatin1Char('#')) && game->color.size() == 7);
        QCOMPARE(s->rules.size(), 1);
    }

    void capsBuses()
    {
        Scene s = defaults::scene();
        for (int i = 0; i < 20; ++i) {
            Bus b;
            b.id = QStringLiteral("extra-%1").arg(i);
            b.name = b.id;
            s.buses.append(b);
        }
        QCOMPARE(toml_io::sanitize(s).buses.size(), kMaxBuses);
    }

    void invalidToml()
    {
        QString err;
        QVERIFY(!toml_io::parseScene(QStringLiteral("name = [unterminated"), &err).has_value());
        QVERIFY(err.startsWith(QStringLiteral("Line 1")));
    }

    void bundleRoundTrip()
    {
        const QList<Scene> scenes = {sample(), defaults::scene(QStringLiteral("Just Chatting")),
                                     defaults::scene(QStringLiteral("BRB"))};
        const auto back = toml_io::parseBundle(toml_io::serializeBundle(scenes));
        QCOMPARE(back, scenes);
        QString err;
        QVERIFY(toml_io::parseBundle(QStringLiteral("format = 1"), &err).isEmpty());
        QVERIFY(!err.isEmpty());
    }

    void storeSkipsBadFiles()
    {
        QTemporaryDir dir;
        SceneStore store(dir.path());
        QVERIFY(store.save(sample()));
        QVERIFY(SceneStore::writeFile(dir.path() + QStringLiteral("/bad.toml"), QStringLiteral("x = ["), nullptr));
        QStringList errors;
        const auto all = store.loadAll(&errors);
        QCOMPARE(all.size(), 1);
        QCOMPARE(errors.size(), 1);
        QVERIFY(SceneStore(dir.path() + QStringLiteral("/missing")).loadAll().isEmpty());
    }

    void structurePersistsWithoutSavingFaders()
    {
        QTemporaryDir dir;
        pw::PwContext pw;
        engine::Engine engine(&pw);
        engine::SceneManager scenes(&engine, dir.path());
        scenes.load(QStringLiteral("Live"));

        engine.setBusVolume(QStringLiteral("game"), 0.2);
        QVERIFY(scenes.dirty());
        engine.renameBus(QStringLiteral("game"), QStringLiteral("Ranked"));
        engine.recolorBus(QStringLiteral("game"), QStringLiteral("#16a085"));
        engine.assignApp({MatchKey::Name, QStringLiteral("Firefox")}, QStringLiteral("music"), true);

        const auto onDisk = SceneStore(dir.path()).loadAll().first();
        QCOMPARE(onDisk.bus(QStringLiteral("game"))->name, QStringLiteral("Ranked"));
        QCOMPARE(onDisk.bus(QStringLiteral("game"))->color, QStringLiteral("#16a085"));
        QCOMPARE(onDisk.bus(QStringLiteral("game"))->volume, 1.0);
        QCOMPARE(onDisk.rules.size(), 1);
        QVERIFY(scenes.dirty()); // the fader move is still unsaved

        // Switching scenes discards the unsaved fader move but keeps the structure.
        QVERIFY(scenes.switchTo(QStringLiteral("Live")));
        QCOMPARE(engine.scene().bus(QStringLiteral("game"))->volume, 1.0);
        QCOMPARE(engine.scene().bus(QStringLiteral("game"))->name, QStringLiteral("Ranked"));
        QVERIFY(!scenes.dirty());
    }

    void sceneManagement()
    {
        QTemporaryDir dir;
        pw::PwContext pw;
        engine::Engine engine(&pw);
        engine::SceneManager scenes(&engine, dir.path());
        scenes.load(QStringLiteral("Live"));
        QCOMPARE(scenes.names(), QStringList{QStringLiteral("Live")});
        QVERIFY(scenes.saveAs(QStringLiteral("Just Chatting")));
        QCOMPARE(scenes.currentName(), QStringLiteral("Just Chatting"));
        QVERIFY(scenes.duplicate(QStringLiteral("Live")));
        QVERIFY(scenes.names().contains(QStringLiteral("Live copy")));
        QVERIFY(!scenes.rename(QStringLiteral("Live copy"), QStringLiteral("live")));
        QVERIFY(scenes.rename(QStringLiteral("Live copy"), QStringLiteral("BRB")));
        scenes.setDefault(QStringLiteral("BRB"));
        QVERIFY(scenes.remove(QStringLiteral("BRB")));
        QCOMPARE(scenes.defaultName(), QStringLiteral("Live"));
        QVERIFY(scenes.next());
        QVERIFY(scenes.previous());

        const QString bundle = dir.path() + QStringLiteral("/backup.toml");
        QVERIFY(scenes.exportTo(bundle));
        QCOMPARE(scenes.importFrom(bundle), 2); // imported under unique names, never overwriting
        QCOMPARE(scenes.names().size(), 4);

        // A fresh manager reads back what was written.
        engine::Engine engine2(&pw);
        engine::SceneManager again(&engine2, dir.path());
        again.load(QStringLiteral("Live"));
        QCOMPARE(again.names().size(), 4);
    }

    void presetsKeepStructure()
    {
        Scene base = sample();
        base.bus(QStringLiteral("mic"))->volume = 0.6;
        base.bus(QStringLiteral("game"))->name = QStringLiteral("Ranked");
        Bus custom;
        custom.id = QStringLiteral("extra");
        custom.name = QStringLiteral("Soundboard");
        custom.volume = 0.3;
        base.buses.append(custom);
        base = toml_io::sanitize(base);

        for (const QString &id : presets::ids()) {
            const Scene s = presets::apply(id, base);
            QCOMPARE(s.buses.size(), base.buses.size());
            QCOMPARE(s.rules, base.rules);
            QCOMPARE(s.sidetoneVolume, base.sidetoneVolume);
            QCOMPARE(s.bus(QStringLiteral("game"))->name, QStringLiteral("Ranked"));
            QCOMPARE(s.bus(QStringLiteral("alerts"))->color, base.bus(QStringLiteral("alerts"))->color);
            QCOMPARE(s.bus(QStringLiteral("mic"))->volume, 0.6); // mic gain is the user's, not the preset's
            QCOMPARE(s.bus(QStringLiteral("extra"))->volume, 0.3);
            QVERIFY(!s.masterStreamMuted);
            QCOMPARE(s.masterPhones, 1.0);
            QCOMPARE(toml_io::sanitize(s), s);
        }

        const Scene gaming = presets::apply(QString::fromLatin1(presets::kGaming), base);
        QVERIFY(!gaming.bus(QStringLiteral("mic"))->muted);
        QVERIFY(!gaming.bus(QStringLiteral("voice"))->muted);
        QCOMPARE(gaming.bus(QStringLiteral("game"))->volume, 1.0);
        QCOMPARE(gaming.bus(QStringLiteral("music"))->destination, Destination::Stream);

        const Scene brb = presets::apply(QString::fromLatin1(presets::kBreak), base);
        QVERIFY(brb.bus(QStringLiteral("mic"))->muted);
        QVERIFY(brb.bus(QStringLiteral("game"))->muted);
        QCOMPARE(brb.bus(QStringLiteral("voice"))->destination, Destination::Phones);

        QCOMPARE(presets::apply(QStringLiteral("nope"), base), base);
    }

    void autoSaveWritesLevels()
    {
        QTemporaryDir dir;
        pw::PwContext pw;
        engine::Engine engine(&pw);
        engine::SceneManager scenes(&engine, dir.path());
        scenes.setAutoSave(true);
        scenes.load(QStringLiteral("Live"));
        QVERIFY(scenes.saveAs(QStringLiteral("Ranked")));

        engine.setBusVolume(QStringLiteral("game"), 0.2);
        QVERIFY(scenes.dirty());
        QTRY_VERIFY(!scenes.dirty()); // after the debounce
        auto onDisk = [&](const QString &name) {
            const auto all = SceneStore(dir.path()).loadAll();
            return *std::find_if(all.begin(), all.end(), [&](const Scene &s) { return s.name == name; });
        };
        QCOMPARE(onDisk(QStringLiteral("Ranked")).bus(QStringLiteral("game"))->volume, 0.2);

        // A switch right after a move keeps the move in the scene being left.
        engine.setBusMuted(QStringLiteral("music"), true);
        QVERIFY(scenes.switchTo(QStringLiteral("Live")));
        QVERIFY(onDisk(QStringLiteral("Ranked")).bus(QStringLiteral("music"))->muted);
        QVERIFY(!onDisk(QStringLiteral("Live")).bus(QStringLiteral("music"))->muted);

        // A scene made from a preset starts from the live structure.
        Scene preset = presets::apply(QString::fromLatin1(presets::kBreak), engine.scene());
        preset.name = QStringLiteral("BRB");
        QVERIFY(scenes.create(preset));
        QVERIFY(onDisk(QStringLiteral("BRB")).bus(QStringLiteral("mic"))->muted);
        QCOMPARE(scenes.currentName(), QStringLiteral("Live"));

        // With auto-save off, moves wait for Save again.
        scenes.setAutoSave(false);
        engine.setBusVolume(QStringLiteral("voice"), 0.4);
        QTest::qWait(1300);
        QVERIFY(scenes.dirty());
        scenes.flush();
        QCOMPARE(onDisk(QStringLiteral("Live")).bus(QStringLiteral("voice"))->volume, 1.0);
    }

    void settingsRoundTrip()
    {
        Settings s = defaultSettings();
        QCOMPARE(s.hotkeys.value(QStringLiteral("mute_mic")), QStringLiteral("Meta+Alt+M"));
        QVERIFY(s.scrollToAdjust);
        QVERIFY(!s.confirmSceneSwitch);
        QVERIFY(s.autoSaveScenes);
        s.autoSaveScenes = false;
        QCOMPARE(parseSettings(serializeSettings(s)).autoSaveScenes, false);
        s.autoSaveScenes = true;
        s.wizardDone = true;
        s.headphones = QStringLiteral("alsa_output.usb-HyperX");
        s.hotkeys[QStringLiteral("next_scene")] = QString();
        s.meterSpeed = QStringLiteral("low");
        s.lastPage = QStringLiteral("apps");
        QCOMPARE(parseSettings(serializeSettings(s)), s);
        QString err;
        QCOMPARE(parseSettings(QStringLiteral("[[["), &err), defaultSettings());
        QVERIFY(!err.isEmpty());
        // Window size never drops below the minimum.
        QCOMPARE(parseSettings(QStringLiteral("[window]\nwidth = 100\nheight = 100\n")).windowWidth, 960);
    }

    void settingsPrivacyAndUpdates()
    {
        const Settings d = defaultSettings();
        QCOMPARE(d.crashReports, QStringLiteral("ask"));
        QVERIFY(d.checkUpdates);
        QCOMPARE(d.setupVersion, 0);

        Settings s = d;
        s.wizardDone = true;
        s.setupVersion = kSetupVersion;
        s.crashReports = QStringLiteral("never");
        s.checkUpdates = false;
        s.installUpdates = false;
        s.skippedVersion = QStringLiteral("0.2.0");
        s.lastUpdateCheck = 1791000000;
        QCOMPARE(parseSettings(serializeSettings(s)), s);

        // Anything but the three modes means "ask", never "send".
        QCOMPARE(parseSettings(QStringLiteral("[privacy]\ncrash_reports = 'yes'\n")).crashReports, QStringLiteral("ask"));
        // A file from before setup was versioned saw only the device steps.
        QCOMPARE(parseSettings(QStringLiteral("[general]\nwizard_done = true\n")).setupVersion, 1);
        QCOMPARE(parseSettings(QStringLiteral("[general]\nwizard_done = false\n")).setupVersion, 0);
    }
};

QTEST_GUILESS_MAIN(TestScenes)
#include "tst_scenes.moc"
