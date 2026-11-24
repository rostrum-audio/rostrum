"""No deck or running mixer required: python3 -B tools/opendeck/test_plugin.py."""
import asyncio
import base64
import hashlib
import importlib.util
import os
import struct
import json
import re
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
        self.list_buses = [('mic', 'Mic', 1.0, False), ('game', 'Game', 1.0, True)]
        # None answers ListBusLevels as Rostrum 0.5 does (UnknownMethod), so tests keep ListBuses.
        self.list_levels = None
        self.step_delay = 0
        self.in_flight = 0
        self.in_flight_max = 0
        self.closed = False
        # Rostrum emits PropertiesChanged inside the method's slot, so it reaches the plugin
        # before the reply. member -> props that change before replying.
        self.changes_before_reply = {}
        # A change sent just after the reply, read from the same socket buffer before the
        # caller resumes. member -> props that change after the reply is built.
        self.changes_after_reply = {}

    def _changed(self, changed):
        if changed and self.on_signal is not None:
            self.props.update(changed)
            self.on_signal(_signal('PropertiesChanged', 'sa{sv}as', (plugin.IFACE, dict(changed), []),
                                   plugin.PROPS_IFACE, sender=self.owner, path=plugin.OBJECT))

    async def call(self, destination, path, interface, member, signature='', *args, timeout=2.0):
        result = await self._reply(destination, path, interface, member, signature, *args)
        self._changed(self.changes_after_reply.get(member))
        return result

    async def _reply(self, destination, path, interface, member, signature='', *args):
        self.calls.append({'destination': destination, 'path': path, 'interface': interface,
                           'member': member, 'signature': signature, 'args': args})
        failure = self.failures.get(member)
        if failure is not None:
            raise failure() if isinstance(failure, type) else failure
        self._changed(self.changes_before_reply.get(member))
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
                # A unique name that left gets ServiceUnknown from the bus; anything else (the
                # well-known name) is a plugin bug.
                if isinstance(destination, str) and destination.startswith(':'):
                    raise plugin.DBusError('org.freedesktop.DBus.Error.ServiceUnknown', destination)
                raise AssertionError(destination)
        if member == 'GetAll':
            return (dict(self.props),)
        if member == 'ListBuses':
            return (list(self.list_buses),)
        if member == 'ListScenes':
            return (['Live', 'Just Chatting'],)
        if member == 'ListBusLevels':
            if self.list_levels is None:
                raise plugin.DBusError('org.freedesktop.DBus.Error.UnknownMethod', 'No such method')
            return (list(self.list_levels),)
        if member == 'StepBusVolume':
            return await self._step(*args)
        if member in ('ToggleBusMuted', 'ToggleBusInMix', 'TriggerAction', 'SwitchScene', 'PressAction',
                      'ReleaseAction'):
            return ()
        raise AssertionError(member)

    async def _step(self, bus_id, side, delta):
        # Rostrum's rules: one side alone, or both by the Stream level's actual change.
        self.in_flight += 1
        self.in_flight_max = max(self.in_flight_max, self.in_flight)
        try:
            if self.step_delay:
                await asyncio.sleep(self.step_delay)
            rows = self.list_levels or []
            for index, (row_id, name, stream, phones, muted, destination) in enumerate(rows):
                if row_id != bus_id:
                    continue
                top = 1.5 if row_id == 'mic' else 1.0
                if not destination:
                    stream = phones = new = min(top, max(0.0, stream + delta))
                elif side == 'phones':
                    phones = new = min(1.0, max(0.0, phones + delta))
                elif side == 'stream':
                    stream = new = min(1.0, max(0.0, stream + delta))
                else:
                    new = min(1.0, max(0.0, stream + delta))
                    phones = min(1.0, max(0.0, phones + (new - stream)))
                    stream = new
                rows[index] = (row_id, name, stream, phones, muted, destination)
                return (new,)
            raise plugin.DBusError(plugin.IFACE + '.Error.NoSuchBus', bus_id)
        finally:
            self.in_flight -= 1

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


class PluginTest(unittest.IsolatedAsyncioTestCase):
    def setup_plugin(self, *, owner=':1.42', connect=True, run=None):
        bus = FakeBus(owner=owner)
        runner = run or CliRunner()
        factory = fake_connect(bus) if connect else refuse_bus
        backend = plugin.Controls(run=runner, executable='/test/rostrum', bus_factory=factory)
        events = []
        app = plugin.Plugin(backend, events.append)
        return app, bus, runner, events

    async def appear(self, app, action='mic', settings=None, context='key'):
        await app.handle({'event': 'willAppear', 'context': context,
                          'action': plugin.PREFIX + action, 'payload': {'settings': settings or {}}})

    async def down(self, app, context='key'):
        await app.handle({'event': 'keyDown', 'context': context})

    def titles(self, events):
        return [event['payload']['title'] for event in events if event.get('event') == 'setTitle']

    async def test_absent_does_not_launch(self):
        app, bus, runner, events = self.setup_plugin(owner=None)
        await self.appear(app)
        await self.down(app)
        self.assertEqual(self.titles(events)[-1], 'Unavailable')
        self.assertEqual(runner.commands, [])
        self.assertTrue(any(call['member'] == 'GetNameOwner' and call['args'] == (plugin.SERVICE,)
                            for call in bus.calls))
        self.assertFalse(any(call['member'] in ('ListBuses', 'ToggleBusMuted') for call in bus.calls))

    async def test_mic_command_and_state(self):
        app, bus, runner, events = self.setup_plugin()
        await self.appear(app)
        await self.down(app)
        toggles = [call for call in bus.calls if call['member'] == 'ToggleBusMuted']
        self.assertEqual(toggles[-1]['args'], ('mic',))
        self.assertEqual(toggles[-1]['destination'], ':1.42')
        self.assertNotEqual(toggles[-1]['destination'], plugin.SERVICE)
        self.assertEqual(toggles[-1]['path'], plugin.OBJECT)
        self.assertTrue(any(event['payload'].get('title') == 'Mic live' for event in events))
        self.assertEqual(runner.commands, [])

    async def test_named_scene(self):
        app, bus, runner, _ = self.setup_plugin()
        await self.appear(app, 'scene', {'scene': 'Just Chatting'})
        await self.down(app)
        self.assertTrue(any(call['member'] == 'SwitchScene' and call['args'] == ('Just Chatting',)
                            and call['destination'] == ':1.42' for call in bus.calls))
        self.assertEqual(runner.commands, [])

    async def test_bus_id(self):
        app, bus, runner, events = self.setup_plugin()
        await self.appear(app, 'bus', {'bus': 'game'})
        await self.down(app)
        self.assertTrue(any(call['member'] == 'ToggleBusMuted' and call['args'] == ('game',)
                            and call['destination'] == ':1.42' for call in bus.calls))
        self.assertTrue(any(event['payload'].get('title') == 'Game muted' for event in events))
        self.assertEqual(runner.commands, [])

    async def test_panic(self):
        app, bus, runner, _ = self.setup_plugin()
        await self.appear(app, 'panic')
        await self.down(app)
        self.assertTrue(any(call['member'] == 'TriggerAction' and call['args'] == ('panic_mute',)
                            and call['destination'] == ':1.42' for call in bus.calls))
        self.assertEqual(runner.commands, [])

    async def test_key_up_releases_and_refreshes(self):
        app, bus, _, _ = self.setup_plugin()
        await self.appear(app)
        # No hold action is exposed. Exercise cleanup if a future action records a press.
        app.held['key'] = 'push_to_talk'
        before = len(bus.calls)
        await app.handle({'event': 'keyUp', 'context': 'key'})
        self.assertEqual(app.held, {})
        later = bus.calls[before:]
        self.assertTrue(any(call['member'] == 'ReleaseAction' and call['args'] == ('push_to_talk',)
                            and call['destination'] == ':1.42' for call in later))
        self.assertTrue(any(call['member'] == 'ListBuses' for call in later))

    async def test_cli_fallback_never_starts(self):
        app, _, runner, _ = self.setup_plugin(connect=False)
        for action, settings, args in [('mic', {}, ['--toggle-mic']),
                                       ('scene', {'scene': 'Just Chatting'}, ['--scene', 'Just Chatting']),
                                       ('bus', {'bus': 'game'}, ['--action', 'mute_bus_game']),
                                       ('panic', {}, ['--action', 'panic_mute'])]:
            await self.appear(app, action, settings)
            await self.down(app)
            self.assertIn(['/test/rostrum', '--no-start'] + args, runner.commands)
        self.assertTrue(app.controls.cli_only)
        self.assertTrue(all(command[:2] == ['/test/rostrum', '--no-start'] for command in runner.commands))

    async def test_missing_and_empty_choices(self):
        for action, settings in [('scene', {'scene': 'Gone'}), ('bus', {'bus': 'gone'})]:
            app, bus, _, events = self.setup_plugin()
            await self.appear(app, action, settings)
            before = len(bus.calls)
            await self.down(app)
            self.assertEqual(len(bus.calls), before)
            self.assertEqual(self.titles(events)[-1], 'Missing')
        app, bus, _, _ = self.setup_plugin()
        await self.appear(app, 'scene')
        before = len(bus.calls)
        await self.down(app)
        self.assertEqual(len(bus.calls), before)

    async def test_poll_rate_and_profile_closed(self):
        app, bus, _, _ = self.setup_plugin()
        await self.appear(app)
        before = len(bus.calls)
        for _ in range(10):
            await app.poll()
        self.assertEqual(len(bus.calls), before)
        await app.handle({'event': 'willDisappear', 'context': 'key'})
        await app.poll(force=True)
        self.assertEqual(len(bus.calls), before)

    async def test_disappeared_choice_in_cli_fallback(self):
        runner = CliRunner()

        def run(command, **kwargs):
            if '--scene' in command:
                runner.commands.append(list(command))
                return subprocess.CompletedProcess(command, 1, '', 'No such scene')
            return runner(command, **kwargs)

        app, _, _, events = self.setup_plugin(connect=False, run=run)
        await self.appear(app, 'scene', {'scene': 'Just Chatting'})
        await self.down(app)
        self.assertEqual(self.titles(events)[-1], 'Missing')
        self.assertTrue(any(command[:2] == ['/test/rostrum', '--no-start'] for command in runner.commands))

    async def test_render_skips_unchanged(self):
        app, _, _, events = self.setup_plugin()
        await app.handle({'event': 'willAppear', 'context': 'key',
                          'action': plugin.PREFIX + 'mic', 'payload': {'settings': {}}})
        n = len(events)
        await app.render('key')
        self.assertEqual(len(events), n)
        self.assertEqual(app.keys['key']['title'], 'Mic live')
        self.assertEqual(app.keys['key']['state'], 0)

    async def test_show_alert_when_bus_unset(self):
        app, bus, _, events = self.setup_plugin()
        await self.appear(app, 'bus', {})
        before = len(bus.calls)
        await self.down(app)
        self.assertTrue(any(event['event'] == 'showAlert' and event['context'] == 'key' for event in events))
        self.assertFalse(any(call['member'] == 'ToggleBusMuted' for call in bus.calls[before:]))
        self.assertFalse(any(event['event'] == 'showOk' for event in events))

    async def test_no_show_ok_on_mic(self):
        app, _, _, events = self.setup_plugin()
        await self.appear(app, 'mic')
        await self.down(app)
        self.assertTrue(any(event['payload'].get('title') == 'Mic live' for event in events))
        self.assertFalse(any(event['event'] == 'showOk' for event in events))

    async def test_name_owner_lost_marks_unavailable(self):
        app, _, _, events = self.setup_plugin()
        await self.appear(app)
        await app.controls._handle_owner('', '')
        self.assertEqual(events[-1]['payload']['title'], 'Unavailable')
        self.assertTrue(any(event['event'] == 'showAlert' for event in events))
        self.assertFalse(app.available)

    async def test_panic_state_from_props(self):
        app, bus, _, events = self.setup_plugin()
        bus.props['Panic'] = True
        await self.appear(app, 'panic')
        self.assertTrue(any(event['event'] == 'setState' and event['payload']['state'] == 1
                            for event in events))
        self.assertEqual(self.titles(events)[-1], 'Panic on')
        bus.props['Panic'] = False
        app.controls._on_signal(_signal(
            'PropertiesChanged', 'sa{sv}as', (plugin.IFACE, {'Panic': False}, []),
            plugin.PROPS_IFACE, sender=':1.42', path=plugin.OBJECT))
        self.assertEqual(self.titles(events)[-1], 'Panic mute')
        self.assertEqual(app.keys['key']['state'], 0)

    async def test_studio_voice_toggles_through_rostrum(self):
        # Rostrum flips the current value itself; a cached !MicFilters could be stale.
        app, bus, runner, events = self.setup_plugin()
        await self.appear(app, 'studioVoice')
        await self.down(app)
        calls = [(call['member'], call['args']) for call in bus.calls if call['interface'] == plugin.IFACE]
        self.assertIn(('TriggerAction', ('toggle_mic_filters',)), calls)
        self.assertFalse(any(member == 'SetMicFilters' for member, _ in calls))
        self.assertFalse(any(event['event'] in ('showAlert', 'showOk') for event in events))
        self.assertEqual(runner.commands, [])

    async def test_studio_voice_state_from_mic_filters(self):
        app, bus, _, events = self.setup_plugin()
        await self.appear(app, 'studioVoice')
        self.assertEqual(app.keys['key']['state'], 0)
        self.assertEqual(self.titles(events)[-1], 'Voice off')
        app.controls._on_signal(_signal(
            'PropertiesChanged', 'sa{sv}as', (plugin.IFACE, {'MicFilters': True}, []),
            plugin.PROPS_IFACE, sender=':1.42', path=plugin.OBJECT))
        self.assertEqual(app.keys['key']['state'], 1)
        self.assertEqual(self.titles(events)[-1], 'Voice on')

    async def test_studio_voice_cli_only_is_neutral_and_still_toggles(self):
        app, _, runner, events = self.setup_plugin(connect=False)
        await self.appear(app, 'studioVoice')
        self.assertEqual(self.titles(events)[-1], 'Studio Voice')
        self.assertEqual(app.keys['key']['state'], 0)
        await self.down(app)
        self.assertIn(['/test/rostrum', '--no-start', '--action', 'toggle_mic_filters'], runner.commands)

    async def test_push_to_talk_key_is_its_own_action(self):
        # Without its own branch it would render as an unset bus key.
        app, _, _, events = self.setup_plugin()
        await self.appear(app, 'pushToTalk')
        self.assertEqual(self.titles(events)[-1], 'Push to talk')
        self.assertEqual(app.keys['key']['state'], 0)

    def ptt_calls(self, bus, member):
        return sum(1 for call in bus.calls if call['member'] == member and call['args'] == ('push_to_talk',))

    def alerts(self, events):
        return sum(1 for event in events if event['event'] == 'showAlert')

    async def up(self, app, context='key'):
        await app.handle({'event': 'keyUp', 'context': context})

    async def test_ptt_holds_and_shows_talking(self):
        app, bus, _, events = self.setup_plugin()
        bus.props['MicMuted'] = True
        bus.changes_before_reply = {'PressAction': {'MicMuted': False}, 'ReleaseAction': {'MicMuted': True}}
        await self.appear(app, 'pushToTalk')
        await self.down(app)
        self.assertEqual(self.ptt_calls(bus, 'PressAction'), 1)
        self.assertEqual((app.keys['key']['state'], app.keys['key']['title']), (1, 'Talking'))
        self.assertEqual(self.alerts(events), 0)
        await self.up(app)
        self.assertEqual(self.ptt_calls(bus, 'ReleaseAction'), 1)
        self.assertEqual((app.keys['key']['state'], app.keys['key']['title']), (0, 'Push to talk'))
        self.assertFalse(any(call['member'] in ('ToggleBusMuted', 'TriggerAction') for call in bus.calls))

    async def test_ptt_released_when_key_disappears_mid_hold(self):
        # Switching deck pages while holding sends willDisappear, not keyUp.
        app, bus, _, _ = self.setup_plugin()
        await self.appear(app, 'pushToTalk')
        await self.down(app)
        await app.handle({'event': 'willDisappear', 'context': 'key'})
        self.assertEqual(self.ptt_calls(bus, 'ReleaseAction'), 1)
        self.assertEqual(app.held, {})

    async def test_key_that_appears_while_rostrum_is_absent_does_not_alert(self):
        # OpenDeck often starts before Rostrum, and page flips re-send willAppear.
        # Nothing was pressed, so no warning: the title already says Unavailable.
        app, _, _, events = self.setup_plugin(owner='')
        await self.appear(app, 'mic', context='a')
        await self.appear(app, 'pushToTalk', context='b')
        self.assertEqual(app.keys['a']['title'], 'Unavailable')
        self.assertEqual(app.keys['b']['title'], 'Unavailable')
        self.assertEqual(self.alerts(events), 0)
        await self.down(app, 'a')
        self.assertEqual(self.alerts(events), 1)  # a press still gets its alert

    async def test_owner_change_from_another_client_is_ignored(self):
        # Only the bus daemon announces owners. A unicast fake must not clear holds.
        app, bus, _, _ = self.setup_plugin()
        await self.appear(app, 'pushToTalk')
        await self.down(app)
        app.controls._on_signal(_signal('NameOwnerChanged', 'sss', (plugin.SERVICE, ':1.42', ''),
                                        plugin.BUS_IFACE, sender=':1.66', path=plugin.BUS_PATH))
        await asyncio.gather(*app.controls._tasks)
        self.assertEqual(app.controls.owner, ':1.42')
        self.assertEqual(app.held, {'key': 'push_to_talk'})

    async def test_ptt_press_that_timed_out_is_released_with_the_key(self):
        # A stalled Rostrum can still run the PressAction after the plugin stopped waiting.
        # The hold would then outlive the key, so keyUp releases it on the same connection.
        app, bus, _, _ = self.setup_plugin()
        await self.appear(app, 'pushToTalk')
        bus.failures['PressAction'] = TimeoutError
        await self.down(app)
        self.assertEqual(app.keys['key']['title'], 'Unavailable')
        del bus.failures['PressAction']
        await self.up(app)
        self.assertEqual(self.ptt_calls(bus, 'ReleaseAction'), 1)
        self.assertEqual(app.held, {})
        self.assertEqual(app.keys['key']['title'], 'Push to talk')

    async def test_ptt_refused_press_is_not_released(self):
        # Rostrum's own error means it ran nothing, so there is no hold to end.
        app, bus, _, _ = self.setup_plugin()
        await self.appear(app, 'pushToTalk')
        bus.failures['PressAction'] = plugin.DBusError(plugin.IFACE + '.Error.NoSuchAction', 'no')
        await self.down(app)
        await self.up(app)
        self.assertEqual(self.ptt_calls(bus, 'ReleaseAction'), 0)

    async def test_ptt_refcount_releases_once(self):
        app, bus, _, _ = self.setup_plugin()
        await self.appear(app, 'pushToTalk', context='a')
        await self.appear(app, 'pushToTalk', context='b')
        await self.down(app, 'a')
        await self.down(app, 'b')
        self.assertEqual(self.ptt_calls(bus, 'PressAction'), 1)
        await self.up(app, 'a')
        self.assertEqual(self.ptt_calls(bus, 'ReleaseAction'), 0)
        await self.up(app, 'b')
        self.assertEqual(self.ptt_calls(bus, 'ReleaseAction'), 1)

    async def test_ptt_blocked_when_held_but_muted(self):
        # Panic or push to mute wins over push to talk, so the mic stays muted while held.
        app, bus, _, events = self.setup_plugin()
        bus.props['MicMuted'] = True
        await self.appear(app, 'pushToTalk')
        await self.down(app)
        self.assertEqual((app.keys['key']['state'], app.keys['key']['title']), (0, 'Blocked'))
        self.assertEqual(self.alerts(events), 1)
        await app.poll(force=True)
        self.assertEqual(self.alerts(events), 1)  # once per entry into Blocked, not per redraw

    async def test_ptt_unavailable_without_bus(self):
        # Holds need the plugin's own connection; a toggle is never a substitute.
        app, _, runner, events = self.setup_plugin(connect=False)
        await self.appear(app, 'mic', context='mic')
        await self.appear(app, 'pushToTalk')
        self.assertEqual(app.keys['key']['title'], 'Unavailable')
        before = self.alerts(events)
        await self.down(app)
        self.assertEqual(self.alerts(events), before + 1)
        self.assertFalse(any('--toggle-mic' in command or '--action' in command for command in runner.commands))
        self.assertTrue(app.available)  # the CLI still serves the other keys
        self.assertEqual(app.keys['mic']['title'], 'Mic live')

    async def test_ptt_owner_change_clears_hold_without_repress(self):
        app, bus, _, _ = self.setup_plugin()
        bus.changes_before_reply = {'PressAction': {'MicMuted': False}}
        bus.props['MicMuted'] = True
        await self.appear(app, 'pushToTalk')
        await self.down(app)
        self.assertEqual(app.keys['key']['title'], 'Talking')
        bus.owner = ':1.77'  # Rostrum restarted; the new instance holds nothing
        await app.controls._handle_owner(':1.42', ':1.77')
        await asyncio.gather(*app._tasks)
        self.assertEqual(app.held, {})
        self.assertEqual((app.keys['key']['state'], app.keys['key']['title']), (0, 'Push to talk'))
        await self.up(app)
        self.assertEqual(self.ptt_calls(bus, 'PressAction'), 1)
        self.assertEqual(self.ptt_calls(bus, 'ReleaseAction'), 0)

    async def test_scene_active_when_current(self):
        app, _, _, events = self.setup_plugin()
        await self.appear(app, 'scene', {'scene': 'Live'})
        self.assertEqual(self.titles(events)[-1], 'Live')
        self.assertEqual(app.keys['key']['state'], 1)
        await self.appear(app, 'scene', {'scene': 'Just Chatting'}, context='other')
        self.assertEqual(app.keys['other']['title'], 'Just Chatting')
        self.assertEqual(app.keys['other']['state'], 0)
        await self.appear(app, 'scene', {'scene': 'Gone'}, context='missing')
        self.assertEqual(app.keys['missing']['title'], 'Missing')

    async def test_mic_title_prefers_props_over_buses(self):
        app, bus, _, events = self.setup_plugin()
        bus.props['MicMuted'] = True
        await self.appear(app, 'mic')
        self.assertEqual(self.titles(events)[-1], 'Mic muted')
        self.assertEqual(app.keys['key']['state'], 1)

    async def test_stream_bus_follows_stream_muted_signal(self):
        app, bus, _, events = self.setup_plugin()
        bus.list_buses = [('mic', 'Mic', 1.0, False), ('stream', 'Stream', 1.0, False)]
        await self.appear(app, 'bus', {'bus': 'stream'})
        self.assertEqual(self.titles(events)[-1], 'Stream live')
        self.assertEqual(app.keys['key']['state'], 0)
        before = len(bus.calls)
        app.controls._on_signal(_signal(
            'PropertiesChanged', 'sa{sv}as', (plugin.IFACE, {'StreamMuted': True}, []),
            plugin.PROPS_IFACE, sender=':1.42', path=plugin.OBJECT))
        self.assertNotIn('ListBuses', [call['member'] for call in bus.calls[before:]])
        self.assertEqual(self.titles(events)[-1], 'Stream muted')
        self.assertEqual(app.keys['key']['state'], 1)

    async def test_properties_signal_rerenders(self):
        app, _, _, events = self.setup_plugin()
        await self.appear(app, 'mic')
        n = len(events)
        app.controls._on_signal(_signal(
            'PropertiesChanged', 'sa{sv}as', (plugin.IFACE, {'MicMuted': True}, []),
            plugin.PROPS_IFACE, sender=':1.42', path=plugin.OBJECT))
        self.assertGreater(len(events), n)
        self.assertEqual(self.titles(events)[-1], 'Mic muted')
        self.assertEqual(app.keys['key']['state'], 1)

    async def test_poll_lists_even_when_props_are_live(self):
        app, bus, _, _ = self.setup_plugin()
        await self.appear(app)
        self.assertTrue(app.controls.props)
        before = len(bus.calls)
        app.last_poll = 0
        await app.poll()
        members = [call['member'] for call in bus.calls[before:]]
        self.assertIn('ListBuses', members)
        self.assertIn('ListScenes', members)

    async def test_keydown_skips_refresh_when_props_are_fresh(self):
        app, bus, _, events = self.setup_plugin()
        await self.appear(app, 'mic')
        app.last_poll = 0
        before = len(bus.calls)
        await self.down(app)
        members = [call['member'] for call in bus.calls[before:]]
        self.assertNotIn('ListBuses', members)
        self.assertNotIn('ListScenes', members)
        self.assertIn('ToggleBusMuted', members)
        self.assertFalse(any(event['event'] == 'showOk' for event in events))

    SIDE_LEVELS = [('stream', 'Stream', 1.0, 1.0, False, ''), ('mic', 'Mic', 1.0, 1.0, False, ''),
                   ('game', 'Game', 0.5, 0.5, False, 'both')]

    def with_levels(self, bus, game=None):
        bus.list_levels = list(self.SIDE_LEVELS)
        if game is not None:
            bus.list_levels[-1] = game

    def shown(self, app, context='key'):
        key = app.keys[context]
        return key['state'], key['title']

    def calls_of(self, bus, member):
        return [call['args'] for call in bus.calls if call['member'] == member]

    async def test_side_mute_takes_bus_off_stream(self):
        app, bus, _, _ = self.setup_plugin()
        self.with_levels(bus)
        await self.appear(app, 'bus', {'bus': 'game', 'side': 'stream'})
        self.assertEqual(self.shown(app), (0, 'Game\non stream'))
        await self.down(app)
        self.assertEqual(self.calls_of(bus, 'ToggleBusInMix'), [('game', 'stream')])
        self.assertEqual(self.calls_of(bus, 'ToggleBusMuted'), [])
        bus.list_levels[-1] = ('game', 'Game', 0.5, 0.5, False, 'phones')
        await self.up(app)
        self.assertEqual(self.shown(app), (1, 'Game\noff stream'))

    async def test_side_mute_for_me_titles(self):
        app, bus, _, _ = self.setup_plugin()
        self.with_levels(bus, ('game', 'Game', 0.5, 0.5, False, 'stream'))
        await self.appear(app, 'bus', {'bus': 'game', 'side': 'phones'})
        self.assertEqual(self.shown(app), (1, 'Game\noff for me'))
        bus.list_levels[-1] = ('game', 'Game', 0.5, 0.5, True, 'both')
        await app.poll(force=True)
        self.assertEqual(self.shown(app), (1, 'Game\noff for me'))
        bus.list_levels[-1] = ('game', 'Game', 0.5, 0.5, False, 'both')
        await app.poll(force=True)
        self.assertEqual(self.shown(app), (0, 'Game\non for me'))

    async def test_side_mute_unavailable_in_cli_mode(self):
        app, _, runner, events = self.setup_plugin(connect=False)
        await self.appear(app, 'bus', {'bus': 'game', 'side': 'stream'})
        self.assertEqual(self.shown(app)[1], 'Unavailable')
        await self.down(app)
        self.assertEqual(self.alerts(events), 1)
        self.assertTrue(all('--list-buses' in command or '--list-scenes' in command
                            for command in runner.commands), runner.commands)

    async def test_side_mute_needs_update_on_older_rostrum(self):
        app, bus, _, events = self.setup_plugin()
        await self.appear(app, 'mic', context='mic')
        await self.appear(app, 'bus', {'bus': 'game', 'side': 'phones'})
        self.assertEqual(self.shown(app), (0, 'Needs update'))
        await self.down(app)
        self.assertEqual(self.alerts(events), 1)
        self.assertEqual(self.calls_of(bus, 'ToggleBusInMix') + self.calls_of(bus, 'ToggleBusMuted'), [])
        self.assertEqual(self.shown(app, 'mic')[1], 'Mic live')

    async def test_bus_key_saved_by_1_3_has_no_side_and_still_mutes(self):
        app, bus, _, _ = self.setup_plugin()
        self.with_levels(bus)
        await self.appear(app, 'bus', {'bus': 'game'})
        self.assertEqual(self.shown(app), (0, 'Game live'))
        await self.down(app)
        self.assertEqual(self.calls_of(bus, 'ToggleBusMuted'), [('game',)])

    async def test_side_mute_on_a_master_behaves_as_both(self):
        app, bus, _, _ = self.setup_plugin()
        self.with_levels(bus)
        await self.appear(app, 'bus', {'bus': 'stream', 'side': 'stream'})
        await self.down(app)
        self.assertEqual(self.calls_of(bus, 'ToggleBusMuted'), [('stream',)])
        self.assertEqual(self.calls_of(bus, 'ToggleBusInMix'), [])

    ONE_LEVEL_ROWS = [('stream', 'Stream', 1.0, 1.0, False, ''), ('phones', 'Headphones', 0.8, 0.8, False, ''),
                      ('mic', 'Mic', 1.0, 1.0, False, ''), ('game', 'Game', 0.5, 0.5, False, 'both')]
    OTHER_SIDE = [('stream', 'phones', 'Stream'), ('phones', 'stream', 'Headphones'), ('mic', 'stream', 'Mic'),
                  ('mic', 'phones', 'Mic')]

    async def test_side_mute_on_a_one_level_bus_of_the_other_side_does_nothing(self):
        # A "for me" key on the Stream master would unmute the whole stream; a side key on the mic
        # would mute it on stream. Only a master on its own side mutes, as a Both key does.
        app, bus, _, events = self.setup_plugin()
        bus.list_levels = list(self.ONE_LEVEL_ROWS)
        for index, (bus_id, side, name) in enumerate(self.OTHER_SIDE):
            await self.appear(app, 'bus', {'bus': bus_id, 'side': side}, context=f'k{index}')
            self.assertEqual(self.shown(app, f'k{index}'), (0, f'{name}\nBoth only'))
            await self.down(app, f'k{index}')
            await self.up(app, f'k{index}')
        self.assertEqual(self.alerts(events), len(self.OTHER_SIDE))
        self.assertEqual(self.calls_of(bus, 'ToggleBusMuted'), [])
        self.assertEqual(self.calls_of(bus, 'ToggleBusInMix'), [])
        await self.appear(app, 'bus', {'bus': 'phones', 'side': 'phones'}, context='own')
        await self.down(app, 'own')
        self.assertEqual(self.calls_of(bus, 'ToggleBusMuted'), [('phones',)])

    async def test_side_volume_on_a_one_level_bus_of_the_other_side_does_nothing(self):
        app, bus, _, events = self.setup_plugin()
        bus.list_levels = list(self.ONE_LEVEL_ROWS)
        for index, (bus_id, side, name) in enumerate(self.OTHER_SIDE):
            await self.appear(app, 'volumeUp', {'bus': bus_id, 'side': side}, context=f'k{index}')
            self.assertEqual(self.shown(app, f'k{index}'), (0, f'{name}\nBoth only'))
            await self.down(app, f'k{index}')
            await self.up(app, f'k{index}')
        self.assertEqual(self.alerts(events), len(self.OTHER_SIDE))
        self.assertEqual(self.calls_of(bus, 'StepBusVolume'), [])
        await self.appear(app, 'volumeDown', {'bus': 'stream', 'side': 'stream'}, context='own')
        self.assertEqual(self.shown(app, 'own')[1], 'Stream\n100%')
        await self.down(app, 'own')
        await self.up(app, 'own')
        self.assertEqual(self.calls_of(bus, 'StepBusVolume'), [('stream', 'both', -0.05)])

    async def test_unknown_side_is_both(self):
        app, bus, _, _ = self.setup_plugin()
        self.with_levels(bus)
        await self.appear(app, 'bus', {'bus': 'game', 'side': 'left'})
        self.assertEqual(self.shown(app), (0, 'Game live'))
        await self.down(app)
        self.assertEqual(self.calls_of(bus, 'ToggleBusMuted'), [('game',)])

    VOLUME_LEVELS = [('stream', 'Stream', 1.0, 1.0, False, ''), ('mic', 'Mic', 1.5, 1.5, False, ''),
                     ('game', 'Game', 0.5, 0.8, False, 'both')]

    def volume_plugin(self, **kwargs):
        app, bus, runner, events = self.setup_plugin(**kwargs)
        bus.list_levels = list(self.VOLUME_LEVELS)
        return app, bus, runner, events

    def steps(self, bus):
        return self.calls_of(bus, 'StepBusVolume')

    def fast_repeat(self):
        delay = patch.object(plugin, 'REPEAT_DELAY', 0.02)
        interval = patch.object(plugin, 'REPEAT_INTERVAL', 0.01)
        delay.start()
        interval.start()
        self.addCleanup(delay.stop)
        self.addCleanup(interval.stop)

    async def test_volume_down_steps_once_per_press(self):
        app, bus, _, _ = self.volume_plugin()
        await self.appear(app, 'volumeDown', {'bus': 'game', 'side': 'stream'})
        await self.down(app)
        await self.up(app)
        self.assertEqual(self.steps(bus), [('game', 'stream', -0.05)])
        self.assertEqual(self.shown(app)[1], 'Game\nStream 45%')

    async def test_volume_hold_repeats_until_key_up(self):
        self.fast_repeat()
        app, bus, _, _ = self.volume_plugin()
        await self.appear(app, 'volumeUp', {'bus': 'game', 'side': 'phones'})
        await self.down(app)
        await asyncio.sleep(0.1)
        self.assertGreaterEqual(len(self.steps(bus)), 3)
        await self.up(app)
        count = len(self.steps(bus))
        await asyncio.sleep(0.05)
        self.assertEqual(len(self.steps(bus)), count)

    async def test_volume_hold_stops_when_key_disappears(self):
        self.fast_repeat()
        app, bus, _, _ = self.volume_plugin()
        await self.appear(app, 'volumeUp', {'bus': 'game', 'side': 'phones'})
        await self.down(app)
        await asyncio.sleep(0.1)
        self.assertGreaterEqual(len(self.steps(bus)), 3)
        await app.handle({'event': 'willDisappear', 'context': 'key'})
        count = len(self.steps(bus))
        await asyncio.sleep(0.05)
        self.assertEqual(len(self.steps(bus)), count)

    async def test_slow_step_drops_repeats(self):
        self.fast_repeat()
        app, bus, _, _ = self.volume_plugin()
        bus.step_delay = 0.05
        await self.appear(app, 'volumeDown', {'bus': 'game', 'side': 'both'})
        await self.down(app)
        await asyncio.sleep(0.2)
        await self.up(app)
        self.assertEqual(bus.in_flight_max, 1)
        self.assertLessEqual(len(self.steps(bus)), 6)

    async def test_volume_titles_for_each_side(self):
        app, bus, _, _ = self.volume_plugin()
        for context, action, settings in (('s', 'volumeDown', {'bus': 'game', 'side': 'stream'}),
                                          ('p', 'volumeDown', {'bus': 'game', 'side': 'phones'}),
                                          ('b', 'volumeUp', {'bus': 'game', 'side': 'both'}),
                                          ('m', 'volumeUp', {'bus': 'mic'}),
                                          ('u', 'volumeUp', {})):
            await self.appear(app, action, settings, context=context)
        self.assertEqual(self.shown(app, 's')[1], 'Game\nStream 50%')
        self.assertEqual(self.shown(app, 'p')[1], 'Game\nMe 80%')
        self.assertEqual(self.shown(app, 'b')[1], 'Game\nS50 Me80')
        self.assertEqual(self.shown(app, 'm')[1], 'Mic\n150%')
        self.assertEqual(self.shown(app, 'u')[1], 'Volume up')
        bus.list_levels[-1] = ('game', 'Game', 0.5, 0.5, False, 'both')
        await app.poll(force=True)
        self.assertEqual(self.shown(app, 'b')[1], 'Game\n50%')

    async def test_volume_state_when_side_is_silent(self):
        app, bus, _, _ = self.volume_plugin()
        bus.list_levels[-1] = ('game', 'Game', 0.5, 0.8, False, 'phones')
        await self.appear(app, 'volumeDown', {'bus': 'game', 'side': 'stream'}, context='s')
        await self.appear(app, 'volumeDown', {'bus': 'game', 'side': 'phones'}, context='p')
        await self.appear(app, 'volumeDown', {'bus': 'game', 'side': 'both'}, context='b')
        self.assertEqual(self.shown(app, 's')[0], 1)
        self.assertEqual(self.shown(app, 'p')[0], 0)
        self.assertEqual(self.shown(app, 'b')[0], 0)
        bus.list_levels[-1] = ('game', 'Game', 0.5, 0.8, True, 'both')
        await app.poll(force=True)
        self.assertEqual(self.shown(app, 'b')[0], 1)

    async def test_volume_keys_unavailable_in_cli_mode(self):
        app, _, runner, events = self.setup_plugin(connect=False)
        await self.appear(app, 'volumeDown', {'bus': 'game', 'side': 'both'})
        self.assertEqual(self.shown(app)[1], 'Unavailable')
        await self.down(app)
        self.assertEqual(self.alerts(events), 1)
        self.assertTrue(all('--list-buses' in command or '--list-scenes' in command
                            for command in runner.commands), runner.commands)

    async def test_volume_needs_update_on_older_rostrum(self):
        app, bus, _, events = self.setup_plugin()
        await self.appear(app, 'mic', context='mic')
        await self.appear(app, 'volumeDown', {'bus': 'game', 'side': 'both'})
        self.assertEqual(self.shown(app)[1], 'Needs update')
        await self.down(app)
        self.assertEqual(self.alerts(events), 1)
        self.assertEqual(self.steps(bus), [])
        self.assertEqual(self.shown(app, 'mic')[1], 'Mic live')

    async def test_side_volume_one_level_without_per_mix_levels(self):
        app, bus, _, events = self.volume_plugin()
        bus.props['PerMixLevels'] = False
        await self.appear(app, 'volumeDown', {'bus': 'game', 'side': 'stream'}, context='s')
        await self.appear(app, 'volumeDown', {'bus': 'game', 'side': 'both'}, context='b')
        self.assertEqual(self.shown(app, 's')[1], 'Game\nOne level')
        await self.down(app, 's')
        self.assertEqual(self.alerts(events), 1)
        self.assertEqual(self.steps(bus), [])
        await self.down(app, 'b')
        await self.up(app, 'b')
        self.assertEqual(self.steps(bus), [('game', 'both', -0.05)])

    async def test_owner_change_stops_repeating(self):
        self.fast_repeat()
        app, bus, _, _ = self.volume_plugin()
        await self.appear(app, 'volumeDown', {'bus': 'game', 'side': 'stream'})
        await self.down(app)
        await asyncio.sleep(0.05)
        bus.owner = ':1.77'
        app.controls._on_signal(_signal('NameOwnerChanged', 'sss', (plugin.SERVICE, ':1.42', ':1.77'),
                                        plugin.BUS_IFACE, sender=plugin.BUS_SERVICE, path=plugin.BUS_PATH))
        await asyncio.gather(*app.controls._tasks)
        self.assertEqual(app.stepping, {})
        count = len(self.steps(bus))
        await asyncio.sleep(0.05)
        self.assertEqual(len(self.steps(bus)), count)
        self.assertFalse(any(call['member'] == 'StepBusVolume' and call['destination'] == ':1.77'
                             for call in bus.calls))

    async def test_missing_bus_stops_repeating(self):
        self.fast_repeat()
        app, bus, _, events = self.volume_plugin()
        bus.failures['StepBusVolume'] = plugin.DBusError(plugin.IFACE + '.Error.NoSuchBus', 'gone')
        await self.appear(app, 'volumeDown', {'bus': 'game', 'side': 'stream'})
        await self.down(app)
        self.assertEqual(len(self.steps(bus)), 1)
        self.assertEqual(self.alerts(events), 1)
        self.assertEqual(self.shown(app)[1], 'Missing')
        await asyncio.sleep(0.05)
        self.assertEqual(len(self.steps(bus)), 1)

    async def test_second_key_down_does_not_start_a_second_repeat(self):
        self.fast_repeat()
        app, bus, _, _ = self.volume_plugin()
        await self.appear(app, 'volumeDown', {'bus': 'game', 'side': 'stream'})
        await self.down(app)
        await self.down(app)
        self.assertEqual(len(self.steps(bus)), 1)
        self.assertEqual(len(app.stepping), 1)
        await self.up(app)
        await asyncio.sleep(0.05)
        self.assertEqual(len(self.steps(bus)), 1)

    async def test_step_setting_is_clamped(self):
        app, bus, _, _ = self.volume_plugin()
        for index, step in enumerate(('5', 0, 400, True, 12.4)):
            context = f'k{index}'
            await self.appear(app, 'volumeDown', {'bus': 'game', 'side': 'stream', 'step': step}, context=context)
            await self.down(app, context)
            await self.up(app, context)
        self.assertEqual([args[2] for args in self.steps(bus)], [-0.05, -0.01, -0.25, -0.05, -0.12])

    @unittest.skipUnless(plugin.shutil.which('node'), 'optional inspector check requires Node.js')
    def test_inspector_settings_round_trip(self):
        result = subprocess.run(['node', str(ROOT.parent / 'test_inspector.js')], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_every_plugin_file_is_bundled(self):
        # Settings → Integrations → OpenDeck installs only the files resources.cmake embeds.
        listed = set(re.findall(r'"\$\{bundle\}/([^"]+)"', (ROOT.parent / 'resources.cmake').read_text()))
        present = {path.relative_to(ROOT).as_posix() for path in ROOT.rglob('*')
                   if path.is_file() and '__pycache__' not in path.parts and path.suffix != '.pyc'}
        self.assertEqual(present - listed, set(), 'add these to tools/opendeck/resources.cmake')
        self.assertEqual(listed - present, set(), 'resources.cmake lists files that do not exist')

    def test_manifest_assets(self):
        manifest = json.loads((ROOT / 'manifest.json').read_text())
        # The installer offers Update only when the bundle is newer than the last release (1.3.0).
        self.assertGreater(tuple(int(part) for part in manifest['Version'].split('.')), (1, 3, 0))
        actions = {action['UUID'].removeprefix(plugin.PREFIX): action for action in manifest['Actions']}
        self.assertEqual(set(actions), {'mic', 'panic', 'scene', 'bus', 'volumeDown', 'volumeUp', 'studioVoice',
                                        'pushToTalk'})
        # The plugin sends setState 1 for each of these; OpenDeck ignores a state the manifest
        # lacks, and two states with one image look the same on the key.
        for name in ('mic', 'panic', 'scene', 'bus', 'volumeDown', 'volumeUp', 'studioVoice', 'pushToTalk'):
            images = [state['Image'] for state in actions[name]['States']]
            self.assertEqual(len(images), 2, name)
            self.assertNotEqual(images[0], images[1], name)
        self.assertNotIn('OnlyShowIn', manifest)
        self.assertTrue((ROOT / manifest['CodePathLin']).stat().st_mode & 0o111)
        for action in manifest['Actions']:
            self.assertNotIn('OnlyShowIn', action)
            for state in action['States']:
                self.assertTrue((ROOT / (state['Image'] + '.svg')).is_file())


class ControlsTest(unittest.IsolatedAsyncioTestCase):
    LEVELS = [('mic', 'Mic', 1.0, 1.0, False, ''), ('game', 'Game', 0.5, 0.8, True, 'both')]

    def controls(self, *, owner=':1.42', connect=True, run=None):
        bus = FakeBus(owner=owner)
        runner = run or CliRunner()
        factory = fake_connect(bus) if connect else refuse_bus
        backend = plugin.Controls(run=runner, executable='/test/rostrum', bus_factory=factory)
        return backend, bus, runner

    def members(self, bus):
        return [call['member'] for call in bus.calls]

    async def test_snapshot_reads_both_levels(self):
        controls, bus, _ = self.controls()
        bus.list_levels = list(self.LEVELS)
        buses, _ = await controls.snapshot()
        self.assertEqual(buses['game'], {'id': 'game', 'name': 'Game', 'muted': True, 'stream': 0.5,
                                         'phones': 0.8, 'destination': 'both'})
        self.assertIs(controls.levels, True)
        self.assertNotIn('ListBuses', self.members(bus))

    async def test_snapshot_falls_back_to_list_buses_on_older_rostrum(self):
        controls, bus, _ = self.controls()
        buses, _ = await controls.snapshot()
        self.assertNotIn('stream', buses['game'])
        self.assertTrue(buses['game']['muted'])
        self.assertIs(controls.levels, False)
        await controls.snapshot()
        self.assertEqual(self.members(bus).count('ListBusLevels'), 1)

    async def test_new_owner_is_probed_again(self):
        # Rostrum upgraded while OpenDeck keeps running: the new instance has the new keys.
        controls, bus, _ = self.controls()
        await controls.snapshot()
        self.assertIs(controls.levels, False)
        bus.owner = ':1.77'
        bus.list_levels = list(self.LEVELS)
        controls._on_signal(_signal('NameOwnerChanged', 'sss', (plugin.SERVICE, ':1.42', ':1.77'),
                                    plugin.BUS_IFACE, sender=plugin.BUS_SERVICE, path=plugin.BUS_PATH))
        await asyncio.gather(*controls._tasks)
        self.assertEqual(controls.owner, ':1.77')
        buses, _ = await controls.snapshot()
        self.assertIs(controls.levels, True)
        self.assertEqual(buses['game']['destination'], 'both')

    async def test_step_sends_a_double_and_returns_the_level(self):
        controls, bus, _ = self.controls()
        bus.list_levels = list(self.LEVELS)
        await controls.ensure_bus()
        self.assertEqual(await controls.step('game', 'stream', -0.05), 0.45)
        call = next(call for call in bus.calls if call['member'] == 'StepBusVolume')
        self.assertEqual((call['signature'], call['args']), ('ssd', ('game', 'stream', -0.05)))
        self.assertEqual(call['destination'], ':1.42')

    async def test_step_and_in_mix_have_no_cli_fallback(self):
        controls, _, runner = self.controls(connect=False)
        with self.assertRaises(plugin.Unavailable):
            await controls.step('game', 'stream', -0.05)
        with self.assertRaises(plugin.Unavailable):
            await controls.toggle_in_mix('game', 'stream')
        self.assertEqual(runner.commands, [])

    async def test_toggle_in_mix_call(self):
        controls, bus, _ = self.controls()
        await controls.ensure_bus()
        await controls.toggle_in_mix('game', 'phones')
        call = next(call for call in bus.calls if call['member'] == 'ToggleBusInMix')
        self.assertEqual((call['signature'], call['args']), ('ss', ('game', 'phones')))

    async def test_unknown_method_is_unsupported(self):
        controls, bus, _ = self.controls()
        bus.failures['StepBusVolume'] = plugin.DBusError(plugin.UNKNOWN_METHOD, 'x')
        with self.assertRaises(plugin.Unsupported) as caught:
            await controls.step('game', 'stream', -0.05)
        self.assertIsInstance(caught.exception, plugin.Unavailable)

    async def test_lost_connection_during_levels_uses_cli(self):
        controls, bus, runner = self.controls()
        bus.failures['ListBusLevels'] = ConnectionResetError
        buses, _ = await controls.snapshot()
        self.assertTrue(controls.cli_only)
        self.assertTrue(buses['game']['muted'])
        self.assertTrue(any('--list-buses' in command for command in runner.commands))

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
            plugin.BUS_IFACE, sender=plugin.BUS_SERVICE, path=plugin.BUS_PATH))
        for _ in range(3):
            await asyncio.sleep(0)
        self.assertIsNone(controls.owner)
        self.assertEqual(controls.props, {})
        self.assertEqual(owners, [None])
        controls.on_owner = explode
        controls._on_signal(_signal(
            'NameOwnerChanged', 'sss', (plugin.SERVICE, '', ''),
            plugin.BUS_IFACE, sender=plugin.BUS_SERVICE))
        for _ in range(3):
            await asyncio.sleep(0)
        self.assertIsNone(controls.owner)
        await controls.close()
        self.assertTrue(bus.closed)

    async def test_properties_changed_after_empty_owner_leaves_props_empty(self):
        controls, _, _ = self.controls()
        await controls.ensure_bus()
        await controls._handle_owner('', '')
        self.assertEqual(controls.props, {})
        controls._on_signal(_signal(
            'PropertiesChanged', 'sa{sv}as', (plugin.IFACE, {'MicMuted': True}, []),
            plugin.PROPS_IFACE, sender=':1.42', path=plugin.OBJECT))
        self.assertEqual(controls.props, {})
        await controls.close()

    async def test_late_getall_after_empty_owner_does_not_refill_props(self):
        controls, bus, _ = self.controls()
        await controls.ensure_bus()
        started = asyncio.Event()
        release = asyncio.Event()
        original = bus.call

        async def delayed_call(destination, path, interface, member, signature='', *args, timeout=2.0):
            if member == 'GetAll':
                started.set()
                await release.wait()
            return await original(destination, path, interface, member, signature, *args, timeout=timeout)

        bus.call = delayed_call
        task = asyncio.create_task(controls._load_props())
        await started.wait()
        await controls._handle_owner('', '')
        self.assertEqual(controls.props, {})
        release.set()
        await task
        self.assertEqual(controls.props, {})
        await controls.close()

    async def test_close_clears_props(self):
        controls, bus, _ = self.controls()
        await controls.ensure_bus()
        self.assertTrue(controls.props)
        await controls.close()
        self.assertIsNone(controls.owner)
        self.assertEqual(controls.props, {})
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

    async def test_starting_rostrum_is_retried_on_the_bus(self):
        # Rostrum claims its name before it registers the Control object, so the first
        # calls after NameOwnerChanged can get UnknownObject. That is not a reason for the CLI.
        controls, bus, runner = self.controls()
        starting = plugin.DBusError('org.freedesktop.DBus.Error.UnknownObject', 'No such object path')
        bus.failures['GetAll'] = starting
        bus.failures['ListBuses'] = starting
        with self.assertRaises(plugin.Unavailable):
            await controls.snapshot()
        self.assertFalse(controls.cli_only)
        self.assertEqual(controls.props, {})
        self.assertEqual(runner.commands, [])
        bus.failures.clear()
        buses, _ = await controls.snapshot()
        self.assertIn('mic', buses)
        # The first publish after startup sends no PropertiesChanged, so the poll loads them.
        self.assertEqual(controls.props.get('CurrentScene'), 'Live')
        self.assertFalse(controls.cli_only)
        self.assertEqual(runner.commands, [])
        await controls.close()

    async def test_list_timeout_is_unavailable_not_cli(self):
        controls, bus, runner = self.controls()
        await controls.snapshot()
        bus.failures['ListBuses'] = TimeoutError
        with self.assertRaises(plugin.Unavailable):
            await controls.snapshot()
        self.assertFalse(controls.cli_only)
        self.assertEqual(runner.commands, [])
        await controls.close()

    async def test_cli_mode_returns_to_the_bus(self):
        # One lost connection must not leave push to talk Unavailable until OpenDeck restarts.
        bus = FakeBus()
        attempts = []

        async def flaky(address=None):
            attempts.append(address)
            if len(attempts) == 1:
                raise OSError('bus restarting')
            return bus

        controls = plugin.Controls(run=CliRunner(), executable='/test/rostrum', bus_factory=flaky)
        clock = [100.0]
        with patch.object(plugin.time, 'monotonic', lambda: clock[0]):
            await controls.ensure_bus()
            self.assertTrue(controls.cli_only)
            await controls.ensure_bus()
            self.assertEqual(len(attempts), 1)  # one try per interval, not one per call
            clock[0] += plugin.BUS_RETRY_SECONDS
            await controls.ensure_bus()
        self.assertEqual(len(attempts), 2)
        self.assertFalse(controls.cli_only)
        self.assertEqual(controls.owner, ':1.42')
        self.assertIs(controls.props['MicMuted'], False)
        await controls.close()

    async def test_bus_connect_that_never_answers_gives_up(self):
        # A socket that accepts and never speaks would otherwise stall every key.
        async def hang(address=None):
            await asyncio.Event().wait()

        controls = plugin.Controls(run=CliRunner(), executable='/test/rostrum', bus_factory=hang)
        with patch.object(plugin, 'BUS_CONNECT_TIMEOUT', 0.05):
            await asyncio.wait_for(controls.ensure_bus(), 1)
        self.assertTrue(controls.cli_only)

    async def test_signal_newer_than_get_all_wins(self):
        # The reader can dispatch a PropertiesChanged sent after GetAll's reply before
        # _load_props resumes. The older snapshot must not overwrite it.
        controls, bus, _ = self.controls()
        bus.changes_after_reply = {'GetAll': {'MicMuted': True}}
        await controls.ensure_bus()
        self.assertIs(controls.props['MicMuted'], True)
        self.assertEqual(controls.props['CurrentScene'], 'Live')
        await controls.close()

    async def test_lost_bus_moves_to_cli_and_drops_stale_props(self):
        controls, bus, runner = self.controls()
        await controls.snapshot()
        self.assertTrue(controls.props)
        bus.failures['ListBuses'] = ConnectionError
        buses, _ = await controls.snapshot()
        self.assertIn('mic', buses)
        self.assertTrue(controls.cli_only)
        # Properties from a dead connection would freeze the mic key; buses come from the CLI.
        self.assertEqual(controls.props, {})
        self.assertTrue(bus.closed)
        self.assertIn(['/test/rostrum', '--no-start', '--list-buses'], runner.commands)
        await controls.close()


class MicFeedbackTest(unittest.IsolatedAsyncioTestCase):
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

    async def asyncSetUp(self):
        self.backend = self.Backend()
        self.app = plugin.Plugin(self.backend, lambda event: None)
        self.feedback = self.Feedback()
        self.app.feedback = self.feedback
        await self.app.handle({'event': 'willAppear', 'context': 'mic-key',
                               'action': plugin.PREFIX + 'mic', 'payload': {'settings': {}}})

    async def event(self, kind):
        await self.app.handle({'event': kind, 'context': 'mic-key'})

    async def test_feedback_follows_confirmed_mute_and_unmute(self):
        await self.event('keyDown')
        self.assertEqual(self.feedback.states, [], 'wait for the confirmed state')
        await self.event('keyUp')
        self.assertEqual(self.feedback.states, [True])
        await self.event('keyDown'); await self.event('keyUp')
        self.assertEqual(self.feedback.states, [True, False])
        await self.app.poll(force=True); await self.event('keyUp')
        self.assertEqual(self.feedback.states, [True, False], 'polling never repeats a cue')

    async def test_feedback_uses_actual_state_instead_of_assuming_toggle(self):
        await self.event('keyDown')
        self.backend.muted = False  # Another control changed it before confirmation.
        await self.event('keyUp')
        self.assertEqual(self.feedback.states, [False])

    async def test_per_key_volume_is_used_on_both_cues(self):
        await self.app.handle({'event': 'didReceiveSettings', 'context': 'mic-key',
                               'payload': {'settings': {'soundVolume': 25}}})
        await self.event('keyDown'); await self.event('keyUp')
        await self.event('keyDown'); await self.event('keyUp')
        self.assertEqual(self.feedback.states, [True, False])
        self.assertEqual(self.feedback.volumes, [25, 25])

    async def test_disabled_feedback_still_toggles(self):
        await self.app.handle({'event': 'didReceiveSettings', 'context': 'mic-key',
                               'payload': {'settings': {'playSound': False}}})
        await self.event('keyDown'); await self.event('keyUp')
        self.assertTrue(self.backend.muted)
        self.assertEqual(self.feedback.states, [])

    async def test_zero_volume_does_not_block_mic_control(self):
        await self.app.handle({'event': 'didReceiveSettings', 'context': 'mic-key',
                               'payload': {'settings': {'soundVolume': 0}}})
        await self.event('keyDown'); await self.event('keyUp')
        self.assertTrue(self.backend.muted)
        self.assertEqual(self.feedback.volumes, [0])

    async def test_unreachable_after_toggle_discards_feedback(self):
        await self.event('keyDown'); self.backend.absent = True
        await self.event('keyUp')
        self.backend.absent = False
        await self.app.poll(force=True)
        self.assertEqual(self.feedback.states, [])

    async def test_failed_toggle_has_no_feedback(self):
        self.backend.fail_toggle = True
        await self.event('keyDown'); await self.event('keyUp')
        self.assertEqual(self.feedback.states, [])

    async def test_disappearing_key_discards_feedback(self):
        await self.event('keyDown'); await self.event('willDisappear')
        await self.app.poll(force=True)
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

    def test_codec_round_trips_mix_signatures(self):
        encode, decode = plugin_dbus.encode_body, plugin_dbus.decode_body
        self.assertEqual(decode('ssd', encode('ssd', 'game', 'stream', -0.05)), ('game', 'stream', -0.05))
        rows = [('mic', 'Mic', 1.0, 1.0, False, ''), ('game', 'Game', 0.5, 0.8, True, 'both')]
        self.assertEqual(decode('a(ssddbs)', encode('a(ssddbs)', rows)), (rows,))
        self.assertEqual(decode('d', encode('d', 0.45)), (0.45,))

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


def _odd_message(msg_type, serial, headers):
    """A well-framed message with an int32 body, which the plugin never asks for."""
    with patch.object(plugin_dbus, '_ALLOWED', plugin_dbus._ALLOWED | {'i'}):
        return plugin_dbus.encode_message('l', msg_type, 0, serial, dict(headers, signature='i'),
                                          struct.pack('<i', 7))


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

    async def _odd_peer(self, reader, writer):
        # A signal and a reply whose bodies are outside the allow-list, then a good reply.
        try:
            await self._sasl(reader, writer)
            first = await self._read_message(reader)
            odd_signal = _odd_message(4, 2, {'path': '/x', 'interface': 'x.Y', 'member': 'Odd'})
            odd_reply = _odd_message(2, 3, {'reply_serial': first.serial, 'sender': ':1.1'})
            writer.write(odd_signal + odd_reply)
            await writer.drain()
            second = await self._read_message(reader)
            reply = plugin_dbus.encode_message(
                'l', 2, 0, 4,
                {'reply_serial': second.serial, 'signature': 's', 'sender': ':1.1'},
                plugin_dbus.encode_body('s', 'ok'))
            writer.write(reply)
            await writer.drain()
            await self._finish(reader, writer)
        except (asyncio.IncompleteReadError, ConnectionError, BrokenPipeError):
            writer.close()

    async def test_message_outside_allow_list_is_skipped_not_fatal(self):
        server = await self._serve(self._odd_peer)
        signals = []

        def on_signal(message):
            signals.append(message.member)
            raise ValueError('a handler bug must not close the connection either')

        async with server:
            bus = await plugin_dbus.SessionBus.connect(address=f'unix:path={self._sock_path}')
            bus.on_signal = on_signal
            with self.assertRaises(plugin_dbus.DBusError):
                await bus.call(':1.1', '/x', 'x.Y', 'Ping', '', timeout=1)
            self.assertEqual(await bus.call(':1.1', '/x', 'x.Y', 'Ping', '', timeout=1), ('ok',))
            self.assertEqual(signals, ['Odd'])
            await bus.close()

    async def test_falls_back_to_runtime_dir_bus(self):
        # libdbus and GDBus use $XDG_RUNTIME_DIR/bus when the variable is unset; so does the CLI.
        server = await self._serve(self._silent_peer)
        async with server:
            with patch.dict(os.environ, {'XDG_RUNTIME_DIR': self._dir.name}, clear=True):
                bus = await plugin_dbus.SessionBus.connect()
            await bus.close()

    async def test_connects_to_abstract_address(self):
        # dbus-launch sessions hand out unix:abstract= addresses.
        name = f'rostrum-opendeck-test-{os.getpid()}-{id(self)}'
        self._server = await asyncio.start_unix_server(self._silent_peer, path='\0' + name)
        async with self._server:
            bus = await plugin_dbus.SessionBus.connect(address=f'unix:abstract={name},guid={_SASL_GUID}')
            await bus.close()

    async def test_reader_rejects_declared_oversize(self):
        server = await self._serve(self._oversize_peer)
        async with server:
            bus = await plugin_dbus.SessionBus.connect(
                address=f'unix:path={self._sock_path}')
            with self.assertRaises(ConnectionError):
                await bus.call(':1.1', '/x', 'x.Y', 'Ping', '', timeout=0.5)
            await bus.close()

    async def _big_endian_peer(self, reader, writer):
        try:
            await self._sasl(reader, writer)
            await self._read_message(reader)
            writer.write(struct.pack('>cBBBIII', b'B', 2, 1, 1, 0, 9, 0))
            await writer.drain()
            await self._finish(reader, writer)
        except (asyncio.IncompleteReadError, ConnectionError, BrokenPipeError):
            writer.close()

    async def test_reader_rejects_big_endian_frame(self):
        # Rostrum's bus is little-endian; a big-endian frame cannot be skipped in step.
        server = await self._serve(self._big_endian_peer)
        async with server:
            bus = await plugin_dbus.SessionBus.connect(address=f'unix:path={self._sock_path}')
            with self.assertRaises(ConnectionError):
                await bus.call(':1.1', '/x', 'x.Y', 'Ping', '', timeout=0.5)
            await bus.close()


if __name__ == '__main__':
    unittest.main()
