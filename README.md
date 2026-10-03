# Rostrum

A stream mix console for Linux, not a patchbay.

Rostrum splits your game, voice chat, mic, music, alerts and desktop audio into named buses,
sends each bus to your headphones, to the stream, or to both, and gives OBS one clean
`Rostrum Stream Mix` and one `Rostrum Mic` to capture. Scenes recall the whole mix with one click.

Status: early development. Not published.

## Development machine

Developed and smoke-tested on **Kubuntu 26.04 LTS (Resolute Raccoon), KDE Plasma 6 on Wayland**,
PipeWire 1.6.2, WirePlumber 0.5.13, Qt 6.10, KDE Frameworks 6.24.

## Requirements

- PipeWire 1.0 or newer and WirePlumber 0.5 or newer. PulseAudio is not used or required.
- Qt 6.5+, KDE Frameworks 6 (Kirigami, Kirigami Addons, GlobalAccel, StatusNotifierItem,
  Config, CoreAddons, I18n, DBusAddons, IconThemes), toml++ 3.

## Build

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

## License

Apache-2.0. See [LICENSE](LICENSE).
