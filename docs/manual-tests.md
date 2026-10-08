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
2. `$B --seconds 3`. It logs `creating` for ten nodes and prints `Mix ready.`
3. `wpctl status | grep Rostrum` lists, under Sinks: Rostrum Game, Voice, Music, Alerts, Desktop,
   Sidetone, Stream Mix, VOD Mix, Headphones Mix; under Sources: Rostrum Mic.
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
Headphones-only buses do not link to `rostrum.vod`. Turning a bus back to Stream or Both restores
its saved VOD flag without clearing it. Mic is not linked to `rostrum.vod` (OBS captures `rostrum.mic`
directly).

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
6. **With mic filters.** Turn on Mic Filters. `pw-link -l` shows the hardware source linked only
   into `rostrum.micfx:input_MONO`, and `rostrum.micfx:output_MONO` linked into `rostrum.mic`,
   `rostrum.sidetone` and `rostrum.filtered`. Repeat steps 2 to 5: same results, with the filtered
   voice. In OBS, exactly one mic meter still moves. If OBS has a source on "Rostrum Filtered
   Mic", the OBS page lists it as doubling the voice and Set Up OBS fixes it.

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
2. Setup opens on Welcome, with the eight steps listed on the left and the diagram. `wpctl status`
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
8. Next. On OBS, with OBS installed and running: the step says whether OBS records Rostrum, and
   the footer button reads Skip. Press Set Up OBS…: a spinner shows while Rostrum creates its
   devices, then the same preview as the OBS page opens. Cancel it. "Follow OBS while it runs" and
   "Warn me when a stream starts with a problem" are on. With OBS not installed, the step says
   so, and Skip moves on.
9. Next. Ready lists each choice, OBS included. Click Crash reports: setup goes back to Privacy
   and Updates. The finished steps on the left show check marks and can be clicked; later steps
   cannot.
10. Go to Ready and press Create Mix: a spinner shows, then the Mixer opens.
    `~/.config/rostrum/scenes/live.toml` exists, and `settings.toml` has `wizard_done = true`,
    `setup_version = 3` and `crash_reports = 'never'`.
11. Repeat from step 1, but press Skip Setup on Welcome: the Mixer opens and the nodes exist
    anyway. Crash reports stay on Ask.
12. People who set Rostrum up before 0.1.0 got these choices: in `settings.toml`, delete the
    `setup_version` line and the `[privacy]`, `[updates]` and `[obs]` tables, and start Rostrum.
    The Mixer opens with a one-time "Crash Reports, Updates and OBS" dialog (or "Updates and
    OBS" without crash reports) with both groups of switches and Open the OBS Page. Done closes
    it and writes `setup_version = 3`, and it does not come back.
13. People who set Rostrum up before the OBS step: set `setup_version = 2` and delete `[obs]`. The
    dialog is called "Rostrum and OBS" and shows only the OBS switches. Open the OBS Page closes it,
    shows the OBS page and writes `setup_version = 3`.

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
   preview lists the mic switch (tracks 1 and 2), the Stream Mix source (track 1), the "Rostrum VOD
   Mix" Audio Output Capture (track 2 only; Desktop Audio 2 is never assigned), setting Twitch VOD
   Track to 2 when streaming to Twitch, and the mutes. Untick one mute and Apply.
   In OBS, the mic source records Rostrum Mic, "Rostrum Stream Mix" and "Rostrum VOD Mix" are in
   scenes, the ticked sources are muted and the unticked one is not. The page now shows a check
   mark. Press Undo OBS Changes: OBS is back as it was, the Stream Mix and VOD Mix sources are gone,
   and the Twitch VOD track setting is restored.
7. OBS, closed: quit OBS and press Set Up OBS, then Apply. A `.rostrum-….bak` file appears next to
   the scene collection. Start OBS: Settings → Audio has Desktop Audio on Rostrum Stream Mix (track 1),
   Mic/Aux on Rostrum Mic (tracks 1 and 2), Desktop Audio 2 unassigned, and "Rostrum VOD Mix"
   captured on track 2. Quit OBS and press Undo OBS Changes: the next start is as before.
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
   Turn off Settings → General → "Hide to tray when closed" and close the window: Rostrum quits.
   Turn on "Hide to tray when minimized" and minimize from the title bar or the task bar: the
   window leaves the task bar and the tray icon brings it back, not minimized. Switch virtual
   desktops with the window open: it stays where it is.
4. Settings → Hotkeys: each row is plain (no warning). System Settings → Keyboard → Shortcuts
   lists Rostrum with the same eight actions. Press Meta+Alt+M with another app focused: the mic
   mutes. Rebind "Mute mic" to Meta+D (Peek at Desktop): the row warns that KWin already uses it,
   and Meta+D still works inside Rostrum's window only. Press Backspace on a focused shortcut
   button: the binding clears.
5. Make a second scene. Meta+Alt+PgDown switches to it from any app; the tray's Scenes submenu
   checks it. With "Save scene changes automatically" off, "Confirm before switching scenes" on
   and a fader moved, the hotkey raises the window and asks first.
6. Turn on "Launch at login": `~/.config/autostart/dev.getrostrum.Rostrum.desktop` exists
   and `desktop-file-validate` passes on it. For an AppImage, `Exec=` has the quoted AppImage path
   plus `--autostart` and `TryExec=` points at the file. For a `~/.local` install, `Exec=` and
   `TryExec=` contain the absolute home path, such as `/home/alex/.local/bin/rostrum`, not a
   literal `~` or an unqualified `rostrum`. With Rostrum already running, run that executable
   with `--no-start --list-buses` under `PATH=/usr/local/bin:/usr/bin:/bin`; it reaches the
   running instance without starting another mix. (Previously, `Exec=` wrote an unqualified
   `rostrum --autostart` or a transient mount path that failed on session login because `~/.local/bin`
   was not in the session manager's default `$PATH` and FUSE mounts disappeared after logout;
   `TryExec=` and `StartupWMClass=` were also missing, and moving the binary did not rewrite the file.)
   Verify moving the binary rewrites the entry on launch. Turn on "Start in tray", log out and in:
   Rostrum launches silently into the tray without showing a window, even if the panel registers after
   Rostrum. Starting Rostrum from the app menu while running raises the window, whereas a second login
   with `--autostart` remains in the tray. Turn "Launch at login" off: the file is removed.
7. Unplug the headset while Rostrum runs: one notification, "Headphones disconnected, scene
   held." Unplug the saved mic: one notification, "Mic disconnected, stream mic silent." (test 17).
   Mute and unmute from the window never notify.
8. Tray menu → Mute Stream: the Stream master strip shows Muted, the item is checked, and the
   tooltip adds "Stream muted". Previous Scene and Next Scene step through the scenes.
   Middle-click the tray icon: the mic toggles. Scroll on the tray icon: the Stream master moves
   a little per wheel step, and the headphones level does not.
9. With the window hidden, Tray menu → Restart Rostrum: the tray icon goes away and comes back
   within a few seconds, the window stays hidden, and music playing through a bus never stops.
   Open the window and restart from Settings → General → Restart Rostrum: the window comes back.
   `pgrep -c rostrum` is 1 afterwards. Reinstall a new build while Rostrum runs and restart: the
   change in the new build is there.

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
3. In an app's own audio settings, pick a specific output other than your headphones (Discord →
   Voice & Video → Output Device, e.g. HDMI): Rostrum leaves it on that device and says why. Pick
   your headphones there instead: Rostrum places the app as usual. Minecraft from Prism Launcher
   (which names the default device) lands on Game.
4. Drag Spotify's chip to Desktop: it stays there, the Auto tag goes away, and Always saves a rule.
   Choose Unassign in Discord's chip menu: Discord goes to the default sink, the row says "You took it off
   its bus.", and restarting Discord keeps it off. Settings → Apps → Forget skipped apps: Discord
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

## 15. OBS while live

Use a test OBS profile and scene collection with a "Starting Soon", a "Game" and a "BRB" scene,
and a stream target you can stream to safely (an `rtmp://127.0.0.1/` server that accepts
anything, or OBS's "Record" only for the REC steps). obs-websocket on, Rostrum set up for OBS.

1. With "Follow OBS while it runs" on and the OBS page closed, start OBS: within about 5 seconds
   `ss -tnp | rg rostrum` shows one connection to `127.0.0.1:4455` and nothing else. Quit OBS:
   the connection is gone, and nothing new appears in `rostrum.log`.
2. Start streaming in OBS: the header shows a red LIVE badge with a clock counting up. Start
   recording: a REC badge appears next to it; pause, and it reads REC paused with the clock held.
   The tray tooltip starts with "LIVE since <time> · REC paused · ". With a screen reader, the
   badges read "Live on stream for …" and "Recording for …". Click one: the OBS page opens.
3. Quit Rostrum while streaming and start it again: the LIVE clock carries on from OBS's time,
   not from zero. Quit OBS while live: both badges disappear at once.
4. Mute your mic in Rostrum and start streaming: a red banner "You're live, but: Your mic is
   muted…" appears with Unmute Mic, and one critical notification "Check your stream audio".
   Press Unmute Mic: the banner goes away. Stop and start the stream with Master Stream muted:
   the banner says no bus reaches the stream; Open Mixer goes to the Mixer. Start again with Music
   soloed and set to Headphones only: the same warning. In OBS, point the Rostrum Stream Mix
   source at Default and start streaming: the banner says OBS isn't recording Rostrum Stream Mix;
   Open OBS Page opens it.
   When the stream service is Twitch, verify Output → Streaming → Twitch VOD Track: if it is not
   set to 2 (e.g. disabled or 1), stream readiness stays "Needs attention" ("Output → Streaming →
   Twitch VOD Track is not set to track 2."). Setting Twitch VOD Track to 2 clears that item, but a
   missing Rostrum VOD Mix capture stays "Needs attention" ("No supported enabled input configured
   for Rostrum VOD Mix.") even when Twitch VOD Track is already 2. Setting an input track mask alone
   is not enough.
   Stopping the stream clears the banner. Muting the mic mid-stream does not raise a new one.
5. Turn off "Warn me when a stream starts with a problem" and repeat a muted-mic start: no
   banner, no notification.
6. OBS page → When OBS switches scenes: the three OBS scenes are listed, and the one on program
   says On program. Map Game → Live and BRB → a "Be Right Back" scene. Switch OBS to BRB:
   Rostrum loads Be Right Back at once, with no confirm dialog, even with "Confirm before
   switching scenes" on, and a toast names both scenes. With auto-save on, a fader moved just
   before is saved in Live. `settings.toml` has an `[obs.scene_map]` table with both lines.
   Starting Soon (No change) leaves the Rostrum scene alone.
7. Rename BRB in OBS: the old name shows "(not in OBS)" with a Forget button, and the new one is
   unmapped. Forget removes the line from `[obs.scene_map]`.
8. Turn "Follow OBS while it runs" off: the connection closes once the OBS page is closed, the
   badges go away, and the scene map section offers Follow OBS While It Runs. Turn it back on.
9. Turn off OBS's WebSocket server (Tools → WebSocket Server Settings) and quit OBS: once OBS is
   started again, Rostrum makes no connection attempt. Turn the server back on and quit OBS.
10. Backoff: with OBS closed and its server on in the config, run a stand-in process named `obs`
    (`cp /bin/sleep /tmp/obs && /tmp/obs 600`). Rostrum's refused attempts
    (`strace -f -e trace=connect -p <pid>`) come 5, 10, 20, 40 and then 60 seconds apart, with no
    toast and nothing in `rostrum.log`. Opening the OBS page tries at once and shows the error.
    Kill the stand-in.

## 16. Hold actions, panic, on-screen feedback, command line and D-Bus

Bind Push to talk, Push to mute and Panic mute in Settings → Hotkeys (they start unbound), and
keep a recording app (or `pw-record --target "Rostrum Stream Mix"`) on the stream so you can hear
what reaches it. The mic path itself is test 4; do it first.

1. Mute the mic. Hold Push to talk with another app focused: the header says live and the stream
   hears you. Let go: muted again. The scene is not marked changed, and `scenes/*.toml` and
   `settings.toml` do not change.
2. With the mic live, hold Push to mute: muted while held, live after.
3. Press Panic mute: Plasma's OSD says "Panic mute: mic and stream muted", and the stream and
   VOD hear nothing, mic or apps (`rostrum.stream` and `rostrum.vod` are both muted). Master stream
   mute also silences both `rostrum.stream` and `rostrum.vod`. Switch scenes: still silent. Press
   Panic again: the mic, stream, and VOD come back as the scene has them. Quit during panic and start
   again: nothing is muted that the scene does not mute.
4. Bind Mute Game bus and Stream volume up, press them with the window in the background: the
   Game strip mutes, the Stream master rises 5 % per press (and repeats while held). With the
   Rostrum window in front, the same keys show no OSD. Turn off Settings → General → "Show hotkey
   changes on screen": no OSD at all. Stop plasmashell (or try another desktop): a short
   notification shows instead, each one replacing the last.
5. From a terminal while Rostrum runs: `rostrum --toggle-mic` toggles the mic and exits 0;
   `rostrum --scene nowhere` prints "There is no scene called" and exits 1;
   `rostrum --set-volume game=50%` moves the Game fader; `rostrum --list-buses` prints it as
   `game	0.50	…`. With "Confirm before switching scenes" on, `rostrum --scene <other>` switches
   without a dialog.
6. Quit Rostrum. `rostrum --list-scenes` still prints the scenes and starts nothing (`pgrep
   rostrum`). `rostrum --mute-mic` starts Rostrum with the mic muted.
7. `qdbus6 dev.getrostrum.Rostrum /dev/getrostrum/Rostrum/Control` lists the methods of
   `data/dev.getrostrum.Rostrum1.xml`. `gdbus monitor --session --dest dev.getrostrum.Rostrum`
   shows `PropertiesChanged` with `MicMuted` when the header mic button is pressed. (Per-bus VOD
   flag control on `dev.getrostrum.Rostrum1` will come in a follow-up commit).

## 17. Mic unplugged

Use the fake devices from test 3 (`rostrumtest.mic` fed by a tone) and keep a second source
around as the system default, for example a webcam or `pw-cli create-node adapter '{
factory.name=support.null-audio-sink node.name=rostrumtest.webcam media.class=Audio/Source/Virtual
audio.position=[MONO] }'` set as default with `wpctl set-default`.

1. Run `$B --headphones rostrumtest.headset --mic rostrumtest.mic`. `pw-link -l` shows
   `rostrumtest.mic` linked into `rostrum.mic` and `rostrum.sidetone`.
2. Destroy `rostrumtest.mic` (`pw-cli destroy <id>`), or unplug the real mic. It prints
   `Mic disconnected (…), stream mic silent.` Nothing is linked into `rostrum.mic` or
   `rostrum.sidetone` (`pw-link -l | rg -B1 'rostrum\.(mic|sidetone):'` is empty), the default
   source is not linked anywhere by Rostrum, and `pw-record --target rostrum.mic m.wav` is silent.
   `wpctl inspect` on `rostrum.mic` shows it muted.
3. Recreate the fake mic and feed it again. It prints `Mic back: rostrumtest.mic`, the links come
   back and the capture from step 2 has signal again.
4. In the app: unplug the mic. The banner reads "Mic disconnected. Your stream mic is silent until
   it comes back." with Choose Mic, the header button says No mic, the status bar shows the mic as
   "(unplugged)", one notification is sent, and the log has `mic missing: … stream mic silent`.
   `settings.toml` still names the unplugged mic.
5. Devices → turn on "Use another mic while mine is unplugged" and unplug again (or rerun step 2
   with `--mic-fallback`): the default source is linked into `rostrum.mic`, the banner says
   "Using … until it comes back", and `settings.toml` has `mic_fallback = true` and still names
   the saved mic. Turn it off again: the fallback links go away at once.
6. With no mic ever chosen (`mic = ''`), Rostrum uses the system default source as before.

## 18. Per-app mute

1. Play Firefox and Spotify, each on a bus. On the Apps page, press Firefox's mute button. Firefox
   goes silent at once, its row meter drops, Spotify keeps playing, and `wpctl inspect` on
   Firefox's stream shows `mute = true`. The log has `mute "Firefox" <id>`.
2. With Always on for Firefox, the scene file's `[[rule]]` for it gains `muted = true`. Unmute:
   the line goes away. Turn Always off and mute: the scene file is unchanged.
3. Mute Firefox in Plasma's volume applet or pavucontrol instead: Rostrum leaves it muted and does
   not unmute it back, and vice versa.
4. With Firefox muted by Rostrum, quit Rostrum from the tray. Firefox plays again (through its bus,
   which lingers). Start Rostrum: if the mute was saved in the rule, Firefox is muted again.
5. Switch to a scene whose rule for Firefox is not muted: Firefox plays.

## 19. Bus balance

1. Play music on the Music bus with Music → Both. Drag its balance slider fully left: only the left
   ear plays. `pw-dump` on `rostrum.music` shows `channelVolumes` with FR at 0 and FL at the fader.
   OBS (capturing `rostrum.stream`) hears it on the left only too.
2. Double-click the slider: back to centre. Use "Centre Balance" in the strip menu after moving it:
   same. The menu item is disabled at centre. The mic strip has no balance slider and its fader is
   as tall as the others.
3. Set a balance of about Right 40%, save the scene. The scene file's `[[bus]]` for music has
   `balance = 0.4`; the other buses have no `balance` line. Centre it: the scene is dirty, save, and
   the line goes away.
4. With a balance set, change the bus volume in pavucontrol: within a second Rostrum puts both
   channels back.
5. Hand-edit `balance = 9` into a scene and load it: the slider is fully right.

## 20. Mono headphones

1. Play a test file with sound only on the left. Turn on Devices → Mono headphones: both ears hear
   it at the same level, quieter than the left ear did before (-6 dB). `pw-link -l` shows each
   `rostrum.phones` monitor port linked to both playback_FL and playback_FR of the headphones.
2. Play centred music: the loudness does not jump when toggling. Toggle quickly a few times: never
   a loud blip.
3. Master Headphones and the scene stay as they were (no dirty scene). The stream (OBS) stays
   stereo.
4. Switch headphones to a mono device or a 5.1 sink while on: mono sinks play normally; on 5.1 only
   the front left and right get the downmix.
5. Quit Rostrum: headphones keep playing, still mono. Start it with the switch off: back to stereo.

## 21. Scene fades

1. With Settings → Scene fade at Off, switch between two scenes with different Music levels: the
   level jumps, as before.
2. Set it to 1 second. Play music and switch from a scene with Music at 0 dB to one with Music
   muted: the M button lights at once, the music fades out over about a second, and only then
   does `pw-dump` show `rostrum.music` muted. Switch back: the node is unmuted at once and fades
   in from silence. Master stream fades and scene fades stay synced across both `rostrum.stream`
   and `rostrum.vod`. Switching to a scene that toggles "Include in Twitch VOD" on a bus links or
   unlinks `rostrum.vod` without pops or interruptions to other buses.
3. Switch to a scene that mutes the mic while talking: the mic cuts at once (OBS meter drops
   immediately), even though buses are still fading.
4. Start a 1 second fade and switch again halfway: the level turns around from where it was, no
   jump to either end.
5. During a fade, grab a fader: it follows your hand at once, the other buses keep fading.
6. With auto-save off, switch scenes with a fade: the scene is not marked as changed during or
   after the fade, and Save writes the target levels. With auto-save on, the scene file never
   contains an in-between level.
7. Quit and start Rostrum with a fade set: the default scene applies at once (no ramp from 0 dB).
8. Press the Next scene shortcut several times quickly: no clicks, and every bus ends at the last
   scene's level.

## 22. Auto-ducking

1. With ducking off (the default), `pw-dump | grep rostrum-meter` shows no meter streams while the
   Mixer page is hidden, and Plasma's mic indicator is off with Rostrum in the tray.
2. Settings → Ducking: turn it on with the defaults (your mic, Music, -12 dB). Play music on the
   Music bus and speak: within about 0.1 s the music drops by 12 dB and the Music strip says
   "Ducked"; its fader does not move. Stop speaking: after half a second it comes back over
   about 0.8 s. Short pauses between words do not let it come back.
3. While ducking listens to the mic, the mic indicator stays lit even with the window hidden.
   Mute the mic: the indicator goes off (after the mic meter closes) and nothing ducks.
4. The scene is never marked as changed by ducking, and the saved scene file has the Music fader
   level, not the ducked one. `pw-dump` on `rostrum.music` shows the lower volume only while
   ducked.
5. Trigger "Someone speaks in voice chat": play a voice call on the Voice bus: Music ducks, and the
   Voice bus never ducks even if it is ticked. Trigger "Either": both work.
6. Change amount, attack and release: the effect follows at once. Tick Alerts too: both duck.
7. Move the Music fader while ducked: it moves, and the ducking stays applied on top.
8. Speak (ducked), and quit Rostrum from the tray mid-sentence: Music returns to its full level.
9. With a scene fade set, switch scenes while ducked: the fade and the ducking combine, and once
   speech stops, every bus ends at the new scene's level.

## 23. Scene order and hotkey slots

1. Make nine scenes. On the Scenes page the first eight show badges 1–8; hovering badges 1–4
   names their shortcuts (Meta+Alt+1 …), and 5–8 say no shortcut is set until one is bound in
   Settings → Hotkeys. The ninth has no badge.
2. Select the fourth scene and press Move Up (or Alt+Up): it becomes third and takes badge 3.
   The header scene menu and the tray's Scenes submenu list the same order, and Meta+Alt+3 now
   loads it from any app. Meta+Alt+PgDown and the tray's Next Scene walk the scenes in this order;
   Meta+Alt+PgUp and Previous Scene walk it backwards. Bind Load scene 6 and press it: the sixth
   scene in the list loads.
3. `rostrum --list-scenes` and `gdbus call --session --dest dev.getrostrum.Rostrum --object-path
   /dev/getrostrum/Rostrum/Control --method dev.getrostrum.Rostrum1.ListScenes` print the same
   order, and `rostrum --scene` by name still works. Quit Rostrum: `rostrum --list-scenes` still
   prints that order.
4. `settings.toml` has `scene_order` under `[scenes]` with the new order. Restart: the order is
   kept.
5. Rename a scene: it keeps its place. Delete one: the rest close up. Import a bundle or make a
   new scene: it goes to the end. Delete `scene_order` from `settings.toml` and restart: scenes
   fall back to file-name order.

## 24. Undo, redo and deleted scenes

1. Drag the Game fader, let go, and wait a second. The app menu shows "Undo Game Volume". Ctrl+Z
   puts the fader back in one step, even after a long drag; Ctrl+Shift+Z (or Redo in the menu)
   moves it again.
2. Mute Voice, rename the Music bus, add a bus, then press Ctrl+Z four times: each edit is
   undone in reverse order, and the menu names each one ("Undo Add Bus “Bus”", "Undo Rename Bus
   to …", "Undo Voice Mute"). The bus list and names on disk follow (the scene file is rewritten).
3. Move Music's balance, then mute Firefox on the Apps page with Always on: the menu says "Undo
   App Mute", then "Undo Music Balance". Both come back with Ctrl+Z, and the scene file's
   `muted = true` and `balance` lines go with them.
4. Solo Game, change Music's level, and press Ctrl+Z: Music goes back and Game stays soloed. Mute
   the mic, move a fader, and press Ctrl+Z twice: the fader goes back and the mic stays muted.
   Hold Push to talk, or turn on Panic mute, and undo a fader move: the hold or panic stays as it
   was, and nothing about it reaches the scene file.
5. With Settings → Scene fade at 1 second, switch scenes, grab the Game fader halfway through the
   fade and press Ctrl+Z at once: every bus stops fading and sits at the new scene's level. With
   ducking on, undo while ducked: the ducking stays applied on top.
6. Load another scene: Undo is empty again. Type in the scene rename field and press Ctrl+Z: the
   text field undoes, not the mixer.
7. Delete a scene that is not live: the toast says "Deleted “…”" with Undo. Press Undo within the
   toast's time: the scene is back at the same place in the list (and as default, if it was).
   `~/.config/rostrum/trash/` is empty again.
8. Delete two scenes, then open Scenes → ⋮ → Recently Deleted…: both are listed newest first with
   the time. Restore one: it comes back at the end of the list, with " 2" added if the name is
   taken. Set a trash file's name to a date more than 30 days ago and restart: it is gone.

## 25. Compact window

1. App menu → Compact View: the window shrinks to the mic button, the scene switcher, Headphones
   and Stream, and one slim fader with a mute button per bus. It can be resized down to about
   420×220; the bus list scrolls.
2. Move a bus fader and mute a bus in the compact window: the full Mixer shows the same state.
   Double-click a slim fader: it goes back to 0 dB. The mic button and scene switcher work as in
   the header; Manage Scenes… in the scene menu opens the full window on the Scenes page.
3. With the mic muted, hold Push to talk: the compact mic button and the Mic row say live while
   held, muted after. Press Panic mute: the mic button says muted and the Stream row shows muted
   until panic is turned off.
4. Resize the compact window, press Full View, then Compact View again: each mode comes back at
   its own size. Quit in compact mode and start again: Rostrum opens compact. `settings.toml`
   has `compact`, `compact_width`, `compact_height` and `keep_on_top` under `[window]`.
5. Turn on Keep on Top. On X11 the window stays above others; on Plasma Wayland the hint may be
   ignored (the tooltip says so; the window menu's Keep Above Others works). Full View drops the
   hint, and it comes back with the compact window. Closing or minimizing the compact window
   hides it to the tray as the tray settings say.
6. Before setup is finished, Compact View is disabled and a saved `compact = true` is ignored.

## 26. Settings backup and restore

1. Settings → Application → Advanced → Back Up Settings…: the save dialog suggests
   `rostrum-backup-<date>.toml` in Documents. The file has `kind = "rostrum-backup"`, a
   `[settings]` table with general, mixer, ducking, apps, obs, scenes, devices, hotkeys and
   advanced, and one `[[scene]]` per scene. It has no `privacy`, `updates`, `window`,
   `wizard_done`, `setup_version` or `last_seen`.
2. Change a hotkey, turn off auto-save, edit the live scene's levels and delete a scene. Restore…
   the backup and confirm: a toast names the safety copy, which is in
   `~/.config/rostrum/backups/before-restore-<time>.toml`. The hotkey and auto-save are back, the
   deleted scene is back, the live scene plays the backed-up levels, and the replaced version is
   in Scenes → Recently Deleted. Scenes made after the backup are still there.
3. Before restoring, also change the ducking settings, the scene fade, "Follow OBS while it runs",
   an OBS scene mapping, "Use another mic while mine is unplugged", Mono headphones and the two
   hide-to-tray switches. After the restore each is back as backed up and takes effect at once
   (ducking and mono headphones apply without a restart; the OBS page shows the restored map).
4. Crash report and update choices, window size and compact mode are unchanged by the restore.
   Launch at login follows the backup (the autostart file appears or goes).
5. Restore a scene export or a random TOML file: the toast says nothing was restored, and no
   safety copy is written.

## 27. Translations

1. `cmake --build build --target rostrum-pot` rewrites `po/rostrum.pot` with no diff other than
   the creation date, unless strings changed.
2. With a stub `po/de/rostrum.po` that translates one string, a build and install puts
   `share/locale/de/LC_MESSAGES/rostrum.mo` in the prefix, and `LANGUAGE=de` shows that string in
   German while the rest stays English.

## 28. Mic filters

Use the real mic, Discord (or any voice chat app) and Audacity. A fan or keyboard near the mic
makes noise removal easy to hear.

1. **Off by default.** `wpctl status` lists no Rostrum Filtered Mic and `pw-dump | grep
   rostrum.micfx` finds nothing. The FX button on the mic strip is not lit.
2. **On.** Press FX. Within a second the Mic Filters page shows the filters as on (not
   "starting"), and the log says `mic filter graph running in <id>` once. Sources now include
   "Rostrum Filtered Mic". The hardware mic and `wpctl inspect @DEFAULT_SOURCE@` are unchanged.
3. **Sound.** Record `pw-record --target rostrum.mic on.wav` with the fan running, then turn the
   filters off and record `off.wav`: the fan is gone or much quieter in `on.wav`, and speech is
   steady in level. Moving a slider (Noise removal strength, Tone) changes the sound at once,
   without a click or dropout.
4. **Apps.** Join a Discord voice channel and open Audacity's recording meter. The Apps list on
   the page shows Discord "Hears the filtered mic" and Audacity "Hears your plain mic … An audio
   tool". `pw-link -l` agrees. Discord's own input test sounds filtered.
5. **Per-app choice.** Switch Discord off in the list: Discord moves to the plain mic without
   leaving the call, and `filtered_apps`/`raw_apps` in `settings.toml` record the choice. Quit and
   restart Discord: it comes back on the plain mic (WirePlumber's remembered target is cleared;
   the log says `(remembered by WirePlumber)`). Switch it on again, and Audacity on: both move.
6. **Scope.** "Only the stream": Discord goes back to the plain mic, Audacity keeps your choice.
   Back to "The stream and every app".
7. **Quit and crash.** Quit Rostrum: Discord stays on the filtered mic and still sounds filtered.
   Start Rostrum: the log has no `filter mic for` line for Discord (nothing is moved again), and
   no `creating` line for the two nodes. Repeat with `kill -9`: the same.
8. **Off restores.** Turn the filters off: Discord and Audacity return to the plain mic, the
   hardware mic is linked straight to `rostrum.mic` and `rostrum.sidetone` again, and both filter
   nodes are gone.
9. **Command line and D-Bus.** `rostrum --action toggle_mic_filters` toggles them, the FX button
   and page follow, and `gdbus call --session --dest dev.getrostrum.Rostrum --object-path
   /dev/getrostrum/Rostrum/Control --method dev.getrostrum.Rostrum1.SetMicFilters true` turns them
   on. The `MicFilters` property changes with `PropertiesChanged`. A hotkey bound to "Mic filters
   on or off" works.
10. **Outside changes.** With the filters on, change a control with
    `pw-cli set-param <micfx id> Props '{ params = [ "rostrum_limiter:Ceiling" -20.0 ] }'`: within
    about a second it is back to the page's value.
11. **Gain warning.** Raise the mic gain above 100 % with the limiter on: the page warns.
12. **Meters and ducking.** With the fan running and filters on, the mic strip's meter stays low
    between words. With ducking on, the fan alone does not duck Music.
13. **Mute in a call.** With Discord using Rostrum Filtered Mic, mute from the mixer,
    OpenDeck, or `rostrum --mute-mic`: Discord's mic test and Rostrum Mic both hear silence.
    Unmute: both hear the mic again. Repeat with Push to mute and Panic mute. Clear Panic
    mute, mute the mic, then hold Push to talk: both sources are live only while held.
    The hardware input remains unchanged. Apps using
    the plain hardware mic directly do not follow Rostrum's mute.

## 29. Mic check

1. **Record and play back.** On Mic Filters, press Check Mic and talk for 5 seconds. The progress
   bar counts down, then the recording plays once in the headphones only: OBS's audio meter for
   Rostrum Stream Mix stays still during playback apart from your live mic. It sounds filtered
   when the filters are on and plain when they are off.
2. **Verdict.** Talk normally: "Good level". Whisper far from the mic: "Too quiet". Mute the mic
   on the device itself (not in Rostrum): "Rostrum heard almost nothing". Gain at 150 % with the
   limiter off and shouting: "Too loud".
3. **Refusals.** With the mic muted in Rostrum, Check Mic says so and records nothing. With no
   headphones chosen and none available, it says so too.
4. **Hotkey.** Bind "Check mic" in Settings → Hotkeys, hide the window and press it: an on-screen
   message says "Mic check: talk now", and after 5 s the verdict. Pressing it again during a check
   stops it.
5. **Hear yourself live.** Turn on "Hear yourself live" with sidetone at 0: you hear yourself at
   once (the volume jumps to 50 %). The mixer's Sidetone expander shows the same state.

## 30. Easy Effects

Needs Easy Effects 7 or 8 with "Process all output streams" and "Process all input streams" on
(the defaults).

1. **Reclaimed at start.** With Easy Effects running, start YouTube Music, Spotify or a browser
   video. Within a second the app is on Music, its strip meter moves, and Mute on Music silences
   it. `journalctl --user` or `~/.local/state/rostrum/rostrum.log` shows `reclaim … from "Easy
   Effects Sink"`.
2. **A later move is yours.** Ten seconds later, move the app to Easy Effects Sink in pavucontrol.
   It stays there. Its chip on Music gets an orange outline and a warning icon, its tooltip says
   "Not on this bus: another program moved it to Easy Effects Sink", the Apps row says "Another
   program moved it to Easy Effects Sink", and the Music meter does not move.
3. **Move Back.** Chip menu → Move Back to This Bus (or Move Back on the Apps row): the app is on
   Music again and the warning goes away.
4. **Mic apps.** With the filters on, join a Discord voice channel: Discord records Rostrum
   Filtered Mic, not Easy Effects Source, and the Mic Filters page says "Hears the filtered mic".
5. **Own streams.** The Devices page test chime and the mic check play in the headphones, never
   through Easy Effects.

## 31. OpenDeck plugin install from Settings

No physical deck is needed for the install checks.

1. Open Settings → Integrations → OpenDeck. Native detection uses `$XDG_CONFIG_HOME/opendeck`
   when set, otherwise `~/.config/opendeck`. Flatpak detection uses
   `~/.var/app/me.amankhanna.opendeck/config/opendeck`. If both exist, choose an installation;
   neither folder changes before a choice. A custom plugins folder stays selected until cleared;
   a missing custom folder is an error, with no switch to another installation.
2. Record the plugins and profiles before pressing Install. If Rostrum's plugin is already
   installed, back up only `dev.getrostrum.Rostrum.sdPlugin` outside the plugins folder first.
   Press Install: only that directory is created under the selected config's `plugins` folder.
   It contains `manifest.json`, executable `plugin.sh`, `plugin.py`, `inspector.html`, and the
   five built-in SVG icons. None is a symlink into a checkout or an AppImage mount. Other plugins
   and profiles remain unchanged. Stop OpenDeck during the comparison so its own writes do not
   obscure the install's changes.
3. The status says the plugin matches this release and tells the user to restart OpenDeck.
   It does not say the plugin loaded. Try the Installed button again: it is disabled, and the
   file contents and modification times do not change. An older copy offers Update; an
   incomplete copy offers Repair. A failed replacement retains the previous copy.
4. Start a test instance with a PATH containing no `python3`: the page reports Python 3 was
   not found, Install is disabled, and no plugin is installed. Restore the normal PATH afterward.
5. Manual copy fallback: in OpenDeck settings choose Open config directory, then copy
   `tools/opendeck/dev.getrostrum.Rostrum.sdPlugin` into `plugins`. Flatpak OpenDeck uses its
   own config directory. Restart OpenDeck, then drag an action onto a key.
6. Separately, when a deck is available: with OpenDeck and Rostrum running, press Toggle mic
   and confirm the mixer strip matches Mic live / Mic muted. The four actions are Toggle mic,
   Panic mute, Switch scene, and Toggle bus mute. Select a scene or bus in the property inspector.
   This hardware check is not part of the install checks above.

## 32. Separate OBS recording tracks

Use a disposable OBS profile and scene collection, Advanced output mode, Standard recording,
and Rostrum's normal stream setup. These are checks to run, not claims of recorded sound.

1. Open OBS → Recording tracks. Each row separates the OBS track number and name, proposed
   Rostrum bus, and sources assigned in OBS. Names are labels, not source assignments. Rename
   a track in OBS Settings → Output → Audio and apply: the page picks up the name on its next
   periodic refresh without writing to OBS. Leave the page open for more than ten seconds: the controls
   remain available when outputs are stopped. Narrow the window: rows stack without clipping.
   Suggested slots are 3 Mic, 4 Game, 5 Voice, 6 Music. Choose Unused for Voice. Review, then
   Cancel: OBS is unchanged. Apply and Undo must also preserve the OBS track names.
2. Review and Apply with outputs stopped. Mic keeps tracks 1/2 and gains 3; named Game and Music
   Audio Output Captures use their bus monitors on tracks 4 and 6 only. Track 5 is disabled in
   recording output. Other sources leave tracks 3–6; their tracks 1/2, mutes, filters, and encoder
   settings remain. Shared nested scenes/groups do not need duplicate enabled placements.
3. Apply the same choices again: no OBS writes or additional captures. Undo Recording Changes
   restores previous track masks (including zero), mutes, enabled recording tracks, and scene
   placements. Undo OBS Changes remains separate from recording Undo.
4. Assign a Headphones-only bus. Its stream destination and VOD flag remain unchanged. Bus mute
   silences its recording capture; mic mute silences track 3. Stream master mute and panic mute
   silence the combined mixes while isolated playback captures keep their own bus state.
5. Run Set Up OBS again: the intended isolated bus captures are not conflict-muted. Change a
   track assignment in OBS, disconnect/reconnect or restart Rostrum: it reports the difference
   and does not overwrite OBS. Rename a bus: its saved assignment follows its id. Remove it or
   load a scene without it: Missing is shown and Apply refuses until the choice is corrected.
6. While streaming or recording, Review/Apply/Undo are unavailable. If an output starts during
   Apply, remaining writes stop; failed setup restores what it can or retains a separate Undo.
   An unwritable backup directory refuses setup before any OBS write.
7. Check readiness: it may report matching recording configuration. It still says recording
   contents and sound were not tested. Separately make a short test recording and inspect each
   selected track in an editor; this physical recording is not part of the unit test.

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
| `PIPEWIRE_REMOTE=nope rostrum` | "Rostrum can't reach PipeWire" says PipeWire is not running (no "Host is down"); each command is on one line and its Copy button puts it on the clipboard; the header reads "Mic offline" and cannot be clicked; the tray tooltip says "PipeWire missing" |
| Close the window (KWin closes it) | App keeps running in the tray |
| Start in tray (`--autostart`) | No window until Show |
| Global shortcuts through KGlobalAccel | All eight registered as `dev.getrostrum.Rostrum`; invoking Mute mic toggles the mic; a shortcut KWin owns (Meta+D) is reported in Settings and stays in-window |
| Automatic assignment with stand-in streams (Discord, Spotify, Streamer.bot, Firefox, an unknown game with `media.role = "Game"`) and the real OBS | Each lands on Voice, Music, Alerts, Desktop and Game with the reason shown; OBS is recognised and left alone; no fragment file changes |
| First-time setup, each step rendered in a headless nested KWin and offscreen at 1280×720 | Step list marks finished steps; no layout overlap; no QML warnings |
| Existing settings with `wizard_done = true` and no `setup_version` (0.1.0) | The one-time Crash Reports and Updates dialog opens over the Mixer, with no binding loops |
| Sentry build, `kill -SEGV` on a test instance (no PipeWire, private D-Bus, temp config), local server as the DSN | Send: one 3.5 KB envelope posted at the next start, file deleted; no user name, path, installation ID, registers, timestamp or device name; only the 4 libraries in the stack listed. Ask: report kept (0600), nothing posted. Never: nothing captured, no sentry folder |
| `official` preset build, same crash, sent to the real Sentry project | Sentry accepted the report (2xx) at the next start; the local file deleted; the stored event has no IP address and no location |
| Update feed 0.2.0 from a local server, as an AppImage | Download checked against SHA-256, AppImage replaced with mode 755, Restart Now offered; no cookies sent; `User-Agent: Rostrum/0.1.0` |
| Same, with a wrong checksum | Refused; the old AppImage kept; no `.part` file left |
| Fake obs-websocket server (`tst_obs`): stream, record and pause events, program scene changes, scene list changes, OBS going away | Live status follows each event and starts the LIVE clock from OBS's duration; nothing is left when OBS goes away; scene mapping ignores unknown targets; go-live problems found for a muted mic, a silent stream mix (solo included) and missing captures |
| LIVE and REC badges, go-live banner, OBS scene map, setup's OBS step and the setup version 1 → 3 and 2 → 3 dialogs, rendered offscreen with stand-in data | No QML warnings, no overlap |
| Unit tests | 11 of 11 pass |
| `tests/dbus-control.sh` (ctest `dbus_control`): Rostrum on a private bus with no service directories, offscreen, no PipeWire | Every CLI option and D-Bus method answers with the right exit status or error name; `PropertiesChanged` sent; introspection matches the XML; a hold is dropped when its caller leaves; nothing about holds or panic saved |
| Settings → Hotkeys rendered offscreen at 1100 px | Grouped as Mic, Stream and Headphones, Scenes, Buses; every row has a name and a description; new actions show None |
| Mic filters in a private PipeWire 1.6.2 + WirePlumber 0.5.13 (own runtime dir, fake mic fed by a tone, stand-in Discord, a stand-in chat app set to plain, Audacity), Rostrum offscreen on a private D-Bus | Both nodes created and adopted; graph loaded once and verified; links as in test 4 step 6; the processed mic measurably different from the bypassed one; Discord moved to the filtered mic, the plain-mic app and Audacity left on the hardware mic, including after WirePlumber restored an old target; D-Bus off and on restored and re-made every move; a control changed with `pw-cli` set back; quit and `kill -9` left the filtered mic running, and the next start moved nothing |
| Mic Filters page and the mixer's FX button, rendered offscreen at 1280×720 | No QML warnings, no overlap |
| Unit tests (with `tst_dsp` and `tst_micfilters`) | 22 of 22 pass |

Still manual (needs hardware or a real session): tests 4 (mic path), 6 and 7 (quit and reboot
routing with Discord), OBS capture, OBS while live (test 15) against a real OBS, the real headset
unplug, the GlobalShortcuts portal on a non-Plasma desktop, updates against the real
getrostrum.dev feed, test 16 (holds, panic and the OSD in a real session), and Keep on Top for
the compact window on Plasma Wayland (test 25).
