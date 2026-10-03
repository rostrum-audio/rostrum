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
