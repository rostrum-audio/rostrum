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

from interruption_checkpoint import Listener, assert_cleanup

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
        from process_lifecycle import Identity
        log = Path(self.temp.name) / 'fixture.log'
        peer = anchor = None
        with Listener(Path(self.temp.name) / 'checkpoint') as listener, log.open('wb') as output:
            env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', TMPDIR=self.temp.name)
            fixture = subprocess.Popen([sys.executable, '-B', str(HERE / 'test_fixture.py'),
                                        str(DRIVER), '--checkpoint', str(listener.path),
                                        '-v', 'FixtureTests.' + method],
                                       env=env, stdout=output, stderr=subprocess.STDOUT)
            try:
                anchor = Identity(fixture.pid, os.getpid())
                peer = listener.accept()
                ready = peer.receive('ready')
                self.assertEqual(ready['phase'], 'live')
                root = Path(ready['root'])
                self.assertIn(Path(self.temp.name), root.parents)
                self.assertEqual((root.stat().st_dev, root.stat().st_ino), tuple(ready['identity']))
                self.roots[root] = tuple(ready['identity'])
                self.assertTrue(ready['jobs'], 'no owned live jobs at checkpoint')
                peer.send('arm')
                self.assertEqual(peer.receive('held')['phase'], 'live')
                self.assertTrue(anchor.send(sig), 'fixture exited before signal delivery')
                interrupted = peer.receive('interrupted')
                self.assertEqual(interrupted['phase'], 'live')
                self.assertEqual(interrupted['kind'], 'KeyboardInterrupt')
                self.assertEqual(interrupted['error'], f'Interrupted by signal {int(sig)}')
                assert_cleanup(peer.receive('cleanup'), ready)
                fixture.wait(timeout=15)
                self.assertNotEqual(fixture.returncode, 0, log.read_text())
                self.assertEqual(fixture.returncode, 1, log.read_text())
                self.assertIn(f'Interrupted by signal {int(sig)}', log.read_text())
                self.assertFalse(descendants(), 'interrupted fixture left owned descendants')
                self.assertFalse(root.exists(), 'interrupted fixture left private tree')
            finally:
                if peer:
                    peer.close()
                if anchor:
                    if anchor.alive():
                        anchor.send(signal.SIGTERM)
                    anchor.close()
                safety.cleanup_preserving_failure(lambda: fixture.wait(timeout=15))

    def test_fixture_term_and_hup_during_live_audio(self):
        for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
            with self.subTest(signal=sig):
                self.interrupt_fixture('test_live_signal_oracle_rejects_silence_and_leak', sig)

    def test_fixture_term_and_hup_during_nested_runner(self):
        for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
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

    def final_publication_fault(self, persistent):
        import process_lifecycle
        session = safety.Session(DRIVER)
        pidfile = Path(self.temp.name) / 'final-child.pid'
        marker = Path(self.temp.name) / 'final-write-failed'
        wrapper = Path(self.temp.name) / 'final-wrapper.py'
        wrapper.write_text(
            'import sys; from pathlib import Path\n'
            f'sys.path.insert(0,{str(HERE)!r})\n'
            'import process_lifecycle as m\n'
            'real=m.publish; injected=False\n'
            'def publish(path,value):\n'
            ' global injected\n'
            f' if value["complete"] and ({persistent!r} or not injected):\n'
            '  injected=True\n'
            f'  Path({str(marker)!r}).touch()\n'
            '  raise OSError("injected final completion publication failure")\n'
            ' real(path,value)\n'
            'm.publish=publish\n'
            'm.guardian(int(sys.argv[2]),Path(sys.argv[3]),sys.argv[4:])\n')
        # The child announces readiness only after installing its TERM handler;
        # its leader exits, leaving an adopted descendant requiring escalation.
        child_code = ('import os,signal,sys,time; from pathlib import Path; '
                      'signal.signal(signal.SIGTERM,signal.SIG_IGN); '
                      'Path(sys.argv[1]).write_text(str(os.getpid())); time.sleep(60)')
        worker = ('import subprocess,sys; subprocess.Popen('
                  '[sys.executable,"-c",sys.argv[2],sys.argv[1]])')
        try:
            with patch.object(process_lifecycle, '__file__', str(wrapper)):
                job = session.spawn([sys.executable, '-c', worker, str(pidfile), child_code])
            observed('descendant ready', lambda: pidfile.exists())
            observed('group leader exited', lambda: job.poll() is not None)
            self.assertEqual(job.returncode, 0)
            child = int(pidfile.read_text())
            self.assertIsNotNone(state(child))
            expected = ('without confirming complete cleanup' if persistent else
                        'injected final completion publication failure')
            with self.assertRaisesRegex(safety.OwnershipError, expected):
                job.stop()
            self.assertTrue(marker.exists(), 'final status fault was not exercised')
            self.assertIsNone(state(child), 'completion preceded descendant reaping')
            self.assertFalse(descendants(), 'owned processes remain after supervisor exit')
            if persistent:
                self.assertFalse(job.closed)
                self.assertFalse(job.status()['complete'], 'unpublished completion was accepted')
                for _ in range(2):
                    with self.assertRaisesRegex(safety.Failure, 'private resources retained'):
                        session.close()
                    self.assertTrue(session.root.exists())
            else:
                self.assertTrue(job.closed)
                self.assertTrue(job.status()['complete'])
                self.assertEqual(job.status()['error'],
                                 'injected final completion publication failure')
                job.stop()
                session.close()
                session.close()
                self.assertFalse(session.root.exists())
        finally:
            # Persistent failure deliberately retains the private tree/handles;
            # the outer test reaper owns recovery of this fault-injection case.
            if not persistent or sys.exc_info()[0] is not None:
                safety.cleanup_preserving_failure(session.close)

    def test_final_status_failure_reports_error_after_complete_cleanup(self):
        self.final_publication_fault(persistent=False)

    def test_persistent_final_status_failure_cannot_acknowledge_cleanup(self):
        self.final_publication_fault(persistent=True)

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
