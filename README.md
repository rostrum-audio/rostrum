# Rostrum

A stream mix console for Linux, not a patchbay.

Rostrum splits your game, voice chat, mic, music, alerts and desktop audio into named buses,
sends each bus to your headphones, to the stream, or to both, and gives OBS one clean
`Rostrum Stream Mix` and one `Rostrum Mic` to capture. Scenes recall the whole mix with one click.

Status: early development. The repo is private for now and is written so it can be made public later.

Rostrum targets any current Linux desktop on PipeWire and WirePlumber. Kubuntu is the development
machine, not the only supported system. Plasma, GNOME, and other desktops are in scope. X11 is a fallback.

## Development machine

Smoke-tested on **Kubuntu 26.04 LTS, KDE Plasma 6 on Wayland**, PipeWire 1.6.2, WirePlumber 0.5.13,
Qt 6.10, KDE Frameworks 6.24.

## Requirements

- PipeWire 1.0 or newer and WirePlumber 0.5 or newer. PulseAudio is not used or required.
- Qt 6.5+, KDE Frameworks 6 (Kirigami, Kirigami Addons, GlobalAccel, StatusNotifierItem,
  Config, CoreAddons, I18n, DBusAddons, IconThemes), toml++ 3.

Packages to install before building:

| Distro | Packages |
| --- | --- |
| Debian, Ubuntu, Kubuntu | `pipewire` `pipewire-pulse` `wireplumber` `qt6-base-dev` `libpipewire-0.3-dev` `libtomlplusplus-dev` `cmake` `ninja-build` `pkg-config` |
| Fedora | `pipewire` `pipewire-pulseaudio` `wireplumber` `qt6-qtbase-devel` `pipewire-devel` `tomlplusplus-devel` `cmake` `ninja-build` `pkgconf-pkg-config` |
| Arch | `pipewire` `pipewire-pulse` `wireplumber` `qt6-base` `pipewire` `tomlplusplus` `cmake` `ninja` `pkgconf` |

Kirigami and the other KDE Frameworks packages are only needed once the UI target exists.
Headset, OBS, and reboot checks stay manual. See [docs/manual-tests.md](docs/manual-tests.md).

## Build

On Kubuntu 26.04:

```sh
sudo apt install build-essential cmake ninja-build pkg-config extra-cmake-modules \
  libpipewire-0.3-dev libtomlplusplus-dev qt6-base-dev qt6-declarative-dev \
  libkirigami-dev kirigami-addons-dev libkf6coreaddons-dev libkf6dbusaddons-dev \
  libkf6i18n-dev libkf6globalaccel-dev libkf6statusnotifieritem-dev libkf6notifications-dev \
  qml6-module-org-kde-kirigami qml6-module-org-kde-kquickcontrols qml6-module-org-kde-desktop
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
./build/src/app/rostrum
```

Logs go to `~/.local/state/rostrum/rostrum.log`; a crash appends a backtrace there.

## License

Apache-2.0. See [LICENSE](LICENSE).
