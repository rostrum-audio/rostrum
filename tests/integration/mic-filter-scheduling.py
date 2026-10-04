#!/usr/bin/env python3
"""Real mic-filter delivery across client-driven quantum changes and legacy-node reuse."""
import argparse
import importlib.util
from pathlib import Path
import signal

spec = importlib.util.spec_from_file_location("safety", Path(__file__).with_name("audio-safety.py"))
safety = importlib.util.module_from_spec(spec)
spec.loader.exec_module(safety)


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

        for _ in range(2):
            deliveries()
            # An ordinary capture client requests latency; the daemon chooses the
            # quantum. No clock.force-quantum, PIPEWIRE_QUANTUM or fixed bounds.
            audio = s.root / "latency-request.raw"
            output = audio.open("wb")
            s.files.append(output)
            target = s.node(s.graph(), "rostrum.filtered")["info"]["props"]["object.serial"]
            reader = s.spawn(["pw-cat", "-r", "-a", "--format", "f32", "--rate", "48000",
                "--channels", "1", "--target", str(target), "-P", '''{
                    node.name = MicLatencyRequest node.latency = 128/48000
                    rostrum.internal = true node.dont-fallback = true node.dont-move = true }''', "-"], output)
            s.route("rostrum.filtered", "MicLatencyRequest")
            s.wait("latency-request capture buffers", lambda: audio.stat().st_size >= 96000)
            deliveries()
            s.stop(reader)
            s.wait("latency-request client removed", lambda: s.node(s.graph(), "MicLatencyRequest") is None)
            deliveries()
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
        deliveries()
        print("PASS filtered mic and stream mic delivery before/during/after latency requests; persistent converter upgrade", flush=True)
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
