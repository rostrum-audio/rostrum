#!/bin/bash
# Packages an installed Rostrum build into Rostrum-<version>-<arch>.AppImage with linuxdeploy.
#
#   tools/appimage/build-appimage.sh <build-dir> <version> [output-dir]
#
# Needs the build's Qt and KDE Frameworks development packages (qmake, qmlimportscanner and the
# QML modules the app imports), plus curl, file and patchelf. Runs without FUSE.
set -euo pipefail

if [ $# -lt 2 ]; then
    echo "usage: $0 <build-dir> <version> [output-dir]" >&2
    exit 2
fi

src="$(cd "$(dirname "$0")/../.." && pwd)"
build="$(cd "$1" && pwd)"
version="$2"
out="$(mkdir -p "${3:-$PWD}" && cd "${3:-$PWD}" && pwd)"
arch="$(uname -m)"
app_id="dev.getrostrum.Rostrum"
# The updater picks the release asset whose name ends in "-<arch>.AppImage".
name="Rostrum-${version}-${arch}.AppImage"

# Pinned tools, checked before they run.
linuxdeploy_tag="1-alpha-20251107-1"
linuxdeploy_sha256="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"
plugin_qt_tag="1-alpha-20250213-1"
plugin_qt_sha256="15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724"
if [ "$arch" != "x86_64" ]; then
    echo "Only x86_64 has pinned checksums; add them for $arch." >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
tools="${ROSTRUM_APPIMAGE_TOOLS:-$work/tools}"
mkdir -p "$tools"

fetch() {
    local file="$1" url="$2" sha="$3"
    if [ ! -f "$tools/$file" ]; then
        curl -fsSL --retry 3 -o "$tools/$file" "$url"
    fi
    echo "$sha  $tools/$file" | sha256sum -c -
    chmod +x "$tools/$file"
}
fetch "linuxdeploy-${arch}.AppImage" \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/${linuxdeploy_tag}/linuxdeploy-${arch}.AppImage" \
    "$linuxdeploy_sha256"
fetch "linuxdeploy-plugin-qt-${arch}.AppImage" \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/${plugin_qt_tag}/linuxdeploy-plugin-qt-${arch}.AppImage" \
    "$plugin_qt_sha256"

appdir="$work/AppDir"
DESTDIR="$appdir" cmake --install "$build" --prefix /usr

# PipeWire loads the mic filter plugin into its own process on the host, so the plugin can use
# nothing from inside the AppImage: only libc and libm.
dsp="$(find "$appdir/usr" -name librostrum-dsp.so -print -quit)"
if [ -z "$dsp" ]; then
    echo "librostrum-dsp.so is missing from the install" >&2
    exit 1
fi
needed="$(readelf -d "$dsp" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | grep -vxE 'libc\.so\.6|libm\.so\.6' || true)"
if [ -n "$needed" ]; then
    echo "librostrum-dsp.so needs $needed; configure with -DROSTRUM_RNNOISE=bundled" >&2
    exit 1
fi

# qmlimportscanner only sees what QML files import. The Rostrum module is compiled into the
# binary, so scan its sources, plus the style and modules that are loaded from C++ or only
# by other modules at run time.
qml_scan="$work/qml"
mkdir -p "$qml_scan"
cp "$src"/src/app/qml/*.qml "$qml_scan/"
cat > "$qml_scan/AppImageImports.qml" <<'EOF'
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Templates
import QtQuick.Window
import QtCore
import QtQml.WorkerScript
import org.kde.desktop
import org.kde.kirigami
import org.kde.kirigamiaddons.formcard
import org.kde.kitemmodels
import org.kde.kquickcontrols
Item {}
EOF

# The app finds translations through the XDG data dirs, which do not include the AppImage.
mkdir -p "$appdir/apprun-hooks"
cat > "$appdir/apprun-hooks/rostrum-data-dirs.sh" <<'EOF'
export XDG_DATA_DIRS="$APPDIR/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
EOF

qt_bin="$(dirname "$(readlink -f "$(command -v qmake6 || command -v qmake)")")"
export QMAKE="${QMAKE:-$qt_bin/qmake}"
libexec="$("$QMAKE" -query QT_INSTALL_LIBEXECS)"
plugins="$("$QMAKE" -query QT_INSTALL_PLUGINS)"
export PATH="$libexec:$qt_bin:$PATH"
export QML_SOURCES_PATHS="$qml_scan"
# Qt 6.10 has one Wayland platform plugin; older Qt split it in two. Offscreen lets the release
# workflow (and ROSTRUM_SCREENSHOT) start the AppImage without a display.
platforms="libqoffscreen.so"
for p in "$plugins"/platforms/libqwayland*.so; do
    [ -e "$p" ] && platforms="$platforms;$(basename "$p")"
done
export EXTRA_PLATFORM_PLUGINS="$platforms"
export APPIMAGE_EXTRACT_AND_RUN=1
export LDAI_OUTPUT="$out/$name"
export ARCH="$arch"
export VERSION="$version"

"$tools/linuxdeploy-${arch}.AppImage" \
    --appdir "$appdir" \
    --executable "$appdir/usr/bin/rostrum" \
    --desktop-file "$appdir/usr/share/applications/${app_id}.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/scalable/apps/${app_id}.svg" \
    --plugin qt

# The Qt plugin does not bring the SVG icon engine (theme icons are SVG) or the Wayland client's
# shell and decoration plugins, without which a Wayland window cannot open.
extra=()
for dir in iconengines wayland-shell-integration wayland-decoration-client wayland-graphics-integration-client; do
    if [ -d "$plugins/$dir" ]; then
        mkdir -p "$appdir/usr/plugins/$dir"
        cp -n "$plugins/$dir"/*.so "$appdir/usr/plugins/$dir/"
        extra+=(--deploy-deps-only "$appdir/usr/plugins/$dir")
    fi
done

"$tools/linuxdeploy-${arch}.AppImage" --appdir "$appdir" "${extra[@]}" --output appimage

test -s "$out/$name"
chmod 755 "$out/$name"
echo "$out/$name"
