#include "core/AppClassifier.h"
#include "core/AppFacts.h"
#include "core/DesktopEntries.h"
#include "core/SceneToml.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace rostrum;

namespace {

AppFacts facts(const QString &appName, const QString &binary)
{
    AppFacts f;
    f.props = {appName, binary, {}, {}};
    return f;
}

void writeFile(const QString &path, const QByteArray &contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(contents);
}

} // namespace

class TestAppClassifier : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void knownApps_data()
    {
        QTest::addColumn<QString>("appName");
        QTest::addColumn<QString>("binary");
        QTest::addColumn<AppCategory>("category");
        QTest::newRow("discord voice") << "WEBRTC VoiceEngine" << "Discord" << AppCategory::Voice;
        QTest::newRow("discord pings") << "Chromium" << "Discord" << AppCategory::Voice;
        QTest::newRow("vesktop") << "Vesktop" << "electron" << AppCategory::Voice;
        QTest::newRow("spotify") << "spotify" << "spotify" << AppCategory::Music;
        // Its desktop entry says AudioVideo;Audio; without Player, so only the catalog knows.
        QTest::newRow("youtube music desktop") << "YouTube Music Desktop App" << "youtube-music-desktop-app"
                                               << AppCategory::Music;
        QTest::newRow("streamer.bot") << "Streamer.bot" << "Streamer.bot.exe" << AppCategory::Alerts;
        QTest::newRow("firefox") << "Firefox" << "firefox" << AppCategory::Desktop;
        QTest::newRow("steam client") << "Steam" << "steamwebhelper" << AppCategory::Desktop;
        QTest::newRow("retroarch") << "RetroArch" << "retroarch" << AppCategory::Game;
    }

    void knownApps()
    {
        QFETCH(QString, appName);
        QFETCH(QString, binary);
        QFETCH(AppCategory, category);
        const Classification c = classify(facts(appName, binary));
        QCOMPARE(c.category, category);
        QCOMPARE(c.evidence, Evidence::Catalog);
        QVERIFY(!c.excluded);
    }

    void flatpakIdIsEnough()
    {
        AppFacts f = facts(QStringLiteral("Client"), QStringLiteral("spotify-bin"));
        f.appId = QStringLiteral("com.spotify.Client");
        QCOMPARE(classify(f).category, AppCategory::Music);
    }

    void minecraftByItsLauncher()
    {
        AppFacts f = facts(QStringLiteral("java"), QStringLiteral("java"));
        QCOMPARE(classify(f).category, AppCategory::None);
        f.appId = QStringLiteral("org.prismlauncher.PrismLauncher");
        QCOMPARE(classify(f).category, AppCategory::Game);
    }

    void obsAndToolsAreNeverPlaced()
    {
        // OBS monitoring routed into the stream bus would loop back into OBS.
        QVERIFY(classify(facts(QStringLiteral("OBS"), QStringLiteral("obs"))).excluded);
        AppFacts flatpakObs = facts(QStringLiteral("OBS"), QStringLiteral("obs"));
        flatpakObs.appId = QStringLiteral("com.obsproject.Studio");
        QVERIFY(classify(flatpakObs).excluded);
        QVERIFY(classify(facts(QStringLiteral("EasyEffects"), QStringLiteral("easyeffects"))).excluded);

        AppFacts reader = facts(QStringLiteral("speech-dispatcher-espeak-ng"), QStringLiteral("sd_espeak-ng"));
        QVERIFY(classify(reader).excluded);
        AppFacts a11y = facts(QStringLiteral("Some Reader"), QStringLiteral("reader"));
        a11y.mediaRole = QStringLiteral("Accessibility");
        QVERIFY(classify(a11y).excluded);

        AppFacts daw = facts(QStringLiteral("My DAW"), QStringLiteral("mydaw"));
        daw.desktopId = QStringLiteral("org.example.MyDaw");
        daw.desktopCategories = {QStringLiteral("AudioVideo"), QStringLiteral("Audio"), QStringLiteral("Recorder")};
        QVERIFY(classify(daw).excluded);
    }

    void ownOutputChoiceIsRespected()
    {
        AppFacts f = facts(QStringLiteral("Discord"), QStringLiteral("Discord"));
        f.ownOutputChoice = true;
        const Classification c = classify(f);
        QVERIFY(c.excluded);
        QCOMPARE(c.evidence, Evidence::OwnOutput);
    }

    void steamGamesWinOverTheirIcon()
    {
        AppFacts f = facts(QStringLiteral("ALSA plug-in [wine64-preloader]"), QStringLiteral("wine64-preloader"));
        f.steamAppId = QStringLiteral("1145350");
        f.iconName = QStringLiteral("steam");
        QCOMPARE(classify(f), (Classification{AppCategory::Game, Evidence::Steam}));
    }

    void wineWithoutSteamIsAGame()
    {
        QCOMPARE(classify(facts(QStringLiteral("ALSA plug-in [wine64-preloader]"), QStringLiteral("wine64-preloader"))),
                 (Classification{AppCategory::Game, Evidence::Wine}));
        QCOMPARE(classify(facts(QStringLiteral("Game.exe"), QStringLiteral("Game.exe"))).category, AppCategory::Game);
    }

    void mediaRole()
    {
        AppFacts f = facts(QStringLiteral("Unknown Player"), QStringLiteral("unknownplayer"));
        f.mediaRole = QStringLiteral("Music");
        QCOMPARE(classify(f), (Classification{AppCategory::Music, Evidence::MediaRole}));
        f.mediaRole = QStringLiteral("Game");
        QCOMPARE(classify(f).category, AppCategory::Game);
        f.mediaRole = QStringLiteral("Communication");
        QCOMPARE(classify(f).category, AppCategory::Voice);
        f.mediaRole = QStringLiteral("Notification");
        QCOMPARE(classify(f).category, AppCategory::Desktop);
    }

    void desktopCategories_data()
    {
        QTest::addColumn<QStringList>("categories");
        QTest::addColumn<AppCategory>("category");
        QTest::newRow("game") << QStringList{"Game", "ActionGame"} << AppCategory::Game;
        QTest::newRow("chat") << QStringList{"Network", "InstantMessaging"} << AppCategory::Voice;
        QTest::newRow("music player") << QStringList{"AudioVideo", "Audio", "Player"} << AppCategory::Music;
        QTest::newRow("video player") << QStringList{"AudioVideo", "Video", "Player"} << AppCategory::Desktop;
        QTest::newRow("other") << QStringList{"Office"} << AppCategory::Desktop;
    }

    void desktopCategories()
    {
        QFETCH(QStringList, categories);
        QFETCH(AppCategory, category);
        AppFacts f = facts(QStringLiteral("Some App"), QStringLiteral("someapp"));
        f.desktopId = QStringLiteral("org.example.SomeApp");
        f.desktopCategories = categories;
        QCOMPARE(classify(f), (Classification{category, Evidence::DesktopEntry}));
    }

    void gameEngineNamesAreAWeakHint()
    {
        QCOMPARE(classify(facts(QStringLiteral("OpenAL Soft"), QStringLiteral("java"))),
                 (Classification{AppCategory::Game, Evidence::GameEngine}));
    }

    void unknownStaysUnknown()
    {
        const Classification c = classify(facts(QStringLiteral("mystery"), QStringLiteral("mystery")));
        QCOMPARE(c.category, AppCategory::None);
        QVERIFY(!c.excluded);
    }

    void execTokensSeeThroughWrappers()
    {
        QCOMPARE(execTokens(QStringLiteral("env BAMF=1 /usr/bin/spotify %U")), QStringList{"spotify"});
        QCOMPARE(execTokens(QStringLiteral("/usr/bin/flatpak run --branch=stable --command=spotify com.spotify.Client")),
                 (QStringList{"com.spotify.client", "spotify"}));
        QCOMPARE(execTokens(QStringLiteral("sh -c \"discord --start-minimized\"")), QStringList{"discord"});
    }

    void parseDesktopEntry()
    {
        const auto e = rostrum::parseDesktopEntry(QStringLiteral("[Desktop Entry]\nType=Application\nName=Elisa\n"
                                                                 "Name[de]=Elisa DE\nExec=elisa %U\n"
                                                                 "Categories=Qt;KDE;AudioVideo;Audio;Player;\n"
                                                                 "StartupWMClass=elisa-wm\n[Desktop Action x]\nExec=other\n"),
                                                  QStringLiteral("org.kde.elisa"));
        QVERIFY(e);
        QCOMPARE(e->name, QStringLiteral("Elisa"));
        QVERIFY(e->categories.contains(QStringLiteral("Player")));
        QVERIFY(e->tokens.contains(QStringLiteral("org.kde.elisa")));
        QVERIFY(e->tokens.contains(QStringLiteral("elisa")));
        QVERIFY(e->tokens.contains(QStringLiteral("elisa-wm")));
        QVERIFY(!e->tokens.contains(QStringLiteral("other")));
        QVERIFY(e->icon.isEmpty());
        const auto withIcon = rostrum::parseDesktopEntry(
            QStringLiteral("[Desktop Entry]\nType=Application\nName=Brave\nIcon=brave-browser\nIcon[de]=x\n"),
            QStringLiteral("brave-browser"));
        QVERIFY(withIcon);
        QCOMPARE(withIcon->icon, QStringLiteral("brave-browser"));
        QVERIFY(!rostrum::parseDesktopEntry(QStringLiteral("[Desktop Entry]\nType=Application\nHidden=true\n"),
                                            QStringLiteral("gone")));
    }

    void desktopIndexLookup()
    {
        QTemporaryDir user;
        QTemporaryDir system;
        writeFile(user.path() + QStringLiteral("/applications/org.example.Game.desktop"),
                  "[Desktop Entry]\nType=Application\nName=Example Game\nExec=examplegame\nCategories=Game;\n");
        writeFile(system.path() + QStringLiteral("/applications/org.example.Game.desktop"),
                  "[Desktop Entry]\nType=Application\nName=Shadowed\nExec=examplegame\nCategories=Office;\n");
        writeFile(
            system.path() + QStringLiteral("/applications/vendor/chat.desktop"),
            "[Desktop "
            "Entry]\nType=Application\nName=Chatter\nExec=/opt/chat/chatter\nCategories=Network;Chat;\n");
        DesktopIndex index({user.path(), system.path()});

        const auto game = index.find({QStringLiteral("ExampleGame")});
        QVERIFY(game);
        QCOMPARE(game->id, QStringLiteral("org.example.Game"));
        QCOMPARE(game->categories, QStringList{"Game"}); // the user's copy wins
        const auto chat = index.find({QString(), QStringLiteral("nope"), QStringLiteral("chatter")});
        QVERIFY(chat);
        QCOMPARE(chat->id, QStringLiteral("vendor-chat"));
        QVERIFY(!index.find({QStringLiteral("missing")}));
    }

    void ownOutputChoiceKeepsAppIcon()
    {
        AppFacts f = facts(QStringLiteral("Discord"), QStringLiteral("Discord"));
        f.desktopIcon = QStringLiteral("discord");
        f.ownOutputChoice = true;
        f.dontMove = true;
        QCOMPARE(iconCandidates(f), QStringList{"discord"});
    }

    void excludedToolsHaveNoAppIcon()
    {
        for (const QString &name : {QStringLiteral("OBS"), QStringLiteral("speech-dispatcher-dummy"),
                                    QStringLiteral("EasyEffects")}) {
            AppFacts f =
                facts(name, name == QLatin1String("speech-dispatcher-dummy") ? QStringLiteral("sd_dummy")
                                                                             : name.toLower());
            f.desktopIcon = QStringLiteral("some-installed-icon");
            f.iconName = QStringLiteral("some-reported-icon");
            QVERIFY(iconCandidates(f).isEmpty());
        }
    }

    void desktopIconsForAppsAndRules()
    {
        QTemporaryDir dir;
        const QString root = dir.path() + QStringLiteral("/applications/");
        writeFile(root + "custom-firefox.desktop",
                  "[Desktop Entry]\nName=Firefox Developer Edition\nExec=firefox-bin %u\n");
        writeFile(root + "firefox-devedition.desktop",
                  "[Desktop Entry]\nName=Firefox Developer Edition\nExec=firefox-devedition %u\n"
                  "StartupWMClass=firefox-dev\nIcon=firefox-devedition\n");
        writeFile(root + "brave-browser.desktop",
                  "[Desktop Entry]\nName=Brave Web Browser\nExec=brave-browser-stable %u\n"
                  "StartupWMClass=brave-browser\nIcon=brave-browser\n");
        writeFile(root + "com.discordapp.Discord.desktop",
                  "[Desktop Entry]\nName=Discord\nExec=flatpak run com.discordapp.Discord\nIcon=discord\n");
        writeFile(root + "appimagekit-player.desktop",
                  "[Desktop Entry]\nName=Portable Player\nExec=/opt/Player.AppImage\nIcon=portable-player\n");
        DesktopIndex index({dir.path()});
        QCOMPARE(index.find({"firefox-bin"})->icon, QStringLiteral("firefox-devedition"));
        QCOMPARE(index.find({"firefox-dev"})->icon, QStringLiteral("firefox-devedition"));
        const auto flatpak =
            collectFacts({"Chromium", "electron", {}, {}}, {{"application.id", "com.discordapp.Discord"}}, 0,
                         index, [](const QString &) { return false; });
        QCOMPARE(iconCandidates(flatpak), QStringList{"discord"});
        const auto appimage =
            collectFacts({"Player", "unknown-bin", {}, {}}, {{"application.id", "appimagekit-player"}}, 0,
                         index, [](const QString &) { return false; });
        QCOMPARE(iconCandidates(appimage), QStringList{"portable-player"});
        const auto firefox = collectFacts({"Firefox Developer Edition", "firefox-bin", {}, {}}, {}, 0, index,
                                          [](const QString &) { return false; });
        QCOMPARE(iconCandidates(firefox), QStringList{"firefox-devedition"});
        const auto brave = index.find({"Brave"});
        QVERIFY(brave);
        QCOMPARE(brave->icon, QStringLiteral("brave-browser"));
        const auto java =
            collectFacts({"java", "java", {}, {}}, {}, 0, index, [](const QString &) { return false; });
        QVERIFY(iconCandidates(java).isEmpty());
        QVERIFY(!index.find({"java"}));
    }

    void iconCandidatesBestFirst()
    {
        AppFacts f;
        f.props.binary = QStringLiteral("/opt/brave.com/brave/Brave");
        f.iconName = QStringLiteral("brave-browser");
        f.desktopIcon = QStringLiteral("brave-browser");
        f.desktopId = QStringLiteral("brave-browser");
        QCOMPARE(iconCandidates(f), (QStringList{"brave-browser"}));

        f.steamAppId = QStringLiteral("1145350");
        f.desktopIcon = QStringLiteral("/home/x/.local/share/icons/game.png");
        QCOMPARE(iconCandidates(f).first(), QStringLiteral("steam_icon_1145350"));
        QCOMPARE(iconCandidates(f).at(1), QStringLiteral("/home/x/.local/share/icons/game.png"));
        QVERIFY(iconCandidates(AppFacts{}).isEmpty());
    }

    void processFactsNeedTheRightBinary()
    {
        QTemporaryDir proc;
        const QString dir = proc.path() + QStringLiteral("/4242");
        writeFile(dir + QStringLiteral("/comm"), "wine64-preloade\n");
        QByteArray environ;
        for (const char *var : {"HOME=/home/x", "SteamAppId=0", "SteamGameId=1145350", "FLATPAK_ID="}) {
            environ.append(var).append('\0');
        }
        writeFile(dir + QStringLiteral("/environ"), environ);
        const ProcessFacts ok = readProcessFacts(4242, QStringLiteral("wine64-preloader"), proc.path());
        QCOMPARE(ok.steamAppId, QStringLiteral("1145350"));

        // A sandboxed app reports a pid from inside its sandbox: on the host that is another process.
        const ProcessFacts other = readProcessFacts(4242, QStringLiteral("obs"), proc.path());
        QVERIFY(other.steamAppId.isEmpty());
    }

    void steamGameNameFromLibrary()
    {
        QTemporaryDir root;
        QTemporaryDir library;
        writeFile(root.path() + QStringLiteral("/steamapps/libraryfolders.vdf"),
                  QStringLiteral("\"libraryfolders\"\n{\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"%1\"\n\t}\n}\n")
                      .arg(library.path())
                      .toUtf8());
        writeFile(library.path() + QStringLiteral("/steamapps/appmanifest_1145350.acf"),
                  "\"AppState\"\n{\n\t\"appid\"\t\t\"1145350\"\n\t\"name\"\t\t\"Hades II\"\n}\n");
        QCOMPARE(steamGameName(QStringLiteral("1145350"), {root.path()}), QStringLiteral("Hades II"));
        QVERIFY(steamGameName(QStringLiteral("42"), {root.path()}).isEmpty());
        QVERIFY(steamGameName(QStringLiteral("../etc"), {root.path()}).isEmpty());
    }

    void collectFactsTreatsRostrumTargetsAsOurs()
    {
        DesktopIndex empty({QStringLiteral("/nonexistent")});
        const auto isOurs = [](const QString &t) { return t.startsWith(QLatin1String("rostrum.")); };
        const StreamProps props{QStringLiteral("Discord"), QStringLiteral("Discord"), {}, {}};
        QMap<QString, QString> node{{QStringLiteral("target.object"), QStringLiteral("rostrum.voice")}};
        QVERIFY(!collectFacts(props, node, 0, empty, isOurs).ownOutputChoice);
        node.insert(QStringLiteral("target.object"), QStringLiteral("alsa_output.usb-headset"));
        QVERIFY(collectFacts(props, node, 0, empty, isOurs).ownOutputChoice);
        node.insert(QStringLiteral("pipewire.access.portal.app_id"), QStringLiteral("com.discordapp.Discord"));
        QCOMPARE(collectFacts(props, node, 0, empty, isOurs).appId, QStringLiteral("com.discordapp.Discord"));
    }

    void busAutoCategoryRoundTrip()
    {
        Scene s = defaults::scene();
        QCOMPARE(s.busFor(AppCategory::Game)->id, QStringLiteral("game"));
        QCOMPARE(s.busFor(AppCategory::Desktop)->id, QStringLiteral("desktop"));
        QVERIFY(!s.busFor(AppCategory::None));
        s.bus(QStringLiteral("music"))->autoCategory = AppCategory::None;

        const auto back = toml_io::parseScene(toml_io::serializeScene(s));
        QVERIFY(back);
        QCOMPARE(back->bus(QStringLiteral("music"))->autoCategory, AppCategory::None);
        QCOMPARE(back->bus(QStringLiteral("voice"))->autoCategory, AppCategory::Voice);
    }

    void scenesFromBeforeAutoKeepDefaults()
    {
        const auto s = toml_io::parseScene(QStringLiteral("format = 1\nname = 'Old'\n"
                                                          "[[bus]]\nid = 'game'\nname = 'Game'\nkind = 'playback'\n"
                                                          "[[bus]]\nid = 'extra'\nname = 'Extra'\nkind = 'playback'\n"));
        QVERIFY(s);
        QCOMPARE(s->bus(QStringLiteral("game"))->autoCategory, AppCategory::Game);
        QCOMPARE(s->bus(QStringLiteral("extra"))->autoCategory, AppCategory::None);
        QCOMPARE(s->micBus()->autoCategory, AppCategory::None);
    }

    void oneBusPerCategory()
    {
        const auto s = toml_io::parseScene(QStringLiteral("format = 1\nname = 'Twice'\n"
                                                          "[[bus]]\nid = 'a'\nname = 'A'\nkind = 'playback'\nauto = 'music'\n"
                                                          "[[bus]]\nid = 'b'\nname = 'B'\nkind = 'playback'\nauto = 'music'\n"));
        QVERIFY(s);
        QCOMPARE(s->bus(QStringLiteral("a"))->autoCategory, AppCategory::Music);
        QCOMPARE(s->bus(QStringLiteral("b"))->autoCategory, AppCategory::None);
    }
};

QTEST_GUILESS_MAIN(TestAppClassifier)
#include "tst_appclassifier.moc"
