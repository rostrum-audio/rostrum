# Crash reports and updates

Rostrum makes two kinds of network request, and only if you allow them: it sends a crash report
after a crash, and it checks once a day for a new version. Both are chosen during first-run setup
and can be changed later in Settings → Privacy and Settings → Updates. Nothing else in Rostrum
touches the network, apart from obs-websocket on `localhost`.

## Crash reports

### Choices

| Setting (`[privacy] crash_reports`) | What happens after a crash |
| --- | --- |
| `ask` (default) | The next time Rostrum starts, a dialog offers to send the report. It can show the exact report first. |
| `send` | The report goes out quietly the next time Rostrum starts. |
| `never` | Crash files are deleted at the next start and nothing is sent. |

### What is captured

When Rostrum crashes, its signal handler writes
`~/.local/state/rostrum/crashes/crash-<time>.txt` (mode 0600) and the usual backtrace in
`rostrum.log`. The handler only writes data that was prepared before the crash: the version, the
build id, how Rostrum was installed, and the system versions listed below. The crash file stays on
your computer. Rostrum keeps at most 10 of them, and none older than 30 days.

### What is sent

The report is built from the crash file by copying a fixed list of fields. Every other line is
ignored, so even if the file held something private, it could not reach the report. An example,
exactly as sent:

```json
{
  "schema": 1,
  "date": "2026-10-03",
  "app": { "version": "0.1.0", "build_id": "2615ebc32d3f3d30705b9729536fae62112f393d", "install": "source" },
  "crash": {
    "signal": "SIGSEGV",
    "uptime_seconds": 16,
    "frames": [
      "rostrum +0x5aa4b",
      "libc.so.6 ppoll+0x46",
      "libQt6Core.so.6 _ZN16QCoreApplication4execEv+0xb7",
      "rostrum +0x5799d"
    ]
  },
  "system": {
    "os": "Ubuntu 26.04", "kernel": "7.0.0-38-generic", "arch": "x86_64",
    "desktop": "KDE", "session": "wayland",
    "qt": "6.10.2", "kf": "6.24.0", "pipewire": "1.6.2", "wireplumber": "0.5.13"
  }
}
```

| Field | Source | Cleaning |
| --- | --- | --- |
| `date` | Crash time | Day only, in UTC. No time of day. |
| `app.version` | Build | Allowed characters only, 64 at most |
| `app.build_id` | GNU build id of the binary | Lowercase hex only, otherwise dropped |
| `app.install` | `source`, `package`, `flatpak` or `appimage` | As above |
| `crash.signal` | The signal | Sent as its name, such as `SIGSEGV` |
| `crash.uptime_seconds` | Seconds since Rostrum started | A number |
| `crash.frames` | The backtrace, 64 frames at most | See below |
| `system.os` | `NAME` and `VERSION_ID` from `/etc/os-release` | Allowed characters only, 64 at most |
| `system.kernel`, `system.arch` | `QSysInfo` | As above |
| `system.desktop`, `system.session` | `XDG_CURRENT_DESKTOP`, `XDG_SESSION_TYPE` | As above |
| `system.qt`, `system.kf` | Qt and KDE Frameworks runtime versions | As above |
| `system.pipewire`, `system.wireplumber` | Versions Rostrum read from PipeWire | As above |

"Allowed characters" means letters, digits, space and `. _ : + ( ) / -`.

Each frame becomes `<library file name> <symbol>+<offset>`:

- The folder is removed from the library path, so `/home/alex/.local/bin/rostrum` becomes
  `rostrum`. A file name with anything other than letters, digits and `. _ + -` becomes `?`.
- A symbol must look like a C or C++ symbol name with an optional `+0x` offset, or it becomes `?`.
- The absolute address (`[0x55d1c84f2a1]`) is dropped. With address space layout randomisation it
  would make each report unique.
- Offsets inside Rostrum's own binary are enough to find the line: match `app.build_id` to the
  release's debug symbols and run `addr2line -e rostrum -f -C 0x5aa4b`.

The following are never read and never sent: user names, home folders and file paths, host names,
app, device, bus and scene names, settings, logs, audio, clipboard contents, and any per-install or
per-user ID. Reports carry no ID, so two reports from the same computer cannot be linked.
`tests/tst_privacy.cpp` checks this, including that a crash file containing a user name, a device
name and a raw address produces a report with none of them.

### How it is sent

- `POST` to `https://getrostrum.dev/api/v1/crash-reports` with `Content-Type: application/json`
  and `User-Agent: Rostrum/<version>`. Cookies are neither sent nor stored, and redirects may not
  downgrade from HTTPS.
- On a 2xx reply, the crash file is deleted. A 4xx reply also deletes it, so a report the server
  rejects is not retried forever. A network error or 5xx keeps the file for the next start.
- Each request times out after 15 seconds and failures are silent.

### What the server must do

The endpoint is not part of this repository. Whatever runs it should:

- Accept `schema: 1` JSON up to about 64 KiB, and answer 2xx, or 400 for anything malformed.
- Not log or store client IP addresses. The report itself never contains one, and the promise in
  the app depends on the server keeping it that way.
- Delete reports once they are no longer needed for fixing bugs.

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

Or a GitHub "latest release" response
(`https://api.github.com/repos/<owner>/<repo>/releases/latest`). Rostrum reads `tag_name`,
`html_url`, `body` and `published_at`, skips drafts and pre-releases, and picks the asset whose
name ends in `-<arch>.AppImage`, using GitHub's `digest` (`sha256:…`) as the checksum.

## Builds and testing

The endpoints are CMake cache variables, so a distribution or fork can point them at its own
servers:

```sh
cmake -S . -B build -DROSTRUM_UPDATE_URL=https://example.org/latest.json \
  -DROSTRUM_CRASH_URL=https://example.org/crash-reports
```

The environment variables of the same names override them at run time. Only `https://` URLs are
used, plus `http://` to `localhost` for testing. Runs with `QT_QPA_PLATFORM=offscreen` or
`ROSTRUM_SCREENSHOT` never touch the network unless one of the variables is set.
