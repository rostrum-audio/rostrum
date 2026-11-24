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
                # Rostrum answered but cannot serve this yet: it claims its name before it
                # registers the Control object (UnknownObject), or it is leaving. The CLI
                # would reach the same instance over the same bus; the next poll retries.
                raise Unavailable() from error
            except TimeoutError as error:
                # asyncio.TimeoutError subclasses OSError. A lost reply may mean the
                # toggle already applied, and a hung instance would hang the CLI too.
                raise Unavailable() from error
            except OSError as error:
                # The connection itself is gone. Its properties would go stale, so drop
                # them; lists continue through the CLI. A toggle may already have applied.
                await self._abandon(self.bus)
                if method not in _LIST_METHODS:
                    raise Unavailable() from error
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
        if not self.cli_only and self.owner and not self.props:
            # GetAll on NameOwnerChanged can beat the Control object's registration, and
            # Rostrum's first publish sends no PropertiesChanged. Load them once lists work.
            await self._load_props()
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
        self._tasks = set()
        self._polled = False
        # Signals redraw every key. main assigns the same callbacks so the wiring stays visible.
        controls.on_properties = self._on_properties
        controls.on_owner = self._on_owner

    def _on_properties(self, _props):
        # GetAll during the first poll must not paint before lists and availability settle.
        if not self._polled:
            return
        self._render_all()

    def _on_owner(self, owner):
        # Holds lived on the previous instance (Rostrum releases them when it quits, and a
        # new instance starts with none). Forget them; never press again on someone's behalf.
        self.held.clear()
        if owner:
            # The forced poll paints. An early render would show the previous process's buses.
            if self._polled:
                self._schedule(self.poll(force=True))
            return
        # CLI mode has no owner. An empty owner there is not a quit.
        if getattr(self.controls, 'cli_only', False):
            return
        self.available = False
        self._render_all()

    def _schedule(self, coro):
        try:
            task = asyncio.get_running_loop().create_task(coro)
        except RuntimeError:
            coro.close()
            return None
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)
        return task

    async def _invoke(self, func, *args):
        result = func(*args)
        if inspect.isawaitable(result):
            return await result
        return result

    def _props(self):
        props = getattr(self.controls, 'props', None)
        return props if isinstance(props, dict) else {}

    def _snapshot_available(self):
        """True when a returned list still belongs to a live owner, or to CLI-only mode.

        The mic-feedback fake has neither attribute; a snapshot that returned means it is up.
        """
        controls = self.controls
        if not hasattr(controls, 'owner') and not hasattr(controls, 'cli_only'):
            return True
        if getattr(controls, 'cli_only', False):
            return True
        return bool(getattr(controls, 'owner', None))

    def _props_fresh(self):
        # A live owner with properties is enough to act. Lists stay on the 1 s poll.
        return bool(getattr(self.controls, 'owner', None)) and bool(self._props())

    async def _ready(self):
        """Cheap presence check. Does not list buses or scenes."""
        controls = self.controls
        if not hasattr(controls, 'owner') and not hasattr(controls, 'cli_only'):
            return self.available
        ensure = getattr(controls, 'ensure_bus', None)
        if ensure is not None:
            await self._invoke(ensure)
        if getattr(controls, 'cli_only', False):
            return True
        return bool(getattr(controls, 'owner', None))

    async def poll(self, force=False):
        self.feedback.reap()
        now = time.monotonic()
        if not self.keys or (not force and now - self.last_poll < 1):
            return
        self.last_poll = now
        try:
            self.buses, self.scenes = await self._invoke(self.controls.snapshot)
            # A list that is still in flight after the owner leaves must not mark the deck live.
            self.available = self._snapshot_available()
        except (Unavailable, ValueError, SyntaxError, IndexError, TypeError):
            self.available = False
        # Start the interval after the query, too, if an unavailable service took time to answer.
        self.last_poll = time.monotonic()
        self._polled = True
        pending, self.pending_feedback = self.pending_feedback, set()
        for context in pending:
            key = self.keys.get(context)
            if (self.available and 'mic' in self.buses and key
                    and key['settings'].get('playSound', True) is not False):
                self.feedback.play(self.buses['mic']['muted'], key['settings'].get('soundVolume', 50))
        self._render_all()

    def _appearance(self, key, context=None):
        action, settings = key['action'], key['settings']
        props = self._props()
        if not self.available:
            return 0, 'Unavailable'
        if action == 'pushToTalk':
            # Holds need this process's own bus connection; the CLI has none.
            if getattr(self.controls, 'cli_only', False):
                return 0, 'Unavailable'
            if context not in self.held:
                return 0, 'Push to talk'
            # Panic or push to mute beat push to talk: held, yet the mic stays muted.
            if props.get('MicMuted') is True:
                return 0, 'Blocked'
            return 1, 'Talking'
        if action == 'panic':
            if 'Panic' in props:
                state = int(bool(props['Panic']))
                return state, 'Panic on' if state else 'Panic mute'
            return 0, 'Panic mute'
        if action == 'scene':
            name = settings.get('scene', '')
            if not name:
                return 0, 'Switch scene'
            current = props.get('CurrentScene')
            if name in self.scenes or current == name:
                return int(current == name), name
            return 0, 'Missing'
        if action == 'studioVoice':
            # MicFilters means switched on, not that the filters are running.
            if 'MicFilters' in props:
                state = int(bool(props['MicFilters']))
                return state, 'Voice on' if state else 'Voice off'
            return 0, 'Studio Voice'
        if action == 'mic':
            if 'MicMuted' in props:
                state = int(bool(props['MicMuted']))
                return state, 'Mic muted' if state else 'Mic live'
            bus = self.buses.get('mic')
            if bus:
                state = int(bool(bus['muted']))
                return state, 'Mic muted' if state else 'Mic live'
            return 0, 'Missing'
        bus_id = settings.get('bus', '')
        bus = self.buses.get(bus_id)
        # Signals drive the stream master. Other buses have no mute property.
        if bus_id == 'stream' and 'StreamMuted' in props:
            state = int(bool(props['StreamMuted']))
            name = bus['name'] if bus else 'Stream'
            return state, name + (' muted' if state else ' live')
        if bus:
            state = int(bool(bus['muted']))
            return state, bus['name'] + (' muted' if state else ' live')
        return 0, 'Missing' if bus_id else 'Toggle bus mute'

    def _alert(self, context):
        self.send({'event': 'showAlert', 'context': context})

    def _paint(self, context):
        key = self.keys.get(context)
        if key is None:
            return
        state, title = self._appearance(key, context)
        same_state = 'state' in key and key['state'] == state
        same_title = 'title' in key and key['title'] == title
        if same_state and same_title:
            return
        # One alert when a key becomes Unavailable or Blocked, not on every later redraw.
        if title in ('Unavailable', 'Blocked') and key.get('title') != title:
            self._alert(context)
        if not same_state:
            key['state'] = state
            self.send({'event': 'setState', 'context': context, 'payload': {'state': state}})
        if not same_title:
            key['title'] = title
            self.send({'event': 'setTitle', 'context': context, 'payload': {'title': title, 'target': 0}})

    def _render_all(self):
        for context in list(self.keys):
            self._paint(context)

    async def render(self, context):
        self._paint(context)

    async def release(self, context):
        action = self.held.pop(context, None)
        # Rostrum keeps one flag per hold action, not a count: release only with the last key.
        if action and action not in self.held.values():
            try:
                await self._invoke(self.controls.release, action)
            except (Unavailable, ValueError):
                pass

    async def close(self):
        for context in list(self.held):
            await self.release(context)
        self.pending_feedback.clear()
        self.feedback.close()
        for task in list(self._tasks):
            task.cancel()
        if self._tasks:
            await asyncio.gather(*self._tasks, return_exceptions=True)
        closer = getattr(self.controls, 'close', None)
        if closer is not None:
            try:
                await self._invoke(closer)
            except TimeoutError:
                pass
            except (Unavailable, OSError):
                pass

    async def handle(self, event):
        kind, context = event.get('event'), event.get('context')
        payload = event.get('payload', {})
        if kind == 'willAppear':
            action = event.get('action', '').removeprefix(PREFIX)
            if action not in ('mic', 'panic', 'scene', 'bus', 'studioVoice', 'pushToTalk'):
                return
            self.keys[context] = {'action': action, 'settings': payload.get('settings', {})}
            await self.poll()
            await self.render(context)
        elif kind == 'willDisappear':
            self.pending_feedback.discard(context)
            self.keys.pop(context, None)
            await self.release(context)
        elif kind == 'keyUp':
            await self.release(context)
            await self.poll(force=True)
        elif context in self.keys:
            if kind == 'didReceiveSettings':
                self.keys[context]['settings'] = payload.get('settings', {})
                await self.render(context)
            elif kind in ('sendToPlugin', 'propertyInspectorDidAppear'):
                await self.poll()
                self.send({'event': 'sendToPropertyInspector', 'action': PREFIX + self.keys[context]['action'],
                           'context': context, 'payload': {'available': self.available, 'scenes': self.scenes,
                                                         'buses': list(self.buses.values()),
                                                         'settings': self.keys[context]['settings']}})
            elif kind == 'keyDown':
                await self._key_down(context)

    async def _push_to_talk(self, context):
        # No poll first: a refresh before PressAction would clip the first syllable.
        if context in self.held:
            return
        if not await self._ready():
            self.available = False
            self._alert_if_still_down(context)
            self._paint(context)
            return
        if getattr(self.controls, 'cli_only', False):
            self._alert(context)  # never a mute toggle in place of a hold
            self._paint(context)
            return
        if 'push_to_talk' not in self.held.values():
            try:
                await self._invoke(self.controls.press, 'push_to_talk')
            except Unavailable:
                self.available = False
                self._alert_if_still_down(context)
                self._paint(context)
                return
            except ValueError:
                self._alert(context)
                self._paint(context)
                return
        # Rostrum publishes MicMuted before it replies, so the props already show the result.
        self.held[context] = 'push_to_talk'
        self.available = True
        self._paint(context)

    async def _key_down(self, context):
        # Do not poll(force=True) here; keys act on the state signals already delivered.
        if self.keys[context]['action'] == 'pushToTalk':
            await self._push_to_talk(context)
            return
        if not await self._ready():
            self.available = False
            self._alert_if_still_down(context)
            self._paint(context)
            return
        if not self._props_fresh():
            await self.poll()
            if not self.available:
                self._alert_if_still_down(context)
                self._paint(context)
                return
        key = self.keys[context]
        action, settings = key['action'], key['settings']
        props = self._props()
        acted = False
        try:
            if action == 'mic' and ('MicMuted' in props or 'mic' in self.buses):
                await self._invoke(self.controls.toggle, 'mic')
                self.pending_feedback.add(context)
                acted = True
            elif action == 'panic':
                await self._invoke(self.controls.panic)
                acted = True
            elif action == 'studioVoice':
                # Rostrum flips its own value; SetMicFilters(!cached) could undo a window change.
                await self._invoke(self.controls.trigger, 'toggle_mic_filters')
                acted = True
            elif action == 'scene' and settings.get('scene') in self.scenes:
                await self._invoke(self.controls.scene, settings['scene'])
                acted = True
            elif action == 'bus' and settings.get('bus') in self.buses:
                await self._invoke(self.controls.toggle, settings['bus'])
                acted = True
        except Unavailable:
            self.available = False
        except ValueError:
            # The choice can disappear between the snapshot and the command.
            if action == 'scene':
                self.scenes = [name for name in self.scenes if name != settings.get('scene')]
            elif action in ('bus', 'mic'):
                self.buses.pop('mic' if action == 'mic' else settings.get('bus'), None)
        if acted:
            # A press that ran means the service answered, even if an earlier poll latched Unavailable.
            self.available = True
        else:
            self._alert_if_still_down(context)
        self._paint(context)

    def _alert_if_still_down(self, context):
        """Alert a press that did not run. The Unavailable transition alerts inside _paint."""
        key = self.keys.get(context)
        if key is None:
            return
        if not self.available and key.get('title') != 'Unavailable':
            return
        self._alert(context)


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
    controls = Controls()
    plugin = Plugin(controls, socket.send)
    controls.on_properties = plugin._on_properties
    controls.on_owner = plugin._on_owner
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
