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


if __name__ == '__main__':
    unittest.main()
