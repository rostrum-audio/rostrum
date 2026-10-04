# Isolated audio safety tests

This suite runs the existing `rostrum-graphtest` driver (real Engine/PwContext,
no UI) against a fresh PipeWire daemon, WirePlumber policy manager and D-Bus
bus for **each scenario**. No real devices are discovered. No network calls,
telemetry or audio delivery outside the private graph are introduced.

## Requirements and execution

Linux, Python 3 (standard library), PipeWire 1.x with `pipewire`, `pw-cat`,
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
```

`--scenario startup|destinations|microphones` selects one scenario for diagnosis;
the default is all three. Every run/scenario must finish; failure stops the batch
and reports the completed clean-run count. Missing tools, incompatible policy,
stalled recordings and timeouts fail rather than skip. CTest registration is
opt-in; CI enables it on the existing Ubuntu 26.04 unit-test container. CTest
also runs the fixture guard and live negative-control tests. Tests are serialized
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

Owned child process groups are registered with handled signals blocked; SIGINT,
SIGTERM and SIGHUP produce failure and trigger cleanup. Cleanup terminates children
in reverse order, escalates to SIGKILL after a bounded wait, closes recordings
and removes the tree, on success and failure. SIGKILL of the harness, kernel crash
or power loss cannot run cleanup. In that case stop orphaned test processes and
remove their `rostrum-safety-*` tree; never target desktop audio processes.
Failure diagnostics print only the private graph and private child logs before
removal. `/tmp/rostrum-safety-*` contains local deterministic test audio while a
scenario is active; no test recordings persist after normal cleanup.

## Verified scenarios

1. **Startup before buses:** a 440 Hz playback app names the not-yet-existing Game
   bus, initially plays through fake headphones, then moves to Game when Rostrum
   starts. Active links and captures at Game and Stream verify the intended route.
2. **Destination isolation:** Game set to phones produces a tone at fake headphones
   and recorded silence at Stream. Restarting the driver with Game set to stream
   reverses those assertions. Positive captures accompany silence assertions.
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
overrides, test child/tree cleanup and SIGTERM, reject absent expected audio, and
prove that an injected forbidden-destination signal fails the silence assertion.

## Limits

These are focused engine/policy integration checks, not end-to-end UI, OBS,
PulseAudio compatibility, ALSA/Bluetooth hardware, permission-portal, DSP, latency,
performance or subjective quality tests. Persistence coverage reads saved device
and fallback settings; it does not exercise the UI saving them. Destination changes
use a driver restart rather than the UI's live toggle. Observations check settled
routing and short capture windows; they do not prove zero transient leakage at
every sample during hotplug or arbitrary startup races. Fresh sessions have no
third-party audio processors or restored user policy. No guarantee is made for
other WirePlumber profiles or distributions until exercised there.
