# Keep `rostrum.filtered` Alive Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** While the mix is on, keep `rostrum.filtered` as a stable PipeWire source across mic-filter off / on / failed / recovered, with a unity-gain hardware bypass when DSP is inactive, so OBS name-based captures do not go silent or need a restart ([#22](https://github.com/rostrum-audio/rostrum/issues/22)).

**Architecture:** Decouple “filtered node exists” from “micfx DSP is running.” `reconcileMicFx` always maintains `rostrum.filtered` when the mix is enabled; it only creates/destroys `rostrum.micfx` and the filter graph. `EngineMix` links either `hw → micfx → filtered` or `hw → filtered` (bypass), never both. App `target.object` routing stays as today.

**Tech Stack:** C++/Qt engine (`EngineMicFx`, `EngineMix`), PipeWire graph links, existing Python integration harness under `tests/integration/`.

**Spec:** `docs/superpowers/specs/2026-11-11-filtered-mic-always-alive-design.md`

## Global Constraints

- Never destroy `rostrum.filtered` while cleaning up a failed or unwanted `micfx` (mix still on).
- During transitions, hardware→filtered and hardware→micfx→filtered must not both feed filtered at once.
- Do not auto-move OBS; do not change Set Up OBS default device choices; do not fix [#21](https://github.com/rostrum-audio/rostrum/issues/21) in this plan.
- Prefer same filtered node id/serial across filter toggles (no destroy/recreate when the node already exists).
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

### Task 2: Exclusive mix links — processed vs bypass

**Files:**
- Modify: `src/engine/EngineMix.cpp` (mic/filter linking ~690–710 and any link-repair for filtered)
- Modify: `src/engine/EngineMicFx.cpp` `reconcileMicFx` / route gates as needed so “filtered carries sound” is true for bypass

**Interfaces:**
- Consumes: `micFxRunning()`, `micFxNode()`, `filteredNode()`, hardware source node
- Produces: when mix on + fx running → `hw → micfx → filtered`; when mix on + fx not running → `hw → filtered` only; never both into filtered

- [ ] **Step 1: Inspect current link pairs**

Read `EngineMix.cpp` around the micFx/filtered linking block. Note how unwanted links are dropped today.

- [ ] **Step 2: Implement exclusive feeds**

Pseudo-structure (match local style; do not invent new abstractions unless needed):

```cpp
const pw::Node *micFx = micFxRunning() ? micFxNode() : nullptr;
const pw::Node *filtered = filteredNode(); // may exist in bypass
// If micFx: link hw→micFx and micFx→filtered (and existing mic/sidetone from micFx).
// Else if filtered && mix on: link hw→filtered (bypass), and ensure no micFx→filtered / stale micFx links remain.
// Never append both bypass and processed pairs in the same reconcile.
```

Transition rule: if switching modes in one pass, omit the old feed from desired links so the existing link reconciler deletes it before/as the new feed is added (same mechanism other Rostrum links use).

- [ ] **Step 3: “Carries sound” for app moves**

In `reconcileMicRoutes` (`EngineMicFx.cpp` ~517+), today apps move onto filtered only when `micFxRunning()`. **Keep that** (apps should not be moved onto filtered when filters are off). Bypass is for name-pinned capturers (OBS), not for expanding auto-move.

- [ ] **Step 4: Manual or graphtest sanity (optional if integration is next)**

With a private graphtest session if available: filters on → see micfx in path; filters off → filtered still linked from hw, no micfx.

- [ ] **Step 5: Commit**

```bash
git add src/engine/EngineMix.cpp src/engine/EngineMicFx.cpp
git commit -m "$(cat <<'EOF'
Feed rostrum.filtered from hardware when filters are off.

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

1. Run the existing release hold based on apps still linked to **micfx** (and filtered only insofar as needed for moving apps)—do not wait to destroy filtered.
2. Destroy `micfx` nodes only after hold.
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

1. Start private PW + Rostrum with filters on; confirm `rostrum.filtered` id/serial I0.
2. Start capture/record of `rostrum.filtered` (keep process alive).
3. Toggle filters off; assert same node id (or at least name still present), capture still receives non-silent audio (bypass).
4. Toggle filters on; assert same node id; capture still non-silent (processed).
5. Force filter failure if the harness already has a breaker pattern (`mic-filter-breaker.py`); assert filtered survives and bypass audio returns.
6. Recover filters if applicable; assert filtered still present.

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
- [ ] Mix never dual-feeds filtered
- [ ] Filters off/fail keep filtered + bypass audio
- [ ] Filters on restore processed path without recreating filtered when avoidable
- [ ] Apps still leave filtered when filters turn off
- [ ] Integration test covers off → on → failed → recovered capture
- [ ] `docs/audio.md` updated
- [ ] PR open against main referencing #22
