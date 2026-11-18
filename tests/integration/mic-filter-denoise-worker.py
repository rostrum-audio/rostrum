#!/usr/bin/env python3
"""Noise removal never holds up the PipeWire daemon's realtime thread.

The daemon runs every node in one realtime thread, so a node that takes long holds up all the
others, headphones included. RNNoise runs on the denoise plugin's worker thread. Here a test
switch makes that worker sleep before every tenth frame, and pw-profiler measures how long
rostrum.micfx holds the thread in each graph cycle:

- With run() made to wait for the worker, as when RNNoise ran inline, cycles where rostrum.micfx
  sits through the stall must show up. The worker starts sleeping when a frame is handed over and
  the frame is due a cycle later, so such a cycle holds the thread for about the stall minus one
  10.7 ms cycle. This proves the measurement sees the problem.
- With the real worker, no cycle may come near the stall. The late frames go out without noise
  removal, and the plugin logs them in the daemon's log."""
import argparse
import importlib.util
import json
from pathlib import Path
import signal
import uuid

spec = importlib.util.spec_from_file_location("safety", Path(__file__).with_name("audio-safety.py"))
safety = importlib.util.module_from_spec(spec)
spec.loader.exec_module(safety)

STALL_MS = 60
STALL_EVERY = 10  # frames of 10 ms, so about ten stalls a second
HELD_MS = 30      # a cycle where run() waited sits through about STALL_MS minus one cycle
CYCLES = 300      # about 3 s at the forced 512-sample cycle
LATE_LINE = "went out without noise removal"


def micfx_run_times(s):
    """How long rostrum.micfx ran in each of at least CYCLES graph cycles, in ms."""
    path = s.root / f"profile-{uuid.uuid4().hex}.json"
    output = path.open("wb")
    s.files.append(output)
    profiler = s.spawn(["pw-profiler", "-J"], output)
    s.wait(f"{CYCLES} profiled rostrum.micfx cycles",
           lambda: path.read_bytes().count(b'"name": "rostrum.micfx"') >= CYCLES)
    s.stop(profiler)
    times = []
    for line in path.read_text(errors="replace").splitlines():
        # A JSON array, one object per line: "[{ ... }," first, "{ } ]" last.
        line = line.strip().lstrip("[").rstrip("], ")
        if not line.startswith("{"):
            continue
        entry = json.loads(line)
        if entry.get("type") == "follower" and entry.get("name") == "rostrum.micfx" and \
                entry["finish"] > entry["awake"] > 0:
            times.append((entry["finish"] - entry["awake"]) / 1e6)
    if len(times) < CYCLES:
        raise safety.Failure(f"pw-profiler recorded only {len(times)} rostrum.micfx cycles")
    return times


def daemon_log(s):
    return "".join(log.read_text(errors="replace") for _, log in s.processes if log.name.endswith("-pipewire.log"))


def run(driver, plugin, wait):
    s = safety.Session(driver)
    try:
        s.env["ROSTRUM_DSP_TEST_DENOISE_STALL"] = f"{STALL_MS}:{STALL_EVERY}"
        if wait:
            s.env["ROSTRUM_DSP_TEST_DENOISE_WAIT"] = "1"
        s.start()
        s.device("test.saved-mic", source=True)
        s.device("test.headphones")
        config = s.root / "config/rostrum"
        config.mkdir()
        (config / "settings.toml").write_text('''format = 1
[mic_filters]
enabled = true
[mic_filters.highpass]
enabled = false
[mic_filters.denoise]
enabled = true
[mic_filters.gate]
enabled = false
[mic_filters.eq]
enabled = false
[mic_filters.compressor]
enabled = false
[mic_filters.limiter]
enabled = false
''')
        s.spawn([str(driver), "--mic", "test.saved-mic", "--headphones", "test.headphones",
                 "--mic-filter-plugin", str(plugin), "--mic-filter-denoise"])
        s.tone("WorkerMic", "0", 440, 1, internal=True)
        s.command("pw-link", "WorkerMic:output_MONO", "test.saved-mic:input_MONO")
        s.route("WorkerMic", "test.saved-mic")
        s.route("test.saved-mic", "rostrum.micfx")
        s.route("rostrum.micfx", "rostrum.filtered")
        # A call app recording the filtered mic keeps the graph running.
        audio = s.root / "recorder.raw"
        output = audio.open("wb")
        s.files.append(output)
        target = s.node(s.graph(), "rostrum.filtered")["info"]["props"]["object.serial"]
        recorder = s.spawn(["pw-cat", "-r", "-a", "--format", "f32", "--rate", "48000", "--channels", "1",
                            "--target", str(target), "-P", '''{ node.name = WorkerRecorder
                            rostrum.internal = true node.dont-fallback = true node.dont-move = true }''', "-"],
                           output)
        s.route("rostrum.filtered", "WorkerRecorder")
        s.wait("recorder buffers", lambda: audio.stat().st_size >= 96000)

        times = micfx_run_times(s)
        held = [t for t in times if t >= HELD_MS]
        late = LATE_LINE in daemon_log(s)
        mode = "run() waiting for the worker" if wait else "worker thread"
        print(f"  {mode}: {len(times)} cycles, rostrum.micfx longest {max(times):.2f} ms, "
              f"{len(held)} at or over {HELD_MS} ms; late frames logged: {late}", flush=True)
        if wait:
            if len(held) < 5:
                raise safety.Failure(f"with run() waiting, only {len(held)} cycles sat through a "
                                     f"{STALL_MS} ms stall: the measurement cannot see a held thread")
            if late:
                raise safety.Failure("with run() waiting, frames were still logged late")
        else:
            if held:
                raise safety.Failure(f"rostrum.micfx held the realtime thread for {max(times):.2f} ms "
                                     f"in {len(held)} cycles: run() waited for the worker")
            if not late:
                raise safety.Failure(f"the worker slept {STALL_MS} ms but the daemon log has no late frames")
        s.stop(recorder)
    except BaseException:
        safety.cleanup_preserving_failure(s.diagnostics)
        raise
    finally:
        safety.cleanup_preserving_failure(s.close)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path)
    parser.add_argument("plugin", type=Path)
    args = parser.parse_args()
    for sig in safety.INTERRUPTS:
        signal.signal(sig, lambda signum, _: (_ for _ in ()).throw(safety.Failure(f"Interrupted by signal {signum}")))
    driver, plugin = args.driver.resolve(strict=True), args.plugin.resolve(strict=True)
    run(driver, plugin, wait=True)
    print(f"PASS the measurement sees rostrum.micfx hold the realtime thread when run() waits for "
          f"a {STALL_MS} ms RNNoise stall", flush=True)
    run(driver, plugin, wait=False)
    print(f"PASS with the worker thread, rostrum.micfx never holds the realtime thread through a "
          f"{STALL_MS} ms RNNoise stall; the late frames are logged", flush=True)
