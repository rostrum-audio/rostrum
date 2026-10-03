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

- 🎚️ **Buses, not wires.** Game, Voice, Mic, Music, Alerts and Desktop each get a fader, mute and solo.
- 🎧 **Two mixes from one console.** Send each bus to your headphones, to the stream, or to both.
  Music can play for viewers without playing in your ears.
- 🔴 **OBS in one click.** OBS captures one clean `Rostrum Stream Mix` and one `Rostrum Mic`.
  Rostrum sets it up, mutes the sources that would double your audio, and can undo every change.
- 🎬 **Scenes.** Recall every level, mute and destination at once, from the header or a global shortcut.
- 🧲 **Apps remember their bus.** Drag an app onto a bus once and it lands there every time, even
  before Rostrum starts at your next login.
- 🛟 **Safe by design.** The virtual devices live in PipeWire, so audio keeps flowing if Rostrum
  quits or crashes.


## ℹ️ Overview

Rostrum splits your game, voice chat, mic, music, alerts and desktop audio into named buses. Each
bus goes to your headphones, to the stream, or to both, and OBS records the stream mix. Tools like
qpwgraph and Helvum show every port and every link. Rostrum hides the graph and gives you a mixing
desk instead: faders, meters, mute and solo, and scenes you can switch live.

Rostrum targets any current Linux desktop on PipeWire and WirePlumber. Kubuntu is the development
machine, not the only supported system. Plasma, GNOME and other desktops are in scope. X11 is a
fallback.

**Status:** v1 is feature-complete and smoke-tested on Kubuntu. The headset, OBS and reboot checks in
[docs/manual-tests.md](docs/manual-tests.md) still need a person with the hardware. The repo is
private for now and is written so it can be made public later.

### ✍️ Authors

Rostrum is made by [Rostrum Audio](https://github.com/rostrum-audio). More at
[getrostrum.dev](https://getrostrum.dev).


## 🚀 Usage

Five minutes to a split stream:

1. **Start Rostrum** from the app menu (after installing, see below) or run `./build/src/app/rostrum`.
2. **Run the wizard.** Welcome → Headphones (press Test: a short chime plays only there) → Mic (speak
   and watch its meter) → Buses → **Create Mix**. Skip creates the same mix with the defaults.

   ![First-run wizard, step 1 of 4: apps go to buses, buses go to your headphones and to OBS](docs/screenshots/wizard.png)

3. **Put apps on buses.** Start Discord, your game and Spotify. On the Mixer, drag each app chip
   onto its bus, or open Apps and press Move. Leave "Always" on and the app goes straight to that
   bus whenever Rostrum sees it, and from your next login on even before Rostrum starts. Apps that
   report no name (Wine and Proton games) get a banner so you can name them once.

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

6. **Save the scene** (Ctrl+S or the scene menu in the header). Scenes recall every level, mute and
   destination. Make a second one for "Just Chatting" and switch with Meta+Alt+PgDown.

### ⌨️ Shortcuts

Default global shortcuts, rebindable in Settings or in System Settings → Shortcuts on Plasma:

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
- Solo is not saved in the scene.
- Close window hides to tray (when the desktop has one). Quit from the tray or Settings.
- Scroll-to-adjust faders: on. Confirm scene switch: off. Launch at login: off.

### 🗂️ Where things live

- Settings and scenes: `~/.config/rostrum/` (TOML). Log: `~/.local/state/rostrum/rostrum.log`.
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
- Qt 6.5+ (with Qt WebSockets), KDE Frameworks 6 (Kirigami, Kirigami Addons, GlobalAccel,
  StatusNotifierItem, CoreAddons, I18n, DBusAddons), toml++ 3.

Base packages on other distros (add the Kirigami and KDE Frameworks packages above):

| Distro | Packages |
| --- | --- |
| Debian, Ubuntu, Kubuntu | `pipewire` `pipewire-pulse` `wireplumber` `qt6-base-dev` `qt6-websockets-dev` `libpipewire-0.3-dev` `libtomlplusplus-dev` `cmake` `ninja-build` `pkg-config` |
| Fedora | `pipewire` `pipewire-pulseaudio` `wireplumber` `qt6-qtbase-devel` `qt6-qtwebsockets-devel` `pipewire-devel` `tomlplusplus-devel` `cmake` `ninja-build` `pkgconf-pkg-config` |
| Arch | `pipewire` `pipewire-pulse` `wireplumber` `qt6-base` `qt6-websockets` `pipewire` `tomlplusplus` `cmake` `ninja` `pkgconf` |

Smoke-tested on **Kubuntu 26.04 LTS, KDE Plasma 6 on Wayland**, PipeWire 1.6.2, WirePlumber 0.5.13,
Qt 6.10, KDE Frameworks 6.24.


## 💭 Feedback and Contributing

- 🐛 Found a bug or want a feature? [Open an issue](https://github.com/rostrum-audio/rostrum/issues).
- ✉️ Questions and feedback: hello@getrostrum.dev
- 🔒 Security problems: security@getrostrum.dev. See [SECURITY.md](SECURITY.md).

Contributions are welcome. Run `ctest --test-dir build` before sending a change. Headset, OBS and
reboot checks stay manual: walk through [docs/manual-tests.md](docs/manual-tests.md) for anything
that touches audio routing.

### 🛠️ Development

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
./build/src/app/rostrum
```

Configure with `-DROSTRUM_BUILD_APP=OFF` to build only the engine, tools and tests (CI does this).
Logs go to `~/.local/state/rostrum/rostrum.log`; a crash appends a backtrace there.


## 📖 Further reading

- [How the audio side works, and why](docs/audio.md)
- [Manual test plan](docs/manual-tests.md)
- [Security policy](SECURITY.md)
- [getrostrum.dev](https://getrostrum.dev)


## 📄 License

Apache-2.0. See [LICENSE](LICENSE).
