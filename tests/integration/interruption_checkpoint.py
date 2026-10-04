"""Bounded private control channel for live-work interruption regressions only."""
import json
from pathlib import Path
import signal
import socket

from process_lifecycle import report

TIMEOUT = 12


class Peer:
    def __init__(self, connection):
        self.connection = connection
        connection.settimeout(TIMEOUT)
        self.reader = connection.makefile('rb')

    def send(self, event, **values):
        self.connection.sendall((json.dumps(dict(event=event, **values)) + '\n').encode())

    def receive(self, event):
        line = self.reader.readline(65537)
        if not line or len(line) > 65536:
            raise RuntimeError('Missing or oversized interruption checkpoint message')
        value = json.loads(line)
        if value.get('event') != event:
            raise RuntimeError(f'Expected {event}, received {value}')
        return value

    def close(self):
        try:
            self.reader.close()
        finally:
            self.connection.close()


class Listener:
    def __init__(self, path):
        # Callers place this socket only inside their owned mode-0700 test tree.
        self.path = Path(path)
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(TIMEOUT)
        try:
            self.socket.bind(str(path))
            info = self.path.lstat()
            self.identity = (info.st_dev, info.st_ino)
            self.socket.listen(1)
        except BaseException:
            self.socket.close()
            raise

    def accept(self):
        connection, _ = self.socket.accept()
        return Peer(connection)

    def close(self):
        self.socket.close()
        if self.path.exists():
            info = self.path.lstat()
            if (info.st_dev, info.st_ino) != self.identity:
                raise RuntimeError('Refusing to remove a replaced checkpoint socket')
            self.path.unlink()

    def __enter__(self):
        return self

    def __exit__(self, kind, error, traceback):
        try:
            self.close()
        except BaseException as cleanup_error:
            if error is None:
                raise
            report(f'Additional checkpoint cleanup error: {cleanup_error}')


def assert_cleanup(value, ready):
    """Check real supervisor statuses copied before the private tree was removed."""
    assert value['root'] == ready['root']
    assert value['root_removed'] is True
    # Failure diagnostics may also spawn short-lived, supervised graph clients.
    assert set(ready['jobs']) <= {job['job'] for job in value['jobs']}
    for job in value['jobs']:
        assert job['closed'] is True and job['supervisor_exited'] is True, job
        assert job['status']['complete'] is True, job
        assert job['status']['error'] is None, job


def hold(session, address):
    """Cannot advance from positive audio capture into cleanup before interruption."""
    connection = socket.socket(socket.AF_UNIX)
    connection.settimeout(TIMEOUT)
    try:
        connection.connect(str(address))
    except BaseException:
        connection.close()
        raise
    peer = Peer(connection)
    acknowledgements = {}
    real_stop, real_close = session.stop, session.close
    finished = False

    def stop(job):
        real_stop(job)
        # Job.stop has waited for the supervisor and checked its kernel-ECHILD
        # acknowledgement. Copy status while its file still exists.
        acknowledgements[job.status_path.name] = dict(
            job=job.status_path.name, supervisor=job.process.pid, closed=job.closed,
            supervisor_exited=job.process.returncode is not None, status=job.status())

    def close():
        nonlocal finished
        try:
            real_close()
            if not finished:
                peer.send('cleanup', root=str(session.root),
                          root_removed=not session.root.exists(),
                          jobs=list(acknowledgements.values()))
        finally:
            if not finished:
                finished = True
                peer.close()

    session.stop, session.close = stop, close
    try:
        assert all(callable(signal.getsignal(sig)) for sig in
                   (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)), 'Live handlers not installed'
        def live_jobs():
            assert session.processes, 'No supervised live work at checkpoint'
            for job, _ in session.processes:
                if job.poll() is not None:
                    raise RuntimeError('Owned job exited before live checkpoint')
        live_jobs()
        peer.send('ready', phase='live', root=str(session.root),
                  identity=session.root_identity,
                  jobs=[job.status_path.name for job, _ in session.processes])
        peer.receive('arm')
        live_jobs()
        peer.send('held', phase='live')
        # No release command: only the real signal handler may end this hold.
        peer.receive('interruption-required')
        raise RuntimeError('Live checkpoint was released without a signal')
    except BaseException as error:
        try:
            peer.send('interrupted', phase='live', error=str(error), kind=type(error).__name__)
        except BaseException as publication_error:
            report(f'Additional interruption checkpoint error: {publication_error}')
        raise
