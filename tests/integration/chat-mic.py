#!/usr/bin/env python3
"""Sounds in voice chat: Rostrum Chat Mic in a private PipeWire session.

A playback bus with voice chat on feeds its chat send, rostrum.<bus>.chat, into rostrum.chatmic, a
mono virtual source that also takes rostrum.filtered (the voice). Apps that record the mic are moved
onto it while it is in use. Each case runs in a fresh private PipeWire/WirePlumber/D-Bus session
with private saved scenes. Their one playback bus, Sounds, goes to both mixes, has no VOD and sends
to voice chat at full level. A 440 Hz clip plays on Sounds and a 220 Hz voice feeds the fake mic, so
the frequency tells which one arrived where. At full level the chat send's half gain makes up for
summing two channels into one, so both tones reach the chat mic at the test level (amplitude 0.1).

Routing claims are checked in the graph. The claims about which links ever existed (an app never
recorded the raw mic, moved before the chat mic went) use pw-mon, which reports every link the
daemon creates and every object it removes in order, so a link shorter than a graph poll is seen.
"""
import argparse
import array
import cmath
import importlib.util
import math
from pathlib import Path
import re
import signal
import sys
import time
import uuid

HERE = Path(__file__).parent


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, HERE / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


levels = load("per_mix_levels", "per-mix-levels.py")
release = load("mic_filter_release", "mic-filter-release.py")
stable = load("mic_filter_filtered_stable", "mic-filter-filtered-stable.py")
restart = load("wireplumber_restart", "wireplumber-restart.py")
pwrestart = load("pipewire_restart", "pipewire-restart.py")
# levels.run_case builds each Session from this copy, so its Failure is the one sessions raise.
safety = levels.safety
Failure = safety.Failure
RATE = safety.RATE

MIC = "test.saved-mic"
BUS, SEND, CHAT = "rostrum.sounds", "rostrum.sounds.chat", "rostrum.chatmic"
FILTERED, MICFX, STREAM_MIC = "rostrum.filtered", "rostrum.micfx", "rostrum.mic"
CLIP, APP, PICKED = "ClipPlayer", "ChatApp", "PickedApp"
VOICE_HZ, CLIP_HZ = 220, 440
NAMES = {VOICE_HZ: "voice", CLIP_HZ: "clip"}
# A tone that must not be there: 40 dB under the test level. 300 ms holds whole cycles of both tones,
# so neither leaks into the other's bin.
ABSENT = 0.001
# Voice and clip come back from a panic in the same volume pass; one quantum is about 5 ms.
TOGETHER_MS = 50
PLUGIN = None

SCENE = '''format = 1
name = "{name}"
[[bus]]
id = "mic"
kind = "input"
name = "Mic"
[[bus]]
id = "sounds"
name = "Sounds"
kind = "playback"
volume = 1.0
destination = "both"
auto = "none"
vod = {vod}
chat = {chat}
chat_muted = {muted}
chat_volume = 1.0
[[rule]]
match = "{clip}"
key = "name"
bus = "sounds"
'''


def toml_bool(value):
    return "true" if value else "false"


def configure(s, scenes, filters):
    """Saved scenes {name: (chat, vod, chat muted)}, Live the default, and Studio Voice on or off."""
    config = s.root / "config/rostrum"
    (config / "scenes").mkdir(parents=True, exist_ok=True)
    for name, (chat, vod, muted) in scenes.items():
        (config / "scenes" / f"{name.lower()}.toml").write_text(SCENE.format(
            name=name, chat=toml_bool(chat), vod=toml_bool(vod), muted=toml_bool(muted), clip=CLIP))
    settings = config / "settings.toml"
    settings.write_text(release.SETTINGS.format(enabled=toml_bool(filters)))
    return settings


class Rostrum:
    """rostrum-graphtest on the saved config, driven the way the deck and the mixer drive the app."""

    def __init__(self, s, *extra):
        self.s = s
        self.request = 0
        self.control = s.root / f"control-{uuid.uuid4().hex}"
        self.control.write_text("")
        self.filters = s.root / f"mic-filter-{uuid.uuid4().hex}"
        self.filters.write_text("")
        self.job = s.spawn([str(s.driver), "--config", "--mic", MIC, "--headphones", "test.headphones",
                            "--mic-filter-plugin", str(PLUGIN), "--mic-filter-file", str(self.filters),
                            "--control-file", str(self.control), *extra])
        self.log = next(log for child, log in s.processes if child is self.job)
        levels.wait_ready(s, self.job)

    def text(self):
        return self.log.read_text(errors="replace")

    def _send(self, path, what, acknowledgement):
        self.request += 1
        temporary = path.with_suffix(".tmp")
        temporary.write_text(f"{self.request} {what}\n")
        temporary.replace(path)
        done = f"{acknowledgement} {self.request}: {what}\n"
        self.s.wait(f"Rostrum applied {what}", lambda: done in self.text())

    def send(self, what):
        """mic-muted=, panic=, chat-muted:<bus>=, chat:<bus>=, scene= or save=."""
        self._send(self.control, what, "Control request")

    def filter(self, what):
        self._send(self.filters, what, "Mic filter request")


class Recording:
    """A capture that keeps running: a call app recording the default source, or a probe on a node."""

    def __init__(self, s, app=None, node=None):
        self.s, self.name = s, app or node
        self.path = s.root / f"recording-{uuid.uuid4().hex}.raw"
        output = self.path.open("wb")
        s.files.append(output)
        args = ["pw-cat", "-r", "-a", "--format", "f32", "--rate", str(RATE), "--channels", "1"]
        if node:
            args += ["--target", serial(s, node),
                     "-P", "{ rostrum.internal = true node.dont-fallback = true node.dont-move = true }"]
        else:
            # No target: WirePlumber links it to the default source, the fake mic.
            args += ["-P", f"{{ node.name = {app} application.name = {app} }}"]
        self.job = s.spawn(args + ["-"], output)
        s.wait(f"{self.name} recording", lambda: self.size() >= RATE * 4 // 10)

    def size(self):
        return self.path.stat().st_size

    def heard(self, *present):
        """The next half second carries exactly these test tones (silence for none)."""
        start = self.size()
        self.s.wait(f"{self.name} records half a second more", lambda: self.size() >= start + RATE * 4 // 2)
        check(f"{self.name} recording", self.path.read_bytes()[start:], present)


class Monitor:
    """The private daemon's registry events from now on, in order: links added and objects removed."""

    def __init__(self, s):
        self.s = s
        self.path = s.root / f"pw-mon-{uuid.uuid4().hex}.txt"
        output = self.path.open("wb")
        s.files.append(output)
        self.job = s.spawn(["pw-mon", "-N", "-o", "-a"], output)
        # It lists the existing objects once it is bound to the registry; later events follow in order.
        s.wait("pw-mon bound to the private registry", lambda: "added:" in self.path.read_text(errors="replace"))

    def stop(self):
        """Stop watching (pw-mon flushes on exit) and return (kind, id, from node, to node) events."""
        self.s.stop(self.job)
        events = []
        for block in re.split(r"(?m)^(?=added:|changed:|removed:)", self.path.read_text(errors="replace")):
            kind = block.split(":", 1)[0]
            found = re.search(r"(?m)^\tid: (\d+)$", block)
            if not found:
                continue
            if kind == "removed":
                events.append(("removed", int(found.group(1)), None, None))
            elif kind == "added" and "type: PipeWire:Interface:Link" in block:
                out = re.search(r"output-node-id: (\d+)", block)
                into = re.search(r"input-node-id: (\d+)", block)
                if out and into:
                    events.append(("link", int(found.group(1)), int(out.group(1)), int(into.group(1))))
        return events


def amplitude(samples, frequency):
    return 2 * abs(sum(v * cmath.exp(-2j * math.pi * frequency * i / RATE)
                       for i, v in enumerate(samples))) / len(samples)


def check(label, raw, present, channels=1):
    """The last 300 ms of every channel carry each tone in `present` at the test level and no other
    test tone. With none present the bar is the harness's silence (RMS at most -100 dBFS)."""
    if not present:
        safety.Session.verify(label, raw, None, channels)
        return
    data = array.array("f")
    data.frombytes(raw[:len(raw) // (4 * channels) * 4 * channels])
    if len(data) < RATE * channels * 3 // 10:
        raise Failure(f"{label}: fewer than 300 ms of real buffers")
    for channel in range(channels):
        samples = data[channel::channels][-RATE * 3 // 10:]
        found = []
        for frequency in (VOICE_HZ, CLIP_HZ):
            a = amplitude(samples, frequency)
            what = f"{NAMES[frequency]} ({frequency} Hz)"
            if frequency in present and not 0.07 < a < 0.13:
                raise Failure(f"{label}: expected the {what} on channel {channel} at the test level, amplitude={a:.6f}")
            if frequency not in present and a > ABSENT:
                raise Failure(f"{label}: the {what} leaked on channel {channel}, amplitude={a:.6f}")
            found.append(f"{NAMES[frequency]} {a:.4f}")
        print(f"  {label}[{channel}]: {', '.join(found)}", flush=True)


def heard(s, node, *present, channels=1):
    check(node, s._capture_raw(node, channels, 0.5), present, channels)


def node_id(s, name):
    return levels.node_id(s.graph(), name)


def serial(s, name):
    value = stable.serial_of(s.graph(), name)
    if value is None:
        raise Failure(f"Missing {name}")
    return value


def mic(s):
    """The fake mic, above the headphones' monitor so that it is the default source apps record."""
    s.device(MIC, source=True, priority=3000)


def voice(s):
    """220 Hz into the fake mic by an explicit link, never a chosen target. Linked again when the fake
    mic came back, or WirePlumber rebuilt its ports on starting (which drops the link)."""
    if not s.node(s.graph(), "VoiceFeed"):
        s.tone("VoiceFeed", "0", VOICE_HZ, 1, internal=True)
    if not stable.links(s.graph(), "VoiceFeed", MIC):
        s.command("pw-link", "VoiceFeed:output_MONO", f"{MIC}:input_MONO")
    s.route("VoiceFeed", MIC)


def clip(s):
    s.tone(CLIP, BUS, CLIP_HZ)
    s.route(CLIP, BUS)


def studio_voice(s):
    s.route(MIC, MICFX)
    s.route(MICFX, FILTERED)


def chat_path(s):
    """Sounds -> its chat send -> the chat mic, and the voice from filtered."""
    s.route(BUS, SEND)
    s.route(SEND, CHAT)
    s.route(FILTERED, CHAT)


def records(s, app, source, target, label):
    """Wait until `app` records `source` only, with `target` (a serial, or None) as its target.object."""
    def settled():
        g = s.graph()
        node = s.node(g, app)
        return node and release.sources_of(g, app) == {source} and s.linked(g, source, app) \
            and release.target(g, node) == target
    s.wait(f"{label}: {app} records only {source}", settled)


def stays(s, app, app_id, chat_serial, label):
    """The app is the same stream, still records the chat mic only, by the same target."""
    g = s.graph()
    node = s.node(g, app)
    if not node or node["id"] != app_id:
        raise Failure(f"{label}: {app} is not the stream it was")
    if release.target(g, node) != chat_serial or release.sources_of(g, app) != {CHAT}:
        raise Failure(f"{label}: {app} left the chat mic (target {release.target(g, node)}, "
                      f"sources {release.sources_of(g, app)})")
    if stable.serial_of(g, CHAT) != chat_serial:
        raise Failure(f"{label}: the chat mic is not the node it was")


def moved_only_by_chat(events, app_id, chat_id, label):
    """While watched, nothing but the chat mic was linked into the app."""
    others = {out for kind, _, out, into in events if kind == "link" and into == app_id and out != chat_id}
    if others:
        raise Failure(f"{label}: {APP} was linked from nodes {sorted(others)} besides the chat mic {chat_id}")


def in_log_before(text, first, then, label):
    a, b = text.find(first), text.find(then)
    if a < 0 or b < 0 or b < a:
        raise Failure(f"{label}: expected '{first}' before '{then}' in Rostrum's log")


def onset_ms(raw, frequency, level=0.05):
    """Where `frequency` first reaches half the test level: the start of a 50 ms window, in 5 ms steps."""
    data = array.array("f")
    data.frombytes(raw[:len(raw) // 4 * 4])
    window, hop = RATE // 20, RATE // 200
    basis = [cmath.exp(-2j * math.pi * frequency * i / RATE) for i in range(window)]
    for start in range(0, len(data) - window + 1, hop):
        if 2 * abs(sum(v * b for v, b in zip(data[start:start + window], basis))) / window >= level:
            return start * 1000 // RATE
    return None


# ---- cases ------------------------------------------------------------------------------------


def clip_reaches_chat_once(s):
    """A clip on Sounds reaches the chat mic and the Stream Mix once; never the stream mic, filtered,
    or the VOD mix while the bus has VOD off."""
    mic(s)
    configure(s, {"Live": (True, False, False), "Vod": (True, True, False)}, filters=True)
    rostrum = Rostrum(s)
    studio_voice(s)
    clip(s)
    chat_path(s)
    s.mix_route(BUS, "rostrum.stream")
    heard(s, CHAT, CLIP_HZ)
    s.capture("rostrum.stream", CLIP_HZ)  # once: a second path would double the level
    s.capture(STREAM_MIC, None, 1)
    s.capture(FILTERED, None, 1)
    s.capture("rostrum.vod")
    g = s.graph()
    chat, send = s.node(g, CHAT)["info"]["props"], s.node(g, SEND)["info"]["props"]
    want = {"node.description": "Rostrum Chat Mic", "rostrum.role": "chatmic", "media.class": "Audio/Source/Virtual",
            "audio.channels": "1", "object.linger": "true"}
    got = {k: str(chat.get(k)).lower() if k == "object.linger" else str(chat.get(k)) for k in want}
    if got != want:
        raise Failure(f"chat mic properties {got}, want {want}")
    if send.get("rostrum.role") != "send" or send.get("rostrum.mix") != "chat" or send.get("media.class") \
            or send.get("node.description") != "Rostrum Sounds → Voice chat":
        raise Failure(f"chat send properties: {send}")
    # The send feeds only the chat mic, which takes only the send and the voice.
    if restart.targets(s, g, SEND) != {CHAT} or release.sources_of(g, CHAT) != {SEND, FILTERED}:
        raise Failure(f"chat send feeds {restart.targets(s, g, SEND)}, chat mic takes {release.sources_of(g, CHAT)}")
    for node in (STREAM_MIC, FILTERED, MICFX):
        if release.sources_of(g, node) - {MIC, MICFX}:
            raise Failure(f"{node} takes {release.sources_of(g, node)}")
    rostrum.send("scene=Vod")
    s.mix_route(BUS, "rostrum.vod")
    s.capture("rostrum.vod", CLIP_HZ)
    s.capture("rostrum.stream", CLIP_HZ)
    heard(s, CHAT, CLIP_HZ)
    s.capture(STREAM_MIC, None, 1)
    s.capture(FILTERED, None, 1)


def mic_mute_keeps_clips(s):
    """Mic mute takes the voice off the chat mic and the stream mic; clips keep going to the chat."""
    mic(s)
    voice(s)
    configure(s, {"Live": (True, False, False)}, filters=True)
    rostrum = Rostrum(s)
    studio_voice(s)
    clip(s)
    chat_path(s)
    heard(s, CHAT, VOICE_HZ, CLIP_HZ)
    heard(s, STREAM_MIC, VOICE_HZ)
    rostrum.send("mic-muted=1")
    heard(s, CHAT, CLIP_HZ)
    s.capture(STREAM_MIC, None, 1)
    s.capture(FILTERED, None, 1)
    rostrum.send("mic-muted=0")
    heard(s, CHAT, VOICE_HZ, CLIP_HZ)
    heard(s, STREAM_MIC, VOICE_HZ)


def panic_silences_chat(s):
    """Panic silences the chat mic, clips too, and the call app on it hears nothing."""
    mic(s)
    voice(s)
    configure(s, {"Live": (True, False, False)}, filters=False)
    rostrum = Rostrum(s)
    clip(s)
    chat_path(s)
    app = Recording(s, app=APP)
    records(s, APP, CHAT, serial(s, CHAT), "before panic")
    app.heard(VOICE_HZ, CLIP_HZ)
    rostrum.send("panic=1")
    app.heard()
    s.capture(CHAT, None, 1)
    s.capture(STREAM_MIC, None, 1)
    s.capture(FILTERED, None, 1)
    s.capture("rostrum.stream")
    rostrum.send("panic=0")
    app.heard(VOICE_HZ, CLIP_HZ)
    heard(s, STREAM_MIC, VOICE_HZ)


def panic_then_unmute(s):
    """Unmuting the mic after panic brings voice and clips back on the chat mic together."""
    mic(s)
    voice(s)
    configure(s, {"Live": (True, False, False)}, filters=True)
    rostrum = Rostrum(s)
    studio_voice(s)
    clip(s)
    chat_path(s)
    probe = Recording(s, node=CHAT)
    probe.heard(VOICE_HZ, CLIP_HZ)
    rostrum.send("panic=1")
    probe.heard()
    mark = probe.size()
    rostrum.send("mic-muted=0")
    probe.heard(VOICE_HZ, CLIP_HZ)
    raw = probe.path.read_bytes()[mark:]
    back = {frequency: onset_ms(raw, frequency) for frequency in (VOICE_HZ, CLIP_HZ)}
    print(f"  back after unmute (ms into the recording): voice {back[VOICE_HZ]}, clip {back[CLIP_HZ]}", flush=True)
    if None in back.values() or abs(back[VOICE_HZ] - back[CLIP_HZ]) > TOGETHER_MS:
        raise Failure(f"voice and clip did not come back together: {back}")


def capture_app_moves_to_chatmic_and_back(s):
    """A call app moves to the chat mic; the live toggle and chat off leave it there while a saved
    scene still chats; with chat off everywhere it goes to filtered before the chat mic is removed,
    and it never records the raw mic (Studio Voice on)."""
    mic(s)
    voice(s)
    configure(s, {"Live": (True, False, False), "Backup": (True, False, False)}, filters=True)
    rostrum = Rostrum(s)
    studio_voice(s)
    clip(s)
    chat_path(s)
    app = Recording(s, app=APP)
    chat_serial = serial(s, CHAT)
    records(s, APP, CHAT, chat_serial, "chat on")
    app_id, chat_id, mic_id, filtered_id = (node_id(s, n) for n in (APP, CHAT, MIC, FILTERED))
    app.heard(VOICE_HZ, CLIP_HZ)

    rostrum.send("chat-muted:sounds=1")
    app.heard(VOICE_HZ)
    if not s.node(s.graph(), SEND):
        raise Failure("the live toggle removed the chat send")
    stays(s, APP, app_id, chat_serial, "chat muted")
    rostrum.send("chat-muted:sounds=0")
    app.heard(VOICE_HZ, CLIP_HZ)
    stays(s, APP, app_id, chat_serial, "chat unmuted")

    # Live and Backup are still saved with chat on, then Backup alone.
    for what in ("chat:sounds=0", "save=1"):
        rostrum.send(what)
        s.wait(f"after {what}, chat send removed", lambda: s.node(s.graph(), SEND) is None)
        app.heard(VOICE_HZ)
        stays(s, APP, app_id, chat_serial, f"after {what}")
    if "chat mic wanted false" in rostrum.text():
        raise Failure("the chat mic stopped being wanted while Backup still chats")
    rostrum.send("scene=Backup")
    chat_path(s)
    app.heard(VOICE_HZ, CLIP_HZ)
    stays(s, APP, app_id, chat_serial, "Backup")

    monitor, since = Monitor(s), len(rostrum.text())
    mark = app.size()
    rostrum.send("chat:sounds=0")
    app.heard(VOICE_HZ)
    stays(s, APP, app_id, chat_serial, "Backup, chat off live")
    rostrum.send("save=1")  # now no saved scene chats
    s.wait("chat mic removed", lambda: s.node(s.graph(), CHAT) is None)
    records(s, APP, FILTERED, serial(s, FILTERED), "chat off everywhere")
    app.heard(VOICE_HZ)
    events = monitor.stop()
    released_to_filtered(rostrum.text()[since:], events, app_id, chat_id, mic_id, filtered_id, "chat off everywhere")
    if s.node(s.graph(), MICFX) is None or s.node(s.graph(), FILTERED) is None:
        raise Failure("Studio Voice nodes went with the chat mic")
    found = [(base, length) for base, length in stable.gaps_ms(app.path.read_bytes()[mark:])
             if length > stable.MAX_GAP_MS]
    if found:
        raise Failure(f"{APP} went silent for longer than one handoff (start ms, length ms): {found}")


def released_to_filtered(text, events, app_id, chat_id, mic_id, filtered_id, label):
    """R9 with Studio Voice on: the app was linked from filtered before the chat mic was removed, and
    nothing linked the raw mic into the app or into filtered while it moved. `text` is Rostrum's log
    since the watch began."""
    raw = [e for e in events if e[0] == "link" and e[2] == mic_id and e[3] in (app_id, filtered_id)]
    if raw:
        raise Failure(f"{label}: the raw mic was linked into {APP} or filtered: {raw}")
    linked = next((i for i, e in enumerate(events) if e[0] == "link" and e[2] == filtered_id and e[3] == app_id), None)
    removed = next((i for i, e in enumerate(events) if e[0] == "removed" and e[1] == chat_id), None)
    if linked is None or removed is None or removed < linked:
        raise Failure(f"{label}: {APP} was not linked from filtered before the chat mic went "
                      f"(event {linked} vs {removed})")
    in_log_before(text, f'filtered mic for "{APP}"', 'removing "rostrum.chatmic"', label)
    if f'raw mic for "{APP}"' in text or "removing the chat mic with apps still on it" in text:
        raise Failure(f"{label}: {APP} went to the raw mic, or the chat mic went with it still on")
    print(f"  {label}: {APP} on filtered before the chat mic went, never on the raw mic", flush=True)


def filters_toggle_keeps_apps_on_chatmic(s):
    """Studio Voice off, on, failed and back leaves the app on the chat mic, hearing voice and clip."""
    mic(s)
    voice(s)
    settings = configure(s, {"Live": (True, False, False)}, filters=True)
    rostrum = Rostrum(s)
    studio_voice(s)
    clip(s)
    chat_path(s)
    app = Recording(s, app=APP)
    chat_serial = serial(s, CHAT)
    records(s, APP, CHAT, chat_serial, "Studio Voice on")
    app_id, chat_id = node_id(s, APP), node_id(s, CHAT)
    app.heard(VOICE_HZ, CLIP_HZ)
    monitor = Monitor(s)
    for label, enabled, request, processed in (("Studio Voice off", False, "settings", False),
                                               ("Studio Voice on again", True, "settings", True),
                                               ("filter library failed", None, "unavailable", False),
                                               ("filter library back", None, "available", True)):
        if enabled is not None:
            settings.write_text(release.SETTINGS.format(enabled=toml_bool(enabled)))
        rostrum.filter(request)
        want = {MICFX} if processed else {MIC}
        s.wait(f"{label}: filtered fed by {want} only",
               lambda: release.sources_of(s.graph(), FILTERED) == want
               and (processed or s.node(s.graph(), MICFX) is None))
        app.heard(VOICE_HZ, CLIP_HZ)
        stays(s, APP, app_id, chat_serial, label)
    moved_only_by_chat(monitor.stop(), app_id, chat_id, "filter changes")


def scene_switch(s):
    """Switching to a scene without chat, or with it muted, leaves the app on the chat mic (another
    saved scene still chats); the clip leaves the chat and comes back with the scene (Studio Voice off)."""
    mic(s)
    voice(s)
    configure(s, {"Live": (True, False, False), "Quiet": (False, False, False), "Hushed": (True, False, True)},
              filters=False)
    rostrum = Rostrum(s)
    clip(s)
    chat_path(s)
    app = Recording(s, app=APP)
    chat_serial = serial(s, CHAT)
    records(s, APP, CHAT, chat_serial, "Live")
    app_id, chat_id = node_id(s, APP), node_id(s, CHAT)
    app.heard(VOICE_HZ, CLIP_HZ)
    monitor = Monitor(s)
    rostrum.send("scene=Quiet")
    s.wait("Quiet: chat send removed", lambda: s.node(s.graph(), SEND) is None)
    app.heard(VOICE_HZ)
    s.capture("rostrum.stream", CLIP_HZ)
    stays(s, APP, app_id, chat_serial, "Quiet")
    rostrum.send("scene=Hushed")
    chat_path(s)
    app.heard(VOICE_HZ)
    stays(s, APP, app_id, chat_serial, "Hushed")
    rostrum.send("scene=Live")
    app.heard(VOICE_HZ, CLIP_HZ)
    stays(s, APP, app_id, chat_serial, "Live again")
    events = monitor.stop()
    moved_only_by_chat(events, app_id, chat_id, "scene switches")
    if any(kind == "removed" and obj == chat_id for kind, obj, _, _ in events):
        raise Failure("a scene switch removed the chat mic")


def wireplumber_restart(s):
    """An app on the chat mic is back on it after WirePlumber restarts, and is still released to
    filtered, never the raw mic, once chat is off everywhere.

    A starting WirePlumber rebuilds the ports of the fake mic, filtered and the chat mic, which drops
    their links. Rostrum links its own again; the test links the voice feed again, as a real mic comes
    back with its device. While filtered is not linked into the chat mic, Rostrum counts the chat mic
    as not in use. The app was already on it, so its target stays the chat mic: the log must not send
    it to the raw mic during a restart. WirePlumber's own default link can show in pw-mon, so the
    check is the log."""
    mic(s)
    voice(s)
    configure(s, {"Live": (True, False, False)}, filters=True)
    rostrum = Rostrum(s)
    studio_voice(s)
    clip(s)
    chat_path(s)
    app = Recording(s, app=APP)
    chat_serial = serial(s, CHAT)
    records(s, APP, CHAT, chat_serial, "before restarts")
    app_id, chat_id, mic_id, filtered_id = (node_id(s, n) for n in (APP, CHAT, MIC, FILTERED))
    app.heard(VOICE_HZ, CLIP_HZ)
    for attempt in (1, 2):
        print(f"  WirePlumber restart {attempt}", flush=True)
        since = len(rostrum.text())
        restart.restart_policy(s)
        studio_voice(s)
        chat_path(s)
        voice(s)
        records(s, APP, CHAT, chat_serial, f"after restart {attempt}")
        app.heard(VOICE_HZ, CLIP_HZ)
        stays(s, APP, app_id, chat_serial, f"after restart {attempt}")
        if f'raw mic for "{APP}"' in rostrum.text()[since:]:
            raise Failure(f"restart {attempt} sent {APP} to the raw mic")
    monitor, since = Monitor(s), len(rostrum.text())
    rostrum.send("chat:sounds=0")
    rostrum.send("save=1")
    s.wait("chat mic removed", lambda: s.node(s.graph(), CHAT) is None)
    records(s, APP, FILTERED, serial(s, FILTERED), "chat off after restarts")
    app.heard(VOICE_HZ)
    released_to_filtered(rostrum.text()[since:], monitor.stop(), app_id, chat_id, mic_id, filtered_id,
                         "chat off after restarts")


def chat_off_everywhere(s):
    """R9 with Studio Voice off: once no scene chats, apps go back where they were (the raw mic, or the
    target they had) before the chat mic is removed, and only the voice reaches them."""
    mic(s)
    voice(s)
    configure(s, {"Live": (True, False, False)}, filters=False)
    app = Recording(s, app=APP)
    picked = Recording(s, app=PICKED)
    mic_serial = serial(s, MIC)
    # PickedApp chose the mic itself; WirePlumber holds that choice in the default metadata.
    s.command("pw-metadata", str(node_id(s, PICKED)), "target.object", mic_serial, "Spa:Id")
    records(s, PICKED, MIC, mic_serial, "picked the mic")
    records(s, APP, MIC, None, "default source")
    rostrum = Rostrum(s)
    clip(s)
    chat_path(s)
    chat_serial = serial(s, CHAT)
    for name, recording in ((APP, app), (PICKED, picked)):
        records(s, name, CHAT, chat_serial, "chat on")
        recording.heard(VOICE_HZ, CLIP_HZ)
    rostrum.send("chat:sounds=0")  # the saved Live scene still chats
    s.wait("chat send removed", lambda: s.node(s.graph(), SEND) is None)
    for name, recording in ((APP, app), (PICKED, picked)):
        recording.heard(VOICE_HZ)
        stays(s, name, node_id(s, name), chat_serial, "chat off live, still saved")
    since = len(rostrum.text())
    rostrum.send("save=1")
    s.wait("chat mic removed", lambda: s.node(s.graph(), CHAT) is None)
    records(s, APP, MIC, None, "chat off everywhere")
    records(s, PICKED, MIC, mic_serial, "chat off everywhere")
    app.heard(VOICE_HZ)
    picked.heard(VOICE_HZ)
    heard(s, STREAM_MIC, VOICE_HZ)
    if s.node(s.graph(), FILTERED) is None:
        raise Failure("rostrum.filtered went with the chat mic")
    text = rostrum.text()[since:]
    for name in (APP, PICKED):
        in_log_before(text, f'raw mic for "{name}"', 'removing "rostrum.chatmic"', "chat off everywhere")
    if "removing the chat mic with apps still on it" in text:
        raise Failure("the chat mic was removed with apps still on it")


def missing_mic_clips_still_reach_chat(s):
    """With the saved mic missing, clips reach the chat mic; an app on it stays there while the mic is
    unplugged and gets the voice back when it returns."""
    configure(s, {"Live": (True, False, False)}, filters=True)
    rostrum = Rostrum(s)
    clip(s)
    chat_path(s)
    heard(s, CHAT, CLIP_HZ)
    s.capture(STREAM_MIC, None, 1)
    s.capture(FILTERED, None, 1)
    mic(s)
    voice(s)
    studio_voice(s)
    app = Recording(s, app=APP)
    chat_serial = serial(s, CHAT)
    records(s, APP, CHAT, chat_serial, "mic present")
    app_id = node_id(s, APP)
    app.heard(VOICE_HZ, CLIP_HZ)
    s.remove(MIC)
    app.heard(CLIP_HZ)
    stays(s, APP, app_id, chat_serial, "mic unplugged")
    s.capture(STREAM_MIC, None, 1)
    mic(s)
    voice(s)
    studio_voice(s)
    app.heard(VOICE_HZ, CLIP_HZ)
    stays(s, APP, app_id, chat_serial, "mic back")
    moves = rostrum.text().count(f'chat mic for "{APP}"')
    if moves != 1:
        raise Failure(f"{APP} was moved to the chat mic {moves} times; once was expected")


def stop_players(s):
    """pw-cat exits with the daemon. Drop those jobs before the next wait, which treats any exit as
    a failure."""
    for child, log in list(s.processes):
        if log.name.endswith("-pw-cat.log"):
            s.stop(child)


def pipewire_restart(s):
    """After PipeWire restarts, the chat mic comes back as a new node and the call app is moved
    onto it. Voice and the clip both reach the app.

    The daemon numbers objects from scratch. An extra device is created before Rostrum reconnects,
    so the new chat mic cannot come back under the serial it had."""
    mic(s)
    voice(s)
    configure(s, {"Live": (True, False, False)}, filters=True)
    reconnect = s.root / "reconnect-request"
    reconnect.write_text("")
    rostrum = Rostrum(s, "--reconnect-file", str(reconnect))
    studio_voice(s)
    clip(s)
    chat_path(s)
    app = Recording(s, app=APP)
    before = serial(s, CHAT)
    records(s, APP, CHAT, before, "before the restart")
    app.heard(VOICE_HZ, CLIP_HZ)
    stop_players(s)
    pwrestart.restart_daemon(s, rostrum.log, 1)
    # One node that did not exist before Rostrum last created the chat mic, so the new serial differs.
    s.device("test.extra", source=True, priority=1000)
    reconnect.write_text("1\n")
    s.wait("Rostrum reconnected", lambda: "Reconnect request 1" in rostrum.text())
    s.wait("mix ready again", lambda: rostrum.text().count("Mix ready.") >= 2)
    mic(s)
    voice(s)
    studio_voice(s)
    clip(s)
    chat_path(s)
    after = serial(s, CHAT)
    if after == before:
        raise Failure(f"the chat mic came back under serial {before}")
    app = Recording(s, app=APP)
    records(s, APP, CHAT, after, "after the restart")
    app.heard(VOICE_HZ, CLIP_HZ)
    moves = rostrum.text().count(f'chat mic for "{APP}"')
    if moves < 1:
        raise Failure(f"{APP} was not moved to the new chat mic")
    print(f"  chat mic serial {before} -> {after}", flush=True)


def no_dont_flags(s):
    """No Rostrum node and no exported rule carries node.dont-*; only its meters do. Rostrum moves the
    app by target.object alone, and writes nothing to the WirePlumber configuration."""
    mic(s)
    voice(s)
    configure(s, {"Live": (True, False, False)}, filters=True)
    rostrum = Rostrum(s, "--export-rules")
    s.wait("rules exported", lambda: "Rules exported." in rostrum.text())
    studio_voice(s)
    clip(s)
    chat_path(s)
    # A meter as the app runs one. The meter-only driver has an engine of its own with the mix off,
    # which would point a call app off the chat mic, so the app starts once the meter has gone.
    meter = f"rostrum-meter.{CHAT}"
    metering = s.spawn([str(s.driver), "--meter", CHAT])
    s.wait("chat mic meter", lambda: s.node(s.graph(), meter))
    checked, meters = [], []
    for o in s.graph():
        if not o["type"].endswith(":Node"):
            continue
        props = o["info"]["props"]
        name = props.get("node.name", "")
        if not (name.startswith("rostrum.") or name.startswith("rostrum-meter.") or "rostrum.role" in props):
            continue
        flags = sorted(k for k in props if k.startswith("node.dont-"))
        if name.startswith("rostrum-meter."):
            meters.append((name, flags))
        elif flags:
            raise Failure(f"{name} carries {flags}")
        else:
            checked.append(name)
    s.stop(metering)
    for want in (CHAT, SEND, FILTERED, MICFX, STREAM_MIC, BUS):
        if want not in checked:
            raise Failure(f"{want} was not checked; checked {checked}")
    # The meter shows the scan sees these properties at all.
    if meters != [(meter, ["node.dont-fallback", "node.dont-move", "node.dont-reconnect"])]:
        raise Failure(f"meters seen: {meters}")
    Recording(s, app=APP)
    records(s, APP, CHAT, serial(s, CHAT), "chat on")
    g = s.graph()
    app = s.node(g, APP)
    flags = sorted(k for k in app["info"]["props"] if k.startswith("node.dont-"))
    if flags:
        raise Failure(f"{APP} carries {flags}")
    for o in g:
        if o["type"].endswith(":Metadata"):
            entries = o.get("metadata") or []
            keys = {entry.get("key", "") for entry in entries}
            if any(key.startswith("node.dont-") for key in keys):
                raise Failure(f"metadata {o.get('props', {}).get('metadata.name')} holds {sorted(keys)}")
            on_app = {entry.get("key") for entry in entries if entry.get("subject") == app["id"]}
            if o.get("props", {}).get("metadata.name") == "default" and on_app != {"target.object"}:
                raise Failure(f"{APP} has {sorted(on_app)} in the default metadata, want target.object only")
    config = s.root / "config"
    for fragment in ("pipewire/pipewire-pulse.conf.d/50-rostrum.conf", "pipewire/client.conf.d/50-rostrum.conf"):
        text = (config / fragment).read_text()
        if f'target.object = "{BUS}"' not in text or "dont-" in text:
            raise Failure(f"{fragment}: no rule to {BUS}, or a node.dont-* flag:\n{text}")
    written = sorted(p.name for p in (config / "wireplumber").rglob("*"))
    if written != ["wireplumber.conf"]:  # the harness's own copy
        raise Failure(f"files under the WirePlumber configuration: {written}")
    print(f"  checked {len(checked)} Rostrum nodes and both rule fragments; only {meter} carries node.dont-*",
          flush=True)


CASES = (clip_reaches_chat_once, mic_mute_keeps_clips, panic_silences_chat, capture_app_moves_to_chatmic_and_back,
         panic_then_unmute, filters_toggle_keeps_apps_on_chatmic, scene_switch, wireplumber_restart,
         pipewire_restart, chat_off_everywhere, missing_mic_clips_still_reach_chat, no_dont_flags)


def main():
    global PLUGIN
    for sig in safety.INTERRUPTS:
        signal.signal(sig, lambda signum, _: (_ for _ in ()).throw(Failure(f"Interrupted by {signum}")))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path)  # levels.run_case reads it from sys.argv[1]
    parser.add_argument("plugin", type=Path)
    parser.add_argument("--case", choices=[case.__name__ for case in CASES], action="append")
    args = parser.parse_args()
    if Path(sys.argv[1]) != args.driver:
        parser.error("give the driver first")
    args.driver.resolve(strict=True)
    PLUGIN = args.plugin.resolve(strict=True)
    try:
        for case in CASES:
            if args.case and case.__name__ not in args.case:
                continue
            print(f"Case {case.__name__}", flush=True)
            began = time.monotonic()
            levels.run_case(case.__name__, case)
            print(f"PASS {case.__name__} ({time.monotonic() - began:.1f} s)", flush=True)
    except RuntimeError as error:  # every harness module's Failure
        safety.report(f"FAIL: {error}")
        return 1
    print("PASS: sounds reach voice chat once, never the stream mic", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
