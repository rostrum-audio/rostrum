# Keep `rostrum.filtered` alive across mic filter toggle

Date: 2026-11-11  
Issue: [#22](https://github.com/rostrum-audio/rostrum/issues/22)  
Status: approved design

## Problem

OBS (and any client that captures `rostrum.filtered` by device name) goes silent when mic filters turn off, because Rostrum destroys that node. When filters turn back on, a new node is created (new PipeWire serial); OBS often stays unlinked until OBS is restarted.

App capture streams are already moved off filtered via `target.object` when filters turn off. OBS is intentionally never auto-moved by the mic router.

## Goal

While the mix is running, `rostrum.filtered` remains a stable PipeWire source whether mic filters are on, off, failed, or unavailable, so name-based captures keep working across filter toggles without restarting OBS.

## Non-goals

- Fixing OBS captures after a full PipeWire restart ([#21](https://github.com/rostrum-audio/rostrum/issues/21)).
- Auto-retargeting OBS sources between `rostrum.mic` and `rostrum.filtered`.
- Changing Set Up OBS’s default recording-track device choices.
- Changing which apps the router moves onto filtered when filters are on.

## Behavior

### Mix on, filters on (unchanged path)

- Create/adopt `rostrum.micfx` and `rostrum.filtered`.
- Link hardware mic → `micfx` → `filtered` (and stream mic / sidetone as today).
- Load the filter graph into `micfx` as today.
- Apps may be moved onto `rostrum.filtered` per existing rules.

### Mix on, filters off / failed / unavailable (new)

- **Do not destroy** `rostrum.filtered`.
- Do not run (or keep) a loaded filter graph on that path: tear down / skip `rostrum.micfx` as appropriate for “filters not active.”
- Ensure a mono bypass so `rostrum.filtered` still carries voice at **unity gain**: hardware mic → `rostrum.filtered`.
- Stream mic and sidetone keep today’s filters-off wiring (hardware → `rostrum.mic` / sidetone directly).
- App routing unchanged: turning filters off still restores/clears `target.object` so apps leave filtered. Clients that **pinned** Filtered Mic by name (including OBS) keep hearing bypass audio on that node.

### Filters returning on

- Adopt the existing `rostrum.filtered` node (no destroy/recreate when possible).
- Recreate/reload `micfx`, restore `hw → micfx → filtered`, unload the bypass link.
- Same node identity/serial when the node was never destroyed → OBS stays linked.

### Mix off / quit / crash

- Unchanged linger/adopt semantics for owned nodes, except documentation must no longer say filtered exists only while filters are on.

## Implementation sketch

Primary touchpoint: `Engine::reconcileMicFx()` in `src/engine/EngineMicFx.cpp`.

Today, `!micFxWanted()` destroys both `micfx` and `filtered` after the release hold. Change that path so that when the mix is enabled:

1. Keep / create `rostrum.filtered`.
2. Remove `rostrum.micfx` (after the existing app-release hold if needed).
3. Drive mix linking so filtered is fed from the hardware source while filters are inactive (bypass), and from `micfx` while filters are active.

Mix linking in `EngineMix` (and any “filtered carries sound” gates used before moving apps) must treat bypass-fed filtered as a live mic source when filters are off, without treating filters as “on.”

## Docs

Update `docs/audio.md`:

- Node table / prose: `rostrum.filtered` exists while the mix runs; with filters on it is processed, with filters off it is a unity bypass of the hardware mic.
- “Quitting, crashes and turning off”: turning filters off no longer removes `rostrum.filtered` (may still remove `micfx`).
- OBS note: capturing `rostrum.filtered` remains valid across filter toggle; doubling with Rostrum Mic on the stream tracks is unchanged guidance.

## Tests

- Update integration coverage that expects `removing "rostrum.filtered"` when filters turn off (e.g. `tests/integration/mic-filter-release.py`): filtered should remain; `micfx` removal / app return to raw mic may still be asserted.
- Add or extend a check that after filters off → on, `rostrum.filtered` is still present (prefer same node id/serial when possible) and carries audio in both states.
- Unit/graph tests that assume filtered vanishes when filters are off need the same correction.

## Risks

- **Double path:** bypass and `micfx` must not both feed filtered at once during transitions (doubled level / phasing). Tear down one feed before enabling the other; never leave both linked.
- **Release hold:** apps still on filtered when filters turn off must still be moved before tearing down `micfx`; filtered itself stays.
- **Failed filters / cleanup:** bypass must keep filtered alive when filters fail mid-session. Cleanup that removes a failed `micfx` must **never** destroy `rostrum.filtered`. Today `filteredNode()` returns null unless `micFxNode()` is usable — that coupling must be broken so callers can see filtered in bypass mode without implying DSP is running.
- **Docs/product copy:** “Filtered Mic” while filters are off is unfiltered unity audio; that matches the node’s role for apps at unity, but UI strings should not imply processing is active.

## Review notes (2026-11-11)

External review agreed with this architecture. Emphasized: (1) exclusive audio paths during transitions, (2) never destroy filtered when cleaning up failed micfx, including after decoupling `filteredNode()` from `micFxNode()`.

## Success criteria

1. Toggle mic filters off then on with an OBS (or `pw-record`) capture of `rostrum.filtered`: capture stays linked and audible in both states without restarting the capturer.
2. Apps that were auto-moved to filtered still return to the plain mic when filters turn off.
3. Existing filters-on DSP behavior and stream mic / sidetone paths remain correct.
