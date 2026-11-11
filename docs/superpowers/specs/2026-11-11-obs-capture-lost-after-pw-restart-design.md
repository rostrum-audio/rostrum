# Detect OBS capture loss after PipeWire restart

Date: 2026-11-11  
Issue: [#21](https://github.com/rostrum-audio/rostrum/issues/21)  
Status: approved design

## Problem

After a **full PipeWire restart** while OBS is running, OBS’s audio captures of Rostrum devices stay silent until OBS itself is restarted. Rostrum already reconnects and recreates its graph; the break is in OBS’s clients.

Issue text that blames “new node serials” is **incorrect** for Set Up OBS sources: those are pulse captures addressed by **stable node names** (`rostrum.mic`, `rostrum.stream.monitor`, etc.). Smoke (E3, 2026-11-11) showed OBS had **no** capture streams in the graph after the restart; quitting and reopening OBS restored them.

Root cause (OBS 33 / linux-pulseaudio): one process-global `pa_context` whose state callback is an intentional no-op with an upstream `@todo` to reconnect. The PipeWire audio-capture plugin similarly holds a dead `pw_core` with no reconnect. Rostrum cannot revive those from outside.

## Goal

When Rostrum recovers from a PipeWire (or equivalent) disconnect that dropped OBS’s Rostrum captures, **detect** the loss and **tell the user to restart OBS**, with docs that match the real cause.

## Non-goals

- Auto-repairing OBS (websocket recreate, scene-collection bounce, settings toggles) in this change.
- Changing the policy that Rostrum never moves or rewrites OBS capture links.
- Fixing upstream OBS pulse reconnect (separate upstream request if desired).
- Guaranteeing recovery of third-party OBS app captures (e.g. Spotify) after a WirePlumber-only restart — document as related, out of primary scope.
- New user settings beyond reusing existing OBS-follow / go-live warning gates.

## Cases

| Case | What restarts | OBS Rostrum captures | This design |
|------|---------------|----------------------|-------------|
| **A — full PipeWire** | `pipewire` (+ pulse + WP) | Clients die; streams gone until OBS restarts | **In scope** — detect + warn |
| **B — WirePlumber only** | WirePlumber | Rostrum name captures typically stay linked | No alert for healthy Rostrum captures; optional note for app-capture unlink |

Primary product path is Case A.

## Behavior

### Detection (`obs::CaptureWatch` or equivalent)

Pure logic (injectable clock / OBS identity) fed from `Obs` / engine hooks:

1. **Baseline (while PipeWire Ready):** from `obs::obsRecordings(graph)`, keep a candidate set of OBS recordings whose far-end device name starts with `rostrum.`. Debounce ~2 s of stability into a **committed** baseline, plus OBS pid and process start time (extend `isObsRunning()` helpers; keep Flatpak-safe: use host `/proc` pid, not sandbox ids).
2. **Freeze:** on PipeWire leaving Ready (`Failed` / disconnect), freeze the committed baseline; drop the uncommitted candidate (avoids eroding the baseline as globals vanish before `-EPIPE`).
3. **Epoch:** on return to Ready (real reconnect, not every `stateChanged`), wait until `mixEnabled() && mixReady()` and a grace window (~8 s after mix ready, hard cap ~20 s after Ready). Re-arm on relevant `graphChanged` up to the cap.
4. **Alert if** all hold: same OBS pid/start time still running; mix healthy; gate settings on; at least one committed `(source, device)` is missing / not on that Rostrum device in current `obsRecordings`.
5. **Clear** when all baseline items reappear, OBS exits, or OBS pid changes (new OBS process). Dismiss is per epoch.

Evaluate only when PipeWire state is Ready (Failed leaves a stale graph; Connecting leaves empty).

### UX

- **Banner** separate from go-live problems (`Obs.captureLost` or similar): Warning normally, Error if OBS is known streaming/recording. Actions: Open OBS Page, Dismiss. Do not reuse `refreshWarnings()` as-is — it clears when not streaming.
- **Desktop notification** (`Desktop::notifyCaptureLost` or shared `notify`): urgency higher if live; title/body plain second-person (“PipeWire restarted… Restart OBS to bring Rostrum Mic / Stream Mix back.”). Exact copy via `i18n`.
- **OBS page:** short persistent line under “What OBS records right now” while the condition holds (accessible fallback if notifications are off).
- **Gates:** require background OBS follow (`obsBackground`) and the existing go-live warnings toggle (`obsGoLiveWarnings`); no new setting. If product copy of that toggle becomes inaccurate, reword the description only.

### Docs

- `docs/audio.md` (OBS section): after a full PipeWire restart, restart OBS; Rostrum reconnecting is not enough; name-based captures, not serials.
- `docs/manual-tests.md`: Case A procedure (`systemctl --user restart pipewire pipewire-pulse wireplumber`) and expected banner.
- `README.md` if the troubleshooting / restart PipeWire tip would otherwise imply OBS keeps working.
- Update issue #21 framing in the PR description (clients don’t reconnect; not serial mismatch).

## Implementation sketch

| Piece | Role |
|-------|------|
| `src/obs/CaptureWatch.{h,cpp}` (name flexible) | Baseline / epoch / decision; unit-testable |
| `src/obs/ObsConfig` | `runningObs()` → pid + start time; `isObsRunning()` wrapper |
| `src/app/Obs.cpp` | Wire Ready/Failed, `graphChanged`, mix ready; expose QML properties |
| `src/app/Desktop.*` + `Banners.qml` / OBS page QML | Notification + banner + page line |
| Settings | Reuse `obsBackground` + `obsGoLiveWarnings` only |

Do **not** add obs-websocket writes for recovery in this design.

## Tests

1. **Unit:** `CaptureWatch` — baseline debounce; discard uncommitted on disconnect; OBS closed / new pid; no baseline; mix off / mix error; grace then alert; self-clear; dismiss per epoch.
2. **Integration (private PW):** stand-in OBS capture (`pw-cat`/`pacat` with OBS naming) + process named `obs`; restart daemon via existing `pipewire-restart` helpers; assert alert fires when stand-in does not return, and does not fire when stand-in respawns in grace or OBS process is gone.
3. **Manual:** real OBS (native + Flatpak if practical) after Case A restart.

## Risks

- **False positives:** mitigated by baseline debounce, pid/start time, mix-ready gate, Ready-only evaluation, grace for late relink.
- **False negatives:** OBS never had Rostrum in the graph (lazy sources) → no baseline → no alert (accept). WirePlumber-only Spotify unlink → out of primary scope.
- **Stale graph while Failed:** never decide until Ready after reconnect.
- **Product promise:** background connection stays read-only; warn only.

## Stretch (explicitly later)

Opt-in “Reload OBS audio” (scene-collection bounce) for mid-stream emergencies — invasive, needs a second collection, black/silent for seconds. Not part of this design.

## Success criteria

1. After Case A with OBS capturing at least one Rostrum device before the restart, Rostrum shows banner/notification directing a restart of OBS once the mix is healthy again (unless captures somehow returned).
2. No alert when OBS was not using Rostrum, OBS exited, or a new OBS process started and reattached.
3. Docs describe the OBS client limitation accurately (not serials).
4. No automatic OBS graph or websocket mutation for recovery.
