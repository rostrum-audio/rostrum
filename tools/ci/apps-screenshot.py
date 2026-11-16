#!/usr/bin/env python3
"""Page screenshots with a private PipeWire session (integration fixture).

Uses tests/integration/audio-safety.Session — the same isolated HOME /
XDG_RUNTIME_DIR / PipeWire / WirePlumber / D-Bus setup as the audio suite.
Never connects to the user's session PipeWire socket or D-Bus, never calls
systemctl, and only writes under the session temp tree and reports/.

Any Rostrum page id (mixer, apps, scenes, …) can be shot via --page.

Example:
  tools/ci/apps-screenshot.py build/src/app/rostrum reports/step3a/after/scenes-1280-dark.png \\
      --page scenes --width 1280 --theme dark
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

SETTINGS_TABS = ("general", "audio", "hotkeys", "integrations", "application")
PAGES = ("mixer", "apps", "scenes", "devices", "filters", "obs", "settings")

# Realistic media-player names so Apps-row truncation is visible.
TONES = (
    ("Firefox Developer Edition", 440),
    ("Discord", 554),
    ("Spotify", 659),
    ("Chromium Web Browser — Very Long Window Title For Truncation", 880),
)

# Minimal extra scenes for the Scenes page (Live is created as the default).
SCENE_SEED = (
    ("streaming.toml", '''format = 1
name = "Streaming"

[master]
phones = 1.0
phones_muted = false
stream = 1.0
stream_muted = false

[mic]
sidetone = 1.0

[[bus]]
auto = "none"
color = "#da4453"
destination = "stream"
id = "mic"
kind = "input"
muted = false
name = "Mic"
volume = 0.8

[[bus]]
auto = "game"
color = "#27ae60"
destination = "stream"
id = "game"
kind = "playback"
muted = false
name = "Game"
volume = 1.0
'''),
    ("desktop-only.toml", '''format = 1
name = "Desktop only"

[master]
phones = 1.0
phones_muted = false
stream = 1.0
stream_muted = false

[mic]
sidetone = 1.0

[[bus]]
auto = "none"
color = "#da4453"
destination = "stream"
id = "mic"
kind = "input"
muted = false
name = "Mic"
volume = 0.8

[[bus]]
auto = "desktop"
color = "#f67400"
destination = "phones"
id = "desktop"
kind = "playback"
muted = false
name = "Desktop"
volume = 1.0
'''),
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


def write_settings(config_home: Path, width: int, height: int, page: str) -> Path:
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
page = "{page}"
''')
    return settings


def seed_scenes(config_home: Path) -> None:
    scenes = config_home / "rostrum" / "scenes"
    scenes.mkdir(parents=True, exist_ok=True)
    for name, body in SCENE_SEED:
        (scenes / name).write_text(body)


def seed_obs(config_home: Path, kind: str) -> None:
    """Fake OBS settings inside the private config home. Screenshot runs never look for a
    running OBS, so this reaches only the "OBS is closed" states and no real OBS."""
    obs = config_home / "obs-studio"
    (obs / "basic" / "scenes").mkdir(parents=True, exist_ok=True)
    (obs / "user.ini").write_text("[Basic]\nProfile=Test\nSceneCollectionFile=config\n")
    if kind == "closed":
        fixture = HERE / "tests/data/obs-collection.json"
        (obs / "basic" / "scenes" / "config.json").write_bytes(fixture.read_bytes())


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


def capture(application: Path, output: Path, width: int, height: int, theme: str, page: str,
            with_tones: bool, obs: str = "none", settings_tab: str = "") -> None:
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
        write_settings(config_home, width, height, page)
        if page == "scenes":
            seed_scenes(config_home)
        if obs != "none":
            seed_obs(config_home, obs)
        kde = write_kdeglobals(config_home, theme)
        schemes = Path(session.env["XDG_DATA_HOME"]) / "color-schemes"
        schemes.mkdir(parents=True, exist_ok=True)
        target = schemes / SCHEMES[theme].name
        target.write_bytes(SCHEMES[theme].read_bytes())

        session.env.update(
            QT_QUICK_CONTROLS_STYLE="org.kde.desktop",
            QT_QPA_PLATFORMTHEME="kde",
            ROSTRUM_SCREENSHOT=str(session.root / "shot.png"),
        )
        if settings_tab:
            session.env["ROSTRUM_SCREENSHOT_SETTINGS_TAB"] = settings_tab
        print_isolation(session.env, session.root)
        print(f"kdeglobals={kde}", flush=True)
        print(f"theme={theme} scheme={SCHEMES[theme]} page={page}", flush=True)

        if with_tones:
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
                          "width": width, "height": height, "theme": theme, "page": page}), flush=True)
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
    parser.add_argument("--page", choices=PAGES, default="apps",
                        help="Rostrum window.page id to open (default: apps)")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=800)
    parser.add_argument("--theme", choices=sorted(SCHEMES), required=True)
    parser.add_argument("--tones", choices=("auto", "always", "never"), default="auto",
                        help="Play named tone streams (auto: only for --page apps)")
    parser.add_argument("--obs", choices=("none", "closed", "unreadable"), default="none",
                        help="Seed fake OBS settings in the private session: closed = a scene "
                             "collection Rostrum can read, unreadable = settings without one")
    parser.add_argument("--settings-tab", choices=SETTINGS_TABS, default="",
                        help="With --page settings: the tab to open before the shot")
    args = parser.parse_args()
    with_tones = args.tones == "always" or (args.tones == "auto" and args.page == "apps")

    interrupted = None

    def on_signal(sig, _frame):
        nonlocal interrupted
        interrupted = sig
        raise KeyboardInterrupt(f"Interrupted by signal {sig}")

    old = {sig: signal.signal(sig, on_signal) for sig in safety.INTERRUPTS}
    code = 1
    try:
        capture(args.application, args.output, args.width, args.height, args.theme, args.page,
                with_tones, args.obs, args.settings_tab)
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
