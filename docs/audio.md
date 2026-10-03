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
| `rostrum.phones` | `Audio/Sink` | FL FR | `Rostrum Headphones Mix` | Sum of everything bound for headphones. Volume = Master Headphones. |
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
| `rostrum.phones` monitor | headphone device | always; with mono headphones each side also goes into the other front channel |
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

- Bus node: fader, muted if the bus is muted or dimmed by solo. A playback bus with a balance gets
  one volume per channel (FL, FR): like PulseAudio's balance, the far side is turned down in fader
  space (position × (1 − |balance|)) and the near side stays at the fader.
- `rostrum.phones` / `rostrum.stream`: Master Headphones / Master Stream. These multiply every bus send.
  "Mute all playback to stream" mutes `rostrum.stream`.
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
| 1 | The stream carries its own `target.object` / `node.target` that is not a Rostrum bus, or sets `node.dont-move` | Excluded: the user picked this app's output in its own settings |
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
`pw_stream` (`rostrum-test-tone`, `rostrum.internal = true`), so the router never moves it onto a
bus. `rostrum-graphtest --tone <sink>` plays the same chime from a terminal.

## OBS

OBS should record exactly two things: `Rostrum Mic` (`rostrum.mic`) and `Rostrum Stream Mix`
(PulseAudio name `rostrum.stream.monitor`). Any OBS audio source type works, PulseAudio or the
PipeWire plugin, as long as it records one of those. The failures come from sources that record
something else: "Default" or the headphones (everything you hear, including buses you keep off
stream), the hardware mic (a doubled voice), or one app (that app doubled, its bus ignored).

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
   keeps its filters, tracks and scenes. Global Mic/Aux comes first. Failing that, add one.
2. Stream mix: the same, preferring the global Desktop Audio source. A new source gets the audio
   tracks of the desktop or app source it replaces.
3. Every other unmuted source that records audio Rostrum handles (hardware mic, headphones,
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

- `settings.toml`: general options, mixer options, ducking, default scene, saved headphone and
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
truth: removing it in System Settings → Autostart turns the switch off. Set Up OBS, when pressed
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
  (±5 % on the Stream master), and `mute_bus_<bus id>` for every playback bus of the saved scenes.
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
| `ListScenes() → as`, `ListBuses() → a(ssdb)`, `ListActions() → a(ss)` | Scene names; id, name, position, muted; id, label |
| Properties `MicMuted`, `StreamMuted`, `Panic`, `CurrentScene`, `Connected` | Read-only, with `PropertiesChanged` |

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
