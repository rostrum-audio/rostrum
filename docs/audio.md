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

## Solo is never persisted

Solo is session-only state owned by the engine. It is never a field of `Bus` or `Scene` and never
written to TOML. Saving while soloed writes the user's own mute flags. A unit test enforces this.
Do not "fix" this by persisting solo.
