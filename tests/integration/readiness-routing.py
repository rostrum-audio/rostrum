#!/usr/bin/env python3
"""Read the real Qt graph/evaluator while using only the fail-closed private session."""
import importlib.util
import json
from pathlib import Path
import signal
import sys
spec = importlib.util.spec_from_file_location('safety', Path(__file__).with_name('audio-safety.py'))
safety = importlib.util.module_from_spec(spec)
spec.loader.exec_module(safety)
for sig in safety.INTERRUPTS:
    signal.signal(sig, lambda signum, _: (_ for _ in ()).throw(safety.Failure(f'Interrupted by {signum}')))
s = safety.Session(Path(sys.argv[1]).resolve(strict=True))
try:
    s.start()
    s.device('test.headphones')
    s.device('test.saved-mic', source=True)
    s.destination_request = 0
    s.destination_file = s.root / 'destinations'
    s.destination_file.write_text('')
    snapshot = s.root / 'readiness.json'
    engine = s.spawn([str(s.driver), '--headphones', 'test.headphones', '--mic', 'test.saved-mic',
        '--dest', 'desktop=both', '--rule', 'name:SafetyPlayer=desktop',
        '--destination-file', str(s.destination_file), '--readiness-file', str(snapshot)])
    s.tone('SafetyPlayer', 'rostrum.desktop')
    s.route('SafetyPlayer', 'rostrum.desktop')
    def status(identifier, expected):
        if not snapshot.exists(): return False
        rows = json.loads(snapshot.read_text())
        return any(row['id'] == identifier and row['status'] == expected for row in rows)
    for destination in ('both', 'phones', 'stream', 'both'):
        s.destination(engine, 'desktop', destination)
        s.wait('mirror/evaluator phones state', lambda: status('desktop-phones', 2 if destination == 'stream' else 0))
        s.wait('mirror/evaluator stream state', lambda: status('desktop-stream', 2 if destination == 'phones' else 0))
        if destination == 'phones':
            s.capture('test.headphones', 440)
            s.capture('rostrum.stream')
        elif destination == 'stream':
            s.capture('rostrum.stream', 440)
            s.capture('test.headphones')
        else:
            s.capture('rostrum.stream', 440)
            s.capture('test.headphones', 440)
    s.remove('test.saved-mic')
    s.wait('missing saved microphone reported', lambda: status('mic-device', 1))
    s.device('test.saved-mic', source=True)
    s.wait('saved microphone recovery reported', lambda: status('mic-device', 0))
    print('PASS private Qt link-state mirror and readiness evaluator, stereo signals and exclusion, mic disappearance/recovery', flush=True)
finally:
    safety.cleanup_preserving_failure(s.close)
    safety.cleanup_preserving_failure(safety.close_sessions)
