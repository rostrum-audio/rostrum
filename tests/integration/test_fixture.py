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

spec = importlib.util.spec_from_file_location("audio_safety", Path(__file__).with_name("audio-safety.py"))
safety = importlib.util.module_from_spec(spec)
spec.loader.exec_module(safety)
DRIVER = Path(sys.argv.pop(1)).resolve()


class FixtureTests(unittest.TestCase):
    def setUp(self):
        self.s = safety.Session(DRIVER)
        self.addCleanup(self.s.close)

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
            # Deliberately add a forbidden send: isolation must fail on actual audio.
            # Rostrum removes forbidden bus-to-bus sends, so inject an internal
            # test stream directly into the destination to exercise the audio oracle.
            self.s.tone("InjectedLeak", "rostrum.stream", internal=True)
            self.s.route("InjectedLeak", "rostrum.stream")
            with self.assertRaisesRegex(safety.Failure, "leaked audio"):
                self.s.capture("rostrum.stream")
        except BaseException:
            self.s.diagnostics()
            raise

    def test_interrupt_reclaims_session(self):
        log = self.s.root / "interruption.log"
        roots, children = [], []
        with log.open("wb") as output:
            runner = subprocess.Popen([sys.executable, str(Path(__file__).with_name("audio-safety.py")),
                                       "--driver", str(DRIVER), "--runs", "20"],
                                      stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
            def inventory():
                for child in Path("/proc").glob("[0-9]*/cmdline"):
                    try:
                        status = (child.parent / "status").read_text()
                        if f"PPid:\t{runner.pid}\n" not in status:
                            continue
                        children.append(int(child.parent.name))
                        args = child.read_bytes().split(b"\0")
                        if args and args[0] == b"pipewire" and len(args) > 2:
                            path = Path(os.fsdecode(args[2])).parent.parent.parent
                            if path.name.startswith("rostrum-safety-"):
                                roots.append(path)
                    except (OSError, ValueError):
                        pass
            try:
                self.s.wait("interruption checkpoint", lambda: "test.headphones[0]" in log.read_text())
                inventory()
                runner.send_signal(signal.SIGTERM)
                runner.wait(timeout=15)
                text = log.read_text()
                self.assertEqual(runner.returncode, 1, text)
                self.assertIn("Interrupted by signal 15", text)
                self.assertTrue(roots, "No owned session observed")
                self.assertTrue(all(not root.exists() for root in roots))
            finally:
                inventory()  # Also covers a timeout before the checkpoint.
                if runner.poll() is None:
                    runner.send_signal(signal.SIGTERM)
                    try:
                        runner.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        runner.kill()
                        runner.wait(timeout=3)
                # Children were observed with this runner as parent, never ambient daemons.
                for pid in set(children):
                    try:
                        os.killpg(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                for root in set(roots):
                    if root.exists():
                        safety.shutil.rmtree(root)



if __name__ == "__main__":
    unittest.main()
