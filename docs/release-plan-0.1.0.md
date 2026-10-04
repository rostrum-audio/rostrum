# Release plan: validated 0.1.0 x86_64 candidate

Release preparation only: intended date **2026-10-04**, version **0.1.0**, stable
AppStream metadata. Nothing is published by these metadata changes. No tag, GitHub
release, public feed or website change is authorized. Earlier artifacts and their
completed tests are retained; unchanged audio campaigns need not be repeated.

## Retained manually tested local baseline

| Item | Value |
| --- | --- |
| Application source | `ef7ea69036c89340e7387761a1b1ebc598a3e3d2` |
| Artifact | `build-appimage-validation/out/mic-filter-ef7ea69/Rostrum-0.1.0-x86_64.AppImage` |
| Size | 74,394,104 bytes |
| SHA-256 | `68118e2c58b6c6a88f02c31635cff0523a7beae6a4f8efb1606531759b70ada1` |
| Complete clean runtime | Ubuntu 26.04 x86_64, glibc 2.43, PipeWire 1.6.2, WirePlumber 0.5.13 |
| Compatibility statement | This tested environment only; glibc 2.43 is the artifact's binary floor, not proof of other distribution compatibility |
| Launch paths checked | Extract-and-run in clean runtime; FUSE-mounted wizard in a private development-host session |

The artifact directory holds `SHA256SUMS`, `VALIDATION.md`, runtime logs/reports and
supplementary `release-checks/`. No application rebuild was made for this release
record. [Release notes](release-notes-0.1.0-draft.md) give dependencies and launch
instructions. The docs commit is separate from the artifact's application source.

## Candidate-specific checklist

| Check | Status and scope |
| --- | --- |
| Normal/official tests | Reused 28/28 normal and 23/23 official passes; official suite does not enable audio integration |
| Converter scheduling and legacy migration | Focused real-engine regression passed; signal oracle unchanged |
| Complete default-scheduling runtime | Five previous passes on unchanged fix plus one complete pass on this committed artifact |
| Stereo destination exclusions | Automated intended delivery and excluded silence; user-confirmed Both → Headphones Only → Both OBS recording |
| Mic recording and mute | User-confirmed continuous microphone audio and Rostrum mic mute in OBS recording |
| OBS-page keyboard and resize | User-confirmed navigation and resizing without clipped/unreachable controls; no claim for every other page |
| Tray Quit/relaunch | User-confirmed settings preserved; not an autostart/reboot or all-tray-actions pass |
| Mic/headphone unplug/reconnect | User-confirmed recovery for their devices; not proof of explicit fallback, every device or reboot behavior |
| Narrow/scaled rendering and accessible metadata | Automated component and initial-page checks passed; manual 200% and screen-reader behavior unreported |
| Remaining listed desktop interactions | User confirmed all listed checks in their current setup; no other desktop/GPU/hardware compatibility inferred |
| First-run setup | Actual candidate wizard navigated through Create Mix on private Xvfb; Mixer opened, setup version 3/scene saved and fake-output route verified; OBS setup skipped |
| DSP/plugin | Host daemon loading, deterministic delivery, rumble attenuation and RNNoise chain loading passed; subjective voice quality unreported |
| Updates | Local checksum tests plus runnable older-image → candidate replacement and UI Restart Now passed; new PID/ELF identity and disposable settings/scenes verified; feed version synthetic, public-version upgrade still untested |
| Bundle integrity | SHA256SUMS, 243-ELF audit, DSP libc/libm dependencies, metadata and license presence passed; AppStream uppercase-ID pedantic notice is not a failure |
| Hosted release workflow | Separate empty-Tag build at 5c0ccd4 passed official and complete private-runtime validation; this does not transfer manual passes to those bytes |
| Public feed/downloads | Not verified usable for this candidate; no public request or update in this task |

Earlier readiness-panel manual observations and historical smoke logs keep their
original scope. They do not automatically become passes for this artifact.

## Current source and hosted evidence

Before this metadata update, local main matched origin/main at
`01686933e22663006e2731befbc9c87f8d8f4005`, with a clean working tree. That commit
fixes the final lifecycle cleanup acknowledgement defect found in run 37188483352.
Its focused final-write regressions, full lifecycle/fixture tests and both
[PR CI](https://github.com/rostrum-audio/rostrum/actions/runs/37189404292) and
[main CI](https://github.com/rostrum-audio/rostrum/actions/runs/37189727954) passed:
Ubuntu unit 24/24, Ubuntu app 22/22, Fedora 23/23 and Arch 23/23. Arch is configured
nonblocking but actually passed. These build jobs do not prove Fedora/Arch AppImage
compatibility. Evidence is retained in
`build-appimage-validation/out/lifecycle-final-ack-08cfee5/`.

The previous [empty-Tag Release run](https://github.com/rostrum-audio/rostrum/actions/runs/37188483084)
at source `5c0ccd41e0535d47052977f9d2267a51854704f4` passed 23/23 official tests
and complete runtime-only Ubuntu 26.04 validation under default scheduling.
Publication and symbol upload were skipped. That independent artifact has SHA-256
`386f3bb265a355cecc5c55cb34201fb9307ae4c3e964f4576abad847523ba730`, with source,
logs and reports in `build-appimage-validation/out/hosted-5c0ccd4-run37188483084/`.
Its then-failing regular CI is historical; the lifecycle fix and subsequent green
CI above resolve that defect without erasing the original evidence.

## Metadata and release notes

- CMake and the intended artifact filename identify 0.1.0; no version bump.
- Source metainfo now explicitly sets type **stable** and intended date
  **2026-10-04**, as authorized for release preparation. Stable metadata is not
  evidence that publication occurred. If the intended date changes, update it
  through review and rebuild before publication.
- Description and [release notes](release-notes-0.1.0-draft.md) include stream
  readiness, live destination exclusions and microphone delivery across quantum
  changes. Readiness observes evidence; it does not guarantee recorded/audience
  audio. Unsupported or stale OBS observations stay Not verified.
- The compatibility claim remains the tested Ubuntu 26.04 x86_64 environment:
  glibc 2.43, PipeWire 1.6.2, WirePlumber 0.5.13. Source minimum versions are not a
  tested runtime matrix. No other architecture/distribution/older-glibc claim.
- `tools/release/set-version.py` updates an existing entry's date, not its type or
  description. Neither release workflow enforces stable type; maintainers must
  inspect the exact source and embedded metainfo.

## Nonpublishing build sequence

1. Validate AppStream and documentation consistency; commit only focused metadata
   and release docs. Integrate through a PR with required Ubuntu unit and Fedora
   checks; no protected-branch bypass. Monitor the resulting main CI.
2. Dispatch **Release** on that resulting main commit with **Tag empty**:

   ```sh
   gh workflow run release.yml --repo rostrum-audio/rostrum --ref main -f tag=
   ```

   Confirm the run's source SHA and both build/runtime checkout SHAs match the
   frozen main commit. The build exports its exact checkout for runtime validation.
3. Require official tests, AppImage build/resource/dependency checks and the complete
   runtime-only private-audio validator to pass. Confirm `publish` job and
   `Upload debug symbols to Sentry` step explicitly report skipped.
4. Download the AppImage, SHA256SUMS, runtime reports and generated notes into a
   **new** `build-appimage-validation/out/hosted-<source>-run<id>/` directory. Verify
   checksum and the generated fixture feed's size/hash, and inspect embedded
   version/date/type. Save exact source, run inputs, package/host versions, logs
   and results in its own `VALIDATION.md`. Empty-Tag notes default to
   “Rostrum 0.1.0”; prepare the full public notes separately for eventual tagging.
5. Keep generated `latest.json` local: its future release URLs are not usable public
   assets. Do not copy it to the public feed. New bytes require their own short
   manual smoke check; old manual passes retain the baseline/setup scope above.
   No unchanged audio stability campaign, local live-audio launch or install is
   required for these metadata-only changes.

## Remaining publication gates

No confirmed audio-delivery defect remains in the tested environment. The following
are operational gates, not permission to publish:

1. Complete and review the new stable-metadata artifact's hosted validation and
   exact source/checksum record; perform a short final-artifact desktop/OBS smoke
   test. Do not transfer the baseline's manual passes to new bytes automatically.
2. Confirm release permissions, remote `v0.1.0` absence, final plain-text tag notes
   and bundled license review. Decide whether exact-build Sentry symbols will be
   uploaded or explicitly omitted. Empty-Tag builds skip symbol upload by design.
3. Obtain explicit publication authorization. The workflow rebuilds on a final tag;
   it has no promotion input for existing artifacts. Any final-tag rebuilt bytes
   have a new identity and must pass build and runtime gates before publication.
4. Verify actual downloadable AppImage/SHA256SUMS/latest.json after publication and
   confirm the public HTTPS feed serves or redirects to the exact GitHub feed
   without downgrade. Public-feed usability cannot be established by fixtures.
5. Test the eventual real public-version upgrade on disposable settings/private
   audio. The previous runnable replacement and Restart Now passed using synthetic
   feed version 9.9.9 and two real 0.1.0 images; that is not a public-version upgrade.
   For a first release without an older public version, retain this as a limitation
   and later-version gate; do not claim it passed or announce verified public updates
   before usable assets/feed and the applicable upgrade test exist.

When authorized, use Prepare release with Dry run first, inspect its diff/date and
then follow [releasing.md](releasing.md). Metadata changes must go through protected
review; do not use a privileged token to bypass PR/status rules. Non-dry-run Prepare
release or an annotated `v0.1.0` tag can publish assets and upload symbols. Neither
is authorized in this preparation task. The website is external to Release and is
not modified by this repository's workflow.

## Documented limitations and smoke checklist

No broad hardware, desktop/GPU/Wayland or older PipeWire compatibility claim; no
screen-reader/manual-200% claim; no every-sample transient isolation or unlimited
uptime guarantee. Subjective DSP/RNNoise quality, explicit fallback and unreported
per-app/reboot checks retain their unverified status. These are documented limits
unless adopted as intended support requirements, in which case their checks become
gates. Confirmed baseline desktop checks and interactive first-run/local restart
checks above do not need a new full campaign.

For the new artifact, first verify SHA256SUMS. Fully quit the existing instance via
tray/menu Quit before opting into a live-session test; closing the window can only
hide it, and a second launch may activate the old instance. Both builds share
settings. Merely changing XDG paths does not isolate audio. This preparation task
must leave the user's running instance and configuration untouched.

Short user smoke checklist for the newly built image:

- Confirm the launched executable belongs to this artifact; Mixer/OBS/Mic Filters
  render, navigation and resize work, and existing settings/scenes remain present.
- Make a fresh OBS recording: continuous filtered mic; Desktop Both → Headphones
  Only → Both gives audible → absent → audible stream audio while headphones continue;
  Rostrum mic mute removes the mic from the recording.
- Readiness: correct captures display scoped Verified evidence, Headphones Only is
  intentionally excluded, muted required capture needs attention, and OBS closed
  removes previous verified capture rows and reports Not verified.
- Tray Quit and relaunch preserve settings. Fully quit before returning to
  `/home/william/.local/bin/rostrum`.

No tag, GitHub release, public asset/feed or website publication is performed by
this plan or the authorized empty-Tag workflow run.
