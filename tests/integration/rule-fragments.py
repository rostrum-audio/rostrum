#!/usr/bin/env python3
"""Exported app rules move only playback streams; a ruled app's own recording keeps its microphone.

The real rule exporter writes both fragments into the private configuration. The driver then
quits, so nothing but the fragments (read by native clients and a private pipewire-pulse) and
WirePlumber decide where the app's streams go. Rostrum's nodes linger, as they do after a quit.
"""
import importlib.util
from pathlib import Path
import shutil
import signal
import sys
import uuid

spec = importlib.util.spec_from_file_location("safety", Path(__file__).with_name("audio-safety.py"))
safety = importlib.util.module_from_spec(spec)
spec.loader.exec_module(safety)
Failure = safety.Failure
RATE = safety.RATE
APP = "SafetyPlayer"


def nodes_of(graph, media_name=None, node_name=None):
    return [o for o in graph if o["type"].endswith(":Node") and
            o["info"]["props"].get("application.name") == APP and
            (media_name is None or o["info"]["props"].get("media.name") == media_name) and
            (node_name is None or o["info"]["props"].get("node.name") == node_name)]


def peers(graph, node, outgoing):
    """node.name of every node linked to or from this one."""
    mine, theirs = ("output-node-id", "input-node-id") if outgoing else ("input-node-id", "output-node-id")
    ids = {o["info"].get(theirs) for o in graph
           if o["type"].endswith(":Link") and o["info"].get(mine) == node["id"]}
    return {o["info"]["props"].get("node.name") for o in graph if o["type"].endswith(":Node") and o["id"] in ids}


def one(s, description, find):
    """The single node find(graph) returns, once it exists."""
    def ready():
        found = find(s.graph())
        if len(found) > 1:
            raise Failure(f"{description}: {len(found)} matching nodes")
        return found[0] if found else None
    return s.wait(description, ready)


def check_playback(s, description, find):
    node = one(s, description, find)
    props = node["info"]["props"]
    if props.get("target.object") != "rostrum.game":
        raise Failure(f"{description}: rule fragment not applied (target.object={props.get('target.object')!r})")
    s.wait(f"{description} linked to Game", lambda: peers(s.graph(), one(s, description, find), True) == {"rostrum.game"})


def check_recording(s, description, find, path):
    node = one(s, description, find)
    target = node["info"]["props"].get("target.object")
    if target is not None:
        raise Failure(f"{description}: a rule reached a recording stream (target.object={target!r})")
    s.wait(f"{description} records only the microphone",
           lambda: peers(s.graph(), one(s, description, find), False) == {"test.mic"})
    s.wait(f"{description} buffers", lambda: path.stat().st_size >= RATE * 4)
    s.verify(description, path.read_bytes(), 330, 1)


def record_file(s, name):
    path = s.root / f"{name}-{uuid.uuid4().hex}.raw"
    output = path.open("wb")
    s.files.append(output)
    return path, output


def main():
    for name in ("pipewire-pulse", "pacat"):
        if not shutil.which(name):
            raise Failure(f"Missing required executable: {name} (suite cannot be skipped)")
    system_pulse = Path("/usr/share/pipewire/pipewire-pulse.conf")
    if not system_pulse.is_file():
        raise Failure(f"Missing {system_pulse}")
    s = safety.Session(Path(sys.argv[1]).resolve(strict=True))
    try:
        s.start()
        s.device("test.headphones", priority=2000)
        s.device("test.mic", source=True, priority=3000)
        s.tone("MicFeed", "0", 330, 1, internal=True)
        s.command("pw-link", "MicFeed:output_MONO", "test.mic:input_MONO")

        config = s.root / "config/pipewire"
        pulse_fragment = config / "pipewire-pulse.conf.d/50-rostrum.conf"
        client_fragment = config / "client.conf.d/50-rostrum.conf"
        engine = s.spawn([str(s.driver), "--headphones", "test.headphones", "--mic", "test.mic",
                          "--rule", f"name:{APP}=game", "--export-rules"])
        log = next(log for child, log in s.processes if child is engine)
        s.wait("rules exported", lambda: "Rules exported." in log.read_text())
        s.wait("Game bus", lambda: s.node(s.graph(), "rostrum.game"))
        for fragment in (pulse_fragment, client_fragment):
            if "dont-" in fragment.read_text():
                raise Failure(f"{fragment.name}: rule fragment pins streams")
        s.stop(engine)
        if not s.node(s.graph(), "rostrum.game"):
            raise Failure("Rostrum's Game bus did not linger after the driver quit")

        # Native PipeWire clients read client.conf.d.
        player = s.tone(APP, "auto")
        check_playback(s, "native playback", lambda g: nodes_of(g, node_name=APP))
        s.capture("rostrum.game", 440)
        native, output = record_file(s, "native-recording")
        s.spawn(["pw-cat", "-r", "-a", "--format", "f32", "--rate", str(RATE), "--channels", "1",
                 "-P", f'{{ node.name = "{APP}.mic" application.name = "{APP}" }}', "-"], output)
        check_recording(s, "native recording", lambda g: nodes_of(g, node_name=f"{APP}.mic"), native)
        s.stop(player)  # One 440 Hz source at a time, so the Game capture below has a known level.
        s.wait("native playback gone", lambda: not nodes_of(s.graph(), node_name=APP))

        # PulseAudio clients go through a private pipewire-pulse that reads pipewire-pulse.conf.d.
        shutil.copy(system_pulse, config / "pipewire-pulse.conf")
        server = s.spawn(["pipewire-pulse"])
        socket = s.root / "runtime/pulse/native"
        s.wait("private pulse socket", lambda: socket.is_socket())
        # pipewire-pulse is the pipewire binary run with its own config name.
        s.wait("private pipewire-pulse client", lambda: any(
            o["type"].endswith(":Client") and
            str(o["info"]["props"].get("application.process.id")) == str(server.pid) and
            o["info"]["props"].get("config.name") == "pipewire-pulse.conf" for o in s.graph()))
        s.env["PULSE_SERVER"] = f"unix:{socket}"
        tone = s.root / f"{APP}.raw"  # Written by s.tone() above.
        s.spawn(["pacat", "--playback", "--raw", "--format=float32le", f"--rate={RATE}", "--channels=2",
                 "--latency-msec=20", f"--property=application.name={APP}", "--stream-name=PulsePlayback",
                 str(tone)])
        check_playback(s, "pulse playback", lambda g: nodes_of(g, media_name="PulsePlayback"))
        s.capture("rostrum.game", 440)
        pulse, output = record_file(s, "pulse-recording")
        s.spawn(["pacat", "--record", "--raw", "--format=float32le", f"--rate={RATE}", "--channels=1",
                 "--latency-msec=20", f"--property=application.name={APP}", "--stream-name=PulseRecording"],
                output)
        check_recording(s, "pulse recording", lambda g: nodes_of(g, media_name="PulseRecording"), pulse)
        print("PASS rule fragments route native and pulse playback; the same app's recordings keep the mic",
              flush=True)
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
        main()
    except Failure as error:
        safety.report(f"FAIL: {error}")
        sys.exit(1)
