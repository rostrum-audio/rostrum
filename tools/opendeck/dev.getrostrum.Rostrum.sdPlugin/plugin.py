#!/usr/bin/env python3
"""OpenAction client; only Python's standard library and local Rostrum controls."""
import argparse
import asyncio
import base64
import hashlib
import inspect
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

_PLUGIN_DIR = Path(__file__).resolve().parent
if str(_PLUGIN_DIR) not in sys.path:
    sys.path.insert(0, str(_PLUGIN_DIR))
from dbus_client import DBusError, SessionBus, decode_body

SERVICE = 'dev.getrostrum.Rostrum'
IFACE = SERVICE + '1'
OBJECT = '/dev/getrostrum/Rostrum/Control'
PREFIX = SERVICE + '.'
BUS_SERVICE = 'org.freedesktop.DBus'
BUS_PATH = '/org/freedesktop/DBus'
BUS_IFACE = 'org.freedesktop.DBus'
PROPS_IFACE = 'org.freedesktop.DBus.Properties'
_NAME_OWNER_MATCH = (
    "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
    "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='dev.getrostrum.Rostrum'"
)
_PROPS_MATCH = (
    "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
    "path='/dev/getrostrum/Rostrum/Control',arg0='dev.getrostrum.Rostrum1'"
)
_LIST_METHODS = frozenset({'ListBuses', 'ListScenes'})


def _drive(coro):
    """Run a coroutine from the still-sync Plugin. A live loop is given the coroutine to await."""
    try:
        asyncio.get_running_loop()
    except RuntimeError:
        return asyncio.run(coro)
    return coro


class Unavailable(Exception):
    pass


class CommandMissing(Unavailable):
    pass


class Controls:
    def __init__(self, run=subprocess.run, executable=None, bus_factory=None):
        self.run = run
        self.executable = executable or shutil.which('rostrum') or str(Path.home() / '.local/bin/rostrum')
        self.bus_factory = bus_factory or SessionBus.connect
        self.bus = None
        self.owner = None
        self.cli_only = False
        self.props = {}
        self.on_properties = None
        self.on_owner = None
        self._closed = False
        self._tasks = set()

    def command(self, args):
        try:
            return self.run(args, capture_output=True, text=True, timeout=2)
        except FileNotFoundError as error:
            raise CommandMissing() from error
        except (OSError, subprocess.TimeoutExpired) as error:
            raise Unavailable() from error

    async def ensure_bus(self):
        if self._closed or self.bus is not None or self.cli_only:
            return
        bus = None
        try:
            bus = await self.bus_factory()
            bus.on_signal = self._on_signal
            self.bus = bus
            await self._subscribe()
            await self._refresh_owner()
        except TimeoutError:
            # Connect or the first daemon call never answered. Lists may use the CLI.
            await self._abandon(bus)
        except (ConnectionError, OSError, ValueError, DBusError):
            await self._abandon(bus)

    async def close(self):
        if self._closed:
            return
        self._closed = True
        bus = self.bus
        self.bus = None
        self.owner = None
        self.props = {}
        if bus is not None:
            bus.on_signal = None
            await _await_close(bus)
        pending = [task for task in list(self._tasks) if not task.done()]
        if pending:
            await asyncio.gather(*pending, return_exceptions=True)

    async def call(self, method, signature='', *args, cli=None):
        await self.ensure_bus()
        if self.bus is not None and self.owner and not self.cli_only:
            try:
                return await self.bus.call(self.owner, OBJECT, IFACE, method, signature, *args)
            except DBusError as error:
                if error.name.startswith(IFACE + '.Error.'):
                    raise ValueError(str(error)) from error
                if method not in _LIST_METHODS:
                    raise Unavailable() from error
                self.cli_only = True
            except TimeoutError as error:
                # asyncio.TimeoutError subclasses OSError. A lost reply may mean the
                # toggle already applied, so never repeat it through the CLI.
                if method not in _LIST_METHODS:
                    raise Unavailable() from error
                self.cli_only = True
            except OSError as error:
                if method not in _LIST_METHODS:
                    raise Unavailable() from error
                self.cli_only = True
        if cli is None:
            raise Unavailable()
        # --no-start makes this safe even if the owner disappeared since the last poll.
        result = await asyncio.to_thread(self.command, [self.executable, '--no-start', *cli])
        if result.returncode == 2:
            raise Unavailable()
        if result.returncode == 1 and method not in _LIST_METHODS:
            raise ValueError(result.stderr)
        if result.returncode:
            # Older Rostrum installs do not know --no-start; never retry without it.
            raise Unavailable()
        return result.stdout

    async def snapshot(self):
        await self.ensure_bus()
        # The bus is up and Rostrum is not on it. Do not query through the CLI either:
        # an absent owner is Unavailable, not a prompt to start an instance.
        if not self.cli_only and not self.owner:
            raise Unavailable()
        buses = await self.call('ListBuses', '', cli=['--list-buses'])
        scenes = await self.call('ListScenes', '', cli=['--list-scenes'])
        return _bus_map(buses), _scene_list(scenes)

    async def toggle(self, bus):
        action = {'stream': 'mute_stream', 'phones': 'mute_headphones'}.get(bus, 'mute_bus_' + bus)
        cli = ['--toggle-mic'] if bus == 'mic' else ['--action', action]
        await self.call('ToggleBusMuted', 's', bus, cli=cli)

    async def panic(self):
        await self.trigger('panic_mute')

    async def trigger(self, action):
        await self.call('TriggerAction', 's', action, cli=['--action', action])

    async def scene(self, name):
        await self.call('SwitchScene', 's', name, cli=['--scene', name])

    async def press(self, action):
        # Holds have no CLI equivalent. A short-lived process would release them on exit.
        await self.call('PressAction', 's', action)

    async def release(self, action):
        # The CLI exposes no hold actions. D-Bus also releases holds when a caller exits.
        await self.call('ReleaseAction', 's', action)

    def _on_signal(self, message):
        # Runs on the bus reader. Decode and return; never raise, never block.
        if self._closed or self.bus is None:
            return
        try:
            if message.member == 'NameOwnerChanged':
                self._signal_owner(message)
            elif message.member == 'PropertiesChanged':
                self._signal_properties(message)
        except Exception:
            return

    def _signal_owner(self, message):
        if message.interface not in (None, BUS_IFACE):
            return
        name, old, new = decode_body(message.signature or 'sss', message.body)
        if name != SERVICE:
            return
        self._spawn(self._handle_owner(old, new))

    def _signal_properties(self, message):
        if message.path not in (None, OBJECT):
            return
        if not message.sender or not self.owner or message.sender != self.owner:
            return
        iface, changed, invalidated = decode_body(message.signature or 'sa{sv}as', message.body)
        if iface != IFACE or not isinstance(changed, dict):
            return
        if changed:
            self.props.update(changed)
        if invalidated:
            self._spawn(self._load_props())
            return
        if changed:
            self._notify_properties()

    async def _subscribe(self):
        await self._daemon('Hello', '')
        await self._daemon('AddMatch', 's', _NAME_OWNER_MATCH)
        await self._daemon('AddMatch', 's', _PROPS_MATCH)

    async def _refresh_owner(self):
        try:
            body = await self._daemon('GetNameOwner', 's', SERVICE)
        except DBusError as error:
            if error.name == 'org.freedesktop.DBus.Error.NameHasNoOwner':
                await self._handle_owner(self.owner or '', '')
                return
            raise
        owner = body[0] if body else ''
        await self._handle_owner(self.owner or '', owner)

    async def _handle_owner(self, _old, new):
        # `_old` is NameOwnerChanged's previous unique name. Availability follows `new`.
        if self._closed:
            return
        try:
            owner = new if isinstance(new, str) and new.startswith(':') else None
            if owner is not None and owner == self.owner:
                return
            self.owner = owner
            self.props = {}
            if self._closed:
                return
            if self.owner is None:
                self._notify_owner()
                return
            await self._load_props()
            if not self._closed:
                self._notify_owner()
        except Exception:
            return

    def _spawn(self, coro):
        # The reader only holds a weak reference to tasks it creates. Keep one here.
        try:
            task = asyncio.get_running_loop().create_task(coro)
        except RuntimeError:
            coro.close()
            return None
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)
        return task

    async def _load_props(self):
        if self.bus is None or not self.owner or self._closed:
            return
        owner = self.owner
        try:
            body = await self.bus.call(owner, OBJECT, PROPS_IFACE, 'GetAll', 's', IFACE)
        except TimeoutError:
            return
        except (OSError, DBusError, ValueError):
            return
        if self.owner != owner:
            return
        props = body[0] if body else {}
        if isinstance(props, dict):
            self.props = dict(props)
            self._notify_properties()

    async def _daemon(self, member, signature, *args):
        return await self.bus.call(BUS_SERVICE, BUS_PATH, BUS_IFACE, member, signature, *args)

    async def _abandon(self, bus):
        self.cli_only = True
        self.bus = None
        self.owner = None
        self.props = {}
        if bus is None:
            return
        bus.on_signal = None
        await _await_close(bus)

    def _notify_owner(self):
        _notify(self.on_owner, self.owner)

    def _notify_properties(self):
        _notify(self.on_properties, self.props)


def _notify(callback, value):
    if callback is None:
        return
    try:
        callback(value)
    except Exception:
        return


async def _await_close(bus):
    close = getattr(bus, 'close', None)
    if close is None:
        return
    try:
        result = close()
        if inspect.isawaitable(result):
            await result
    except TimeoutError:
        pass
    except OSError:
        pass


def _bus_map(payload):
    if isinstance(payload, str):
        rows = []
        for line in payload.splitlines():
            if not line:
                continue
            bus_id, _level, muted, name = line.split('\t', 3)
            rows.append((bus_id, name, 0.0, muted == 'muted'))
    else:
        rows = payload[0] if isinstance(payload, tuple) else payload
    return {row[0]: {'id': row[0], 'name': row[1], 'muted': bool(row[3])} for row in rows}


def _scene_list(payload):
    if isinstance(payload, str):
        return payload.splitlines()
    names = payload[0] if isinstance(payload, tuple) else payload
    return list(names)


class Feedback:
    """Short local cues, pinned to headphones; no default-output fallback."""
    def __init__(self):
        self.process = None
        self.started = 0

    def play(self, muted, volume=50):
        self.close()  # Fast repeated presses never overlap or queue old feedback.
        if isinstance(volume, bool) or not isinstance(volume, (int, float)) or not math.isfinite(volume):
            volume = 50
        gain = (max(0, min(100, volume)) / 100) ** 2
        if gain == 0:
            return
        player = shutil.which('pw-play')
        if not player:
            return
        sound = Path(__file__).resolve().parent / 'sounds' / ('mic-muted.wav' if muted else 'mic-live.wav')
        properties = json.dumps({'application.name': 'Rostrum',
                                 'node.name': 'rostrum.opendeck-feedback',
                                 'node.dont-move': True, 'node.dont-fallback': True,
                                 'node.dont-reconnect': True})
        try:
            self.process = subprocess.Popen(
                [player, '--target', 'rostrum.phones', '--latency', '30ms', '--volume', str(gain),
                 '--properties', properties, str(sound)],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.started = time.monotonic()
        except OSError:
            self.process = None  # Feedback failure must never break mic control.

    def reap(self):
        if self.process and (self.process.poll() is not None or time.monotonic() - self.started > 2):
            self.close()

    def close(self):
        if self.process:
            if self.process.poll() is None:
                self.process.kill()
            self.process.wait()
            self.process = None


class Plugin:
    def __init__(self, controls, send):
        self.controls, self.send = controls, send
        self.keys, self.held = {}, {}
        self.feedback, self.pending_feedback = Feedback(), set()
        self.buses, self.scenes = {}, []
        self.available = False
        self.last_poll = float('-inf')

    def poll(self, force=False):
        return _drive(self._poll(force))

    async def _invoke(self, func, *args):
        result = func(*args)
        if inspect.isawaitable(result):
            return await result
        return result

    async def _poll(self, force=False):
        self.feedback.reap()
        now = time.monotonic()
        if not self.keys or (not force and now - self.last_poll < 1):
            return
        self.last_poll = now
        try:
            self.buses, self.scenes = await self._invoke(self.controls.snapshot)
            self.available = True
        except (Unavailable, ValueError, SyntaxError, IndexError, TypeError):
            self.available = False
        # Start the interval after the query, too, if an unavailable service took time to answer.
        self.last_poll = time.monotonic()
        pending, self.pending_feedback = self.pending_feedback, set()
        for context in pending:
            key = self.keys.get(context)
            if (self.available and 'mic' in self.buses and key
                    and key['settings'].get('playSound', True) is not False):
                self.feedback.play(self.buses['mic']['muted'], key['settings'].get('soundVolume', 50))
        for context in self.keys:
            self.render(context)

    def render(self, context):
        key = self.keys[context]
        action, settings = key['action'], key['settings']
        state = 0
        if not self.available:
            title = 'Unavailable'
        elif action == 'panic':
            title = 'Panic mute'
        elif action == 'scene':
            name = settings.get('scene', '')
            title = name if name in self.scenes else ('Missing' if name else 'Switch scene')
        else:
            bus = self.buses.get('mic' if action == 'mic' else settings.get('bus', ''))
            if bus:
                state = int(bus['muted'])
                title = ('Mic' if action == 'mic' else bus['name']) + (' muted' if state else ' live')
            else:
                title = 'Missing' if action == 'mic' or settings.get('bus') else 'Toggle bus mute'
        self.send({'event': 'setState', 'context': context, 'payload': {'state': state}})
        self.send({'event': 'setTitle', 'context': context, 'payload': {'title': title, 'target': 0}})

    def release(self, context):
        return _drive(self._release(context))

    async def _release(self, context):
        action = self.held.pop(context, None)
        if action:
            try:
                await self._invoke(self.controls.release, action)
            except (Unavailable, ValueError):
                pass

    def close(self):
        return _drive(self._close())

    async def _close(self):
        for context in list(self.held):
            await self._release(context)
        self.pending_feedback.clear()
        self.feedback.close()
        closer = getattr(self.controls, 'close', None)
        if closer is not None:
            try:
                await self._invoke(closer)
            except TimeoutError:
                pass
            except (Unavailable, OSError):
                pass

    def handle(self, event):
        return _drive(self._handle(event))

    async def _handle(self, event):
        kind, context = event.get('event'), event.get('context')
        payload = event.get('payload', {})
        if kind == 'willAppear':
            action = event.get('action', '').removeprefix(PREFIX)
            if action not in ('mic', 'panic', 'scene', 'bus'):
                return
            self.keys[context] = {'action': action, 'settings': payload.get('settings', {})}
            await self._poll()
            self.render(context)
        elif kind == 'willDisappear':
            self.pending_feedback.discard(context)
            await self._release(context)
            self.keys.pop(context, None)
        elif kind == 'keyUp':
            await self._release(context)
            await self._poll(force=True)
        elif context in self.keys:
            if kind == 'didReceiveSettings':
                self.keys[context]['settings'] = payload.get('settings', {})
                self.render(context)
            elif kind in ('sendToPlugin', 'propertyInspectorDidAppear'):
                await self._poll()
                self.send({'event': 'sendToPropertyInspector', 'action': PREFIX + self.keys[context]['action'],
                           'context': context, 'payload': {'available': self.available, 'scenes': self.scenes,
                                                         'buses': list(self.buses.values()),
                                                         'settings': self.keys[context]['settings']}})
            elif kind == 'keyDown':
                await self._poll()
                if not self.available:
                    self.render(context)
                    return
                key = self.keys[context]
                action, settings = key['action'], key['settings']
                try:
                    if action == 'mic' and 'mic' in self.buses:
                        await self._invoke(self.controls.toggle, 'mic')
                        self.pending_feedback.add(context)
                    elif action == 'panic':
                        await self._invoke(self.controls.panic)
                    elif action == 'scene' and settings.get('scene') in self.scenes:
                        await self._invoke(self.controls.scene, settings['scene'])
                    elif action == 'bus' and settings.get('bus') in self.buses:
                        await self._invoke(self.controls.toggle, settings['bus'])
                except Unavailable:
                    self.available = False
                except ValueError:
                    # The choice can disappear between the poll and the command.
                    if action == 'scene':
                        self.scenes = [name for name in self.scenes if name != settings.get('scene')]
                    elif action in ('bus', 'mic'):
                        self.buses.pop('mic' if action == 'mic' else settings.get('bus'), None)
                self.render(context)


class WebSocket:
    """Bounded RFC 6455 client; no listener, device access or third-party SDK."""
    def __init__(self, reader, writer):
        self.reader, self.writer = reader, writer

    @classmethod
    async def connect(cls, port):
        reader, writer = await asyncio.wait_for(asyncio.open_connection('127.0.0.1', port), 5)
        key = base64.b64encode(os.urandom(16)).decode()
        writer.write((f'GET / HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\n'
                      f'Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
        await writer.drain()
        header = (await asyncio.wait_for(reader.readuntil(b'\r\n\r\n'), 5)).decode('ascii')
        accept = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
        fields = dict(line.split(':', 1) for line in header.split('\r\n')[1:] if ':' in line)
        if header.split()[1] != '101' or {k.lower(): v.strip() for k, v in fields.items()}.get('sec-websocket-accept') != accept:
            writer.close()
            raise ValueError('OpenDeck WebSocket handshake failed')
        return cls(reader, writer)

    def frame(self, opcode, data):
        mask = os.urandom(4)
        length = len(data)
        header = bytes([0x80 | opcode])
        if length < 126:
            header += bytes([0x80 | length])
        elif length <= 65535:
            header += b'\xfe' + struct.pack('!H', length)
        else:
            header += b'\xff' + struct.pack('!Q', length)
        self.writer.write(header + mask + bytes(value ^ mask[i % 4] for i, value in enumerate(data)))

    def send(self, message):
        self.frame(1, json.dumps(message).encode())

    async def receive(self):
        message = bytearray()
        while True:
            flags, length = await self.reader.readexactly(2)
            opcode = flags & 15
            if length & 128:
                raise ValueError('Server frame must not be masked')
            if length == 126:
                length = struct.unpack('!H', await self.reader.readexactly(2))[0]
            elif length == 127:
                length = struct.unpack('!Q', await self.reader.readexactly(8))[0]
            if length + len(message) > 1024 * 1024:
                raise ValueError('OpenDeck message too large')
            data = await self.reader.readexactly(length)
            if opcode == 8:
                self.frame(8, data)
                raise EOFError()
            if opcode == 9:
                self.frame(10, data)
                continue
            if opcode == 10:
                continue
            if opcode not in (0, 1):
                raise ValueError('Expected a text message')
            message.extend(data)
            if flags & 128:
                return json.loads(message)


async def main(args):
    socket = await WebSocket.connect(args.port)
    socket.send({'event': args.registerEvent, 'uuid': args.pluginUUID})
    plugin = Plugin(Controls(), socket.send)
    receiving = asyncio.create_task(socket.receive())
    try:
        while True:
            done, _ = await asyncio.wait([receiving], timeout=0.1)
            # Control calls stay on this thread: sends and socket frames cannot interleave.
            if done:
                await plugin.handle(receiving.result())
                receiving = asyncio.create_task(socket.receive())
            await plugin.poll()
            await socket.writer.drain()
    finally:
        await plugin.close()
        receiving.cancel()
        socket.writer.close()
        await socket.writer.wait_closed()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('-port', type=int, required=True)
    parser.add_argument('-pluginUUID', required=True)
    parser.add_argument('-registerEvent', required=True)
    parser.add_argument('-info', default='{}')
    try:
        asyncio.run(main(parser.parse_args()))
    except (OSError, EOFError, asyncio.IncompleteReadError, ValueError):
        raise SystemExit(2)
