#include "core/SceneStore.h"
#include "core/SceneToml.h"
#include "core/Settings.h"
#include "core/SettingsBackup.h"
#include "engine/Engine.h"
#include "engine/SceneManager.h"
#include "pw/PwContext.h"

#include <QTemporaryDir>
#include <QTest>

using namespace rostrum;

namespace {

const QDateTime kNow(QDate(2026, 10, 3), QTime(12, 0), QTimeZone::UTC);

Settings configured()
{
    Settings s = defaultSettings();
    s.wizardDone = true;
    s.setupVersion = kSetupVersion;
    s.launchAtLogin = true;
    s.autoSaveScenes = false;
    s.confirmSceneSwitch = true;
    s.meterSpeed = QStringLiteral("low");
    s.autoSkip = {QStringLiteral("binary:obs")};
    s.crashReports = QString::fromLatin1(crashmode::kSend);
    s.checkUpdates = false;
    s.skippedVersion = QStringLiteral("0.2.0");
    s.lastUpdateCheck = 1791000000;
    s.defaultScene = QStringLiteral("Ranked");
    s.sceneOrder = {QStringLiteral("Ranked"), QStringLiteral("Live")};
    s.headphones = QStringLiteral("alsa_output.usb-HyperX_Cloud-00.analog-stereo");
    s.mic = QStringLiteral("alsa_input.usb-Shure_MV7-00.mono-fallback");
    s.hotkeys[QStringLiteral("mute_mic")] = QStringLiteral("Meta+F1");
    s.windowWidth = 1500;
    s.compactWindow = true;
    s.keepOnTop = true;
    s.closeToTray = false;
    s.minimizeToTray = true;
    s.osdFeedback = false;
    s.sceneFadeMs = 300;
    s.ducking.enabled = true;
    s.ducking.trigger = ducking::Trigger::Either;
    s.ducking.buses = {QStringLiteral("music"), QStringLiteral("alerts")};
    s.ducking.amountDb = -18;
    s.obsBackground = false;
    s.obsGoLiveWarnings = false;
    s.obsSceneMap = {{QStringLiteral("BRB"), QStringLiteral("Ranked")}};
    s.micFallback = true;
    s.monoHeadphones = true;
    return s;
}

QList<Scene> scenes()
{
    Scene live = defaults::scene(QStringLiteral("Live"));
    Scene ranked = defaults::scene(QStringLiteral("Ranked"));
    ranked.bus(QStringLiteral("game"))->volume = 0.42;
    AppRule r{QStringLiteral("Discord"), MatchKey::Binary, QStringLiteral("voice")};
    r.lastSeen = kNow.addDays(-1);
    ranked.rules.append(r);
    return {live, ranked};
}

} // namespace

class TestBackup : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void roundTrip()
    {
        const Settings s = configured();
        const QString text = backup::serialize(s, scenes(), kNow);
        QString err;
        const auto b = backup::parse(text, &err);
        QVERIFY2(b.has_value(), qPrintable(err));
        QCOMPARE(b->created, kNow);
        QCOMPARE(b->scenes.size(), 2);
        QCOMPARE(b->scenes.at(1).name, QStringLiteral("Ranked"));
        QCOMPARE(b->scenes.at(1).bus(QStringLiteral("game"))->volume, 0.42);
        QCOMPARE(b->scenes.at(1).rules, scenes().at(1).rules);

        const Settings &r = b->settings;
        QCOMPARE(r.autoSaveScenes, false);
        QCOMPARE(r.confirmSceneSwitch, true);
        QCOMPARE(r.meterSpeed, QStringLiteral("low"));
        QCOMPARE(r.autoSkip, s.autoSkip);
        QCOMPARE(r.launchAtLogin, true);
        QCOMPARE(r.defaultScene, s.defaultScene);
        QCOMPARE(r.sceneOrder, s.sceneOrder);
        QCOMPARE(r.headphones, s.headphones);
        QCOMPARE(r.mic, s.mic);
        QCOMPARE(r.hotkeys, s.hotkeys);
        QCOMPARE(r.closeToTray, false);
        QCOMPARE(r.minimizeToTray, true);
        QCOMPARE(r.osdFeedback, false);
        QCOMPARE(r.sceneFadeMs, 300);
        QCOMPARE(r.ducking, s.ducking);
        QCOMPARE(r.obsBackground, false);
        QCOMPARE(r.obsGoLiveWarnings, false);
        QCOMPARE(r.obsSceneMap, s.obsSceneMap);
        QCOMPARE(r.micFallback, true);
        QCOMPARE(r.monoHeadphones, true);

        // Restoring onto another setup takes everything the backup carries, nothing else.
        Settings other = defaultSettings();
        other.crashReports = QString::fromLatin1(crashmode::kNever);
        other.windowWidth = 1200;
        const Settings merged = backup::restoredSettings(other, r);
        Settings expected = s;
        for (bool Settings::*field :
             {&Settings::wizardDone, &Settings::launchAtLogin, &Settings::checkUpdates,
              &Settings::installUpdates, &Settings::sidebarCollapsed, &Settings::compactWindow,
              &Settings::keepOnTop}) {
            expected.*field = other.*field;
        }
        expected.setupVersion = other.setupVersion;
        expected.crashReports = other.crashReports;
        expected.skippedVersion = other.skippedVersion;
        expected.lastUpdateCheck = other.lastUpdateCheck;
        expected.windowWidth = other.windowWidth;
        expected.windowHeight = other.windowHeight;
        expected.lastPage = other.lastPage;
        expected.compactWidth = other.compactWidth;
        expected.compactHeight = other.compactHeight;
        QCOMPARE(merged, expected);
    }

    void leavesOutPrivateData()
    {
        const QString text = backup::serialize(configured(), scenes(), kNow);
        for (const char *word : {"crash_reports", "privacy", "updates", "skipped_version", "last_check",
                                 "wizard_done", "setup_version", "last_seen", "[window]", "compact",
                                 "keep_on_top", "solo"}) {
            QVERIFY2(!text.contains(QLatin1String(word), Qt::CaseInsensitive), word);
        }
        QVERIFY(!text.contains(QStringLiteral("2026-10-02"))); // the rule's last-seen date
        QVERIFY(text.contains(QStringLiteral("kind = 'rostrum-backup'")) ||
                text.contains(QStringLiteral("kind = \"rostrum-backup\"")));
    }

    void refusesOtherFiles()
    {
        QString err;
        QVERIFY(!backup::parse(QStringLiteral("[[["), &err));
        QVERIFY(!err.isEmpty());
        // A scene export is not a backup.
        QVERIFY(!backup::parse(toml_io::serializeBundle(scenes()), &err));
        QVERIFY(!backup::parse(QStringLiteral("kind = 'rostrum-backup'\nformat = 99\n"), &err));
        QVERIFY(backup::parse(QStringLiteral("kind = 'rostrum-backup'\nformat = 1\n"), &err));
    }

    void restoreScenes()
    {
        QTemporaryDir dir;
        pw::PwContext pw;
        engine::Engine engine(&pw);
        engine::SceneManager manager(&engine, dir.path() + QStringLiteral("/scenes"));
        manager.setTrashDir(dir.path() + QStringLiteral("/trash"));
        manager.load(QStringLiteral("Live"));
        QVERIFY(manager.create(QStringLiteral("Just Chatting")));
        engine.setBusVolume(QStringLiteral("game"), 0.9);
        QVERIFY(manager.save());

        const auto b = backup::parse(backup::serialize(configured(), scenes(), kNow));
        QCOMPARE(manager.restoreScenes(b->scenes), 2);
        QCOMPARE(manager.names(), (QStringList{"Live", "Just Chatting", "Ranked"}));
        // The live scene was replaced, so the mix now plays the backed-up version.
        QCOMPARE(engine.scene().bus(QStringLiteral("game"))->volume, 1.0);
        QCOMPARE(manager.currentName(), QStringLiteral("Live"));
        // The overwritten version is in the trash.
        const auto trash = manager.trash();
        QCOMPARE(trash.size(), 1);
        QCOMPARE(trash.first().name, QStringLiteral("Live"));

        // Restoring the same backup again changes nothing.
        QCOMPARE(manager.restoreScenes(b->scenes), 2);
        QCOMPARE(manager.trash().size(), 1);
    }
};

QTEST_GUILESS_MAIN(TestBackup)
#include "tst_backup.moc"
