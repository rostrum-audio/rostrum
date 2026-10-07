"""No deck or running mixer required: python3 -B tools/opendeck/test_plugin.py."""
import asyncio
import base64
import hashlib
import importlib.util
import os
import struct
import json
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch, Mock
import wave

ROOT = Path(__file__).parent / 'dev.getrostrum.Rostrum.sdPlugin'
spec = importlib.util.spec_from_file_location('plugin', ROOT / 'plugin.py')
plugin = importlib.util.module_from_spec(spec)
spec.loader.exec_module(plugin)


class Runner:
    def __init__(self, absent=False, fallback=False):
        self.commands = []
        self.absent = absent
        self.fallback = fallback

    def __call__(self, command, **kwargs):
        self.commands.append(command)
        method = command[command.index('--method') + 1] if '--method' in command else ''
        output = '()'
        status = 0
        if method.endswith('GetNameOwner'):
            status = 1 if self.absent else 0
            output = "(':1.42',)"
        elif self.fallback and method:
            status = 1
        elif method.endswith('ListBuses'):
            output = "([('mic', 'Mic', 1.0, false), ('game', 'Game', 1.0, true)],)"
        elif method.endswith('ListScenes'):
            output = "(['Live', 'Just Chatting'],)"
        elif '--list-buses' in command:
            output = 'mic\t1.00\tunmuted\tMic\ngame\t1.00\tmuted\tGame\n'
        elif '--list-scenes' in command:
            output = 'Live\nJust Chatting\n'
        return subprocess.CompletedProcess(command, status, output, 'unreachable' if status else '')


class PluginTest(unittest.TestCase):
    def setup_plugin(self, **kwargs):
        runner = Runner(**kwargs)
        backend = plugin.Controls(run=runner, executable='/test/rostrum')
        events = []
        app = plugin.Plugin(backend, events.append)
        return app, runner, events

    def appear(self, app, action='mic', settings=None):
        app.handle({'event': 'willAppear', 'context': 'key',
                    'action': plugin.PREFIX + action, 'payload': {'settings': settings or {}}})

    def down(self, app):
        app.handle({'event': 'keyDown', 'context': 'key'})

    def test_absent_does_not_launch(self):
        app, runner, events = self.setup_plugin(absent=True)
        self.appear(app)
        self.down(app)
        self.assertEqual(events[-1]['payload']['title'], 'Unavailable')
        self.assertFalse(any(command[0] == '/test/rostrum' for command in runner.commands))

    def test_mic_command_and_state(self):
        app, runner, events = self.setup_plugin()
        self.appear(app)
        self.down(app)
        self.assertTrue(any(command[-3:] == ['--method', plugin.IFACE + '.ToggleBusMuted', json.dumps('mic')]
                            for command in runner.commands))
        self.assertTrue(any(event['payload'].get('title') == 'Mic live' for event in events))

    def test_named_scene(self):
        app, runner, _ = self.setup_plugin()
        self.appear(app, 'scene', {'scene': 'Just Chatting'})
        self.down(app)
        self.assertTrue(any(command[-2:] == [plugin.IFACE + '.SwitchScene', json.dumps('Just Chatting')]
                            for command in runner.commands))

    def test_bus_id(self):
        app, runner, events = self.setup_plugin()
        self.appear(app, 'bus', {'bus': 'game'})
        self.down(app)
        self.assertTrue(any(command[-2:] == [plugin.IFACE + '.ToggleBusMuted', json.dumps('game')]
                            for command in runner.commands))
        self.assertTrue(any(event['payload'].get('title') == 'Game muted' for event in events))

    def test_panic(self):
        app, runner, _ = self.setup_plugin()
        self.appear(app, 'panic')
        self.down(app)
        self.assertTrue(any(command[-2:] == [plugin.IFACE + '.TriggerAction', json.dumps('panic_mute')]
                            for command in runner.commands))

    def test_key_up_releases_and_refreshes(self):
        app, runner, _ = self.setup_plugin()
        self.appear(app)
        # No hold action is exposed. Exercise cleanup if a future action records a press.
        app.held['key'] = 'push_to_talk'
        before = len(runner.commands)
        app.handle({'event': 'keyUp', 'context': 'key'})
        self.assertEqual(app.held, {})
        self.assertTrue(any(command[-2:] == [plugin.IFACE + '.ReleaseAction', json.dumps('push_to_talk')]
                            for command in runner.commands[before:]))
        self.assertTrue(any(command[-1] == plugin.IFACE + '.ListBuses'
                            for command in runner.commands[before:]))

    def test_cli_fallback_never_starts(self):
        app, runner, _ = self.setup_plugin(fallback=True)
        for action, settings, args in [('mic', {}, ['--toggle-mic']),
                                       ('scene', {'scene': 'Just Chatting'}, ['--scene', 'Just Chatting']),
                                       ('bus', {'bus': 'game'}, ['--action', 'mute_bus_game']),
                                       ('panic', {}, ['--action', 'panic_mute'])]:
            self.appear(app, action, settings)
            self.down(app)
            self.assertIn(['/test/rostrum', '--no-start'] + args, runner.commands)

    def test_missing_and_empty_choices(self):
        for action, settings in [('scene', {'scene': 'Gone'}), ('bus', {'bus': 'gone'})]:
            app, runner, events = self.setup_plugin()
            self.appear(app, action, settings)
            before = len(runner.commands)
            self.down(app)
            self.assertEqual(len(runner.commands), before)
            self.assertEqual(events[-1]['payload']['title'], 'Missing')
        app, runner, _ = self.setup_plugin()
        self.appear(app, 'scene')
        before = len(runner.commands)
        self.down(app)
        self.assertEqual(len(runner.commands), before)

    def test_poll_rate_and_profile_closed(self):
        app, runner, _ = self.setup_plugin()
        self.appear(app)
        before = len(runner.commands)
        for _ in range(10):
            app.poll()
        self.assertEqual(len(runner.commands), before)
        app.handle({'event': 'willDisappear', 'context': 'key'})
        app.poll(force=True)
        self.assertEqual(len(runner.commands), before)

    def test_master_cli_actions(self):
        runner = Runner(fallback=True)
        controls = plugin.Controls(run=runner, executable='/test/rostrum')
        controls.snapshot()
        controls.toggle('stream')
        controls.toggle('phones')
        self.assertIn(['/test/rostrum', '--no-start', '--action', 'mute_stream'], runner.commands)
        self.assertIn(['/test/rostrum', '--no-start', '--action', 'mute_headphones'], runner.commands)

    def test_timeout_does_not_toggle_twice(self):
        runner = Runner()
        def run(command, **kwargs):
            if command[-2:] == [plugin.IFACE + '.ToggleBusMuted', json.dumps('mic')]:
                raise subprocess.TimeoutExpired(command, 2)
            return runner(command, **kwargs)
        controls = plugin.Controls(run=run, executable='/test/rostrum')
        controls.snapshot()
        with self.assertRaises(plugin.Unavailable):
            controls.toggle('mic')
        self.assertFalse(any(command[0] == '/test/rostrum' for command in runner.commands))

    def test_missing_gdbus_and_unreachable_cli(self):
        commands = []
        def run(command, **kwargs):
            commands.append(command)
            if command[0] == 'gdbus':
                raise FileNotFoundError()
            return subprocess.CompletedProcess(command, 2, '', '')
        controls = plugin.Controls(run=run, executable='/test/rostrum')
        with self.assertRaises(plugin.Unavailable):
            controls.snapshot()
        self.assertEqual(commands[-1], ['/test/rostrum', '--no-start', '--list-buses'])

    def test_gvariant_strings_are_preserved(self):
        self.assertEqual(plugin.variant("(['true', 'false', \"Just Chatting\", '@as in a name'],)"),
                         (['true', 'false', 'Just Chatting', '@as in a name'],))
        self.assertEqual(plugin.variant('(@a(ssdb) [],)'), ([],))

    def test_disappeared_choice_in_cli_fallback(self):
        runner = Runner(fallback=True)
        def run(command, **kwargs):
            if '--scene' in command:
                return subprocess.CompletedProcess(command, 1, '', 'No such scene')
            return runner(command, **kwargs)
        controls = plugin.Controls(run=run, executable='/test/rostrum')
        events = []
        app = plugin.Plugin(controls, events.append)
        self.appear(app, 'scene', {'scene': 'Just Chatting'})
        self.down(app)
        self.assertEqual(events[-1]['payload']['title'], 'Missing')

    def test_manifest_assets(self):
        manifest = json.loads((ROOT / 'manifest.json').read_text())
        self.assertEqual(len(manifest['Actions']), 4)
        self.assertNotIn('OnlyShowIn', manifest)
        self.assertTrue((ROOT / manifest['CodePathLin']).stat().st_mode & 0o111)
        for action in manifest['Actions']:
            self.assertNotIn('OnlyShowIn', action)
            for state in action['States']:
                self.assertTrue((ROOT / (state['Image'] + '.svg')).is_file())


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
        def __init__(self): self.states = []
        def play(self, muted): self.states.append(muted)
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

    def test_disabled_feedback_still_toggles(self):
        self.app.handle({'event': 'didReceiveSettings', 'context': 'mic-key',
                         'payload': {'settings': {'playSound': False}}})
        self.event('keyDown'); self.event('keyUp')
        self.assertTrue(self.backend.muted)
        self.assertEqual(self.feedback.states, [])

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
            self.assertLess(max(map(abs, samples)), 5000)
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


if __name__ == '__main__':
    unittest.main()
