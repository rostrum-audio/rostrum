#include "core/AppClassifier.h"

#include <QHash>

#include <optional>

namespace rostrum {

namespace {

struct Known
{
    AppCategory category = AppCategory::None;
    bool excluded = false;
};

// Matched against the binary, application.name, the Flatpak/Snap id and the icon name, all
// lower-case. Only apps whose job is unambiguous belong here; the rest is left to the signals
// below, which the user can always overrule.
const QHash<QString, Known> &catalog()
{
    static const QHash<QString, Known> table = [] {
        QHash<QString, Known> t;
        const auto add = [&t](AppCategory c, std::initializer_list<const char *> names) {
            for (const char *n : names) {
                t.insert(QString::fromLatin1(n), {c, false});
            }
        };
        const auto exclude = [&t](std::initializer_list<const char *> names) {
            for (const char *n : names) {
                t.insert(QString::fromLatin1(n), {AppCategory::None, true});
            }
        };
        add(AppCategory::Voice,
            {"discord", "discordcanary", "discordptb", "discord canary", "discord ptb", "com.discordapp.discord",
             "com.discordapp.discordcanary", "vesktop", "dev.vencord.vesktop", "webcord", "io.github.spacingbat3.webcord",
             "armcord", "legcord", "xyz.armcord.armcord", "app.legcord.legcord", "dorion", "goofcord",
             "teamspeak", "teamspeak3", "teamspeak 3", "ts3client_linux_amd64", "teamspeak5", "teamspeak-client",
             "com.teamspeak.teamspeak", "com.teamspeak.teamspeak3", "mumble", "info.mumble.mumble",
             "zoom", "us.zoom.zoom", "slack", "com.slack.slack", "skype", "skypeforlinux", "com.skype.client",
             "element", "element-desktop", "im.riot.riot", "signal", "signal-desktop", "org.signal.signal",
             "telegram", "telegram-desktop", "telegramdesktop", "org.telegram.desktop", "teams-for-linux",
             "com.github.ismaelmartinez.teams_for_linux", "guilded", "revolt", "chat.revolt.revoltdesktop",
             "nheko", "im.nheko.nheko", "fractal", "org.gnome.fractal", "jitsi meet", "org.jitsi.jitsi-meet",
             "linphone", "com.belledonnecommunications.linphone", "mattermost", "mattermost-desktop",
             "com.mattermost.desktop", "whatsapp", "zapzap", "com.rtosta.zapzap", "kaidan", "dino", "im.dino.dino"});
        add(AppCategory::Music,
            {"spotify", "com.spotify.client", "spotify-launcher", "spotube", "com.github.krtirtho.spotube",
             "ncspot", "spotify_player", "rhythmbox", "org.gnome.rhythmbox3", "elisa", "org.kde.elisa",
             "strawberry", "org.strawberrymusicplayer.strawberry", "clementine", "org.clementine_player.clementine",
             "amarok", "org.kde.amarok", "lollypop", "org.gnome.lollypop", "audacious", "deadbeef", "quodlibet",
             "quod libet", "io.github.quodlibet.quodlibet", "cmus", "mpd", "music player daemon", "mopidy",
             "tidal-hifi", "com.mastermindzh.tidal-hifi", "cider", "sh.cider.cider", "youtube-music",
             "youtube music", "app.ytmdesktop.ytmdesktop", "com.github.th_ch.youtube_music", "nuclear",
             "org.js.nuclear.nuclear", "feishin", "sublime-music", "tauon", "tauonmb", "com.github.taiko2k.tauonmb",
             "g4music", "com.github.neithern.g4music", "amberol", "io.bassi.amberol", "gnome-music",
             "org.gnome.music", "harmonoid", "plexamp", "com.plexamp.plexamp", "deezer", "kew", "termusic",
             "musikcube", "fooyin", "org.fooyin.fooyin", "supersonic", "io.github.dweymouth.supersonic"});
        add(AppCategory::Alerts,
            {"streamer.bot", "streamerbot", "streamer.bot.exe", "mixitup", "mix it up", "mixitup.wpf",
             "firebot", "firebot v5", "sammi", "speaker.bot", "speakerbot", "lioranboard"});
        add(AppCategory::Game,
            {"gamescope", "retroarch", "org.libretro.retroarch", "dolphin-emu", "org.dolphinemu.dolphin-emu",
             "pcsx2", "pcsx2-qt", "net.pcsx2.pcsx2", "rpcs3", "net.rpcs3.rpcs3", "ppsspp", "ppssppsdl",
             "org.ppsspp.ppsspp", "duckstation", "duckstation-qt", "org.duckstation.duckstation", "cemu",
             "info.cemu.cemu", "ryujinx", "org.ryujinx.ryujinx", "citra", "citra-qt", "melonds",
             "net.kuribo64.melonds", "mgba", "mgba-qt", "io.mgba.mgba", "xemu", "app.xemu.xemu", "flycast",
             "minecraft", "minecraft launcher", "com.mojang.minecraft"});
        add(AppCategory::Desktop,
            {"firefox", "firefox-bin", "firefox-esr", "org.mozilla.firefox", "librewolf", "io.gitlab.librewolf-community",
             "floorp", "one.ablaze.floorp", "zen", "zen-bin", "app.zen_browser.zen", "waterfox", "chrome",
             "google-chrome", "com.google.chrome", "chromium", "chromium-browser", "org.chromium.chromium", "brave",
             "brave-browser", "com.brave.browser", "vivaldi", "vivaldi-bin", "com.vivaldi.vivaldi", "opera",
             "com.opera.opera", "microsoft-edge", "msedge", "com.microsoft.edge", "epiphany", "org.gnome.epiphany",
             "falkon", "org.kde.falkon", "qutebrowser", "mpv", "io.mpv.mpv", "vlc", "org.videolan.vlc", "celluloid",
             "io.github.celluloid_player.celluloid", "totem", "org.gnome.totem", "haruna", "org.kde.haruna",
             "smplayer", "showtime", "org.gnome.showtime", "steam", "steamwebhelper", "com.valvesoftware.steam",
             "plasmashell", "kded6", "kwin_wayland", "gnome-shell", "libcanberra", "notify-send"});
        exclude({"obs", "obs studio", "obs-studio", "com.obsproject.studio", "rostrum", "pavucontrol",
                 "org.pulseaudio.pavucontrol", "pwvucontrol", "com.saivert.pwvucontrol", "helvum",
                 "org.pipewire.helvum", "qpwgraph", "org.rncbc.qpwgraph", "easyeffects", "com.github.wwmm.easyeffects",
                 "noisetorch", "carla", "studio.kx.carla", "ardour", "reaper", "bitwig-studio", "audacity",
                 "org.audacityteam.audacity", "speech-dispatcher", "sd_espeak-ng", "sd_dummy", "orca", "spd-say"});
        return t;
    }();
    return table;
}

bool isWineBinary(const QString &binary)
{
    const QString b = binary.toLower();
    return b == QLatin1String("wine") || b == QLatin1String("wine64") || b == QLatin1String("wine-preloader") ||
           b == QLatin1String("wine64-preloader") || b.endsWith(QLatin1String(".exe"));
}

Classification byMediaRole(const QString &role)
{
    const QString r = role.trimmed().toLower();
    if (r == QLatin1String("game")) {
        return {AppCategory::Game, Evidence::MediaRole};
    }
    if (r == QLatin1String("music")) {
        return {AppCategory::Music, Evidence::MediaRole};
    }
    if (r == QLatin1String("communication") || r == QLatin1String("phone")) {
        return {AppCategory::Voice, Evidence::MediaRole};
    }
    if (r == QLatin1String("notification") || r == QLatin1String("event") || r == QLatin1String("movie") ||
        r == QLatin1String("video") || r == QLatin1String("animation")) {
        return {AppCategory::Desktop, Evidence::MediaRole};
    }
    return {};
}

Classification byDesktopCategories(const QStringList &categories)
{
    const auto has = [&categories](const char *c) { return categories.contains(QLatin1String(c), Qt::CaseInsensitive); };
    if (has("Game")) {
        return {AppCategory::Game, Evidence::DesktopEntry};
    }
    if (has("InstantMessaging") || has("Chat") || has("VideoConference") || has("Telephony") || has("IRCClient")) {
        return {AppCategory::Voice, Evidence::DesktopEntry};
    }
    if (has("Music") || (has("Audio") && has("Player") && !has("Video"))) {
        return {AppCategory::Music, Evidence::DesktopEntry};
    }
    return {AppCategory::Desktop, Evidence::DesktopEntry};
}

} // namespace

Classification classify(const AppFacts &f)
{
    Classification excluded;
    excluded.excluded = true;
    excluded.evidence = Evidence::Catalog;

    if (f.dontMove || f.ownOutputChoice) {
        return {AppCategory::None, Evidence::OwnOutput, true};
    }
    const QString role = f.mediaRole.trimmed().toLower();
    if (role == QLatin1String("accessibility") || role == QLatin1String("production") || role == QLatin1String("test")) {
        return {AppCategory::None, Evidence::MediaRole, true};
    }

    const QString binary = cleanBinary(f.props.binary).toLower();
    const auto known = [&excluded](const QStringList &names) -> std::optional<Classification> {
        for (const QString &n : names) {
            if (auto it = catalog().constFind(n); !n.isEmpty() && it != catalog().cend()) {
                return it->excluded ? excluded : Classification{it->category, Evidence::Catalog};
            }
        }
        return std::nullopt;
    };
    if (auto c = known({binary, f.props.appName.trimmed().toLower(), f.appId.trimmed().toLower()})) {
        return *c;
    }

    // Audio tools by their menu entry: a mixer or recorder is part of the signal chain, not a source.
    for (const char *tool : {"Mixer", "Recorder", "Sequencer", "Midi"}) {
        if (f.desktopCategories.contains(QLatin1String(tool), Qt::CaseInsensitive)) {
            return {AppCategory::None, Evidence::DesktopEntry, true};
        }
    }

    if (!f.steamAppId.isEmpty()) {
        return {AppCategory::Game, Evidence::Steam};
    }
    if (isWineBinary(binary) || f.props.appName.trimmed().endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)) {
        return {AppCategory::Game, Evidence::Wine};
    }
    // After Steam and Wine: games often carry Steam's or Wine's icon.
    if (auto c = known({f.iconName.trimmed().toLower()})) {
        return *c;
    }
    if (const Classification r = byMediaRole(role); r.category != AppCategory::None) {
        return r;
    }
    if (!f.desktopId.isEmpty()) {
        return byDesktopCategories(f.desktopCategories);
    }
    static const QStringList engines = {QStringLiteral("openal soft"), QStringLiteral("sdl application"),
                                        QStringLiteral("fmod ex"), QStringLiteral("fmod")};
    if (engines.contains(f.props.appName.trimmed().toLower())) {
        return {AppCategory::Game, Evidence::GameEngine};
    }
    return {};
}

} // namespace rostrum
