# Audio Safety Integration Suite Implementation Plan

> Execute inline using superpowers:executing-plans; the user's implementation request authorizes this plan's execution.

**Goal:** Verify startup routing, destination isolation and saved-mic safety against private PipeWire/WirePlumber daemons.
**Architecture:** Python standard-library orchestration reuses rostrum-graphtest, pw-cat and graph inspection. Each scenario gets fresh runtime, config, data, state and D-Bus resources. Observe links and capture bounded windows of deterministic signals; fail on missing dependencies, daemon identity mismatch, stalled capture or timeout.
**Tech Stack:** Python 3, PipeWire 1.x command-line tools, WirePlumber 0.5+, Qt/C++ engine driver.
**Spec:** User's three-scenario implementation request in this chat; no product changes unless independently reproduced and explained.

## Global Constraints
- Never connect to the user's audio or D-Bus session; disable hardware discovery.
- Preserve routing, persistence and opt-in mic fallback semantics.
- Clean owned process groups and temporary resources on success, failure and catchable interruption.
- No telemetry, push, release or PR.

## Review Focus
- Ambient environment overrides cannot select another remote or load user configuration.
- Silence must be captured buffers, not absence of recording progress.
- Positive signal assertions prevent a disconnected graph passing isolation checks.
- Restored mic must displace the fallback and use the saved name.
- Failure/interruption cleanup must terminate only owned processes.

## Task 1: Local suite
- [x] Implement the isolation fixture and three scenarios; use saved settings for mic choice.
- [x] Run missing-daemon and deliberate broken-route negative controls.
- [x] Run each real scenario and correct only fixture problems, or explain a reproduced product bug before fixing it.

## Task 2: Verification and CI
- [x] Attempt 20 consecutive clean runs, recording results.
- [x] Add opt-in CTest registration and CI only after a matching runner environment passes.
- [x] Document dependencies, commands, assertions and limitations.
- [x] Build, run existing tests, install locally as requested by project instructions, review and commit locally.

## Execution results

- No applicable AGENTS.md found; current repository and user handoff rules rechecked.
- Reused `rostrum-graphtest`; no production source changed and no product bug exposed.
- Initial fixture failures: empty default metadata was omitted by pw-dump; corrected readiness to pw-cli registry inspection. Negative-control link injection was removed by Rostrum itself; switched to an internal injected destination tone. Cleanup guard failures led to idempotent cleanup and owned-process filtering.
- Independent review found registration/interruption/cleanup races; fixed and added deterministic guard tests. Final review found no unresolved P2-or-higher issue.
- Baseline CTest: 22/22. Final configure/build succeeded; CTest: 24/24 in 22.75s (including nine fixture checks).
- Final `audio-safety.py --driver build/tools/rostrum-graphtest/rostrum-graphtest --runs 20`: 20/20 complete runs, 60/60 scenarios, no failures. Two earlier 20-run batches also completed successfully before final construction cleanup changes.
- Ubuntu26.04 Docker: first engine-only validation passed three full runs. Full validation initially failed because the ad hoc dependency command omitted ca-certificates required by existing RNNoise FetchContent. Corrected to the CI dependency list: full build and CTest passed 20/20 in 16.92s, then three more full audio runs passed. Both host and container used PipeWire1.6.2/WirePlumber0.5.13.
- `docker run --rm -v /home/william/rostrum:/src:ro -w /src rhysd/actionlint:latest -color .github/workflows/ci.yml`: exit0.
- `cmake --install build --prefix /home/william/.local`: exit0; running app untouched.
- Final process/directory inspection found no remaining private audio daemons or session directories. Git diff whitespace check passed.
- CI enabled only on the existing Ubuntu unit job; remote GitHub Actions execution remains unverified until a user-authorized push. No push, PR, release, UI or packaging change.
