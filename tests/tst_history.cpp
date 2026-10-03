#include "core/SceneHistory.h"
#include "core/SceneStore.h"
#include "core/SceneTrash.h"
#include "engine/Engine.h"
#include "engine/SceneManager.h"
#include "pw/PwContext.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace rostrum;

namespace {

Scene withVolume(Scene s, const QString &bus, double v)
{
    s.bus(bus)->volume = v;
    return s;
}

const QDateTime kNow(QDate(2026, 10, 3), QTime(12, 0), QTimeZone::UTC);

} // namespace

class TestHistory : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void undoRedo()
    {
        SceneHistory h;
        const Scene a = defaults::scene();
        h.reset(a);
        QVERIFY(!h.canUndo());
        QVERIFY(!h.canRedo());
        QVERIFY(!h.undo(a));

        const Scene b = withVolume(a, QStringLiteral("game"), 0.5);
        const Scene c = withVolume(b, QStringLiteral("music"), 0.2);
        QVERIFY(h.record(b));
        QVERIFY(!h.record(b)); // no change, no step
        Scene renamed = b;
        renamed.name = QStringLiteral("Other");
        QVERIFY(!h.record(renamed)); // names are not steps
        QVERIFY(h.record(c));
        QCOMPARE(h.undoChange(), (SceneHistory::Change{SceneHistory::Kind::Volume, QStringLiteral("Music")}));

        QCOMPARE(*h.undo(c), b);
        QCOMPARE(*h.undo(b), a);
        QVERIFY(!h.canUndo());
        QCOMPARE(h.redoChange(), (SceneHistory::Change{SceneHistory::Kind::Volume, QStringLiteral("Game")}));
        QCOMPARE(*h.redo(a), b);

        // A new edit after undo drops the redo steps.
        const Scene d = withVolume(b, QStringLiteral("voice"), 0.1);
        QVERIFY(h.record(d));
        QVERIFY(!h.canRedo());
        QCOMPARE(*h.undo(d), b);
    }

    void undoKeepsLiveNameAndMicMute()
    {
        SceneHistory h;
        Scene a = defaults::scene(QStringLiteral("Live"));
        h.reset(a);
        Scene muted = a;
        muted.micBus()->muted = true;
        QVERIFY(!h.record(muted)); // muting the mic is not a step

        Scene b = withVolume(muted, QStringLiteral("game"), 0.3);
        QVERIFY(h.record(b));
        b.name = QStringLiteral("Renamed");
        const Scene back = *h.undo(b);
        QCOMPARE(back.name, QStringLiteral("Renamed"));
        QVERIFY(back.micBus()->muted); // undo never unmutes the mic
        QCOMPARE(back.bus(QStringLiteral("game"))->volume, 1.0);
    }

    void limit()
    {
        SceneHistory h;
        Scene s = defaults::scene();
        h.reset(s);
        for (int i = 1; i <= SceneHistory::kLimit + 20; ++i) {
            s.masterPhones = i / 1000.0;
            QVERIFY(h.record(s));
        }
        QCOMPARE(h.size(), SceneHistory::kLimit + 1);
        int undos = 0;
        while (h.undo(s)) {
            ++undos;
        }
        QCOMPARE(undos, SceneHistory::kLimit);
    }

    void describe()
    {
        using K = SceneHistory::Kind;
        const Scene a = defaults::scene();
        auto change = [&](auto edit) {
            Scene b = a;
            edit(b);
            return SceneHistory::describe(a, b).kind;
        };
        QCOMPARE(change([](Scene &) {}), K::None);
        QCOMPARE(change([](Scene &s) { s.bus(QStringLiteral("game"))->muted = true; }), K::Mute);
        QCOMPARE(change([](Scene &s) { s.bus(QStringLiteral("game"))->destination = Destination::Phones; }),
                 K::Destination);
        QCOMPARE(change([](Scene &s) { s.masterStreamMuted = true; }), K::StreamMaster);
        QCOMPARE(change([](Scene &s) { s.masterPhones = 0.5; }), K::PhonesMaster);
        QCOMPARE(change([](Scene &s) { s.sidetoneVolume = 0.4; }), K::Sidetone);
        QCOMPARE(change([](Scene &s) { s.bus(QStringLiteral("game"))->name = QStringLiteral("Ranked"); }),
                 K::RenameBus);
        QCOMPARE(change([](Scene &s) { s.bus(QStringLiteral("game"))->color = QStringLiteral("#000000"); }),
                 K::BusColor);
        QCOMPARE(change([](Scene &s) { s.buses.move(1, 2); }), K::BusOrder);
        QCOMPARE(change([](Scene &s) { s.bus(QStringLiteral("music"))->balance = -0.5; }), K::Balance);
        QCOMPARE(change([](Scene &s) {
                     s.rules.append({QStringLiteral("Discord"), MatchKey::Name, QStringLiteral("voice")});
                 }),
                 K::AppRules);
        QCOMPARE(change([](Scene &s) {
                     Bus b;
                     b.id = QStringLiteral("sfx");
                     b.name = QStringLiteral("SFX");
                     s.buses.append(b);
                 }),
                 K::AddBus);
        QCOMPARE(change([](Scene &s) {
                     s.bus(QStringLiteral("game"))->muted = true;
                     s.masterPhones = 0.2;
                 }),
                 K::Several);
        // Removing a bus takes its rules with it; still one step.
        Scene withRule = a;
        withRule.rules.append({QStringLiteral("Steam"), MatchKey::Name, QStringLiteral("game")});
        Scene removed = withRule;
        removed.buses.removeIf([](const Bus &b) { return b.id == QLatin1String("game"); });
        removed.removeRulesForBus(QStringLiteral("game"));
        QCOMPARE(SceneHistory::describe(withRule, removed),
                 (SceneHistory::Change{K::RemoveBus, QStringLiteral("Game")}));
        Scene appMuted = withRule;
        appMuted.rules.first().muted = true;
        QCOMPARE(SceneHistory::describe(withRule, appMuted).kind, K::AppMute);
        appMuted.rules.first().busId = QStringLiteral("music");
        QCOMPARE(SceneHistory::describe(withRule, appMuted).kind, K::AppRules);
    }

    void undoCoversBalanceAndAppMutes()
    {
        SceneHistory h;
        Scene a = defaults::scene();
        a.rules.append({QStringLiteral("Firefox"), MatchKey::Name, QStringLiteral("music")});
        h.reset(a);
        Scene b = a;
        b.bus(QStringLiteral("music"))->balance = 0.4;
        b.rules.first().muted = true;
        QVERIFY(h.record(b));
        const Scene back = *h.undo(b);
        QCOMPARE(back.bus(QStringLiteral("music"))->balance, 0.0);
        QVERIFY(!back.rules.first().muted);
        QCOMPARE(*h.redo(back), b);
    }

    void engineRestoreLeavesSessionStateAlone()
    {
        pw::PwContext pw;
        engine::Engine engine(&pw);
        const Scene before = defaults::scene();
        engine.setScene(before);
        engine.setSceneFadeMs(1000);
        engine.setScene(withVolume(before, QStringLiteral("game"), 0.2), true);
        QVERIFY(engine.fading());
        engine.setPanic(true);
        engine.setPushToTalk(true);

        engine.restoreScene(before);
        QVERIFY(!engine.fading()); // no ramp toward the levels just undone
        QVERIFY(engine.panic());
        QVERIFY(engine.pushToTalk());
        QVERIFY(engine.effectiveMicMuted());
        QVERIFY(engine.effectiveStreamMuted());
        QCOMPARE(engine.scene(), before); // holds never reach the scene
    }

    void engineRestoreKeepsSolo()
    {
        pw::PwContext pw;
        engine::Engine engine(&pw);
        engine.setScene(defaults::scene());
        const Scene before = engine.scene();
        const QString sfx = engine.addBus(QStringLiteral("SFX"));
        engine.setSolo(QStringLiteral("game"), true);
        engine.setSolo(sfx, true);

        QSignalSpy structure(&engine, &engine::Engine::structureChanged);
        QSignalSpy scene(&engine, &engine::Engine::sceneChanged);
        engine.restoreScene(before);
        QCOMPARE(engine.scene(), before);
        QCOMPARE(structure.count(), 1);
        QVERIFY(scene.count() >= 1);
        QVERIFY(engine.isSoloed(QStringLiteral("game")));
        QVERIFY(!engine.isSoloed(sfx)); // gone with its bus

        structure.clear();
        engine.restoreScene(withVolume(before, QStringLiteral("game"), 0.4));
        QCOMPARE(structure.count(), 0); // levels only
        QVERIFY(engine.isSoloed(QStringLiteral("game")));
    }

    void trashFiles()
    {
        QTemporaryDir dir;
        SceneTrash trash(dir.path() + QStringLiteral("/trash"));
        QVERIFY(trash.entries().isEmpty());
        const Scene a = withVolume(defaults::scene(QStringLiteral("Ranked")), QStringLiteral("game"), 0.3);
        const QString older = trash.put(a, kNow.addDays(-31));
        const QString newer = trash.put(defaults::scene(QStringLiteral("Ranked")), kNow);
        QVERIFY(!older.isEmpty());
        QVERIFY(!newer.isEmpty());
        QVERIFY(older != newer);
        QVERIFY(trash.put(a, kNow) != newer); // same name, same moment: still two files

        const auto entries = trash.entries();
        QCOMPARE(entries.size(), 3);
        QCOMPARE(entries.last().file, older); // newest first
        QCOMPARE(entries.last().deleted, kNow.addDays(-31));
        QCOMPARE(entries.last().name, QStringLiteral("Ranked"));

        // A stray file without a time stamp is never touched.
        QVERIFY(SceneStore::writeFile(trash.dir() + QStringLiteral("/notes.toml"), QStringLiteral("x = 1\n"),
                                      nullptr));
        QCOMPARE(trash.purge(kNow), 1);
        QVERIFY(!QFile::exists(older));
        QVERIFY(QFile::exists(trash.dir() + QStringLiteral("/notes.toml")));
        QCOMPARE(trash.entries().size(), 2);

        const auto back = trash.take(newer);
        QVERIFY(back.has_value());
        QCOMPARE(back->name, QStringLiteral("Ranked"));
        QVERIFY(!QFile::exists(newer));
        QVERIFY(!trash.take(newer));
        QVERIFY(!trash.take(dir.path() + QStringLiteral("/elsewhere.toml")));
    }

    void deleteGoesToTrash()
    {
        QTemporaryDir dir;
        pw::PwContext pw;
        engine::Engine engine(&pw);
        engine::SceneManager scenes(&engine, dir.path() + QStringLiteral("/scenes"));
        scenes.setTrashDir(dir.path() + QStringLiteral("/trash"));
        scenes.load(QStringLiteral("Live"));
        for (const auto &name : {QStringLiteral("Chat"), QStringLiteral("BRB")}) {
            QVERIFY(scenes.create(name));
        }
        scenes.setDefault(QStringLiteral("Chat"));
        QSignalSpy trashChanged(&scenes, &engine::SceneManager::trashChanged);

        QVERIFY(scenes.remove(QStringLiteral("Chat")));
        QCOMPARE(trashChanged.count(), 1);
        QVERIFY(!QFile::exists(
            SceneStore(dir.path() + QStringLiteral("/scenes")).fileFor(QStringLiteral("Chat"))));
        QCOMPARE(scenes.trash().size(), 1);
        QCOMPARE(scenes.defaultName(), QStringLiteral("Live"));

        // Undo puts it back where it was, as the default again.
        QCOMPARE(scenes.restore(scenes.lastTrashed()), QStringLiteral("Chat"));
        QCOMPARE(scenes.names(), (QStringList{"Live", "Chat", "BRB"}));
        QCOMPARE(scenes.defaultName(), QStringLiteral("Chat"));
        QVERIFY(scenes.trash().isEmpty());
        QVERIFY(scenes.lastTrashed().isEmpty());

        // An older entry comes back at the end, under a free name.
        QVERIFY(scenes.remove(QStringLiteral("BRB")));
        const QString brb = scenes.lastTrashed();
        QVERIFY(scenes.create(QStringLiteral("BRB")));
        QVERIFY(scenes.remove(QStringLiteral("Live")));
        QCOMPARE(scenes.trash().size(), 2);
        QCOMPARE(scenes.restore(brb), QStringLiteral("BRB 2"));
        QCOMPARE(scenes.names(), (QStringList{"Chat", "BRB", "BRB 2"}));

        // Old entries are purged when the trash is opened.
        SceneTrash(dir.path() + QStringLiteral("/trash"))
            .put(defaults::scene(QStringLiteral("Ancient")), QDateTime::currentDateTimeUtc().addDays(-40));
        QCOMPARE(scenes.trash().size(), 2);
        scenes.setTrashDir(dir.path() + QStringLiteral("/trash"));
        QCOMPARE(scenes.trash().size(), 1);
    }

    void deleteWithoutTrashIsPermanent()
    {
        QTemporaryDir dir;
        pw::PwContext pw;
        engine::Engine engine(&pw);
        engine::SceneManager scenes(&engine, dir.path() + QStringLiteral("/scenes"));
        scenes.load(QStringLiteral("Live"));
        QVERIFY(scenes.create(QStringLiteral("Chat")));
        QVERIFY(scenes.remove(QStringLiteral("Chat")));
        QVERIFY(scenes.lastTrashed().isEmpty());
        QVERIFY(!QDir(dir.path() + QStringLiteral("/trash")).exists());
    }
};

QTEST_GUILESS_MAIN(TestHistory)
#include "tst_history.moc"
