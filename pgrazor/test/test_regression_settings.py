import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock

spec = importlib.util.spec_from_file_location('settings_adapter', Path(__file__).resolve().parents[2] / 'tools/regression/settings.py')
s = importlib.util.module_from_spec(spec)
spec.loader.exec_module(s)


class FakeBridge:
    def __init__(self):
        self.value = False
        self.pid = '42'
        self.writes = []
        self.fail_restore = False

    def snapshot(self):
        return {'pid': self.pid, 'settings': {'joystickVisible': self.value, 'quickSpin': True, 'autoTransfer': False}}

    def set_joystick(self, value):
        self.writes.append(value)
        if self.fail_restore and value is False:
            raise s.SettingsError('restore-unavailable')
        self.value = value
        return self.snapshot()

    def restart(self, old):
        self.pid = '43'
        return self.pid


class SettingsTests(unittest.TestCase):
    def execute(self, bridge, restart):
        with tempfile.TemporaryDirectory() as directory:
            recovery = Path(directory) / 'recovery.json'
            rows = s.execute(bridge, restart, recovery)
            return rows, recovery.exists()

    def test_round_trip_restores_original(self):
        bridge = FakeBridge()
        rows, recovery = self.execute(bridge, bridge.restart)
        self.assertEqual(s._overall(rows), 'passed')
        self.assertEqual(bridge.writes, [True, False])
        self.assertFalse(bridge.value)
        self.assertFalse(recovery)

    def test_initial_failure_never_mutates(self):
        bridge = FakeBridge()
        bridge.snapshot = Mock(side_effect=s.SettingsError('missing-bridge'))
        rows, recovery = self.execute(bridge, bridge.restart)
        self.assertEqual(s._overall(rows), 'blocked')
        self.assertEqual(bridge.writes, [])
        self.assertFalse(recovery)

    def test_restart_failure_and_interruption_restore(self):
        for error in [s.SettingsError('restart-failed'), KeyboardInterrupt()]:
            bridge = FakeBridge()
            rows, recovery = self.execute(bridge, Mock(side_effect=error))
            self.assertEqual(s._overall(rows), 'blocked')
            self.assertEqual(bridge.writes, [True, False])
            self.assertFalse(recovery)

    def test_persistence_mismatch_fails_but_restores(self):
        bridge = FakeBridge()
        def restart(old):
            bridge.value = False
            return bridge.restart(old)
        rows, recovery = self.execute(bridge, restart)
        self.assertEqual(s._overall(rows), 'failed')
        self.assertTrue(any(r['test'] == 'persistence' and r['status'] == 'failed' for r in rows))
        self.assertFalse(recovery)

    def test_restore_failure_retains_recovery(self):
        bridge = FakeBridge()
        bridge.fail_restore = True
        rows, recovery = self.execute(bridge, bridge.restart)
        self.assertEqual(s._overall(rows), 'failed')
        self.assertTrue(recovery)

    def test_same_pid_restart_does_not_prove_persistence(self):
        bridge = FakeBridge()
        rows, _ = self.execute(bridge, lambda old: old)
        self.assertEqual(s._overall(rows), 'blocked')
        self.assertFalse(bridge.value)

    def test_strict_response_identity_and_types(self):
        bridge = s.Bridge(Mock(), '123')
        good = {'id':'a','pid':42,'ok':True,'settings':{'joystickVisible':False,'quickSpin':True,'autoTransfer':False}}
        self.assertEqual(bridge._validate(good, '42')['pid'], '42')
        with self.assertRaises(s.SettingsError): bridge._validate(good, '43')
        for value in ['false', 0, None]:
            bad = json.loads(json.dumps(good))
            bad['settings']['joystickVisible'] = value
            with self.assertRaises(s.SettingsError): bridge._validate(bad, '42')
        self.assertIsNone(bridge._match(json.dumps(good), 'foreign'))
        self.assertIsNone(bridge._match('broken', 'a'))
        with self.assertRaises(s.SettingsError): bridge.set_joystick('false')

    def test_poll_delivers_once_despite_stale_response(self):
        bridge = s.Bridge(Mock(), '123', clock=Mock())
        bridge.clock.monotonic.return_value = 0
        bridge._pid = Mock(return_value='42')
        answer = {'id':'a','pid':42,'ok':True,'settings':{'joystickVisible':False,'quickSpin':True,'autoTransfer':False}}
        bridge._su = Mock(side_effect=['', '{}', json.dumps(answer)])
        bridge._poll(s.TMP_PREFIX+'a'*32+'.json', 'a')
        deliveries = [c for c in bridge._su.call_args_list if c.args[0].startswith('cp ')]
        self.assertEqual(len(deliveries), 1)

    def test_inventory_requires_strict_counts_and_revision(self):
        bridge = s.Bridge(Mock(), '123')
        answer = {'pid':42,'ok':True,'settings':{'joystickVisible':False,'quickSpin':True,'autoTransfer':False},
                  'inventory':{'1':12,'701':0},'inventoryRevision':3}
        self.assertEqual(bridge._validate(answer, '42')['inventory'], {'1':12,'701':0})
        for counts, revision in [({'1':True},3),({'1':-1},3),({'0':1},3),({'x':1},3),([],3),({'1':1},True),({'1':1},0)]:
            with self.assertRaises(s.SettingsError):
                bridge._validate(dict(answer, inventory=counts, inventoryRevision=revision), '42')
        bridge._exchange = Mock(return_value={'pid':'42','settings':answer['settings']})
        with self.assertRaises(s.SettingsError): bridge.inventory()

    def test_backup_never_accepts_missing_or_non_boolean_checks(self):
        bridge = s.Bridge(Mock(), '123')
        fields = ('alwaysHitIncluded','mutated','settingsMatch','preferencesMatch','speedMatch','restored','ok')
        good = {'backup':dict.fromkeys(fields, True)}
        bridge._exchange = Mock(return_value=good)
        self.assertEqual(bridge.backup(), good)
        for bad in [{}, {'restored':True}, dict(good['backup'],ok='true')]:
            bridge._exchange.return_value = {'backup':bad}
            with self.assertRaises(s.SettingsError): bridge.backup()
        bridge._exchange.return_value = {'backup':{'restored':True}}
        self.assertTrue(bridge.backup(recover=True)['backup']['restored'])


if __name__ == '__main__':
    unittest.main()
