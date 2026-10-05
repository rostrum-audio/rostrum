# AppImage dependencies, licenses and corresponding source

This document ships in `usr/share/licenses/rostrum/DEPENDENCIES.md`. Rostrum's
source is Apache-2.0; this does not replace the licenses of bundled dependencies.
Qt/KDE libraries use their applicable LGPL/GPL alternatives and embedded third-party
licenses. They are dynamically linked. Other bundled libraries and resources retain
their package copyright notices, including BSD, MIT, Apache, MPL and other licenses.

## Exact versions and notices

`manifest.json` alongside this file records the binary/source package versions,
source download pages and build IDs of the ELF dependencies actually packaged.
The builder resolves each dependency to an installed package by filename **and ELF
build ID**, failing if it cannot establish provenance. It preserves distro-provided
copyright files under `usr/share/doc/<package>/copyright`. Those files are derived
from the corresponding upstream sources, cover embedded third-party code and give
applicable license alternatives. Full referenced GNU/other common-license texts
are included under `usr/share/common-licenses`, including the GPL text incorporated
by each LGPL version. Breeze and RNNoise's existing notices are preserved.

The official build also includes these pinned sources, obtained and hash-checked
by CMake. Their own upstream notices are copied without rewriting their terms:

- Sentry native **0.17.1**, MIT:
  https://github.com/getsentry/sentry-native/releases/tag/0.17.1 .
  `Sentry-LICENSE` plus licenses from that release for compiled mpack, jsmn,
  stb_sprintf and vendored libunwind are supplied alongside this file. These
  source files include additional MIT notices (stb also offers public domain).
  `Sentry-libunwind-components-NOTICES` preserves complete upstream component
  headers, including David Mosberger-Tang notices absent from top-level COPYING.
  Component collections conservatively include conditional source/header notices
  from the exact pinned source trees, without changing their terms.
  The in-process backend does not include Crashpad or Breakpad; their dependencies
  are not bundled by this build. No Sentry transport is built into the SDK.
- RNNoise **0.2**, BSD-3-Clause:
  https://github.com/xiph/rnnoise/releases/tag/v0.2 .
  Its upstream `COPYING` is `RNNoise-COPYING`; it is statically linked into the DSP.
  `RNNoise-components-NOTICES` preserves component copyright and license headers,
  including CSIRO notices absent from the top-level COPYING.
- toml++'s installed development package supplies headers compiled into Rostrum;
  its version, source and copyright notice are also recorded, even when a runtime
  library is present. The applicable upstream license is MIT.

The statically linked AppImage launcher is pinned to runtime source commit
`8f39b89e2ac31e1640b3d3f7e9a5108e6ce805fa` and SHA-256
`156f4bdbde9c52d01814600013e0a273f0118dc2de98975f3c8c63427ec79074`.
Its upstream [build evidence](https://github.com/AppImage/type2-runtime/actions/runs/36463736478)
records musl 1.2.5-r11, zlib 1.3.2-r0, zstd 1.5.6-r2 and mimalloc2 2.1.7-r0.
The runtime source pins libfuse 3.15.0 (with its documented mount.c patch) and
squashfuse 0.5.2. Version-matched upstream runtime/MIT, musl/MIT, libfuse/LGPL-2.1,
squashfuse/BSD, zstd/BSD, zlib and mimalloc/MIT notices ship alongside this file;
`manifest.json` records their source URLs and hashes. The generated AppRun and Qt hook also retain the MIT notices from the pinned
linuxdeploy and Qt-plugin versions used to generate them; the tooling binaries
are not bundled. The launcher build recipe
and libfuse patch are at that exact runtime commit. Alpine package patches can be
retrieved from the v3.21 source-package recipes at https://gitlab.alpinelinux.org/alpine/aports .
The runtime uses LGPL code statically: rebuild the launcher with a modified
libfuse using that public recipe, then use appimagetool's `--runtime-file` option
to combine it with your extracted/rebuilt AppDir. No proprietary relinking objects
are needed. Preserve the corresponding runtime/library sources and build scripts
when redistributing modified launchers.

Qt's licensing explanation: https://www.qt.io/development/open-source-lgpl-obligations .
Review the exact component notices rather than assuming all files in a source
package share one license; tool/test license alternatives do not automatically
become runtime dependencies.

## Obtaining corresponding source

For each distro-built component use the **source package and source version** in
`manifest.json`, not the binary version alone or the latest upstream release.
Its Launchpad source page provides the original upstream archive, Debian/Ubuntu
packaging changes and `.dsc` metadata. Download all files referenced by the `.dsc`;
`dpkg-source -x <file>.dsc` extracts the corresponding patched source. In a matching
Ubuntu environment with source repositories enabled, an equivalent command is:

```
apt-get source SOURCE_PACKAGE=SOURCE_VERSION
apt-get build-dep SOURCE_PACKAGE=SOURCE_VERSION
```

Rostrum's exact source commit is recorded with each release's validation evidence;
GitHub's tag source archive provides its source, build scripts and configuration.
The workflow uses `cmake --preset official` and `cmake --build --preset official`;
[releasing.md](https://github.com/rostrum-audio/rostrum/blob/main/docs/releasing.md)
records builder packages. Pinned Sentry/RNNoise archives and hashes appear in
`src/app/CMakeLists.txt` and `src/dsp/CMakeLists.txt`. Fetch those exact archives to
reproduce their static integrations; changing them requires rebuilding the linked
executable or DSP plugin. Preserve corresponding source archives and patches when
redistributing this AppImage; distributors must maintain applicable source access,
not substitute a link to an unrelated current version. Launchpad is the source
location for the unmodified distro packages; if you modify them, supply your changes
and corresponding build instructions too.

## Replacing dynamically linked libraries / relinking

Users may inspect, modify and replace LGPL components and debug their changes.
Extract a **copy** of the AppImage into a writable directory:

```
./Rostrum-0.1.0-x86_64.AppImage --appimage-extract
```

The resulting `squashfs-root/usr/lib`, `usr/plugins` and `usr/qml` contain the
bundled libraries, plugins and QML modules. Replace compatible library files and
associated plugins/resources there, preserving names/SONAMEs and directory layout,
then run `squashfs-root/AppRun`. The AppImage's immutable filesystem is not a
restriction on replacing libraries after extraction. Rebuild the matching Qt/KDE
source when needed; ABI-incompatible replacements can require rebuilding Rostrum
and other dependent libraries. For relinking, the public Rostrum source and its
CMake build scripts are available under Apache-2.0; use matching dependencies and
the documented build configuration. No proprietary object files are required.
The DSP plugin is separately installed into the private host filter location at
runtime; replacing statically linked RNNoise requires rebuilding that plugin.

Fully quit an existing Rostrum instance before a deliberate live-session launch.
Extraction, inspection and rebuilding do not require launching the application.
Use disposable configuration and a fully private PipeWire/D-Bus session for
experiments; changing XDG configuration paths alone does not isolate audio.

## Verification scope

The packaging audit rejects missing/empty mandatory notices, missing referenced
full license texts, altered notice hashes and unaccounted dependency ELF files.
It is an inventory/completeness check, not a legal opinion or proof that external
source archives will remain available forever. It does not claim that arbitrary
replacement libraries are ABI-compatible. The maintainer must preserve exact
source availability and review the inventory before public distribution.
