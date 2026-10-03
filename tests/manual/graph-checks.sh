#!/bin/bash
# Destination and mic-path checks against fake devices, so nothing reaches real speakers.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
B="$ROOT/build/tools/rostrum-graphtest/rostrum-graphtest"
T="${TMPDIR:-/tmp}/rostrum-test"; mkdir -p "$T"; cp "$ROOT/tests/manual/rms.py" "$T/"
[ -f "$T/tone.wav" ] || python3 - "$T/tone.wav" <<'EOF'
import sys, wave, struct, math
rate = 48000
with wave.open(sys.argv[1], 'wb') as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate)
    w.writeframes(b''.join(struct.pack('<hh', v, v) for v in
        (int(0.3 * 32767 * math.sin(2 * math.pi * 440 * i / rate)) for i in range(rate * 60))))
EOF
rms() { python3 $T/rms.py "$1"; }
rec() { # rec <target> <file> [capture.sink]
    if [ "$3" = sink ]; then
        timeout 1.5 pw-record -P '{ stream.capture.sink = true }' --target "$1" "$2" 2>/dev/null
    else
        timeout 1.5 pw-record --target "$1" "$2" 2>/dev/null
    fi
    rms "$2"
}

for id in $(pw-dump 2>/dev/null | python3 -c "
import json,sys
for o in json.load(sys.stdin):
  if o['type'].endswith('Node') and str(o['info']['props'].get('node.name','')).startswith('rostrumtest.'): print(o['id'])
"); do pw-cli destroy $id >/dev/null; done
sleep 0.3
pw-cli create-node adapter '{ factory.name=support.null-audio-sink node.name=rostrumtest.headset node.description="Test Headset" media.class=Audio/Sink audio.position=[FL FR] object.linger=true monitor.channel-volumes=true }' >/dev/null
pw-cli create-node adapter '{ factory.name=support.null-audio-sink node.name=rostrumtest.mic node.description="Test Mic" media.class=Audio/Source/Virtual audio.position=[MONO] object.linger=true }' >/dev/null
sleep 0.5

pw-play --volume 0.2 --target rostrum.desktop $T/tone.wav & PLAY=$!
sleep 0.5

for d in both phones stream; do
    "$B" --headphones rostrumtest.headset --rule name:pw-play=game --dest game=$d --seconds 3 >/dev/null 2>&1 &
    G=$!
    sleep 2
    echo "game=$d  headset: $(rec rostrumtest.headset $T/h.wav sink)  stream mix: $(rec rostrum.stream $T/s.wav sink)"
    wait $G
done

echo "--- solo voice while game plays (game=both):"
"$B" --headphones rostrumtest.headset --rule name:pw-play=game --solo voice --seconds 3 >/dev/null 2>&1 &
G=$!; sleep 2
echo "solo voice  headset: $(rec rostrumtest.headset $T/h.wav sink)  stream mix: $(rec rostrum.stream $T/s.wav sink)"
wait $G
kill $PLAY

echo "--- mic path:"
pw-play --volume 0.2 -P '{ node.autoconnect = false }' $T/tone.wav & MP=$!
sleep 0.7
pw-link pw-play:output_FL rostrumtest.mic:input_MONO
"$B" --headphones rostrumtest.headset --mic rostrumtest.mic --seconds 4 >/dev/null 2>&1 &
G=$!; sleep 2
echo "mic live:  Rostrum Mic: $(rec rostrum.mic $T/m.wav)  stream mix: $(rec rostrum.stream $T/s.wav sink)  headset (sidetone off): $(rec rostrumtest.headset $T/h.wav sink)"
pw-link -l | grep -A2 '^rostrumtest.mic:capture' | sed 's/^/    /'
wait $G
"$B" --headphones rostrumtest.headset --mic rostrumtest.mic --mic-muted --seconds 3 >/dev/null 2>&1 &
G=$!; sleep 2
echo "mic muted: Rostrum Mic: $(rec rostrum.mic $T/m.wav)"
wait $G
"$B" --headphones rostrumtest.headset --mic rostrumtest.mic --sidetone 0.8 --seconds 3 >/dev/null 2>&1 &
G=$!; sleep 2
echo "sidetone:  headset: $(rec rostrumtest.headset $T/h.wav sink)  stream mix: $(rec rostrum.stream $T/s.wav sink)"
wait $G
kill $MP

# Remove the fake devices. Rostrum's own nodes stay (they linger); use --teardown to remove them.
for id in $(pw-dump 2>/dev/null | python3 -c "
import json,sys
for o in json.load(sys.stdin):
  if o['type'].endswith('Node') and str(o['info']['props'].get('node.name','')).startswith('rostrumtest.'): print(o['id'])
"); do pw-cli destroy $id >/dev/null; done
