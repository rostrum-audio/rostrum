# OBS Capture Loss After PipeWire Restart Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After Rostrum recovers from a full PipeWire restart, detect that OBS is still running but no longer capturing Rostrum devices, and tell the user to restart OBS ([#21](https://github.com/rostrum-audio/rostrum/issues/21)).

**Architecture:** Pure `obs::CaptureWatch` owns baseline / epoch / alert state. `Obs` feeds it Ready/Failed transitions, the graph via `obsRecordings`, mix readiness, and OBS process identity. UI is a dedicated banner + desktop notification + OBS-page line — not an extension of go-live `warnings`. No obs-websocket writes.

**Tech Stack:** C++/Qt (`src/obs`, `src/app`), existing `obsRecordings` / `/proc` OBS detection, Kirigami banners, `Desktop::notify`, QTest + private-PipeWire Python harness.

**Spec:** `docs/superpowers/specs/2026-11-11-obs-capture-lost-after-pw-restart-design.md`

## Global Constraints

- Case A (full PipeWire restart) is the primary path; do not auto-repair OBS.
- Never move or rewrite OBS capture links; never add websocket recovery in this plan.
- Background connection stays read-only; warn only.
- Gate alerts on `obsBackground` and `obsGoLiveWarnings` (no new setting).
- Evaluate only while PipeWire is `Ready`; Failed leaves a stale graph.
- Baseline only includes recordings whose `device` starts with `rostrum.`.
- Same OBS pid + process start time required for an alert; new OBS process clears.
- Debounce baseline ~2 s; grace ~8 s after `mixReady()`, hard cap ~20 s after Ready.
- Correct docs: OBS clients do not reconnect — not “new serials.”
- Work on dedicated branch `fix/21-obs-capture-lost-after-pw-restart`; frequent small commits; TDD.

## File map

| File | Responsibility |
|------|----------------|
| `src/obs/ObsConfig.h/.cpp` | `ObsProcess` + `runningObs()` (pid, start time); keep `isObsRunning()` |
| `src/obs/CaptureWatch.h/.cpp` | Pure baseline / freeze / epoch / alert / dismiss / clear |
| `src/obs/CMakeLists.txt` | Add `CaptureWatch` sources |
| `tests/tst_capturewatch.cpp` (+ CMake) | Unit tests for watch + process helpers |
| `src/app/Obs.h/.cpp` | Wire watch; QML props `captureLost`, `captureLostText`; dismiss |
| `src/app/Desktop.h/.cpp` | `notifyCaptureLost` |
| `src/app/qml/Banners.qml` | Capture-lost `InlineMessage` |
| `src/app/qml/ObsPage.qml` | Persistent line under recordings |
| `docs/audio.md`, `docs/manual-tests.md`, `README.md` | Cause + workaround |
| `tests/integration/obs-capture-lost.py` (+ CMake/README) | Private-PW Case A stand-in |

---

### Task 1: OBS process identity

**Files:**
- Modify: `src/obs/ObsConfig.h`, `src/obs/ObsConfig.cpp`
- Test: `tests/tst_capturewatch.cpp` (create; also covers Task 2) or add cases to `tests/tst_obs.cpp` if preferred — prefer new `tst_capturewatch` linked to `rostrum_obs`

**Interfaces:**
- Produces:
```cpp
namespace rostrum::obs {
struct ObsProcess {
    qint64 pid = 0;
    qint64 startTime = 0; // from /proc/<pid>/stat field 22 (starttime), comparable across calls
};
std::optional<ObsProcess> runningObs(); // first matching uid-owned "obs" process
bool isObsRunning(); // return runningObs().has_value(); keep existing callers
}
```

- [ ] **Step 1: Write failing tests**

```cpp
void TestCaptureWatch::runningObs_none()
{
    // When no process named obs: runningObs() is nullopt, isObsRunning() false.
}

void TestCaptureWatch::runningObs_matchesFake()
{
    // Spawn a short-lived child renamed via prctl(PR_SET_NAME, "obs") as tst_recordingtracks does;
    // runningObs()->pid matches; startTime > 0; second call with same process returns equal startTime.
}
```

- [ ] **Step 2: Run — expect FAIL** (symbols missing or always nullopt)

Run: `ctest --test-dir build-official -R tst_capturewatch --output-on-failure`  
(or configure preset first: `cmake --preset official && cmake --build build-official --target tst_capturewatch`)

- [ ] **Step 3: Implement `runningObs()`**

In `ObsConfig.cpp`, factor the `/proc` scan from `isObsRunning()`:

1. Match `comm == "obs"` and owner uid (unchanged).
2. Skip zombies if easy (`stat` state field).
3. Read `starttime` from `/proc/<pid>/stat` (field 22 after the `comm` parenthesis).
4. Return the first match as `ObsProcess`.

`isObsRunning()` becomes `return runningObs().has_value();`.

- [ ] **Step 4: Run — expect PASS**

- [ ] **Step 5: Commit**

```bash
git add src/obs/ObsConfig.h src/obs/ObsConfig.cpp tests/tst_capturewatch.cpp tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
Identify the running OBS process by pid and start time.

EOF
)"
```

---

### Task 2: `CaptureWatch` pure logic

**Files:**
- Create: `src/obs/CaptureWatch.h`, `src/obs/CaptureWatch.cpp`
- Modify: `src/obs/CMakeLists.txt`
- Test: `tests/tst_capturewatch.cpp`

**Interfaces:**
- Consumes: `QList<obs::Recording>`, `ObsProcess`, mix flags, injectable monotonic ms (`qint64 nowMs`)
- Produces (illustrative API — match local style; keep methods small):

```cpp
namespace rostrum::obs {
struct CaptureKey {
    QString source;
    QString device; // must start with "rostrum."
    bool operator==(const CaptureKey &) const = default;
};

class CaptureWatch {
public:
    void setNowMs(qint64 ms); // tests set explicitly; production passes QElapsedTimer/msec

    // While Ready and OBS known: feed current recordings + process.
    void observe(const QList<Recording> &recordings, const std::optional<ObsProcess> &obs, qint64 nowMs);

    // PipeWire left Ready (Failed / disconnect).
    void onDisconnected(qint64 nowMs);

    // PipeWire became Ready again after a disconnect (start epoch).
    void onReconnected(qint64 nowMs);

    // Mix status after reconnect.
    void setMix(bool enabled, bool ready, bool error);

    // Optional: OBS live outputs known via websocket (severity only).
    void setObsLive(bool streamingOrRecording);

    void tick(qint64 nowMs); // advance debounce / grace; call from Obs on timers/graph

    bool alertActive() const;
    QStringList missingDevices() const; // human-facing device names still missing
    bool liveSeverity() const;          // streamingOrRecording at decision time
    void dismiss();                     // clears alert for this epoch only
};
}
```

Constants (named, in the `.cpp` or header):
- `kBaselineStableMs = 2000`
- `kGraceAfterMixReadyMs = 8000`
- `kEpochCapAfterReadyMs = 20000`

Rules (must match spec):
1. Candidate keys = recordings with `device.startsWith("rostrum.")` as `(source, device)`.
2. Promote candidate → committed only after unchanged for `kBaselineStableMs` while Ready and OBS present.
3. `onDisconnected`: freeze committed; clear candidate.
4. `onReconnected`: start epoch; do not alert until mix enabled+ready, no mix error, grace elapsed (or cap).
5. Alert only if OBS still same pid+startTime, committed non-empty, and at least one key absent from current observations.
6. Clear alert when all keys back, OBS gone, or pid/startTime changed; `dismiss()` suppresses until next epoch.

- [ ] **Step 1: Write failing unit tests** (table-driven where natural)

Cover at least:
- baseline debounce then disconnect → reconnect → grace → alert
- uncommitted candidate discarded on disconnect (no false alert)
- OBS closed / new pid → no alert
- no baseline → no alert
- mix off / mix error → no alert
- capture returns during grace → no alert
- dismiss then same epoch stays quiet; new disconnect/reconnect can alert again
- partial loss (one of two devices missing) → alert

- [ ] **Step 2: Run — expect FAIL**

- [ ] **Step 3: Implement `CaptureWatch`**

Keep all branching in this class; no Qt widgets / no Engine includes.

- [ ] **Step 4: Run — expect PASS**

- [ ] **Step 5: Commit**

```bash
git add src/obs/CaptureWatch.h src/obs/CaptureWatch.cpp src/obs/CMakeLists.txt tests/tst_capturewatch.cpp
git commit -m "$(cat <<'EOF'
Add CaptureWatch for OBS Rostrum captures across PipeWire restarts.

EOF
)"
```

---

### Task 3: Wire into `Obs` + banner + notification

**Files:**
- Modify: `src/app/Obs.h`, `src/app/Obs.cpp`
- Modify: `src/app/Desktop.h`, `src/app/Desktop.cpp`
- Modify: `src/app/qml/Banners.qml`, `src/app/qml/ObsPage.qml` (recordings section ~223+)
- Optionally: `src/app/qml/ObsChoices.qml` — reword go-live warnings description if it implies stream-start only

**Interfaces:**
- Consumes: Task 1–2 APIs; `AppController` / `PwContext::stateChanged`; `Engine::mixReady` / mix error signals; `obsRecordings(graph)`
- Produces QML:
  - `Obs.captureLost` (bool)
  - `Obs.captureLostText` (QString)
  - `Obs.dismissCaptureLost()`
  - Signal `captureLostChanged`

Wiring sketch:

```cpp
// On PwContext state:
//   Failed/Idle (was Ready) -> m_captureWatch.onDisconnected(now)
//   Ready (after having been non-Ready since last Ready) -> m_captureWatch.onReconnected(now)
// On graphChanged / poll / mix signals -> observe(recordings, runningObs(), now); setMix(...); tick(now)
// If alert becomes active and gates on (obsBackground && obsGoLiveWarnings && !offscreen):
//   emit captureLostChanged; Desktop::notifyCaptureLost(...); urgency 2 if live else 1
// If alert clears -> update props (notification may remain; banner hides)
```

Use a connection-generation or `m_pwWasReady` flag so WirePlumber client churn that does **not** leave Ready does not start a Case A epoch. Spec primary path is full disconnect (`Failed`).

`Desktop::notifyCaptureLost(const QString &body, uchar urgency)` — own notification id member like go-live.

Banner in `Banners.qml` (new `InlineMessage`, not the go-live Error block):
- `visible: Obs.captureLost`
- `type: Obs.captureLostLive ? Error : Warning` (or derive from a bool property)
- Actions: Open OBS Page, Dismiss → `Obs.dismissCaptureLost()`

OBS page: small `InlineMessage` or label above/below “What OBS records right now” when `Obs.captureLost`.

Copy (i18n; adjust to product voice):
- Title: “OBS stopped recording Rostrum.”
- Body: “PipeWire restarted, and OBS did not pick Rostrum back up. Restart OBS to restore %1.” with missing device labels joined.

- [ ] **Step 1: Add properties + Desktop notify + QML; build**

- [ ] **Step 2: Manual or existing offscreen smoke if available** — at minimum compile + `tst_capturewatch` still green

- [ ] **Step 3: Commit**

```bash
git add src/app/Obs.h src/app/Obs.cpp src/app/Desktop.h src/app/Desktop.cpp \
        src/app/qml/Banners.qml src/app/qml/ObsPage.qml
git commit -m "$(cat <<'EOF'
Warn when OBS loses Rostrum captures after PipeWire restarts.

EOF
)"
```

---

### Task 4: Docs

**Files:**
- Modify: `docs/audio.md` (OBS section)
- Modify: `docs/manual-tests.md` (add Case A procedure)
- Modify: `README.md` only if the “restart pipewire” tip would imply OBS keeps working

- [ ] **Step 1: Update prose**

Must say:
- Full PipeWire restart drops OBS’s audio clients; Rostrum reconnecting is not enough.
- Workaround: restart OBS.
- Cause is OBS not reconnecting (pulse `@todo` / plugin), not Rostrum serial churn for name-based Set Up OBS sources.

- [ ] **Step 2: Commit**

```bash
git add docs/audio.md docs/manual-tests.md README.md
git commit -m "$(cat <<'EOF'
Document restarting OBS after a PipeWire restart.

EOF
)"
```

---

### Task 5: Integration test (private PipeWire)

**Files:**
- Create: `tests/integration/obs-capture-lost.py`
- Modify: `tests/integration/README.md`, `tests/CMakeLists.txt` (register like other audio_* tests)

**Approach:** Reuse `pipewire-restart.py` reconnect helpers and `audio-safety` session patterns.

Stand-in:
1. Symlink or `prctl` so a helper process has `comm == obs` **or** document that CaptureWatch is fed via a graphtest hook if `/proc` rename is awkward in the harness — prefer real `comm` so production `runningObs()` is exercised.
2. Start `pw-cat -r` (or `pacat`) targeting `rostrum.mic` / stream monitor with OBS naming (`application.name=OBS`, `OBS: …` node name / `media.name`) so `obsRecordings` sees a Rostrum device.
3. Baseline settle ≥2 s.
4. Restart private PipeWire; tell Rostrum to reconnect (`--reconnect-file`).
5. **Do not** restart the stand-in capture → expect Rostrum to log or expose capture-lost (add `--capture-lost-file` ack on graphtest **or** assert a dedicated log line if that matches existing patterns better — prefer a small graphtest status file for determinism).
6. Negative: kill stand-in OBS process before grace ends → no alert; or respawn capture in grace → no alert.

If graphtest needs a new flag (`--capture-watch-file`), keep it test-only and mirror `--mic-filter-file` / `--reconnect-file` style.

- [ ] **Step 1: Write test + any graphtest hook**

- [ ] **Step 2: Run — expect PASS on this branch**

- [ ] **Step 3: Commit**

```bash
git add tests/integration/ tools/rostrum-graphtest/ tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
Prove capture-lost detection after a private PipeWire restart.

EOF
)"
```

---

### Task 6: Branch hygiene and issue close-out

- [ ] **Step 1: Branch `fix/21-obs-capture-lost-after-pw-restart`**

- [ ] **Step 2: Run `tst_capturewatch`, relevant `tst_obs`, and `obs-capture-lost` integration**

- [ ] **Step 3: Draft PR referencing #21; comment on issue with corrected framing + test evidence**

```bash
gh issue comment 21 --body "$(cat <<'EOF'
Implementation on fix/21-obs-capture-lost-after-pw-restart: detect + warn when OBS loses Rostrum captures after a full PipeWire restart (OBS clients do not reconnect; not a serial mismatch). Spec: docs/superpowers/specs/2026-11-11-obs-capture-lost-after-pw-restart-design.md
EOF
)"
```

---

## Definition of done

- [ ] `runningObs()` exposes pid + start time; `isObsRunning()` preserved
- [ ] `CaptureWatch` unit-tested for debounce, freeze, grace, dismiss, OBS identity
- [ ] Banner + notification + OBS page line when Case A loss detected
- [ ] Gated by `obsBackground` + `obsGoLiveWarnings`; no auto OBS writes
- [ ] Docs correct the serial framing
- [ ] Integration proof on private PW (or documented harness limitation if blocked)
- [ ] PR open against main referencing #21

## Spec coverage (self-check)

| Spec item | Task |
|-----------|------|
| Case A detect + warn | 2–3, 5 |
| Non-goal: no auto-repair / no link writes | Global + Task 3 |
| Baseline debounce + freeze | 2 |
| Pid/start time | 1–2 |
| Ready-only / mix gates / grace | 2–3 |
| Banner + notify + page line | 3 |
| Reuse settings gates | 3 |
| Docs + issue framing | 4, 6 |
| Unit + integration + manual doc | 2, 4, 5 |
| Stretch bounce deferred | Global non-goal |
