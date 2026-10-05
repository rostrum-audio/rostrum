"""Exercise the CI watchdog with disposable executables, never a real audio session."""
import json
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest


HELPER = Path(__file__).resolve().parents[1] / 'tools/ci/offscreen.py'


class OffscreenTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.output = self.root / 'evidence'

    def executable(self, body):
        app = self.root / 'app'
        app.write_text('#!' + sys.executable + '\n' + body)
        app.chmod(0o700)
        return app

    def run_probe(self, body, timeout=2, diagnostic_at=.5):
        app = self.executable(body)
        # Short deadlines are an internal test seam, not a CI/CLI override.
        command = [sys.executable, '-B', '-c',
                   'import importlib.util, pathlib, sys; '
                   's=importlib.util.spec_from_file_location("offscreen", sys.argv[1]); '
                   'm=importlib.util.module_from_spec(s); s.loader.exec_module(m); '
                   'sys.exit(m.run(pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3]), '
                   'timeout_seconds=float(sys.argv[4]), diagnostic_at=float(sys.argv[5])))',
                   str(HELPER), str(app), str(self.output), str(timeout), str(diagnostic_at)]
        return subprocess.run(command, capture_output=True, text=True, timeout=15)

    def test_success_keeps_screenshot_logs_and_private_environment(self):
        result = self.run_probe('''import json, os
from pathlib import Path
assert os.environ['QT_QPA_PLATFORM'] == 'offscreen'
assert 'DBUS_SESSION_BUS_ADDRESS' in os.environ
assert os.environ['PIPEWIRE_REMOTE'].startswith('rostrum-offscreen-')
assert os.environ['PULSE_SERVER'].startswith('unix:')
assert 'ROSTRUM_UPDATE_URL' not in os.environ
assert 'ROSTRUM_SENTRY_DSN' not in os.environ
Path(os.environ['ROSTRUM_SCREENSHOT']).write_bytes(b'screenshot')
p = Path(os.environ['XDG_STATE_HOME']) / 'rostrum/rostrum.log'
p.parent.mkdir(parents=True)
p.write_text('private startup log')
print('startup output', flush=True)
''')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.output / 'screenshot.png').read_bytes(), b'screenshot')
        self.assertIn('startup output', (self.output / 'app.log').read_text())
        self.assertEqual((self.output / 'rostrum.log').read_text(), 'private startup log')
        self.assertFalse((self.output / 'process-state.json').exists())
        self.assertFalse(list(self.output.glob('private-*')))
        self.assertTrue(json.loads((self.output / 'result.json').read_text())['cleanup_complete'])

    def test_timeout_retains_live_descendant_state_and_reclaims_tree(self):
        result = self.run_probe('''import os, subprocess, sys, time
from pathlib import Path
p = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(100)'])
Path(os.environ['ROSTRUM_SCREENSHOT']).with_name('child.pid').write_text(str(p.pid))
print('live startup checkpoint', flush=True)
time.sleep(100)
''')
        self.assertEqual(result.returncode, 124, result.stderr)
        state = json.loads((self.output / 'process-state.json').read_text())
        child = int((self.output / 'child.pid').read_text())
        self.assertIn(child, [p['pid'] for p in state['processes']])
        self.assertTrue(any(p.get('threads') for p in state['processes']))
        self.assertIn('live startup checkpoint', (self.output / 'app.log').read_text())
        self.assertFalse(Path(f'/proc/{child}').exists())
        self.assertFalse(list(self.output.glob('private-*')))
        record = json.loads((self.output / 'result.json').read_text())
        self.assertEqual(record['returncode'], 124)
        self.assertTrue(record['cleanup_complete'])
        self.assertLess(record['elapsed_seconds'], 10)

    def test_nonzero_app_exit_is_not_replaced_by_diagnostics(self):
        result = self.run_probe('import sys; print("startup failed"); sys.exit(17)')
        self.assertEqual(result.returncode, 17, result.stderr)
        self.assertIn('startup failed', (self.output / 'app.log').read_text())

    def test_missing_screenshot_and_qml_error_still_fail(self):
        for body in ('print("no screenshot")', '''import os
from pathlib import Path
Path(os.environ['ROSTRUM_SCREENSHOT']).write_bytes(b'screenshot')
print('qrc:/qml/Page.qml:12: MissingThing is not a type')
'''):
            with self.subTest(body=body):
                self.output = self.root / ('failure-' + str(len(list(self.root.glob('failure-*')))))
                result = self.run_probe(body)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(json.loads((self.output / 'result.json').read_text())['error'],
                              ('Missing or empty screenshot', 'QML startup error'))

    def test_stale_evidence_is_rejected_without_launch(self):
        self.output.mkdir()
        (self.output / 'screenshot.png').write_bytes(b'old screenshot')
        result = self.run_probe('raise RuntimeError("must not launch")')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.output / 'screenshot.png').read_bytes(), b'old screenshot')
        self.assertFalse((self.output / 'app.log').exists())
        self.assertIn('FileExistsError', result.stderr)

    def test_term_ignoring_client_cannot_extend_deadline(self):
        result = self.run_probe('''import signal, time
signal.signal(signal.SIGTERM, signal.SIG_IGN)
time.sleep(100)
''', timeout=.7, diagnostic_at=.2)
        self.assertEqual(result.returncode, 124, result.stderr)
        record = json.loads((self.output / 'result.json').read_text())
        self.assertTrue(record['cleanup_complete'])
        self.assertLess(record['elapsed_seconds'], 7)

    def test_interruption_reclaims_owned_session_and_preserves_evidence(self):
        app = self.executable('''import os, time
from pathlib import Path
Path(os.environ['ROSTRUM_SCREENSHOT']).with_name('live.pid').write_text(str(os.getpid()))
time.sleep(100)
''')
        process = subprocess.Popen([sys.executable, '-B', str(HELPER), str(app), str(self.output)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 5
            checkpoint = self.output / 'live.pid'
            while not checkpoint.exists():
                if process.poll() is not None or time.monotonic() >= deadline:
                    self.fail('Disposable fixture never reached live work')
                time.sleep(.01)
            pid = int(checkpoint.read_text())
            process.send_signal(signal.SIGTERM)
            stdout, stderr = process.communicate(timeout=12)
            self.assertEqual(process.returncode, 143, stderr + stdout)
            self.assertFalse(Path(f'/proc/{pid}').exists())
            self.assertFalse(list(self.output.glob('private-*')))
            self.assertTrue(json.loads((self.output / 'result.json').read_text())['cleanup_complete'])
        finally:
            if process.poll() is None:
                process.terminate()
                process.communicate(timeout=12)


if __name__ == '__main__':
    unittest.main()
