#!/usr/bin/env python3
"""After a private PipeWire restart, warn when OBS still runs but no longer captures Rostrum.

A stand-in renames its process to obs and runs an OBS-named pw-cat capture on rostrum.mic. Once
that baseline settles, PipeWire restarts and Rostrum reconnects without bringing the capture back:
CaptureWatch must raise capture-lost. Killing the stand-in OBS or respawning capture during grace
must stay quiet.
"""
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import signal
import sys
import time

spec = importlib.util.spec_from_file_location("restart", Path(__file__).with_name("pipewire-restart.py"))
restart = importlib.util.module_from_spec(spec)
spec.loader.exec_module(restart)
safety = restart.safety
Failure = safety.Failure

BASELINE_MS = 2500
GRACE_MS = 8500
EPOCH_CAP_MS = 21000
SOURCE = "Mic/Aux"
FAKE_OBS = """
import ctypes, time
libc = ctypes.CDLL(None, use_errno=True)
libc.prctl(15, b"obs", 0, 0, 0)
while True:
    time.sleep(3600)
"""


def host_obs_running():
    uid = os.getuid()
    proc = Path("/proc")
    if not proc.is_dir():
        return False
    for name in proc.iterdir():
        if not name.name.isdigit():
            continue
        comm = name / "comm"
        try:
            if comm.read_text().strip() != "obs":
                continue
            if os.stat(name).st_uid != uid:
                continue
            return True
        except OSError:
            continue
    return False


def read_capture_lost(path):
    if not path.exists():
        return None
    return json.loads(path.read_text())


def restart_daemon(s, log, lost):
    restart.restart_daemon(s, log, lost)


def fake_obs(s):
    return s.spawn([sys.executable, "-c", FAKE_OBS])


def start_obs_stand_in(s):
    if host_obs_running():
        return None
    return fake_obs(s)


def wait_running_obs(s, path, obs):
    if obs is None:
        s.wait("host OBS visible to CaptureWatch",
               lambda: (row := read_capture_lost(path)) and row.get("obsPid"))
    else:
        wait_obs_pid(s, path, obs.pid)


def engine(s, driver, reconnect, capture_lost):
    s.device("test.headphones")
    engine = s.spawn([str(driver), "--headphones", "test.headphones", "--reconnect-file", str(reconnect),
                      "--capture-lost-file", str(capture_lost)])
    log = next(log for child, log in s.processes if child is engine)
    s.wait("Rostrum mix ready", lambda: "Mix ready." in log.read_text())
    return engine, log


def obs_capture(s, obs_name=f"OBS: {SOURCE}"):
    mic = s.wait("rostrum.mic", lambda: s.node(s.graph(), "rostrum.mic"))
    serial = mic["info"]["props"]["object.serial"]
    props = (f'{{ application.name = OBS node.name = "{obs_name}" media.name = "{SOURCE}" '
             "rostrum.internal = true node.dont-move = true node.dont-fallback = true }")
    capture = s.spawn(["pw-cat", "-r", "-a", "--format", "f32", "--rate", str(safety.RATE), "--channels", "2",
                       "--target", str(serial), "-P", props, "-", "/dev/null"])
    s.wait(obs_name, lambda: s.node(s.graph(), obs_name))
    s.route("rostrum.mic", obs_name)
    return capture


def wait_obs_pid(s, path, pid):
    s.wait("CaptureWatch sees the stand-in OBS", lambda: (row := read_capture_lost(path)) and row.get("obsPid") == pid)


def settle_baseline(capture_lost):
    time.sleep(BASELINE_MS / 1000)
    row = read_capture_lost(capture_lost)
    if not row or row.get("alert"):
        raise Failure("Capture baseline must be settled and quiet before PipeWire restarts")


def wait_alert(capture_lost, want, within_ms):
    deadline = time.monotonic() + within_ms / 1000

    def due():
        row = read_capture_lost(capture_lost)
        return row and bool(row.get("alert")) == want

    while time.monotonic() < deadline:
        if due():
            return read_capture_lost(capture_lost)
        time.sleep(0.1)
    row = read_capture_lost(capture_lost)
    state = "set" if row and row.get("alert") else "clear"
    raise Failure(f"Timed out waiting for capture-lost alert {'set' if want else 'clear'} (still {state})")


def scenario_loss(s, driver):
    reconnect = s.root / "reconnect-request"
    reconnect.write_text("")
    capture_lost = s.root / "capture-lost.json"
    capture_lost.write_text("")
    obs = start_obs_stand_in(s)
    try:
        _, log = engine(s, driver, reconnect, capture_lost)
        wait_running_obs(s, capture_lost, obs)
        capture = obs_capture(s)
        settle_baseline(capture_lost)
        s.stop(capture)
        restart_daemon(s, log, 1)
        reconnect.write_text("1\n")
        s.wait("Rostrum reconnected", lambda: "Reconnect request 1" in log.read_text())
        s.wait("mix ready after reconnect", lambda: log.read_text().count("Mix ready.") >= 2)
        row = wait_alert(capture_lost, True, GRACE_MS + 4000)
        if not row.get("missing"):
            raise Failure("Expected missing capture devices when capture-lost is set")
        print("  positive: capture-lost after PipeWire restart without OBS capture", flush=True)
    finally:
        if obs is not None:
            s.stop(obs)


def scenario_obs_gone(s, driver):
    reconnect = s.root / "reconnect-request"
    reconnect.write_text("")
    capture_lost = s.root / "capture-lost-gone.json"
    if host_obs_running():
        print("  negative: skip obs-gone while a host OBS process is running", flush=True)
        return
    obs = start_obs_stand_in(s)
    try:
        _, log = engine(s, driver, reconnect, capture_lost)
        wait_running_obs(s, capture_lost, obs)
        capture = obs_capture(s)
        settle_baseline(capture_lost)
        s.stop(capture)
        restart_daemon(s, log, 1)
        reconnect.write_text("1\n")
        s.wait("Rostrum reconnected", lambda: "Reconnect request 1" in log.read_text())
        s.stop(obs)
        wait_alert(capture_lost, False, EPOCH_CAP_MS)
        print("  negative: no alert when the stand-in OBS exits during grace", flush=True)
    finally:
        if obs is not None:
            try:
                s.stop(obs)
            except Failure:
                pass


def scenario_capture_returns(s, driver):
    reconnect = s.root / "reconnect-request"
    reconnect.write_text("")
    capture_lost = s.root / "capture-lost-return.json"
    obs = start_obs_stand_in(s)
    try:
        _, log = engine(s, driver, reconnect, capture_lost)
        wait_running_obs(s, capture_lost, obs)
        capture = obs_capture(s)
        settle_baseline(capture_lost)
        s.stop(capture)
        restart_daemon(s, log, 1)
        reconnect.write_text("1\n")
        s.wait("Rostrum reconnected", lambda: "Reconnect request 1" in log.read_text())
        s.wait("mix ready after reconnect", lambda: log.read_text().count("Mix ready.") >= 2)
        obs_capture(s)
        wait_alert(capture_lost, False, EPOCH_CAP_MS)
        print("  negative: no alert when OBS capture returns during grace", flush=True)
    finally:
        if obs is not None:
            s.stop(obs)


def main(driver):
    s = safety.Session(driver)
    try:
        s.start()
        scenario_loss(s, driver)
        scenario_obs_gone(s, driver)
        scenario_capture_returns(s, driver)
        print("PASS capture-lost detection and negative controls on private PipeWire", flush=True)
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
        main(Path(sys.argv[1]).resolve(strict=True))
    except Failure as error:
        safety.report(f"FAIL: {error}")
        sys.exit(1)
