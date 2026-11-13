#!/usr/bin/env python3
"""Apps-page screenshots with a private PipeWire session (integration fixture).

Uses tests/integration/audio-safety.Session — the same isolated HOME /
XDG_RUNTIME_DIR / PipeWire / WirePlumber / D-Bus setup as the audio suite.
Never connects to the user's session PipeWire socket or D-Bus, never calls
systemctl, and only writes under the session temp tree and reports/.

Example:
  tools/ci/apps-screenshot.py build/src/app/rostrum reports/step2/after \\
      --width 1280 --theme dark
"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import signal
import sys
import time

HERE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / "tests/integration"))
spec = importlib.util.spec_from_file_location("safety", HERE / "tests/integration/audio-safety.py")
safety = importlib.util.module_from_spec(spec)
spec.loader.exec_module(safety)

SCHEMES = {
    "dark": Path("/usr/share/color-schemes/BreezeDark.colors"),
    "light": Path("/usr/share/color-schemes/BreezeLight.colors"),
}

# Realistic media-player names so row truncation is visible at 1280 / 960.
TONES = (
    ("Firefox Developer Edition", 440),
    ("Discord", 554),
    ("Spotify", 659),
    ("Chromium Web Browser — Very Long Window Title For Truncation", 880),
)


def report_dir(path: Path) -> Path:
    reports = (HERE / "reports").resolve()
    resolved = path.resolve()
    if reports != resolved and reports not in resolved.parents:
        raise SystemExit(f"Output must be under {reports}, got {resolved}")
    return resolved


def write_kdeglobals(config_home: Path, theme: str) -> Path:
    scheme = SCHEMES[theme]
    if not scheme.is_file():
        raise safety.Failure(f"Missing color scheme file: {scheme}")
    # Kirigami reads [Colors:*] from kdeglobals; embed the scheme so we never
    # need plasma-apply-* or the live session.
    out = config_home / "kdeglobals"
    out.write_text(scheme.read_text() + f"\n[General]\nColorScheme={scheme.stem}\n")
    return out


def write_settings(config_home: Path, width: int, height: int) -> Path:
    rostrum = config_home / "rostrum"
    rostrum.mkdir(parents=True, exist_ok=True)
    settings = rostrum / "settings.toml"
    settings.write_text(f'''format = 1
[general]
wizard_done = true
setup_version = 3
auto_save_scenes = false
scene_fade_ms = 0
[devices]
headphones = "test.headphones"
mic = "test.saved-mic"
[apps]
auto_assign = true
[updates]
check = false
[privacy]
crash_reports = "never"
[window]
width = {width}
height = {height}
page = "apps"
''')
    return settings


def print_isolation(env: dict, root: Path) -> None:
    print("=== apps-screenshot isolation ===", flush=True)
    print(f"session_root={root}", flush=True)
    for key in sorted(env):
        print(f"{key}={env[key]}", flush=True)
    forbidden = []
    remote = Path(env["PIPEWIRE_REMOTE"])
    if not str(remote).startswith(str(root)):
        forbidden.append(f"PIPEWIRE_REMOTE outside session: {remote}")
    bus = env.get("DBUS_SESSION_BUS_ADDRESS", "")
    if "unix:path=" in bus and not bus.split("unix:path=", 1)[1].startswith(str(root)):
        forbidden.append(f"DBUS_SESSION_BUS_ADDRESS outside session: {bus}")
    for key in ("XDG_RUNTIME_DIR", "HOME", "XDG_CONFIG_HOME", "TMPDIR"):
        if not env[key].startswith(str(root)):
            forbidden.append(f"{key} outside session: {env[key]}")
    if env.get("PIPEWIRE_REMOTE", "").startswith("/run/user/"):
        forbidden.append("PIPEWIRE_REMOTE looks like the user session")
    if forbidden:
        raise safety.Failure("Isolation check failed: " + "; ".join(forbidden))
    print("isolation_ok=true", flush=True)


def capture(application: Path, output: Path, width: int, height: int, theme: str) -> None:
    application = application.resolve(strict=True)
    output = report_dir(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()

    session = safety.Session(application)
    app = None
    try:
        session.start()
        session.device("test.headphones")
        session.device("test.saved-mic", source=True)

        config_home = Path(session.env["XDG_CONFIG_HOME"])
        write_settings(config_home, width, height)
        kde = write_kdeglobals(config_home, theme)
        # Optional lookup path if something still resolves ColorScheme= by name.
        schemes = Path(session.env["XDG_DATA_HOME"]) / "color-schemes"
        schemes.mkdir(parents=True, exist_ok=True)
        target = schemes / SCHEMES[theme].name
        target.write_bytes(SCHEMES[theme].read_bytes())

        session.env.update(
            QT_QUICK_CONTROLS_STYLE="org.kde.desktop",
            QT_QPA_PLATFORMTHEME="kde",
            ROSTRUM_SCREENSHOT=str(session.root / "shot.png"),
        )
        # QT_QPA_PLATFORM=offscreen is already set by Session.prepare_environment.
        print_isolation(session.env, session.root)
        print(f"kdeglobals={kde}", flush=True)
        print(f"theme={theme} scheme={SCHEMES[theme]}", flush=True)

        for name, frequency in TONES:
            session.tone(name, "test.headphones", frequency=frequency)
        for name, _ in TONES:
            session.wait(f"tone still present {name}",
                         lambda n=name: session.node(session.graph(), n) is not None)

        shot = Path(session.env["ROSTRUM_SCREENSHOT"])
        app = session.spawn([str(application)])
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            if app.poll() is not None:
                break
            if shot.is_file() and shot.stat().st_size > 0:
                # App quits itself shortly after writing; give it a moment.
                time.sleep(0.2)
                if app.poll() is not None:
                    break
            time.sleep(0.05)
        else:
            raise safety.Failure("Timed out waiting for ROSTRUM_SCREENSHOT")

        if app.poll() is None:
            session.stop(app)
            app = None
        if not shot.is_file() or shot.stat().st_size == 0:
            raise safety.Failure(f"Missing or empty screenshot (app rc={getattr(app, 'returncode', None)})")
        output.write_bytes(shot.read_bytes())
        print(json.dumps({"ok": True, "output": str(output), "bytes": output.stat().st_size,
                          "width": width, "height": height, "theme": theme}), flush=True)
    except BaseException:
        safety.cleanup_preserving_failure(session.diagnostics)
        raise
    finally:
        handlers = {sig: signal.signal(sig, signal.SIG_IGN) for sig in safety.INTERRUPTS}
        try:
            if app is not None and app.poll() is None:
                try:
                    session.stop(app)
                except BaseException as error:
                    safety.report(f"Additional app stop error: {error}")
            session.close()
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("application", type=Path)
    parser.add_argument("output", type=Path, help="PNG path under reports/")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=800)
    parser.add_argument("--theme", choices=sorted(SCHEMES), required=True)
    args = parser.parse_args()

    interrupted = None

    def on_signal(sig, _frame):
        nonlocal interrupted
        interrupted = sig
        raise KeyboardInterrupt(f"Interrupted by signal {sig}")

    old = {sig: signal.signal(sig, on_signal) for sig in safety.INTERRUPTS}
    code = 1
    try:
        capture(args.application, args.output, args.width, args.height, args.theme)
        code = 0
    except KeyboardInterrupt as error:
        safety.report(str(error))
        code = 128 + (interrupted or signal.SIGINT)
    except BaseException as error:
        safety.report(f"apps-screenshot failed: {error}")
        code = 1
    finally:
        try:
            safety.close_sessions()
        except BaseException as error:
            safety.report(f"Additional session cleanup error: {error}")
            if code == 0:
                code = 1
        for sig, handler in old.items():
            signal.signal(sig, handler)
    return code


if __name__ == "__main__":
    sys.exit(main())
