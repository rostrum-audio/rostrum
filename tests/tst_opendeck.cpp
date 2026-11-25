#include "core/OpenDeck.h"
#include "core/Settings.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace rostrum;
using namespace rostrum::opendeck;

class TestOpenDeck : public QObject
{
    Q_OBJECT
    static void write(const QString &path, const QByteArray &bytes)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
    }
    static QByteArray read(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
private Q_SLOTS:
    void freshInstallAndSecondClick()
    {
        QTemporaryDir temp;
        const QString plugins = temp.path() + QStringLiteral("/plugins");
        QVERIFY(QDir().mkdir(plugins));
        const Target target{QStringLiteral("custom"), plugins};
        QCOMPARE(inspect(target), State::Absent);
        auto result = install(target, true);
        QCOMPARE(result.error, Error::None);
        QVERIFY(result.changed);
        QCOMPARE(inspect(target), State::Matches);
        const QString installed = plugins + QLatin1Char('/') + pluginId();
        QCOMPARE(read(installed + QStringLiteral("/plugin.py")),
                 read(QStringLiteral(":/opendeck/plugin.py")));
        QVERIFY(QFileInfo(installed + QStringLiteral("/plugin.sh")).isExecutable());
        int renames = 0;
        result = install(target, true, [&renames](const QString &, const QString &) {
            ++renames;
            return false;
        });
        QCOMPARE(result.error, Error::None);
        QVERIFY(!result.changed);
        QCOMPARE(renames, 0);
    }
    void bundledSoundsAreInstalledAndRepairable()
    {
        QTemporaryDir temp;
        const Target target{QStringLiteral("custom"), temp.path()};
        QCOMPARE(install(target, true).error, Error::None);
        const QString root = temp.path() + QLatin1Char('/') + pluginId();
        for (const auto &name : {QStringLiteral("mic-muted.wav"), QStringLiteral("mic-live.wav")}) {
            const auto bundled = read(QStringLiteral(":/opendeck/sounds/") + name);
            QVERIFY(bundled.startsWith("RIFF"));
            QCOMPARE(read(root + QStringLiteral("/sounds/") + name), bundled);
        }
        QVERIFY(QFile::remove(root + QStringLiteral("/sounds/mic-live.wav")));
        QCOMPARE(inspect(target), State::Incomplete);
        QCOMPARE(install(target, true).error, Error::None);
        QCOMPARE(inspect(target), State::Matches);
    }
    void updateAndRepair()
    {
        QTemporaryDir temp;
        Target target{QStringLiteral("custom"), temp.path()};
        QCOMPARE(install(target, true).error, Error::None);
        const QString manifest =
            temp.path() + QLatin1Char('/') + pluginId() + QStringLiteral("/manifest.json");
        auto object = QJsonDocument::fromJson(read(manifest)).object();
        object.insert(QStringLiteral("Version"), QStringLiteral("0.9.0"));
        write(manifest, QJsonDocument(object).toJson());
        QCOMPARE(inspect(target), State::Newer);
        QVERIFY(install(target, true).changed);
        QCOMPARE(inspect(target), State::Matches);
        QVERIFY(
            QFile::remove(temp.path() + QLatin1Char('/') + pluginId() + QStringLiteral("/images/mic.svg")));
        QCOMPARE(inspect(target), State::Incomplete);
        QCOMPARE(install(target, true).error, Error::None);
        QCOMPARE(inspect(target), State::Matches);
    }
    void failedReplaceKeepsPreviousAndNeighbours()
    {
        QTemporaryDir temp;
        Target target{QStringLiteral("custom"), temp.path()};
        QCOMPARE(install(target, true).error, Error::None);
        const QString installed = temp.path() + QLatin1Char('/') + pluginId();
        const QString oldFile = installed + QStringLiteral("/plugin.py");
        write(oldFile, QByteArray("previous copy"));
        QVERIFY(QDir().mkdir(temp.path() + QStringLiteral("/other.sdPlugin")));
        write(temp.path() + QStringLiteral("/other.sdPlugin/keep"), QByteArray("other plugin"));
        write(temp.path() + QStringLiteral("/profiles.json"), QByteArray("profile assignments"));
        auto result = install(target, true, [installed](const QString &from, const QString &to) {
            if (from.endsWith(QLatin1String("/new")) && to == installed)
                return false;
            return QDir().rename(from, to);
        });
        QCOMPARE(result.error, Error::ReplaceFailed);
        QCOMPARE(read(oldFile), QByteArray("previous copy"));
        QCOMPARE(read(temp.path() + QStringLiteral("/other.sdPlugin/keep")), QByteArray("other plugin"));
        QCOMPARE(read(temp.path() + QStringLiteral("/profiles.json")), QByteArray("profile assignments"));
    }
    void customFolderIsStickyAndMissingIsAnError()
    {
        QTemporaryDir temp;
        const QString native = temp.path() + QStringLiteral("/config/opendeck");
        QVERIFY(QDir().mkpath(native));
        const QString custom = temp.path() + QStringLiteral("/My Deck plugins");
        QVERIFY(QDir().mkdir(custom));
        const auto targets = findTargets(temp.path(), temp.path() + QStringLiteral("/config"), custom);
        QCOMPARE(targets.size(), 1);
        QCOMPARE(targets.first().plugins, custom);
        QVERIFY(install(targets.first(), true).changed);
        const QString missing = temp.path() + QStringLiteral("/gone");
        const auto missingTargets =
            findTargets(temp.path(), temp.path() + QStringLiteral("/config"), missing);
        QCOMPARE(missingTargets.size(), 1);
        QCOMPARE(missingTargets.first().plugins, missing);
        QCOMPARE(install(missingTargets.first(), true).error, Error::MissingFolder);
        QVERIFY(!QFileInfo::exists(native + QStringLiteral("/plugins")));
    }
    void nativeAndFlatpakChoice()
    {
        QTemporaryDir temp;
        const QString config = temp.path() + QStringLiteral("/xdg");
        const QString native = config + QStringLiteral("/opendeck");
        const QString flatpak =
            temp.path() + QStringLiteral("/.var/app/me.amankhanna.opendeck/config/opendeck");
        QVERIFY(QDir().mkpath(native));
        QVERIFY(QDir().mkpath(flatpak));
        const auto targets = findTargets(temp.path(), config);
        QCOMPARE(targets.size(), 2);
        QCOMPARE(selectTarget(targets, QString()).plugins, QString());
        QCOMPARE(selectTarget(targets, QStringLiteral("native")).plugins,
                 native + QStringLiteral("/plugins"));
        QCOMPARE(selectTarget(targets, QStringLiteral("flatpak")).plugins,
                 flatpak + QStringLiteral("/plugins"));
        QVERIFY(install(selectTarget(targets, QStringLiteral("flatpak")), true).changed);
        QVERIFY(!QFileInfo::exists(native + QStringLiteral("/plugins")));
        QVERIFY(findTargets(temp.path(), temp.path() + QStringLiteral("/other")).size() == 1);
    }
    void noPythonAndNoTarget()
    {
        QTemporaryDir temp;
        Target target{QStringLiteral("custom"), temp.path()};
        QCOMPARE(install(target, false).error, Error::NoPython);
        QVERIFY(!QFileInfo::exists(temp.path() + QLatin1Char('/') + pluginId()));
        QCOMPARE(install(Target{}, true).error, Error::ChooseTarget);
        QCOMPARE(inspect(Target{}), State::NotFound);
    }
    void symlinkTargetIsNotFollowed()
    {
        QTemporaryDir temp, outside;
        QVERIFY(QFile::link(outside.path(), temp.path() + QLatin1Char('/') + pluginId()));
        Target target{QStringLiteral("custom"), temp.path()};
        QCOMPARE(install(target, true).error, Error::UnsafeTarget);
        QVERIFY(QDir(outside.path()).isEmpty());
    }
    void relativeCustomFolderIsRejected()
    {
        QTemporaryDir temp;
        const QString previous = QDir::currentPath();
        QVERIFY(QDir().mkdir(temp.path() + QStringLiteral("/relative-plugins")));
        QVERIFY(QDir::setCurrent(temp.path()));
        const Target target{QStringLiteral("custom"), QStringLiteral("relative-plugins")};
        const auto result = install(target, true);
        QVERIFY(QDir::setCurrent(previous));
        QCOMPARE(result.error, Error::MissingFolder);
    }
    void corruptLauncherNeedsRepair()
    {
        QTemporaryDir temp;
        Target target{QStringLiteral("custom"), temp.path()};
        QCOMPARE(install(target, true).error, Error::None);
        const QString launcher = temp.path() + QLatin1Char('/') + pluginId() + QStringLiteral("/plugin.sh");
        write(launcher, QByteArray("broken launcher"));
        QCOMPARE(inspect(target), State::Incomplete);
        QCOMPARE(install(target, true).error, Error::None);
        QCOMPARE(inspect(target), State::Matches);
    }
    void installedNewerSkipsFileMatch()
    {
        QTemporaryDir temp;
        Target target{QStringLiteral("custom"), temp.path()};
        QCOMPARE(install(target, true).error, Error::None);
        const QString root = temp.path() + QLatin1Char('/') + pluginId();
        auto object = QJsonDocument::fromJson(read(root + QStringLiteral("/manifest.json"))).object();
        // Simulate a future plugin (e.g. 1.4) with a renamed asset missing from this bundle's files().
        object.insert(QStringLiteral("Version"), QStringLiteral("99.0.0"));
        write(root + QStringLiteral("/manifest.json"), QJsonDocument(object).toJson());
        QVERIFY(QFile::remove(root + QStringLiteral("/images/mic.svg")));
        QCOMPARE(inspect(target), State::InstalledNewer);
    }
    void installNeverDowngradesANewerPlugin()
    {
        // The button is disabled, but a stale page or a race must not replace a newer copy.
        QTemporaryDir temp;
        Target target{QStringLiteral("custom"), temp.path()};
        QCOMPARE(install(target, true).error, Error::None);
        const QString manifest =
            temp.path() + QLatin1Char('/') + pluginId() + QStringLiteral("/manifest.json");
        auto object = QJsonDocument::fromJson(read(manifest)).object();
        object.insert(QStringLiteral("Version"), QStringLiteral("99.0.0"));
        write(manifest, QJsonDocument(object).toJson());
        const QByteArray newer = read(manifest);
        QCOMPARE(inspect(target), State::InstalledNewer);
        const auto result = install(target, true);
        QCOMPARE(result.error, Error::None);
        QVERIFY(!result.changed);
        QCOMPARE(read(manifest), newer);
        QCOMPARE(inspect(target), State::InstalledNewer);
    }
    void releasedOneTwoPluginOffersUpdate()
    {
        // The shape 0.4.0 installed: Version 1.2.0, four actions, no dbus_client.py and none
        // of the 1.3.0 art. That is an upgrade, not a repair.
        QTemporaryDir temp;
        Target target{QStringLiteral("custom"), temp.path()};
        QCOMPARE(install(target, true).error, Error::None);
        const QString root = temp.path() + QLatin1Char('/') + pluginId();
        auto object = QJsonDocument::fromJson(read(root + QStringLiteral("/manifest.json"))).object();
        object.insert(QStringLiteral("Version"), QStringLiteral("1.2.0"));
        QJsonArray actions;
        for (const auto &action : object.value(QStringLiteral("Actions")).toArray()) {
            const QString uuid = action.toObject().value(QStringLiteral("UUID")).toString();
            if (!uuid.endsWith(QLatin1String(".studioVoice")) && !uuid.endsWith(QLatin1String(".pushToTalk")) &&
                !uuid.endsWith(QLatin1String(".volumeDown")) && !uuid.endsWith(QLatin1String(".volumeUp")))
                actions.append(action);
        }
        QCOMPARE(actions.size(), 4);
        object.insert(QStringLiteral("Actions"), actions);
        write(root + QStringLiteral("/manifest.json"), QJsonDocument(object).toJson());
        for (const char *name : {"dbus_client.py", "images/panic-active.svg", "images/scene-active.svg",
                                 "images/bus-muted.svg", "images/studio-voice.svg",
                                 "images/studio-voice-off.svg", "images/push-to-talk.svg",
                                 "images/push-to-talk-active.svg"})
            QVERIFY(QFile::remove(root + QLatin1Char('/') + QLatin1String(name)));
        QCOMPARE(inspect(target), State::Newer);
        QVERIFY(install(target, true).changed);
        QCOMPARE(inspect(target), State::Matches);
    }
    void releasedOneThreePluginOffersUpdate()
    {
        // The shape 0.5 installed: Version 1.3.0, six actions, none of the volume art.
        QTemporaryDir temp;
        Target target{QStringLiteral("custom"), temp.path()};
        QCOMPARE(install(target, true).error, Error::None);
        const QString root = temp.path() + QLatin1Char('/') + pluginId();
        auto object = QJsonDocument::fromJson(read(root + QStringLiteral("/manifest.json"))).object();
        object.insert(QStringLiteral("Version"), QStringLiteral("1.3.0"));
        QJsonArray actions;
        for (const auto &action : object.value(QStringLiteral("Actions")).toArray()) {
            const QString uuid = action.toObject().value(QStringLiteral("UUID")).toString();
            if (!uuid.endsWith(QLatin1String(".volumeDown")) && !uuid.endsWith(QLatin1String(".volumeUp")))
                actions.append(action);
        }
        QCOMPARE(actions.size(), 6);
        object.insert(QStringLiteral("Actions"), actions);
        write(root + QStringLiteral("/manifest.json"), QJsonDocument(object).toJson());
        for (const char *name : {"images/volume-down.svg", "images/volume-down-muted.svg", "images/volume-up.svg",
                                 "images/volume-up-muted.svg"})
            QVERIFY(QFile::remove(root + QLatin1Char('/') + QLatin1String(name)));
        QCOMPARE(inspect(target), State::Newer);
        QVERIFY(install(target, true).changed);
        QCOMPARE(inspect(target), State::Matches);
    }
    void sixActionsDoNotForceIncompleteByCount()
    {
        // After Task 6 ships six bundled actions, Matches covers the happy path.
        // This test proves an older tree that *only* differs by Actions length is not
        // rejected in identityValid: bump Actions on an installed copy that still
        // byte-matches every file except manifest.json → Incomplete from mismatch,
        // not from actions.size()!=4 (which used to fail valid() before inspect ran).
        QTemporaryDir temp;
        Target target{QStringLiteral("custom"), temp.path()};
        QCOMPARE(install(target, true).error, Error::None);
        const QString root = temp.path() + QLatin1Char('/') + pluginId();
        auto object = QJsonDocument::fromJson(read(root + QStringLiteral("/manifest.json"))).object();
        auto actions = object.value(QStringLiteral("Actions")).toArray();
        QVERIFY(actions.size() >= 1);
        actions.append(actions.first());
        object.insert(QStringLiteral("Actions"), actions);
        write(root + QStringLiteral("/manifest.json"), QJsonDocument(object).toJson());
        QCOMPARE(inspect(target), State::Incomplete);
        // Identity still parses: re-read Name/CodePathLin/Version would succeed;
        // Incomplete is solely because matches() fails on manifest bytes.
    }
    void noticeOnlyForAnOlderInstalledPlugin()
    {
        const NoticeFacts base{State::Newer, QStringLiteral("1.4.0"), QStringLiteral("1.3.0"), QString(), true, true};
        QVERIFY(shouldNotify(base));
        for (State other : {State::NotFound, State::Absent, State::Matches, State::InstalledNewer, State::Incomplete}) {
            NoticeFacts facts = base;
            facts.state = other;
            QVERIFY(!shouldNotify(facts));
        }
        NoticeFacts facts = base;
        facts.targetResolved = false;
        QVERIFY(!shouldNotify(facts));
        facts = base;
        facts.python = false;
        QVERIFY(!shouldNotify(facts));
        facts = base;
        facts.notified = QStringLiteral("1.4.0"); // shown once for this bundle
        QVERIFY(!shouldNotify(facts));
        facts.notified = QStringLiteral("1.3.0"); // an older notice does not count
        QVERIFY(shouldNotify(facts));
        facts = base;
        facts.bundled.clear();
        QVERIFY(!shouldNotify(facts));
    }
    void versionsComeFromTheManifests()
    {
        const auto bundledManifest = QJsonDocument::fromJson(read(QStringLiteral(":/opendeck/manifest.json"))).object();
        QCOMPARE(bundledVersion(), bundledManifest.value(QStringLiteral("Version")).toString());
        QCOMPARE(bundledVersion(), QStringLiteral("1.4.1"));
        QTemporaryDir temp;
        Target target{QStringLiteral("custom"), temp.path()};
        QCOMPARE(installedVersion(target), QString());
        QCOMPARE(install(target, true).error, Error::None);
        QCOMPARE(installedVersion(target), bundledVersion());
        const QString manifest = temp.path() + QLatin1Char('/') + pluginId() + QStringLiteral("/manifest.json");
        auto object = QJsonDocument::fromJson(read(manifest)).object();
        object.insert(QStringLiteral("Version"), QStringLiteral("1.3.0"));
        write(manifest, QJsonDocument(object).toJson());
        QCOMPARE(installedVersion(target), QStringLiteral("1.3.0"));
    }
    void customSettingsRoundTrip()
    {
        Settings settings = defaultSettings();
        settings.openDeckPluginsFolder = QStringLiteral("/home/person/Custom plugins");
        settings.openDeckInstallation = QStringLiteral("flatpak");
        settings.openDeckNotifiedVersion = QStringLiteral("1.4.0");
        const auto restored = parseSettings(serializeSettings(settings));
        QCOMPARE(restored.openDeckPluginsFolder, settings.openDeckPluginsFolder);
        QCOMPARE(restored.openDeckInstallation, settings.openDeckInstallation);
        QCOMPARE(restored.openDeckNotifiedVersion, QStringLiteral("1.4.0"));
        QCOMPARE(parseSettings(QStringLiteral("format = 1\n")).openDeckPluginsFolder, QString());
    }
};
QTEST_GUILESS_MAIN(TestOpenDeck)
#include "tst_opendeck.moc"
