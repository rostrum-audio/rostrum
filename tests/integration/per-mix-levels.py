#!/usr/bin/env python3
"""Per-mix levels: Headphones and Stream sends in a private PipeWire session."""
import importlib.util
import json
import math
from pathlib import Path
import signal
import sys
import time
import uuid

spec = importlib.util.spec_from_file_location("safety", Path(__file__).with_name("audio-safety.py"))
safety = importlib.util.module_from_spec(spec)
spec.loader.exec_module(safety)

RATE = safety.RATE
# 20 * log10(1.0 / 0.5³) = 20 * log10(8) ≈ 18.0618
LEVEL_RATIO_DB = 20 * math.log10(1.0 / (0.5 ** 3))
RATIO_TOLERANCE_DB = 0.5
CYCLES = 300
# 1% of a 512-sample cycle at 48 kHz is about 107 µs. The private daemon has no
# realtime priority; allow headroom so CI load does not flake the bar.
SEND_BUSY_MEDIAN_US = 200  # per-cycle sum of all send nodes


def mean_rms(values):
    return sum(values) / len(values)


def ratio_db(phones, stream):
    p, s = mean_rms(phones), mean_rms(stream)
    if s <= 0:
        raise safety.Failure(f"stream RMS is {s}")
    return 20 * math.log10(p / s)


def node_id(graph, name):
    node = safety.Session.node(graph, name)
    if not node:
        raise safety.Failure(f"Missing {name}")
    return node["id"]


def media_names(graph):
    names = set()
    for o in graph:
        if not o["type"].endswith(":Node"):
            continue
        props = o["info"]["props"]
        media = props.get("media.class", "")
        if media.startswith("Audio/Sink") or media.startswith("Audio/Source"):
            names.add(props.get("node.name"))
    return names


def is_send_name(name):
    return name.startswith("rostrum.") and (name.endswith(".phones") or name.endswith(".stream")) \
        and name.count(".") >= 2 and name not in ("rostrum.phones", "rostrum.stream")


def dump_sends(graph):
    """Each send's ports and links from pw-dump, so a CI failure shows what the sends had."""
    sends = {o["id"]: o["info"]["props"].get("node.name") for o in graph
             if o["type"].endswith(":Node") and is_send_name(o["info"]["props"].get("node.name", ""))}
    print(f"Send nodes: {len(sends)}", file=sys.stderr)
    for node, name in sorted(sends.items()):
        ports = [o for o in graph if o["type"].endswith(":Port")
                 and str(o["info"]["props"].get("node.id")) == str(node)]
        print(f"  {node} {name}: {len(ports)} ports", file=sys.stderr)
        for o in sorted(ports, key=lambda o: o["id"]):
            props = o["info"]["props"]
            print(f"    port {o['id']} {props.get('port.name')} {props.get('port.direction')} "
                  f"channel={props.get('audio.channel')}", file=sys.stderr)
        for o in graph:
            info = o.get("info", {})
            if o["type"].endswith(":Link") and node in (info.get("output-node-id"), info.get("input-node-id")):
                print(f"    link {o['id']} {info.get('output-node-id')}:{info.get('output-port-id')} -> "
                      f"{info.get('input-node-id')}:{info.get('input-port-id')} state={info.get('state')} "
                      f"error={info.get('error')}", file=sys.stderr)


def wait_ready(s, engine):
    log = next(log for child, log in s.processes if child is engine)
    s.wait("Mix ready", lambda: "Mix ready." in log.read_text())


def start_engine(s, *extra, env=None):
    if env:
        s.env.update(env)
    # Pin the tone: without a rule, automatic placement puts SafetyPlayer on Music.
    args = [str(s.driver), "--headphones", "test.headphones",
            "--rule", "name:SafetyPlayer=game", "--dest", "game=both", *extra]
    engine = s.spawn(args)
    wait_ready(s, engine)
    return engine


def levels_differ(s):
    engine = start_engine(s, "--volume", "game.phones=1.0", "--volume", "game.stream=0.5")
    s.tone("SafetyPlayer", "rostrum.game")
    s.route("SafetyPlayer", "rostrum.game")
    s.mix_route("rostrum.game", "rostrum.phones")
    s.mix_route("rostrum.game", "rostrum.stream")
    phones, stream = s.rms("rostrum.phones"), s.rms("rostrum.stream")
    got = ratio_db(phones, stream)
    print(f"  levels_differ: phones/stream = {got:.2f} dB (want {LEVEL_RATIO_DB:.2f} ± {RATIO_TOLERANCE_DB})",
          flush=True)
    if abs(got - LEVEL_RATIO_DB) > RATIO_TOLERANCE_DB:
        raise safety.Failure(f"level ratio {got:.2f} dB outside {LEVEL_RATIO_DB:.2f} ± {RATIO_TOLERANCE_DB}")
    print("PASS levels_differ", flush=True)
    return engine


def levels_persist_after_exit(s):
    engine = start_engine(s, "--volume", "game.phones=1.0", "--volume", "game.stream=0.5")
    s.tone("SafetyPlayer", "rostrum.game")
    s.route("SafetyPlayer", "rostrum.game")
    s.mix_route("rostrum.game", "rostrum.stream")
    s.stop(engine)  # no --teardown: sends and levels linger
    phones, stream = s.rms("rostrum.phones"), s.rms("rostrum.stream")
    got = ratio_db(phones, stream)
    print(f"  levels_persist_after_exit: {got:.2f} dB", flush=True)
    if abs(got - LEVEL_RATIO_DB) > RATIO_TOLERANCE_DB:
        raise safety.Failure(f"after exit, level ratio {got:.2f} dB outside tolerance")
    print("PASS levels_persist_after_exit", flush=True)


def restart_keeps_sends(s):
    engine = start_engine(s, "--volume", "game.stream=1.0")
    s.tone("SafetyPlayer", "rostrum.game")
    s.route("SafetyPlayer", "rostrum.game")
    s.mix_route("rostrum.game", "rostrum.stream")
    before = {name: node_id(s.graph(), name)
              for name in ("rostrum.game.phones", "rostrum.game.stream")}
    # Continuous capture across the restart.
    path = s.root / f"stream-cont-{uuid.uuid4().hex}.raw"
    output = path.open("wb")
    s.files.append(output)
    stream = s.node(s.graph(), "rostrum.stream")
    recorder = s.spawn(
        ["pw-cat", "-r", "-a", "--format", "f32", "--rate", str(RATE), "--channels", "2",
         "--target", str(stream["info"]["props"]["object.serial"]),
         "-P", "{ rostrum.internal = true node.dont-fallback = true node.dont-move = true "
               "stream.capture.sink = true }", "-"],
        output)
    s.wait("continuous capture started", lambda: path.stat().st_size >= RATE * 2 * 4 // 10)
    mark = path.stat().st_size
    s.stop(engine)
    engine = start_engine(s)
    s.wait("continuous capture after restart",
           lambda: path.stat().st_size >= mark + RATE * 2 * 4 // 2)
    time.sleep(0.3)
    s.stop(recorder)
    after = {name: node_id(s.graph(), name)
             for name in ("rostrum.game.phones", "rostrum.game.stream")}
    if after != before:
        raise safety.Failure(f"send ids changed across restart: {before} -> {after}")
    # No 20 ms window below −40 dBFS while the tone plays (skip the first 100 ms of the file).
    import array
    data = array.array("f")
    data.frombytes(path.read_bytes())
    window = RATE * 2 * 20 // 1000  # 20 ms of interleaved stereo samples
    skip = RATE * 2 // 10
    floor = 10 ** (-40 / 20)
    for start in range(skip, len(data) - window, window):
        chunk = data[start:start + window]
        rms = math.sqrt(sum(v * v for v in chunk) / len(chunk))
        if rms < floor:
            raise safety.Failure(f"stream went below −40 dBFS for 20 ms (RMS={rms:.6g})")
    print("PASS restart_keeps_sends", flush=True)


def upgrade_from_direct_links(s):
    s.env["ROSTRUM_TEST_SEND_PORTS_FAIL"] = "1"
    engine = start_engine(s, "--volume", "game=1.0", "--dest", "game=stream")
    s.tone("SafetyPlayer", "rostrum.game")
    s.route("SafetyPlayer", "rostrum.game")
    # Wait for the 4 s send-port fallback: only then does the bus link straight into the stream.
    s.wait("fallback before upgrade baseline",
           lambda: not any(is_send_name(o["info"]["props"].get("node.name", ""))
                           for o in s.graph() if o["type"].endswith(":Node")))
    s.route("rostrum.game", "rostrum.stream")  # single-level direct link
    baseline = mean_rms(s.rms("rostrum.stream"))
    s.stop(engine)
    # A killed FAIL run can leave linger sends with no ports. The next start would adopt
    # them, call setPortConfig once, and fall back again when ports never arrive.
    for o in list(s.graph()):
        if not o["type"].endswith(":Node"):
            continue
        name = o["info"]["props"].get("node.name", "")
        if is_send_name(name):
            s.command("pw-cli", "destroy", str(o["id"]))
    s.wait("linger sends cleared before upgrade",
           lambda: not any(is_send_name(o["info"]["props"].get("node.name", ""))
                           for o in s.graph() if o["type"].endswith(":Node")))
    s.env["ROSTRUM_TEST_SEND_PORTS_FAIL"] = "0"  # explicit: a leftover "1" would keep ports from arriving
    engine = start_engine(s, "--volume", "game=1.0", "--dest", "game=stream")
    # Adopted direct links are removed before the send path is added; wait for the settled path.
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if s.mix_linked(s.graph(), "rostrum.game", "rostrum.stream"):
            break
        time.sleep(0.05)
    else:
        g = s.graph()
        dump_sends(g)
        raise safety.Failure(
            "Timed out: active mix route after upgrade from direct links "
            f"(bus→send={s.linked(g, 'rostrum.game', 'rostrum.game.stream')}, "
            f"send→stream={s.linked(g, 'rostrum.game.stream', 'rostrum.stream')}, "
            f"direct={s.linked(g, 'rostrum.game', 'rostrum.stream')})")
    if s.linked(s.graph(), "rostrum.game", "rostrum.stream"):
        raise safety.Failure("direct game→stream link still present after upgrade")
    got = mean_rms(s.rms("rostrum.stream"))
    # +6 dB would be about 2× linear. Stay within 0.5 dB of the single path.
    db = 20 * math.log10(got / baseline) if baseline > 0 else 99
    print(f"  upgrade_from_direct_links: stream RMS {baseline:.5f} → {got:.5f} ({db:+.2f} dB)", flush=True)
    if abs(db) > 0.5:
        raise safety.Failure(f"upgrade changed stream level by {db:.2f} dB (want ±0.5, not +6)")
    print("PASS upgrade_from_direct_links", flush=True)
    return engine


def no_new_devices(s):
    engine = start_engine(s)
    names = media_names(s.graph())
    expected = {
        "rostrum.game", "rostrum.voice", "rostrum.music", "rostrum.alerts", "rostrum.desktop",
        "rostrum.phones", "rostrum.stream", "rostrum.vod", "rostrum.mic", "rostrum.sidetone",
        "rostrum.filtered", "test.headphones",
    }
    # Sends have no media.class, so they must not appear.
    unexpected = {n for n in names if n and n.startswith("rostrum.") and n not in expected}
    if unexpected:
        raise safety.Failure(f"unexpected Audio/Sink or Source nodes: {sorted(unexpected)}")
    for name in expected:
        if name not in names and name != "test.headphones":
            # filtered may be suspended but still present
            if safety.Session.node(s.graph(), name) is None:
                raise safety.Failure(f"missing expected node {name}")
    send = safety.Session.node(s.graph(), "rostrum.game.phones")
    if not send:
        raise safety.Failure("send node missing")
    if send["info"]["props"].get("media.class"):
        raise safety.Failure("send node has a media.class and would show in device lists")
    print("PASS no_new_devices", flush=True)
    return engine


def solo_headphones_only(s):
    engine = start_engine(s, "--solo", "music", "--dest", "game=both")
    s.tone("SafetyPlayer", "rostrum.game")
    s.route("SafetyPlayer", "rostrum.game")
    s.mix_route("rostrum.game", "rostrum.phones")
    s.mix_route("rostrum.game", "rostrum.stream")
    s.capture("rostrum.phones")  # silent: Game dimmed by Music solo
    s.capture("rostrum.stream", 440)
    print("PASS solo_headphones_only", flush=True)
    return engine


def fallback(s):
    s.env["ROSTRUM_TEST_SEND_PORTS_FAIL"] = "1"
    engine = start_engine(s, "--volume", "game.stream=1.0", "--dest", "game=both")
    s.tone("SafetyPlayer", "rostrum.game")
    s.route("SafetyPlayer", "rostrum.game")
    s.wait("sends removed after fallback",
           lambda: not any(is_send_name(o["info"]["props"].get("node.name", ""))
                           for o in s.graph() if o["type"].endswith(":Node")),
           )
    # After fallback the bus links straight into both masters at the Stream level.
    s.route("rostrum.game", "rostrum.phones")
    s.route("rostrum.game", "rostrum.stream")
    s.capture("rostrum.phones", 440)
    s.capture("rostrum.stream", 440)
    print("PASS fallback", flush=True)
    return engine


def realtime_cost(s):
    buses = ("game", "voice", "music", "alerts", "desktop", "extra-6")
    # Pin each tone; automatic placement would put them on Music or elsewhere.
    pin = []
    for i, bus in enumerate(buses):
        pin += ["--session", f"name:Tone{i}={bus}"]
    engine = start_engine(s, "--buses", "11", *pin)
    # Six tones keep the graph busy. --buses 11 adds Extra 6..Extra 11 after the five defaults.
    for i, bus in enumerate(buses):
        name = f"Tone{i}"
        target = f"rostrum.{bus}"
        s.tone(name, target)
        s.route(name, target)
    path = s.root / f"profile-{uuid.uuid4().hex}.json"
    output = path.open("wb")
    s.files.append(output)
    profiler = s.spawn(["pw-profiler", "-J"], output)
    # A loaded graph here runs fewer cycles per second than the denoise test's forced 512,
    # so allow longer than the shared 12 s wait.
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        if profiler.poll() is not None:
            raise safety.Failure(f"pw-profiler exited ({profiler.returncode})")
        if path.read_text(errors="replace").count('"type": "driver"') >= CYCLES:
            break
        time.sleep(0.05)
    else:
        raise safety.Failure(f"Timed out: {CYCLES} profiled graph cycles")
    s.stop(profiler)
    # Group follower busy times by the profiler entry order: each "driver" starts a cycle.
    per_cycle = []
    current = 0.0
    saw_send = False
    for line in path.read_text(errors="replace").splitlines():
        line = line.strip().lstrip("[").rstrip("], ")
        if not line.startswith("{"):
            continue
        entry = json.loads(line)
        if entry.get("type") == "driver":
            if saw_send:
                per_cycle.append(current)
            current = 0.0
            saw_send = False
            continue
        if entry.get("type") != "follower":
            continue
        name = entry.get("name") or ""
        if not is_send_name(name):
            continue
        if entry["finish"] > entry["awake"] > 0:
            current += (entry["finish"] - entry["awake"]) / 1e3  # ns → µs
            saw_send = True
    if saw_send:
        per_cycle.append(current)
    if len(per_cycle) < CYCLES:
        raise safety.Failure(f"pw-profiler recorded only {len(per_cycle)} send cycles")
    per_cycle = per_cycle[:CYCLES]
    per_cycle.sort()
    median = per_cycle[len(per_cycle) // 2]
    p99 = per_cycle[int(len(per_cycle) * 0.99)]
    peak = per_cycle[-1]
    print(f"  realtime_cost: send busy median {median:.1f} µs, p99 {p99:.1f} µs, max {peak:.1f} µs "
          f"over {len(per_cycle)} cycles", flush=True)
    if median > SEND_BUSY_MEDIAN_US:
        raise safety.Failure(f"send busy median {median:.1f} µs over {SEND_BUSY_MEDIAN_US} µs")
    print("PASS realtime_cost", flush=True)
    return engine, median, p99, peak


def run_case(name, fn):
    s = safety.Session(Path(sys.argv[1]).resolve(strict=True))
    try:
        s.start()
        s.device("test.headphones")
        return fn(s)
    except BaseException:
        safety.cleanup_preserving_failure(s.diagnostics)
        raise
    finally:
        safety.cleanup_preserving_failure(s.close)
        safety.cleanup_preserving_failure(safety.close_sessions)


def main():
    for sig in safety.INTERRUPTS:
        signal.signal(sig, lambda signum, _: (_ for _ in ()).throw(safety.Failure(f"Interrupted by {signum}")))
    if len(sys.argv) < 2:
        print("usage: per-mix-levels.py <rostrum-graphtest>", file=sys.stderr)
        return 2
    try:
        run_case("levels_differ", levels_differ)
        run_case("levels_persist_after_exit", levels_persist_after_exit)
        run_case("restart_keeps_sends", restart_keeps_sends)
        run_case("upgrade_from_direct_links", upgrade_from_direct_links)
        run_case("no_new_devices", no_new_devices)
        run_case("solo_headphones_only", solo_headphones_only)
        run_case("fallback", fallback)
        run_case("realtime_cost", realtime_cost)
    except safety.Failure as error:
        safety.report(f"FAIL: {error}")
        return 1
    print("PASS: per-mix levels", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
