# Isolated audio safety tests

This suite runs the existing `rostrum-graphtest` driver (real Engine/PwContext,
no UI) against a fresh PipeWire daemon, WirePlumber policy manager and D-Bus
bus for **each scenario**. No real devices are discovered. No network calls,
telemetry or audio delivery outside the private graph are introduced.

## Requirements and execution

Linux with readable `/proc`, child subreapers and pidfd support (kernel 5.3+
with these syscalls allowed), Python 3.9+ (standard library), PipeWire 1.x with `pipewire`, `pw-cat`,
`pw-cli`, `pw-dump`, `pw-link`, WirePlumber 0.5+ with its installed configuration
and scripts in `/usr/share/wireplumber`, and `dbus-daemon`. On Ubuntu 26.04 the
additional runtime packages are `pipewire-bin wireplumber dbus python3`.
Build dependencies remain those documented in the main README.

```sh
cmake -S . -B build -DROSTRUM_AUDIO_INTEGRATION_TESTS=ON
cmake --build build
ctest --test-dir build -L audio-integration --output-on-failure
python3 tests/integration/audio-safety.py \
  --driver build/tools/rostrum-graphtest/rostrum-graphtest --runs 20
# Focused mic-filter scheduling/restart regression (add --denoise for an RNNoise build):
python3 tests/integration/mic-filter-scheduling.py \
  build/tools/rostrum-graphtest/rostrum-graphtest build/lib/rostrum/librostrum-dsp.so
```

`--scenario startup|destinations|microphones` selects one scenario for diagnosis;
the default is all three. Every run/scenario must finish; failure stops the batch
and reports the completed clean-run count. Missing tools, incompatible policy,
stalled recordings and timeouts fail rather than skip. CTest registration is
opt-in; CI enables it on the existing Ubuntu 26.04 unit-test container. CTest
also runs the fixture guard, live negative-control and lifecycle regression tests. Tests are serialized
and have a 180-second outer deadline; each observed-state wait has a 12-second
deadline and each command has a 3-second deadline.

## Isolation and cleanup

A mode-0700 temporary tree provides private HOME, runtime, configuration, data,
state and cache directories. Child environments are built from scratch, retaining
only PATH and setting locale explicitly. PipeWire clients use an absolute private
socket and private client configuration. Before policy or Rostrum starts, the
suite verifies the socket ownership/type and a per-session UUID on the daemon's
Core; each graph observation repeats this check. A missing or symlinked socket or
wrong identity aborts. WirePlumber reads a private copy of the installed config,
with a dedicated policy profile disabling audio, Bluetooth and video hardware
monitors, logind, portal permission store and device reservation. Its installed
scripts are explicitly pinned to `/usr/share/wireplumber`. Both D-Bus addresses
point to a private bus without service activation directories.

Every spawned job, including short-lived inspection commands and nested runners,
has a dedicated Python supervisor. The supervisor enables Linux child-subreaper
behavior **before** launching the worker, so descendants whose parent exits are
adopted by that supervisor. It launches only that job. The caller pins the
supervisor's identity before granting permission to launch the worker; unsupported
pidfds/subreapers abort instead of running unsupervised.

Cleanup closes a private control socket. The supervisor snapshots its own process
tree, opens a pidfd for each descendant and verifies its birth identity, kernel
pidfd identity and ancestry. It sends signals through those pinned kernel handles,
never stored numeric PIDs or process-group IDs. Mismatches are rejected and the
owned tree is observed again. Reaping a leader therefore does not lose ownership
of adopted descendants; numeric PID reuse cannot redirect a pidfd signal.

SIGINT, SIGTERM and SIGHUP abort the scenario runner, fixture program and lifecycle
regression driver. Supervisor startup inherits a blocked signal mask until its
handlers are installed, so interruption during interpreter startup also cleans up.
The fixture has an outer cleanup guarantee covering unittest interruption and its
nested runner. Registration is protected against handled signals. Each supervisor
requests SIGTERM, escalates to SIGKILL after three seconds, reaps adopted children
and acknowledges completion only when the kernel's `waitpid` reports `ECHILD`
(no remaining children). An empty `/proc` snapshot alone is never sufficient. The caller waits
at most eight seconds per job. A cleanup deadline failure is reported, ownership
is retained for continued reclamation, and the private tree is retained for
diagnosis; it is never reported as successful cleanup. Cleanup attempts all jobs
and preserves the original test exception when reporting additional cleanup errors.
Repeated successful cleanup is safe.

TMPDIR points inside each private tree, so nested runners' resources stay under
the enclosing fixture. The enclosing tree is removed only after its supervised
jobs finish cleanup, and its original directory identity is checked before
removal. No directories are rediscovered from process command lines and no broad
process-name termination is used.

These guarantees assume an ordinary Linux process tree and working kernel syscalls
and `/proc`. Processes stuck in uninterruptible kernel sleep may outlast SIGKILL;
cleanup reports failure and retains ownership/resources. SIGKILL of the caller
cannot execute its directory cleanup; socket EOF still requests supervisor cleanup
but that path is best effort. SIGKILL of a supervisor, kernel crash or power loss
can strand descendants and private files. Hostile processes with the same UID or
root can interfere with private files and supervisors; this harness is not a
security sandbox against them. After such failures, inspect retained session data
and process identities; never use old PID/group IDs, wildcard deletion, or process
names to reclaim desktop audio processes.

Failure diagnostics print only the private graph and private child logs before
removal. Temporary trees contain deterministic test audio and supervisor status
while active; normal successful cleanup leaves no recordings or directories.
The supervisor is test tooling only: these changes do not alter the installed
application's routing, microphone fallback, persistence or runtime behavior.

## Verified scenarios

1. **Startup before buses:** a 440 Hz playback app names the not-yet-existing Game
   bus, initially plays through fake headphones, then moves to Game when Rostrum
   starts. Active links and captures at Game and Stream verify the intended route.
2. **Destination isolation:** Game set to phones produces a tone at fake headphones
   and recorded silence at Stream. Restarting the driver with Game set to stream
   reverses those assertions. Then one running engine and playback stream exercise
   Desktop through phones, stream, both and phones twice, without restarting the
   engine or playback client. Each change uses the real `setBusDestination` API;
   the driver observes an atomic private command file and acknowledges the request.
   Tests wait for that acknowledgement and observed graph state, require every
   excluded-destination link (including inactive links) to disappear, and capture
   both channels at the intended and excluded outputs. Positive captures accompany
   silence assertions. This catches pending link-deletion state surviving registry
   ID reuse, which could leave a forbidden send after a live destination change.
3. **Saved microphone:** private saved settings select a named fake mic carrying
   330 Hz, while a higher-priority alternative carries 660 Hz. Removing the saved
   mic with fallback disabled leaves no input links and recorded silence at
   `rostrum.mic`; restoring its name recovers 330 Hz. With saved fallback enabled,
   removal routes 660 Hz, and restoration displaces the alternative and recovers
   330 Hz. A positive capture proves the alternative is live before removal.

Each capture requires half a second of real float32 buffers at 48 kHz. It checks
the last 300 ms, allowing initial graph/recording settling, on **every channel**.
Expected tones require RMS 0.04–0.1 and amplitude 0.07–0.13 at their known frequency.
Silence requires RMS at most 0.00001 (−100 dBFS). No buffers never counts as silence.
Polling checks graph state or recording progress rather than assumed startup sleeps.
Fixture controls reject missing/symlinked/wrong-identity sockets, discard ambient
overrides, test child/tree cleanup, reject absent expected audio, and prove that
an injected forbidden-destination signal fails the silence assertion. Lifecycle
regressions interrupt the fixture with SIGTERM/SIGHUP during both a live session
and its nested runner, also check SIGINT, exercise exited leaders and descendants
ignoring SIGTERM, reject stale/mismatched identities without signaling a sentinel,
verify repeated cleanup, handle a fork during SIGTERM and an incomplete process
snapshot, reclaim descendants after status-publication and diagnostic-write failures, retain startup
ownership, and preserve an original failure when cleanup also fails.

## Mic-filter scheduling regression

`audio_mic_filter_scheduling` is registered when the DSP plugin is built and audio
integration tests are enabled. It uses the real engine and plugin, with high-pass
filtering enabled and the other modules bypassed. It requires the deterministic
440 Hz signal at the fake hardware mic, Filtered Mic and Rostrum Mic, before,
during and after an ordinary capture client's 128/48000 latency request. Starting
and removing the client lets PipeWire negotiate its quantum normally; no forced
quantum, fixed bounds, sleeps or relaxed signal thresholds are used. A restart
also installs a legacy persistent converter in the private graph and requires its
replacement plus correct delivery. The public virtual mic nodes remain intact.

The old `rostrum.micfx` bare `audio.convert` used the default input/split direction.
After quantum changes its partial-buffer state could yield alternating silent
blocks. This was reproduced with no filter and with PipeWire's built-in copy
filter as well as Rostrum's DSP, ruling out RNNoise and AppImage bundling as
necessary causes. Explicit output/merge direction consumes and flushes the DSP
input per quantum; existing converters with the old creation properties are
replaced on startup. Changing the fake mic's driver role alone did not fix the
transition failure, so that candidate change was discarded. The fixture's clock
selection and original signal oracle are unchanged.

This is focused scheduling and migration coverage, not a hardware timing or
subjective filter-quality test. The packaged validator separately checks 440 Hz
delivery across a latency request, 20 Hz attenuation and RNNoise loading.

On 2026-10-04 the user reported that the updated normal build was working in
manual use so far. This is limited confirmation of ordinary use, not a confirmed
pass for a specific microphone recording, hardware matrix or other release check.
The five successful clean packaged validations are automated evidence; they do
not turn unreported manual checks into passes.

## Manual confirmation (2026-10-04)

The user confirmed a fresh OBS recording on the installed normal x86_64 build:
Brave playing YouTube was assigned to Desktop; Both included it in the recording,
Headphones Only excluded it, and returning to Both restored it. Headphone playback
continued throughout. The running binary was verified against the installed build
(SHA-256 `3640059d24b1d41e173733d19b5dc0922a7f3ef71b2adc7899eab9a14b6b1fe9`),
with one Rostrum instance. Read-only inspection identified OBS's capture as
`rostrum.stream.monitor`, not the headphones/default monitor. This is a user-reported
desktop/OBS confirmation of live isolation, not an AppImage or broad hardware test.
The prior AppImage did not include the registry-removal fix.

## Limits

The original three scenarios are focused engine/policy integration checks, not end-to-end UI, OBS,
PulseAudio compatibility, ALSA/Bluetooth hardware, permission-portal, DSP, latency,
performance or subjective quality tests. Persistence coverage reads saved device
and fallback settings; it does not exercise the UI saving them. Live Desktop destination
changes use the engine API, not the QML control; Game's original destination check
still uses a driver restart. Observations check settled
routing and short capture windows; they do not prove zero transient leakage at
every sample during hotplug or arbitrary startup races. Fresh sessions have no
third-party audio processors or restored user policy. No guarantee is made for
other WirePlumber profiles or distributions until exercised there.
