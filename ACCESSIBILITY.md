# Accessibility

Streaming should not depend on being able to see a meter or use a mouse. Rostrum aims to be fully
usable with a keyboard and with a screen reader, and we treat an accessibility barrier as a bug.
This page says what works today, what does not yet, and how to tell us about a problem.

## Supported environments

- **Desktop:** KDE Plasma 6 on Wayland is the development and test environment. X11 is a fallback.
  GNOME and other desktops running PipeWire are in scope, but get less testing.
- **Toolkit:** Rostrum's interface is Qt Quick with KDE's Kirigami, using the Plasma style when it
  is available. Qt exposes the interface to assistive technology through AT-SPI, the standard Linux
  accessibility bus.
- **Screen reader:** Orca is the screen reader we design for.
- **Display:** Rostrum follows the desktop's color scheme, fonts and scale factor. The smoke tests
  include a 200 % scale factor at 1280×720 with all six bus strips fitting and no overlapping
  controls.

## What is implemented

### Screen readers

Custom controls set an accessible name and, where Qt cannot work it out, a role:

- **Faders** are announced with their bus, for example "Game volume" or "Mic gain". Master faders
  ("Headphones volume", "Stream volume") and per-app volume sliders on the Apps page work the same
  way.
- **Mute and solo** buttons are named per bus ("Game mute", "Game solo") and report whether they
  are on. The sidetone switch and its volume are named too.
- **Destinations** are a named group ("Game destination") of radio buttons for Headphones, Stream
  and Both.
- **Meters** are exposed as progress bars. Their description gives the level in words: "Silent",
  a peak level in dB such as "-12 dB", or "Clipping". It updates at most twice a second so it does
  not flood the screen reader.
- **Apps:** each row is read as the app and its bus ("Firefox, on Music"). App chips on the Mixer
  give the reason when Rostrum placed an app automatically.
- **Navigation:** the sidebar is a list of page tabs, and the scene switcher says the current scene
  and whether it has unsaved changes.
- **First-time setup:** each step says whether it is done or current. The mix diagram has a text
  summary, and its decorative parts are hidden from assistive technology.
- **Status:** the PipeWire connection status in the status bar is readable as text.

### Keyboard

The Mixer can be driven from the keyboard alone. Tab moves between controls, and faders and
destination buttons draw a focus ring. A keyboard-only run (F6 to the page, Tab to the Game fader,
Page Down twice, M, 2, Ctrl+S) is part of the smoke tests in
[docs/manual-tests.md](docs/manual-tests.md).

| Where | Key | Action |
| --- | --- | --- |
| Any page | F6 / Shift+F6 | Move focus between the header, the sidebar and the page |
| Any page | Ctrl+S | Save the scene |
| Any page | Ctrl+M | Mute or unmute the mic |
| Any page | Ctrl+Q | Quit |
| Bus fader | Up / Down (or Right / Left) | Level up or down 1 % |
| Bus fader | Page Up / Page Down | Level up or down 10 % |
| Bus fader | M / S | Mute / solo |
| Bus fader | 1 / 2 / 3 | Send to Headphones / Stream / Both |
| Bus fader | F2 | Rename the bus |
| Bus fader | Menu key | Open the bus's context menu |
| Master fader | Right / Left | Level up or down 1 % |
| Master fader | Page Up / Page Down, M | Level up or down 10 %, mute |
| Sidebar | Enter or Space | Open the selected page |
| Scene switcher, header menu | Enter or Space | Open the menu |
| Scenes page | Enter | Load the selected scene |
| Renaming | Escape | Cancel |
| Settings → Hotkeys | Backspace | Clear the focused shortcut |

A fader's tooltip lists its keys when it has focus. Global shortcuts (mute mic, mute all playback
to stream, previous and next scene, load scenes 1–4) work from any app and can be rebound in
Settings, or in System Settings → Shortcuts on Plasma. The Shortcuts section of the
[README](README.md) lists the defaults.

Dragging an app chip between buses is mouse-only, but there are keyboard routes to the same result:
each chip has an options button with **Move to** and **Unassign**, and every app on the Apps page
has a button to assign it to a bus.

### Not by color alone

- A muted strip says "Muted", and a strip silenced by another bus's solo says "Dimmed by solo".
- A muted mic shows a slashed-mic badge on the tray icon, and the tray tooltip says "Mic muted".
- Meter clipping is also given in words to screen readers.

### Other

- Settings → Mixer → Meter speed → Low halves the meter update rate (and uses less power).
- Settings → General → "Scroll to adjust faders" can be turned off, so scrolling the window never
  changes a level by accident.
- Screenshots in the README have text descriptions.

## Known limitations

- **Not yet verified with Orca end to end.** The accessible names and roles are in the code, but no
  one has yet run through a full streaming session with Orca. Reports from screen reader users are
  especially welcome.
- **Meters are visual first.** The spoken level is the peak in whole dB, at most twice a second.
  It tells you whether a bus has signal and roughly how loud it is, but it is not a replacement for
  listening. The clip mark on screen lasts 1.5 seconds.
- **Meter zones use color.** The amber (from -12 dB) and red (from -6 dB) zones, and the bus colors,
  are shown only as color on screen.
- **Custom-drawn controls.** Faders, meters and strip toggles are drawn by Rostrum. They take their
  colors from the desktop theme but have not been checked against high-contrast color schemes.
- **Motion.** There is no setting of Rostrum's own to reduce motion; meters move while audio plays.
  Lowering the meter speed makes them update less often.
- **Other desktops.** On GNOME and other desktops without KDE's global shortcut service, global
  shortcuts go through the XDG GlobalShortcuts portal. That path is untested so far.
- **Translations.** Interface text is prepared for translation, but no translations ship yet.

## Reporting a barrier

Open an [accessibility issue](https://github.com/rostrum-audio/rostrum/issues/new?template=accessibility.yml).
Tell us what you were trying to do, what got in the way, and what assistive technology you use.
If GitHub's forms are hard to use, email **hello@getrostrum.dev** instead.

A barrier that blocks a core task (mixing, muting the mic, switching scenes, setting up OBS) is
treated as a high-priority bug.
