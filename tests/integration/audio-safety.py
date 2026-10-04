#!/usr/bin/env python3
"""Real-engine safety checks; every scenario owns a private audio session."""

import argparse
import array
import json
import math
import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
import uuid

from process_lifecycle import Job, OwnershipError, report

RATE = 48000
TIMEOUT = 12
INTERRUPTS = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
SESSIONS = set()


class Failure(RuntimeError):
    pass


def cleanup_preserving_failure(cleanup):
    original = sys.exc_info()[1]
    try:
        cleanup()
    except BaseException as error:
        if original is None:
            raise
        report(f"Additional cleanup error (original failure preserved): {error}")


def close_sessions():
    errors = []
    for session in list(SESSIONS):
        try:
            session.close()
        except BaseException as error:
            errors.append(str(error))
    if errors:
        raise Failure("; ".join(errors))


class Session:
    def __init__(self, driver):
        previous = signal.pthread_sigmask(signal.SIG_BLOCK, INTERRUPTS)
        try:
            self.initialize(driver)
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, previous)

    def initialize(self, driver):
        self.driver = driver
        self.root = Path(tempfile.mkdtemp(prefix="rostrum-safety-", dir=os.environ.get("TMPDIR", "/tmp")))
        self.root_identity = (self.root.stat().st_dev, self.root.stat().st_ino)
        self.sequence = 0
        self.processes = []
        self.files = []
        try:
            self.prepare_environment()
            SESSIONS.add(self)
        except BaseException:
            cleanup_preserving_failure(self._close)
            raise

    def prepare_environment(self):
        self.token = uuid.uuid4().hex
        self.remote = self.root / "runtime" / "audio"
        self.env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LANG": "C.UTF-8"}
        for name, directory in (("HOME", "home"), ("XDG_RUNTIME_DIR", "runtime"),
                                ("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                                ("XDG_STATE_HOME", "state"), ("XDG_CACHE_HOME", "cache")):
            path = self.root / directory
            path.mkdir(mode=0o700)
            self.env[name] = str(path)
        self.env.update(PIPEWIRE_RUNTIME_DIR=str(self.root / "runtime"),
                        PIPEWIRE_REMOTE=str(self.remote),
                        PIPEWIRE_CONFIG_DIR=str(self.root / "config/pipewire"),
                        WIREPLUMBER_CONFIG_DIR=str(self.root / "config/wireplumber"),
                        WIREPLUMBER_DATA_DIR="/usr/share/wireplumber",
                        QT_QPA_PLATFORM="offscreen", TMPDIR=str(self.root))

    def spawn(self, args, output=None):
        self.sequence += 1
        log = self.root / f"{self.sequence:03d}-{Path(args[0]).name}.log"
        err = log.open("wb")
        self.files.append(err)
        previous = signal.pthread_sigmask(signal.SIG_BLOCK, INTERRUPTS)
        try:
            # Register before initialization: failed startup must remain cleanup-owned.
            p = Job.__new__(Job)
            self.processes.append((p, log))
            Job.__init__(p, args, self.env, output if output is not None else err, err,
                         self.root / f"job-{self.sequence}.json")
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, previous)
        return p

    def command(self, *args):
        path = self.root / f"command-{uuid.uuid4().hex}.txt"
        with path.open("wb") as output:
            p = self.spawn(list(args), output)
            try:
                code = p.wait(timeout=3)
                if code:
                    log = next(log for child, log in self.processes if child is p)
                    raise Failure(f"{' '.join(args)}: {log.read_text(errors='replace').strip()}")
                return path.read_text()
            finally:
                cleanup_preserving_failure(lambda: self.stop(p))

    def wait(self, description, predicate):
        deadline = time.monotonic() + TIMEOUT
        while time.monotonic() < deadline:
            for p, log in self.processes:
                if p.poll() is not None:
                    raise Failure(f"{description}: {log.name} exited ({p.returncode})")
            result = predicate()
            if result:
                return result
            time.sleep(0.05)  # Poll observed state, not an assumed startup delay.
        raise Failure(f"Timed out: {description}")

    def graph(self):
        self.check_socket()
        graph = json.loads(self.command("pw-dump"))
        core = next((o for o in graph if o["type"].endswith(":Core")), None)
        if not core or core["info"]["props"].get("rostrum.test.token") != self.token:
            raise Failure("Private PipeWire identity mismatch; refusing further commands")
        return graph

    def check_socket(self):
        info = self.remote.lstat()
        if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid():
            raise Failure("Private PipeWire socket is not an owned Unix socket")

    @staticmethod
    def node(graph, name):
        return next((o for o in graph if o["type"].endswith(":Node") and
                     o["info"]["props"].get("node.name") == name), None)

    @staticmethod
    def linked(graph, source, destination):
        a, b = Session.node(graph, source), Session.node(graph, destination)
        if not a or not b:
            return False
        return any(o["type"].endswith(":Link") and
                   o["info"].get("output-node-id") == a["id"] and
                   o["info"].get("input-node-id") == b["id"] and
                   o["info"].get("state") == "active" for o in graph)

    def route(self, source, destination):
        return self.wait(f"active route {source} -> {destination}",
                         lambda: self.linked(self.graph(), source, destination))

    def start(self):
        config = self.root / "config/pipewire"
        config.mkdir()
        common = '''context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    support.* = support/libspa-support
}
'''
        (config / "client.conf").write_text(common + f'''
context.properties = {{ remote.name = "{self.remote}" }}
context.modules = [
    {{ name = libpipewire-module-protocol-native }}
    {{ name = libpipewire-module-client-node }}
    {{ name = libpipewire-module-adapter }}
    {{ name = libpipewire-module-metadata }}
]
''')
        (config / "pipewire.conf").write_text(common + f'''
context.properties = {{
    core.daemon = true
    core.name = "{self.remote}"
    rostrum.test.token = "{self.token}"
    support.dbus = false
    default.clock.rate = 48000
    default.clock.allowed-rates = [ 48000 ]
    default.clock.quantum = 256
}}
context.modules = [
    {{ name = libpipewire-module-protocol-native }}
    {{ name = libpipewire-module-metadata }}
    {{ name = libpipewire-module-spa-node-factory }}
    {{ name = libpipewire-module-client-node }}
    {{ name = libpipewire-module-adapter }}
    {{ name = libpipewire-module-link-factory }}
    {{ name = libpipewire-module-access }}
]
context.objects = [
    {{ factory = spa-node-factory args = {{
        factory.name = support.node.driver
        node.name = Safety-Driver
        node.group = pipewire.dummy
        priority.driver = 200000
    }} }}
]
''')
        # No service directories: requests cannot activate the user's desktop services.
        bus = self.root / "bus.conf"
        bus.write_text(f'''<busconfig><type>session</type>
<listen>unix:path={self.root}/runtime/bus</listen><auth>EXTERNAL</auth>
<policy context="default"><allow send_destination="*"/><allow own="*"/>
<allow eavesdrop="true"/></policy></busconfig>''')
        address = f"unix:path={self.root}/runtime/bus"
        self.env.update(DBUS_SESSION_BUS_ADDRESS=address, DBUS_SYSTEM_BUS_ADDRESS=address)
        self.spawn(["dbus-daemon", "--nofork", f"--config-file={bus}"])
        self.wait("private D-Bus socket", lambda: (self.root / "runtime/bus").is_socket())
        self.spawn(["pipewire", "-c", str(config / "pipewire.conf")])
        self.wait("private PipeWire socket", lambda: self.remote.is_socket())
        self.graph()  # Authenticate the daemon before starting any policy/engine clients.
        wp = self.root / "config/wireplumber"
        wp.mkdir()
        base = Path("/usr/share/wireplumber/wireplumber.conf")
        if not base.is_file():
            raise Failure(f"Missing WirePlumber 0.5 configuration: {base}")
        profile = '''wireplumber.profiles = {
    rostrum-test = {
        inherits = [ policy ]
        metadata.default = required
        hardware.audio = disabled
        hardware.bluetooth = disabled
        hardware.video-capture = disabled
        support.logind = disabled
        support.portal-permissionstore = disabled
        support.reserve-device = disabled
    }
'''
        (wp / "wireplumber.conf").write_text(base.read_text().replace("wireplumber.profiles = {", profile, 1))
        manager = self.spawn(["wireplumber", "-p", "rostrum-test"])
        self.wait("private WirePlumber and default metadata", lambda: self.manager_ready(manager.pid))

    def manager_ready(self, pid):
        graph = self.graph()
        client = any(o["type"].endswith(":Client") and
                     str(o["info"]["props"].get("application.process.id")) == str(pid) and
                     o["info"]["props"].get("application.process.binary") == "wireplumber" for o in graph)
        # pw-dump omits metadata objects with no entries. Inspect the registry too.
        metadata = 'metadata.name = "default"' in self.command("pw-cli", "ls", "Metadata")
        return client and metadata

    def device(self, name, source=False, priority=1000):
        channels = "MONO" if source else "FL FR"
        media = "Audio/Source/Virtual" if source else "Audio/Sink"
        self.command("pw-cli", "create-node", "adapter", f'''{{
            factory.name = support.null-audio-sink node.name = "{name}"
            node.description = "{name}" media.class = {media}
            audio.position = [ {channels} ] object.linger = true
            monitor.channel-volumes = true priority.session = {priority}
        }}''')
        return self.wait(f"fake device {name}", lambda: self.node(self.graph(), name))

    def tone(self, name, target, frequency=440, channels=2, internal=False):
        path = self.root / f"{name}.raw"
        block = array.array("f", (0.1 * math.sin(2 * math.pi * frequency * i / RATE)
                                 for i in range(RATE) for _ in range(channels))).tobytes()
        with path.open("wb") as f:
            for _ in range(120):
                f.write(block)
        props = f'{{ node.name = "{name}" application.name = "{name}"'
        if internal:
            props += ' rostrum.internal = true node.dont-move = true node.dont-fallback = true'
        props += " }"
        p = self.spawn(["pw-cat", "-p", "-a", "--format", "f32", "--rate", str(RATE),
                        "--channels", str(channels), "--channel-map", "MONO" if channels == 1 else "FL,FR",
                        "--target", target, "-P", props, str(path)])
        self.wait(f"tone stream {name}", lambda: self.node(self.graph(), name))
        return p

    def engine(self, fallback=False, destination="both", bus="game", live_destinations=False):
        config = self.root / "config/rostrum"
        config.mkdir(exist_ok=True)
        (config / "settings.toml").write_text(f'''format = 1
[devices]
headphones = "test.headphones"
mic = "test.saved-mic"
mic_fallback = {str(fallback).lower()}
''')
        args = [str(self.driver), "--config", "--rule", f"name:SafetyPlayer={bus}",
                "--dest", f"{bus}={destination}"]
        if live_destinations:
            self.destination_request = 0
            self.destination_file = self.root / "destination-request"
            self.destination_file.write_text("")
            args += ["--destination-file", str(self.destination_file)]
        return self.spawn(args)

    def destination(self, engine, bus, destination):
        self.destination_request += 1
        request = f"{self.destination_request} {bus}={destination}"
        temporary = self.destination_file.with_suffix(".tmp")
        temporary.write_text(request + "\n")
        temporary.replace(self.destination_file)
        log = next(log for child, log in self.processes if child is engine)
        acknowledgement = f"Destination request {self.destination_request}: {bus}={destination}\n"
        self.wait(f"applied destination request {request}", lambda: acknowledgement in log.read_text())

    def capture(self, name, frequency=None, channels=2):
        graph = self.graph()
        node = self.node(graph, name)
        if not node:
            raise Failure(f"Missing capture target {name}")
        path = self.root / f"capture-{uuid.uuid4().hex}.raw"
        output = path.open("wb")
        self.files.append(output)
        props = '{ rostrum.internal = true node.dont-fallback = true node.dont-move = true'
        if node["info"]["props"].get("media.class") == "Audio/Sink":
            props += " stream.capture.sink = true"
        props += " }"
        p = self.spawn(["pw-cat", "-r", "-a", "--format", "f32", "--rate", str(RATE),
                        "--channels", str(channels), "--target",
                        str(node["info"]["props"]["object.serial"]), "-P", props, "-"], output)
        self.wait(f"real capture buffers from {name}", lambda: path.stat().st_size >= RATE * channels * 4 // 2)
        self.stop(p)
        data = array.array("f")
        data.frombytes(path.read_bytes()[:RATE * channels * 4 // 2])
        for channel in range(channels):
            samples = list(data)[channel::channels][-RATE * 3 // 10:]
            rms = math.sqrt(sum(v * v for v in samples) / len(samples))
            if not math.isfinite(rms):
                raise Failure(f"{name}: non-finite audio")
            if frequency is None:
                if rms > 0.00001:
                    raise Failure(f"{name}: leaked audio on channel {channel}, RMS={rms:.6f}")
            else:
                amplitude = 2 * abs(sum(v * complex(math.cos(2 * math.pi * frequency * i / RATE),
                                                    math.sin(2 * math.pi * frequency * i / RATE))
                                        for i, v in enumerate(samples))) / len(samples)
                if not 0.04 < rms < 0.1 or not 0.07 < amplitude < 0.13:
                    raise Failure(f"{name}: expected {frequency} Hz on channel {channel}, RMS={rms:.6f}, amplitude={amplitude:.6f}")
            print(f"  {name}[{channel}]: {'silent' if frequency is None else str(frequency) + ' Hz'}, RMS={rms:.6f}", flush=True)

    def remove(self, name):
        node = self.node(self.graph(), name)
        self.command("pw-cli", "destroy", str(node["id"]))
        self.wait(f"{name} removed", lambda: self.node(self.graph(), name) is None)

    def stop(self, p):
        p.stop()
        self.processes = [(child, log) for child, log in self.processes if child is not p]

    def close(self):
        # A second terminal signal must not interrupt resource reclamation.
        handlers = {sig: signal.signal(sig, signal.SIG_IGN)
                    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
        try:
            self._close()
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)

    def _close(self):
        errors = []
        for p, _ in reversed(self.processes[:]):
            try:
                self.stop(p)
            except (OSError, OwnershipError, subprocess.SubprocessError) as e:
                errors.append(str(e))
        for f in self.files:
            try:
                f.close()
            except OSError as e:
                errors.append(str(e))
        if errors:
            raise Failure("Cleanup failed; private resources retained: " + "; ".join(errors))
        if self.root.exists():
            info = self.root.lstat()
            if not stat.S_ISDIR(info.st_mode) or (info.st_dev, info.st_ino) != self.root_identity:
                raise Failure("Refusing to remove a replaced private directory")
            shutil.rmtree(self.root)
        SESSIONS.discard(self)

    def diagnostics(self):
        if self.remote.is_socket():
            try:
                for o in self.graph():
                    if o["type"].endswith((":Node", ":Link", ":Client", ":Metadata")):
                        info = o.get("info", {})
                        props = info.get("props", {})
                        print(json.dumps({"id": o["id"], "type": o["type"],
                                          "name": props.get("node.name", props.get("application.name")),
                                          "state": info.get("state"), "from": info.get("output-node-id"),
                                          "to": info.get("input-node-id")}), file=sys.stderr)
                        if o["type"].endswith(":Metadata"):
                            print(o.get("props"), file=sys.stderr)
                        if o["type"].endswith(":Client"):
                            print({k: props.get(k) for k in ("application.process.id", "application.process.binary")}, file=sys.stderr)
            except (Failure, OSError, subprocess.SubprocessError):
                pass
        for path in sorted(self.root.glob("*.log")):
            print(f"--- {path.name} ---\n{path.read_text(errors='replace')[-6000:]}", file=sys.stderr)


def startup(s):
    s.device("test.headphones")
    s.tone("SafetyPlayer", "rostrum.game")
    s.route("SafetyPlayer", "test.headphones")
    if s.node(s.graph(), "rostrum.game"):
        raise Failure("Startup precondition failed: Rostrum bus already exists")
    s.capture("test.headphones", 440)
    s.engine()
    s.route("SafetyPlayer", "rostrum.game")
    s.route("rostrum.game", "rostrum.stream")
    s.capture("rostrum.game", 440)
    s.capture("rostrum.stream", 440)


def destinations(s):
    s.device("test.headphones")
    engine = s.engine(destination="phones")
    s.wait("Game bus", lambda: s.node(s.graph(), "rostrum.game"))
    s.tone("SafetyPlayer", "rostrum.game")
    s.route("SafetyPlayer", "rostrum.game")
    s.route("rostrum.phones", "test.headphones")
    s.capture("test.headphones", 440)
    s.capture("rostrum.stream")
    s.stop(engine)
    engine = s.engine(destination="stream")
    s.route("rostrum.game", "rostrum.stream")
    s.wait("headphones send removed", lambda: not s.linked(s.graph(), "rostrum.game", "rostrum.phones"))
    s.capture("rostrum.stream", 440)
    s.capture("test.headphones")

    # Keep one engine and playback stream alive while changing Desktop's destination.
    # Restarting the engine clears its pending deletion bookkeeping and hid ID reuse.
    s.stop(engine)
    engine = s.engine(bus="desktop", live_destinations=True)
    s.route("SafetyPlayer", "rostrum.desktop")
    s.route("rostrum.desktop", "rostrum.stream")
    s.route("rostrum.desktop", "rostrum.phones")
    s.capture("rostrum.stream", 440)
    s.capture("test.headphones", 440)

    def absent(destination):
        graph = s.graph()
        source = s.node(graph, "rostrum.desktop")
        target = s.node(graph, destination)
        return source and target and not any(
            o["type"].endswith(":Link") and
            o["info"].get("output-node-id") == source["id"] and
            o["info"].get("input-node-id") == target["id"] for o in graph)

    for destination in ("phones", "stream", "both", "phones") * 2:
        print(f"  live Desktop -> {destination}", flush=True)
        s.destination(engine, "desktop", destination)
        if destination in ("phones", "both"):
            s.route("rostrum.desktop", "rostrum.phones")
        if destination in ("stream", "both"):
            s.route("rostrum.desktop", "rostrum.stream")
        if destination == "phones":
            s.wait("all Desktop Stream links removed", lambda: absent("rostrum.stream"))
            s.capture("test.headphones", 440)
            s.capture("rostrum.stream")
        elif destination == "stream":
            s.wait("all Desktop headphones links removed", lambda: absent("rostrum.phones"))
            s.capture("rostrum.stream", 440)
            s.capture("test.headphones")
        else:
            s.capture("rostrum.stream", 440)
            s.capture("test.headphones", 440)


def microphones(s):
    s.device("test.headphones")
    s.device("test.saved-mic", source=True)
    s.device("test.other-mic", source=True, priority=2000)
    # Feed fake sources by explicit links, never an automatically chosen target.
    s.tone("SavedFeed", "0", 330, 1, internal=True)
    s.tone("OtherFeed", "0", 660, 1, internal=True)
    s.command("pw-link", "SavedFeed:output_MONO", "test.saved-mic:input_MONO")
    s.command("pw-link", "OtherFeed:output_MONO", "test.other-mic:input_MONO")
    engine = s.engine()
    s.route("test.saved-mic", "rostrum.mic")
    s.capture("rostrum.mic", 330, 1)
    s.capture("test.other-mic", 660, 1)  # Prove that the alternative is live.
    s.remove("test.saved-mic")
    s.wait("missing saved mic has no input", lambda: mic_disconnected(s))
    s.capture("rostrum.mic", channels=1)
    s.device("test.saved-mic", source=True)
    s.command("pw-link", "SavedFeed:output_MONO", "test.saved-mic:input_MONO")
    s.route("test.saved-mic", "rostrum.mic")
    s.capture("rostrum.mic", 330, 1)
    s.stop(engine)
    s.engine(fallback=True)
    s.route("test.saved-mic", "rostrum.mic")
    s.remove("test.saved-mic")
    s.route("test.other-mic", "rostrum.mic")
    s.capture("rostrum.mic", 660, 1)
    s.device("test.saved-mic", source=True)
    s.command("pw-link", "SavedFeed:output_MONO", "test.saved-mic:input_MONO")
    s.route("test.saved-mic", "rostrum.mic")
    s.wait("alternative mic disconnected", lambda: not s.linked(s.graph(), "test.other-mic", "rostrum.mic"))
    s.capture("rostrum.mic", 330, 1)


def mic_disconnected(s):
    graph = s.graph()
    mic = s.node(graph, "rostrum.mic")
    return mic and not any(o["type"].endswith(":Link") and
                           o["info"].get("input-node-id") == mic["id"] for o in graph)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--scenario", choices=["all", "startup", "destinations", "microphones"], default="all")
    parser.add_argument("--runs", type=int, default=1)
    args = parser.parse_args()
    if args.runs < 1 or not args.driver.resolve().is_file():
        parser.error("positive --runs and a built --driver are required")
    for name in ("pipewire", "wireplumber", "pw-cat", "pw-cli", "pw-dump", "pw-link", "dbus-daemon"):
        if not shutil.which(name):
            parser.error(f"Missing required executable: {name} (suite cannot be skipped)")
    def interrupted(signum, _):
        raise Failure(f"Interrupted by signal {signum}")
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, interrupted)
    completed = 0
    try:
        for run in range(1, args.runs + 1):
            for name, scenario in (("startup", startup), ("destinations", destinations), ("microphones", microphones)):
                if args.scenario not in ("all", name):
                    continue
                print(f"Run {run}/{args.runs}: {name}", flush=True)
                previous = signal.pthread_sigmask(signal.SIG_BLOCK, INTERRUPTS)
                try:
                    s = Session(args.driver.resolve())
                except BaseException:
                    signal.pthread_sigmask(signal.SIG_SETMASK, previous)
                    raise
                try:
                    signal.pthread_sigmask(signal.SIG_SETMASK, previous)
                    s.start()
                    scenario(s)
                except BaseException:
                    cleanup_preserving_failure(s.diagnostics)
                    raise
                finally:
                    cleanup_preserving_failure(s.close)
                print(f"PASS {name}", flush=True)
            completed += 1
    except (Failure, OwnershipError, OSError, ValueError, subprocess.SubprocessError) as e:
        report(f"FAIL: {e}; completed clean runs: {completed}/{args.runs}")
        return 1
    print(f"PASS: {completed} clean run(s)", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
