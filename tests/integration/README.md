# Isolated audio safety tests

This suite runs the existing `rostrum-graphtest` driver (real Engine/PwContext,
no UI) against a fresh PipeWire daemon, WirePlumber policy manager and D-Bus
bus for **each scenario**. No real devices are discovered. No network calls,
telemetry or audio delivery outside the private graph are introduced.

## Requirements and execution

Linux with readable `/proc`, child subreapers and pidfd support (kernel 5.3+
with these syscalls allowed), Python 3.9+ (standard library), PipeWire 1.x with `pipewire`, `pw-cat`,
`pw-cli`, `pw-dump`, `pw-link`, WirePlumber 0.5+ with its installed configuration
and scripts in `/usr/share/wireplumber`, `dbus-daemon`, and `pipewire-pulse` and `pacat` for the
rule fragment test. On Ubuntu 26.04 the additional runtime packages are
`pipewire-bin pipewire-pulse pulseaudio-utils wireplumber dbus python3`.
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

SIGINT, SIGTERM and SIGHUP abort live work in the scenario runner, fixture program
and lifecycle regression driver. Once session cleanup starts, those signals are
deliberately ignored until reclamation finishes; a first signal arriving only at
that phase is not evidence of a live-work interruption. A dedicated fixture check
delivers all three signals during cleanup and requires reclamation to finish.
Supervisor startup inherits a blocked signal mask until its
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

Reclamation and durable completion acknowledgement are separate: after kernel
`ECHILD`, the supervisor allows up to one second to publish the final status
atomically. A transient write failure remains recorded as the original operational
error and is surfaced by the caller even after a later write confirms reclamation.
The confirmed job can then be closed repeatedly and its private tree removed.
If final publication remains unavailable, the caller cannot confirm cleanup and
retains its handle and private resources, even if the supervisor actually reaped
every child. This path never converts missing status into successful cleanup.
The one-second publication window is inside the existing eight-second caller
deadline; filesystem operations that block beyond that deadline still fail closed.

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
Final-status regressions inject both a one-time and a persistent failure specifically
on the completion write, require full descendant reaping, preserve the first error,
and distinguish confirmed repeatable cleanup from unacknowledged retained resources.

Live interruption tests use a private Unix socket inside the owned test tree.
After positive deterministic audio delivery, the fixture (or nested startup runner)
announces readiness, accepts the parent's arm request, acknowledges that it is held
and cannot advance to cleanup without interruption. The parent sends through a
pinned process identity and requires the matching live-phase signal exception,
exit code 1, real supervisor completion statuses copied before their files are
removed, no owned descendants and removal of the private tree. Nested runners use
the same hold and report their cleanup separately. Socket waits are bounded to
12 seconds; missing events fail, never skip or retry. The internal `--checkpoint`
argument activates this test-only protocol; ordinary scenario execution is unchanged.
Logs and process scans are not readiness synchronization. Cleanup still relies on
the existing subreaper/pidfd ownership and kernel `ECHILD` proof.

This replaces the raced log checkpoint in CI run 37190525757 (SIGINT saw exit 0).
The original failure evidence and cleanup-phase diagnostic remain in
`build-appimage-validation/out/hosted-97b5dd6-run37190536318/`. The exact phase of
that historical CI signal was not observed; the new protocol proves the intended
phase rather than assuming it from timing. No production audio behavior changed.

## Rule fragments

`audio_rule_fragments` (`rule-fragments.py`, needs `pipewire-pulse` and `pacat` from
`pipewire-pulse` and `pulseaudio-utils`) has the real exporter write both fragments for a
`name:SafetyPlayer=game` rule into the private configuration, then stops the driver so only the
fragments and WirePlumber place streams; the Game bus lingers as it does after a quit. A native
`pw-cat` and a PulseAudio `pacat` client (through a private `pipewire-pulse` identified by its
process and config name) each play 440 Hz and record from the default source, a fake mic carrying
330 Hz. Playback must carry the rule's `target.object` and reach Game with the tone. Each recording
must carry no rule target, be linked only from the fake mic and record 330 Hz. WirePlumber 0.5 does
not link a capture stream to a sink named by `target.object`, so the property check is what catches
a rule reaching a recording; the audio check proves the app still hears its mic.

## Stream replacement

`audio_stream_replacement` (`stream-replacement.py`) uses `rostrum-test-stream`
(`stream-recreate.cpp`), a playback client that, on request, destroys its stream and creates an
identical one in the same loop iteration, as browsers do. It opts out of WirePlumber's target
restore, so only Rostrum's router can place the new stream. With a `name:SafetyPlayer=game` rule
and Game sent to the stream only, each of three replacements must get the old node ID back with a
new `object.serial` (otherwise the run fails: it could not exercise ID reuse), be linked only to
Game, deliver 440 Hz there, and leave the fake headphones silent. Before the fix the router kept
the old stream's "already requested" state and left the replacement on the default sink.

## WirePlumber restart

`audio_wireplumber_restart` (`wireplumber-restart.py`) routes the same no-restore test app to
Game, then twice stops the private WirePlumber, waits until its `default` metadata is gone and
starts a new instance. Each time the app must be linked only to Game again, with 440 Hz at Game and
the Stream Mix and silence at the fake headphones. Before the fix Rostrum kept the old metadata's
`target.object` values in its cache, believed the app was still routed and left it on the default
sink.

## PipeWire restart

`audio_pipewire_restart` (`pipewire-restart.py`) stops the private WirePlumber and PipeWire and
starts them again, twice: once with Rostrum reconnecting before the test app and once after it.
The run must reuse the old bus IDs and serials (it fails otherwise, since it could not exercise
ID reuse). Each time the app must be only on Game, Music muted again by solo, and Game must reach
the stream only. The driver reconnects when `--reconnect-file` holds a new request id, as the
app's retry timer would. This is a regression test, not a red one: it also passes without the
state reset on disconnect, because the first pass after reconnecting already drops per-ID state.

## Mic filter release

`audio_mic_filter_release` (`mic-filter-release.py`, registered when the DSP plugin is built)
leaves a call app on `rostrum.filtered` with its `target.object` in the metadata, stops the
driver, and starts it again without filters: once with `--mic-filters-unavailable` (settings
still enabled, no filter library) and once with filters turned off in settings. Each time the
app's target must be cleared, the app linked only from the mic and recording 440 Hz,
`rostrum.micfx` gone while `rostrum.filtered` stays, and the driver's log must show the app pointed
back before micfx was removed (and never `rostrum.filtered` removed), without the 3 s wait running
out. Before the fix the first start left the stale target for good, and both removed the nodes on
the first pass, before the metadata had been read.

## Mic filter crash breaker

`audio_mic_filter_breaker` (`mic-filter-breaker.py`, registered when the DSP plugin is built)
records the stream mic so the filter node runs, then restarts the private PipeWire twice. Both
losses must count as strikes, and the filter nodes must be gone afterwards. The breaker now times
from when the filter node began running with the graph, not from when controls were last sent.
The other side, that a restart with the filter node idle does not count, is not covered here:
every node in the private session stays running.

## Quit signals

`audio_quit_signals` (`quit-signals.py`, registered only when the app is built) runs the real
`rostrum` offscreen in the private session. The driver writes the default scene first; the test
adds a rule that mutes the test app. For SIGTERM, SIGINT and SIGHUP in turn, sent through a pinned
pidfd, the app's stream must be muted with Game silent while Rostrum runs, and after the signal
Rostrum must exit with status 0 and leave the stream unmuted, with 440 Hz back on the lingering
Game bus. Before the fix SIGTERM ended Rostrum with status -15 and left the app muted.

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
