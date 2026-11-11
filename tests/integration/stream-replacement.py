#!/usr/bin/env python3
"""A ruled app that replaces its stream (same node ID, new serial) is routed again every time.

Browsers replace their playback stream on track or format changes. The removal and the new node
reach Rostrum in one burst, and PipeWire hands the new node the freed ID. The app opts out of
WirePlumber's target restore, so only Rostrum's router can put the new stream back on its bus.
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


def targets(s, graph, node):
    ids = {o["info"].get("input-node-id") for o in graph
           if o["type"].endswith(":Link") and o["info"].get("output-node-id") == node["id"]}
    return {o["info"]["props"].get("node.name") for o in graph if o["type"].endswith(":Node") and o["id"] in ids}


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
        app = s.spawn([str(client), "--app", APP, "--control", str(control), "--no-restore-target"])
        log = next(log for child, log in s.processes if child is app)
        s.route(APP, "rostrum.game")
        s.capture("rostrum.game", 440)
        for attempt in range(1, 4):
            graph = s.graph()
            old = s.node(graph, APP)
            old_serial = old["info"]["props"]["object.serial"]
            control.write_text(f"{attempt}\n")
            s.wait(f"replacement {attempt}", lambda: f"replaced {attempt}\n" in log.read_text())

            def replaced():
                node = s.node(s.graph(), APP)
                return node if node and node["info"]["props"]["object.serial"] != old_serial else None
            new = s.wait(f"replacement {attempt} stream", replaced)
            if new["id"] != old["id"]:
                raise Failure(f"PipeWire gave the replacement stream ID {new['id']}, not {old['id']}: "
                              "this run cannot exercise ID reuse")
            print(f"  replacement {attempt}: node {new['id']} reused, serial {old_serial} -> "
                  f"{new['info']['props']['object.serial']}", flush=True)
            s.route(APP, "rostrum.game")
            s.wait(f"replacement {attempt} only on Game",
                   lambda: targets(s, s.graph(), s.node(s.graph(), APP)) == {"rostrum.game"})
            s.capture("rostrum.game", 440)
            s.capture("test.headphones")  # Game goes to the stream only: nothing leaks to the phones.
        print("PASS replaced streams with reused node IDs are routed again", flush=True)
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
