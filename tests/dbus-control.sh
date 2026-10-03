#!/bin/sh
# Drives the D-Bus control interface and the command line of a Rostrum started on a private
# session bus, offscreen, with no PipeWire, no update check and a throwaway config. It never
# reaches the desktop's own session bus or a Rostrum already running there.
#
#   tests/dbus-control.sh build/src/app/rostrum
set -u

if [ "${1:-}" != "--inside" ]; then
    B=$(realpath "${1:-build/src/app/rostrum}")
    T=$(mktemp -d)
    trap 'rm -rf "$T"' EXIT
    export XDG_CONFIG_HOME="$T/config" XDG_STATE_HOME="$T/state" XDG_DATA_HOME="$T/data"
    export PIPEWIRE_REMOTE=rostrum-test-none QT_QPA_PLATFORM=offscreen ROSTRUM_NO_GLOBAL_SHORTCUTS=1
    mkdir -p "$XDG_CONFIG_HOME/rostrum"
    printf 'format = 1\n[general]\nwizard_done = true\nsetup_version = 2\n[updates]\ncheck = false\n' \
        >"$XDG_CONFIG_HOME/rostrum/settings.toml"
    # No service directories: nothing (portal, KWallet, accessibility bus) is activated and outlives the bus.
    cat >"$T/bus.conf" <<'EOF'
<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=/tmp</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF
    dbus-run-session --config-file="$T/bus.conf" -- sh "$0" --inside "$B"
    exit $?
fi

B=$2
DEST=dev.getrostrum.Rostrum
OBJ=/dev/getrostrum/Rostrum/Control
IFACE=dev.getrostrum.Rostrum1
failures=0

check() { # description, expected, actual
    if [ "$2" = "$3" ]; then
        echo "ok   $1"
    else
        echo "FAIL $1: expected [$2], got [$3]"
        failures=$((failures + 1))
    fi
}
status() { "$@" >/dev/null 2>&1; echo $?; }
prop() { gdbus call --session --dest $DEST --object-path $OBJ --method org.freedesktop.DBus.Properties.Get $IFACE "$1"; }

# Without a running Rostrum, lists come from disk and nothing is written.
check "offline scene list" "Live" "$("$B" --list-scenes)"
check "offline query writes nothing" "no" "$([ -d "$XDG_CONFIG_HOME/rostrum/scenes" ] && echo yes || echo no)"
check "version needs no display" "0" "$(status env -u DISPLAY -u WAYLAND_DISPLAY -u QT_QPA_PLATFORM "$B" --version)"
check "bad volume argument" "1" "$(status "$B" --set-volume game)"

"$B" >/dev/null 2>&1 &
PID=$!
i=0
until gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
    --method org.freedesktop.DBus.NameHasOwner $DEST 2>/dev/null | grep -q true; do
    i=$((i + 1))
    if [ $i -gt 100 ]; then
        echo "FAIL Rostrum did not take its D-Bus name"
        kill $PID 2>/dev/null
        exit 1
    fi
    sleep 0.1
done
sleep 0.5

check "scene list" "Live" "$("$B" --list-scenes)"
check "bus list" "game	1.00	unmuted	Game" "$("$B" --list-buses | grep '^game')"
gdbus monitor --session --dest $DEST --object-path $OBJ >"$XDG_STATE_HOME/monitor.txt" 2>&1 &
MON=$!
sleep 0.3
check "mute mic" "0" "$(status "$B" --mute-mic)"
sleep 0.3
kill $MON
check "PropertiesChanged sent" "1" \
    "$(grep -c "PropertiesChanged ('$IFACE', {'MicMuted': <true>" "$XDG_STATE_HOME/monitor.txt")"
check "MicMuted property" "(<true>,)" "$(prop MicMuted)"
check "toggle mic" "0" "$(status "$B" --toggle-mic)"
check "MicMuted after toggle" "(<false>,)" "$(prop MicMuted)"
check "unknown scene" "1" "$(status "$B" --scene Nowhere)"
check "known scene" "0" "$(status "$B" --scene live)"
check "set volume by name" "0" "$(status "$B" --set-volume Game=50%)"
check "volume applied" "game	0.50	unmuted	Game" "$("$B" --list-buses | grep '^game')"
check "volume out of range" "1" "$(status "$B" --set-volume game=2)"
check "mic gain to 150 %" "0" "$(status "$B" --set-volume mic=1.5)"
check "unknown bus" "1" "$(status "$B" --set-volume nowhere=0.5)"
check "unknown action" "1" "$(status "$B" --action launch_rockets)"
check "hold action refused" "1" "$(status "$B" --action push_to_talk)"
check "bus mute action" "0" "$(status "$B" --action mute_bus_game)"
check "bus muted" "game	0.50	muted	Game" "$("$B" --list-buses | grep '^game')"
check "action list has bus actions" "mute_bus_game	Mute Game bus" "$("$B" --list-actions | grep '^mute_bus_game')"
check "panic" "0" "$(status "$B" --action panic_mute)"
check "Panic property" "(<true>,)" "$(prop Panic)"
check "StreamMuted during panic" "(<true>,)" "$(prop StreamMuted)"
check "panic off" "0" "$(status "$B" --action panic_mute)"
check "StreamMuted after panic" "(<false>,)" "$(prop StreamMuted)"
check "masters by reserved id" "0" "$(status gdbus call --session --dest $DEST --object-path $OBJ --method $IFACE.SetBusMuted stream true)"
check "StreamMuted property" "(<true>,)" "$(prop StreamMuted)"
gdbus call --session --dest $DEST --object-path $OBJ --method $IFACE.SetBusMuted stream false >/dev/null

# A client that presses push to talk and leaves must not leave the mic live.
"$B" --mute-mic
gdbus call --session --dest $DEST --object-path $OBJ --method $IFACE.PressAction push_to_talk >/dev/null
sleep 0.5
check "hold released when the client left" "(<true>,)" "$(prop MicMuted)"
check "error name" "GDBus.Error:dev.getrostrum.Rostrum1.Error.NoSuchScene" \
    "$(gdbus call --session --dest $DEST --object-path $OBJ --method $IFACE.SwitchScene Nowhere 2>&1 | grep -o 'GDBus.Error:[A-Za-z0-9.]*')"

methods=$(gdbus introspect --session --dest $DEST --object-path $OBJ | sed -n "/interface $IFACE/,/};/p" |
    grep -oE '^ +[A-Z][A-Za-z]+\(' | tr -d ' (' | sort | tr '\n' ' ')
documented=$(grep -oE '<method name="[A-Za-z]+"' "$(dirname "$0")/../data/$IFACE.xml" |
    sed 's/.*name="//; s/"//' | sort | tr '\n' ' ')
check "introspection matches the XML" "$documented" "$methods"

kill $PID
wait $PID 2>/dev/null
check "holds and panic never saved" "" "$(grep -rilE 'panic|push_to_talk_held|hold' "$XDG_CONFIG_HOME/rostrum/scenes" 2>/dev/null)"

[ $failures -eq 0 ] && echo "all D-Bus checks passed"
exit $failures
