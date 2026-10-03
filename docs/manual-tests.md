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
   Sidetone, Stream Mix, Headphones Mix; under Sources: Rostrum Mic.
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
   5. Repeat sub-step 4 after Set Up OBS on the OBS page (test 10) instead of the manual setup.
      The result must be the same: one mic meter, and it belongs to a source on Rostrum Mic.
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

## 8. Mixer page

1. Start Rostrum and play something from Firefox. Its chip appears on the bus its rule names,
   or on none if it has no rule.
2. Drag the chip onto Music. The chip moves, `pw-link -l | rg -A2 -i firefox` shows the stream
   linked to `rostrum.music`, and the scene file gains a `[[rule]]` for it.
3. Drag the chip onto the empty space between the strips. The chip disappears and the stream goes
   back to the default sink.
4. The Music meter moves with the audio and falls off smoothly after you pause. The Mic meter
   moves while you speak. Mute Music: its meter drops to zero and the strip says "Muted".
5. Solo Game: every other playback strip dims and says "Dimmed by solo". Quit and restart:
   nothing is soloed, and `rg -i solo ~/.config/rostrum` finds nothing.
6. Using only the keyboard, tab to a fader and press Page Up, M, S, 1, 2 and 3. Each key does what
   its tooltip says, and the focus ring is always visible.

## 9. First-time setup

1. Move `~/.config/rostrum` aside and tear the buses down (`$B --teardown`). Start Rostrum.
2. Setup opens on Welcome, with the seven steps listed on the left and the diagram. `wpctl status`
   shows no Rostrum nodes yet.
3. Get Started. On Headphones, press Test on your headset: you hear a short chime there and
   nowhere else, the first note on the left, the second on the right. Pick it. The star moves to it.
4. Next. On Mic, speak: only your mic's meter moves. Press Mute Mic: the header button turns
   red too. Unmute.
5. Next. Apps and Buses lists the six buses with what each receives (Game receives Games, Desktop
   receives Everything else, Mic says Your mic) and where each goes. Turn "Put apps on buses
   automatically" off: every line changes to "Apps you drag here". Turn it back on.
6. Next. On Startup, turn on "Launch Rostrum at login":
   `~/.config/autostart/dev.getrostrum.Rostrum.desktop` appears at once.
7. Next. On Privacy and Updates, "Ask me after a crash" is selected. Press See an Example Report:
   it shows your distribution, desktop and versions, and no user name, path or device name.
   Pick Never send. The update switches match how this copy was installed (test 14).
8. Next. Ready lists each choice. Click Crash reports: setup goes back to Privacy and Updates. The
   finished steps on the left show check marks and can be clicked; later steps cannot.
9. Go to Ready and press Create Mix: a spinner shows, then the Mixer opens.
   `~/.config/rostrum/scenes/live.toml` exists, and `settings.toml` has `wizard_done = true`,
   `setup_version = 2` and `crash_reports = 'never'`.
10. Repeat from step 1, but press Skip Setup on Welcome: the Mixer opens and the nodes exist
    anyway. Crash reports stay on Ask.
11. People who set Rostrum up before 0.1.0 got these choices: in `settings.toml`, delete the
    `setup_version` line and the `[privacy]` and `[updates]` tables, and start Rostrum. The Mixer
    opens with a one-time "Crash Reports and Updates" dialog. Done closes it and writes
    `setup_version = 2`, and it does not come back.

## 10. Apps, Scenes, Devices, OBS and Settings pages

1. Apps: turn off "Assign automatically", then play Firefox and Discord. Each shows with its
   binary and "matched by name". Assign
   Firefox to Music: the chip says Music and Always is on, and a rule appears on the right.
   Turn Always off: the rule disappears but Firefox stays on Music until it quits.
2. Apps: drag Firefox's volume slider. The row does not jump or reset while you drag.
   Type "disc" in the search box: only Discord rows remain in both lists.
3. Apps: run `pw-play -P '{ application.name = "ALSA plug-in [x]" application.process.binary = "mygame" }' file.wav`.
   A banner offers to name it. Name it "My Game" on Game: the rule shows "My Game → Game",
   matched by binary.
4. Scenes: New, Duplicate, Rename and Set as Default all update the list. Selecting a row does
   not change the mix; Load does. Export, delete a scene, then Import: it comes back, with
   " 2" added if the name is taken. New → Be Right Back… creates "Be Right Back" with your buses
   and rules, the mic muted and Game muted; the live scene is unchanged until you Load it.
   Move a fader, wait a second, and quit with `kill -TERM`: the scene file has the new level.
   With "Save scene changes automatically" off, the header shows the unsaved dot again.
5. Devices: the input meters move only while the page is open (`pw-cli ls Node | rg rostrum-meter`
   lists them only then). Picking another output moves Rostrum's headphone mix there.
6. OBS, live: with OBS running and obs-websocket on, the page says "Connected to OBS …" and lists
   what OBS records, with warnings for desktop audio and a direct mic. Press Set Up OBS: the
   preview lists the mic switch, the Stream Mix source and the mutes. Untick one mute and Apply.
   In OBS, the mic source records Rostrum Mic, "Rostrum Stream Mix" is in every scene, the ticked
   sources are muted and the unticked one is not. The page now shows a check mark. Press Undo OBS
   Changes: OBS is back as it was, and the Stream Mix source is gone.
7. OBS, closed: quit OBS and press Set Up OBS, then Apply. A `.rostrum-….bak` file appears next to
   the scene collection. Start OBS: Settings → Audio has Desktop Audio on Rostrum Stream Mix and
   Mic/Aux on Rostrum Mic. Quit OBS and press Undo OBS Changes: the next start is as before.
8. OBS, wrong password: change the obs-websocket password in OBS without restarting Rostrum. The
   page says the password was refused, and recovers on its own after OBS saves the new one.
9. OBS, by hand: open "Set it up by hand". Both node names show a check mark, and Copy puts
   "Rostrum Stream Mix" on the clipboard. With the nodes torn down, the page offers Create Mix and
   Set Up OBS is disabled.
10. Settings: turn off "Scroll to adjust faders": the wheel no longer moves faders. Turn on
   "Show dB readouts": every strip prints its level. Rebind "Mute mic" and restart: the new
   binding is kept in `settings.toml`.

## 11. Tray, close to tray, autostart and hotkeys

1. Install (`cmake --install build --prefix ~/.local`) and start Rostrum from the app menu. The
   launcher shows the Rostrum icon, and so does the tray.
2. The tray tooltip reads "Mic live · Scene: Live". Press the header mic button: the tray icon
   gains the red slashed-mic badge and the tooltip says "Mic muted". Unmute from the tray menu:
   the header button turns back too. The tray, the header and the hotkey drive the same state.
3. Close the window with the title bar button. Rostrum stays in the tray (`pgrep rostrum`), and
   audio keeps flowing. Left-click the tray icon: the window comes back. Tray menu → Quit exits.
4. Settings → Hotkeys: each row is plain (no warning). System Settings → Keyboard → Shortcuts
   lists Rostrum with the same eight actions. Press Meta+Alt+M with another app focused: the mic
   mutes. Rebind "Mute mic" to Meta+D (Peek at Desktop): the row warns that KWin already uses it,
   and Meta+D still works inside Rostrum's window only. Press Backspace on a focused shortcut
   button: the binding clears.
5. Make a second scene. Meta+Alt+PgDown switches to it from any app; the tray's Scenes submenu
   checks it. With "Save scene changes automatically" off, "Confirm before switching scenes" on
   and a fader moved, the hotkey raises the window and asks first.
6. Turn on "Launch at login": `~/.config/autostart/dev.getrostrum.Rostrum.desktop` exists
   and `desktop-file-validate` passes on it. Turn on "Start in tray", log out and in: Rostrum is
   in the tray with no window. Starting it from the app menu while it runs raises the window.
   Turn "Launch at login" off: the file is gone.
7. Unplug the headset while Rostrum runs: one notification, "Headphones disconnected, scene
   held." Mute and unmute never notify.

Tray, hotkeys, start in tray and close to tray were checked with a script that runs Rostrum in a
private D-Bus session with a headless nested KWin and a fake tray host, so nothing reaches the
real desktop. On GNOME, or anywhere without `org.kde.kglobalaccel`, hotkeys go through the XDG
GlobalShortcuts portal instead. That path is untested so far.

## 12. Automatic assignment

Start from a scene with no rules and "Assign automatically" on.

1. Start Discord, Spotify and a Steam game (Proton or native). Within a second each lands on
   Voice, Music and Game. The Apps rows say **Auto** with a reason ("Recognised as a voice chat
   app", "Steam game"), and a nameless Proton game is named from its Steam manifest. The log has
   one `recognised` and one `route … (automatic)` line per app. Nothing new appears in
   `~/.config/pipewire/*/50-rostrum.conf`.
2. With OBS running, OBS is listed but stays unassigned, with the reason shown. Start
   pavucontrol's test sound or Helvum: they are not moved either.
3. In an app's own audio settings, pick a specific output (Discord → Voice & Video → Output
   Device): Rostrum leaves it on that device and says why.
4. Drag Spotify's chip to Desktop: it stays there, the Auto tag goes away, and Always saves a rule.
   Press the chip's ✕ on Discord: Discord goes to the default sink, the row says "You took it off
   its bus.", and restarting Discord keeps it off. Settings → Apps → Forget Skipped Apps: Discord
   goes back to Voice.
5. Right-click the Music strip → Receives Automatically → Nothing: Spotify returns to the default
   sink and Apps says no bus receives music players. Pick Music Players on the Alerts strip
   instead: Spotify moves to Alerts, and Music's label no longer reads "Auto: music". Save, restart,
   and the choice is kept.
6. Turn "Assign automatically" off: every Auto row goes back to the default sink; rule-based apps
   do not move.

## 13. Crash reports

Needs a build with `-DROSTRUM_WITH_SENTRY=ON`. Without a DSN built in, start it with
`ROSTRUM_SENTRY_DSN=http://key@127.0.0.1:8766/1` and a local server that logs POSTs to
`/api/1/envelope/`. Use a copy started from a terminal, not one that holds your real session's
audio; `kill -SEGV` makes it crash, and the buses stay up, as in test 6.

1. A build without `ROSTRUM_WITH_SENTRY`, or without a DSN, shows no crash reporting anywhere:
   setup's step is called Updates, and Settings has no Privacy group.
2. With crash reports on Ask, run `kill -SEGV <pid>`. `rostrum.log` ends with a backtrace, and
   `~/.local/state/rostrum/sentry/` holds a `.run` folder with the crash.
3. Start Rostrum again. `~/.local/state/rostrum/crashes/crash-<time>-0.envelope` appears (mode
   0600, folder 0700), nothing reaches the server, and after a moment Rostrum asks to send the
   report. Press Show the Report: the JSON has the signal, frames with `package` file names and
   addresses, and the versions. It contains no `user`, no `/home` path, no device name, no
   registers and no timestamp.
4. Press Don't Send: the file is deleted, and the next start does not ask again.
5. Crash again and answer Send Report. The server receives one envelope with an `X-Sentry-Auth`
   header, no cookie, and exactly the JSON the dialog showed; the file is deleted. With the
   server stopped, the file stays and goes out at a later start.
6. Tick "Send reports without asking from now on" in the dialog: Settings → Privacy now says
   Send automatically, and the next crash is sent at start without a dialog.
7. Set Never send: `~/.local/state/rostrum/sentry/` is deleted at once. Crash again: nothing is
   captured, and the next start sends and asks nothing.

## 14. Updates

Serve a feed from a local folder (`python3 -m http.server 8765`) with a `latest.json` in the
format from [privacy.md](privacy.md), and start Rostrum with
`ROSTRUM_UPDATE_URL=http://127.0.0.1:8765/latest.json`. With the variable set, the first check runs
at once instead of after 20 seconds.

1. Source build, feed version 0.2.0: a banner says 0.2.0 is out and to pull and rebuild, with
   What's New and Skip This Version. What's New opens the feed's notes. Skip This Version hides
   the banner, and it stays hidden after a restart. Settings → Updates → Check now still says
   whether you are up to date.
2. Installed into `/usr` (or with `$SNAP` set): the banner says to update from the software center
   or package manager, and the automatic install switch is off and disabled.
3. AppImage: start Rostrum with `APPIMAGE=/tmp/rt/Rostrum.AppImage` pointing at a writable copy,
   and give the feed the SHA-256 and size of a different file as the new AppImage. The banner
   shows the download progress, then "Rostrum 0.2.0 is installed" with Restart Now. The file at
   `$APPIMAGE` is now the new one, with mode 755, and no `.part` file is left.
4. Same, with a wrong `sha256` in the feed: the old AppImage is untouched and the `.part` file is
   gone. The banner falls back to "Rostrum 0.2.0 is available" with Install, and Settings →
   Updates says it could not be installed because the download does not match the release
   checksum.
5. Turn off "Install updates automatically": the next new version shows a banner with Install
   instead of downloading on its own.
6. Turn off "Check for updates": no request reaches the server for the rest of the run, even
   with Settings → Updates open.
7. A feed with `"version": "0.3.0-rc1"`, or a GitHub release with `"prerelease": true`, is never
   offered.
8. Run with `FLATPAK_ID=dev.getrostrum.Rostrum`: Settings → Updates says Flatpak keeps Rostrum up
   to date, and there are no switches.

## Smoke test log

Kubuntu 26.04, Plasma 6.6 Wayland, PipeWire 1.6.2, WirePlumber 0.5.13, build 0.1.0. The checks
were scripted so that no test window, tray icon, shortcut or tone reached the real desktop: a
private D-Bus session, a temp `XDG_CONFIG_HOME`, and either a private Xvfb display, a headless
nested KWin with a fake tray host, or offscreen rendering. Fake devices were null sinks.

| Check | Result |
| --- | --- |
| Keyboard only: F6 to the page, Tab to Game, Page Down ×2, M, 2, Ctrl+S | Saved scene has Game at 0.8, muted, destination Stream |
| First-run wizard by mouse: Start, Next, Next, Create Mix (Test not pressed) | Mixer opens with the six default buses; `wizard_done = true`, `scenes/live.toml` written |
| 200 % scale (`QT_SCALE_FACTOR=2`), window 1280×720 | All six strips fit, no overlapping controls |
| Unplug the saved headset (fake), then plug it back | Banner and "PipeWire degraded"; Headphones fall back to the default sink; app keeps running; routes come back and the banner clears |
| Tray | Tooltip and menu follow the mic state and scene; Mute Mic from the tray mutes the header too; Show/Hide works |
| Close the window (KWin closes it) | App keeps running in the tray |
| Start in tray (`--autostart`) | No window until Show |
| Global shortcuts through KGlobalAccel | All eight registered as `dev.getrostrum.Rostrum`; invoking Mute mic toggles the mic; a shortcut KWin owns (Meta+D) is reported in Settings and stays in-window |
| Automatic assignment with stand-in streams (Discord, Spotify, Streamer.bot, Firefox, an unknown game with `media.role = "Game"`) and the real OBS | Each lands on Voice, Music, Alerts, Desktop and Game with the reason shown; OBS is recognised and left alone; no fragment file changes |
| First-time setup, each step rendered in a headless nested KWin and offscreen at 1280×720 | Step list marks finished steps; no layout overlap; no QML warnings |
| Existing settings with `wizard_done = true` and no `setup_version` | The one-time Crash Reports and Updates dialog opens over the Mixer, with no binding loops |
| Sentry build, `kill -SEGV` on a test instance (no PipeWire, private D-Bus, temp config), local server as the DSN | Send: one 3.5 KB envelope posted at the next start, file deleted; no user name, path, installation ID, registers, timestamp or device name; only the 4 libraries in the stack listed. Ask: report kept (0600), nothing posted. Never: nothing captured, no sentry folder |
| `official` preset build, same crash, sent to the real Sentry project | Sentry accepted the report (2xx) at the next start; the local file deleted; the stored event has no IP address and no location |
| Update feed 0.2.0 from a local server, as an AppImage | Download checked against SHA-256, AppImage replaced with mode 755, Restart Now offered; no cookies sent; `User-Agent: Rostrum/0.1.0` |
| Same, with a wrong checksum | Refused; the old AppImage kept; no `.part` file left |
| Unit tests | 11 of 11 pass |

Still manual (needs hardware or a real session): tests 4 (mic path), 6 and 7 (quit and reboot
routing with Discord), OBS capture, the real headset unplug, the GlobalShortcuts portal on a
non-Plasma desktop, and updates against the real getrostrum.dev feed.
