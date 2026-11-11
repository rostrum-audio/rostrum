#!/usr/bin/env python3
"""Apps on the filtered mic go back to the mic before micfx is removed; filtered stays.

A call app records rostrum.filtered while the filters run, and WirePlumber remembers that target.
When Rostrum next starts with the filters turned off, or with the filter library unable to load,
it must clear the app's target and wait for WirePlumber to move it back to the mic before it
removes rostrum.micfx. rostrum.filtered always stays while the mix runs (it just carries the raw
mic), so the app never records a node that is going away.
"""
import argparse
import importlib.util
from pathlib import Path
import signal
import uuid

spec = importlib.util.spec_from_file_location("safety", Path(__file__).with_name("audio-safety.py"))
safety = importlib.util.module_from_spec(spec)
spec.loader.exec_module(safety)
Failure = safety.Failure
APP = "CallApp"
FILTER_NODES = ("rostrum.micfx", "rostrum.filtered")
SETTINGS = '''format = 1
[mic_filters]
enabled = {enabled}
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
'''


def target(graph, node):
    """The target.object WirePlumber holds for a stream in the default metadata, or None."""
    for o in graph:
        if not o["type"].endswith(":Metadata") or o.get("props", {}).get("metadata.name") != "default":
            continue
        for entry in o.get("metadata") or []:
            if entry.get("subject") == node["id"] and entry.get("key") == "target.object":
                return str(entry.get("value"))
    return None


def sources_of(graph, name):
    """Names of the nodes linked into a node."""
    node = safety.Session.node(graph, name)
    ids = {o["info"].get("output-node-id") for o in graph
           if node and o["type"].endswith(":Link") and o["info"].get("input-node-id") == node["id"]}
    return {o["info"]["props"].get("node.name") for o in graph if o["type"].endswith(":Node") and o["id"] in ids}


def recorded(s, path, frequency):
    """Check the newest half second the call app recorded."""
    start = path.stat().st_size
    s.wait(f"{APP} records half a second more", lambda: path.stat().st_size >= start + safety.RATE * 4 // 2)
    s.verify(APP, path.read_bytes()[start:], frequency, 1)


def filtering(s, args, path):
    """Rostrum runs the filters and moves the call app to the filtered mic."""
    engine = s.spawn(args)
    s.route("test.saved-mic", "rostrum.micfx")
    s.route("rostrum.micfx", "rostrum.filtered")
    s.route("rostrum.filtered", APP)
    graph = s.graph()
    serial = safety.Session.node(graph, "rostrum.filtered")["info"]["props"]["object.serial"]
    s.wait(f"{APP} targeted at the filtered mic",
           lambda: target(s.graph(), s.node(s.graph(), APP)) == str(serial))
    recorded(s, path, 440)
    return engine


def released(s, engine, label):
    """The call app is back on the mic, its target is cleared, micfx is gone and filtered stays."""
    log = next(log for child, log in s.processes if child is engine)
    s.wait(f"{label}: {APP} target cleared", lambda: target(s.graph(), s.node(s.graph(), APP)) is None)
    s.wait(f"{label}: micfx removed", lambda: not s.node(s.graph(), "rostrum.micfx"))
    s.wait(f"{label}: {APP} records only the mic", lambda: sources_of(s.graph(), APP) == {"test.saved-mic"})
    if not s.node(s.graph(), "rostrum.filtered"):
        raise Failure(f"{label}: rostrum.filtered was removed; it must stay while the mix runs")
    text = log.read_text()
    moved, removed = text.find(f"raw mic for \"{APP}\""), text.find("removing \"rostrum.micfx\"")
    if moved < 0 or removed < 0 or removed < moved:
        raise Failure(f"{label}: micfx was removed before {APP} was pointed back at the mic")
    if "removing \"rostrum.filtered\"" in text:
        raise Failure(f"{label}: the engine removed rostrum.filtered")
    if "apps still on them" in text:
        raise Failure(f"{label}: {APP} was still on the filtered mic when the release wait ran out")
    print(f"  {label}: {APP} pointed back at the mic before micfx was removed; filtered kept", flush=True)


def run(driver, plugin):
    s = safety.Session(driver)
    try:
        s.start()
        s.device("test.saved-mic", source=True)
        s.device("test.headphones")
        config = s.root / "config/rostrum"
        config.mkdir()
        settings = config / "settings.toml"
        settings.write_text(SETTINGS.format(enabled="true"))
        base = [str(driver), "--mic", "test.saved-mic", "--headphones", "test.headphones"]
        with_plugin = base + ["--mic-filter-plugin", str(plugin)]
        s.tone("CallFeed", "0", 440, 1, internal=True)
        s.command("pw-link", "CallFeed:output_MONO", "test.saved-mic:input_MONO")
        s.route("CallFeed", "test.saved-mic")

        # An ordinary call app asks for the mic by its own target property.
        mic = s.node(s.graph(), "test.saved-mic")["info"]["props"]["object.serial"]
        path = s.root / f"call-{uuid.uuid4().hex}.raw"
        output = path.open("wb")
        s.files.append(output)
        s.spawn(["pw-cat", "-r", "-a", "--format", "f32", "--rate", str(safety.RATE), "--channels", "1",
                 "--target", str(mic), "-P", f'{{ node.name = {APP} application.name = {APP} }}', "-"], output)
        s.wait(f"{APP} capture stream", lambda: s.node(s.graph(), APP))

        for label, enabled, args in (
                ("filter library failed to load", "true", base + ["--mic-filters-unavailable"]),
                ("filters turned off", "false", with_plugin)):
            settings.write_text(SETTINGS.format(enabled="true"))
            engine = filtering(s, with_plugin, path)
            s.stop(engine)
            # The filter nodes linger and WirePlumber still holds the app's target.
            graph = s.graph()
            if not all(s.node(graph, name) for name in FILTER_NODES) or target(graph, s.node(graph, APP)) is None:
                raise Failure("The filtered mic or its app target did not outlast Rostrum")
            settings.write_text(SETTINGS.format(enabled=enabled))
            engine = s.spawn(args)
            released(s, engine, label)
            recorded(s, path, 440)
            s.stop(engine)
        print("PASS apps leave the filtered mic before micfx is removed; filtered stays", flush=True)
    except BaseException:
        safety.cleanup_preserving_failure(s.diagnostics)
        raise
    finally:
        safety.cleanup_preserving_failure(s.close)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path)
    parser.add_argument("plugin", type=Path)
    args = parser.parse_args()
    for sig in safety.INTERRUPTS:
        signal.signal(sig, lambda signum, _: (_ for _ in ()).throw(Failure(f"Interrupted by signal {signum}")))
    try:
        run(args.driver.resolve(strict=True), args.plugin.resolve(strict=True))
    except Failure as error:
        safety.report(f"FAIL: {error}")
        raise SystemExit(1)
