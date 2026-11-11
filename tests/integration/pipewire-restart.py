#!/usr/bin/env python3
"""After PipeWire restarts, Rostrum routes a ruled app and sets bus levels again.

A restarted daemon numbers every object afresh, so IDs, and even serials, repeat. The first
restart rebuilds the session in its original order (daemon, WirePlumber, devices, Rostrum, then
the app): Rostrum's buses and the app come back under their old IDs and serials. The second
restart lets the app reconnect first, as apps usually do before Rostrum's retry timer fires, so
Rostrum finds it already playing. Each time the app must end up only on Game, the bus Rostrum
dims for solo must be muted again, and Game must reach the stream only. The driver reconnects
only when told, as the app's retry timer would, once the rest of the session is back.
"""
import importlib.util
from pathlib import Path
import signal
import sys

spec = importlib.util.spec_from_file_location("safety", Path(__file__).with_name("audio-safety.py"))
safety = importlib.util.module_from_spec(spec)
spec.loader.exec_module(safety)
Failure = safety.Failure
APP = "SafetyPlayer"
BUSES = ("rostrum.game", "rostrum.music")


def identity(graph, name):
    node = safety.Session.node(graph, name)
    if not node:
        raise Failure(f"Missing {name}")
    return node["id"], node["info"]["props"]["object.serial"]


def muted(s, name):
    node = s.node(s.graph(), name)
    props = node["info"].get("params", {}).get("Props", []) if node else []
    values = [p["mute"] for p in props if isinstance(p, dict) and "mute" in p]
    return values[0] if values else None


def job(s, suffix):
    found = [p for p, log in s.processes if log.name.endswith(suffix)]
    if len(found) != 1:
        raise Failure(f"Expected one private {suffix} job, found {len(found)}")
    return found[0]


def restart_daemon(s, driver_log, lost):
    s.stop(job(s, "-wireplumber.log"))
    s.stop(job(s, "-pipewire.log"))
    s.wait("Rostrum noticed PipeWire went away",
           lambda: driver_log.read_text().count("Lost the connection to PipeWire") == lost)
    s.spawn(["pipewire", "-c", str(s.root / "config/pipewire/pipewire.conf")])
    s.wait("restarted private PipeWire socket", lambda: s.remote.is_socket())
    s.graph()
    manager = s.spawn(["wireplumber", "-p", "rostrum-test"])
    s.wait("restarted private WirePlumber and default metadata", lambda: s.manager_ready(manager.pid))
    s.device("test.headphones")


def settled(s):
    s.route(APP, "rostrum.game")
    s.wait("Music dimmed by solo", lambda: muted(s, "rostrum.music") is True)
    s.route("rostrum.game", "rostrum.stream")
    s.capture("rostrum.game", 440)
    s.capture("test.headphones")  # Game goes to the stream only.


def main(driver, client):
    s = safety.Session(driver)
    try:
        s.start()
        s.device("test.headphones")
        reconnect = s.root / "reconnect-request"
        reconnect.write_text("")
        engine = s.spawn([str(driver), "--headphones", "test.headphones", "--dest", "game=stream",
                          "--rule", f"name:{APP}=game", "--solo", "game", "--reconnect-file", str(reconnect)])
        driver_log = next(log for child, log in s.processes if child is engine)
        s.wait("Game bus", lambda: s.node(s.graph(), "rostrum.game"))
        control = s.root / "replace-request"
        control.write_text("")
        args = [str(client), "--app", APP, "--control", str(control), "--no-restore-target"]
        app = s.spawn(args)
        settled(s)
        graph = s.graph()
        before = {name: identity(graph, name) for name in (APP,) + BUSES}

        for restart, app_first in ((1, False), (2, True)):
            print(f"  PipeWire restart {restart}: {'app' if app_first else 'Rostrum'} reconnects first",
                  flush=True)
            s.stop(app)  # Its connection goes with the daemon; it starts again afterwards, like an app.
            restart_daemon(s, driver_log, restart)
            if app_first:
                app = s.spawn(args)
                s.wait("test app stream before Rostrum is back", lambda: s.node(s.graph(), APP))
            reconnect.write_text(f"{restart}\n")
            s.wait(f"Rostrum reconnected ({restart})", lambda: f"Reconnect request {restart}" in driver_log.read_text())
            s.wait("Game and Music buses recreated",
                   lambda: (g := s.graph()) and all(s.node(g, name) for name in BUSES))
            if not app_first:
                app = s.spawn(args)
                s.wait("test app stream", lambda: s.node(s.graph(), APP))
                graph = s.graph()
                after = {name: identity(graph, name) for name in before}
                for name in before:
                    print(f"    {name}: id/serial {before[name]} -> {after[name]}", flush=True)
                # Stale state hides behind objects that come back under their old IDs.
                if any(after[name] != before[name] for name in BUSES):
                    raise Failure("PipeWire numbered the restarted buses differently: "
                                  "this run cannot exercise ID reuse")
            settled(s)
        print("PASS routing and bus levels are set again after PipeWire restarts", flush=True)
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
        main(Path(sys.argv[1]).resolve(strict=True), Path(sys.argv[2]).resolve(strict=True))
    except Failure as error:
        safety.report(f"FAIL: {error}")
        sys.exit(1)
