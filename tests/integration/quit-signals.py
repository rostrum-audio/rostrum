#!/usr/bin/env python3
"""SIGTERM, SIGINT and SIGHUP quit the real app normally, so it unmutes the apps it muted.

The saved default scene has a rule that mutes the test app. While Rostrum runs, the app's stream
is muted and Game is silent. After each signal Rostrum must exit with status 0, having run its
normal quit path, and leave the stream unmuted and playing on Game (the bus lingers). Logging out
or `systemctl --user stop` sends SIGTERM; quitting must never leave an app silent.
"""
import importlib.util
from pathlib import Path
import signal
import sys

spec = importlib.util.spec_from_file_location("safety", Path(__file__).with_name("audio-safety.py"))
safety = importlib.util.module_from_spec(spec)
spec.loader.exec_module(safety)
from process_lifecycle import Identity  # noqa: E402  (importable once audio-safety.py is loaded)

Failure = safety.Failure
APP = "SafetyPlayer"


def stream_muted(s):
    """The test app's stream mute from its Props, or None while unknown."""
    node = s.node(s.graph(), APP)
    if not node:
        return None
    props = node["info"].get("params", {}).get("Props", [])
    values = [p["mute"] for p in props if isinstance(p, dict) and "mute" in p]
    return values[0] if values else None


def seed_config(s, graphtest):
    config = s.root / "config/rostrum"
    config.mkdir(parents=True, exist_ok=True)
    (config / "settings.toml").write_text('''format = 1
[general]
wizard_done = true
setup_version = 2
[updates]
check = false
[devices]
headphones = "test.headphones"
''')
    # The driver writes the default scene the app would create on first start.
    seeder = s.spawn([str(graphtest), "--config"])
    s.wait("default scene written", lambda: list((config / "scenes").glob("*.toml")))
    s.stop(seeder)
    scenes = list((config / "scenes").glob("*.toml"))
    if len(scenes) != 1:
        raise Failure(f"Expected one default scene, found {scenes}")
    with scenes[0].open("a") as f:
        f.write(f'''
[[rule]]
match = "{APP}"
key = "name"
bus = "game"
volume = 1.0
muted = true
''')


def main(app_binary, graphtest, client):
    s = safety.Session(graphtest)
    try:
        s.start()
        s.device("test.headphones")
        seed_config(s, graphtest)
        s.env["ROSTRUM_NO_GLOBAL_SHORTCUTS"] = "1"
        control = s.root / "replace-request"
        control.write_text("")
        s.spawn([str(client), "--app", APP, "--control", str(control)])
        s.wait("test app stream", lambda: s.node(s.graph(), APP))
        for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            name = signal.Signals(sig).name
            print(f"  {name}", flush=True)
            rostrum = s.spawn([str(app_binary)])
            s.route(APP, "rostrum.game")
            s.wait(f"{APP} muted by its rule", lambda: stream_muted(s) is True)
            s.capture("rostrum.game")
            identity = Identity(rostrum.pid, rostrum.process.pid)
            try:
                if not identity.send(sig):
                    raise Failure(f"Rostrum exited before {name} could be sent")
            finally:
                identity.close()
            # Session.wait treats any exit as a failure; this one is expected.
            try:
                rostrum.wait(timeout=10)
            except safety.subprocess.TimeoutExpired as error:
                raise Failure(f"{name}: Rostrum did not exit") from error
            if rostrum.returncode != 0:
                raise Failure(f"{name}: Rostrum did not quit normally (exit status {rostrum.returncode})")
            s.stop(rostrum)
            if stream_muted(s) is not False:
                raise Failure(f"{name}: Rostrum left {APP} muted")
            s.capture("rostrum.game", 440)
        print("PASS SIGTERM, SIGINT and SIGHUP quit normally and leave no app muted", flush=True)
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
        main(*(Path(arg).resolve(strict=True) for arg in sys.argv[1:4]))
    except Failure as error:
        safety.report(f"FAIL: {error}")
        sys.exit(1)
