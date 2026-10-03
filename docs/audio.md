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
"Rebuild virtual devices", because a live node's `node.description` is fixed at creation.

## Destinations and links

Rostrum creates links port by port with `link-factory` (`object.linger = true`), matched by
`audio.channel`. The desired link set is recomputed on every graph change:

| From | To | When |
|------|----|------|
| `rostrum.<bus>` monitor | `rostrum.phones` | bus destination is Headphones or Both |
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
- `rostrum.phones` / `rostrum.stream`: Master Headphones / Master Stream. These multiply every bus send.
  "Mute all playback to stream" mutes `rostrum.stream`.
- `rostrum.mic`: mic gain (0 to 150%, 100% = 0 dB), muted if the mic is muted or the mic
  destination does not include Stream.
- `rostrum.sidetone`: sidetone fader, muted unless the mic destination includes Headphones, the mic is
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
the list. Settings → Apps → **Forget Skipped Apps** clears the list. Steam games are skipped by
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

Meter streams exist only while the page that shows them is visible and the window is shown. They carry
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

## Files

All configuration is TOML under `$XDG_CONFIG_HOME/rostrum/` (default `~/.config/rostrum/`):

- `settings.toml`: general options, mixer options, default scene, saved headphone and mic
  `node.name`, shortcuts, window size, last page.
- `scenes/<slug>.toml`: one scene per file. The `name` inside the file wins over the file name.
  A scene holds master levels, sidetone level, the bus list (id, name, color, kind, volume, mute,
  destination) and app rules (match, key, bus, per-app volume, an optional label for apps that
  report no name, last seen).
- Export writes every scene into one TOML file with a `[[scene]]` array. Import never overwrites:
  clashing names get a numeric suffix.

Loading is forgiving. Unreadable files are skipped with a message. Missing fields take defaults.
The mic bus is always present and first, ids must be unique slugs, there are at most 12 buses, bad
colors are replaced from the palette, levels are clamped, and rules for unknown buses are dropped.

Bus renames, colors, adding or removing a bus and app rules are written to the current scene file
right away, merged onto its saved levels. Levels (faders, mutes, destinations, masters, sidetone)
are saved to the live scene one second after the last change, before a scene switch and on quit,
while "Save scene changes automatically" is on (the default; `[general] auto_save_scenes`). With
it off, level changes make the scene dirty until Save, and switching scenes discards them.

New → From a Preset makes a scene from the live one: same buses, names, colors and app rules,
with the preset's levels for buses that have an automatic category (Game, Voice, Music, Alerts,
Desktop). Masters go to 0 dB, the mic keeps its gain (only Be Right Back mutes it), and buses
without a category keep their levels. The presets live in `src/core/ScenePresets.cpp`.

The log is `$XDG_STATE_HOME/rostrum/rostrum.log`.

Outside `~/.config/rostrum/`, Rostrum writes only the two PipeWire rule fragments above and,
while "Launch at login" is on, `$XDG_CONFIG_HOME/autostart/dev.getrostrum.Rostrum.desktop`
(`Exec=… --autostart`; "Start in tray" only applies to that launch). That file is the source of
truth: removing it in System Settings → Autostart turns the switch off. Set Up OBS, when pressed
with OBS closed, edits OBS's scene collection after backing it up.

## Desktop integration

- Tray: a StatusNotifierItem, shown only when a tray host is registered
  (`org.kde.StatusNotifierWatcher`). Without a tray, closing the window quits instead of hiding.
- Global shortcuts: through KGlobalAccel when `org.kde.kglobalaccel` is running (Plasma), as the
  component `dev.getrostrum.Rostrum`, so they show up in System Settings → Shortcuts and
  can be rebound there too. Otherwise through the XDG GlobalShortcuts portal. A shortcut the
  desktop refuses, or one another component already owns, stays active inside the window and
  Settings says why. There is no X11 key grab.
- The one notification is "Headphones disconnected, scene held.", sent straight to
  `org.freedesktop.Notifications`. Mute changes never notify.
- Offscreen runs (`QT_QPA_PLATFORM=offscreen` or `ROSTRUM_SCREENSHOT`) skip the tray, shortcuts
  and notifications. `ROSTRUM_NO_GLOBAL_SHORTCUTS=1` skips only the shortcuts.

## Solo is never persisted

Solo is session-only state owned by the engine. It is never a field of `Bus` or `Scene` and never
written to TOML. Saving while soloed writes the user's own mute flags. A unit test enforces this.
Do not "fix" this by persisting solo.
