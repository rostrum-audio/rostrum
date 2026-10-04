# Rostrum 0.1.0 — draft release notes

**Local candidate only; not published.** Source commit:
`ef7ea69036c89340e7387761a1b1ebc598a3e3d2`. Release date is not assigned.

Rostrum routes application audio into separate buses for headphones and streaming,
with local microphone filtering and OBS integration. This candidate includes live
routing-exclusion fixes, stream-readiness checks and a microphone converter fix
that preserves delivery when PipeWire changes its graph quantum.

## Changes in this candidate

- Check stream readiness on the OBS page: grouped results, summary counts,
  attention items first, corrective links where available and expandable evidence.
  Results distinguish Verified, Needs attention, Intentionally excluded/idle and
  Not verified. The check observes controls, devices, channel links and OBS capture
  observations; it does not change settings, play tones or guarantee audience audio.
- Live destination changes remove obsolete sends: Headphones Only excludes Stream
  Mix, and Stream Only excludes headphones.
- Microphone filters use output/merge conversion so client-driven quantum changes
  do not leave alternating silent blocks. Old persistent converters are migrated
  without resetting settings; replacing the converter may briefly interrupt its
  processing path on the first restart after upgrade.
- x86_64 AppImage packaging includes Qt/KDE/QML resources and Breeze icons, with
  private-session signal validation, runtime dependency checks and local updater
  checksum tests. Audio remains local. Official builds offer optional crash reports
  and update checks, chosen during setup; see [privacy.md](privacy.md).

## Artifact and integrity

File: `Rostrum-0.1.0-x86_64.AppImage`

Local artifact:
`build-appimage-validation/out/mic-filter-ef7ea69/Rostrum-0.1.0-x86_64.AppImage`

SHA-256:

```text
68118e2c58b6c6a88f02c31635cff0523a7beae6a4f8efb1606531759b70ada1
```

The companion `SHA256SUMS` verifies this exact candidate. Earlier AppImages without
this microphone converter fix are outdated. Public download/feed URLs are not
assigned in this draft; local feed fixtures are not production feeds.

## Tested environment and host requirements

The complete packaged functional check passed in a clean **Ubuntu 26.04 x86_64**
runtime container: **glibc 2.43, PipeWire 1.6.2, WirePlumber 0.5.13**, without host
Qt/KDE/QML or development packages. Audio sessions used private directories,
private D-Bus and fake devices, with no host audio sockets and external network
disconnected. X11 checks used Xvfb and software rendering.

This artifact requires **glibc 2.43 or newer** because its bundled ELF libraries
reference symbols through 2.43. This is a binary requirement, not a claim that all
systems with that glibc work. Only the environment above has complete clean-runtime
validation. There is no aarch64 build and older glibc is unsupported by this artifact.

A working PipeWire/WirePlumber audio session, desktop D-Bus, X11 or Wayland and a
usable graphics/font environment are required. Source-level version checks allow
PipeWire 1.0+ and WirePlumber 0.5+; microphone filters additionally require PipeWire
1.4+ audio-convert graph support. Runtime compatibility of this candidate is verified
only on PipeWire 1.6.2/WirePlumber 0.5.13. Use the host PipeWire client library and
modules from the same installation. The DSP plugin loads into the host daemon and
requires only libc/libm, with RNNoise compiled in.

The clean Ubuntu runtime explicitly installed these packages, plus dependencies:
`pipewire-bin wireplumber dbus libgl1 libegl1 libopengl0 libfontconfig1 libharfbuzz0b
fonts-dejavu-core libxkbcommon0 libxcb-cursor0 libsm6 libice6`. Qt, KDE, Kirigami,
QML and theme resources are bundled. Python and Xvfb are validation dependencies,
not ordinary launch requirements. Normal mounted launch needs working FUSE;
extract-and-run is available without it.

## Launch

Fully quit any running Rostrum through its application/tray **Quit** action first.
Closing its window can leave it running. A second launch can activate the existing
instance rather than the AppImage. The installed build and AppImage share settings,
scenes and routing rules; changing XDG paths alone does not isolate audio.

From the artifact directory:

```sh
sha256sum -c SHA256SUMS
chmod +x Rostrum-0.1.0-x86_64.AppImage
./Rostrum-0.1.0-x86_64.AppImage
# Without working FUSE:
./Rostrum-0.1.0-x86_64.AppImage --appimage-extract-and-run
```

To return to the installed normal build, fully quit the AppImage, then launch
`/home/william/.local/bin/rostrum`. No settings reset is needed. Do not run two copies
against the same audio session for a comparison.

## Validation and confirmed manual results

Reused applicable results: **28/28 normal tests**, **23/23 official tests** and five
successful complete packaged validations of the unchanged microphone fix. The
committed candidate additionally passed its focused scheduling/migration regression
and one complete clean packaged-runtime run under default scheduling, without
forcing a quantum, relaxing signal thresholds or skipping microphone validation.

Packaged functional coverage includes saved-setup mix creation, twelve live Desktop
destination transitions with intended stereo delivery and excluded silence,
440 Hz at hardware/Filtered Mic/Rostrum Mic before/during/after a low-latency client,
20 Hz attenuation, host DSP mapping, RNNoise loading, first-run/readiness rendering
and local updater checksum rejection/verified replacement. Fixture replacement is
not launched; rendering does not establish interactive setup completion.

The user confirmed these **manual passes for this exact ef7ea69 AppImage**
on 2026-10-04:

- Continuous microphone audio.
- Both → Headphones Only → Both isolation.
- Rostrum mic mute.
- OBS-page keyboard navigation.
- Window resizing without clipped or unreachable controls.
- Tray Quit and relaunch with settings preserved.
- Microphone/headphone unplug and reconnect recovery.

The complete manual-session environment and test duration were not reported.
These passes do not imply every release-check item or filter-quality check passed.

Supplementary isolated/offline checks passed on the development host:

- QML component keyboard, accessible-metadata and narrow-layout checks at 100%
  and 200% Qt scaling (mock results, not a real screen reader).
- FUSE-mounted first-run wizard rendering in a private audio/D-Bus session.
- Packaged OBS page rendering at its minimum 960×600 logical window size at
  100% and 200% scaling. This is the initial page, not populated readiness results.
  Offscreen rendering does not establish desktop theme-icon behavior.
- Offline audit of 243 ELF files: highest referenced glibc version is 2.43;
  DSP dependencies are limited to libc/libm; RNNoise/Breeze license notices exist.
- Bundled desktop/AppStream metadata validation and a generated local-only feed
  fixture with the exact artifact size and checksum.

These are supplementary host checks, not another clean-runtime campaign. Logs,
screenshots and the retained first harness attempt are in the artifact's
`release-checks/`; `VALIDATION.md` records their scope. The first attempt stopped
because completed test clients had not been unregistered from the temporary
harness; this was corrected, then the full supplementary check passed. No product
failure or audio assertion was bypassed. Passed audio campaigns were not repeated.

Additional isolated UI checks navigated this candidate's actual first-run wizard
through Create Mix on private Xvfb. The Mixer opened, setup version 3 and the Live
scene were saved, and the output route to fake headphones was verified. OBS setup
was skipped; no live OBS setup or real-device first-run test is inferred.

A local feed replaced a disposable older runnable AppImage with this candidate.
Clicking the actual Restart Now button exited the old process and started a new
PID whose running ELF hash matches this candidate. All disposable settings groups
except expected updater bookkeeping and all saved scene files were preserved.
Both binaries report 0.1.0; synthetic feed version 9.9.9 only triggers the update.
This proves runnable replacement/restart, not a real public-version upgrade. The
fixture is never published. Evidence is in `interactive-checks/` beside the artifact.

## Known limitations and remaining release gates

- Compatibility on other distributions, older PipeWire versions and physical
  device combinations remains unverified. Short captures and settled-link checks
  do not establish zero transient leakage at every sample or unlimited uptime.
- Readiness observes configuration and routing; unsupported, incomplete, failed or
  stale OBS observations remain Not verified. It cannot guarantee recorded tracks
  or audience sound. Silent meters and intentionally excluded/idle applications
  are not automatically failures.
- Interactive first-run setup through Create Mix passed in isolation; review remaining Mixer/Mic Filters/
  dialogs and theme icons on the intended desktop, remaining tray actions, global
  shortcuts and autostart. Tray Quit/relaunch is confirmed; other tray behavior is
  not inferred from it.
- OBS-page keyboard navigation and window resizing are confirmed. Keyboard use
  elsewhere, manual 200% scaling and a real screen reader remain unreported.
  Component tests cover accessible metadata, not screen-reader behavior.
- Unplug/reconnect recovery is confirmed for the user's microphone/headphones.
  Other hardware combinations, explicitly enabled microphone fallback, per-app
  microphone choices, subjective DSP/RNNoise voice quality, sidetone/ducking and
  relevant reboot behavior remain unverified by these manual reports.
- Wayland/GPU/desktop integration and interactive FUSE-mounted use remain manual.
- Runnable AppImage replacement and Restart Now passed with a local fixture.
  Public-feed upgrading between real release versions remains untested and must
  be checked on the actual release assets.
- Before public release, verify version/tag/metainfo consistency, intended release
  notes and licenses, exact-build Sentry symbols and the hosted workflow. No hosted
  release workflow has been run for this local candidate. Verify public feed wiring
  and usable assets as part of publishing; local fixtures do not establish them.

There is no confirmed remaining audio-delivery blocker in the tested environment.
The outstanding manual/operational gates above remain open; this draft is not an
approval to publish. See [releasing.md](releasing.md) and
[manual-tests.md](manual-tests.md) for the full checklists, and
[release-plan-0.1.0.md](release-plan-0.1.0.md) for the candidate-specific release gates
and version/tag/feed/CI sequence. This draft is reviewed and finalized as the local
candidate record; it remains unpublished until those gates are resolved.
