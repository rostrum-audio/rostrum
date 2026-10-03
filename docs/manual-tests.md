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
   pw-play.
