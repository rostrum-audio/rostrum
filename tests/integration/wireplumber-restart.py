#!/usr/bin/env python3
"""After WirePlumber restarts, Rostrum routes a ruled app back to its bus.

WirePlumber's links and its "default" metadata go with it. The new instance relinks every stream
by its own properties, so the app lands on the default sink until Rostrum sets target.object
again. The app opts out of WirePlumber's target restore, so only Rostrum's router can move it.
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


def targets(s, graph, name):
    node = s.node(graph, name)
    ids = {o["info"].get("input-node-id") for o in graph
           if o["type"].endswith(":Link") and o["info"].get("output-node-id") == node["id"]}
    return {o["info"]["props"].get("node.name") for o in graph if o["type"].endswith(":Node") and o["id"] in ids}


def restart_policy(s):
    old = [p for p, log in s.processes if log.name.endswith("-wireplumber.log")]
    if len(old) != 1:
        raise Failure(f"Expected one private WirePlumber job, found {len(old)}")
    s.stop(old[0])
    s.wait("WirePlumber's default metadata gone",
           lambda: 'metadata.name = "default"' not in s.command("pw-cli", "ls", "Metadata"))
    manager = s.spawn(["wireplumber", "-p", "rostrum-test"])
    s.wait("restarted private WirePlumber and default metadata", lambda: s.manager_ready(manager.pid))


def main(driver, client):
    s = safety.Session(driver)
    try:
        s.start()
        s.device("test.headphones")
        s.spawn([str(driver), "--headphones", "test.headphones", "--dest", "game=stream",
                 "--rule", f"name:{APP}=game"])
        s.wait("Game bus", lambda: s.node(s.graph(), "rostrum.game"))
        control = s.root / "replace-request"
        control.write_text("")
        s.spawn([str(client), "--app", APP, "--control", str(control), "--no-restore-target"])
        s.route(APP, "rostrum.game")
        s.capture("rostrum.game", 440)
        for restart in range(1, 3):
            print(f"  WirePlumber restart {restart}", flush=True)
            restart_policy(s)
            s.route(APP, "rostrum.game")
            s.wait(f"after restart {restart}, {APP} only on Game",
                   lambda: targets(s, s.graph(), APP) == {"rostrum.game"})
            s.route("rostrum.game", "rostrum.stream")
            s.capture("rostrum.game", 440)
            s.capture("rostrum.stream", 440)
            s.capture("test.headphones")  # Game goes to the stream only.
        print("PASS a ruled app is routed back to its bus after WirePlumber restarts", flush=True)
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
