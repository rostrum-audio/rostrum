<h1 align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/brand/lockup-on-dark.svg">
    <img alt="Rostrum" src="docs/brand/lockup.svg" height="72">
  </picture>
</h1>

<p align="center">
  <a href="https://github.com/rostrum-audio/rostrum/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/rostrum-audio/rostrum/actions/workflows/ci.yml/badge.svg"></a>
  <a href="LICENSE"><img alt="License: Apache-2.0" src="https://img.shields.io/badge/license-Apache--2.0-blue"></a>
  <img alt="Platform: Linux" src="https://img.shields.io/badge/platform-Linux-FFB43C">
  <img alt="Audio: PipeWire" src="https://img.shields.io/badge/audio-PipeWire-22D3B6">
  <img alt="UI: Qt 6 and Kirigami" src="https://img.shields.io/badge/UI-Qt%206%20%2B%20Kirigami-7B61FF">
  <a href="https://getrostrum.dev"><img alt="Website: getrostrum.dev" src="https://img.shields.io/badge/web-getrostrum.dev-FF4F5E"></a>
</p>

> *A stream mix console for Linux, not a patchbay.*

![The Mixer: Headphones and Stream masters, then six buses with faders, mute, solo, destinations and the apps on each bus](docs/screenshots/mixer.png)


## 🌟 Highlights

- 🎚️ **Buses, not wires.** Game, Voice, Mic, Music, Alerts and Desktop each get a fader and mute,
  and every playback bus gets solo.
- 🎧 **Two mixes from one console.** Send each bus to your headphones, to the stream, or to both.
  Music can play for viewers without playing in your ears.
- 🔴 **OBS in one click.** OBS captures one clean `Rostrum Stream Mix` and one `Rostrum Mic`.
  Rostrum sets it up, mutes the sources that would double your audio, and can undo every change.
- 🎬 **Scenes.** Recall every level, mute and destination at once, from the header or a global shortcut.
- 🪄 **Apps find their bus on their own.** Discord goes to Voice, Spotify to Music, Steam and Proton
  games to Game, and Streamer.bot to Alerts. Every placement shows why it was made, and OBS and audio
  tools are never touched.
- 🧲 **Apps remember their bus.** Drag an app onto a bus once and it lands there every time, even
  before Rostrum starts at your next login.
- 🛟 **Safe by design.** The virtual devices live in PipeWire, so audio keeps flowing if Rostrum
  quits or crashes.
- 🔒 **Private crash reports, your call.** In official builds, Rostrum asks after a crash before
  sending anything to Sentry. A report holds only the crash location and software versions: no
  names, paths, device names or IDs. You can read the exact report first. See
  [docs/privacy.md](docs/privacy.md).
- ⬆️ **Stays up to date.** A daily check announces new versions. Once AppImage builds are published,
  they update themselves after checking the release checksum. Package installs are left to the
  package manager.


## ℹ️ Overview

Rostrum splits your game, voice chat, mic, music, alerts and desktop audio into named buses. Each
bus goes to your headphones, to the stream, or to both, and OBS records the stream mix. Tools like
qpwgraph and Helvum show every port and every link. Rostrum hides the graph and gives you a mixing
desk instead: faders, meters, mute and solo, and scenes you can switch live.

Rostrum targets any current Linux desktop on PipeWire and WirePlumber. Kubuntu is the development
machine, not the only supported system. Plasma, GNOME and other desktops are in scope. X11 is a
fallback.

**Status:** version 0.1.0 is feature-complete and smoke-tested on Kubuntu. The headset, OBS and
reboot checks in [docs/manual-tests.md](docs/manual-tests.md) still need a person with the hardware.
There are no packages or AppImage builds yet, so Rostrum installs from source (see below).

### ✍️ Authors

Rostrum is made by [Rostrum Audio](https://github.com/rostrum-audio). More at
[getrostrum.dev](https://getrostrum.dev).


## 🚀 Usage

Five minutes to a split stream:

1. **Start Rostrum** from the app menu (after installing, see below) or run `./build/src/app/rostrum`.
2. **Run first-time setup.** Eight short steps, with a summary at the end:
   - **Headphones:** press Test and a short chime plays only there.
   - **Mic:** speak and watch its meter.
   - **Apps and Buses:** whether apps go to their bus automatically, and which kind of app each bus receives.
   - **Startup:** launch at login (recommended) and start hidden in the tray.
   - **Privacy and Updates:** crash reports (send, ask, or never; official builds only) and update
     checks.
   - **OBS:** if OBS is installed, **Set Up OBS** shows the same preview as the OBS page (Rostrum
     creates its devices first), or **Skip**. Also whether Rostrum follows OBS while it runs.
   - **Ready:** each choice in one list. Click one to change it, then press **Create Mix**.

   Skip Setup creates the same mix with the defaults. Everything can be changed later in Settings.

   ![First-time setup, step 1 of 7: a step list on the left, and how apps go to buses and buses go to your headphones and to OBS](docs/screenshots/wizard.png)

3. **Put apps on buses.** Start Discord, your game and Spotify. Rostrum recognises most apps and
   puts them on the matching bus by itself; the Apps page marks those **Auto** and says why ("Steam
   game", "Recognised as a voice chat app"). To change one, drag its chip onto another bus on the
   Mixer, or open Apps and press Move. Leave "Always" on and the app goes straight to that bus
   whenever Rostrum sees it, and from your next login on even before Rostrum starts. Take an app off
   its bus and Rostrum stops placing it. Right-click a strip → **Receives Automatically** to choose
   which kind of app each bus gets. Apps that report no name (Wine and Proton games) get a banner so
   you can name them once.

   ![The Apps page: what is playing now, and the saved rules](docs/screenshots/apps.png)

4. **Pick destinations.** Each bus goes to Headphones, Stream or Both. Defaults: Music → Stream (your
   viewers hear it, you don't), the other playback buses → Both, Mic → Stream with sidetone off.
5. **Add Rostrum to OBS.** Open the OBS page and press **Set Up OBS**. A preview lists every
   change: your mic source switches to **Rostrum Mic**, a **Rostrum Stream Mix** source joins
   every scene, and sources that would double audio (desktop audio, single-app captures) are
   muted, never deleted. With OBS running this goes through obs-websocket (on by default since
   OBS 28; Rostrum reads its password from OBS's own settings). With OBS closed, Rostrum edits the
   scene collection after backing it up. **Undo OBS Changes** puts everything back. The page also
   shows what OBS really records, from the PipeWire graph. "Set it up by hand" has the manual steps.

   ![The OBS page: OBS is set up, and the list of what OBS records right now](docs/screenshots/obs.png)

   **While you stream**, Rostrum follows OBS quietly over the same localhost connection, whenever
   OBS is open (Settings → OBS → "Follow OBS while it runs"). The header shows a red **LIVE** badge
   and a **REC** badge with the elapsed time, and the tray tooltip says the same. If a stream
   starts with your mic muted, nothing reaching the stream mix, or OBS not recording Rostrum, a
   banner and a desktop notification say so. Under "When OBS switches scenes" on the OBS page,
   pick a Rostrum scene for each OBS scene. When OBS puts that scene on program, Rostrum loads it
   at once, without asking, after saving the current scene if auto-save is on.

6. **Make more scenes.** Scenes recall every level, mute and destination, and changes save to the
   live scene by themselves. On the Scenes page, **New** starts an empty scene or one from a preset
   (Gaming, Just Chatting, Music Stream, Podcast, Be Right Back) that keeps your buses and app
   rules. Switch with Meta+Alt+PgDown or the scene menu in the header.

### ⌨️ Shortcuts

Default global shortcuts, rebindable in Settings or in System Settings → Keyboard → Shortcuts on Plasma:

| Action | Shortcut |
| --- | --- |
| Mute mic | Meta+Alt+M |
| Mute all playback to stream | Meta+Alt+S |
| Previous / next scene | Meta+Alt+PgUp / Meta+Alt+PgDown |
| Load scene 1–4 | Meta+Alt+1 … Meta+Alt+4 |

With a fader focused: Up/Down 1 %, Page Up/Down 10 %, M mute, S solo, 1/2/3 Headphones/Stream/Both.
F6 moves focus between the header, the sidebar and the page.

### ⚙️ Defaults

- Default scene name: `Live`.
- Six buses: Mic, Game, Voice, Music, Alerts, Desktop (at most 12).
- Music bus destination: Stream. Other playback buses: Both. Mic: Stream, sidetone off.
- Assign apps automatically: on. Game, Voice, Music, Alerts and Desktop each receive their own kind
  of app; Desktop gets everything else that is recognised. The Mic bus receives nothing.
- Solo is not saved in the scene.
- Close window hides to tray (when the desktop has one). Quit from the tray or Settings.
- Scroll-to-adjust faders: on. Confirm scene switch: off. Launch at login: off until you turn it
  on (setup recommends it).
- Crash reports: ask after a crash. Update checks: once a day. Automatic install: on, for the
  AppImage only.
- Follow OBS while it runs: on (localhost only, read-only). Go-live warnings: on. No OBS scenes
  mapped.

### 🗂️ Where things live

- Settings and scenes: `~/.config/rostrum/` (TOML). Log: `~/.local/state/rostrum/rostrum.log`.
- Crash reports waiting to be sent or discarded: `~/.local/state/rostrum/crashes/` (at most 10,
  none older than 30 days), and sentry-native's own database in `~/.local/state/rostrum/sentry/`.
- App rules for the next login: `~/.config/pipewire/pipewire-pulse.conf.d/50-rostrum.conf` and
  `~/.config/pipewire/client.conf.d/50-rostrum.conf`. Nothing is written to `~/.config/wireplumber/`.
- Autostart, when on: `~/.config/autostart/dev.getrostrum.Rostrum.desktop`.

Rostrum's virtual devices live in PipeWire, not in the app, so audio keeps flowing if Rostrum quits
or crashes. To remove Rostrum completely, quit it, delete the files above, and run
`systemctl --user restart pipewire pipewire-pulse wireplumber` to drop its devices.


## ⬇️ Installation

There are no packages yet, so Rostrum installs from source into your home folder. On Kubuntu 26.04:

```sh
sudo apt install build-essential cmake ninja-build pkg-config extra-cmake-modules \
  libpipewire-0.3-dev libtomlplusplus-dev qt6-base-dev qt6-declarative-dev qt6-websockets-dev \
  libkirigami-dev kirigami-addons-dev libkf6coreaddons-dev libkf6dbusaddons-dev \
  libkf6i18n-dev libkf6globalaccel-dev libkf6statusnotifieritem-dev \
  qml6-module-org-kde-kirigami qml6-module-org-kde-kquickcontrols qml6-module-org-kde-desktop \
  qml6-module-org-kde-kirigamiaddons-formcard qml6-module-org-kde-kitemmodels \
  qml6-module-qtquick-dialogs qml6-module-qtcore
cmake -S . -B build -G Ninja
cmake --build build
cmake --install build --prefix ~/.local
```

Installing into `~/.local` adds the app menu entry, the icon and the System Settings shortcut page.

### 📋 Requirements

- Linux with PipeWire 1.0 or newer and WirePlumber 0.5 or newer. PulseAudio is not used or required.
- Qt 6.5+ (with Qt WebSockets), KDE Frameworks 6.8+ (Kirigami, Kirigami Addons, GlobalAccel,
  StatusNotifierItem, CoreAddons, I18n, DBusAddons), toml++ 3.

Base packages on other distros (add the Kirigami and KDE Frameworks packages above):

| Distro | Packages |
| --- | --- |
| Debian, Ubuntu, Kubuntu | `build-essential` `pipewire` `pipewire-pulse` `wireplumber` `qt6-base-dev` `qt6-websockets-dev` `libpipewire-0.3-dev` `libtomlplusplus-dev` `cmake` `ninja-build` `pkg-config` |
| Fedora | `pipewire` `pipewire-pulseaudio` `wireplumber` `qt6-qtbase-devel` `qt6-qtwebsockets-devel` `pipewire-devel` `tomlplusplus-devel` `gcc-c++` `cmake` `ninja-build` `pkgconf-pkg-config` |
| Arch | `pipewire` `pipewire-pulse` `wireplumber` `qt6-base` `qt6-websockets` `tomlplusplus` `gcc` `cmake` `ninja` `pkgconf` |

Smoke-tested on **Kubuntu 26.04 LTS, KDE Plasma 6 on Wayland**, PipeWire 1.6.2, WirePlumber 0.5.13,
Qt 6.10, KDE Frameworks 6.24.


## 💭 Feedback and Contributing

- 🐛 Found a bug or want a feature? [Open an issue](https://github.com/rostrum-audio/rostrum/issues).
- ✉️ Questions and feedback: hello@getrostrum.dev
- 🔒 Security problems: security@getrostrum.dev. See [SECURITY.md](SECURITY.md).

Contributions are welcome: start with [CONTRIBUTING.md](CONTRIBUTING.md), and please follow the
[Code of Conduct](CODE_OF_CONDUCT.md). Run `ctest --test-dir build` before sending a change.
Headset, OBS and reboot checks stay manual: walk through [docs/manual-tests.md](docs/manual-tests.md)
for anything that touches audio routing.

### 🛠️ Development

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
./build/src/app/rostrum
```

Configure with `-DROSTRUM_BUILD_APP=OFF` to build only the engine, tools and tests (CI does this).
Logs go to `~/.local/state/rostrum/rostrum.log`; a crash appends a backtrace there. Official
builds use `cmake --preset official`, which turns on crash reports to Rostrum's Sentry project and
fetches a pinned sentry-native release at configure time; other builds have no crash reporting.
`-DROSTRUM_UPDATE_URL=…` points updates at your own feed. See [docs/privacy.md](docs/privacy.md)
for both, and for testing against a local server.


## 📖 Further reading

- [How the audio side works, and why](docs/audio.md)
- [Crash reports and updates: what is sent, and when](docs/privacy.md)
- [Manual test plan](docs/manual-tests.md)
- [Security policy](SECURITY.md)
- [Contributing](CONTRIBUTING.md) and [Code of Conduct](CODE_OF_CONDUCT.md)
- [Accessibility](ACCESSIBILITY.md)
- [getrostrum.dev](https://getrostrum.dev)


## 📄 License

Apache-2.0. See [LICENSE](LICENSE).
