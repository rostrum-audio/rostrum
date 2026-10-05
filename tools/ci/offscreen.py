#!/usr/bin/env python3
"""Fedora CI startup check: unchanged deadline, passive pre-timeout evidence."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import signal
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tests/integration'))
from process_lifecycle import Job, children, report


QML_ERROR = re.compile(r'qrc:/.*\.qml:[0-9]+.*(Error|error|is not a type|is not installed|not found)')


def snapshot(owner, output, budget=2):
    """Read only the owned tree; no ptrace, process signals or global command scan."""
    end = time.monotonic() + budget
    state = {'owner': owner, 'processes': [], 'truncated': False}
    for pid, (_, birth, _) in sorted(children(owner).items()):
        if time.monotonic() >= end:
            state['truncated'] = True
            break
        try:
            # An open proc directory pins these reads to this proc entry. Validate
            # birth before reading it; never follow a reused numeric path later.
            fd = os.open(f'/proc/{pid}', os.O_DIRECTORY | os.O_RDONLY)
        except OSError as error:
            state['processes'].append({'pid': pid, 'error': str(error)})
            continue
        try:
            def read(name):
                try:
                    item = os.open(name, os.O_RDONLY | os.O_NONBLOCK, dir_fd=fd)
                    try:
                        return os.read(item, 65536).decode(errors='replace').replace('\0', ' ')
                    finally:
                        os.close(item)
                except OSError as error:
                    return {'unavailable': str(error)}

            stat = read('stat')
            if not isinstance(stat, str) or int(stat.rpartition(')')[2].split()[19]) != birth:
                state['processes'].append({'pid': pid, 'error': 'process exited or identity changed'})
                continue
            process = {'pid': pid, 'birth_tick': birth, 'stat': stat,
                       'cmdline': read('cmdline'), 'status': read('status'),
                       'maps': read('maps'), 'threads': []}
            task_fd = os.open('task', os.O_DIRECTORY | os.O_RDONLY, dir_fd=fd)
            try:
                for tid in sorted(os.listdir(task_fd))[:256]:
                    if time.monotonic() >= end:
                        state['truncated'] = True
                        break
                    process['threads'].append({'tid': int(tid), **{
                        name: read(f'task/{tid}/{name}')
                        for name in ('stat', 'wchan', 'syscall', 'stack')}})
            finally:
                os.close(task_fd)
            state['processes'].append(process)
        except (OSError, ValueError, IndexError) as error:
            state['processes'].append({'pid': pid, 'error': str(error)})
        finally:
            os.close(fd)
    (output / 'process-state.json').write_text(json.dumps(state, indent=2))


def run(application, output, timeout_seconds=60, diagnostic_at=50):
    # Only tests shorten these values. CI's CLI has no deadline/retry override.
    if not 0 < diagnostic_at < timeout_seconds:
        raise ValueError('Diagnostics must precede the unchanged launch deadline')
    application = application.resolve(strict=True)
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)  # Old screenshots cannot grant a pass.
    private = Path(tempfile.mkdtemp(prefix='private-', dir=output))
    env = os.environ.copy()
    for name in list(env):
        if name.startswith(('DBUS_', 'PIPEWIRE_', 'PULSE_', 'XDG_')) or name in (
                'DISPLAY', 'WAYLAND_DISPLAY', 'ROSTRUM_UPDATE_URL', 'ROSTRUM_SENTRY_DSN',
                'ROSTRUM_SENTRY_DEBUG'):
            env.pop(name)
    env['HOME'] = str(private / 'home')
    for name, directory in (('HOME', 'home'), ('XDG_CONFIG_HOME', 'config'),
                            ('XDG_STATE_HOME', 'state'), ('XDG_DATA_HOME', 'data'),
                            ('XDG_CACHE_HOME', 'cache'), ('XDG_RUNTIME_DIR', 'runtime')):
        path = private / directory
        path.mkdir(mode=0o700)
        env[name] = str(path)
    env.update(QT_QPA_PLATFORM='offscreen', PIPEWIRE_REMOTE='rostrum-offscreen-none',
               PULSE_SERVER=f'unix:{private}/runtime/no-pulse',
               ROSTRUM_SCREENSHOT=str(output / 'screenshot.png'))
    job = None
    result = {'returncode': 1, 'cleanup_complete': False, 'error': None,
              'diagnostic_at_seconds': diagnostic_at, 'deadline_seconds': timeout_seconds}
    started = time.monotonic()
    interrupted = None
    cleaning = False
    old_handlers = {}

    def interrupt(sig, _frame):
        nonlocal interrupted
        if not cleaning and interrupted is None:
            interrupted = sig
            raise InterruptedError(f'Interrupted by {signal.Signals(sig).name}')

    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        old_handlers[sig] = signal.signal(sig, interrupt)
    try:
        with (output / 'app.log').open('wb') as log:
            job = Job(['timeout', str(timeout_seconds), 'dbus-run-session', '--', str(application)],
                      env, log, log, output / 'job-status.json')
            captured = False
            while job.poll() is None:
                if not captured and time.monotonic() - started >= diagnostic_at:
                    captured = True
                    try:
                        snapshot(job.process.pid, output)
                    except Exception as error:
                        (output / 'diagnostic-error.txt').write_text(str(error))
                        report(f'Passive startup diagnostics unavailable: {error}')
                if time.monotonic() - started >= timeout_seconds:
                    # GNU timeout can itself wait forever if a client ignores
                    # TERM. Do not grant more launch time; reclaim the owned job.
                    result['returncode'] = 124
                    break
                time.sleep(.02)  # Observed process status, not a startup-success delay.
            else:
                result['returncode'] = job.returncode
        if result['returncode'] == 0:
            screenshot = output / 'screenshot.png'
            if not screenshot.is_file() or screenshot.stat().st_size == 0:
                result.update(returncode=1, error='Missing or empty screenshot')
            elif QML_ERROR.search((output / 'app.log').read_text(errors='replace')):
                result.update(returncode=1, error='QML startup error')
    except BaseException as error:
        result.update(returncode=128 + interrupted if interrupted else 1, error=str(error))
        report(f'Offscreen check failed: {error}')
    finally:
        cleaning = True
        if job is not None:
            try:
                job.stop()
            except BaseException as error:
                # A cleanup error must not replace the actual timeout/app failure.
                result['cleanup_error'] = str(error)
                if result['returncode'] == 0:
                    result['returncode'] = 1
                report(f'Additional offscreen cleanup failure: {error}')
            result['cleanup_complete'] = job.closed
        # The private log may not exist if startup blocked before Logging::install.
        try:
            log = private / 'state/rostrum/rostrum.log'
            if log.is_file() and not log.is_symlink():
                shutil.copyfile(log, output / 'rostrum.log')
        except OSError as error:
            result['log_copy_error'] = str(error)
        try:
            if result['cleanup_complete']:
                shutil.rmtree(private)
            else:
                result['retained_private_directory'] = str(private)
        except OSError as error:
            result['resource_cleanup_error'] = str(error)
            if result['returncode'] == 0:
                result['returncode'] = 1
        result['elapsed_seconds'] = round(time.monotonic() - started, 3)
        (output / 'result.json').write_text(json.dumps(result, indent=2))
        for sig, handler in old_handlers.items():
            signal.signal(sig, handler)
    print((output / 'app.log').read_text(errors='replace') if (output / 'app.log').exists() else '')
    print(json.dumps(result))
    return result['returncode']


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('application', type=Path)
    parser.add_argument('output', type=Path)
    arguments = parser.parse_args()
    sys.exit(run(arguments.application, arguments.output))
