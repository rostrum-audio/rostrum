# Fedora offscreen startup diagnostics

The Fedora CI startup check still requires exit zero, a nonempty screenshot and
no matching QML startup error. Its launch deadline is still **60 seconds**. There
is one launch, with no automatic retry. Diagnostic instrumentation is not a fix
for the unresolved timeout in CI run 37253994053.

Run in a disposable Fedora container with CI's build dependencies:

```sh
python3 -B tools/ci/offscreen.py ./build/src/app/rostrum fedora-offscreen
python3 -B tests/test_offscreen_diagnostics.py -v
```

The output directory must not already exist: a previous screenshot cannot grant
a pass. The helper uses disposable HOME/config/data/cache/state/runtime directories,
a private D-Bus session, offscreen Qt, a nonexistent PipeWire remote and a private
nonexistent PulseAudio socket. It removes inherited audio/D-Bus/display addresses
and update/crash endpoint overrides. It does not start an audio daemon or test
audio. Run it in CI's container; do not use it for live-session diagnosis.

## Retained evidence

CI uploads `app-offscreen-fedora` on success or failure:

- `app.log`: application and private D-Bus stdout/stderr, written during startup.
- `rostrum.log`: Rostrum's private startup log, if logging was initialized.
- `screenshot.png`: if the app produced it.
- `result.json` and `job-status.json`: actual exit, elapsed time and supervisor
  cleanup acknowledgement. Timeout remains exit 124; cleanup errors cannot replace
  the original failure and make an otherwise successful check fail.
- `process-state.json`: if still running at 50 seconds, a single passive snapshot
  of the owned process tree, process/thread status, wait channels, current syscalls,
  maps and kernel stacks. Each proc read is capped at 64 KiB; at most 256 threads
  per process are inspected, with a two-second collection budget. Exits, permission
  failures and budget exhaustion are explicitly recorded, not successful stack reads.
- `diagnostic-error.txt`: if snapshot collection itself fails.

There is no debugger attachment or continuous syscall tracing. Kernel stacks are
not userspace backtraces and can be denied by container permissions; wait channels,
syscalls and maps may still help locate the wait. Passive reads also have a small
timing cost. If future investigation uses ptrace/strace, a passing traced run is
not proof of resolution: tracing can change the race being investigated.

The existing Linux subreaper/pidfd test supervisor owns and reclaims the D-Bus/app
descendants, including clients ignoring TERM. Cleanup has its separate existing
bounded grace period; it never extends the successful-launch deadline. Private
resources are removed only after confirmed reclamation, otherwise retained and
reported. SIGKILL of the supervisor or uninterruptible kernel waits cannot provide
a truthful cleanup acknowledgement; see [audio-suite guarantees](../tests/integration/README.md).

The regressions use disposable executables for success, timeout with a descendant,
TERM resistance, interruption, original exit preservation, stale evidence and the
existing screenshot/QML failure gates. They do not reproduce the original Fedora
hang, exercise real audio, or establish broader release compatibility.
