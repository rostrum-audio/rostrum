#!/usr/bin/env python3
"""Exercise an AppImage using only the audio suite's private session.

Run in a runtime-only container; requires the tools in tests/integration/README.md.
No desktop audio socket or user home should be mounted into that container.
"""
import argparse
import array
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import shutil
import signal
import sys

HERE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / "tests/integration"))
spec = importlib.util.spec_from_file_location("safety", HERE / "tests/integration/audio-safety.py")
safety = importlib.util.module_from_spec(spec)
spec.loader.exec_module(safety)


def check_updates(s, image, settings):
    fixtures = s.root / "update-fixtures"
    fixtures.mkdir()
    payload = b"local updater fixture; never executed\n"
    (fixtures / "download.AppImage").write_bytes(payload)
    port = s.root / "http-port"
    server = s.spawn([sys.executable, "-c", '''
from http.server import HTTPServer, SimpleHTTPRequestHandler
from functools import partial
from pathlib import Path
import sys
server = HTTPServer(('127.0.0.1', 0), partial(SimpleHTTPRequestHandler, directory=sys.argv[1]))
Path(sys.argv[2]).write_text(str(server.server_port))
server.serve_forever()
''', str(fixtures), str(port)])
    s.wait("local update fixture listening", port.exists)
    url = f"http://127.0.0.1:{port.read_text()}"
    s.env["ROSTRUM_UPDATE_URL"] = url + "/latest.json"
    settings.write_text(settings.read_text().replace("check = false", "check = true") +
                        '\n# update fixtures only\n')
    installed = s.root / "update-test.AppImage"
    log = s.root / "state/rostrum/rostrum.log"
    for valid in (False, True):
        shutil.copyfile(image, installed)
        installed.chmod(0o755)
        original = hashlib.sha256(installed.read_bytes()).hexdigest()
        feed = {"version": "9.9.9", "url": url, "date": "2026-10-03",
                "appimage": {"x86_64": {"url": url + "/download.AppImage",
                    "size": len(payload),
                    "sha256": hashlib.sha256(payload).hexdigest() if valid else "0" * 64}}}
        (fixtures / "latest.json").write_text(json.dumps(feed))
        start = log.stat().st_size if log.exists() else 0
        app = s.spawn([str(installed), "--appimage-extract-and-run"])
        expected = "installed to" if valid else "does not match the release checksum"
        s.wait("update accepted" if valid else "bad checksum rejected",
               lambda: expected in log.read_text()[start:])
        if valid:
            if installed.read_bytes() != payload or installed.stat().st_mode & 0o111 != 0o111:
                raise safety.Failure("Verified update was not installed executable")
        elif hashlib.sha256(installed.read_bytes()).hexdigest() != original:
            raise safety.Failure("Bad-checksum download replaced the original image")
        if list(s.root.glob("*.part")) or list(s.root.glob(".*.part")):
            raise safety.Failure("Update left a partial download")
        s.stop(app)
    s.stop(server)
    print("PASS local update feed, checksum rejection, verified replacement and executable permissions (replacement fixture not launched)", flush=True)


def x11_smoke(s, image, reports):
    display = s.root / "x11-display"
    with display.open("wb") as output:
        server = s.spawn(["Xvfb", "-displayfd", "1", "-screen", "0", "1280x800x24", "-nolisten", "tcp"], output)
    s.wait("private X server", lambda: display.read_text().strip().isdigit())
    shot = s.root / "x11.png"
    s.env.update(DISPLAY=":" + display.read_text().strip(), QT_QPA_PLATFORM="xcb",
                 QT_QUICK_BACKEND="software", ROSTRUM_SCREENSHOT=str(shot))
    app = s.spawn([str(image), "--appimage-extract-and-run"])
    if app.wait(timeout=60) != 0 or not shot.is_file() or not shot.stat().st_size:
        raise safety.Failure("X11/xcb wizard failed to render")
    shutil.copyfile(shot, reports / "x11.png")
    s.stop(app)
    s.stop(server)
    for key in ("DISPLAY", "QT_QUICK_BACKEND", "ROSTRUM_SCREENSHOT"):
        del s.env[key]
    s.env["QT_QPA_PLATFORM"] = "offscreen"
    print("PASS X11/xcb wizard render with private Xvfb (software rendering)", flush=True)


def routing_scenes(config):
    """Identical private scenes except for Desktop's destination; no app test hook."""
    scenes = config / "scenes"
    scenes.mkdir(exist_ok=True)
    for name, destination in (("Live", "both"), ("Headphones Only", "phones"),
                              ("Stream Only", "stream")):
        text = f'name = "{name}"\n'
        for bus in ("mic", "game", "voice", "music", "alerts", "desktop"):
            target = "stream" if bus == "mic" else destination if bus == "desktop" else "both"
            kind = "input" if bus == "mic" else "playback"
            text += f'''\n[[bus]]
id = "{bus}"
name = "{bus.title()}"
kind = "{kind}"
destination = "{target}"
volume = 1.0
muted = false
'''
        text += '''\n[[rule]]
key = "name"
match = "PackagedBrowser"
bus = "desktop"
volume = 1.0
'''
        (scenes / (name.lower().replace(" ", "-") + ".toml")).write_text(text)


def check_live_destinations(s, image, app):
    player = s.tone("PackagedBrowser", "rostrum.desktop")
    s.route("PackagedBrowser", "rostrum.desktop")
    serial = s.node(s.graph(), "PackagedBrowser")["info"]["props"]["object.serial"]

    def check_identity():
        source = s.node(s.graph(), "PackagedBrowser")
        if app.poll() is not None or player.poll() is not None or not source or \
                source["info"]["props"]["object.serial"] != serial:
            raise safety.Failure("Packaged application or playback source changed during live routing")

    def no_links(destination):
        graph = s.graph()
        source, target = s.node(graph, "rostrum.desktop"), s.node(graph, destination)
        return source and target and not any(
            o["type"].endswith(":Link") and
            o["info"].get("output-node-id") == source["id"] and
            o["info"].get("input-node-id") == target["id"] for o in graph)

    s.route("rostrum.desktop", "rostrum.phones")
    s.route("rostrum.desktop", "rostrum.stream")
    s.capture("test.headphones", 440)
    s.capture("rostrum.stream", 440)
    # Repeating this small sequence exercises registry ID reuse after capture/link churn.
    for scene in ("Headphones Only", "Live", "Stream Only", "Live", "Headphones Only", "Live") * 2:
        control = s.spawn([str(image), "--appimage-extract-and-run", "--scene", scene])
        try:
            if control.wait(timeout=15) != 0:
                raise safety.Failure(f"Packaged CLI could not switch to {scene}")
        finally:
            safety.cleanup_preserving_failure(lambda: s.stop(control))
        check_identity()
        s.route("PackagedBrowser", "rostrum.desktop")
        if scene != "Stream Only":
            s.route("rostrum.desktop", "rostrum.phones")
        if scene == "Headphones Only":
            s.wait("packaged Desktop Stream links removed", lambda: no_links("rostrum.stream"))
        else:
            s.route("rostrum.desktop", "rostrum.stream")
        if scene == "Stream Only":
            s.wait("packaged Desktop headphone links removed", lambda: no_links("rostrum.phones"))
        # Positive source delivery is required before silence can pass.
        if scene == "Stream Only":
            s.capture("rostrum.stream", 440)
            s.capture("test.headphones")
        else:
            s.capture("test.headphones", 440)
            s.capture("rostrum.stream", None if scene == "Headphones Only" else 440)
        check_identity()
        print(f"PASS live packaged Desktop -> {scene}: both stereo channels verified", flush=True)
    s.stop(player)


def run(image, reports, x11):
    s = safety.Session(image)
    try:
        s.start()
        s.device("test.headphones")
        s.device("test.saved-mic", source=True)
        config = s.root / "config/rostrum"
        config.mkdir(exist_ok=True)
        settings = config / "settings.toml"
        # Keep first-start UI intact, but never contact the public update feed or
        # opt into crash uploads, including in CI containers that have a network.
        settings.write_text('''format = 1
[updates]
check = false
[privacy]
crash_reports = "never"
''')
        shot = s.root / "wizard.png"
        s.env["ROSTRUM_SCREENSHOT"] = str(shot)
        app = s.spawn([str(image), "--appimage-extract-and-run"])
        if app.wait(timeout=60) != 0 or not shot.is_file() or shot.stat().st_size == 0:
            raise safety.Failure("First-start wizard failed to render")
        s.stop(app)
        shutil.copyfile(shot, reports / "wizard.png")
        print("PASS first-start wizard render (not interactive setup completion)", flush=True)
        del s.env["ROSTRUM_SCREENSHOT"]
        if x11:
            x11_smoke(s, image, reports)
        settings.write_text('''format = 1
[general]
wizard_done = true
setup_version = 3
auto_save_scenes = false
scene_fade_ms = 0
[devices]
headphones = "test.headphones"
mic = "test.saved-mic"
[updates]
check = false
[privacy]
crash_reports = "never"
[mic_filters]
enabled = true
[mic_filters.highpass]
enabled = true
frequency = 80
steep = true
[mic_filters.denoise]
enabled = false
[mic_filters.eq]
enabled = false
[mic_filters.compressor]
enabled = false
[mic_filters.limiter]
enabled = false
''')
        routing_scenes(config)
        app = s.spawn([str(image), "--appimage-extract-and-run"])
        s.wait("packaged mix", lambda: all(s.node(s.graph(), name) for name in
               ("rostrum.game", "rostrum.stream", "rostrum.phones", "rostrum.filtered")))
        s.route("rostrum.phones", "test.headphones")
        s.route("test.saved-mic", "rostrum.micfx")
        s.route("rostrum.micfx", "rostrum.filtered")
        check_live_destinations(s, image, app)
        tone = s.tone("PackagedMic", "0", 440, 1, internal=True)
        s.command("pw-link", "PackagedMic:output_MONO", "test.saved-mic:input_MONO")
        s.route("PackagedMic", "test.saved-mic")
        s.capture("test.saved-mic", 440, 1)
        s.capture("rostrum.filtered", 440, 1)
        s.stop(tone)
        s.tone("PackagedRumble", "0", 20, 1, internal=True)
        s.command("pw-link", "PackagedRumble:output_MONO", "test.saved-mic:input_MONO")
        s.route("PackagedRumble", "test.saved-mic")
        s.capture("test.saved-mic", 20, 1)
        target = s.node(s.graph(), "rostrum.filtered")["info"]["props"]["object.serial"]
        audio = s.root / "rumble.raw"
        with audio.open("wb") as output:
            recorder = s.spawn(["pw-cat", "-r", "-a", "--format", "f32", "--rate", "48000",
                               "--channels", "1", "--target", str(target), "-P",
                               "{ rostrum.internal = true node.dont-fallback = true }", "-"], output)
            s.wait("filtered rumble buffers", lambda: audio.stat().st_size >= 96000)
            s.route("test.saved-mic", "rostrum.micfx")
            s.route("rostrum.micfx", "rostrum.filtered")
            s.stop(recorder)
        samples = array.array("f")
        samples.frombytes(audio.read_bytes()[:96000])
        rms = math.sqrt(sum(v*v for v in samples[-14400:]) / 14400)
        amplitude = 2 * abs(sum(v * complex(math.cos(2 * math.pi * 20 * i / 48000),
                                            math.sin(2 * math.pi * 20 * i / 48000))
                                for i, v in enumerate(samples[-14400:]))) / 14400
        if not math.isfinite(rms) or not 0.0001 < rms < 0.001 or not 0.00015 < amplitude < 0.001:
            raise safety.Failure(f"Rumble filter did not attenuate 20 Hz: RMS={rms}")
        copies = list((s.root / "data/rostrum/dsp").glob("*/librostrum-dsp.so"))
        if len(copies) != 1:
            raise safety.Failure("Expected one stable DSP copy outside AppImage")
        daemons = [p for p, log in s.processes if log.name.endswith("-pipewire.log")]
        if len(daemons) != 1 or str(copies[0]) not in Path(f"/proc/{daemons[0].pid}/maps").read_text():
            raise safety.Failure("Host PipeWire did not map the stable DSP copy")
        print(f"PASS packaged mix, stable host DSP load, 440 Hz delivery and 20 Hz attenuation (RMS={rms:.6f})", flush=True)
        # Restart with RNNoise enabled: instantiate it in the real daemon. A sine
        # wave is not speech; this verifies loading, not voice quality or suppression.
        s.stop(app)
        settings.write_text(settings.read_text().replace('[mic_filters.denoise]\nenabled = false',
                                                        '[mic_filters.denoise]\nenabled = true'))
        app = s.spawn([str(image), "--appimage-extract-and-run"])
        s.route("test.saved-mic", "rostrum.micfx")
        s.route("rostrum.micfx", "rostrum.filtered")
        s.wait("RNNoise graph rebuilt", lambda: 'rostrum_denoise' in str(s.graph()))
        print("PASS bundled RNNoise chain loaded (voice quality not evaluated)", flush=True)
        s.stop(app)
        check_updates(s, image, settings)
        logs = "\n".join(p.read_text(errors="replace") for p in s.root.glob("*-*.AppImage.log"))
        logs += (s.root / "state/rostrum/rostrum.log").read_text(errors="replace")
        (reports / "application.log").write_text(logs)
        for message in ("failed to load component", "is not installed", "is unavailable", "Cannot load library"):
            if message.lower() in logs.lower():
                raise safety.Failure(f"Packaged resource error: {message}")
    except BaseException:
        # Preserve only this private session's diagnostics before its cleanup.
        safety.cleanup_preserving_failure(lambda: (reports / "application.log").write_text(
            "\n".join(p.read_text(errors="replace") for p in s.root.glob("*.log")) +
            ((s.root / "state/rostrum/rostrum.log").read_text(errors="replace")
             if (s.root / "state/rostrum/rostrum.log").exists() else "")))
        safety.cleanup_preserving_failure(s.diagnostics)
        raise
    finally:
        safety.cleanup_preserving_failure(s.close)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("appimage", type=Path)
    parser.add_argument("--reports", required=True, type=Path)
    parser.add_argument("--x11", action="store_true", help="also require private Xvfb/xcb launch")
    args = parser.parse_args()
    def interrupted(signum, _):
        raise safety.Failure(f"Interrupted by signal {signum}")
    for sig in safety.INTERRUPTS:
        signal.signal(sig, interrupted)
    args.reports.mkdir(parents=True, exist_ok=True)
    try:
        run(args.appimage.resolve(strict=True), args.reports, args.x11)
    finally:
        safety.cleanup_preserving_failure(safety.close_sessions)


if __name__ == "__main__":
    main()
