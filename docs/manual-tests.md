# Manual graph tests

These need a running PipeWire + WirePlumber session, so they are not part of `ctest`.
Run them on the development machine (Kubuntu 26.04, Plasma Wayland) after graph changes.

`rostrum-graphtest` drives the same engine as the app without the UI:

```sh
B=./build/tools/rostrum-graphtest/rostrum-graphtest
$B --teardown          # remove every Rostrum node
$B --seconds 0         # create/adopt the mix and keep running until Ctrl-C
```

## 1. Virtual buses

1. `$B --teardown`, then `wpctl status | grep Rostrum` prints nothing.
2. `$B --seconds 3`. It logs `creating` for nine nodes and prints `Mix ready.`
3. `wpctl status | grep Rostrum` lists, under Sinks: Rostrum Game, Voice, Music, Alerts, Desktop,
   Sidetone, Stream Mix, Phones Mix; under Sources: Rostrum Mic.
4. The nodes are still listed after the tool exits (`object.linger`).
5. Run `$B --seconds 2` again. It logs no `creating` lines, and the count from step 3 is unchanged
   (existing nodes are adopted, not duplicated).
6. `$B --teardown` removes them again.

## 2. Move a stream

Use a tone file (any 30–60 s WAV) and target a bus first, so the test is silent until step 5.

1. `$B --seconds 2` (create the mix), then `pw-play --target rostrum.desktop tone.wav &`.
2. `pw-link -l` shows `pw-play:output_FL -> rostrum.desktop:playback_FL`.
3. `$B --list-apps --rule name:pw-play=game --unassign-after 4 --seconds 7`.
   It prints `route "pw-play" … -> "rostrum.game"` and the app's `target.object` becomes the
   serial of `rostrum.game`.
4. While routed, `pw-link -l` shows `pw-play -> rostrum.game`, and
   `pw-record -P '{ stream.capture.sink = true }' --target rostrum.game g.wav` has signal
   (RMS well above 0.01). The same capture on `rostrum.voice` is silent.
5. After the unassign, `target.object` is cleared and `pw-link -l` shows `pw-play -> rostrum.desktop`
   again (previous target restored).
6. Bus re-creation: with the rule active, run `$B --teardown`, then `$B --rule name:pw-play=game`.
   The stream is moved onto the new `rostrum.game` as soon as it appears, without restarting
   pw-play. While the bus is gone, WirePlumber falls back to the default sink (Rostrum never sets
   `node.dont-fallback`).

## 3. Destinations, solo, mic, sidetone (scripted)

`tests/manual/graph-checks.sh` runs these against a fake headset (`rostrumtest.headset`, a null
sink) and a fake mic (`rostrumtest.mic`, a virtual source fed by a tone), so nothing reaches real
speakers. RMS is computed from `pw-record` captures. Expected output on the development machine:

```
game=both  headset: 0.0424  stream mix: 0.0424
game=phones  headset: 0.0424  stream mix: 0.0000
game=stream  headset: 0.0000  stream mix: 0.0424
solo voice  headset: 0.0000  stream mix: 0.0000
mic live:  Rostrum Mic: 0.0424  stream mix: 0.0000  headset (sidetone off): 0.0000
    rostrumtest.mic:capture_MONO
      |-> rostrum.sidetone:playback_MONO
      |-> rostrum.mic:input_MONO
mic muted: Rostrum Mic: 0.0000
sidetone:  headset: 0.0217  stream mix: 0.0000
```

Any non-zero value is signal; 0.0000 is digital silence. Sidetone at 0.8 reads 0.8³ × 0.0424.

## 4. Mic path (explicit, real hardware)

A doubled voice is the most likely real-world failure: OBS captures the headset mic directly and
also captures `Rostrum Mic`. Run this with the real mic and OBS.

1. **Graph.** Start Rostrum with the real mic selected on the Devices page. `pw-link -l` shows the
   hardware source's capture ports (for example
   `alsa_input.usb-…:capture_FL` / `capture_FR`) linked into `rostrum.mic:input_MONO`, and into
   `rostrum.sidetone:playback_MONO`. Nothing else from Rostrum is linked to the hardware source.
2. **Signal.** Speak. `pw-record --target rostrum.mic mic.wav` has signal. Mute the mic from the
   header: the same capture is silent. Move the mic gain fader: the level changes.
3. **Not in the stream mix.** With nothing playing, speak and capture the stream mix:
   `pw-record -P '{ stream.capture.sink = true }' --target rostrum.stream s.wav`. It must be
   silent. The mic reaches OBS only through `Rostrum Mic`.
4. **OBS captures `Rostrum Mic` only.**
   1. In OBS → Settings → Audio, set every global "Mic/Auxiliary Audio" device to Disabled.
   2. Add an *Audio Capture (PipeWire)* source (or *Audio Input Capture*) and pick
      **Rostrum Mic**. Its meter in the OBS Audio Mixer moves when you speak.
   3. Make sure no other source captures the headset mic directly (no second Audio Input Capture
      on the hardware device, and no desktop-wide capture).
   4. Speak with no playback: exactly one mic-driven meter moves in the OBS Audio Mixer. If two
      move, the voice will be doubled on stream. Remove the extra capture.
5. **Sidetone.** Turn sidetone on and raise its fader: you hear yourself in the headphones. The
   stream-mix capture from step 3 is still silent.

## 5. Headphones unplugged

1. Run `$B --headphones <your headset node.name>` and unplug the headset (or, with fakes, destroy
   `rostrumtest.headset` with `pw-cli destroy <id>`).
2. It prints `Headphones disconnected (…), scene held. Falling back to <default sink>`, and
   `rostrum.phones` is linked to the default sink.
3. Plug it back in (or recreate the fake). It prints `Headphones back`, `rostrum.phones` is linked
   to the headset again, and the fallback links are removed. The scene was never modified.

## 6. Quitting Rostrum falls back to the default sink

1. Assign Discord (or any playing app) to Voice with "Always". Check that
   `~/.config/pipewire/pipewire-pulse.conf.d/50-rostrum.conf` lists it with only `target.object`.
2. Quit Rostrum and tear the buses down (`$B --teardown`).
3. Discord keeps playing, now through the default sink (`wpctl status` shows its stream linked
   there). It is never silent.
4. `rg 'dont-(fallback|move|reconnect)' ~/.config/pipewire` finds nothing.

## 7. Reboot restores routing

1. Keep the rule from test 6, enable "Launch at login", and reboot.
2. Start Discord before Rostrum starts (or turn autostart off, log in, start Discord, and then start
   Rostrum).
3. Before Rostrum runs, Discord plays through the default sink.
4. Within a second of Rostrum starting, Discord's stream is linked to `rostrum.voice`
   (`pw-link -l | rg -A2 -i discord`), without restarting Discord. If Discord stays on the default
   sink until it is restarted, that is a router bug. Fix it in the router, not with a WirePlumber
   script.
5. Quit Rostrum again: Discord falls back to the default sink, as in test 6.

The fragment matching itself can be checked without logging out. Point `XDG_CONFIG_HOME` at a
directory containing `pipewire/client.conf.d/50-rostrum.conf`, then run
`pw-play -P '{ application.name = "firefox" }' file.wav`. The stream's `target.object` (shown by
`pw-dump`) is the bus from the rule.
