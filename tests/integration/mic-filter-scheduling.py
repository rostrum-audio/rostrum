#!/usr/bin/env python3
"""Real mic-filter delivery across client-driven quantum changes and legacy-node reuse.

With noise removal, rostrum.micfx forces a 512-sample cycle (raised to clock.min-quantum), so a
client asking for 128 samples cannot shrink the cycle below what RNNoise needs. Without it,
nothing is forced and the client gets its 128."""
import argparse
import importlib.util
from pathlib import Path
import re
import signal

spec = importlib.util.spec_from_file_location("safety", Path(__file__).with_name("audio-safety.py"))
safety = importlib.util.module_from_spec(spec)
spec.loader.exec_module(safety)


def driver_quantum(s, name):
    """The quantum pw-top reports for the driver whose group runs node `name`, or None."""
    blocks = re.split(r"^S\s+ID\s+QUANT.*$", s.command("pw-top", "-b", "-n", "2"), flags=re.M)
    quantum = None
    for line in blocks[-1].splitlines():
        fields = line.split()
        if len(fields) < 4 or fields[0] not in ("R", "I", "S", "C"):
            continue
        # Followers are marked "+", or "=" for streams; anything else starts a driver's group.
        if fields[-2] not in ("+", "="):
            quantum = int(fields[2]) if fields[2].isdigit() else None
        elif fields[-1] == name:
            return quantum
    return None


def setting(s, key):
    out = s.command("pw-metadata", "-n", "settings", "0", key)
    match = re.search(rf"key:'{re.escape(key)}' value:'([^']*)'", out)
    if not match:
        raise safety.Failure(f"settings metadata has no {key}: {out!r}")
    return match.group(1)


def force_quantum(s):
    value = s.node(s.graph(), "rostrum.micfx")["info"]["props"].get("node.force-quantum")
    return None if value is None else str(value)


def run(driver, plugin, denoise):
    s = safety.Session(driver)
    try:
        s.start()
        s.device("test.saved-mic", source=True)
        s.device("test.headphones")
        config = s.root / "config/rostrum"
        config.mkdir()
        (config / "settings.toml").write_text('''format = 1
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
''')
        args = [str(driver), "--mic", "test.saved-mic", "--headphones", "test.headphones",
                "--mic-filter-plugin", str(plugin)]
        if denoise:
            args.append("--mic-filter-denoise")
        engine = s.spawn(args)
        s.tone("SchedulingMic", "0", 440, 1, internal=True)
        s.command("pw-link", "SchedulingMic:output_MONO", "test.saved-mic:input_MONO")
        s.route("SchedulingMic", "test.saved-mic")

        def deliveries():
            s.route("test.saved-mic", "rostrum.micfx")
            s.route("rostrum.micfx", "rostrum.filtered")
            s.capture("test.saved-mic", 440, 1)
            s.capture("rostrum.filtered", 440, 1)
            s.capture("rostrum.mic", 440, 1)

        def latency_request(tag):
            # An ordinary capture client requests latency; the daemon chooses the
            # quantum. No clock.force-quantum, PIPEWIRE_QUANTUM or fixed bounds.
            audio = s.root / f"latency-request-{tag}.raw"
            output = audio.open("wb")
            s.files.append(output)
            target = s.node(s.graph(), "rostrum.filtered")["info"]["props"]["object.serial"]
            reader = s.spawn(["pw-cat", "-r", "-a", "--format", "f32", "--rate", "48000",
                "--channels", "1", "--target", str(target), "-P", '''{
                    node.name = MicLatencyRequest node.latency = 128/48000
                    rostrum.internal = true node.dont-fallback = true node.dont-move = true }''', "-"], output)
            s.route("rostrum.filtered", "MicLatencyRequest")
            s.wait("latency-request capture buffers", lambda: audio.stat().st_size >= 96000)
            return reader

        def end_request(reader):
            s.stop(reader)
            s.wait("latency-request client removed", lambda: s.node(s.graph(), "MicLatencyRequest") is None)

        for round in range(2):
            deliveries()
            reader = latency_request(round)
            if force_quantum(s) != ("512" if denoise else None):
                raise safety.Failure(f"rostrum.micfx node.force-quantum is {force_quantum(s)!r}")
            expected = 512 if denoise else 128
            s.wait(f"graph quantum {expected} with a 128-sample request",
                   lambda: driver_quantum(s, "rostrum.micfx") == expected)
            deliveries()
            end_request(reader)
            deliveries()
        if denoise:
            # The user's own floor wins when it is higher: the node is replaced to force it, and
            # put back when the floor drops again. The voice keeps flowing either way, and a
            # recorder that asks for 128 samples is still running.
            reader = latency_request("settings")
            floor = setting(s, "clock.min-quantum")
            for value, forced in (("1024", "1024"), (floor, "512")):
                serial = s.node(s.graph(), "rostrum.micfx")["info"]["props"]["object.serial"]
                s.command("pw-metadata", "-n", "settings", "0", "clock.min-quantum", value)
                s.wait(f"rostrum.micfx replaced to force {forced}",
                       lambda: (node := s.node(s.graph(), "rostrum.micfx")) and
                       node["info"]["props"]["object.serial"] != serial and force_quantum(s) == forced)
                deliveries()
                s.wait(f"graph quantum {forced}", lambda: driver_quantum(s, "rostrum.micfx") == int(forced))
            end_request(reader)
        # A persistent converter from an older build has no convert.direction.
        # Restarting the engine must upgrade it, not merely reload its LADSPA graph.
        s.stop(engine)
        s.remove("rostrum.micfx")
        s.command("pw-cli", "create-node", "spa-node-factory", '''{
            factory.name = audio.convert node.name = rostrum.micfx
            audio.channels = 1 audio.position = [ MONO ] object.linger = true
            node.virtual = true rostrum.role = micfx }''')
        legacy = s.wait("legacy persistent converter", lambda: s.node(s.graph(), "rostrum.micfx"))
        serial = legacy["info"]["props"]["object.serial"]
        engine = s.spawn(args)
        s.wait("legacy converter replaced", lambda: (node := s.node(s.graph(), "rostrum.micfx")) and
               node["info"]["props"]["object.serial"] != serial)
        if force_quantum(s) != ("512" if denoise else None):
            raise safety.Failure(f"replacement rostrum.micfx node.force-quantum is {force_quantum(s)!r}")
        deliveries()
        # A call app records rostrum.filtered, not the separately gained stream mic.
        # Global mic mute must silence both public sources without muting the hardware.
        s.stop(engine)
        engine = s.spawn(args + ["--mic-muted"])
        s.wait("muted stream mic", lambda: any(p.get("mute") is True
               for p in s.node(s.graph(), "rostrum.mic")["info"]["params"].get("Props", [])))
        s.route("rostrum.micfx", "rostrum.filtered")
        s.capture("test.saved-mic", 440, 1)
        s.capture("rostrum.filtered", None, 1)
        s.capture("rostrum.mic", None, 1)
        s.stop(engine)
        # A headphones-only destination silences the stream source, not call apps.
        engine = s.spawn(args + ["--dest", "mic=phones"])
        s.route("rostrum.micfx", "rostrum.filtered")
        s.capture("rostrum.filtered", 440, 1)
        s.capture("rostrum.mic", None, 1)
        s.stop(engine)
        engine = s.spawn(args)
        deliveries()
        print("PASS mic mute silences filtered call apps; unmute restores; hardware and destination preserved", flush=True)
        print("PASS filtered mic and stream mic delivery before/during/after latency requests; persistent converter upgrade", flush=True)
        if denoise:
            print("PASS noise removal holds the graph at 512 samples against a 128-sample request; "
                  "clock.min-quantum above it wins and the node follows it back", flush=True)
        else:
            print("PASS without noise removal nothing is forced; the 128-sample request is honoured", flush=True)
    except BaseException:
        safety.cleanup_preserving_failure(s.diagnostics)
        raise
    finally:
        safety.cleanup_preserving_failure(s.close)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path)
    parser.add_argument("plugin", type=Path)
    parser.add_argument("--denoise", action="store_true")
    args = parser.parse_args()
    for sig in safety.INTERRUPTS:
        signal.signal(sig, lambda signum, _: (_ for _ in ()).throw(safety.Failure(f"Interrupted by signal {signum}")))
    run(args.driver.resolve(strict=True), args.plugin.resolve(strict=True), args.denoise)
