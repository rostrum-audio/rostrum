# Audio design

Rostrum owns a small, fixed PipeWire graph. It never runs a second audio daemon and never
shells out to `pactl` in the steady state.

```
App streams ----> rostrum.<bus> ----> rostrum.phones ----> headphones device
                         |
                         +--> rostrum.stream (virtual sink, "Rostrum Stream Mix")
                                    |
                                    +--> monitor  (OBS captures this)

Hardware mic ---> rostrum.mic (virtual source, "Rostrum Mic")      (OBS captures this)
             \--> rostrum.sidetone ---> rostrum.phones             (optional, default off)
```

## Nodes

Every Rostrum node is a `support.null-audio-sink` adapter created in the PipeWire daemon with
`pw_core_create_object("adapter", ...)`.

| Node | media.class | Channels | Description | Purpose |
|------|-------------|----------|-------------|---------|
| `rostrum.<bus>` | `Audio/Sink` | FL FR | `Rostrum <Name>` | One per playback bus. Apps are moved here. |
| `rostrum.phones` | `Audio/Sink` | FL FR | `Rostrum Phones Mix` | Sum of everything bound for headphones. Volume = Master Phones. |
| `rostrum.stream` | `Audio/Sink` | FL FR | `Rostrum Stream Mix` | Sum of everything bound for the stream. Volume = Master Stream. OBS captures its monitor. |
| `rostrum.mic` | `Audio/Source/Virtual` | MONO | `Rostrum Mic` | The hardware mic after Rostrum's gain and mute. OBS captures this. |
| `rostrum.sidetone` | `Audio/Sink` | MONO | `Rostrum Sidetone` | Mic monitoring into headphones. Volume = sidetone fader. |

Properties set on every node:

- `object.linger = true`: the node lives in the daemon, not in Rostrum. If Rostrum quits or
  crashes, audio keeps flowing. When Rostrum starts it adopts existing nodes by `node.name`
  instead of creating duplicates, and removes duplicates left by a race (oldest wins).
- `monitor.channel-volumes = true`: the node's volume and mute also apply to its monitor ports,
  which is what makes a bus fader or the Master Stream fader audible to OBS.
- `priority.session = 0`, `priority.driver = 0`: WirePlumber should not pick a Rostrum node as
  the system default device.
- `rostrum.role` (`bus`, `phones`, `stream`, `mic`, `sidetone`) and `rostrum.bus`: Rostrum only
  ever destroys nodes that carry these.

Limitation and workaround: the spec asked for "loopback nodes". An in-process
`libpipewire-module-loopback` would put Rostrum's own process in the audio path, so a Rostrum
crash would cut the stream. Null-sink adapters plus explicit links give the same topology with the
processing inside the PipeWire daemon. Bus renames change the description only after
"Rebuild virtual devices", because a live node's `node.description` is fixed at creation.

## Destinations and links

Rostrum creates links port by port with `link-factory` (`object.linger = true`), matched by
`audio.channel`. The desired link set is recomputed on every graph change:

| From | To | When |
|------|----|------|
| `rostrum.<bus>` monitor | `rostrum.phones` | bus destination is Phones or Both |
| `rostrum.<bus>` monitor | `rostrum.stream` | bus destination is Stream or Both |
| `rostrum.phones` monitor | headphone device | always |
| hardware mic | `rostrum.mic` | always (mute/destination act on the node, not the link) |
| hardware mic | `rostrum.sidetone` | always |
| `rostrum.sidetone` monitor | `rostrum.phones` | always (muted unless sidetone is on) |

Rostrum only removes links it manages: links that start at a Rostrum node and end at a Rostrum node
or a hardware sink, and links into `rostrum.mic` / `rostrum.sidetone`. App streams going into a bus
and OBS capturing a Rostrum monitor are never touched. A link you patch by hand from a Rostrum node
to a hardware sink will be removed.

Ports of a new device arrive one by one, so a node is only linked once it has as many ports as its
channel count. Otherwise a half-enumerated stereo device would briefly look mono.

### Volumes, mute and solo

Fader positions are perceptual: linear gain = position³, as in pavucontrol. They are sent as
`SPA_PROP_channelVolumes` and `SPA_PROP_mute` on the node's `Props` param.

- Bus node: fader, muted if the bus is muted or dimmed by solo.
- `rostrum.phones` / `rostrum.stream`: Master Phones / Master Stream. These multiply every bus send.
  "Mute all playback to stream" mutes `rostrum.stream`.
- `rostrum.mic`: mic gain (0 to 150%, 100% = 0 dB), muted if the mic is muted or the mic
  destination does not include Stream.
- `rostrum.sidetone`: sidetone fader, muted unless the mic destination includes Phones, the mic is
  live, and the fader is above zero.

If something else changes a Rostrum node's volume (WirePlumber's state restore, another mixer),
Rostrum re-applies the scene value at most once a second, so two tools cannot get into a loop.

### Mic channel handling

`rostrum.mic` and `rostrum.sidetone` are mono. Every channel of the hardware mic is summed into
them, because many USB interfaces expose a mono mic as stereo with signal on one side only. A true
dual-mono feed comes out 6 dB hotter, which the mic gain fader covers. A virtual source built from a
null sink (Easy Effects Source, for example) reports its capture ports as `port.monitor = true`.
Rostrum accepts those ports when a non-sink node has no other outputs.

### Devices

The headphone target is the saved `node.name` if present, otherwise the system default sink
(`default.audio.sink` metadata), otherwise the highest `priority.session` sink. Rostrum nodes are
never candidates. The mic works the same way with sources.

If the saved headphones disappear, the scene is held unchanged. `rostrum.phones` is relinked to the
fallback, the UI shows a banner, and one desktop notification is sent ("Headphones disconnected,
scene held"). When a node with the saved `node.name` returns, Rostrum relinks to it. The saved
device is never rewritten by a fallback.

## Moving app streams

An app stream is moved by setting `target.object` in the `default` metadata object for the stream's
node id, with type `Spa:Id` and the bus node's `object.serial` as the value. This is the same key
WirePlumber 0.5 writes itself; `scripts/linking/find-defined-target.lua` reads it and relinks the
stream. No `pactl`, no `module-move`.

- The router sets `target.object` for every matching running stream on every reconcile pass. A
  pass runs whenever a node, port, link or metadata value changes. So when a bus node is created or
  re-created (new serial), every stream whose rule points at that bus is retargeted right away.
  Rostrum never waits for a WirePlumber rescan to do this.
- Before the first move, the router records the stream's previous metadata target (if it was not a
  Rostrum node). Unassigning restores that value, or clears `target.object` / `target.node` so
  WirePlumber relinks the stream to the default sink (or to the stream's own `target.object`
  property).
- Once Rostrum has asked for a target, it does not re-send it unless the bus serial changes. If the
  user moves the stream in another mixer afterwards, Rostrum leaves it there.
- Streams from Rostrum's own process (meters, test tones) and any `rostrum.*` node are ignored.

### App identity

Apps are shown by `application.name`. Generic names that do not identify the app (Chromium's
`WEBRTC VoiceEngine` used by Discord, ALSA/SDL/OpenAL shims) fall back to
`application.process.binary`, cleaned of a ` (deleted)` suffix (left when a binary updates while
running). A new rule matches on whichever key the identity came from, and the app row shows that
key. Name rules are checked before binary rules. Matching is case-insensitive.

### Rules for apps that start before Rostrum

While Rostrum runs, the router above handles every stream. App rules from the default scene are
also written as PipeWire client rule fragments. That way an app started at login, before Rostrum,
asks for its bus itself:

| File | Array | Read by |
| --- | --- | --- |
| `~/.config/pipewire/pipewire-pulse.conf.d/50-rostrum.conf` | `pulse.rules` | `pipewire-pulse` when it starts (normally at login), applied to every PulseAudio client: Discord, browsers, most games |
| `~/.config/pipewire/client.conf.d/50-rostrum.conf` | `stream.rules` | every native PipeWire app when it starts |

The array names are copied from `/usr/share/pipewire/pipewire-pulse.conf` and
`/usr/share/pipewire/client.conf`, and a unit test checks them against the installed files. Fragment
arrays are appended to the system ones, so the system rules (for example Firefox quirks) stay in
effect.

Each rule matches `application.name` or `application.process.binary` with an anchored,
ASCII-case-insensitive regex. The only property it sets is `target.object = "rostrum.<bus>"`.
Binary rules are written first, so a name rule for the same app wins, as it does in the app.
WirePlumber's `find-defined-target.lua` honours the property when it first links the stream.

The rules never set `node.dont-fallback`, `node.dont-move` or `node.dont-reconnect`. If the bus
node is missing, for example because Rostrum is not running, WirePlumber links the stream to the
default sink. Quitting Rostrum therefore never mutes Discord, and the user can still move the stream
in any other mixer. A golden test fails if any of these keys appears.

Nothing is written to `~/.config/wireplumber/`. In WirePlumber 0.5, `stream.rules` in
`wireplumber.conf` only feed `state-stream.lua` (restoring volume and mute), so they cannot route a
stream. Rostrum also does not ship a Lua script.

The fragments are rewritten when the default scene's rules change, or when another scene becomes
the default. Files are only written when their content changes. They are removed when there are no
rules. `pipewire-pulse` reads its fragment at startup, so new rules reach PulseAudio apps started
before Rostrum after the next login. Until then the router handles them.

After a reboot or re-login, the order is as follows:

1. Discord starts before Rostrum. Its stream asks for `rostrum.voice`, which does not exist yet, so
   WirePlumber falls back to the default sink.
2. Rostrum starts and creates the buses.
3. The router sets `target.object` for Discord's stream, and it moves to Voice.

If Discord stays on the default sink until it is restarted, that is a router bug.

## Meters

Each strip's meter is a passive `pw_stream` capture named `rostrum-meter.<node>`, targeted at the
node's serial. Playback buses and the masters are read from their monitor
(`stream.capture.sink`), the mic from the source itself. The realtime callback only keeps the
peak sample in an atomic. The UI takes it every 40 ms (80 ms with low meter speed), falls off at
20 dB/s and holds a clip mark for 1.5 s.

Meter streams exist only while the Mixer page is visible and the window is shown. They carry
`node.dont-fallback`, `node.dont-move` and `node.dont-reconnect` so a meter never wanders onto
another device. These keys are allowed here because meters are Rostrum's own internal streams
(`rostrum.internal = true`). They never appear in app rules.

## Files

All configuration is TOML under `$XDG_CONFIG_HOME/rostrum/` (default `~/.config/rostrum/`):

- `settings.toml`: general options, mixer options, default scene, saved headphone and mic
  `node.name`, shortcuts, window size, last page.
- `scenes/<slug>.toml`: one scene per file. The `name` inside the file wins over the file name.
  A scene holds master levels, sidetone level, the bus list (id, name, color, kind, volume, mute,
  destination) and app rules (match, key, bus, per-app volume, last seen).
- Export writes every scene into one TOML file with a `[[scene]]` array. Import never overwrites:
  clashing names get a numeric suffix.

Loading is forgiving. Unreadable files are skipped with a message. Missing fields take defaults.
The mic bus is always present and first, ids must be unique slugs, there are at most 12 buses, bad
colors are replaced from the palette, levels are clamped, and rules for unknown buses are dropped.

Fader moves make the scene dirty until Save. Bus renames, colors, adding or removing a bus and app
rules are written to the current scene file right away, merged onto its saved levels, so they
survive a quit without saving faders. Switching scenes discards unsaved fader moves.

The log is `$XDG_STATE_HOME/rostrum/rostrum.log`.

## Solo is never persisted

Solo is session-only state owned by the engine. It is never a field of `Bus` or `Scene` and never
written to TOML. Saving while soloed writes the user's own mute flags. A unit test enforces this.
Do not "fix" this by persisting solo.
