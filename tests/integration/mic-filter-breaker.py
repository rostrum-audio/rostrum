#!/usr/bin/env python3
"""The mic-filter crash breaker trips when PipeWire goes away twice right after the filters ran.

Rostrum turns the filters off when PipeWire goes away twice within seconds of the filter graph
starting to run, since that points at the filter library. Here an app records the stream mic, so
the filter node runs, and PipeWire is restarted twice: both must count and the filter nodes must
be gone afterwards. The other side, that a restart with the filter node idle does not count, is
not covered: every node in the private session stays running.
"""
import argparse
import importlib.util
from pathlib import Path
import signal


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(file))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


restart = load("restart", "pipewire-restart.py")
safety = restart.safety
Failure = safety.Failure
SETTINGS = '''format = 1
[mic_filters]
enabled = true
[mic_filters.highpass]
enabled = true
frequency = 80
steep = true
[mic_filters.denoise]
enabled = false
[mic_filters.gate]
enabled = false
[mic_filters.eq]
enabled = false
[mic_filters.compressor]
enabled = false
[mic_filters.limiter]
enabled = false
'''


def restart_daemon(s, log, lost, headphones):
    # As pipewire-restart.py does, but an output device comes back only when the run has one:
    # any output keeps the whole mix, and with it the filter node, running.
    s.stop(restart.job(s, "-wireplumber.log"))
    s.stop(restart.job(s, "-pipewire.log"))
    s.wait("Rostrum noticed PipeWire went away",
           lambda: log.read_text().count("Lost the connection to PipeWire") == lost)
    s.spawn(["pipewire", "-c", str(s.root / "config/pipewire/pipewire.conf")])
    s.wait("restarted private PipeWire socket", lambda: s.remote.is_socket())
    s.graph()
    manager = s.spawn(["wireplumber", "-p", "rostrum-test"])
    s.wait("restarted private WirePlumber and default metadata", lambda: s.manager_ready(manager.pid))
    if headphones:
        s.device("test.headphones")


def run(driver, plugin, recording):
    label = "an app recording the mic" if recording else "an idle mic"
    s = safety.Session(driver)
    try:
        s.start()
        s.device("test.saved-mic", source=True)
        if recording:
            s.device("test.headphones")
        config = s.root / "config/rostrum"
        config.mkdir()
        (config / "settings.toml").write_text(SETTINGS)
        reconnect = s.root / "reconnect-request"
        reconnect.write_text("")
        engine = s.spawn([str(driver), "--mic", "test.saved-mic", "--headphones", "test.headphones",
                          "--mic-filter-plugin", str(plugin), "--reconnect-file", str(reconnect)])
        log = next(log for child, log in s.processes if child is engine)

        def recorder():
            # Records the stream mic, which pulls the filter node into running.
            s.wait("stream mic", lambda: s.node(s.graph(), "rostrum.mic"))
            serial = s.node(s.graph(), "rostrum.mic")["info"]["props"]["object.serial"]
            sink = (s.root / "breaker-capture.raw").open("wb")
            s.files.append(sink)
            reader = s.spawn(["pw-cat", "-r", "-a", "--format", "f32", "--rate", str(safety.RATE), "--channels", "1",
                              "--target", str(serial), "-P",
                              "{ node.name = BreakerRecorder rostrum.internal = true node.dont-move = true "
                              "node.dont-fallback = true }", "-"], sink)
            s.route("rostrum.micfx", "rostrum.mic")
            s.wait("filter graph running", lambda: log.read_text().count("mic filter graph running") >= loads)
            return reader

        loads = 0
        for lost in (1, 2):
            loads += 1
            s.wait(f"filter graph sent ({loads})",
                   lambda: log.read_text().count("loading mic filter graph") >= loads)
            reader = recorder() if recording else None
            if reader:
                s.stop(reader)  # Its connection goes with the daemon.
            restart_daemon(s, log, lost, recording)
            s.device("test.saved-mic", source=True)
            reconnect.write_text(f"{lost}\n")
            s.wait(f"Rostrum reconnected ({lost})", lambda: f"Reconnect request {lost}" in log.read_text())
        text = log.read_text()
        strikes = text.count("after the mic filters loaded, strike")
        if strikes != (2 if recording else 0):
            raise Failure(f"With {label}, two PipeWire restarts counted {strikes} strikes")
        if recording:
            s.wait("filters turned off", lambda: s.node(s.graph(), "rostrum.mic") and
                   not s.node(s.graph(), "rostrum.micfx"))
        else:
            s.wait("filters still on", lambda: log.read_text().count("loading mic filter graph") >= 3)
        print(f"PASS with {label}, two PipeWire restarts counted {strikes} strikes", flush=True)
    except BaseException:
        safety.cleanup_preserving_failure(s.diagnostics)
        raise
    finally:
        safety.cleanup_preserving_failure(s.close)
        safety.cleanup_preserving_failure(safety.close_sessions)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path)
    parser.add_argument("plugin", type=Path)
    args = parser.parse_args()
    for sig in safety.INTERRUPTS:
        signal.signal(sig, lambda signum, _: (_ for _ in ()).throw(Failure(f"Interrupted by signal {signum}")))
    try:
        run(args.driver.resolve(strict=True), args.plugin.resolve(strict=True), True)
    except Failure as error:
        safety.report(f"FAIL: {error}")
        raise SystemExit(1)
