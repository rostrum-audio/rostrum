"""Fail-closed fixture checks and live negative controls (no desktop session)."""
import importlib.util
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import unittest
from unittest.mock import patch

from interruption_checkpoint import Listener, assert_cleanup, hold

spec = importlib.util.spec_from_file_location("audio_safety", Path(__file__).with_name("audio-safety.py"))
safety = importlib.util.module_from_spec(spec)
spec.loader.exec_module(safety)
DRIVER = Path(sys.argv.pop(1)).resolve()
CHECKPOINT = None
if '--checkpoint' in sys.argv:
    index = sys.argv.index('--checkpoint')
    CHECKPOINT = Path(sys.argv[index + 1])
    del sys.argv[index:index + 2]


class FixtureTests(unittest.TestCase):
    def setUp(self):
        self.s = safety.Session(DRIVER)
        # The interruption checkpoint observes this instance's real cleanup.
        self.addCleanup(lambda: self.s.close())

    def test_missing_socket_never_invokes_client(self):
        with patch.object(self.s, "command") as command:
            with self.assertRaises(FileNotFoundError):
                self.s.graph()
            command.assert_not_called()

    def test_symlink_socket_refused(self):
        other = self.s.root / "other"
        with socket.socket(socket.AF_UNIX) as sock:
            sock.bind(str(other))
            self.s.remote.symlink_to(other)
            with patch.object(self.s, "command") as command:
                with self.assertRaises(safety.Failure):
                    self.s.graph()
                command.assert_not_called()

    def test_wrong_daemon_identity_refused(self):
        with socket.socket(socket.AF_UNIX) as sock:
            sock.bind(str(self.s.remote))
            with patch.object(self.s, "command", return_value='[{"type":"PipeWire:Interface:Core","info":{"props":{"rostrum.test.token":"wrong"}}}]'):
                with self.assertRaises(safety.Failure):
                    self.s.graph()

    def test_ambient_overrides_discarded(self):
        with patch.dict(os.environ, {"PIPEWIRE_REMOTE": "real", "DBUS_SESSION_BUS_ADDRESS": "real", "PIPEWIRE_CONFIG_PREFIX": "real", "WIREPLUMBER_DATA_DIR": "real"}):
            other = safety.Session(DRIVER)
            try:
                self.assertEqual(other.env["PIPEWIRE_REMOTE"], str(other.remote))
                self.assertNotIn("DBUS_SESSION_BUS_ADDRESS", other.env)
                self.assertNotIn("PIPEWIRE_CONFIG_PREFIX", other.env)
                self.assertEqual(other.env["WIREPLUMBER_DATA_DIR"], "/usr/share/wireplumber")
            finally:
                other.close()

    def test_cleanup_terminates_child_and_removes_root(self):
        child = self.s.spawn([sys.executable, "-c", "import time; time.sleep(60)"])
        root = self.s.root
        self.s.close()
        self.assertIsNotNone(child.poll())
        self.assertFalse(root.exists())

    def test_constructor_failure_removes_tree(self):
        root = self.s.root / "failed-construction"
        root.mkdir()
        with patch.object(safety.tempfile, "mkdtemp", return_value=str(root)), \
                patch.object(safety.Session, "prepare_environment", side_effect=safety.Failure("injected")):
            with self.assertRaisesRegex(safety.Failure, "injected"):
                safety.Session(DRIVER)
        self.assertFalse(root.exists())

    def test_signal_during_spawn_is_cleanup_owned(self):
        real_popen = subprocess.Popen
        children = []
        def popen(*args, **kwargs):
            child = real_popen(*args, **kwargs)
            children.append(child)
            os.kill(os.getpid(), signal.SIGTERM)
            return child
        def interrupted(*_):
            raise safety.Failure("injected interrupt")
        previous = signal.signal(signal.SIGTERM, interrupted)
        try:
            with patch.object(safety.subprocess, "Popen", side_effect=popen):
                with self.assertRaisesRegex(safety.Failure, "injected interrupt"):
                    self.s.spawn([sys.executable, "-c", "import time; time.sleep(60)"])
            self.assertEqual(len(self.s.processes), 1)
            self.s.close()
            self.assertIsNotNone(children[0].poll())
        finally:
            signal.signal(signal.SIGTERM, previous)

    def test_live_signal_oracle_rejects_silence_and_leak(self):
        try:
            self.s.start()
            self.s.device("test.headphones")
            self.s.engine(destination="phones")
            self.s.wait("Game bus", lambda: self.s.node(self.s.graph(), "rostrum.game"))
            with self.assertRaisesRegex(safety.Failure, "expected 440"):
                self.s.capture("rostrum.stream", 440)
            self.s.tone("SafetyPlayer", "rostrum.game")
            self.s.route("rostrum.game", "rostrum.phones")
            self.s.capture("test.headphones", 440)
            if CHECKPOINT:
                hold(self.s, CHECKPOINT)
            # Deliberately add a forbidden send: isolation must fail on actual audio.
            # Rostrum removes forbidden bus-to-bus sends, so inject an internal
            # test stream directly into the destination to exercise the audio oracle.
            self.s.tone("InjectedLeak", "rostrum.stream", internal=True)
            self.s.route("InjectedLeak", "rostrum.stream")
            with self.assertRaisesRegex(safety.Failure, "leaked audio"):
                self.s.capture("rostrum.stream")
        except BaseException:
            safety.cleanup_preserving_failure(self.s.diagnostics)
            raise

    def test_interrupt_reclaims_session(self):
        log = self.s.root / "interruption.log"
        with Listener(self.s.root / 'nested-checkpoint') as listener, log.open("wb") as output:
            runner = self.s.spawn([sys.executable, str(Path(__file__).with_name("audio-safety.py")),
                                   "--driver", str(DRIVER), '--scenario', 'startup',
                                   '--checkpoint', str(listener.path)], output)
            peer = ready = None
            try:
                peer = listener.accept()
                ready = peer.receive('ready')
                self.assertEqual(ready['phase'], 'live')
                peer.send('arm')
                self.assertEqual(peer.receive('held')['phase'], 'live')
                if CHECKPOINT:
                    hold(self.s, CHECKPOINT)
                # Closing the owned job asks its supervisor to terminate the runner
                # and reclaim all descendants, including adopted orphan supervisors.
            finally:
                def stop_and_verify():
                    error_log = next(path for child, path in self.s.processes if child is runner)
                    self.s.stop(runner)
                    if ready:
                        interrupted = peer.receive('interrupted')
                        self.assertEqual(interrupted['phase'], 'live')
                        self.assertEqual(interrupted['kind'], 'Failure')
                        self.assertEqual(interrupted['error'], 'Interrupted by signal 15')
                        assert_cleanup(peer.receive('cleanup'), ready)
                        self.assertFalse(Path(ready['root']).exists())
                    text = log.read_text() + error_log.read_text()
                    self.assertEqual(runner.returncode, 1, text)
                    self.assertIn("Interrupted by signal 15", text)
                    self.assertFalse(list(self.s.root.glob("rostrum-safety-*")))
                try:
                    safety.cleanup_preserving_failure(stop_and_verify)
                finally:
                    if peer:
                        peer.close()

    def test_signals_during_cleanup_do_not_abandon_jobs(self):
        child = self.s.spawn([sys.executable, '-c', 'import time; time.sleep(60)'])
        original = self.s._close
        def interrupted_cleanup():
            # These are cleanup-phase signals, not the live-work case above.
            for sig in safety.INTERRUPTS:
                self.assertEqual(signal.getsignal(sig), signal.SIG_IGN)
                os.kill(os.getpid(), sig)
            original()
        with patch.object(self.s, '_close', side_effect=interrupted_cleanup):
            self.s.close()
        self.assertTrue(child.closed)
        self.assertIsNotNone(child.process.returncode)
        self.assertFalse(self.s.root.exists())



def main():
    def interrupted(signum, _):
        # unittest does not swallow KeyboardInterrupt; the outer finally owns cleanup.
        raise KeyboardInterrupt(f"Interrupted by signal {signum}")
    for sig in safety.INTERRUPTS:
        signal.signal(sig, interrupted)
    code = 1
    try:
        result = unittest.main(exit=False).result
        code = 0 if result.wasSuccessful() else 1
    except KeyboardInterrupt as error:
        safety.report(str(error))
    finally:
        if sys.exc_info()[1] is not None:
            safety.cleanup_preserving_failure(safety.close_sessions)
        else:
            try:
                safety.close_sessions()
            except BaseException as error:
                # unittest already reported the original failures; retain that result.
                safety.report(f"Additional fixture cleanup error: {error}")
                code = 1
    return code


if __name__ == "__main__":
    sys.exit(main())
