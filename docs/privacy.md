# Crash reports and updates

Rostrum makes two kinds of network request, and only if you allow them: it sends a crash report
after a crash, and it checks once a day for a new version. Both are chosen during first-run setup
and can be changed later in Settings → Privacy and Settings → Updates. Nothing else in Rostrum
touches the network, apart from obs-websocket on `localhost` (see [OBS](#obs) below).

Your audio never leaves your computer. Mic filters, noise removal included, run inside PipeWire on
your machine; RNNoise's model is part of the plugin, and no audio, level or transcript is stored or
sent. Check Mic keeps its 5-second recording in memory only and never writes it to disk.

## Crash reports

Crash reports go to [Sentry](https://sentry.io) (sentry.io, US region), a crash reporting
service. Only official builds have them: a build needs `-DROSTRUM_WITH_SENTRY=ON` and a
`-DROSTRUM_SENTRY_DSN=…`. Without both, Rostrum has no crash reporting at all and the settings
are hidden.

### Choices

| Setting (`[privacy] crash_reports`) | What happens after a crash |
| --- | --- |
| `ask` (default) | The next time Rostrum starts, a dialog offers to send the report. It can show the exact report first. |
| `send` | The report goes out quietly the next time Rostrum starts. |
| `never` | Crashes are not captured. Waiting reports and sentry's local database are deleted. |

### What is captured

Rostrum links [sentry-native](https://github.com/getsentry/sentry-native) 0.17.1, built with its
in-process (`inproc`) backend and without a transport of its own:

1. When Rostrum crashes, sentry's signal handler walks the stack and stores a crash envelope in
   `~/.local/state/rostrum/sentry/`. Then Rostrum's own handler appends the usual backtrace to
   `rostrum.log`. No minidump is made, so no memory contents are captured.
2. At the next start, sentry hands the envelope to Rostrum, which writes it to
   `~/.local/state/rostrum/crashes/crash-<time>-<n>.envelope` (mode 0600, folder 0700).
   Nothing has been sent at this point.
3. Rostrum sends it, asks, or deletes it, depending on the setting. At most 10 are kept, none
   older than 30 days.

Session tracking (a ping on every launch), breadcrumbs and client reports are switched off.
sentry-native keeps an `installation_id` file in its folder; it is never sent (see below).

### What is sent

Before anything is shown or sent, Rostrum copies a fixed list of fields out of sentry's event
(`crash::scrubEvent()` in `src/core/CrashReport.cpp`). Everything else is dropped, including
fields a later sentry-native version might add. The **Show the Report** button shows exactly this
JSON. A real example, shortened:

```json
{
  "event_id": "7b593f71cc4b4e6d594980991c051b95",
  "platform": "native",
  "level": "fatal",
  "release": "rostrum@0.1.0",
  "environment": "production",
  "sdk": { "name": "sentry.native", "version": "0.17.1", "settings": { "infer_ip": "never" } },
  "user": { "geo": {} },
  "exception": { "values": [ {
    "type": "SIGSEGV",
    "value": "Segfault",
    "mechanism": { "type": "signalhandler", "handled": false, "synthetic": true,
                   "meta": { "signal": { "name": "SIGSEGV", "number": 11 } } },
    "stacktrace": { "frames": [
      { "instruction_addr": "0x5d42d2e763d7", "image_addr": "0x5d42d2e1f000", "package": "rostrum" },
      { "instruction_addr": "0x75d342792c87", "image_addr": "0x75d342600000", "package": "libQt6Core.so.6",
        "function": "_ZN16QCoreApplication4execEv", "symbol_addr": "0x75d342792bd0" },
      { "instruction_addr": "0x75d341f28136", "image_addr": "0x75d341e00000", "package": "libc.so.6",
        "function": "ppoll", "symbol_addr": "0x75d341f280f0" }
    ] }
  } ] },
  "contexts": {
    "os": { "name": "Linux", "version": "7.0.0", "build": "38-generic",
            "distribution_name": "ubuntu", "distribution_version": "26.04" },
    "rostrum": { "install": "source", "arch": "x86_64", "desktop": "KDE", "session": "wayland",
                 "qt": "6.10.2", "kf": "6.24.0", "pipewire": "1.6.2", "wireplumber": "0.5.13" }
  },
  "debug_meta": { "images": [
    { "type": "elf", "code_file": "rostrum", "image_addr": "0x5d42d2e1f000", "image_size": 11010048,
      "code_id": "…", "debug_id": "…" }
  ] }
}
```

| Kept | Cleaning |
| --- | --- |
| `event_id` | A random ID for this report only. Lowercase hex. |
| `release`, `environment`, `sdk` | Letters, digits and `. _ + @ -` only |
| Signal (`type`, `value`, `mechanism`) | Signal names in capitals; short words with no `/` |
| Stack frames, 128 at most (the crash end) | Addresses as hex; `package` cut to the file name; `function` only if it looks like a C or C++ symbol |
| `contexts.os` | Kernel and distribution name and version; sanitized |
| `contexts.rostrum` | Install type, CPU architecture, desktop, session type, Qt, KDE Frameworks, PipeWire and WirePlumber versions; sanitized |
| `debug_meta.images` | Only libraries the stack runs through. File name only, plus address, size and build IDs. |

Two fields are always added, not copied. Sentry sees the IP address every upload comes from, and
by default it stores it as the user's address and looks up a city from it. `"infer_ip": "never"`
stops the first. The empty `user.geo` stops the second: Sentry only looks up a location for events
that have none, and its "Prevent Storing of IP Addresses" setting does not stop the lookup.

"Sanitized" means letters, digits, space and `. _ : + ( ) / -` only, at most 64 characters. A
file name with anything other than letters, digits and `. _ + -` becomes `?`.

Dropped, among everything else: sentry-native's `user` (the installation ID), the
timestamp, CPU registers, trace IDs, tags, extras, breadcrumbs, the host name, and the list of
other libraries loaded into Rostrum (it would show what else is installed, such as overlays).

The memory addresses are kept because Sentry needs them to find the line of code. Address space
layout randomisation changes them every time Rostrum starts, so they say nothing about you and
cannot link two reports. Frames in Rostrum itself have no function name until the release's debug
symbols are uploaded to Sentry (`sentry debug-files upload`). Sentry then matches them by
`debug_id`.

`tests/tst_privacy.cpp` feeds the scrubber an event shaped like sentry-native's, with a user ID,
home folder paths, a device name, registers, a host name and an unrelated library added, and
checks that none of them survive.

### How it is sent

- `POST` to the DSN's envelope endpoint (`https://<host>/api/<project>/envelope/`) with
  `Content-Type: application/x-sentry-envelope`, `User-Agent: Rostrum/<version>` and an
  `X-Sentry-Auth` header carrying the DSN's public key. Cookies are neither sent nor stored, and
  redirects may not downgrade from HTTPS.
- A 2xx reply deletes the report. So does a 4xx other than 429, so a report Sentry rejects is not
  retried forever. A network error, 429 or 5xx keeps it for the next start.
- Each request times out after 15 seconds, and failures are silent.

### Sentry project settings

Set these in the Sentry project so the promise in the app holds on the server too:

- **Security & Privacy → Prevent Storing of IP Addresses:** on. Reports never contain an IP
  address, but every HTTP request has one.
- **Security & Privacy → Data Scrubber** and **Use Default Scrubbers:** on.
- **Security & Privacy → Advanced Data Scrubbing:** add `[Remove] [Anything] from [$user.geo.**]`,
  as a server-side backup to the empty `user.geo` Rostrum sends.
- **Client Keys (DSN) → Rate limit:** the DSN is public inside every build, so cap it (for example
  500 events an hour) so nobody can use up the quota.
- **Data retention:** as short as the plan allows.
- Upload debug symbols for each release so Rostrum's own frames get function names and lines.

## Updates

### Choices

| Setting (`[updates]`) | Default | Meaning |
| --- | --- | --- |
| `check` | `true` | Check for a new version once a day |
| `install` | `true` | Download and install new versions by itself (AppImage only) |
| `skipped_version` | empty | Set by "Skip This Version"; that version is not offered again |
| `last_check` | 0 | Unix time of the last successful check |

The first check runs 20 seconds after Rostrum starts, then at most once every 24 hours while it
runs. It is a plain `GET` of the release feed with `User-Agent: Rostrum/<version>` and no cookies.

What happens next depends on how Rostrum was installed:

| Install | Detected by | On a new version |
| --- | --- | --- |
| AppImage | `$APPIMAGE` is set | Downloads it, checks its SHA-256, replaces the AppImage file, and offers **Restart Now**. With `install = false`, a banner offers **Install**. |
| Package | Binary under `/usr` (not `/usr/local`), or `$SNAP` is set | A banner says to update from the software center or package manager |
| Source | Anything else | A banner links to what changed |
| Flatpak | `$FLATPAK_ID` or `/.flatpak-info` | Rostrum does not check. Flatpak updates it. |

An AppImage update only goes ahead if the feed gives a full SHA-256 checksum and an HTTPS URL. The
download goes to a hidden `.part` file in the same folder, is hashed while it downloads (512 MiB at
most), and only replaces the running AppImage once the checksum matches. If the folder is not
writable, Rostrum only announces the update. Pre-releases and versions with a suffix such as
`-rc1` are never offered.

### Feed format

`ROSTRUM_UPDATE_URL` (default `https://getrostrum.dev/releases/latest.json`) can serve either of
these:

```json
{
  "version": "0.2.0",
  "date": "2026-10-20",
  "notes": "What changed, as plain text.",
  "url": "https://getrostrum.dev/releases/0.2.0",
  "appimage": {
    "x86_64":  { "url": "https://…/Rostrum-0.2.0-x86_64.AppImage",  "sha256": "<64 hex>", "size": 41943040 },
    "aarch64": { "url": "https://…/Rostrum-0.2.0-aarch64.AppImage", "sha256": "<64 hex>", "size": 40894464 }
  }
}
```

The release workflow generates this file and attaches it to each GitHub release, and the default
URL is meant to redirect to the newest one
([releasing.md](releasing.md#where-the-updater-finds-the-feed)). The check then talks to
getrostrum.dev and GitHub, and AppImage downloads come from GitHub.

Or a GitHub "latest release" response
(`https://api.github.com/repos/<owner>/<repo>/releases/latest`). Rostrum reads `tag_name`,
`html_url`, `body` and `published_at`, skips drafts and pre-releases, and picks the asset whose
name ends in `-<arch>.AppImage`, using GitHub's `digest` (`sha256:…`) as the checksum.

## OBS

Rostrum talks to OBS through obs-websocket on `127.0.0.1` only, on the port and with the password
in OBS's own config. That traffic never leaves your computer. It is used in two ways:

- **Set Up OBS and Undo** on the OBS page, when you press them. These are the only requests that
  change OBS.
- **Following OBS while it runs** (Settings → OBS, `[obs] background`, on by default; also offered
  in first-run setup and in the one-time dialog after upgrading). While an OBS process runs with
  its WebSocket server turned on, Rostrum stays connected and only reads: whether OBS is streaming
  or recording and for how long, the scene on program, and the scene list, for the LIVE and REC
  badges, go-live warnings and scene mapping. Nothing is stored apart from the scene mapping you
  choose. When OBS is not running, no connection is attempted. Turn the switch off and Rostrum
  connects only while the OBS page is open.

Runs with `QT_QPA_PLATFORM=offscreen` or `ROSTRUM_SCREENSHOT` never connect in the background.

## Builds and testing

Official builds use the `official` preset, which turns crash reports on with the DSN of Rostrum's
Sentry project (US region), builds with debug info, and compiles RNNoise 0.2 into the mic filter
plugin (downloaded at configure time and checked against a pinned SHA-256, like sentry-native):

```sh
cmake --preset official
cmake --build --preset official
```

Then upload the release's debug symbols, so Sentry can name Rostrum's own frames. This uses the
[Sentry CLI](https://cli.sentry.dev) (`sentry`), signed in once with `sentry auth login`; in CI,
set `SENTRY_AUTH_TOKEN` instead and keep the token out of the repository:

```sh
SENTRY_ORG=rostrum SENTRY_PROJECT=rostrum sentry debug-files upload --wait build-official/src/app/rostrum
```

Upload the binary before stripping it. A stripped copy keeps the same `debug_id`, so reports from
it still match. A fork or other distribution builds with its own Sentry project, or none:

```sh
cmake -S . -B build -DROSTRUM_WITH_SENTRY=ON \
  -DROSTRUM_SENTRY_DSN=https://<key>@o<org>.ingest.us.sentry.io/<project> \
  -DROSTRUM_UPDATE_URL=https://example.org/latest.json
```

`ROSTRUM_WITH_SENTRY` downloads the pinned sentry-native release at configure time and checks its
SHA-256. For offline builds, set `FETCHCONTENT_SOURCE_DIR_SENTRY` to an unpacked copy of the same
release. A system-wide sentry-native is deliberately not used: one built with the crashpad
backend would upload crashes by itself, ignoring the user's choice.

The environment variables `ROSTRUM_SENTRY_DSN` and `ROSTRUM_UPDATE_URL` override the built-in
values at run time; `ROSTRUM_SENTRY_DEBUG=1` turns on sentry-native's log. Only `https://` URLs are
used, plus `http://` to `localhost` for testing. Runs with `QT_QPA_PLATFORM=offscreen` or
`ROSTRUM_SCREENSHOT` never touch the network unless one of the variables is set.
