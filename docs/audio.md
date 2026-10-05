# Audio design

Rostrum owns a small, fixed PipeWire graph. It never runs a second audio daemon and never
shells out to `pactl` in the steady state.

```
App streams ----> rostrum.<bus> ----> rostrum.phones ----> headphones device
                         |
                         +--> rostrum.stream (virtual sink, "Rostrum Stream Mix")
                         |          |
                         |          +--> monitor  (OBS captures this on Track 1)
                         |
                         +--> rostrum.vod (virtual sink, "Rostrum VOD Mix")
                                    |
                                    +--> monitor  (OBS captures this on Track 2)

Hardware mic ---> rostrum.mic (virtual source, "Rostrum Mic")      (OBS captures this)
             \--> rostrum.sidetone ---> rostrum.phones             (optional, default off)
```

With mic filters on, the voice goes through one filter node first, and apps record its output:

```
Hardware mic ---> rostrum.micfx ---> rostrum.mic, rostrum.sidetone   (as above)
                                \--> rostrum.filtered ("Rostrum Filtered Mic")
                                         ^ app capture streams are moved here
```

## Nodes

Every Rostrum node is a `support.null-audio-sink` adapter created in the PipeWire daemon with
`pw_core_create_object("adapter", ...)`.

| Node | media.class | Channels | Description | Purpose |
|------|-------------|----------|-------------|---------|
| `rostrum.<bus>` | `Audio/Sink` | FL FR | `Rostrum <Name>` | One per playback bus. Apps are moved here. |
| `rostrum.phones` | `Audio/Sink` | FL FR | `Rostrum Headphones Mix` | Sum of everything bound for headphones. Volume = Master Headphones. |
| `rostrum.stream` | `Audio/Sink` | FL FR | `Rostrum Stream Mix` | Sum of everything bound for the stream. Volume = Master Stream. OBS captures its monitor on Track 1. |
| `rostrum.vod` | `Audio/Sink` | FL FR | `Rostrum VOD Mix` | Twitch VOD mix (playback buses with VOD enabled, default on except Music). Volume = Master Stream. OBS captures its monitor on Track 2. |
| `rostrum.mic` | `Audio/Source/Virtual` | MONO | `Rostrum Mic` | The hardware mic after Rostrum's gain and mute. OBS captures this. |
| `rostrum.sidetone` | `Audio/Sink` | MONO | `Rostrum Sidetone` | Mic monitoring into headphones. Volume = sidetone fader. |
| `rostrum.micfx` | none | MONO | `Rostrum Mic Filters` | Only while mic filters are on. Runs the filter chain (below). |
| `rostrum.filtered` | `Audio/Source/Virtual` | MONO | `Rostrum Filtered Mic` | Only while mic filters are on. The filtered voice for apps, at unity gain. |

Properties set on every node:

- `object.linger = true`: the node lives in the daemon, not in Rostrum. If Rostrum quits or
  crashes, audio keeps flowing. When Rostrum starts it adopts existing nodes by `node.name`
  instead of creating duplicates, and removes duplicates left by a race (oldest wins).
- `monitor.channel-volumes = true`: the node's volume and mute also apply to its monitor ports,
  which is what makes a bus fader or the Master Stream fader audible to OBS.
- `priority.session = 0`, `priority.driver = 0`: WirePlumber should not pick a Rostrum node as
  the system default device.
- `rostrum.role` (`bus`, `phones`, `stream`, `vod`, `mic`, `sidetone`, `micfx`, `filtered`) and
  `rostrum.bus`: Rostrum only ever destroys nodes that carry these.

`rostrum.micfx` is the exception to the null-sink rule: it is a bare `audio.convert` node made with
`spa-node-factory`, because a filter graph runs inside audioconvert. It lingers like the others.

Limitation and workaround: the spec asked for "loopback nodes". An in-process
`libpipewire-module-loopback` would put Rostrum's own process in the audio path, so a Rostrum
crash would cut the stream. Null-sink adapters plus explicit links give the same topology with the
processing inside the PipeWire daemon. Bus renames change the description only after
"Rebuild virtual devices", because a live node's `node.description` is fixed at creation. The
headphones mix and sidetone are the exception: only Rostrum links to them, so on start Rostrum
recreates either one whose description is out of date (the headphone mix drops for a moment).

## Destinations and links

Rostrum creates links port by port with `link-factory` (`object.linger = true`), matched by
`audio.channel`. The desired link set is recomputed on every graph change:

| From | To | When |
|------|----|------|
| `rostrum.<bus>` monitor | `rostrum.phones` | bus destination is Headphones or Both |
| `rostrum.<bus>` monitor | `rostrum.stream` | bus destination is Stream or Both |
| `rostrum.<bus>` monitor | `rostrum.vod` | bus destination is Stream or Both, and VOD is on |
| `rostrum.phones` monitor | headphone device | always; with mono headphones each side also goes into the other front channel |
| hardware mic | `rostrum.mic` | mic filters off (mute/destination act on the node, not the link) |
| hardware mic | `rostrum.sidetone` | mic filters off |
| hardware mic | `rostrum.micfx` | mic filters on |
| `rostrum.micfx` | `rostrum.mic`, `rostrum.sidetone`, `rostrum.filtered` | mic filters on |
| `rostrum.sidetone` monitor | `rostrum.phones` | always (muted unless sidetone is on) |

Rostrum only removes links it manages: links that start at a Rostrum node and end at a Rostrum node
or a hardware sink, and links into `rostrum.mic`, `rostrum.sidetone` and `rostrum.micfx`. App streams going into a bus
and OBS capturing a Rostrum monitor are never touched. A link you patch by hand from a Rostrum node
to a hardware sink will be removed.

Ports of a new device arrive one by one, so a node is only linked once it has as many ports as its
channel count. Otherwise a half-enumerated stereo device would briefly look mono.

### Volumes, mute and solo

Fader positions are perceptual: linear gain = position³, as in pavucontrol. They are sent as
`SPA_PROP_channelVolumes` and `SPA_PROP_mute` on the node's `Props` param.

- Bus node: fader, muted if the bus is muted or dimmed by solo. A playback bus with a balance gets
  one volume per channel (FL, FR): like PulseAudio's balance, the far side is turned down in fader
  space (position × (1 − |balance|)) and the near side stays at the fader.
- `rostrum.phones` / `rostrum.stream` / `rostrum.vod`: Master Headphones / Master Stream. These multiply every bus send.
  "Mute all playback to stream" mutes `rostrum.stream` and `rostrum.vod`.
- `rostrum.mic`: mic gain (0 to 150%, 100% = 0 dB), muted if the mic is muted or the mic
  destination does not include Stream.
- `rostrum.sidetone`: sidetone fader, muted unless the mic destination includes Headphones, the mic is
  live, and the fader is above zero.

If something else changes a Rostrum node's volume (WirePlumber's state restore, another mixer),
Rostrum re-applies the scene value at most once a second, so two tools cannot get into a loop.
Every channel is compared, so a balance changed elsewhere comes back too.

Balance is per bus (`balance` in the scene's `[[bus]]`, -1 left to 1 right, 0 = centre and not
written). It is a level like the fader: saved with the scene, and it makes the scene dirty. Values
outside -1..1 are clamped, anything that is not a number is centre, and the mono mic bus never has
one. Set it with the small slider under each playback fader; double-click it or use "Centre
Balance" in the strip's menu to reset.

### Mono headphones

Devices → "Mono headphones" (`[devices] mono_headphones`, off by default) gives both ears the whole
mix. It is done with links only, so the downmix happens in the PipeWire daemon like every other
mix: each `rostrum.phones` monitor port is linked into both front inputs (FL and FR) of the
headphone device, which PipeWire sums, and `rostrum.phones` is set to half volume (-6 dB) so a
centred sound stays at the same level. Master Headphones and the scene are unchanged; the halving is
applied at send time and never saved. While turning it on or off, the halving stays until the last
cross-link is gone, so the change can only be briefly quieter, never 6 dB louder. Mono sinks
already sum, and sinks without FL and FR ports keep the normal links. The stream mix stays stereo.
Like every Rostrum link, the cross-links linger, so headphones stay mono if Rostrum quits.

### Scene fades

Settings → General → Scene fade (`[general] scene_fade_ms`: 0 = off, the default, or 150, 300, 600,
1000; other values snap to the nearest) makes a scene switch glide instead of jump. Only bus and
master nodes fade; `rostrum.mic` and `rostrum.sidetone` switch at once, so a scene that mutes the mic
never leaves it live for a moment.

- The engine's scene takes the new values at once, so the faders show the target, the scene is
  never dirty because of a fade, and a save mid-fade saves the target. The ramp is a session-only
  overlay per node, never stored.
- Every 16 ms the overlay sends `from + (to − from) × t` in fader space (already perceptual), for
  the level and the balance. A muted bus counts as level 0: a bus being unmuted is unmuted at the
  start and ramps up from silence, and a bus being muted ramps down and is muted at the end. Solo
  is part of the starting level.
- A switch mid-fade starts from wherever the running fade got to. Moving a fader, mute or balance
  takes that node out of the fade at once; toggling solo ends the whole fade.
- Loading the default scene at startup never fades: the nodes may still be playing at their
  lingering levels, and the scene applies at once as before.
- Quitting mid-fade sends every node its scene level first, so nothing is left halfway.

### Auto-ducking

Settings → Ducking turns chosen playback buses down while someone speaks. Like solo, it is
session behaviour: the scene never changes, it never makes the scene dirty, and the ducked level is
never written anywhere. The settings live in `settings.toml`:

| Key | Default | Values |
|-----|---------|--------|
| `[ducking] enabled` | `false` | |
| `[ducking] trigger` | `"mic"` | `"mic"`, `"voice"` (the bus that receives voice chat) or `"either"` |
| `[ducking] buses` | `["music"]` | playback bus ids; the mic and unknown ids are ignored |
| `[ducking] amount_db` | `-12` | -6, -9, -12, -18, -24 |
| `[ducking] attack_ms` | `100` | 20, 50, 100, 250, 500 |
| `[ducking] release_ms` | `800` | 250, 500, 800, 1500, 3000 |

Hand-edited numbers snap to the nearest offered value. The amount stops at -24 dB on purpose: a
ducked bus is quieter, never silent.

- Detection: while ducking is on, the engine runs its own meter streams (the same `MeterBank` as
  the strips, independent of what is on screen). With the mic trigger it meters the hardware mic
  summed to mono, times the mic gain, and only while the mic can be heard on stream (not muted,
  destination includes Stream, not unplugged). With the voice trigger it meters the voice bus's
  monitor, which is after its fader and mute. A peak above -40 dBFS counts as speech. Turning
  ducking off removes the meter streams.
- The mic meter is an active source stream, like every mic meter, so while ducking listens to the
  mic, Plasma's microphone indicator stays lit, even with Rostrum in the tray. Muting the mic stops
  the meter and the indicator. The Settings switch says so.
- Envelope (`ducking::Envelope`, unit tested): every 20 ms the gain moves toward the amount at
  amount/attack dB per ms while speech is heard, and stays there until 500 ms after the last
  speech, so it does not pump between words. Then it returns to 0 dB at amount/release dB per ms.
- Applying: the gain multiplies the linear volume of each target bus when volumes are sent, on
  top of fader, balance and any scene fade. The value sent changes, the scene does not, and the
  once-a-second re-apply compares against the ducked value, so ducking never fights it. The bus
  that receives voice chat is never ducked when it is the trigger.
- The strip of a ducked bus says "Ducked" in its status line.
- Quitting while ducked sends the scene levels first and waits for PipeWire to confirm. After a
  crash a ducked bus stays down (by at most 24 dB) until Rostrum starts again.

### Mic channel handling

`rostrum.mic` and `rostrum.sidetone` are mono. Every channel of the hardware mic is summed into
them, because many USB interfaces expose a mono mic as stereo with signal on one side only. A true
dual-mono feed comes out 6 dB hotter, which the mic gain fader covers. A virtual source built from a
null sink (Easy Effects Source, for example) reports its capture ports as `port.monitor = true`.
Rostrum accepts those ports when a non-sink node has no other outputs.

### Devices

The headphone target is the saved `node.name` if present, otherwise the system default sink
(`default.audio.sink` metadata), otherwise the highest `priority.session` sink. Rostrum nodes are
never candidates. The mic works the same way with sources, except when a saved mic goes missing
(below). With no saved mic, Rostrum follows the system default source. The choice is a pure
function of the graph (`engine::resolveDevice`), covered by `tests/tst_engine.cpp`.

If the saved headphones disappear, the scene is held unchanged. `rostrum.phones` is relinked to the
fallback, the UI shows a banner, and one desktop notification is sent ("Headphones disconnected,
scene held"). When a node with the saved `node.name` returns, Rostrum relinks to it. The saved
device is never rewritten by a fallback.

If the saved mic disappears, the stream mic goes silent instead of falling back. Nothing is linked
into `rostrum.mic` or `rostrum.sidetone`, and both nodes are muted, so a webcam or laptop mic never
goes live on stream by surprise. The UI shows a banner ("Your stream mic is silent until it comes
back", with Choose Mic), the header mic button says No mic, and one desktop notification is sent
("Mic disconnected, stream mic silent"). The log says `mic missing: … stream mic silent until it
returns`. When the saved mic returns, it is relinked and its mute and gain come back from the
scene. Devices → "Use another mic while mine is unplugged" (`[devices] mic_fallback`, off by
default) brings back the fallback to the default source, the way headphones work. Either way the
saved mic is never rewritten.

## Mic filters

The Mic Filters page ("Clean up my mic", `[mic_filters] enabled`, off by default), the FX button on
the mic strip, the `toggle_mic_filters` action and D-Bus `SetMicFilters` switch one filter chain
on the mic. It is a setting, not part of a scene, so a scene switch never changes how the voice
sounds. The hardware mic and the system default source are never changed.

The chain, in signal order (`micfx::kModules`):

| Module | Default | What it does |
| --- | --- | --- |
| Rumble filter (`highpass`) | on, 80 Hz | High-pass, 12 dB/octave or 24 with "steep" |
| Noise removal (`denoise`) | on, 100 % | RNNoise, mixed with the dry signal by strength; optional voice threshold |
| Noise gate (`gate`) | off | Threshold, range, attack, hold, release |
| Tone (`eq`) | on | Low and high shelves (low, air), peaking mud cut and presence lift |
| Compressor (`compressor`) | on, 3:1 at −20 dB | Soft knee, makeup gain |
| Limiter (`limiter`) | on, −1 dBFS | Peak limiter with 1.5 ms lookahead |

Presets (Light, Streaming, Noisy room, Broadcast) set every module value and keep the scope, the app
choices and the master switch. The defaults are the Streaming preset; any other value shows as
Custom. Values are clamped and snapped to the page's steps when loaded (`micfx::sanitize`).

### The plugin

The modules are one LADSPA plugin, `librostrum-dsp.so` (`src/dsp/`, plain C). The PipeWire daemon
loads it itself, so the processing runs in the daemon like every other Rostrum mix, and a Rostrum
crash never touches it. The `run` callbacks allocate nothing and take no locks. Parameter changes
are smoothed, and a module switched off fades to bypass instead of clicking.

`ROSTRUM_RNNOISE` picks where noise removal comes from: `auto` (the default) uses a system rnnoise
found by pkg-config, else downloads the pinned RNNoise 0.2 release and compiles it in; `system`
and `bundled` force one or the other; `off` builds the plugin without noise removal and the page
hides that module. Official builds use `bundled`, because the AppImage's plugin is loaded into the
host's PipeWire process and may link nothing but libc and libm; `tools/appimage/build-appimage.sh`
refuses one with any other dependency. A bundled build installs RNNoise's licence as
`share/licenses/rostrum/RNNoise-COPYING`.

RNNoise works on 10 ms frames at 48 kHz, so noise removal adds 10 ms, and the dry path is delayed
to match. At any other graph rate it passes audio through. The other modules work at any rate.

Rostrum looks for the plugin in `ROSTRUM_DSP_PLUGIN`, then the build tree (a binary run from the
build directory), then `../lib/rostrum/` next to the binary, then the install path. An AppImage's
files vanish when it quits, and the daemon keeps the graph after that, so the AppImage copies its
plugin to `$XDG_DATA_HOME/rostrum/dsp/<hash>/` and removes older copies.

### The filter node

Mic filters need PipeWire 1.4 or newer (filter graphs in audioconvert) and `spa-node-factory`,
which PipeWire loads by default. Otherwise the page says why, and the mic works as before.

1. Rostrum creates `rostrum.micfx` and `rostrum.filtered`, and adopts them when they already exist
   (duplicates from a race are removed, oldest wins).
2. A new `audio.convert` node starts in convert mode with one port per side and no channel.
   Rostrum sends a mono DSP `PortConfig` for both directions, and only uses the node once it has
   `MONO` ports on both sides. If they do not show up within 4 s the filters fail (below).
3. The graph goes into the `audioconvert.filter-graph.0` property, once per session per node, with
   the current values as the initial controls. A node lingering from an earlier run gets the graph
   again, so it picks up this run's plugin, in the same pass that makes the links. Later changes
   are control values only (`rostrum_gate:Threshold` and so on), which change the sound without a
   glitch.
4. audioconvert builds the graph only while the node runs, that is while something records the mic.
   Audio passes through unfiltered until then, so the path is used at once.
5. Rostrum checks the graph loaded by reading the node's Props: index 0 holds audioconvert's own
   settings, index 1 and up each graph's controls. Props events may carry one index at a time, so
   the indexes are merged. PipeWire caches the params a node reports, and audioconvert refreshes
   them when a control is set but not when the graph starts. So while the node runs and the graph
   is not yet seen, Rostrum sends the controls every 250 ms, which brings it into view.
6. A running node that shows no graph after 4 s has failed: the filters go off for this session,
   the nodes are removed, and the page says "Your mic is used without filters". A graph that ran
   and then disappeared is sent again, keeping the links and app moves in place.

A control changed by another tool is set back at most once a second, like volumes. audioconvert
keeps whatever number of channel volumes it was last sent, so the channel count of every node is
taken from its channel map, not from its volumes.

### Which apps get the filtered mic

Apps record `rostrum.filtered` at unity gain. `rostrum.mic` (the stream) takes the filtered signal
and applies the mic gain after it. A mic gain above 100 % with the limiter on pushes the stream
past the ceiling, and the page warns about it.

`[mic_filters] scope` decides the default:

- `"all"` (the default, "The stream and every app"): every app that records the mic gets the
  filtered mic, except audio tools and recorders (the apps automatic assignment leaves alone:
  Audacity, DAWs, mixers, anything with the `production` media role), which keep the plain mic.
- `"stream"` ("Only the stream"): apps keep the plain mic.

Each app's switch on the page overrides that, saved by app key in `filtered_apps` or `raw_apps`.
A choice made while the app is not running is kept for next time.

The router moves a capture stream the same way it moves a playback stream: `target.object`
metadata with the serial of `rostrum.filtered`, after recording the previous target. Turning the
filters off, or an app to plain, restores that target or clears it. It moves:

- only streams that record the mic apps use (the saved mic, or the default source). A stream that
  picked another source keeps it.
- only once `rostrum.filtered` carries sound (mic, filter and filtered mic linked), so an app is
  never cut off for a moment.
- never OBS (the OBS plan decides what OBS records), Rostrum's own streams, streams that record a
  sink's monitor, or streams with `node.dont-move`.

Once moved, a stream the user moves elsewhere stays there. As with playback streams, a move within
5 s of Rostrum's is taken back up to 3 times. For capture streams that also covers Easy Effects
moving an app to Easy Effects Source before Rostrum got to it ("Process all input streams"): a
stream first seen less than 5 s ago on Easy Effects' source (`application.id` =
`com.github.wwmm.easyeffects`, or the node names `easyeffects_source` / `easyeffects_sink`) is
treated as recording the mic. The page shows an app Easy Effects holds as "Easy Effects moved it to
Easy Effects Source". WirePlumber remembers where a stream
was moved and puts the app back there when it next starts (by `application.name`, or `media.role`
when set). When an app set to plain comes back on the filtered mic that way, Rostrum clears the
target, which also makes WirePlumber forget it. An app that picked "Rostrum Filtered Mic" in its
own settings carries `target.object` in its own properties; WirePlumber and Rostrum leave it alone.

### Meters, ducking and OBS

While the filters run, the mic strip's meter and the ducking mic trigger read `rostrum.filtered`
times the mic gain, so they show what the stream hears, after noise removal. OBS treats
`rostrum.filtered` like the hardware mic: a source recording it doubles the voice that Rostrum Mic
already carries, so Set Up OBS points it at `rostrum.mic` or mutes it.

### Quitting, crashes and turning off

Like every Rostrum node, both nodes and their links linger. If Rostrum quits or crashes, apps keep
the filtered mic with the last settings, and the next start adopts the nodes without moving an
app. Turning the filters off moves apps back, links the hardware mic straight to `rostrum.mic` and
`rostrum.sidetone` again, and removes both nodes. While the mix is off they are left alone, like
the others.

If PipeWire goes away twice within 10 s of a graph load, the graph is taken to be crashing the
daemon: Rostrum turns the filters off, saves that, and sends one notification ("Mic filters
turned off"). They stay off until switched on again.

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
- The exception is a move within 5 s of Rostrum's own request. Easy Effects, with "Process all
  output streams" on (its default), moves every new stream to its own sink as the stream appears,
  so it and Rostrum race when an app starts. A move that soon is taken back, up to 3 times per
  stream, and logged as `reclaim … (moved by another program as it started)`. A program that keeps
  fighting wins after that; a later move is the user's choice and stays. To keep Easy Effects on
  what they hear, users choose Easy Effects Sink as the headphone device, so `rostrum.phones` feeds
  it.
- A stream placed on a bus but playing somewhere else bypasses the bus: its meter, fader and mute
  do not reach it. Its chip on the mixer and its row on the Apps page say so ("Not on this bus:
  another program moved it to Easy Effects Sink…"), and Move Back asks for the bus again, which
  also resets the reclaim count.
- Streams from Rostrum's own process (meters, test tones, the mic check) and any `rostrum.*` node
  are ignored.

### Per-app volume and mute

The Apps page sets a volume and a mute per app. Both go to the app's own stream node
(`SPA_PROP_channelVolumes` and `SPA_PROP_mute`), not to a bus. A stream is only touched once the
user has set something other than 100% or muted it, and Rostrum sends a value only when it changes,
so it does not fight a volume or mute set in another mixer.

Both are stored the same way: in the app's rule (`volume`, and `muted = true`, omitted when not
muted) when the app has one, so they belong to the scene and save with it; otherwise only until
Rostrum quits. Turning on Always carries the current volume and mute into the new rule. Rule
fragments never carry either: they only set `target.object`.

When Rostrum quits normally it unmutes every stream it muted and waits for PipeWire to confirm,
so quitting never leaves an app silent (and WirePlumber does not remember the app as muted). A
saved mute applies again when Rostrum starts. After a crash, a stream Rostrum muted stays muted
until it is unmuted in any mixer; WirePlumber may also restore that mute when the app restarts,
exactly as it does for a mute set in Plasma's volume applet.

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

### Automatic assignment

With **Assign apps automatically** on (the default), a stream that matches no rule is sorted by
what kind of app it is. Each bus has an `auto` key in the scene (`game`, `voice`, `music`,
`alerts`, `desktop` or `none`), and the stream goes to the bus that receives its kind. At most one
bus receives each kind, and the input bus never receives one. Scenes saved before this key existed
get it from the bus id, so the default buses work without editing anything.

The router decides where each stream goes in this order:

1. A "this launch only" assignment made in the app.
2. A rule in the current scene.
3. Automatic assignment, unless it is off or the user has taken this app off its bus before.

Automatic placements are live only. They are not written to the client rule fragments, because a
guess should not outlive Rostrum. Turning on "Always" for an automatic row saves a real rule, which
then also applies before Rostrum starts. Taking an app off its bus (or unassigning it) adds it to a
skip list in `settings.toml` (`[apps] auto_skip`). Assigning it to any bus again removes it from
the list. Settings → Apps → **Forget skipped apps** clears the list. Steam games are skipped by
Steam app id (`steam:<id>`), so skipping one Proton game does not skip all of `wine64-preloader`.

The classifier (`src/core/AppClassifier.cpp`) is a pure function of facts collected once per
stream and cached until the stream's properties change. It checks, in order:

| Step | Evidence | Result |
| --- | --- | --- |
| 1 | The stream carries its own `target.object` / `node.target` that is not a Rostrum node, the headphone device or the default sink, or sets `node.dont-move` | Excluded: the user picked this app's output in its own settings. Naming the headphones or the default is no choice: Java's OpenAL (Minecraft) and some SDL builds name the default device by itself |
| 2 | `media.role` is `Accessibility`, `Production` or `Test` | Excluded |
| 3 | The binary, `application.name` or Flatpak/Snap id is in the built-in catalog | Its kind; OBS, audio tools (pavucontrol, Helvum, qpwgraph, EasyEffects, Carla, DAWs) and screen readers are excluded |
| 4 | The app's menu entry lists it as a Mixer, Recorder, Sequencer or MIDI tool | Excluded |
| 5 | `SteamAppId` / `SteamGameId` in the process environment | Game, named from the Steam app manifest when the stream has no name |
| 6 | The binary is Wine or a `.exe` | Game |
| 7 | The icon name is in the catalog | Its kind |
| 8 | `media.role` is `Game`, `Music`, `Communication`/`Phone`, or a desktop role such as `Notification` or `Movie` | That kind |
| 9 | The app's `.desktop` entry: `Game`; `InstantMessaging`, `Chat`, `VideoConference`, `Telephony`; `Music` or `Audio`+`Player` | Game, Voice, Music; any other menu entry is Desktop |
| 10 | `application.name` is a game audio engine (OpenAL Soft, SDL, FMOD) | Game |

Anything else stays unassigned and plays on the default sink, as it would without Rostrum.

Process facts are read from `/proc/<pid>/environ` only after `/proc/<pid>/exe` (or `comm`) matches
`application.process.binary`. Flatpak apps report a pid from their own namespace, so an unchecked
read could belong to an unrelated process. Flatpak apps are still identified by
`pipewire.access.portal.app_id`. Desktop entries are indexed from `XDG_DATA_DIRS` plus the Flatpak
and Snap export directories. They are looked up by app id, binary, `Exec`, `TryExec`,
`StartupWMClass`, icon and name, and rescanned at most once a minute when a lookup misses.

App rows, saved rules and strip chips show the app's icon: for a Steam game `steam_icon_<appid>`,
then the desktop entry's `Icon=`, then `application.icon-name`, then the app id and the binary,
whichever the icon theme has first. A saved rule whose app is not running takes the icon of the
desktop entry its match finds. With none, a generic app icon.

The Apps page marks automatic rows **Auto** and states the evidence in plain words, and the strip
chip's tooltip repeats it. Each placement is logged as `recognised "<app>" <id> as "<kind>"` and
`route ... (automatic)`.

## Meters

Each strip's meter is a `pw_stream` capture named `rostrum-meter.<node>`, targeted at the
node's serial. Playback buses and the masters are read from their monitor
(`stream.capture.sink`). The Mixer's mic strip reads the hardware mic, with every channel summed
to mono as `rostrum.mic` does, and applies the mic gain itself. It does not read `rostrum.mic`,
because that node is muted whenever the mic's destination leaves out Stream, and the strip must
still show a voice that only goes to sidetone. Sink and app-stream meters are passive
(`node.passive = true`), because the audio playing through them already keeps them running. Mic
and other source meters are not passive. A passive link never wakes a suspended source, so before
anything else records the mic (the wizard, the Devices page) its meter would stay flat. While a
mic meter is on screen, Plasma's microphone indicator shows that Rostrum is listening. The meters
stop when the page is hidden. A watchdog checks every second: a meter whose stream fails, or a
source meter that gets no buffers for about 3 s, is torn down and rebuilt, and the first time
that happens for a node it is logged. The realtime callback only keeps the
peak sample in an atomic. The UI takes it every 40 ms (80 ms with low meter speed), falls off at
20 dB/s and holds a clip mark for 1.5 s.

The Apps page meters each running app the same way, by capturing the app's own playback stream
(targeted by node id, since app node names repeat). WirePlumber links a capture stream to a
playback stream's output ports without moving the app, so the app keeps playing where it was.
The Devices page and the wizard meter every input.

While mic filters run, the mic strip reads `rostrum.filtered` instead of the hardware mic, still
times the mic gain, so it shows the voice after the filters.

Auto-ducking has its own meters (below), which run whenever ducking is on, whatever is on screen.
Otherwise, meter streams exist only while the page that shows them is visible and the window is
shown. They carry
`node.dont-fallback`, `node.dont-move` and `node.dont-reconnect` so a meter never wanders onto
another device. These keys are allowed here because meters are Rostrum's own internal streams
(`rostrum.internal = true`). They never appear in app rules.

## Test tone

The Devices page and the wizard play a short chime on the chosen sink: four rising bell-like notes
(C5, E5, G5, C6) over about 1.4 s, peaking near −10 dBFS. The first note leans left and the second
leans right, so one press also shows that both ear cups work. It plays through an internal
`pw_stream` (`rostrum-test-tone`, `rostrum.internal = true`, `node.dont-move = true`), so neither
the router nor another program (Easy Effects, for one) moves it onto a bus or an effects sink.
`rostrum-graphtest --tone <sink>` plays the same chime from a terminal.

## Mic check

"Check Your Mic" on the Mic Filters and Devices pages, Check Mic in the header's mic popup, and the
`mic_check` action (no default shortcut) record 5 s of `rostrum.mic`, which is what the stream gets:
after the mic gain, the mute and the mic filters. Then the recording plays back once, mono to both
ears, on the headphone device in use (`rostrum-mic-check-record`, then `rostrum-mic-check-play`,
both internal with `node.dont-move`, `node.dont-fallback` and `node.dont-reconnect`). Playback goes
straight to the hardware sink and never to the default sink, which may be a Rostrum bus and so
reach the stream. A muted mic, no mic or no headphones refuses with a message instead of recording
silence.

The loudest sample picks the verdict: below −60 dBFS "nothing heard", below −24 dBFS "too quiet",
from −0.5 dBFS "too loud", anything else "good level". "Too loud" starts above the limiter's
default −1 dB ceiling, so a limiter doing its job is not called clipping. Started from a hotkey or
the command line, the start and the verdict show as on-screen feedback. The recording is kept in
memory only and dropped when playback ends.

"Hear yourself live" next to it is sidetone (the mic destination's Headphones half). Turned on at
zero volume, sidetone starts at fader position 0.5 so it is audible.

## OBS

OBS records `Rostrum Mic` (`rostrum.mic`) on Track 1 and Track 2, `Rostrum Stream Mix` (PulseAudio name `rostrum.stream.monitor`) on Track 1 only, and `Rostrum VOD Mix` (`rostrum.vod.monitor`) on Track 2 only (the Twitch VOD track, containing all playback buses with VOD enabled, excluding Music by default). When streaming to Twitch, Output → Streaming → Twitch VOD Track must be set to Track 2, and stream readiness stays "Needs attention" until that output setting is set. Any OBS audio source type works, PulseAudio or the
PipeWire plugin, as long as it records one of those. The failures come from sources that record
something else: "Default" or the headphones (everything you hear, including buses you keep off
stream), the hardware mic or `Rostrum Filtered Mic` (a doubled voice), or one app (that app
doubled, its bus ignored).

### Status

The OBS page reads what OBS records from the PipeWire graph, with no help from OBS: capture streams
owned by OBS, and the node at the other end of each one's links. This works without obs-websocket.
It only shows sources that are running, which is also how it catches a mismatch. If OBS's settings
say `Rostrum Mic` but PipeWire links the source to the hardware mic, something moved it. Easy
Effects does this to every recording app unless OBS is on its excluded list.

### One-click setup

The plan is the same whether OBS is running or not (`makePlan` in `src/obs/ObsPlan.cpp`):

1. Mic: if an unmuted source already records `rostrum.mic`, keep it. If only a muted one does,
   unmute it. Otherwise point the source that records the hardware mic at `rostrum.mic`, which
   keeps its filters, tracks and scenes. Global Mic/Aux comes first. Failing that, add one. Configured for OBS Track 1 and Track 2.
2. Stream mix: the same, preferring the global Desktop Audio source, configured for OBS Track 1 only.
3. VOD mix: add a named Audio Output Capture, "Rostrum VOD Mix" (device `rostrum.vod.monitor`), configured for OBS Track 2 only. Desktop Audio 2 is never assigned.
4. Output setting: when the stream service is Twitch, Output → Streaming → Twitch VOD Track is set to 2.
5. Every other unmuted source that records audio Rostrum handles (hardware mic, headphones,
   Default, one app, a Rostrum bus) is muted. These steps are optional in the preview. Nothing is
   ever deleted.

With OBS running, Rostrum talks obs-websocket 5 on `127.0.0.1`. The port and password come from
OBS's own config (`plugin_config/obs-websocket/config.json`). Native, Flatpak and Snap installs
are found, newest first. A new source is added to every scene, because OBS has no API to turn on a
global Desktop Audio device. If OBS refuses a step, the steps already done stay undoable.

With OBS closed, Rostrum edits the active scene collection JSON directly (global Desktop Audio and
Mic/Aux channels included). First it writes a timestamped `.rostrum-….bak` copy next to the file.
It checks again that OBS is not running just before writing, because OBS overwrites the file when
it quits.

Each applied step records its inverse in `$XDG_STATE_HOME/rostrum/obs-undo.json`, so Undo restores
the previous device, mute state, and removes what was added. That works live or offline,
whichever OBS state applies at the time.

Trade-off: OBS now gets a single stream mix, so per-scene audio in OBS (a source muted in one scene)
moves to Rostrum scenes.

`rostrum-obs` prints the same status and, with `--plan`, the plan, without changing anything.

### While OBS runs

With "Follow OBS while it runs" on (`[obs] background`, default on), Rostrum keeps the same
obs-websocket connection open while OBS runs, even with the OBS page closed. Every 5 seconds it
checks whether an OBS process is running and its WebSocket server is turned on in OBS's config.
Only then does it connect, to `127.0.0.1` only. A refused connection waits 5 seconds, then 10,
20, 40 and at most 60 before the next try. Starting OBS, or opening the OBS page, resets that. The
background connection shows no toasts and writes nothing to the log. Offscreen runs
(`QT_QPA_PLATFORM=offscreen` or `ROSTRUM_SCREENSHOT`) never connect in the background.

It subscribes to the Outputs and Scenes events (`StreamStateChanged`, `RecordStateChanged`,
`CurrentProgramSceneChanged`, scene list changes), and asks `GetStreamStatus`,
`GetRecordStatus`, `GetCurrentProgramScene` and `GetSceneList` when it connects. The background
connection only reads. OBS is changed only by Set Up OBS and Undo. `obs::LiveStatus` in
`src/obs/ObsStatus.cpp` holds the result and clears it when OBS goes away, so a LIVE badge never
outlives OBS.

- **Badges.** The header shows a red LIVE badge and a REC badge (REC paused while paused) with the
  elapsed time, counted from the duration OBS reports. Clicking one opens the OBS page. Screen
  readers hear "Live on stream for …" and "Recording for …". The tray tooltip starts with "LIVE
  since 20:04" and "REC since 20:10": a start time, because the tray host only hears about changes.
- **Go-live warnings** (`[obs] go_live_warnings`, default on). When a stream starts, or when
  Rostrum connects to a stream already running, it checks the current scene
  (`obs::goLiveProblems`). It warns if the mic is muted, at zero, or its destination leaves out
  Stream. It warns if Master Stream is muted or at zero, or if no unmuted playback bus with a
  level above zero and a Stream destination is left, solo included. It also warns if OBS records
  nothing from `Rostrum Stream Mix` or `Rostrum Mic`, read from the PipeWire graph as on the OBS
  page. One banner and one desktop notification are shown per stream start. A problem drops off
  the banner once it is fixed, and the banner goes away when the stream stops. A problem that
  appears later in the stream is not reported.
- **Scene mapping** (`[obs.scene_map]`, OBS scene name = Rostrum scene name). The OBS page lists
  OBS's scenes, plus mapped ones OBS no longer has, so they can be forgotten. When OBS puts a
  mapped scene on program, Rostrum switches straight to the Rostrum scene, as a hotkey does but
  without the confirm dialog: blocking a switch while live would be worse than losing unsaved
  levels. With auto-save on, the current scene is saved first. With it off, unsaved levels are
  dropped, and the page says so. A toast names both scenes. Nothing happens at connect, only on a
  change, and a mapping to a Rostrum scene that no longer exists does nothing.

## Files

All configuration is TOML under `$XDG_CONFIG_HOME/rostrum/` (default `~/.config/rostrum/`):

- `settings.toml`: general options, mixer options, ducking, mic filters, default scene, saved headphone and
  mic `node.name`, shortcuts, window size, last page, OBS options and the OBS scene map
  (`[obs.scene_map]`).
- `scenes/<slug>.toml`: one scene per file. The `name` inside the file wins over the file name.
  A scene holds master levels, sidetone level, the bus list (id, name, color, kind, volume, mute,
  balance, destination) and app rules (match, key, bus, per-app volume and mute, an optional label for apps
  that report no name, last seen).
- Export writes every scene into one TOML file with a `[[scene]]` array. Import never overwrites:
  clashing names get a numeric suffix.

Loading is forgiving. Unreadable files are skipped with a message. Missing fields take defaults.
The mic bus is always present and first, ids must be unique slugs, there are at most 12 buses, bad
colors are replaced from the palette, levels are clamped, and rules for unknown buses are dropped.

Bus renames, colors, adding or removing a bus and app rules are written to the current scene file
right away, merged onto its saved levels. Levels (faders, mutes, balance, destinations, masters,
sidetone) are saved to the live scene one second after the last change, before a scene switch and
on quit, while "Save scene changes automatically" is on (the default; `[general] auto_save_scenes`).
With it off, level changes make the scene dirty until Save, and switching scenes discards them.

New → From a Preset makes a scene from the live one: same buses, names, colors and app rules,
with the preset's levels for buses that have an automatic category (Game, Voice, Music, Alerts,
Desktop). Masters go to 0 dB, the mic keeps its gain (only Be Right Back mutes it), and buses
without a category keep their levels. The presets live in `src/core/ScenePresets.cpp`.

The log is `$XDG_STATE_HOME/rostrum/rostrum.log`.

Outside `~/.config/rostrum/`, Rostrum writes only the two PipeWire rule fragments above and,
while "Launch at login" is on, `$XDG_CONFIG_HOME/autostart/dev.getrostrum.Rostrum.desktop`
(`Exec=… --autostart`, pointing at `$APPIMAGE` when running from an AppImage, as Restart does;
"Start in tray" only applies to that launch). That file is the source of
truth: removing it in System Settings → Autostart turns the switch off. An AppImage with mic
filters on also keeps a copy of the filter plugin in `$XDG_DATA_HOME/rostrum/dsp/`. Set Up OBS, when pressed
with OBS closed, edits OBS's scene collection after backing it up.

## Desktop integration

- Tray: a StatusNotifierItem, shown only when a tray host is registered
  (`org.kde.StatusNotifierWatcher`). Closing the window hides it to the tray (`[general]
  close_to_tray = true`); off, or without a tray, closing quits. With `minimize_to_tray = true`
  (default off), minimizing hides the window to the tray too. X11 reports minimizing as a window
  state; Wayland does not, so there Rostrum takes "the active window stopped being shown" as a
  minimize. A switch to another virtual desktop also stops showing it, but takes focus away
  first, so it is left alone. Minimizing an inactive window (from a task bar menu) just
  minimizes.
  The menu has Show or Hide Rostrum, Mute Mic, Mute Stream, Previous Scene, Next Scene, a Scenes submenu, Restart Rostrum
  and Quit.
- Restart (tray, or Settings → General): quits as Quit does, then starts the same program again
  with `--restart-after=<pid>`. The new copy waits up to 10 s for the old process to exit before
  it takes the D-Bus name or touches PipeWire, so the two never overlap. A copy restarted from
  the tray while the window was hidden gets `--start-hidden` and stays in the tray. An AppImage
  restarts as `$APPIMAGE`; a binary replaced by a reinstall restarts as the new one. Self-updates
  restart the same way.
  Middle-click toggles the mic; scrolling moves the Stream master 2 % per wheel step. The tooltip
  adds "Stream muted" while it is. Tray scene changes go through the same confirm dialog as the
  window.
- Global shortcuts: through KGlobalAccel when `org.kde.kglobalaccel` is running (Plasma), as the
  component `dev.getrostrum.Rostrum`, so they show up in System Settings → Keyboard → Shortcuts and
  can be rebound there too. Otherwise through the XDG GlobalShortcuts portal. A shortcut the
  desktop refuses, or one another component already owns, stays active inside the window and
  Settings says why. There is no X11 key grab.
- Actions (`[hotkeys]` keys, `src/core/Settings.cpp`): `mute_mic`, `mute_stream`,
  `previous_scene`, `next_scene`, `scene_1` … `scene_8`, `push_to_talk`, `push_to_mute`,
  `panic_mute`, `toggle_sidetone`, `mute_headphones`, `stream_volume_up`, `stream_volume_down`
  (±5 % on the Stream master), `toggle_mic_filters`, `mic_check` (start or stop a mic check), and
  `mute_bus_<bus id>` for every playback bus of the saved scenes.
  Only the first eight have default shortcuts. `scene_<n>` loads the n-th scene in the Scenes page
  order (`[scenes] scene_order`), which the header, the tray, Previous/Next and `ListScenes` share.
  A bus action whose bus is not in the live scene says so and does nothing.
- Push to talk, push to mute and panic are holds in the engine, like solo: session-only, never in
  TOML, never make the scene dirty. They change what reaches PipeWire, not the scene's mute
  flags. Push to talk unmutes a muted mic while held; push to mute mutes it while held. Panic mutes
  the mic and the Stream master; pressing it again lifts both holds, so the mic and stream come
  back to what the scene says. Panic outlives a scene switch. Choosing mute or unmute for the mic
  or the stream directly (button, tray, hotkey, D-Bus) clears the holds that contradict it. On
  quit, holds are dropped and the scene's own mutes are applied before Rostrum disconnects.
- Releasing a hold: KGlobalAccel reports the keys going up (`globalShortcutActiveChanged`), and
  so does the portal (`Deactivated`). A desktop that never reports the release makes a second
  press end the hold. Inside the window each press turns a hold on or off. Over D-Bus,
  `PressAction`/`ReleaseAction` hold, and a hold is dropped when the caller leaves the bus.
- On-screen feedback: when the window is not in front, a hotkey or D-Bus change to the mic, the
  stream, panic or the scene shows Plasma's OSD (`org.kde.plasmashell /org/kde/osdService
  showText`). Without plasmashell, a transient notification (`org.freedesktop.Notifications`,
  urgency low, 2 s, replacing the previous one). Holds show nothing. Off with `[general]
  osd_feedback = false` (Settings → General → "Show hotkey changes on screen").
- Other notifications, sent straight to `org.freedesktop.Notifications`: "Headphones
  disconnected, scene held.", "Mic disconnected, stream mic silent." (or "Mic disconnected." when
  another mic stands in), and "Check your stream audio" when a stream starts with a problem
  (critical urgency; see OBS above). Changes made in the window never notify.
- Offscreen runs (`QT_QPA_PLATFORM=offscreen` or `ROSTRUM_SCREENSHOT`) skip the tray, shortcuts,
  notifications and on-screen feedback. `ROSTRUM_NO_GLOBAL_SHORTCUTS=1` skips only the shortcuts.

### D-Bus and the command line

The running instance owns `dev.getrostrum.Rostrum` (KDBusService, which also holds
`/dev/getrostrum/Rostrum`) and exports `dev.getrostrum.Rostrum1` at
`/dev/getrostrum/Rostrum/Control`. `data/dev.getrostrum.Rostrum1.xml` is the reference and is
installed to `share/dbus-1/interfaces/`; `tests/dbus-control.sh` checks the live interface against
it.

| Member | What it does |
| --- | --- |
| `TriggerAction(s id)` | Runs an action as a shortcut press. Hold actions are refused (`Error.HoldAction`) |
| `PressAction(s id)`, `ReleaseAction(s id)` | Start and end a hold (or press any other action) |
| `SwitchScene(s name)` | Switches at once, name matched without case |
| `SetBusVolume(s bus, d position)` | Fader travel 0–1, mic gain 0–1.5 |
| `SetBusMuted(s bus, b)`, `ToggleBusMuted(s bus)` | Mute a bus, the mic or a master |
| `SetMicMuted(b)`, `ToggleMicMute()` | The mic, as the header button |
| `SetMicFilters(b)` | Mic filters on or off, saved (`toggle_mic_filters` toggles) |
| `ListScenes() → as`, `ListBuses() → a(ssdb)`, `ListActions() → a(ss)` | Scene names; id, name, position, muted; id, label |
| Properties `MicMuted`, `StreamMuted`, `Panic`, `CurrentScene`, `Connected`, `MicFilters` | Read-only, with `PropertiesChanged` |

Bus arguments take a bus id or a bus name (any case), `mic`, and `stream` or `phones` for the
masters; those two can never be bus ids. Errors are `dev.getrostrum.Rostrum1.Error.UnknownAction`,
`NoSuchScene`, `NoSuchBus`, `InvalidValue` and `HoldAction`. Calls never open a dialog: with
"Confirm scene switch" on, a D-Bus or command-line switch still happens at once, and with "Save
scene changes automatically" off, unsaved moves in the scene being left are dropped, as the
dialog's Switch button does. A script has no one to answer a dialog.

`rostrum --scene`, `--action` (repeatable), `--mute-mic`, `--unmute-mic`, `--toggle-mic` and
`--set-volume bus=level` call these methods on the running instance and exit 0 (done), 1
(refused, with the reason on stderr) or 2 (unreachable). `--list-scenes`, `--list-actions` and
`--list-buses` print plain lines (tab-separated fields) and, with no instance running, read the
saved scenes without writing anything. A control option with no instance running starts Rostrum
and applies it once the scenes are loaded. Nothing listens on the network: D-Bus is local to the
session.

## Solo is never persisted

Solo is session-only state owned by the engine. It is never a field of `Bus` or `Scene` and never
written to TOML. Saving while soloed writes the user's own mute flags. A unit test enforces this.
Do not "fix" this by persisting solo.

The same holds for undo (`SceneHistory`): each step is a whole `Scene`, so solo, holds, ducking
and scene fades are never in it, and the mic mute is taken from the live scene on every undo and
redo. Restoring a step cancels a running scene fade and leaves holds and ducking as they are.
