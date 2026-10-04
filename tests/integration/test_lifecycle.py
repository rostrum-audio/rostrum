"""Lifecycle regressions; the outer reaper contains deliberately broken children."""
import contextlib
import io
import ctypes
import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('audio_safety', HERE / 'audio-safety.py')
safety = importlib.util.module_from_spec(spec)
spec.loader.exec_module(safety)
DRIVER = Path(sys.argv.pop(1)).resolve()
ACTIVE = set()


def state(pid):
    try:
        fields = Path(f'/proc/{pid}/stat').read_text().rpartition(')')[2].split()
        return fields[0], int(fields[1]), int(fields[19])
    except FileNotFoundError:
        return None


def descendants():
    records = {}
    for entry in Path('/proc').glob('[0-9]*'):
        try:
            item = state(int(entry.name))
            if item:
                records[int(entry.name)] = item
        except (OSError, ValueError):
            pass
    owned = {os.getpid()}
    while True:
        more = {pid for pid, (_, parent, _) in records.items() if parent in owned}
        if more <= owned:
            return {pid: records[pid] for pid in owned if pid != os.getpid()}
        owned |= more


def observed(description, predicate, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.02)
    raise AssertionError(f'Timed out: {description}')


class LifecycleTests(unittest.TestCase):
    def setUp(self):
        # Adopt deliberate orphans so red tests can reclaim them without old PGIDs.
        if ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0):
            raise OSError(ctypes.get_errno(), 'subreaper unavailable')
        self.temp = tempfile.TemporaryDirectory(prefix='rostrum-lifecycle-')
        self.roots = {}
        ACTIVE.add(self)
        real_mkdtemp = tempfile.mkdtemp
        def tracked(*args, **kwargs):
            path = real_mkdtemp(*args, **kwargs)
            root = Path(path)
            if root.name.startswith('rostrum-safety-'):
                self.roots[root] = (root.stat().st_dev, root.stat().st_ino)
            return path
        self.tracker = patch.object(tempfile, 'mkdtemp', tracked)
        self.tracker.start()

    def inventory(self):
        records = descendants()
        for pid in records:
            try:
                values = Path(f'/proc/{pid}/environ').read_bytes().split(b'\0')
                for value in values:
                    if value.startswith(b'XDG_RUNTIME_DIR='):
                        root = Path(os.fsdecode(value.split(b'=', 1)[1])).parent
                        if root.name.startswith('rostrum-safety-') and root.exists():
                            self.roots[root] = (root.stat().st_dev, root.stat().st_ino)
            except OSError:
                pass
        return records

    def tearDown(self):
        if self not in ACTIVE:
            return
        # Test-only recovery: pin and revalidate descendants before signaling.
        for pid, (_, _, birth) in self.inventory().items():
            try:
                fd = os.pidfd_open(pid)
                try:
                    current = state(pid)
                    if current and current[2] == birth and pid in descendants():
                        signal.pidfd_send_signal(fd, signal.SIGKILL)
                finally:
                    os.close(fd)
            except ProcessLookupError:
                pass
        deadline = time.monotonic() + 3
        while descendants() and time.monotonic() < deadline:
            try:
                while os.waitpid(-1, os.WNOHANG)[0]:
                    pass
            except ChildProcessError:
                pass
            time.sleep(.02)
        for root, identity in self.roots.items():
            if root.exists() and (root.stat().st_dev, root.stat().st_ino) == identity:
                safety.shutil.rmtree(root)
        self.temp.cleanup()
        self.tracker.stop()
        ACTIVE.discard(self)
        for session in list(safety.SESSIONS):
            if session.root in self.roots and not session.root.exists():
                for job, _ in session.processes:
                    if job.anchor:
                        job.anchor.close()
                    if job.control:
                        job.control.close()
                    if job.process:
                        job.process.wait(timeout=3)
                for file in session.files:
                    file.close()
                safety.SESSIONS.discard(session)

    def interrupt_fixture(self, method, sig):
        log = Path(self.temp.name) / 'fixture.log'
        with log.open('wb') as output:
            env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', TMPDIR=self.temp.name)
            fixture = subprocess.Popen([sys.executable, '-B', str(HERE / 'test_fixture.py'),
                                        str(DRIVER), '-v', 'FixtureTests.' + method],
                                       env=env, stdout=output, stderr=subprocess.STDOUT)
            def checkpoint():
                self.inventory()
                if method == 'test_interrupt_reclaims_session':
                    for pid in descendants():
                        try:
                            args = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
                            if args and args[0] == b'pw-cat' and b'-p' in args:
                                return True
                        except OSError:
                            pass
                    return False
                return 'test.headphones[0]' in log.read_text()
            observed('live private fixture', checkpoint)
            before = self.inventory()
            fixture.send_signal(sig)
            fixture.wait(timeout=15)
            self.assertNotEqual(fixture.returncode, 0)
            live = [pid for pid, (_, _, birth) in before.items()
                    if (now := state(pid)) and now[2] == birth and now[0] != 'Z']
            self.assertEqual(live, [], 'interrupted fixture left owned live descendants')
            self.assertTrue(all(not root.exists() for root in self.roots),
                            'interrupted fixture left private trees')

    def test_fixture_term_and_hup_during_live_audio(self):
        for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
            with self.subTest(signal=sig):
                self.interrupt_fixture('test_live_signal_oracle_rejects_silence_and_leak', sig)

    def test_fixture_term_and_hup_during_nested_runner(self):
        for sig in (signal.SIGTERM, signal.SIGHUP):
            with self.subTest(signal=sig):
                self.interrupt_fixture('test_interrupt_reclaims_session', sig)

    def test_descendant_outlives_leader(self):
        session = safety.Session(DRIVER)
        root = session.root
        try:
            pidfile = root / 'descendant.pid'
            code = ('import subprocess,sys; from pathlib import Path; '
                    'p=subprocess.Popen([sys.executable,"-c","import time; time.sleep(60)"]); '
                    'Path(sys.argv[1]).write_text(str(p.pid))')
            leader = session.spawn([sys.executable, '-c', code, str(pidfile)])
            leader.wait(timeout=5)
            child = int(pidfile.read_text())
            self.inventory()
            session.close()
            self.assertTrue(state(child) is None or state(child)[0] == 'Z',
                            'descendant remained live after cleanup')
            session.close()
            self.assertFalse(root.exists())
        finally:
            session.close()

    def test_descendant_ignoring_term_is_reclaimed(self):
        session = safety.Session(DRIVER)
        root = session.root
        try:
            pidfile = root / 'ignoring.pid'
            child_code = ('import os,signal,sys,time; from pathlib import Path; '
                          'signal.signal(signal.SIGTERM,signal.SIG_IGN); '
                          'Path(sys.argv[1]).write_text(str(os.getpid())); time.sleep(60)')
            parent_code = ('import subprocess,sys,time; '
                           'subprocess.Popen([sys.executable,"-c",sys.argv[1],sys.argv[2]]); time.sleep(60)')
            session.spawn([sys.executable, '-c', parent_code, child_code, str(pidfile)])
            session.wait('descendant installed SIGTERM handler', lambda: pidfile.exists())
            child = int(pidfile.read_text())
            self.inventory()
            session.close()
            self.assertTrue(state(child) is None or state(child)[0] == 'Z')
            session.close()
            self.assertFalse(root.exists())
        finally:
            session.close()

    def test_mismatched_and_exited_identity_never_signals(self):
        from process_lifecycle import Identity, OwnershipError, record
        sentinel = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
        identity = None
        try:
            birth = record(sentinel.pid)[1]
            with patch.object(signal, 'pidfd_send_signal', wraps=signal.pidfd_send_signal) as send:
                with self.assertRaises(OwnershipError):
                    Identity(sentinel.pid, os.getpid(), birth + 1)
                with self.assertRaises(OwnershipError):
                    Identity(sentinel.pid, sentinel.pid + 1000000, birth)
                send.assert_not_called()
            self.assertIsNone(sentinel.poll(), 'unrelated sentinel was signaled')
            identity = Identity(sentinel.pid, os.getpid(), birth)
            sentinel.terminate()
            sentinel.wait(timeout=3)
            with patch.object(signal, 'pidfd_send_signal', wraps=signal.pidfd_send_signal) as send:
                self.assertFalse(identity.send(signal.SIGKILL))
                send.assert_not_called()
        finally:
            if sentinel.poll() is None:
                sentinel.kill()
                sentinel.wait(timeout=3)
            if identity:
                identity.close()

    def supervised_fault(self, mutation, worker_code):
        import process_lifecycle
        session = safety.Session(DRIVER)
        pidfile = Path(self.temp.name) / 'fault-child.pid'
        wrapper = Path(self.temp.name) / 'guardian-wrapper.py'
        wrapper.write_text(
            'import sys,time; from pathlib import Path\n'
            f'sys.path.insert(0, {str(HERE)!r})\n'
            'import process_lifecycle as m\n' + mutation + '\n'
            'm.guardian(int(sys.argv[2]),Path(sys.argv[3]),sys.argv[4:])\n')
        try:
            with patch.object(process_lifecycle, '__file__', str(wrapper)):
                try:
                    job = session.spawn([sys.executable, '-c', worker_code, str(pidfile)])
                except safety.OwnershipError:
                    job = None
            observed('fork child PID', lambda: pidfile.exists())
            child = int(pidfile.read_text())
            self.inventory()
            try:
                if job is not None:
                    session.stop(job)
            except safety.OwnershipError:
                pass  # A surfaced operational failure is permitted; a live orphan is not.
            self.assertTrue(state(child) is None or state(child)[0] == 'Z',
                            'supervisor abandoned a live descendant')
        finally:
            safety.cleanup_preserving_failure(session.close)

    def test_incomplete_snapshot_cannot_acknowledge_cleanup(self):
        pidfile = Path(self.temp.name) / 'fault-child.pid'
        mutation = (
            'real=m.children; hidden=0\n'
            'def snapshot(owner):\n'
            ' global hidden\n'
            f' if Path({str(pidfile)!r}).exists() and hidden<2:\n'
            '  hidden+=1; return {}\n'
            ' return real(owner)\n'
            'm.children=snapshot')
        # The worker forks from its TERM handler, then its leader exits. Simulate
        # enumeration missing the new child; kernel ECHILD must still prevent success.
        worker = (
            'import os,signal,sys,time; from pathlib import Path\n'
            'def term(*_):\n'
            ' if os.fork()==0:\n'
            '  signal.signal(signal.SIGTERM,signal.SIG_IGN)\n'
            '  Path(sys.argv[1]).write_text(str(os.getpid()))\n'
            '  time.sleep(60)\n'
            ' else: os._exit(0)\n'
            'signal.signal(signal.SIGTERM,term)\n'
            # Announce readiness without using an assumed delay.
            'Path(sys.argv[1]+".ready").write_text("ready")\n'
            'time.sleep(60)')
        # Start cleanup once the handler is installed; supervised_fault normally
        # waits for a child before cleanup, so use an independent owned job here.
        import process_lifecycle
        session = safety.Session(DRIVER)
        wrapper = Path(self.temp.name) / 'snapshot-wrapper.py'
        wrapper.write_text('import sys; from pathlib import Path\n'
                           f'sys.path.insert(0,{str(HERE)!r})\n'
                           'import process_lifecycle as m\n' + mutation + '\n'
                           'm.guardian(int(sys.argv[2]),Path(sys.argv[3]),sys.argv[4:])\n')
        try:
            with patch.object(process_lifecycle, '__file__', str(wrapper)):
                job = session.spawn([sys.executable, '-c', worker, str(pidfile)])
            observed('TERM handler ready', lambda: Path(str(pidfile)+'.ready').exists())
            session.stop(job)
            self.inventory()
            observed('TERM handler forked', lambda: pidfile.exists())
            child = int(pidfile.read_text())
            self.assertTrue(state(child) is None or state(child)[0] == 'Z',
                            'empty snapshot falsely acknowledged cleanup')
        finally:
            safety.cleanup_preserving_failure(session.close)

    def test_status_publication_failure_does_not_abandon_descendant(self):
        mutation = ('real=m.publish; calls=0\n'
                    'def publish(path,value):\n'
                    ' global calls\n'
                    ' calls+=1\n'
                    ' if calls==2: raise OSError("injected status failure")\n'
                    ' real(path,value)\n'
                    'm.publish=publish')
        worker = ('import subprocess,sys; from pathlib import Path; '
                  'p=subprocess.Popen([sys.executable,"-c",'
                  '"import signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); time.sleep(60)"]); '
                  'Path(sys.argv[1]).write_text(str(p.pid))')
        self.supervised_fault(mutation, worker)

    def test_reporting_failure_does_not_abandon_descendant(self):
        pidfile = Path(self.temp.name) / 'fault-child.pid'
        mutation = ('real=m.publish; calls=0\n'
                    'def publish(path,value):\n'
                    ' global calls\n'
                    ' calls+=1\n'
                    ' if calls==1:\n'
                    '  deadline=time.monotonic()+3\n'
                    f'  while not Path({str(pidfile)!r}).exists() and time.monotonic()<deadline: time.sleep(.01)\n'
                    '  raise OSError("injected status failure")\n'
                    ' real(path,value)\n'
                    'm.publish=publish\n'
                    'class BadLog:\n'
                    ' def write(self,text): raise OSError("injected log failure")\n'
                    ' def flush(self): pass\n'
                    'sys.stderr=BadLog()')
        worker = ('import subprocess,sys; from pathlib import Path; '
                  'p=subprocess.Popen([sys.executable,"-c","import time; time.sleep(60)"]); '
                  'Path(sys.argv[1]).write_text(str(p.pid))')
        self.supervised_fault(mutation, worker)

    def test_supervisor_interrupted_before_worker_startup(self):
        from process_lifecycle import Identity
        session = safety.Session(DRIVER)
        marker = Path(self.temp.name) / 'worker-started'
        real_init = Identity.__init__
        def interrupt(identity, *args, **kwargs):
            real_init(identity, *args, **kwargs)
            identity.send(signal.SIGTERM)
        try:
            with contextlib.redirect_stderr(io.StringIO()):
                with patch.object(Identity, '__init__', interrupt):
                    with self.assertRaises(safety.OwnershipError):
                        session.spawn([sys.executable, '-c',
                                       'from pathlib import Path; import sys; Path(sys.argv[1]).touch()', str(marker)])
            self.assertFalse(marker.exists(), 'worker started after supervisor interruption')
            session.close()
            session.close()
            self.assertFalse(session.root.exists())
        finally:
            safety.cleanup_preserving_failure(session.close)

    def test_startup_failure_is_owned_and_preserves_cause(self):
        session = safety.Session(DRIVER)
        root = session.root
        report = io.StringIO()
        try:
            with contextlib.redirect_stderr(report):
                with self.assertRaisesRegex(safety.OwnershipError, 'No such file'):
                    session.spawn(['/nonexistent/rostrum-lifecycle-command'])
            self.assertEqual(len(session.processes), 1, 'failed startup lost cleanup ownership')
            session.close()
            session.close()
            self.assertFalse(root.exists())
        finally:
            session.close()

    def test_cleanup_error_preserves_original_failure(self):
        report = io.StringIO()
        def broken_cleanup():
            raise safety.Failure('cleanup fault')
        with contextlib.redirect_stderr(report):
            with self.assertRaisesRegex(safety.Failure, 'original routing failure'):
                try:
                    raise safety.Failure('original routing failure')
                finally:
                    safety.cleanup_preserving_failure(broken_cleanup)
        self.assertIn('cleanup fault', report.getvalue())
        with self.assertRaisesRegex(safety.Failure, 'cleanup fault'):
            safety.cleanup_preserving_failure(broken_cleanup)

    def test_fallback_never_signals_reaped_identifiers(self):
        spec = importlib.util.spec_from_file_location('fixture', HERE / 'test_fixture.py')
        fixture = importlib.util.module_from_spec(spec)
        with patch.object(sys, 'argv', ['test_fixture.py', str(DRIVER)]):
            spec.loader.exec_module(fixture)
        real_killpg = os.killpg
        def checked_killpg(pid, sig):
            if state(pid) is None:
                raise AssertionError('cleanup tried to signal a stale identifier')
            return real_killpg(pid, sig)
        case = fixture.FixtureTests('test_interrupt_reclaims_session')
        with patch.object(os, 'killpg', checked_killpg):
            case.setUp()
            try:
                case.test_interrupt_reclaims_session()
            finally:
                case.doCleanups()


def main():
    def interrupted(signum, _):
        raise KeyboardInterrupt(f'Interrupted by signal {signum}')
    for sig in safety.INTERRUPTS:
        signal.signal(sig, interrupted)
    code = 1
    try:
        code = 0 if unittest.main(exit=False).result.wasSuccessful() else 1
    except KeyboardInterrupt as error:
        safety.report(str(error))
    finally:
        for sig in safety.INTERRUPTS:
            signal.signal(sig, signal.SIG_IGN)
        for case in list(ACTIVE):
            try:
                case.tearDown()
            except BaseException as error:
                safety.report(f'Additional lifecycle regression cleanup error: {error}')
                code = 1
    return code


if __name__ == '__main__':
    sys.exit(main())
