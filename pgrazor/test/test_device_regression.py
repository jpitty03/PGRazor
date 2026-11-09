"""Host fixtures for regression result accuracy; never connects to a device."""
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('device_regression', Path(__file__).resolve().parents[2] / 'tools/regression/run.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def event(pid, marker, tag='PGRazor'):
    return f'11-08 12:00:00.000 {pid} 123 I {tag} : 2026-11-08 [{marker}]'


READY = ('native-bootstrap-ready', 'native-scheduler-ready', 'account-session-verified')


class FakeClock:
    """Monotonic clock that only advances when the code under test sleeps; no real waits."""

    def __init__(self, start=1000.0):
        self.now = start

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        assert seconds >= 0, 'soak must never request a negative sleep'
        self.now += seconds


class FakeDevice:
    """Deterministic ADB stand-in whose PID and markers can change as the fake clock advances.

    Every log line carries a ' secret-body' suffix so fixtures can prove raw bodies never
    reach events.json or soak.json.
    """

    def __init__(self, clock, pid='42', pid_after=None, markers=READY, markers_after=None, raise_after=None):
        self.clock = clock
        self.base = clock.now
        self.pid = pid
        self.pid_after = pid_after            # (elapsed_seconds, replacement_pid)
        self.markers = markers
        self.markers_after = markers_after    # (elapsed_seconds, extra_markers)
        self.raise_after = raise_after        # (elapsed_seconds, exception) raised from logcat

    def _elapsed(self):
        return self.clock.now - self.base

    def shell(self, *args):
        if args[0] == 'date':
            return '11-08_12:00:00.000'
        if args[0] == 'am' and args[1] == 'force-stop':
            return ''
        if args[0] == 'am' and args[1] == 'start':
            return 'Status: ok'
        if args[0] == 'pidof':
            if self.pid_after and self._elapsed() >= self.pid_after[0]:
                return self.pid_after[1]
            return self.pid
        return ''

    def call(self, *args):
        if self.raise_after and self._elapsed() >= self.raise_after[0]:
            raise self.raise_after[1]
        markers = set(self.markers)
        if self.markers_after and self._elapsed() >= self.markers_after[0]:
            markers |= set(self.markers_after[1])
        return '\n'.join(event(self.pid, marker) + ' secret-body' for marker in markers).encode()


def run_soak_cycle(device, clock, soak_seconds, timeout=30):
    with tempfile.TemporaryDirectory() as directory, \
            patch.object(runner.time, 'monotonic', clock.monotonic), \
            patch.object(runner.time, 'sleep', clock.sleep), patch('builtins.print'):
        rows = runner.cycle(device, Path(directory), timeout, soak_seconds)
        soak_json = json.loads((Path(directory) / 'soak.json').read_text()) if (Path(directory) / 'soak.json').exists() else None
        events_text = (Path(directory) / 'events.json').read_text()
    return rows, soak_json, events_text


class RegressionTests(unittest.TestCase):
    def test_runtime_does_not_prove_authentication(self):
        rows = runner.classify({'native-bootstrap-ready', 'native-scheduler-ready'})
        self.assertEqual([r['status'] for r in rows], ['passed', 'blocked'])

    def test_foreign_pid_tag_and_body_are_not_evidence(self):
        lines = '\n'.join([
            event(99, 'account-session-verified'),
            event(42, 'account-session-verified', 'OtherApp'),
            '11-08 12:00:00.000 42 123 I PGRazor : time [unrelated] [account-session-verified]',
            '11-08 12:00:00.000 42 123 I PGRazor : time [native-bootstrap-ready] private-data',
        ])
        self.assertEqual(runner.evidence(lines, '42'), {'native-bootstrap-ready'})

    def test_error_overrides_readiness(self):
        rows = runner.classify({'native-bootstrap-ready', 'native-scheduler-ready', 'native-scheduler-unavailable'})
        self.assertEqual(rows[0]['status'], 'failed')

    def test_reports_exit_codes_and_partial_status(self):
        with tempfile.TemporaryDirectory() as directory:
            for status, code in [('passed', 0), ('failed', 1), ('blocked', 2)]:
                report = {'results': [{'test': 'fixture', 'status': status, 'reason': 'why'}]}
                self.assertEqual(runner.save_report(Path(directory), report), code)
                self.assertEqual(json.loads((Path(directory) / 'report.json').read_text())['status'], status)

    def test_plan_never_calls_adb(self):
        with patch.object(runner.Adb, 'call', side_effect=AssertionError('Device touched')), patch('builtins.print'):
            self.assertEqual(runner.main(['--serial', 'fixture']), 0)

    def test_offline_device_writes_blocked_report(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(runner.Adb, 'call', side_effect=runner.DeviceError('offline')), patch('builtins.print'):
            self.assertEqual(runner.main(['--serial', 'fixture', '--execute', '--output', directory]), 2)
            report = json.loads(next(Path(directory).glob('*/report.json')).read_text())
            self.assertEqual(report['status'], 'blocked')

    def test_process_change_cannot_pass(self):
        adb = unittest.mock.Mock()
        adb.shell.side_effect = ['11-08_12:00:00.000', '', 'Status: ok', '42', '43']
        with tempfile.TemporaryDirectory() as directory:
            rows = runner.cycle(adb, Path(directory), 5)
        self.assertEqual(rows[0]['status'], 'failed')

    def test_current_launch_can_pass_without_retaining_private_body(self):
        adb = unittest.mock.Mock()
        adb.shell.side_effect = ['11-08_12:00:00.000', '', 'Status: ok', '42', '42']
        adb.call.return_value = ('\n'.join(event(42, marker) + ' private-name' for marker in
                                 ['native-bootstrap-ready', 'native-scheduler-ready', 'account-session-verified'])).encode()
        with tempfile.TemporaryDirectory() as directory:
            rows = runner.cycle(adb, Path(directory), 5)
            self.assertTrue(all(row['status'] == 'passed' for row in rows))
            self.assertNotIn('private-name', (Path(directory) / 'events.json').read_text())
        adb.call.assert_called_once_with('logcat', '-d', '-v', 'threadtime', '-T', '11-08 12:00:00.000', '-s', 'PGRazor:I')

    def test_soak_default_zero_is_backward_compatible(self):
        # soak-seconds 0 keeps the legacy result shape: no soak row and no soak.json.
        clock = FakeClock()
        rows, soak_json, _ = run_soak_cycle(FakeDevice(clock), clock, 0)
        self.assertEqual([r['test'] for r in rows], ['native-runtime', 'authenticated-session'])
        self.assertTrue(all(r['status'] == 'passed' for r in rows))
        self.assertIsNone(soak_json)

    def test_successful_soak_reports_passed_without_real_waits(self):
        clock = FakeClock()
        rows, soak_json, events_text = run_soak_cycle(FakeDevice(clock), clock, 60)
        soak_row = next(r for r in rows if r['test'] == 'soak')
        self.assertEqual(soak_row['status'], 'passed')
        self.assertTrue(all(r['status'] == 'passed' for r in rows))
        self.assertEqual(soak_json['status'], 'passed')
        self.assertEqual(soak_json['observed_seconds'], 60)
        self.assertEqual(soak_json['requested_seconds'], 60)
        self.assertTrue(soak_json['samples'])
        self.assertTrue(all(s['pid_stable'] for s in soak_json['samples']))
        self.assertNotIn('secret-body', events_text)
        self.assertNotIn('secret-body', json.dumps(soak_json))

    def test_soak_detects_process_exit(self):
        clock = FakeClock()
        rows, soak_json, _ = run_soak_cycle(FakeDevice(clock, pid_after=(30, '')), clock, 120)
        soak_row = next(r for r in rows if r['test'] == 'soak')
        self.assertEqual(soak_row['status'], 'failed')
        self.assertEqual(soak_json['status'], 'failed')

    def test_soak_detects_changed_pid(self):
        clock = FakeClock()
        rows, soak_json, _ = run_soak_cycle(FakeDevice(clock, pid_after=(30, '43')), clock, 120)
        soak_row = next(r for r in rows if r['test'] == 'soak')
        self.assertEqual(soak_row['status'], 'failed')
        self.assertIn('process changed', soak_row['reason'])

    def test_soak_checks_final_interval_before_passing(self):
        for changes in ({'pid_after': (59, '')},
                        {'markers_after': (59, {'native-scheduler-unavailable'})}):
            clock = FakeClock()
            rows, progress, _ = run_soak_cycle(FakeDevice(clock, **changes), clock, 60)
            self.assertEqual(rows[-1]['status'], 'failed')
            self.assertEqual(progress['status'], 'failed')

    def test_soak_detects_native_late_failure(self):
        clock = FakeClock()
        device = FakeDevice(clock, markers_after=(30, {'native-scheduler-unavailable'}))
        rows, soak_json, events_text = run_soak_cycle(device, clock, 120)
        soak_row = next(r for r in rows if r['test'] == 'soak')
        self.assertEqual(soak_row['status'], 'failed')
        self.assertIn('native-scheduler-unavailable', events_text)

    def test_interrupted_soak_is_not_passed(self):
        for error in (runner.DeviceError('logcat down'), KeyboardInterrupt()):
            clock = FakeClock()
            device = FakeDevice(clock, raise_after=(30, error))
            rows, soak_json, _ = run_soak_cycle(device, clock, 120)
            soak_row = next(r for r in rows if r['test'] == 'soak')
            self.assertEqual(soak_row['status'], 'blocked')
            self.assertNotEqual(soak_row['status'], 'passed')
            self.assertEqual(soak_json['status'], 'blocked')
            self.assertLess(soak_json['observed_seconds'], 120)

    def test_soak_skipped_when_readiness_incomplete(self):
        # Missing account-session-verified leaves authentication blocked, so no soak runs.
        clock = FakeClock()
        device = FakeDevice(clock, markers=('native-bootstrap-ready', 'native-scheduler-ready'))
        rows, soak_json, _ = run_soak_cycle(device, clock, 60)
        self.assertNotIn('soak', [r['test'] for r in rows])
        self.assertIsNone(soak_json)

    def test_invalid_soak_duration_is_rejected(self):
        for value in ('-1', '43201', '99999'):
            with patch('sys.stderr'):
                with self.assertRaises(SystemExit):
                    runner.main(['--serial', 'fixture', '--soak-seconds', value])


if __name__ == '__main__':
    unittest.main()
