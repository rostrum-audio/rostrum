# Contributing to Rostrum

Thanks for helping. Rostrum is a stream mix console for Linux, not a patchbay: it hides the
PipeWire graph behind buses, faders and scenes. Bug reports, fixes, docs and testing on hardware
and desktops we don't have are all welcome.

By taking part you agree to follow the [Code of Conduct](CODE_OF_CONDUCT.md).

## Before you start

- **Bugs and ideas:** open an issue with one of the
  [issue forms](https://github.com/rostrum-audio/rostrum/issues/new/choose). Search existing issues
  first.
- **Security problems:** do not open a public issue. Follow [SECURITY.md](SECURITY.md).
- **Bigger changes** (new pages, a change to how the graph is built, new files written outside
  `~/.config/rostrum/`): open an issue first so we can agree on the approach before you write it.
- **Questions:** hello@getrostrum.dev.

## Build

You need PipeWire 1.0+ with WirePlumber 0.5+, Qt 6.5+ (with Qt WebSockets), KDE Frameworks 6 and
toml++ 3. The Installation section of the [README](README.md) lists the packages for Kubuntu,
Debian, Ubuntu, Fedora and Arch.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
./build/src/app/rostrum
```

Useful options:

- `-DROSTRUM_BUILD_APP=OFF` builds only the engine, the command-line tools and the tests, without
  Kirigami. CI builds this way, so it only needs Qt Base, Qt WebSockets, libpipewire and toml++.
- `-DROSTRUM_BUILD_TESTS=OFF` skips the unit tests.
- `-DROSTRUM_UPDATE_URL=…` points the update check at your own feed. See
  [docs/privacy.md](docs/privacy.md) for testing it against a local server.

Development builds have no crash reporting. The `official` CMake preset is for Rostrum's own
release builds: it sends crash reports to Rostrum's Sentry project, so don't use it for local
work, and a fork should use its own Sentry project or none (see
[docs/privacy.md](docs/privacy.md#builds-and-testing)).

Rostrum logs to `$XDG_STATE_HOME/rostrum/rostrum.log` (normally
`~/.local/state/rostrum/rostrum.log`). A crash appends a backtrace there.

## Tests

### Unit tests

```sh
ctest --test-dir build --output-on-failure
```

The tests live in `tests/` and use Qt Test. CTest runs each one with `QT_QPA_PLATFORM=offscreen`,
so they need no display, no running PipeWire and no network. To run a single test binary by hand,
set the same variable:

```sh
QT_QPA_PLATFORM=offscreen ./build/tests/tst_scenes
```

New logic in `src/core/`, `src/engine/` or `src/obs/` should come with a test. Add it to
`tests/CMakeLists.txt` with `rostrum_add_test()`.

### Manual tests

Anything that needs a real PipeWire session, a headset, OBS or a reboot stays manual. The plan is
[docs/manual-tests.md](docs/manual-tests.md). If your change touches audio routing, the graph,
the mic path, headphones, OBS or startup, walk through the matching sections and say in the pull
request which ones you ran and on what system.

`rostrum-graphtest` drives the same engine as the app without the UI, and
`tests/manual/graph-checks.sh` checks destinations, solo, mic and sidetone against fake devices so
nothing reaches your speakers.

## Rules that must not break

These protect people who are live on stream. A change that breaks one will not be merged, however
useful it is otherwise. Most of them have a unit test or a manual test behind them.

1. **Quitting or crashing never silences apps.** Rostrum's virtual devices are created with
   `object.linger` and live in PipeWire, not in the Rostrum process. Audio must keep flowing if
   Rostrum quits, crashes or is killed, and routed apps must fall back to the default sink when a
   bus is missing (manual tests 6 and 7).
2. **App rules never pin streams.** The rule fragments set only `target.object`. Never set
   `node.dont-fallback`, `node.dont-move` or `node.dont-reconnect` in a rule; without them
   WirePlumber can fall back to the default sink and the user can still move the stream in any
   other mixer. `tests/tst_ruleexport.cpp` fails if any of these keys appears. The only exception
   is Rostrum's own meter streams (`src/pw/MeterBank.cpp`), which are internal and never appear in
   rules.
3. **Rules persist in exactly two files:**
   `~/.config/pipewire/pipewire-pulse.conf.d/50-rostrum.conf` and
   `~/.config/pipewire/client.conf.d/50-rostrum.conf`. Nothing is written to
   `~/.config/wireplumber/`, and Rostrum does not ship WirePlumber Lua scripts. If routing at login
   is wrong, fix it in the router, not with a WirePlumber script. [docs/audio.md](docs/audio.md)
   explains why.
4. **Solo is never written to a scene.** It is a live monitoring aid, so it is not part of the
   scene TOML format and is not saved. `tests/tst_scenes.cpp` checks this.
5. **Crash reports do not grow.** `crash::scrubEvent()` in `src/core/CrashReport.cpp` copies a fixed
   list of fields, documented in [docs/privacy.md](docs/privacy.md). Don't add fields to the report,
   and don't weaken `tests/tst_privacy.cpp`, which checks that names, paths, device names and other
   personal data don't get through.
6. **The mic path test stays.** [docs/manual-tests.md](docs/manual-tests.md) keeps the explicit
   mic-path test (section 4). A doubled voice on stream is the most likely real-world failure, and
   unit tests can't catch it.

Also from the pull request checklist: update [docs/audio.md](docs/audio.md) when the graph or
routing changes, and keep distro-specific paths and commands out of the code, except in the help
text shown when PipeWire is missing.

## Code style

- **C++20 and Qt 6.** Format C++ with the repository's [`.clang-format`](.clang-format) (LLVM
  based, 4-space indent, 110 columns). Run `clang-format -i` on the files you change, and don't
  reformat files you otherwise leave alone.
- The build defines `QT_NO_KEYWORDS`: use `Q_SIGNALS`, `Q_SLOTS` and `Q_EMIT`, not `signals`,
  `slots` and `emit`.
- **Comments** are `//` and short. They explain why, or a constraint the code can't show (for
  example, why a PipeWire property must never be set), not what the next line does.
- **QML:** every user-visible string goes through `i18n()` or `i18nc()` with a context, and custom
  controls get an `Accessible.name` (and a role where Qt can't infer one). See
  [ACCESSIBILITY.md](ACCESSIBILITY.md).
- **Words:** UI text and docs use short, plain sentences and name things the way the app does
  (bus, scene, Headphones, Stream). Avoid PipeWire jargon in the UI.

## Commits

Look at `git log` for examples. A commit message has:

- A summary line in the imperative, sentence case, no trailing period, about 72 characters at most:
  `Assign apps to buses automatically`, `Stop Sentry from deriving a location from the upload's IP
  address`. A short area prefix is fine when it helps (`OBS: …`, `README: …`).
- A blank line, then a body wrapped at about 72 columns that says what changed and why, in plain
  sentences. Small changes can skip the body.

Keep each commit to one logical change. Squash fix-up commits before asking for review.

## Pull requests

`main` is protected. Every change goes through a pull request, and two CI checks must pass before
it can merge:

- **Ubuntu unit tests:** builds the engine, tools and tests on Ubuntu 26.04 and runs `ctest`.
- **Fedora build:** builds the engine and tools on the latest Fedora.

An Arch build also runs and is allowed to fail. CI does not build the Kirigami app, so build and
start it locally if you change anything in `src/app/`.

1. Fork the repository and create a branch from `main`.
2. Make the change, with tests where they fit.
3. Run `ctest --test-dir build` and the relevant [manual tests](docs/manual-tests.md).
4. Open a pull request and fill in the template: what changed, and the checklist.
5. A maintainer reviews it. Expect questions about anything that changes routing, files written
   outside `~/.config/rostrum/`, or what leaves the computer.

## License

Rostrum is licensed under [Apache-2.0](LICENSE). By contributing, you agree that your contribution
is licensed under the same terms, as described in section 5 of the license.
