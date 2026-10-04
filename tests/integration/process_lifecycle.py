"""Linux-only ownership for test jobs: subreaper supervision and pidfd signaling."""
import ctypes
import json
import os
from pathlib import Path
import select
import signal
import socket
import subprocess
import sys
import time


class OwnershipError(RuntimeError):
    pass


def report(message):
    # A full or unavailable log must not prevent process reclamation.
    try:
        print(message, file=sys.stderr)
    except BaseException:
        pass


def record(pid):
    try:
        fields = Path(f'/proc/{pid}/stat').read_text().rpartition(')')[2].split()
        return int(fields[1]), int(fields[19]), fields[0]  # parent, birth tick, state
    except FileNotFoundError:
        return None


def belongs_to(pid, owner):
    seen = set()
    while pid != owner and pid not in seen:
        seen.add(pid)
        item = record(pid)
        if not item:
            return False
        pid = item[0]
    return pid == owner


class Identity:
    """A pidfd pins the kernel process identity, not a reusable numeric identifier."""
    def __init__(self, pid, owner, expected_birth=None):
        before = record(pid)
        if not before or (expected_birth is not None and before[1] != expected_birth):
            raise OwnershipError(f'Stale process identity: {pid}')
        if not belongs_to(pid, owner):
            raise OwnershipError(f'Process {pid} is outside supervisor {owner}')
        self.fd = os.pidfd_open(pid)
        self.pid, self.owner, self.birth = pid, owner, before[1]
        try:
            after = record(pid)
            fd_pid = Path(f'/proc/self/fdinfo/{self.fd}').read_text().split('Pid:\t')[1].splitlines()[0]
            if not after or after[1] != self.birth or int(fd_pid) != pid or not belongs_to(pid, owner):
                raise OwnershipError(f'Process identity changed during acquisition: {pid}')
        except BaseException:
            os.close(self.fd)
            raise

    def alive(self):
        return not select.select([self.fd], [], [], 0)[0]

    def send(self, sig):
        if not self.alive():
            return False
        # Never send a signal if the pinned process is no longer in the owned tree.
        item = record(self.pid)
        if not item:
            return False
        if item[1] != self.birth or not belongs_to(self.pid, self.owner):
            raise OwnershipError(f'Refusing mismatched process identity: {self.pid}')
        try:
            signal.pidfd_send_signal(self.fd, sig)
        except ProcessLookupError:
            return False
        return True

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


def children(owner):
    records = {}
    for path in Path('/proc').glob('[0-9]*'):
        try:
            pid = int(path.name)
            item = record(pid)
            if item:
                records[pid] = item
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            pass
    owned = {owner}
    while True:
        more = {pid for pid, (parent, _, _) in records.items() if parent in owned}
        if more <= owned:
            return {pid: records[pid] for pid in owned if pid != owner}
        owned |= more


def publish(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value))
    temporary.replace(path)


def guardian(control_fd, status_path, command):
    # This process launches only this job. Adopted descendants remain provably its own.
    if ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0):  # PR_SET_CHILD_SUBREAPER
        raise OwnershipError('Cannot establish a Linux child subreaper')
    owner = os.getpid()
    control = socket.socket(fileno=control_fd)
    control.setblocking(False)
    stopping = False
    def interrupted(*_):
        nonlocal stopping
        stopping = True
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, interrupted)
    # Exec inherits a blocked mask so even immediate termination waits for handlers.
    signal.pthread_sigmask(signal.SIG_SETMASK, [])
    worker = None
    status = {'pid': None, 'returncode': None, 'complete': False, 'error': None}
    identities, terminated = {}, set()
    deadline = None

    def failed(error):
        nonlocal stopping
        stopping = True
        if status['error'] is None:
            status['error'] = str(error)
            report(f'Test supervisor failure: {error}')

    def notify():
        try:
            publish(status_path, status)
        except BaseException as error:
            failed(error)  # Publication failure must never abandon the worker tree.

    def reap():
        # A /proc snapshot can miss a child forked during termination. Only kernel
        # ECHILD proves emptiness; account for the direct worker's actual exit code.
        while True:
            try:
                pid, code = os.waitpid(-1, os.WNOHANG)
            except ChildProcessError:
                return True
            if pid == 0:
                return False
            if worker is not None and pid == worker.pid and worker.returncode is None:
                worker.returncode = os.waitstatus_to_exitcode(code)
                status['returncode'] = worker.returncode

    try:
        # Pinning the supervisor is a prerequisite for granting worker startup.
        startup_deadline = time.monotonic() + 5
        while not stopping:
            if select.select([control], [], [], .02)[0]:
                if control.recv(1) != b'G':
                    stopping = True
                break
            if time.monotonic() >= startup_deadline:
                failed(OwnershipError('Worker startup permission timed out'))
        if not stopping:
            try:
                worker = subprocess.Popen(command, stdin=subprocess.DEVNULL, close_fds=True)
                status['pid'] = worker.pid
            except BaseException as error:
                failed(error)
        notify()
        while True:
            try:
                if worker is not None:
                    code = worker.poll()
                    if code is not None and status['returncode'] is None:
                        status['returncode'] = code
                        notify()
                if control is not None and select.select([control], [], [], .02)[0]:
                    if not control.recv(1):
                        stopping = True
                        control.close()
                        control = None
                elif control is None:
                    time.sleep(.02)
                if not stopping:
                    # Reap adopted zombies even while the direct worker is running.
                    owned = children(owner)
                    for pid, (parent, _, state) in owned.items():
                        if parent == owner and (worker is None or pid != worker.pid) and state == 'Z':
                            try:
                                os.waitpid(pid, os.WNOHANG)
                            except ChildProcessError:
                                pass
                    continue
                if deadline is None:
                    deadline = time.monotonic() + 3
                for pid, (_, birth, state) in children(owner).items():
                    if state == 'Z':
                        continue
                    if pid not in identities or identities[pid].birth != birth:
                        if pid in identities:
                            identities.pop(pid).close()
                        try:
                            identities[pid] = Identity(pid, owner, birth)
                        except (ProcessLookupError, OwnershipError):
                            continue  # Re-snapshot; never signal a doubtful identifier.
                    identity = identities[pid]
                    try:
                        if time.monotonic() >= deadline:
                            identity.send(signal.SIGKILL)
                        elif (pid, birth) not in terminated:
                            identity.send(signal.SIGTERM)
                            terminated.add((pid, birth))
                    except OwnershipError:
                        identities.pop(pid).close()
                if reap():
                    status['complete'] = True
                    notify()
                    return
                if time.monotonic() > deadline + 3 and status['error'] is None:
                    failed(OwnershipError('Owned descendants survived the cleanup deadline'))
                    notify()
                    # Keep ownership and retry; caller reports a bounded failure.
            except BaseException as error:
                failed(error)
                notify()
                time.sleep(.02)
    finally:
        for identity in identities.values():
            identity.close()
        if control is not None:
            control.close()


class Job:
    """Popen-like worker handle whose supervisor lives until the job tree is reclaimed."""
    def __init__(self, command, env, stdout, stderr, status_path):
        self.status_path = status_path
        self.control = None
        self.anchor = None
        self.process = None
        self.closed = False
        self.returncode = None
        if not hasattr(os, 'pidfd_open') or not hasattr(signal, 'pidfd_send_signal'):
            raise OwnershipError('Linux pidfd support is required; refusing unsupervised spawn')
        # Probe kernel support before launching anything.
        probe = os.pidfd_open(os.getpid())
        os.close(probe)
        self.status_path = status_path
        self.control, child = socket.socketpair()
        self.anchor = None
        self.closed = False
        self.returncode = None
        try:
            self.process = subprocess.Popen(
                [sys.executable, str(Path(__file__).resolve()), '--guardian', str(child.fileno()),
                 str(status_path), *command], env=env, stdin=subprocess.DEVNULL,
                stdout=stdout, stderr=stderr, start_new_session=True,
                pass_fds=(child.fileno(),),
                preexec_fn=lambda: signal.pthread_sigmask(signal.SIG_BLOCK,
                                                        (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)))
            self.anchor = Identity(self.process.pid, os.getpid())
            self.control.sendall(b'G')
            deadline = time.monotonic() + 5
            while not status_path.exists():
                if not self.anchor.alive():
                    self.process.wait(timeout=1)
                    raise OwnershipError('Supervisor exited before establishing ownership')
                if time.monotonic() >= deadline:
                    raise OwnershipError('Supervisor startup timed out')
                time.sleep(.02)
            status = self.status()
            if status['error']:
                raise OwnershipError(status['error'])
            if status['pid'] is None:
                raise OwnershipError('Supervisor interrupted before worker startup')
            self.pid = status['pid']
        except BaseException:
            try:
                self.stop()
            except BaseException as cleanup_error:
                report(f'Additional supervisor cleanup error: {cleanup_error}')
            raise
        finally:
            child.close()

    def status(self):
        return json.loads(self.status_path.read_text())

    def poll(self):
        if self.closed:
            return self.returncode
        status = self.status()
        if status['error']:
            raise OwnershipError(status['error'])
        if not self.closed and not self.anchor.alive() and not status['complete']:
            raise OwnershipError('Supervisor died without reclaiming its job')
        self.returncode = status['returncode']
        return self.returncode

    def wait(self, timeout):
        deadline = time.monotonic() + timeout
        while self.poll() is None:
            if time.monotonic() >= deadline:
                raise subprocess.TimeoutExpired(str(self.pid), timeout)
            time.sleep(.02)
        return self.returncode

    def stop(self):
        if self.closed:
            return
        if self.control is not None:
            self.control.close()  # EOF also requests cleanup on caller death.
        if self.process is None:
            self.closed = True
            return
        try:
            self.process.wait(timeout=8)
        except subprocess.TimeoutExpired as e:
            raise OwnershipError('Supervisor cleanup timed out; ownership/resources retained') from e
        if not self.status_path.exists() or not self.status()['complete']:
            raise OwnershipError('Supervisor exited without confirming complete cleanup')
        status = self.status()
        self.returncode = status['returncode']
        if self.anchor is not None:
            self.anchor.close()
        self.closed = True
        if status['error']:
            raise OwnershipError(status['error'])


if __name__ == '__main__':
    try:
        guardian(int(sys.argv[2]), Path(sys.argv[3]), sys.argv[4:])
    except BaseException as error:
        report(f'Test supervisor failed: {error}')
        sys.exit(1)
