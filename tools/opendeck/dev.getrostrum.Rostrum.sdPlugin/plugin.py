#!/usr/bin/env python3
"""OpenAction client; only Python's standard library and local Rostrum controls."""
import argparse
import ast
import asyncio
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time
import tokenize

SERVICE = 'dev.getrostrum.Rostrum'
IFACE = SERVICE + '1'
OBJECT = '/dev/getrostrum/Rostrum/Control'
PREFIX = SERVICE + '.'


def variant(text):
    """Decode gdbus's tuple/array output without evaluating code or string contents."""
    source = list(tokenize.generate_tokens(io.StringIO(text).readline))
    tokens = []
    index = 0
    while index < len(source):
        token = source[index]
        if token.string == '@' and token.type == tokenize.OP:
            # Empty GVariant arrays carry a type annotation, e.g. @as or @a(ssdb).
            index += 2
            if index < len(source) and source[index].string == '(':
                depth = 1
                index += 1
                while index < len(source) and depth:
                    depth += (source[index].string == '(') - (source[index].string == ')')
                    index += 1
            continue
        if token.type == tokenize.NAME and token.string in ('true', 'false'):
            token = token._replace(string={'true': 'True', 'false': 'False'}[token.string])
        tokens.append(token)
        index += 1
    return ast.literal_eval(tokenize.untokenize(tokens))


class Unavailable(Exception):
    pass


class CommandMissing(Unavailable):
    pass


class Controls:
    def __init__(self, run=subprocess.run, executable=None):
        self.run = run
        self.executable = executable or shutil.which('rostrum') or str(Path.home() / '.local/bin/rostrum')
        self.owner = None
        self.cli_only = False

    def command(self, args):
        try:
            return self.run(args, capture_output=True, text=True, timeout=2)
        except FileNotFoundError as error:
            raise CommandMissing() from error
        except (OSError, subprocess.TimeoutExpired) as error:
            raise Unavailable() from error

    def dbus(self, destination, method, *args):
        return self.command(['gdbus', 'call', '--session', '--dest', destination,
                             '--object-path', '/org/freedesktop/DBus' if destination == 'org.freedesktop.DBus' else OBJECT,
                             '--method', method, *(json.dumps(arg, ensure_ascii=False) for arg in args)])

    def call(self, method, args=(), cli=None):
        if self.owner and not self.cli_only:
            try:
                result = self.dbus(self.owner, IFACE + '.' + method, *args)
                if result.returncode == 0:
                    return variant(result.stdout)
                if IFACE + '.Error.' in result.stderr:
                    raise ValueError(result.stderr)
                if method not in ('ListBuses', 'ListScenes'):
                    # A lost reply may follow an applied toggle; do not apply it again.
                    raise Unavailable()
                self.cli_only = True
            except CommandMissing:
                self.cli_only = True
        # --no-start makes this safe even if the owner disappeared since the last poll.
        if cli is None:
            raise Unavailable()
        result = self.command([self.executable, '--no-start', *cli])
        if result.returncode == 2:
            raise Unavailable()
        if result.returncode == 1 and method not in ('ListBuses', 'ListScenes'):
            raise ValueError(result.stderr)
        if result.returncode:
            # Older Rostrum installs do not know --no-start; never retry without it.
            raise Unavailable()
        return result.stdout

    def snapshot(self):
        self.owner = None
        self.cli_only = False
        try:
            result = self.dbus('org.freedesktop.DBus', 'org.freedesktop.DBus.GetNameOwner', SERVICE)
        except CommandMissing:
            # A guarded CLI query also works when gdbus is not installed.
            result = None
            self.cli_only = True
        if result is not None:
            if result.returncode:
                raise Unavailable()
            self.owner = variant(result.stdout)[0]
        buses = self.call('ListBuses', cli=['--list-buses'])
        scenes = self.call('ListScenes', cli=['--list-scenes'])
        if isinstance(buses, str):
            rows = []
            for line in buses.splitlines():
                bus_id, level, muted, name = line.split('\t', 3)
                rows.append((bus_id, name, float(level), muted == 'muted'))
        else:
            rows = buses[0]
        return {row[0]: {'id': row[0], 'name': row[1], 'muted': row[3]} for row in rows}, \
            scenes.splitlines() if isinstance(scenes, str) else scenes[0]

    def toggle(self, bus):
        action = {'stream': 'mute_stream', 'phones': 'mute_headphones'}.get(bus, 'mute_bus_' + bus)
        self.call('ToggleBusMuted', [bus], ['--toggle-mic'] if bus == 'mic' else ['--action', action])

    def panic(self):
        self.call('TriggerAction', ['panic_mute'], ['--action', 'panic_mute'])

    def scene(self, name):
        self.call('SwitchScene', [name], ['--scene', name])

    def release(self, action):
        # The CLI exposes no hold actions. D-Bus also releases holds when a caller exits.
        self.call('ReleaseAction', [action])


class Plugin:
    def __init__(self, controls, send):
        self.controls, self.send = controls, send
        self.keys, self.held = {}, {}
        self.buses, self.scenes = {}, []
        self.available = False
        self.last_poll = float('-inf')

    def poll(self, force=False):
        now = time.monotonic()
        if not self.keys or (not force and now - self.last_poll < 1):
            return
        self.last_poll = now
        try:
            self.buses, self.scenes = self.controls.snapshot()
            self.available = True
        except (Unavailable, ValueError, SyntaxError, IndexError, TypeError):
            self.available = False
        # Start the interval after the query, too, if an unavailable service took time to answer.
        self.last_poll = time.monotonic()
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
        action = self.held.pop(context, None)
        if action:
            try:
                self.controls.release(action)
            except (Unavailable, ValueError):
                pass

    def close(self):
        for context in list(self.held):
            self.release(context)

    def handle(self, event):
        kind, context = event.get('event'), event.get('context')
        payload = event.get('payload', {})
        if kind == 'willAppear':
            action = event.get('action', '').removeprefix(PREFIX)
            if action not in ('mic', 'panic', 'scene', 'bus'):
                return
            self.keys[context] = {'action': action, 'settings': payload.get('settings', {})}
            self.poll()
            self.render(context)
        elif kind == 'willDisappear':
            self.release(context)
            self.keys.pop(context, None)
        elif kind == 'keyUp':
            self.release(context)
            self.poll(force=True)
        elif context in self.keys:
            if kind == 'didReceiveSettings':
                self.keys[context]['settings'] = payload.get('settings', {})
                self.render(context)
            elif kind in ('sendToPlugin', 'propertyInspectorDidAppear'):
                self.poll()
                self.send({'event': 'sendToPropertyInspector', 'action': PREFIX + self.keys[context]['action'],
                           'context': context, 'payload': {'available': self.available, 'scenes': self.scenes,
                                                         'buses': list(self.buses.values()),
                                                         'settings': self.keys[context]['settings']}})
            elif kind == 'keyDown':
                self.poll()
                if not self.available:
                    self.render(context)
                    return
                key = self.keys[context]
                action, settings = key['action'], key['settings']
                try:
                    if action == 'mic' and 'mic' in self.buses:
                        self.controls.toggle('mic')
                    elif action == 'panic':
                        self.controls.panic()
                    elif action == 'scene' and settings.get('scene') in self.scenes:
                        self.controls.scene(settings['scene'])
                    elif action == 'bus' and settings.get('bus') in self.buses:
                        self.controls.toggle(settings['bus'])
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
                plugin.handle(receiving.result())
                receiving = asyncio.create_task(socket.receive())
            plugin.poll()
            await socket.writer.drain()
    finally:
        plugin.close()
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
