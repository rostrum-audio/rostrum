#!/usr/bin/env python3
"""A paused player plays on its bus after WirePlumber restarts.

A music player closes its stream while paused and keeps its connection to PipeWire. If WirePlumber
restarts meanwhile, the sound card goes with it (WirePlumber owns the device nodes) and comes back,
and the player's next stream is a new one that WirePlumber and Rostrum have never seen. It has to
reach its bus like any other new stream, without the player being restarted (issue #29).

With the filter plugin given, Studio Voice is on, as it was when #29 was seen: the mic goes and
comes back too, and the filter node's ports are rebuilt during the restart.
"""
import importlib.util
from pathlib import Path
import signal
import sys

HERE = Path(__file__).parent


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, HERE / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


safety = load("safety", "audio-safety.py")
restart = load("wireplumber_restart", "wireplumber-restart.py")
Failure = safety.Failure
APP = "IdlePlayer"
MIC = "test.saved-mic"


def restart_with_devices(s, mic):
    """WirePlumber's restart as on a desktop: its device nodes go with it and come back after."""
    devices = ["test.headphones"] + ([MIC] if mic else [])
    for name in devices:
        s.command("pw-cli", "destroy", str(s.node(s.graph(), name)["id"]))
    s.wait("sound card gone", lambda: all(s.node(s.graph(), name) is None for name in devices))
    restart.restart_policy(s)
    s.device("test.headphones")
    if mic:
        s.device(MIC, source=True)


def main(driver, client, plugin=None):
    s = safety.Session(driver)
    try:
        s.start()
        s.device("test.headphones")
        args = [str(driver), "--headphones", "test.headphones", "--rule", f"name:{APP}=music"]
        if plugin:
            s.device(MIC, source=True)
            config = s.root / "config/rostrum"
            config.mkdir()
            (config / "settings.toml").write_text("format = 1\n[mic_filters]\nenabled = true\n")
            args += ["--mic", MIC, "--mic-filter-plugin", str(plugin)]
        s.spawn(args)
        s.wait("Music bus", lambda: s.node(s.graph(), "rostrum.music"))
        if plugin:
            s.route(MIC, "rostrum.micfx")
            s.route("rostrum.micfx", "rostrum.filtered")
        control = s.root / "player-request"
        control.write_text("")
        # Like a desktop player: WirePlumber may restore where the stream last went.
        s.spawn([str(client), "--app", APP, "--control", str(control)])
        s.route(APP, "rostrum.music")
        s.capture("rostrum.music", 440)
        for attempt in range(1, 3):
            print(f"  paused across WirePlumber restart {attempt}", flush=True)
            control.write_text(f"pause {attempt}\n")
            s.wait("player's stream closed", lambda: s.node(s.graph(), APP) is None)
            restart_with_devices(s, plugin is not None)
            control.write_text(f"play {attempt}\n")
            s.wait("player's new stream", lambda: s.node(s.graph(), APP))
            s.route(APP, "rostrum.music")
            s.wait(f"after restart {attempt}, {APP} only on Music",
                   lambda: restart.targets(s, s.graph(), APP) == {"rostrum.music"})
            s.capture("rostrum.music", 440)
            if plugin:
                s.route(MIC, "rostrum.micfx")
                s.route("rostrum.micfx", "rostrum.filtered")
        print("PASS a player paused across a WirePlumber restart plays on its bus again", flush=True)
    except BaseException:
        safety.cleanup_preserving_failure(s.diagnostics)
        raise
    finally:
        safety.cleanup_preserving_failure(s.close)
        safety.cleanup_preserving_failure(safety.close_sessions)


if __name__ == "__main__":
    for sig in safety.INTERRUPTS:
        signal.signal(sig, lambda signum, _: (_ for _ in ()).throw(Failure(f"Interrupted by {signum}")))
    try:
        plugin = Path(sys.argv[3]).resolve(strict=True) if len(sys.argv) > 3 else None
        main(Path(sys.argv[1]).resolve(strict=True), Path(sys.argv[2]).resolve(strict=True), plugin)
    except Failure as error:
        safety.report(f"FAIL: {error}")
        sys.exit(1)
