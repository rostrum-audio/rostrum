"""No deck or running mixer required: python3 -B tools/opendeck/test_plugin.py."""
import asyncio
import base64
import hashlib
import importlib.util
import os
import struct
import json
import tempfile
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch, Mock
import wave

ROOT = Path(__file__).parent / 'dev.getrostrum.Rostrum.sdPlugin'
spec = importlib.util.spec_from_file_location('plugin', ROOT / 'plugin.py')
plugin = importlib.util.module_from_spec(spec)
spec.loader.exec_module(plugin)
dbus_spec = importlib.util.spec_from_file_location('dbus_client', ROOT / 'dbus_client.py')
plugin_dbus = importlib.util.module_from_spec(dbus_spec)
dbus_spec.loader.exec_module(plugin_dbus)


class CliRunner:
    def __init__(self):
        self.commands = []

    def __call__(self, command, **kwargs):
        self.commands.append(list(command))
        if command and command[0] == 'gdbus':
            raise AssertionError('gdbus must not be used')
        if '--list-buses' in command:
            output = 'mic\t1.00\tunmuted\tMic\ngame\t1.00\tmuted\tGame\n'
        elif '--list-scenes' in command:
            output = 'Live\nJust Chatting\n'
        else:
            output = ''
        return subprocess.CompletedProcess(command, 0, output, '')


class FakeBus:
    def __init__(self, owner=':1.42'):
        self.owner = owner
        self.calls = []
        self.on_signal = None
        self.failures = {}
        self.props = {'MicMuted': False, 'Panic': False, 'CurrentScene': 'Live', 'MicFilters': False}
        self.closed = False

    async def call(self, destination, path, interface, member, signature='', *args, timeout=2.0):
        self.calls.append({'destination': destination, 'path': path, 'interface': interface,
                           'member': member, 'signature': signature, 'args': args})
        failure = self.failures.get(member)
        if failure is not None:
            raise failure() if isinstance(failure, type) else failure
        if member == 'Hello':
            return (':1.99',)
        if member == 'AddMatch':
            return ()
        if member == 'GetNameOwner':
            if not self.owner:
                raise plugin.DBusError('org.freedesktop.DBus.Error.NameHasNoOwner', 'no owner')
            return (self.owner,)
        if interface == plugin.IFACE or member == 'GetAll':
            if destination != self.owner:
                raise AssertionError(destination)
        if member == 'GetAll':
            return (dict(self.props),)
        if member == 'ListBuses':
            return ([('mic', 'Mic', 1.0, False), ('game', 'Game', 1.0, True)],)
        if member == 'ListScenes':
            return (['Live', 'Just Chatting'],)
        if member in ('ToggleBusMuted', 'TriggerAction', 'SwitchScene', 'PressAction', 'ReleaseAction'):
            return ()
        raise AssertionError(member)

    async def close(self):
        self.closed = True
        self.on_signal = None


def fake_connect(bus):
    async def connect(address=None):
        return bus
    return connect


async def refuse_bus(address=None):
    raise OSError('no session bus')


def _signal(member, signature, values, interface, sender=None, path=None):
    body = plugin_dbus.encode_body(signature, *values)
    return plugin_dbus.Message(4, 0, 1, body, path=path, interface=interface, member=member,
                               sender=sender, signature=signature)


class PluginTest(unittest.TestCase):
    def setup_plugin(self, *, owner=':1.42', connect=True, run=None):
        bus = FakeBus(owner=owner)
        runner = run or CliRunner()
        factory = fake_connect(bus) if connect else refuse_bus
        backend = plugin.Controls(run=runner, executable='/test/rostrum', bus_factory=factory)
        events = []
        app = plugin.Plugin(backend, events.append)
        return app, bus, runner, events

    def appear(self, app, action='mic', settings=None):
        app.handle({'event': 'willAppear', 'context': 'key',
                    'action': plugin.PREFIX + action, 'payload': {'settings': settings or {}}})

    def down(self, app):
        app.handle({'event': 'keyDown', 'context': 'key'})

    def test_absent_does_not_launch(self):
        app, bus, runner, events = self.setup_plugin(owner=None)
        self.appear(app)
        self.down(app)
        self.assertEqual(events[-1]['payload']['title'], 'Unavailable')
        self.assertEqual(runner.commands, [])
        self.assertTrue(any(call['member'] == 'GetNameOwner' and call['args'] == (plugin.SERVICE,)
                            for call in bus.calls))
        self.assertFalse(any(call['member'] in ('ListBuses', 'ToggleBusMuted') for call in bus.calls))

    def test_mic_command_and_state(self):
        app, bus, runner, events = self.setup_plugin()
        self.appear(app)
        self.down(app)
        toggles = [call for call in bus.calls if call['member'] == 'ToggleBusMuted']
        self.assertEqual(toggles[-1]['args'], ('mic',))
        self.assertEqual(toggles[-1]['destination'], ':1.42')
        self.assertNotEqual(toggles[-1]['destination'], plugin.SERVICE)
        self.assertEqual(toggles[-1]['path'], plugin.OBJECT)
        self.assertTrue(any(event['payload'].get('title') == 'Mic live' for event in events))
        self.assertEqual(runner.commands, [])

    def test_named_scene(self):
        app, bus, runner, _ = self.setup_plugin()
        self.appear(app, 'scene', {'scene': 'Just Chatting'})
        self.down(app)
        self.assertTrue(any(call['member'] == 'SwitchScene' and call['args'] == ('Just Chatting',)
                            and call['destination'] == ':1.42' for call in bus.calls))
        self.assertEqual(runner.commands, [])

    def test_bus_id(self):
        app, bus, runner, events = self.setup_plugin()
        self.appear(app, 'bus', {'bus': 'game'})
        self.down(app)
        self.assertTrue(any(call['member'] == 'ToggleBusMuted' and call['args'] == ('game',)
                            and call['destination'] == ':1.42' for call in bus.calls))
        self.assertTrue(any(event['payload'].get('title') == 'Game muted' for event in events))
        self.assertEqual(runner.commands, [])

    def test_panic(self):
        app, bus, runner, _ = self.setup_plugin()
        self.appear(app, 'panic')
        self.down(app)
        self.assertTrue(any(call['member'] == 'TriggerAction' and call['args'] == ('panic_mute',)
                            and call['destination'] == ':1.42' for call in bus.calls))
        self.assertEqual(runner.commands, [])

    def test_key_up_releases_and_refreshes(self):
        app, bus, _, _ = self.setup_plugin()
        self.appear(app)
        # No hold action is exposed. Exercise cleanup if a future action records a press.
        app.held['key'] = 'push_to_talk'
        before = len(bus.calls)
        app.handle({'event': 'keyUp', 'context': 'key'})
        self.assertEqual(app.held, {})
        later = bus.calls[before:]
        self.assertTrue(any(call['member'] == 'ReleaseAction' and call['args'] == ('push_to_talk',)
                            and call['destination'] == ':1.42' for call in later))
        self.assertTrue(any(call['member'] == 'ListBuses' for call in later))

    def test_cli_fallback_never_starts(self):
        app, _, runner, _ = self.setup_plugin(connect=False)
        for action, settings, args in [('mic', {}, ['--toggle-mic']),
                                       ('scene', {'scene': 'Just Chatting'}, ['--scene', 'Just Chatting']),
                                       ('bus', {'bus': 'game'}, ['--action', 'mute_bus_game']),
                                       ('panic', {}, ['--action', 'panic_mute'])]:
            self.appear(app, action, settings)
            self.down(app)
            self.assertIn(['/test/rostrum', '--no-start'] + args, runner.commands)
        self.assertTrue(app.controls.cli_only)
        self.assertTrue(all(command[:2] == ['/test/rostrum', '--no-start'] for command in runner.commands))

    def test_missing_and_empty_choices(self):
        for action, settings in [('scene', {'scene': 'Gone'}), ('bus', {'bus': 'gone'})]:
            app, bus, _, events = self.setup_plugin()
            self.appear(app, action, settings)
            before = len(bus.calls)
            self.down(app)
            self.assertEqual(len(bus.calls), before)
            self.assertEqual(events[-1]['payload']['title'], 'Missing')
        app, bus, _, _ = self.setup_plugin()
        self.appear(app, 'scene')
        before = len(bus.calls)
        self.down(app)
        self.assertEqual(len(bus.calls), before)

    def test_poll_rate_and_profile_closed(self):
        app, bus, _, _ = self.setup_plugin()
        self.appear(app)
        before = len(bus.calls)
        for _ in range(10):
            app.poll()
        self.assertEqual(len(bus.calls), before)
        app.handle({'event': 'willDisappear', 'context': 'key'})
        app.poll(force=True)
        self.assertEqual(len(bus.calls), before)

    def test_disappeared_choice_in_cli_fallback(self):
        runner = CliRunner()

        def run(command, **kwargs):
            if '--scene' in command:
                runner.commands.append(list(command))
                return subprocess.CompletedProcess(command, 1, '', 'No such scene')
            return runner(command, **kwargs)

        app, _, _, events = self.setup_plugin(connect=False, run=run)
        self.appear(app, 'scene', {'scene': 'Just Chatting'})
        self.down(app)
        self.assertEqual(events[-1]['payload']['title'], 'Missing')
        self.assertTrue(any(command[:2] == ['/test/rostrum', '--no-start'] for command in runner.commands))

    @unittest.skipUnless(plugin.shutil.which('node'), 'optional inspector check requires Node.js')
    def test_inspector_settings_round_trip(self):
        result = subprocess.run(['node', str(ROOT.parent / 'test_inspector.js')], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_manifest_assets(self):
        manifest = json.loads((ROOT / 'manifest.json').read_text())
        self.assertEqual(len(manifest['Actions']), 4)
        self.assertNotIn('OnlyShowIn', manifest)
        self.assertTrue((ROOT / manifest['CodePathLin']).stat().st_mode & 0o111)
        for action in manifest['Actions']:
            self.assertNotIn('OnlyShowIn', action)
            for state in action['States']:
                self.assertTrue((ROOT / (state['Image'] + '.svg')).is_file())


class ControlsTest(unittest.IsolatedAsyncioTestCase):
    def controls(self, *, owner=':1.42', connect=True, run=None):
        bus = FakeBus(owner=owner)
        runner = run or CliRunner()
        factory = fake_connect(bus) if connect else refuse_bus
        backend = plugin.Controls(run=runner, executable='/test/rostrum', bus_factory=factory)
        return backend, bus, runner

    async def test_subscribes_and_loads_properties_from_unique_owner(self):
        controls, bus, runner = self.controls()
        await controls.ensure_bus()
        buses, scenes = await controls.snapshot()
        self.assertEqual(controls.owner, ':1.42')
        self.assertFalse(controls.cli_only)
        self.assertEqual(controls.props['MicMuted'], False)
        self.assertEqual(controls.props['CurrentScene'], 'Live')
        self.assertIn('MicFilters', controls.props)
        self.assertEqual(buses['mic']['name'], 'Mic')
        self.assertIn('Just Chatting', scenes)
        members = [call['member'] for call in bus.calls]
        self.assertEqual(members[:5], ['Hello', 'AddMatch', 'AddMatch', 'GetNameOwner', 'GetAll'])
        rules = [call['args'][0] for call in bus.calls if call['member'] == 'AddMatch']
        self.assertTrue(any('NameOwnerChanged' in rule and plugin.SERVICE in rule for rule in rules))
        self.assertTrue(any('PropertiesChanged' in rule and plugin.IFACE in rule for rule in rules))
        get_all = next(call for call in bus.calls if call['member'] == 'GetAll')
        self.assertEqual(get_all['destination'], ':1.42')
        self.assertNotEqual(get_all['destination'], plugin.SERVICE)
        self.assertEqual(get_all['args'], (plugin.IFACE,))
        self.assertTrue(all(call['destination'] == ':1.42'
                            for call in bus.calls if call['interface'] == plugin.IFACE))
        self.assertEqual(runner.commands, [])
        await controls.close()
        self.assertTrue(bus.closed)

    async def test_empty_owner_clears_props_and_notifies(self):
        controls, bus, _ = self.controls()
        await controls.ensure_bus()
        owners = []
        controls.on_owner = owners.append

        def explode(_owner):
            raise RuntimeError('owner callback')

        controls._on_signal(_signal(
            'NameOwnerChanged', 'sss', (plugin.SERVICE, ':1.42', ''),
            plugin.BUS_IFACE, path=plugin.BUS_PATH))
        for _ in range(3):
            await asyncio.sleep(0)
        self.assertIsNone(controls.owner)
        self.assertEqual(controls.props, {})
        self.assertEqual(owners, [None])
        controls.on_owner = explode
        controls._on_signal(_signal(
            'NameOwnerChanged', 'sss', (plugin.SERVICE, '', ''),
            plugin.BUS_IFACE))
        for _ in range(3):
            await asyncio.sleep(0)
        self.assertIsNone(controls.owner)
        await controls.close()
        self.assertTrue(bus.closed)

    async def test_properties_changed_updates_props(self):
        controls, _, _ = self.controls()
        await controls.ensure_bus()
        seen = []
        controls.on_properties = seen.append
        controls._on_signal(_signal(
            'PropertiesChanged', 'sa{sv}as', (plugin.IFACE, {'MicMuted': True, 'Panic': True}, []),
            plugin.PROPS_IFACE, sender=':1.42', path=plugin.OBJECT))
        self.assertTrue(controls.props['MicMuted'])
        self.assertTrue(controls.props['Panic'])
        self.assertEqual(seen[-1]['MicMuted'], True)

        def explode(_props):
            raise RuntimeError('property callback')

        controls.on_properties = explode
        controls._on_signal(_signal(
            'PropertiesChanged', 'sa{sv}as', (plugin.IFACE, {'MicFilters': True}, []),
            plugin.PROPS_IFACE, sender=':1.42', path=plugin.OBJECT))
        self.assertTrue(controls.props['MicFilters'])
        controls._on_signal(_signal(
            'PropertiesChanged', 'sa{sv}as', (plugin.IFACE, {'MicMuted': False}, []),
            plugin.PROPS_IFACE, sender=':1.7', path=plugin.OBJECT))
        self.assertTrue(controls.props['MicMuted'])
        await controls.close()

    async def test_master_cli_actions(self):
        controls, _, runner = self.controls(connect=False)
        await controls.snapshot()
        await controls.toggle('stream')
        await controls.toggle('phones')
        self.assertIn(['/test/rostrum', '--no-start', '--action', 'mute_stream'], runner.commands)
        self.assertIn(['/test/rostrum', '--no-start', '--action', 'mute_headphones'], runner.commands)
        self.assertTrue(all(command[:2] == ['/test/rostrum', '--no-start'] for command in runner.commands))
        await controls.close()

    async def test_timeout_does_not_toggle_twice(self):
        controls, bus, runner = self.controls()
        bus.failures['ToggleBusMuted'] = TimeoutError
        await controls.snapshot()
        with self.assertRaises(plugin.Unavailable):
            await controls.toggle('mic')
        self.assertEqual(runner.commands, [])
        self.assertFalse(controls.cli_only)
        await controls.close()

    async def test_bus_connect_fail_uses_cli(self):
        commands = []

        def run(command, **kwargs):
            commands.append(list(command))
            return subprocess.CompletedProcess(command, 2, '', '')

        controls = plugin.Controls(run=run, executable='/test/rostrum', bus_factory=refuse_bus)
        with self.assertRaises(plugin.Unavailable):
            await controls.snapshot()
        self.assertTrue(controls.cli_only)
        self.assertEqual(commands[-1], ['/test/rostrum', '--no-start', '--list-buses'])
        self.assertTrue(all(command[0] != 'gdbus' for command in commands))
        await controls.close()

    async def test_press_and_release_have_no_cli_fallback(self):
        controls, _, runner = self.controls(connect=False)
        with self.assertRaises(plugin.Unavailable):
            await controls.press('push_to_talk')
        with self.assertRaises(plugin.Unavailable):
            await controls.release('push_to_talk')
        self.assertEqual(runner.commands, [])
        await controls.close()

    async def test_interface_error_is_value_error(self):
        controls, bus, runner = self.controls()
        bus.failures['SwitchScene'] = plugin.DBusError(plugin.IFACE + '.Error.NoSuchScene', 'gone')
        await controls.snapshot()
        with self.assertRaises(ValueError):
            await controls.scene('Gone')
        self.assertEqual(runner.commands, [])
        self.assertFalse(controls.cli_only)
        await controls.close()


class MicFeedbackTest(unittest.TestCase):
    class Backend:
        def __init__(self):
            self.muted = False
            self.absent = False
            self.fail_toggle = False
            self.toggles = 0
        def snapshot(self):
            if self.absent:
                raise plugin.Unavailable()
            return {'mic': {'id': 'mic', 'name': 'Mic', 'muted': self.muted}}, []
        def toggle(self, bus):
            if self.fail_toggle:
                raise plugin.Unavailable()
            self.toggles += 1
            self.muted = not self.muted

    class Feedback:
        def __init__(self): self.states, self.volumes = [], []
        def play(self, muted, volume=50):
            self.states.append(muted); self.volumes.append(volume)
        def reap(self): pass
        def close(self): pass

    def setUp(self):
        self.backend = self.Backend()
        self.app = plugin.Plugin(self.backend, lambda event: None)
        self.feedback = self.Feedback()
        self.app.feedback = self.feedback
        self.app.handle({'event': 'willAppear', 'context': 'mic-key',
                         'action': plugin.PREFIX + 'mic', 'payload': {'settings': {}}})

    def event(self, kind): self.app.handle({'event': kind, 'context': 'mic-key'})

    def test_feedback_follows_confirmed_mute_and_unmute(self):
        self.event('keyDown')
        self.assertEqual(self.feedback.states, [], 'wait for the confirmed state')
        self.event('keyUp')
        self.assertEqual(self.feedback.states, [True])
        self.event('keyDown'); self.event('keyUp')
        self.assertEqual(self.feedback.states, [True, False])
        self.app.poll(force=True); self.event('keyUp')
        self.assertEqual(self.feedback.states, [True, False], 'polling never repeats a cue')

    def test_feedback_uses_actual_state_instead_of_assuming_toggle(self):
        self.event('keyDown')
        self.backend.muted = False  # Another control changed it before confirmation.
        self.event('keyUp')
        self.assertEqual(self.feedback.states, [False])

    def test_per_key_volume_is_used_on_both_cues(self):
        self.app.handle({'event': 'didReceiveSettings', 'context': 'mic-key',
                         'payload': {'settings': {'soundVolume': 25}}})
        self.event('keyDown'); self.event('keyUp')
        self.event('keyDown'); self.event('keyUp')
        self.assertEqual(self.feedback.states, [True, False])
        self.assertEqual(self.feedback.volumes, [25, 25])

    def test_disabled_feedback_still_toggles(self):
        self.app.handle({'event': 'didReceiveSettings', 'context': 'mic-key',
                         'payload': {'settings': {'playSound': False}}})
        self.event('keyDown'); self.event('keyUp')
        self.assertTrue(self.backend.muted)
        self.assertEqual(self.feedback.states, [])

    def test_zero_volume_does_not_block_mic_control(self):
        self.app.handle({'event': 'didReceiveSettings', 'context': 'mic-key',
                         'payload': {'settings': {'soundVolume': 0}}})
        self.event('keyDown'); self.event('keyUp')
        self.assertTrue(self.backend.muted)
        self.assertEqual(self.feedback.volumes, [0])

    def test_unreachable_after_toggle_discards_feedback(self):
        self.event('keyDown'); self.backend.absent = True
        self.event('keyUp')
        self.backend.absent = False
        self.app.poll(force=True)
        self.assertEqual(self.feedback.states, [])

    def test_failed_toggle_has_no_feedback(self):
        self.backend.fail_toggle = True
        self.event('keyDown'); self.event('keyUp')
        self.assertEqual(self.feedback.states, [])

    def test_disappearing_key_discards_feedback(self):
        self.event('keyDown'); self.event('willDisappear')
        self.app.poll(force=True)
        self.assertEqual(self.feedback.states, [])


class FeedbackPlaybackTest(unittest.TestCase):
    def test_player_is_pinned_to_headphones(self):
        feedback = plugin.Feedback()
        process = Mock(); process.poll.return_value = None
        with patch.object(plugin.shutil, 'which', return_value='/usr/bin/pw-play'), \
             patch.object(plugin.subprocess, 'Popen', return_value=process) as spawn:
            feedback.play(True)
            args = spawn.call_args.args[0]
            self.assertEqual(args[args.index('--target') + 1], 'rostrum.phones')
            props = json.loads(args[args.index('--properties') + 1])
            self.assertTrue(props['node.dont-fallback'])
            self.assertTrue(props['node.dont-reconnect'])
            self.assertTrue(props['node.dont-move'])
            self.assertEqual(props['application.name'], 'Rostrum')
            self.assertEqual(Path(args[-1]), ROOT.resolve() / 'sounds/mic-muted.wav')
            feedback.play(False)
            self.assertTrue(process.kill.called, 'replace, do not overlap the previous cue')
            self.assertEqual(Path(spawn.call_args.args[0][-1]), ROOT.resolve() / 'sounds/mic-live.wav')
            feedback.started -= 3
            feedback.reap()
            self.assertIsNone(feedback.process)

    def test_volume_curve_bounds_and_invalid_saved_values(self):
        feedback = plugin.Feedback()
        for value, gain in [(25, 0.0625), (50, 0.25), (100, 1.0), (150, 1.0),
                            (None, 0.25), ('loud', 0.25), (True, 0.25), (float('nan'), 0.25)]:
            with self.subTest(value=value), \
                 patch.object(plugin.shutil, 'which', return_value='/usr/bin/pw-play'), \
                 patch.object(plugin.subprocess, 'Popen') as spawn:
                feedback.play(True, value)
                command = spawn.call_args.args[0]
                self.assertAlmostEqual(float(command[command.index('--volume') + 1]), gain)
                feedback.close()
        for value in (0, -10):
            with patch.object(plugin.subprocess, 'Popen') as spawn:
                feedback.play(False, value)
                spawn.assert_not_called()

    def test_missing_or_failed_player_is_harmless(self):
        feedback = plugin.Feedback()
        with patch.object(plugin.shutil, 'which', return_value=None):
            feedback.play(True)
        self.assertIsNone(feedback.process)
        with patch.object(plugin.shutil, 'which', return_value='/usr/bin/pw-play'), \
             patch.object(plugin.subprocess, 'Popen', side_effect=OSError('player unavailable')):
            feedback.play(False)
        feedback.reap(); feedback.close()
        self.assertIsNone(feedback.process)

    def test_bundled_cues_are_short_soft_and_distinct(self):
        pitches = []
        for name in ('mic-muted.wav', 'mic-live.wav'):
            with wave.open(str(ROOT / 'sounds' / name), 'rb') as audio:
                self.assertEqual((audio.getnchannels(), audio.getsampwidth()), (1, 2))
                rate = audio.getframerate()
                frames = audio.readframes(audio.getnframes())
            samples = struct.unpack('<' + 'h' * (len(frames) // 2), frames)
            self.assertAlmostEqual(len(samples) / rate, 0.2, places=3)
            self.assertLess(max(map(abs, samples)), 16000)
            # At the default 50%, playback gain is 0.25, preserving the original quiet peak.
            self.assertGreater(max(map(abs, samples)) * 0.25 / 32767, 0.09)
            self.assertLess(max(map(abs, samples)) * 0.25 / 32767, 0.12)
            self.assertEqual(samples[0], 0); self.assertEqual(samples[-1], 0)
            # Count crossings in the middle of each note, away from the fades.
            frequencies = []
            for start in (0.02, 0.13):
                segment = samples[int(start * rate):int((start + 0.04) * rate)]
                frequencies.append(sum(a <= 0 < b for a, b in zip(segment, segment[1:])) / 0.04)
            pitches.append(frequencies)
        self.assertGreater(pitches[0][0], pitches[0][1])
        self.assertLess(pitches[1][0], pitches[1][1])


class SocketTest(unittest.IsolatedAsyncioTestCase):
    async def test_launch_registration_and_unavailable_key(self):
        completed = asyncio.get_running_loop().create_future()

        async def read_event(reader):
            flags, size = await reader.readexactly(2)
            self.assertTrue(size & 128, 'client frames must be masked')
            size &= 127
            if size == 126:
                size = struct.unpack('!H', await reader.readexactly(2))[0]
            elif size == 127:
                size = struct.unpack('!Q', await reader.readexactly(8))[0]
            mask = await reader.readexactly(4)
            data = await reader.readexactly(size)
            decoded = bytes(value ^ mask[i % 4] for i, value in enumerate(data))
            return flags & 15, decoded

        async def host(reader, writer):
            try:
                header = (await reader.readuntil(b'\r\n\r\n')).decode()
                key = next(line.split(':', 1)[1].strip() for line in header.splitlines()
                           if line.startswith('Sec-WebSocket-Key:'))
                accept = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
                writer.write(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n'
                              'Connection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + '\r\n\r\n').encode())
                await writer.drain()
                opcode, data = await read_event(reader)
                self.assertEqual(opcode, 1)
                self.assertEqual(json.loads(data), {'event': 'registerPlugin', 'uuid': plugin.SERVICE})
                payload = json.dumps({'event': 'willAppear', 'action': plugin.PREFIX + 'mic',
                                      'context': 'socket-key', 'payload': {'settings': {}}}).encode()
                # Fragment the message and interleave a ping, as permitted by RFC 6455.
                writer.write(bytes([1, 10]) + payload[:10] + b'\x89\x01x' +
                             bytes([0x80, len(payload) - 10]) + payload[10:])
                await writer.drain()
                title = None
                pong = False
                while title is None:
                    opcode, data = await read_event(reader)
                    if opcode == 10:
                        pong = data == b'x'
                    elif opcode == 1:
                        event = json.loads(data)
                        if event['event'] == 'setTitle':
                            title = event['payload']['title']
                self.assertTrue(pong)
                self.assertEqual(title, 'Unavailable')
                writer.write(b'\x88\x00')
                await writer.drain()
                completed.set_result(None)
            except BaseException as error:
                completed.set_exception(error)
            finally:
                writer.close()
                await writer.wait_closed()

        server = await asyncio.start_server(host, '127.0.0.1', 0)
        async with server:
            port = server.sockets[0].getsockname()[1]
            env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS='unix:path=/nonexistent-rostrum-test-bus')
            env.pop('FLATPAK_ID', None)
            process = await asyncio.create_subprocess_exec(str(ROOT / 'plugin.sh'),
                '-port', str(port), '-pluginUUID', plugin.SERVICE, '-registerEvent', 'registerPlugin',
                '-info', '{}', env=env, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
            try:
                await asyncio.wait_for(completed, 10)
                _, stderr = await asyncio.wait_for(process.communicate(), 5)
                self.assertEqual(process.returncode, 2, stderr.decode())
            finally:
                if process.returncode is None:
                    process.kill()
                    await process.wait()


# Captured from the D-Bus little-endian layout (header field codes 1–8, 8-byte body align).
_PRESS_ACTION = bytes.fromhex(
    '6c01020111000000070000007700000001016f001f0000002f6465762f676574726f737472756d2f'
    '526f737472756d2f436f6e74726f6c0002017300170000006465762e676574726f737472756d2e52'
    '6f737472756d3100030173000b0000005072657373416374696f6e00000000000601730005000000'
    '3a312e343200000008016700017300000c000000707573685f746f5f74616c6b00')
_LIST_BUSES_BODY = bytes.fromhex(
    '1c00000000000000030000006d696300030000004d696300000000000000f03f00000000')


class DbusCodecTest(unittest.TestCase):
    def test_round_trip_string_call(self):
        body = plugin_dbus.encode_body('s', 'push_to_talk')
        raw = plugin_dbus.encode_message(
            'l', 1, 0x02, 7,
            {'path': '/dev/getrostrum/Rostrum/Control',
             'interface': 'dev.getrostrum.Rostrum1',
             'member': 'PressAction',
             'destination': ':1.42',
             'signature': 's'},
            body)
        n, msg = plugin_dbus.decode_message(raw)
        self.assertEqual(n, len(raw))
        self.assertEqual(msg.member, 'PressAction')
        self.assertEqual(plugin_dbus.decode_body(msg.signature, msg.body), ('push_to_talk',))

    def test_rejects_oversized(self):
        with self.assertRaises(ValueError):
            plugin_dbus.decode_message(b'l' + b'\x00' * (plugin_dbus.MAX_MESSAGE + 8))

    def test_properties_changed_signal_shape(self):
        # sa{sv}as with one boolean entry MicMuted=true
        body = plugin_dbus.encode_body(
            'sa{sv}as', 'dev.getrostrum.Rostrum1', {'MicMuted': True}, [])
        n, msg = plugin_dbus.decode_message(plugin_dbus.encode_message(
            'l', 4, 0, 1,
            {'path': '/dev/getrostrum/Rostrum/Control',
             'interface': 'org.freedesktop.DBus.Properties',
             'member': 'PropertiesChanged',
             'signature': 'sa{sv}as'},
            body))
        iface, changed, invalidated = plugin_dbus.decode_body(msg.signature, msg.body)
        self.assertEqual(iface, 'dev.getrostrum.Rostrum1')
        self.assertEqual(changed['MicMuted'], True)
        self.assertEqual(invalidated, [])
        self.assertEqual(body, bytes.fromhex(
            '170000006465762e676574726f737472756d2e526f737472756d31001400000008000000'
            '4d69634d75746564000162000100000000000000'))

    def test_press_action_matches_wire_layout(self):
        body = plugin_dbus.encode_body('s', 'push_to_talk')
        raw = plugin_dbus.encode_message(
            'l', 1, 0x02, 7,
            {'path': '/dev/getrostrum/Rostrum/Control',
             'interface': 'dev.getrostrum.Rostrum1',
             'member': 'PressAction',
             'destination': ':1.42',
             'signature': 's'},
            body)
        self.assertEqual(raw, _PRESS_ACTION)
        n, msg = plugin_dbus.decode_message(_PRESS_ACTION)
        self.assertEqual(n, len(_PRESS_ACTION))
        self.assertEqual(msg.type, 1)
        self.assertEqual(msg.flags, 0x02)
        self.assertEqual(msg.serial, 7)
        self.assertEqual(msg.path, '/dev/getrostrum/Rostrum/Control')
        self.assertEqual(msg.interface, 'dev.getrostrum.Rostrum1')
        self.assertEqual(msg.destination, ':1.42')
        self.assertEqual(msg.signature, 's')
        self.assertIsNone(msg.reply_serial)
        self.assertEqual(plugin_dbus.decode_body(msg.signature, msg.body), ('push_to_talk',))

    def test_round_trip_call_and_reply_signatures(self):
        arguments = plugin_dbus.encode_body('ss', 'org.freedesktop.DBus.Properties', 'MicMuted')
        self.assertEqual(plugin_dbus.decode_body('ss', arguments),
                         ('org.freedesktop.DBus.Properties', 'MicMuted'))
        buses = [('mic', 'Mic', 1.0, False)]
        bus_body = plugin_dbus.encode_body('a(ssdb)', buses)
        self.assertEqual(bus_body, _LIST_BUSES_BODY)
        self.assertEqual(plugin_dbus.decode_body('a(ssdb)', bus_body), (buses,))
        scenes = plugin_dbus.encode_body('as', ['Live', 'Just Chatting'])
        self.assertEqual(plugin_dbus.decode_body('as', scenes), (['Live', 'Just Chatting'],))
        props = plugin_dbus.encode_body('a{sv}', {'MicMuted': False, 'CurrentScene': 'Live'})
        decoded, = plugin_dbus.decode_body('a{sv}', props)
        self.assertEqual(decoded, {'MicMuted': False, 'CurrentScene': 'Live'})
        self.assertIs(decoded['MicMuted'], False)

    def test_name_owner_changed_and_reply_serial(self):
        body = plugin_dbus.encode_body('sss', 'dev.getrostrum.Rostrum', '', ':1.42')
        raw = plugin_dbus.encode_message(
            'l', 4, 0, 4,
            {'path': '/org/freedesktop/DBus',
             'interface': 'org.freedesktop.DBus',
             'member': 'NameOwnerChanged',
             'signature': 'sss'},
            body)
        n, msg = plugin_dbus.decode_message(raw)
        self.assertEqual(n, len(raw))
        self.assertEqual(plugin_dbus.decode_body(msg.signature, msg.body),
                         ('dev.getrostrum.Rostrum', '', ':1.42'))
        reply = plugin_dbus.encode_message(
            'l', 2, 0, 9,
            {'reply_serial': 7, 'sender': ':1.42', 'signature': 's'},
            plugin_dbus.encode_body('s', 'ok'))
        n, msg = plugin_dbus.decode_message(reply + b'trailing')
        self.assertEqual(n, len(reply))
        self.assertEqual(msg.reply_serial, 7)
        self.assertEqual(msg.sender, ':1.42')
        self.assertEqual(msg.error_name, None)
        self.assertEqual(plugin_dbus.decode_body(msg.signature, msg.body), ('ok',))

    def test_rejects_declared_oversize_and_incomplete(self):
        declared = struct.pack('<cBBBIII', b'l', 1, 0, 1, plugin_dbus.MAX_MESSAGE + 1, 1, 0)
        with self.assertRaises(ValueError):
            plugin_dbus.decode_message(declared)
        n, msg = plugin_dbus.decode_message(b'l')
        self.assertEqual(n, 0)
        self.assertIsNone(msg)

    def test_rejects_fds_and_unknown_signatures(self):
        with self.assertRaises(ValueError):
            plugin_dbus.encode_body('h', 0)
        with self.assertRaises(ValueError):
            plugin_dbus.encode_body('i', 1)
        with self.assertRaises(ValueError):
            plugin_dbus.decode_body('v', b'')
        with self.assertRaises(ValueError):
            plugin_dbus.encode_body('a{sv}', {'MicMuted': 1})
        with self.assertRaises(ValueError):
            plugin_dbus.encode_message(
                'l', 1, 0, 1,
                {'path': '/dev/getrostrum/Rostrum/Control', 'member': 'Nope', 'signature': 'h'},
                b'')
        raw = bytearray(_PRESS_ACTION)
        raw[3] = 0
        with self.assertRaises(ValueError):
            plugin_dbus.decode_message(bytes(raw))


_SASL_GUID = '00112233445566778899aabbccddeeff'


class SessionBusTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self._dir = tempfile.TemporaryDirectory()
        self._sock_path = str(Path(self._dir.name) / 'bus')
        self._server = None
        self.calls = []

    async def asyncTearDown(self):
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()
        self._dir.cleanup()

    async def _serve(self, handler):
        self._server = await asyncio.start_unix_server(handler, path=self._sock_path)
        return self._server

    async def _sasl(self, reader, writer):
        """AUTH EXTERNAL, then DATA (empty payload accepted), then OK, then BEGIN."""
        seen_auth = False
        while True:
            line = await reader.readuntil(b'\r\n')
            if line.startswith(b'\0'):
                line = line[1:]
            command, _, arg = line[:-2].partition(b' ')
            if command == b'AUTH':
                mechanism, _, initial = arg.partition(b' ')
                self.assertEqual(mechanism, b'EXTERNAL')
                self.assertEqual(initial, str(os.getuid()).encode('ascii').hex().encode('ascii'))
                seen_auth = True
                writer.write(b'DATA\r\n')
                await writer.drain()
            elif command == b'DATA':
                writer.write(f'OK {_SASL_GUID}\r\n'.encode('ascii'))
                await writer.drain()
            elif command == b'BEGIN':
                self.assertTrue(seen_auth)
                return
            else:
                writer.write(b'ERROR\r\n')
                await writer.drain()

    async def _read_message(self, reader):
        header = await reader.readexactly(16)
        body_len, _serial, fields_len = struct.unpack_from('<III', header, 4)
        header_end = 16 + fields_len
        total = ((header_end + 7) & ~7) + body_len
        rest = await reader.readexactly(total - 16)
        consumed, message = plugin_dbus.decode_message(header + rest)
        self.assertEqual(consumed, total)
        self.assertIsNotNone(message)
        return message

    async def _finish(self, reader, writer):
        try:
            await reader.read()
        except (asyncio.IncompleteReadError, ConnectionError, BrokenPipeError):
            pass
        writer.close()
        try:
            await writer.wait_closed()
        except (ConnectionError, BrokenPipeError):
            pass

    async def _reply_peer(self, reader, writer):
        try:
            await self._sasl(reader, writer)
            message = await self._read_message(reader)
            self.calls.append(message)
            signal = plugin_dbus.encode_message(
                'l', 4, 0, 2,
                {'path': '/org/freedesktop/DBus',
                 'interface': 'org.freedesktop.DBus',
                 'member': 'NameOwnerChanged',
                 'signature': 'sss'},
                plugin_dbus.encode_body('sss', 'dev.getrostrum.Rostrum', '', ':1.1'))
            wrong = plugin_dbus.encode_message(
                'l', 2, 0, 3,
                {'reply_serial': message.serial + 50, 'signature': 's', 'sender': ':1.1'},
                plugin_dbus.encode_body('s', 'nope'))
            reply = plugin_dbus.encode_message(
                'l', 2, 0, 4,
                {'reply_serial': message.serial, 'signature': 's', 'sender': ':1.1'},
                plugin_dbus.encode_body('s', 'ok'))
            writer.write(signal + wrong + reply)
            await writer.drain()
            await self._finish(reader, writer)
        except (asyncio.IncompleteReadError, ConnectionError, BrokenPipeError):
            writer.close()

    async def _silent_peer(self, reader, writer):
        try:
            await self._sasl(reader, writer)
            await self._read_message(reader)
            await self._finish(reader, writer)
        except (asyncio.IncompleteReadError, ConnectionError, BrokenPipeError):
            writer.close()

    async def _error_peer(self, reader, writer):
        try:
            await self._sasl(reader, writer)
            message = await self._read_message(reader)
            error = plugin_dbus.encode_message(
                'l', 3, 0, 5,
                {'reply_serial': message.serial,
                 'error_name': 'dev.getrostrum.Rostrum1.Error.Missing',
                 'signature': 's'},
                plugin_dbus.encode_body('s', 'no such bus'))
            writer.write(error)
            await writer.drain()
            await self._finish(reader, writer)
        except (asyncio.IncompleteReadError, ConnectionError, BrokenPipeError):
            writer.close()

    async def _oversize_peer(self, reader, writer):
        try:
            await self._sasl(reader, writer)
            await self._read_message(reader)
            header = struct.pack(
                '<cBBBIII', b'l', 2, 0, 1, plugin_dbus.MAX_MESSAGE + 1, 9, 0)
            writer.write(header)
            await writer.drain()
            await self._finish(reader, writer)
        except (asyncio.IncompleteReadError, ConnectionError, BrokenPipeError):
            writer.close()

    async def test_reply_matched_while_signal_arrives(self):
        signals = []
        server = await self._serve(self._reply_peer)
        async with server:
            bus = await plugin_dbus.SessionBus.connect(
                address=f'unix:path={self._sock_path}')
            bus.on_signal = signals.append
            body = await bus.call(':1.1', '/x', 'x.Y', 'Ping', 's', 'hi')
            self.assertEqual(body, ('ok',))
            self.assertEqual(len(signals), 1)
            self.assertEqual(signals[0].member, 'NameOwnerChanged')
            self.assertEqual(len(self.calls), 1)
            self.assertEqual(self.calls[0].flags, 0x02)
            self.assertEqual(self.calls[0].destination, ':1.1')
            self.assertEqual(self.calls[0].path, '/x')
            self.assertEqual(self.calls[0].interface, 'x.Y')
            self.assertEqual(self.calls[0].member, 'Ping')
            self.assertEqual(plugin_dbus.decode_body(self.calls[0].signature, self.calls[0].body),
                             ('hi',))
            await bus.close()

    async def test_call_timeout(self):
        server = await self._serve(self._silent_peer)
        async with server:
            bus = await plugin_dbus.SessionBus.connect(
                address=f'unix:path={self._sock_path}')
            with self.assertRaises(asyncio.TimeoutError):
                await bus.call(':1.1', '/x', 'x.Y', 'Ping', '', timeout=0.05)
            await bus.close()

    async def test_error_reply_raises_dbus_error(self):
        server = await self._serve(self._error_peer)
        async with server:
            bus = await plugin_dbus.SessionBus.connect(
                address=f'unix:path={self._sock_path}')
            with self.assertRaises(plugin_dbus.DBusError) as caught:
                await bus.call(':1.1', '/x', 'x.Y', 'Ping', '')
            self.assertEqual(caught.exception.name, 'dev.getrostrum.Rostrum1.Error.Missing')
            self.assertEqual(caught.exception.message, 'no such bus')
            await bus.close()

    async def test_connects_using_session_bus_address(self):
        server = await self._serve(self._silent_peer)
        address = f'unix:path={self._sock_path},guid={_SASL_GUID}'
        async with server:
            with patch.dict(os.environ, {'DBUS_SESSION_BUS_ADDRESS': address}):
                bus = await plugin_dbus.SessionBus.connect()
            await bus.close()

    async def test_connect_requires_session_address(self):
        with patch.dict(os.environ, {}, clear=True):
            with self.assertRaises(ValueError):
                await plugin_dbus.SessionBus.connect()

    async def _drop_peer(self, reader, writer):
        try:
            await self._sasl(reader, writer)
            await self._read_message(reader)
        except (asyncio.IncompleteReadError, ConnectionError, BrokenPipeError):
            pass
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except (ConnectionError, BrokenPipeError):
                pass

    async def test_dropped_peer_raises_connection_error(self):
        server = await self._serve(self._drop_peer)
        async with server:
            bus = await plugin_dbus.SessionBus.connect(
                address=f'unix:path={self._sock_path}')
            try:
                with self.assertRaises(ConnectionError):
                    await bus.call(':1.1', '/x', 'x.Y', 'Ping', '', timeout=0.5)
            finally:
                await bus.close()

    async def test_reader_rejects_declared_oversize(self):
        server = await self._serve(self._oversize_peer)
        async with server:
            bus = await plugin_dbus.SessionBus.connect(
                address=f'unix:path={self._sock_path}')
            with self.assertRaises(ValueError):
                await bus.call(':1.1', '/x', 'x.Y', 'Ping', '', timeout=0.5)
            await bus.close()


if __name__ == '__main__':
    unittest.main()
