#!/usr/bin/env bash
# Extracts every i18n string from the C++ and QML sources into rostrum.pot. KDE's scripty runs
# this with its own $XGETTEXT and $podir; elsewhere run it directly, or use
# `cmake --build build --target rostrum-pot`.
set -euo pipefail
cd "$(dirname "$0")/.."

podir=${podir:-$PWD/po}
if [ -z "${XGETTEXT:-}" ]; then
    version=$(sed -n 's/^project(rostrum VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
    XGETTEXT="xgettext --from-code=UTF-8 -C --kde -ci18n \
        -ki18n:1 -ki18nc:1c,2 -ki18np:1,2 -ki18ncp:1c,2,3 \
        -kki18n:1 -kki18nc:1c,2 -kki18np:1,2 -kki18ncp:1c,2,3 \
        -kxi18n:1 -kxi18nc:1c,2 -kxi18np:1,2 -kxi18ncp:1c,2,3 \
        --package-name=rostrum --package-version=$version \
        --msgid-bugs-address=https://github.com/rostrum-audio/rostrum/issues"
fi

mapfile -t sources < <(find src \( -name '*.cpp' -o -name '*.h' -o -name '*.qml' \) | LC_ALL=C sort)
# shellcheck disable=SC2086 # $XGETTEXT is a command line with options
$XGETTEXT "${sources[@]}" -o "$podir/rostrum.pot"
