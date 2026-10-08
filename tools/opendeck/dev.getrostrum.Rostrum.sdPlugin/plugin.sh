#!/bin/sh
# OpenDeck owns the socket/device. Flatpak runs the control client on the host.
set -eu
plugin_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -n "${FLATPAK_ID:-}" ]; then
    exec flatpak-spawn --host python3 "$plugin_dir/plugin.py" "$@"
fi
exec python3 "$plugin_dir/plugin.py" "$@"
