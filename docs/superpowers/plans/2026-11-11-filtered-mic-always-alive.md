# Keep `rostrum.filtered` Alive Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** While the mix is on, keep `rostrum.filtered` as a stable PipeWire source across mic-filter off / on / failed / recovered, with a unity-gain hardware bypass when DSP is inactive, so OBS name-based captures do not go silent or need a restart ([#22](https://github.com/rostrum-audio/rostrum/issues/22)).

**Architecture:** Decouple “filtered node exists” from “micfx DSP is running.” `reconcileMicFx` always maintains `rostrum.filtered` when the mix is enabled; it only creates/destroys `rostrum.micfx` and the filter graph. `EngineMix` links either `hw → micfx → filtered` or `hw → filtered` (bypass), never both. App `target.object` routing stays as today.

**Tech Stack:** C++/Qt engine (`EngineMicFx`, `EngineMix`), PipeWire graph links, existing Python integration harness under `tests/integration/`.

**Spec:** `docs/superpowers/specs/2026-11-11-filtered-mic-always-alive-design.md`

## Global Constraints

- Never destroy `rostrum.filtered` while cleaning up a failed or unwanted `micfx` (mix still on).
- During transitions, hardware→filtered and hardware→micfx→filtered must not both feed filtered at once.
- **Managed bypass ownership:** `EngineMix`’s `managed()` lambda must treat hardware→`rostrum.filtered` as a Rostrum-managed link (today it only special-cases feeds into mic / sidetone / micfx when the output is not owned). Without that, bypass links are created but never unlinked when filters return → dual feed.
- **Async exclusive handoff:** PipeWire link deletion is asynchronous. Do not create the replacement feed in the same reconcile pass as the unlink request. Use a two-stage transition: (1) drop the old feed from `wanted` and destroy managed old links; (2) only after the old link is gone from the graph, add the new feed to `wanted` / create it.
- **App-release before dropping processed feed:** `reconcile()` runs `reconcileLinks()` before `reconcileMicRoutes()` (`Engine.cpp` ~342–348). When filters turn off, do **not** tear down `micfx→filtered` (or other processed feeds apps still hear) until apps have been restored to the raw mic and the existing release hold is satisfied (or `kFxReleaseMs` times out). Only then start the exclusive async handoff to hardware bypass. Otherwise apps still targeted at filtered can briefly go silent even though `micfx` node destruction is delayed.
- Do not auto-move OBS; do not change Set Up OBS default device choices; do not fix [#21](https://github.com/rostrum-audio/rostrum/issues/21) in this plan.
- Prefer same filtered node id/serial across filter toggles (no destroy/recreate when the node already exists). When the same PipeWire daemon stays up, integration tests **must** assert identical `object.serial` (not only node name).
- Work on a dedicated branch for issue #22; frequent small commits; TDD where tests already exist.

## File map

| File | Responsibility |
|------|----------------|
| `src/engine/Engine.h` | Comments / any helper for “filtered present” vs “fx running” |
| `src/engine/EngineMicFx.cpp` | `filteredNode()`, `reconcileMicFx()` keep-filtered / destroy-micfx-only |
| `src/engine/EngineMix.cpp` | Bypass vs processed linking; exclusive feeds |
| `docs/audio.md` | Node lifetime and bypass semantics |
| `tests/integration/mic-filter-release.py` | Stop expecting filtered removal; assert micfx removal / app return |
| New or extended integration test | Continuous capture of filtered through off→on→failed→recovered |

---

### Task 1: Decouple `filteredNode()` from `micFxNode()`

**Files:**
- Modify: `src/engine/Engine.h` (comment on `filteredNode()` ~271)
- Modify: `src/engine/EngineMicFx.cpp:154-161`
- Test: existing engine/unit tests that call graph helpers; add a focused unit assertion if one already covers node lookup, otherwise rely on Task 3–4 integration

**Interfaces:**
- Consumes: `m_pw->graph()`, `isOwnedNode`, `m_pendingDestroy`, `kFilteredNode`
- Produces: `filteredNode()` returns the owned `rostrum.filtered` node even when `micFxNode()` is null; `micFxRunning()` still requires wanted + usable micfx + graph loaded

- [ ] **Step 1: Write a failing check**

If there is an existing C++ test that can construct a fake graph, assert filtered is visible without micfx. Otherwise document the intended assertion in the integration test scaffold (Task 4) and proceed with a compile-time comment + the production change under TDD via the release script’s “filtered still present after filters off” expectation (Task 3 first makes that expectation fail).

Preferred order if no unit harness: start Task 3’s failing expectation that filtered remains after filters off (that currently fails for the wrong reason—node destroyed). Then do this step’s code change as part of making Task 3 pass. If doing this task alone first:

```cpp
// Intent: filteredNode() must not early-return on !micFxNode().
// After change, owned rostrum.filtered is returned whenever present.
```

- [ ] **Step 2: Change `filteredNode()`**

In `EngineMicFx.cpp`, remove the `if (!micFxNode()) return nullptr;` guard. Keep ownership / pending-destroy checks.

Update `Engine.h` comment from “if micFxNode is usable too” to “owned `rostrum.filtered`, whether or not micfx DSP is running.”

- [ ] **Step 3: Audit callers**

Confirm callers that meant “processed filtered path” still use `micFxRunning()` (or equivalent), not bare `filteredNode()`:

- `EngineMix.cpp` ~693–694 (will be fixed in Task 2)
- `EngineMicFx.cpp` `reconcileMicRoutes` ~517–518
- `micMeterNode()` ~176–179 (should stay on filtered only when `micFxRunning()`, already gated)

- [ ] **Step 4: Build**

Run: configure/build the usual developer preset (or `cmake --build` on the existing build dir).

Expected: compiles.

- [ ] **Step 5: Commit**

```bash
git add src/engine/Engine.h src/engine/EngineMicFx.cpp
git commit -m "$(cat <<'EOF'
Return rostrum.filtered even when micfx is down.

EOF
)"
```

---

### Task 2: Exclusive mix links — managed bypass + async handoff

**Files:**
- Modify: `src/engine/EngineMix.cpp` (mic/filter linking ~690–710; `managed()` ~742–759; link create/destroy loop ~761–788)
- Modify: `src/engine/Engine.h` / `EngineMicFx.cpp` if a small pending-mode flag is needed for two-stage handoff
- Modify: `src/engine/EngineMicFx.cpp` route gates only as needed (app moves stay `micFxRunning()`-gated)

**Interfaces:**
- Consumes: `micFxRunning()`, `micFxNode()`, `filteredNode()`, hardware source node, graph links
- Produces: when mix on + fx running → `hw → micfx → filtered`; when mix on + fx not running → `hw → filtered` only; never both into filtered; bypass links are destroyed when leaving bypass

**Critical: `managed()` today** (`EngineMix.cpp` ~742–759):

```cpp
if (in == mic || in == sidetone || (in == micFxAny && isOwnedNode(*in))) {
    return true;
}
if (!isOwnedNode(*out)) {
    return false;  // hardware → filtered falls through HERE and is NOT managed
}
```

Hardware→`rostrum.filtered` has a non-owned output, so Rostrum would create the bypass link but **never unlink it** when filters turn back on → dual feed / phasing.

- [ ] **Step 1: Extend `managed()` for bypass**

Add an explicit case (mirror mic/sidetone/micfx): if `in` is owned `rostrum.filtered`, return true even when `out` is the hardware source. Keep OBS/app→bus exclusions unchanged.

- [ ] **Step 2: Implement two-stage exclusive handoff (not same-pass swap)**

PipeWire `destroy` on a link is async. Same-pass “omit old from wanted + create new” can overlap.

Required state machine (names illustrative; match local style):

1. **Desired mode** = processed if `micFxRunning()`, else bypass if mix on && `filteredNode()` && **app-release complete**, else neither (or hold processed — see Step 3).
2. If graph still has the **other** mode’s feed into filtered (e.g. switching to processed while `hw→filtered` still exists, or switching to bypass while `micfx→filtered` / `hw→micfx` still exist):
   - Put only the teardown in this reconcile: old feed **absent** from `wanted`, so `managed` unlinks fire.
   - Do **not** add the new feed to `wanted` yet.
   - `scheduleReconcile` (or equivalent) so the next pass runs after the graph updates.
3. When the old feed’s links are **gone** from `g.links`, then add the new feed to `wanted` and create it.

Optional helper: `bool filteredFedBy(const pw::Graph&, enum Feed { Bypass, Processed })` for the “old still present?” check.

- [ ] **Step 3: App-release ordering when leaving processed mode**

When filters become unwanted (`!micFxWanted()` but mix still on):

1. **Hold processed feed:** keep `hw→micfx→filtered` (and related processed links) in `wanted` while the existing release hold is active — i.e. while `micStreamsOnFilter` / apps still need filtered audio, or until `kFxReleaseMs` elapses (same clock/`m_fxReleaseClock` pattern as today’s destroy hold in `reconcileMicFx`).
2. **Routes first in effect:** `reconcileMicRoutes` (later in the same or subsequent passes) restores apps to the raw mic; release hold tracks that.
3. **Only after release (or timeout):** stop wanting the processed feed, run the async exclusive handoff to `hw→filtered` bypass, then destroy `micfx` (Task 3).

Do **not** drop `micfx→filtered` in `reconcileLinks` on the first filters-off pass while apps are still targeted at filtered. Coordinate with Task 3’s hold so link teardown and node destroy agree on “release complete.”

- [ ] **Step 4: Wire desired pairs for the steady states**

```cpp
// Steady processed: hw→micFx, micFx→filtered (+ mic/sidetone from voice as today).
// Steady bypass: hw→filtered only; no micFx→filtered; mic/sidetone from hw as today’s filters-off path.
// Leaving processed: keep processed pairs until app-release complete, then hand off (Step 2–3).
// Never append both bypass and processed pairs into nodePairs/wanted in one pass.
```

- [ ] **Step 5: “Carries sound” for app moves**

In `reconcileMicRoutes` (`EngineMicFx.cpp` ~517+), keep moving apps onto filtered only when `micFxRunning()`. Bypass is for name-pinned capturers (OBS), not auto-move.

- [ ] **Step 6: Commit**

```bash
git add src/engine/EngineMix.cpp src/engine/Engine.h src/engine/EngineMicFx.cpp
git commit -m "$(cat <<'EOF'
Manage filtered bypass links and hand off feeds without overlap.

EOF
)"
```

---

### Task 3: Keep filtered in `reconcileMicFx` when filters unwanted

**Files:**
- Modify: `src/engine/EngineMicFx.cpp:216-258` (`!micFxWanted()` branch)
- Test: `tests/integration/mic-filter-release.py`

**Interfaces:**
- Consumes: `micFxWanted()`, `m_mixEnabled`, fx/filtered node lists, release hold
- Produces: with mix on and filters unwanted: destroy only `micfx` (after hold); create/keep `filtered`; clear fx graph state as today

- [ ] **Step 1: Update release test expectations (fail first)**

In `tests/integration/mic-filter-release.py` (~90): stop requiring `removing "rostrum.filtered"`. Instead assert:

- app returned to raw mic (`raw mic for "…"` still present), and
- `removing "rostrum.micfx"` (or equivalent log) if that is what the engine logs, and
- after filters off, graph still has a node named `rostrum.filtered`.

Run the integration test (or the harness command documented in `tests/integration/README.md`).

Expected: FAIL until Task 2–3 production changes land (or FAIL on missing filtered if only Task 1 landed).

- [ ] **Step 2: Change `!micFxWanted()` cleanup**

When `m_mixEnabled`:

1. Run the existing release hold based on apps still on the filtered path (same `micStreamsOnFilter` / `m_fxReleaseClock` idea as today). During the hold, **processed links must remain** (Task 2 Step 3)—do not signal mix to drop `micfx→filtered` yet.
2. After hold (apps restored or timeout): allow the exclusive handoff to bypass, then destroy **`micfx` nodes only**.
3. If `filteredNodes` empty, create `rostrum.filtered` (same create helper as the wanted path).
4. Do not call `destroyOnce` on filtered.

When mix is off, retain today’s linger behavior (no forced create; optional destroy policy unchanged unless tests require otherwise).

- [ ] **Step 3: Failure path**

Wherever failed filters currently destroy both nodes, ensure filtered is kept when mix is on (same as unwanted). Grep for `destroyOnce` + filtered / `removing` filtered.

- [ ] **Step 4: Re-run mic-filter-release**

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/engine/EngineMicFx.cpp tests/integration/mic-filter-release.py
git commit -m "$(cat <<'EOF'
Keep rostrum.filtered when mic filters turn off.

EOF
)"
```

---

### Task 4: Integration proof — continuous filtered capture across transitions

**Files:**
- Create or extend: `tests/integration/mic-filter-filtered-stable.py` (or add a scenario to an existing mic-filter script if that matches harness style better)
- Possibly reuse helpers from `mic-filter-release.py` / graphtest

**Interfaces:**
- Consumes: private PipeWire session, Rostrum offscreen, `pw-record --target rostrum.filtered` or graphtest capture
- Produces: one test that fails if filtered disappears or goes silent across off → on → failed → recovered

- [ ] **Step 1: Write the failing scenario outline**

Script steps:

1. Start private PW + Rostrum with filters on; record `rostrum.filtered` **object.serial** S0 (and node id).
2. Start capture/record of `rostrum.filtered` (keep process alive).
3. Toggle filters off; assert **same object.serial S0** (strict, same daemon), capture still non-silent (bypass); assert no `micfx→filtered` link.
4. Toggle filters on; assert **same object.serial S0**; capture still non-silent (processed); assert no `hw→filtered` bypass link remains.
5. Force filter failure if the harness already has a breaker pattern (`mic-filter-breaker.py`); assert filtered serial still S0 and bypass audio returns.
6. Recover filters if applicable; assert serial still S0 and processed path without bypass overlap.

- [ ] **Step 2: Run — expect fail on current main without full Tasks 2–3** (or pass once those land; if already green, keep the test as regression).

- [ ] **Step 3: Fix product gaps if the new test reveals doubled audio or dropouts** (exclusive-link bugs).

- [ ] **Step 4: Commit**

```bash
git add tests/integration/
git commit -m "$(cat <<'EOF'
Prove rostrum.filtered stays capturable across filter toggles.

EOF
)"
```

---

### Task 5: Docs

**Files:**
- Modify: `docs/audio.md` (node table ~74, filter sections ~300–379, OBS note ~364–369)

- [ ] **Step 1: Update prose**

- `rostrum.filtered` exists while the mix runs.
- Filters on → processed via micfx; filters off/failed/unavailable → unity bypass from hardware.
- Turning filters off removes `micfx`, not `filtered`.
- OBS may capture `rostrum.filtered` across toggles; doubling with stream Rostrum Mic guidance unchanged.

- [ ] **Step 2: Commit**

```bash
git add docs/audio.md
git commit -m "$(cat <<'EOF'
Document rostrum.filtered bypass while filters are off.

EOF
)"
```

---

### Task 6: Branch hygiene and issue close-out

- [ ] **Step 1: Ensure work is on `fix/22-filtered-mic-always-alive` (or similar), not mixed with unrelated commits**

- [ ] **Step 2: Run relevant unit + integration mic-filter tests**

- [ ] **Step 3: Comment on #22 with test evidence; open draft PR when green**

```bash
gh issue comment 22 --body "Implementation on <branch>: filtered kept with hw bypass when filters off; see plan docs/superpowers/plans/2026-11-11-filtered-mic-always-alive.md"
```

---

## Definition of done

- [ ] `filteredNode()` works without micfx
- [ ] `managed()` owns hardware→filtered bypass links (unlink works when leaving bypass)
- [ ] Transitions are two-stage: old feed gone before new feed created
- [ ] When filters turn off, processed feed stays until app-release hold completes (or timeout), then handoff to bypass
- [ ] Mix never dual-feeds filtered
- [ ] Filters off/fail keep filtered + bypass audio
- [ ] Filters on restore processed path without recreating filtered when avoidable
- [ ] Apps still leave filtered when filters turn off
- [ ] Integration test covers off → on → failed → recovered capture with **strict same object.serial**
- [ ] `docs/audio.md` updated
- [ ] PR open against main referencing #22
