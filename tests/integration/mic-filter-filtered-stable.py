#!/usr/bin/env python3
"""rostrum.filtered keeps one identity and carries sound across every filter transition.

An app records rostrum.filtered by its object.serial, as a call app that picked it would. While it
records, the filters go processed -> off -> processed -> failed (the filter library cannot load) ->
recovered in one running engine. Every time the filtered mic must be the same PipeWire object
(same object.serial), must be fed by exactly one source (the processed path micfx -> filtered, or
the raw mic directly, never both at once and never neither once settled), the app's recording must
carry the 440 Hz test tone, and the recording must not have a dropout across the handoffs.

The serial and exclusive-feed checks (audit) run on every graph poll (about every 50 ms plus the
pw-dump time), including while each request is in flight and while settling. That is sampled
polling, not a guarantee about every instant; a feed overlap shorter than one poll could be missed.

Negative control: this test was not run against pre-fix code. The Task 3 mic-filter-release RED run
already showed the old code removing rostrum.filtered when filters went away, which audit()'s
serial check (and the removal/recreation log check) would also flag. A dual feed (micfx and raw mic
both linked into rostrum.filtered) fails audit() and settle()'s exclusive-source assertions.
"""
import argparse
import array
import importlib.util
import math
from pathlib import Path
import signal
import uuid

spec = importlib.util.spec_from_file_location("safety", Path(__file__).with_name("audio-safety.py"))
safety = importlib.util.module_from_spec(spec)
spec.loader.exec_module(safety)
Failure = safety.Failure
APP = "FilteredRecorder"
MIC = "test.saved-mic"
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
# Near-silence tolerated while a filter change swaps the feed, and where it may start. Measured
# handoffs cost about 80 ms; steady state costs nothing.
MAX_GAP_MS = 150
HANDOFF_WINDOW_MS = 400
GAP_RMS = 0.002


def links(graph, source, destination):
    """The links of any state from one named node to another."""
    a, b = safety.Session.node(graph, source), safety.Session.node(graph, destination)
    if not a or not b:
        return []
    return [o for o in graph if o["type"].endswith(":Link") and o["info"].get("output-node-id") == a["id"]
            and o["info"].get("input-node-id") == b["id"]]


def sources_of(graph, name):
    """Names of the nodes linked into a node."""
    node = safety.Session.node(graph, name)
    ids = {o["info"].get("output-node-id") for o in graph
           if node and o["type"].endswith(":Link") and o["info"].get("input-node-id") == node["id"]}
    return {o["info"]["props"].get("node.name") for o in graph if o["type"].endswith(":Node") and o["id"] in ids}


def serial_of(graph, name):
    node = safety.Session.node(graph, name)
    return str(node["info"]["props"]["object.serial"]) if node else None


class Stable:
    def __init__(self, s, log, path, serial):
        self.s, self.log, self.path, self.serial = s, log, path, serial
        self.request = 0
        self.marks = []
        self.control = s.root / "mic-filter-request"

    def audit(self, graph, label):
        """Checks that hold at every instant, polled whenever the graph is read."""
        if serial_of(graph, "rostrum.filtered") != self.serial:
            raise Failure(f"{label}: rostrum.filtered is not the original object "
                          f"(serial {serial_of(graph, 'rostrum.filtered')}, was {self.serial})")
        if links(graph, "rostrum.micfx", "rostrum.filtered") and links(graph, MIC, "rostrum.filtered"):
            raise Failure(f"{label}: the processed path and the raw mic both feed rostrum.filtered")

    def settle(self, label, processed):
        """Wait for one feed, and only that one, into rostrum.filtered."""
        want = {"rostrum.micfx"} if processed else {MIC}

        def settled():
            graph = self.s.graph()
            self.audit(graph, label)
            if not processed and safety.Session.node(graph, "rostrum.micfx"):
                return False
            ok = sources_of(graph, "rostrum.filtered") == want and sources_of(graph, APP) == {"rostrum.filtered"}
            if processed:
                ok = ok and safety.Session.linked(graph, "rostrum.micfx", "rostrum.filtered")
                ok = ok and not links(graph, MIC, "rostrum.filtered")
            else:
                ok = ok and safety.Session.linked(graph, MIC, "rostrum.filtered")
                ok = ok and not links(graph, "rostrum.micfx", "rostrum.filtered")
            return ok

        self.s.wait(f"{label}: filtered fed {'by micfx' if processed else 'by the raw mic'} only", settled)
        graph = self.s.graph()
        self.audit(graph, label)
        if processed and not safety.Session.linked(graph, MIC, "rostrum.micfx"):
            raise Failure(f"{label}: the raw mic is not feeding micfx")
        start = self.path.stat().st_size
        self.s.wait(f"{label}: {APP} records half a second more",
                    lambda: (self.audit(self.s.graph(), label), self.path.stat().st_size >= start + safety.RATE * 2)[1])
        self.s.verify(f"{label}: {APP}", self.path.read_bytes()[start:], 440, 1)
        print(f"  {label}: serial {self.serial} kept, fed {'through micfx' if processed else 'by the raw mic'}",
              flush=True)

    def send(self, what):
        self.request += 1
        self.marks.append((what, self.path.stat().st_size // 4 * 1000 // safety.RATE))
        temporary = self.control.with_suffix(".tmp")
        temporary.write_text(f"{self.request} {what}\n")
        temporary.replace(self.control)
        done = f"Mic filter request {self.request}: {what}\n"

        def applied():
            # The transition itself is audited, not only the settled states on either side: every
            # graph read from the request until the engine acknowledges it must keep the serial
            # and never show both feeds into rostrum.filtered.
            self.audit(self.s.graph(), f"during {what} (request {self.request})")
            return done in self.log.read_text()

        self.s.wait(f"engine applied mic filter request {what}", applied)


def gaps_ms(raw):
    """(start in ms, length in ms) of each stretch of near-silence, in 5 ms steps."""
    data = array.array("f")
    data.frombytes(raw[:len(raw) // 4 * 4])
    block = safety.RATE // 200  # 5 ms
    found, run = [], 0
    for n, i in enumerate(range(0, len(data) - block + 1, block)):
        rms = math.sqrt(sum(v * v for v in data[i:i + block]) / block)
        if rms < GAP_RMS:
            run += 1
        elif run:
            found.append(((n - run) * 5, run * 5))
            run = 0
    return found


def run(driver, plugin):
    s = safety.Session(driver)
    try:
        s.start()
        s.device(MIC, source=True)
        s.device("test.headphones")
        config = s.root / "config/rostrum"
        config.mkdir()
        settings = config / "settings.toml"
        settings.write_text(SETTINGS.format(enabled="true"))
        s.tone("CallFeed", "0", 440, 1, internal=True)
        s.command("pw-link", "CallFeed:output_MONO", f"{MIC}:input_MONO")
        s.route("CallFeed", MIC)

        stable = Stable(s, None, None, None)
        stable.control.write_text("")
        engine = s.spawn([str(driver), "--mic", MIC, "--headphones", "test.headphones",
                          "--mic-filter-plugin", str(plugin), "--mic-filter-file", str(stable.control)])
        log = next(log for child, log in s.processes if child is engine)
        s.route(MIC, "rostrum.micfx")
        s.route("rostrum.micfx", "rostrum.filtered")
        serial = serial_of(s.graph(), "rostrum.filtered")

        # A call app that records rostrum.filtered by its own target property.
        path = s.root / f"call-{uuid.uuid4().hex}.raw"
        output = path.open("wb")
        s.files.append(output)
        s.spawn(["pw-cat", "-r", "-a", "--format", "f32", "--rate", str(safety.RATE), "--channels", "1",
                 "--target", serial, "-P", f'{{ node.name = {APP} application.name = {APP} }}', "-"], output)
        s.wait(f"{APP} capture stream", lambda: s.node(s.graph(), APP))
        stable.log, stable.path, stable.serial = log, path, serial

        stable.settle("filters on", True)
        start_at = path.stat().st_size
        settings.write_text(SETTINGS.format(enabled="false"))
        stable.send("settings")
        stable.settle("filters off", False)
        settings.write_text(SETTINGS.format(enabled="true"))
        stable.send("settings")
        stable.settle("filters on again", True)
        stable.send("unavailable")
        stable.settle("filters failed", False)
        stable.send("available")
        stable.settle("filters recovered", True)

        text = log.read_text()
        if 'removing "rostrum.filtered"' in text or text.count('creating "rostrum.filtered"') > 1 \
                or text.count("creating rostrum.filtered") > 1:
            raise Failure("The engine removed or recreated rostrum.filtered")
        raw = path.read_bytes()
        # Link changes cost the recording a few quanta of silence at each handoff (PipeWire
        # activates the new link after the old one goes: exclusive feeds, never both). Anywhere
        # else, and any longer, is a dropout.
        base = start_at // 4 * 1000 // safety.RATE
        found = [(base + start, length) for start, length in gaps_ms(raw[start_at:])]
        requests = [when for _, when in stable.marks]
        print(f"  handoff silences (start ms, length ms): {found}; requests at {requests}", flush=True)
        for start, length in found:
            if length > MAX_GAP_MS:
                raise Failure(f"{APP} had a {length} ms dropout at {start} ms (limit {MAX_GAP_MS} ms)")
            if not any(0 <= start - when <= HANDOFF_WINDOW_MS for when in requests):
                raise Failure(f"{APP} had a dropout at {start} ms, away from any filter change")
        print("PASS rostrum.filtered kept its serial and carried sound across off, on, failed and recovered",
              flush=True)
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
