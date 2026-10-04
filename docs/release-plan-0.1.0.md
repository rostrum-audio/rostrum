# Release plan: validated 0.1.0 x86_64 candidate

Local planning only. No publication, tag, push, website or public-feed change is
performed by this document. The candidate and its completed tests are retained;
unchanged audio campaigns do not need another run for a documentation update.

## Exact candidate

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
| First-run setup | Actual candidate wizard navigated through Create Mix on private Xvfb; Mixer opened, setup version 3/scene saved and fake-output route verified; OBS setup skipped |
| DSP/plugin | Host daemon loading, deterministic delivery, rumble attenuation and RNNoise chain loading passed; subjective voice quality unreported |
| Updates | Local checksum tests plus runnable older-image → candidate replacement and UI Restart Now passed; new PID/ELF identity and disposable settings/scenes verified; feed version synthetic, public-version upgrade still untested |
| Bundle integrity | SHA256SUMS, 243-ELF audit, DSP libc/libm dependencies, metadata and license presence passed; AppStream uppercase-ID pedantic notice is not a failure |
| Hosted release workflow | Not run for this local candidate; local results do not prove hosted CI completion |
| Public feed/downloads | Not verified usable for this candidate; no public request or update in this task |

Earlier readiness-panel manual observations and historical smoke logs keep their
original scope. They do not automatically become passes for this artifact.

## Blockers and documented limitations

No confirmed audio-delivery defect remains in the tested environment. The following
are **open release gates**, not confirmed product failures:

1. Interactive first-run setup through Create Mix is now confirmed in isolation.
   Inspect the intended desktop's remaining Mixer, Mic Filters, dialogs and
   theme icons. Confirm remaining essential tray actions, shortcuts and autostart.
   Reuse the confirmed Quit/relaunch and OBS navigation/resize tests.
2. Verify the eventual real public-version upgrade as part of the actual release
   sequence. Runnable replacement and Restart Now already passed locally with
   disposable settings; synthetic feed 9.9.9 and two real 0.1.0 binaries do not
   close the public-version upgrade check.
3. Choose the final version/date/notes, resolve metainfo consistency and remote tag
   availability, and validate the exact CI-built release artifact before publishing.
4. Confirm publishing permissions, feed/download destinations and usable published
   assets, plus release-license review and the symbol-upload decision. Public feed
   usability is a stable auto-update release gate; it cannot be proved by local
   fixtures. Finish the post-publication checks before announcing auto-updates.

The following are **documented limitations**, not reasons to repeat passed audio
campaigns: no other distribution or older PipeWire runtime claim, no aarch64 or
older-glibc build; no every-sample transient-isolation or unlimited-uptime guarantee;
no screen-reader/manual-200% claim; no broad hardware, filter-quality, per-app,
explicit-fallback, reboot, GPU/Wayland or portal compatibility claim. Do not advertise
these as supported until tested. If they become intended release support claims,
move their matching checks into the release gates first. Remaining items in
[manual-tests.md](manual-tests.md) retain their historical/unverified status unless
specific current evidence exists.

## Required release sequence — future authorization needed

1. **Freeze scope and identity.** Preserve this candidate and checksum as the tested
   baseline. Choose `0.1.0` for this first release unless requirements change.
   CMake currently says `0.1.0`; metainfo has a `0.1.0` entry dated `2026-10-03`.
   The actual publication date is not chosen. No local tags exist; remote tag
   availability has not been checked in this task.
2. **Prepare metadata and notes.** On the future release branch/main, confirm release
   date and metainfo description. `tools/release/set-version.py` updates the date
   of an existing final entry but does not rewrite that entry's description; review
   the existing text explicitly. Validate with `appstreamcli validate --no-net`.
   The full draft documents compatibility; the tag/Prepare release input needs a
   short plain-text summary, for example: “Rostrum's first x86_64 AppImage includes
   stream readiness checks and local microphone filters. Live output exclusions
   and microphone delivery across PipeWire quantum changes are validated on
   Ubuntu 26.04.” Include the supported-environment/limitations information with
   the public release materials rather than implying broad Linux compatibility.
3. **Run nonpublishing hosted CI.** After separately authorized source synchronization,
   run Actions → Release with an **empty tag** on the frozen source. This builds
   `official`, runs its tests, packages, writes checksums/feed fixtures and runs the
   exact-source runtime-only private-audio job. Require every job to pass; inspect
   artifacts and logs. This workflow test mode uploads artifacts but does not
   publish a GitHub release or symbols. Do not execute it during this docs task.
4. **Account for rebuilt bytes.** Existing `.github/workflows/release.yml` always
   rebuilds; it has no promote-existing-artifact input. A CI image may differ even
   with unchanged application source or docs-only commits. Its checksum, version,
   dependency floor and runtime results must be recorded independently. The manual
   passes above belong to the `68118e…` candidate, not automatically to new bytes.
   Perform relevant final-artifact smoke checks; repeat full audio campaigns only
   for changed source/environment or a failure. Publishing these exact existing
   bytes would require a separately authorized promotion approach; none is added
   or assumed here. Metadata/version changes also require a new artifact.
5. **Prepare the tag only after release gates pass.** Confirm the remote `v0.1.0`
   does not exist and that the chosen source contains the fixes. Run Prepare release
   with Dry run first. Its non-dry-run path may commit metadata, push and tag and
   invokes Release; it is a publishing action. Alternatively use the documented
   annotated-tag procedure. A final tag must match CMake and have a metainfo entry.
   Protected-main metadata commits require `RELEASE_TOKEN`; symbol upload uses
   optional `SENTRY_AUTH_TOKEN` and must match the exact unstripped binary. Missing
   symbols need an explicit decision; they do not block runtime functionality.
6. **Require publishing-job dependencies.** For the actual final-tag build, `publish`
   waits for both `appimage` and `validate-runtime`. Require checksum verification
   and complete private-audio validation, not just launch smoke. Review its newly
   generated `SHA256SUMS` and `latest.json`; the latter must describe the exact final
   release asset's URL, size, checksum and notes. A suffixed prerelease tag omits
   `latest.json` and is not offered by the stable updater.
7. **Publish and verify only with explicit authorization.** The final release uploads
   the AppImage, `SHA256SUMS` and (stable only) `latest.json` to GitHub. Verify actual
   downloads and checksums. The workflow does not update the website: confirm that
   `https://getrostrum.dev/releases/latest.json` serves or redirects to
   `https://github.com/rostrum-audio/rostrum/releases/latest/download/latest.json`
   without an HTTPS downgrade and that the feed describes usable assets. Resolve
   feed problems before announcing stable updates. Test the public feed/update from
   an older disposable AppImage, then archive exact source, symbols and evidence.

No commands in this sequence have been dispatched to GitHub, and no release tag,
public feed or website has been changed as part of finalizing these documents.

## Remaining preparation after setup/restart validation

The setup/update evidence has been reviewed against screenshots, saved disposable
settings, `executable-proof.json` and successful private-session cleanup. Restart
changed the PID and running ELF from the older image to the independently extracted
candidate binary. This remains a synthetic 9.9.9 feed with real 0.1.0 binaries,
not evidence of a public-version upgrade.

### Essential desktop checks still unconfirmed

- On the intended desktop, inspect remaining Mixer and Mic Filters interactions,
  file/settings dialogs and theme icons for usable controls and obvious errors.
  The private Xvfb wizard/Mixer test is narrower than the full physical desktop.
- Check tray Show/Hide and remaining essential tray mute/scene actions. Quit and
  relaunch with preserved settings already passed and need not be repeated.
- Check global mute/scene shortcuts and autostart/start-hidden behavior with a
  disposable profile. Xvfb had no tray host/global-shortcut service; those checks
  must use a controlled desktop session without altering the user's real bindings
  or login configuration.
- For intended native Wayland/GPU support, confirm the window renders and relevant
  dialogs behave on that desktop. Automated X11/software rendering does not prove
  this. If not tested, keep native Wayland/GPU behavior explicitly unverified.

Do not rerun confirmed OBS keyboard/navigation, resizing, tray Quit/relaunch,
microphone/headphone recovery, recording isolation/mute, first-run Create Mix or
local runnable replacement/Restart Now simply to fill this checklist. Screen-reader,
manual 200% scaling, non-Plasma shortcut portals and broad hardware coverage remain
limitations unless adopted as supported-release requirements.

### Metadata and CI/feed review

- Version: CMake, candidate filename and actual executable all identify 0.1.0.
  No version bump is required merely for this documentation update.
- Date: metainfo still has 2026-10-03; draft notes leave publication date unassigned.
  Select the actual release date later and update metadata before the release build.
  The current candidate keeps its original embedded metadata.
- Description: metainfo's existing first-release paragraph lists the older broad
  feature set and omits stream readiness. Review/add readiness and the delivery
  fixes when preparing release metadata. The version helper changes an existing
  entry's final date, not its description. AppStream validation passes with one
  pedantic uppercase-ID notice; validation does not resolve editorial consistency.
- Regular CI: `.github/workflows/ci.yml` runs on main pushes and PRs; Ubuntu unit
  enables isolated audio integration, app/Fedora jobs test app builds and Arch is
  explicitly nonblocking. These jobs do not establish AppImage compatibility on
  Fedora/Arch, nor is their current hosted status verified by local inspection.
- Release CI: `release.yml` builds in Ubuntu 26.04 on an Ubuntu 24.04 runner, then
  validates the AppImage in a runtime-only Ubuntu 26.04 job at the exact source
  commit. Publication requires both jobs. Empty-tag workflow dispatch sets publish
  false; symbols are skipped. The test build's notes default to “Rostrum 0.1.0”,
  so final tag notes must be prepared separately.
- Feed: CMake defaults to `https://getrostrum.dev/releases/latest.json`; stable
  release CI generates a GitHub asset feed with the final image's size/checksum.
  Website redirection is external to this repository's release workflow. Current
  live redirect, assets, remote tags, secrets, permissions and hosted job status
  have not been queried in this preparation task.

### Exact future actions (not executed)

1. Obtain authorization to synchronize the reviewed commits to a remote branch.
   Select that branch/ref and record its exact commit before dispatch. A remote
   workflow cannot test commits that exist only locally.
2. For a nonpublishing AppImage run, use Actions → **Release** → Run workflow,
   select that synchronized ref and leave **Tag** empty. CLI equivalent:

   ```sh
   gh workflow run release.yml --repo rostrum-audio/rostrum --ref main -f tag=
   ```

   Use `main` only after the reviewed source is actually synchronized there.
   Identify the new run ID, inspect its checked-out source commit, wait for the
   build and runtime jobs, and download its `rostrum-0.1.0-appimage`,
   `rostrum-runtime-check` and `rostrum-release-notes` artifacts. Verify SHA256SUMS,
   complete runtime results and host requirements. Confirm `publish` and symbol
   upload were skipped. A test `latest.json` may reference nonexistent future
   release URLs; do not install it as the public feed.
3. Complete the remaining essential desktop checks and choose final 0.1.0 date,
   metainfo text and short release summary. Retain the full compatibility/limits
   notes, linking the final-tag documentation from public release materials.
   Metadata edits require a separately reviewed release build; new bytes have
   their own checksum, not this candidate's checksum by inheritance.
4. After authorization, check remote tag absence and release permissions, and run
   **Prepare release** on main with Version `0.1.0`, finalized plain-text Notes and
   **Dry run checked**. Inspect its metadata diff and AppStream validation. If
   metadata changed, protected-main updates require `RELEASE_TOKEN`. Decide exact-
   build Sentry symbol handling; the upload requires `SENTRY_AUTH_TOKEN`.
5. Only after explicit publishing authorization, run Prepare release again with
   **Dry run unchecked**, or follow the annotated-tag procedure in releasing.md.
   That action may commit/push metadata, create/push `v0.1.0`, invoke release CI,
   upload symbols and publish. Require the final build/runtime checks to pass.
6. Verify the published AppImage/checksum and stable latest.json against that
   exact final build. Confirm the public HTTPS feed redirect and real downloadable
   assets; test a real versioned update from an older disposable image in isolation.
   Resolve any feed or restart problems before announcing stable auto-updates.

This task executes none of those remote or publishing actions. Existing validated
artifact and live configuration remain unchanged.
