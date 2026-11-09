"""Rooted settings-persistence regression adapter for an explicit ADB serial.

Mutates only the joystick-visibility setting, verifies it survives a cold restart,
then always restores the original value. Standard library only. Requires a device
where non-interactive ``su`` already works and the test APK bridge is enabled with
the asset ``pgrazor/regression-v1.enabled=v1``; this host never grants or guesses
root and never selects a device automatically.

The runner talks to the in-game bridge through two app-private files:

    /data/data/com.nianticlabs.pokemongo/files/pgrazor-regression/request.json
    /data/data/com.nianticlabs.pokemongo/files/pgrazor-regression/response.json

Requests are JSON ``{"id": <uuid-hex>, "op": "snapshot"}`` or
``{"id": <uuid-hex>, "op": "set-joystick", "value": <bool>}``. The bridge answers
with ``{"id", "pid", "ok", "settings": {joystickVisible, quickSpin, autoTransfer},
"error"}``. Only fixed status codes are ever saved; raw shell output and the
concrete setting values are never written to the sanitized result.
"""
import argparse
import datetime as dt
import importlib.util
import json
import re
import shlex
import tempfile
import time
import uuid
from pathlib import Path

# Reuse run.py's validated Adb helper and shared constants without importing by name
# (tools/regression is not a package, and this module must load whether run as a
# script or imported by the host unit tests).
_RUN = Path(__file__).resolve().parent / 'run.py'
_spec = importlib.util.spec_from_file_location('regression_run', _RUN)
run = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(run)

Adb = run.Adb
DeviceError = run.DeviceError
PACKAGE = run.PACKAGE
ACTIVITY = run.ACTIVITY

APP_DATA = '/data/data/' + PACKAGE
APP_DIR = APP_DATA + '/files/pgrazor-regression'
REQUEST = APP_DIR + '/request.json'
REQUEST_TMP = APP_DIR + '/request.tmp'
RESPONSE = APP_DIR + '/response.json'
TMP_PREFIX = '/data/local/tmp/pgrazor-regression-'
SETTING_KEYS = ('joystickVisible', 'quickSpin', 'autoTransfer')
HEX32 = re.compile(r'[0-9a-f]{32}')
DIGITS = re.compile(r'\d+')
ERROR_CODE = re.compile(r'[a-z][a-z0-9-]{0,39}')
SAFE_CODE = re.compile(r'[A-Za-z0-9 :._-]{1,80}')


class SettingsError(RuntimeError):
    """Device, protocol, or timeout problem. Carries only a fixed status code."""


class FeatureFailure(Exception):
    """A verified mismatch: the feature behaved incorrectly (not a device fault)."""

    def __init__(self, step, code):
        super().__init__(code)
        self.step = step
        self.code = code


def _safe_code(error):
    """Collapse any exception text to a short fixed code; never leak raw output."""
    text = str(error)
    return text if SAFE_CODE.fullmatch(text) else 'error'


class Bridge:
    """Writes requests and reads responses for the in-app bridge over ADB.

    ``adb`` is a run.Adb (or a compatible fake). ``uid`` is the app's numeric owner,
    captured during preflight and validated as digits so the fixed ``su`` command
    interpolates nothing attacker-controlled. ``clock`` supplies monotonic()/sleep()
    so host tests can drive timeouts without real waits.
    """

    def __init__(self, adb, uid, clock=time, timeout=30.0, poll_seconds=1.0):
        if not DIGITS.fullmatch(str(uid)):
            raise SettingsError('bad-app-uid')
        self.adb = adb
        self.uid = str(uid)
        self.clock = clock
        self.timeout = timeout
        self.poll_seconds = poll_seconds

    def snapshot(self):
        return self._exchange('snapshot', None)

    def inventory(self):
        response = self._exchange('inventory', None)
        if 'inventory' not in response:
            raise SettingsError('inventory-missing')
        return response

    def backup(self, recover=False):
        response = self._exchange('backup-recover' if recover else 'backup-roundtrip', None)
        keys = ('restored',) if recover else ('alwaysHitIncluded','mutated','settingsMatch','preferencesMatch','speedMatch','restored','ok')
        checks = response.get('backup')
        if not isinstance(checks, dict) or set(checks) != set(keys) or any(type(v) is not bool for v in checks.values()):
            raise SettingsError('bad-backup-checks')
        return response

    def set_joystick(self, value):
        if type(value) is not bool:
            raise SettingsError('bad-setting-type')
        return self._exchange('set-joystick', value)

    # -- internals ---------------------------------------------------------

    def _exchange(self, op, value):
        request_id = uuid.uuid4().hex
        token = uuid.uuid4().hex
        if not HEX32.fullmatch(request_id) or not HEX32.fullmatch(token):
            raise SettingsError('bad-id')
        command = {'id': request_id, 'op': op}
        if op == 'set-joystick':
            command['value'] = bool(value)
        payload = json.dumps(command, separators=(',', ':'))
        remote_tmp = TMP_PREFIX + token + '.json'
        with tempfile.TemporaryDirectory() as directory:
            local = Path(directory) / 'request.json'
            local.write_text(payload, encoding='utf-8')
            self._call('push', str(local), remote_tmp)
            try:
                return self._poll(remote_tmp, request_id)
            finally:
                # Best-effort cleanup of the staged copy; failure never masks a result.
                try:
                    self._su('rm -f ' + remote_tmp)
                except SettingsError:
                    pass

    def _deliver_command(self, remote_tmp):
        # Fixed paths plus a validated numeric uid and hex tmp name: nothing here is
        # attacker-controlled, and the atomic mv means the bridge never sees a partial
        # request. restorecon is tolerated-if-missing but must not downgrade the chain.
        return (
            'cp ' + remote_tmp + ' ' + REQUEST_TMP + ' && '
            'chown ' + self.uid + ':' + self.uid + ' ' + REQUEST_TMP + ' && '
            'chmod 600 ' + REQUEST_TMP + ' && '
            '{ restorecon ' + REQUEST_TMP + ' 2>/dev/null || true; } && '
            'mv ' + REQUEST_TMP + ' ' + REQUEST)

    def _poll(self, remote_tmp, request_id):
        deliver = self._deliver_command(remote_tmp)
        deadline = self.clock.monotonic() + self.timeout
        expected_pid = self._pid()
        if not expected_pid:
            raise SettingsError('process-missing')
        # Publish once. Re-delivery can leave a second mutation queued after success
        # or replay it across a restart, violating the bridge's consume-once contract.
        self._su(deliver)
        while True:
            current_pid = self._pid()
            if current_pid != expected_pid:
                raise SettingsError('process-changed')
            text = self._su('cat ' + RESPONSE + ' 2>/dev/null || true')
            parsed = self._match(text, request_id)
            if parsed is not None:
                response = self._validate(parsed, current_pid)
                if self._pid() != expected_pid:
                    raise SettingsError('process-changed')
                return response
            if self.clock.monotonic() >= deadline:
                raise SettingsError('response-timeout')
            self.clock.sleep(min(self.poll_seconds, max(0.0, deadline - self.clock.monotonic())))

    @staticmethod
    def _match(text, request_id):
        text = (text or '').strip()
        if not text:
            return None
        try:
            data = json.loads(text)
        except ValueError:
            return None
        if not isinstance(data, dict) or data.get('id') != request_id:
            # Stale or foreign-by-id responses are ignored so polling can continue.
            return None
        return data

    def _validate(self, data, current_pid):
        ok = data.get('ok')
        if type(ok) is not bool:
            raise SettingsError('bad-response')
        if not ok:
            error = data.get('error')
            code = error if isinstance(error, str) and ERROR_CODE.fullmatch(error) else 'unknown'
            raise SettingsError('bridge-' + code)
        pid = data.get('pid')
        if isinstance(pid, bool) or not isinstance(pid, (int, str)):
            raise SettingsError('bad-pid')
        pid = str(pid)
        if not DIGITS.fullmatch(pid):
            raise SettingsError('bad-pid')
        if not current_pid or pid != current_pid:
            # A response whose pid is not the live game process is never trusted.
            raise SettingsError('pid-mismatch')
        settings = data.get('settings')
        if not isinstance(settings, dict):
            raise SettingsError('bad-settings')
        result = {}
        for key in SETTING_KEYS:
            value = settings.get(key)
            if type(value) is not bool:
                raise SettingsError('bad-setting-type')
            result[key] = value
        response = {'pid': pid, 'settings': result}
        if 'backup' in data:
            response['backup'] = data['backup']
        if 'inventory' in data:
            counts, revision = data['inventory'], data.get('inventoryRevision')
            if not isinstance(counts, dict) or len(counts) > 1024 or type(revision) is not int or revision <= 0:
                raise SettingsError('bad-inventory')
            if any(not re.fullmatch(r'[1-9][0-9]{0,8}', key) or type(value) is not int or value < 0
                   for key, value in counts.items()):
                raise SettingsError('bad-inventory')
            response.update(inventory=counts, inventoryRevision=revision)
        return response

    def _call(self, *args):
        try:
            return self.adb.call(*args)
        except DeviceError as error:
            raise SettingsError('device-error') from error

    def _su(self, command):
        try:
            # adb joins shell arguments: quote the entire compound command so &&
            # and redirections execute inside su, not in the unprivileged shell.
            return self.adb.shell('su', '-c', shlex.quote(command))
        except DeviceError as error:
            raise SettingsError('device-error') from error

    def _pid(self):
        try:
            pid = self.adb.shell('pidof', PACKAGE)
        except DeviceError as error:
            raise SettingsError('device-error') from error
        return pid if DIGITS.fullmatch(pid or '') else ''


def _row(test, status, reason):
    return {'test': test, 'status': status, 'reason': reason}


def _assert_preserved(original, current, expected_joystick, step):
    """The joystick must equal the expected value and nothing else may drift."""
    if current['joystickVisible'] != expected_joystick:
        raise FeatureFailure(step, 'joystick-value-wrong')
    for key in ('quickSpin', 'autoTransfer'):
        if current[key] != original[key]:
            raise FeatureFailure(step, 'unrelated-setting-changed')


def _write_recovery(path, original_joystick):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({
        'schema': 1,
        'setting': 'joystickVisible',
        'original': bool(original_joystick),
        'note': 'Automatic restore failed if this file remains. Re-run set-joystick with "original" to recover.',
    }, indent=2) + '\n', encoding='utf-8')


def _restore(bridge, original, original_joystick, recovery_path, rows):
    """Always attempt restoration and verify it; a failure is never a success."""
    try:
        bridge.set_joystick(original_joystick)
        back = bridge.snapshot()
        _assert_preserved(original, back['settings'], original_joystick, 'restoration')
    except (SettingsError, FeatureFailure):
        rows.append(_row('restoration', 'failed',
                         'Could not confirm the original settings were restored; recovery.json retained.'))
        return
    rows.append(_row('restoration', 'passed', 'Original settings restored and verified by readback.'))
    try:
        recovery_path.unlink()
    except OSError:
        pass


def execute(bridge, restart, recovery_path):
    """Run the persistence check and return sanitized result rows.

    ``restart(old_pid) -> new_pid`` cold-restarts the game and returns the new,
    different process id, raising SettingsError if it cannot. ``recovery_path`` is
    written with the original value before any mutation and removed only once
    restoration is verified. Settings are restored in a finally after any error that
    follows the mutation; a failed or unverifiable restoration is reported failed and
    never upgraded to passed.
    """
    rows = []
    try:
        original_response = bridge.snapshot()
    except SettingsError as error:
        # Initial snapshot failure means the bridge is unavailable: no mutation at all.
        return [_row('initial-snapshot', 'blocked', _safe_code(error))]
    rows.append(_row('initial-snapshot', 'passed', 'Captured the original settings from the live process.'))

    original = original_response['settings']
    original_joystick = original['joystickVisible']
    target = not original_joystick
    try:
        _write_recovery(recovery_path, original_joystick)
    except OSError:
        return rows + [_row('recovery-file', 'blocked', 'recovery-file-unwritable')]

    mutated = True  # We are committing to the mutation; finally must restore from here.
    phase = 'mutation-readback'
    try:
        bridge.set_joystick(target)
        after = bridge.snapshot()
        if after['pid'] != original_response['pid']:
            raise SettingsError('process-changed')
        _assert_preserved(original, after['settings'], target, 'mutation-readback')
        rows.append(_row('mutation-readback', 'passed',
                         'Runtime readback showed the joystick change with other settings preserved.'))

        phase = 'cold-restart'
        new_pid = restart(after['pid'])
        if not DIGITS.fullmatch(str(new_pid)) or str(new_pid) == after['pid']:
            raise SettingsError('restart-no-new-process')
        rows.append(_row('cold-restart', 'passed', 'Game cold-restarted into a new process (pid ' + new_pid + ').'))

        phase = 'persistence'
        persisted = bridge.snapshot()
        if persisted['pid'] != str(new_pid):
            raise SettingsError('process-changed')
        _assert_preserved(original, persisted['settings'], target, 'persistence')
        rows.append(_row('persistence', 'passed',
                         'Joystick change persisted across the cold restart with other settings preserved.'))
    except FeatureFailure as error:
        rows.append(_row(error.step, 'failed', error.code))
    except SettingsError as error:
        rows.append(_row(phase, 'blocked', _safe_code(error)))
    except KeyboardInterrupt:
        rows.append(_row(phase, 'blocked', 'interrupted'))
    finally:
        if mutated:
            _restore(bridge, original, original_joystick, recovery_path, rows)
    return rows


def _overall(rows):
    return ('failed' if any(r['status'] == 'failed' for r in rows) else
            'blocked' if any(r['status'] == 'blocked' for r in rows) else 'passed')


def save_report(folder, report):
    rows = report['results']
    report['status'] = _overall(rows)
    (folder / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    lines = ['# Settings persistence regression', '', 'Overall: ' + report['status'], '',
             '| Test | Result | Detail |', '|---|---|---|']
    for row in rows:
        cells = [row['test'], row['status'], row['reason']]
        lines.append('| ' + ' | '.join(c.replace('|', '\\|').replace('\n', ' ') for c in cells) + ' |')
    lines += ['', 'Only joystick visibility is mutated; quickSpin and autoTransfer are verified unchanged.',
              'A passed result requires the mutation, cold-restart persistence, and restoration to all be verified.',
              'If restoration was not verified, recovery.json holds the original value for manual recovery.']
    (folder / 'report.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return 1 if report['status'] == 'failed' else 2 if report['status'] == 'blocked' else 0


def summarize(rows):
    """One-line completion summary distinguishing blocked preflight from feature failure."""
    status = _overall(rows)
    if status == 'passed':
        return 'PASSED: joystick change mutated, persisted across a cold restart, and was restored.'
    if status == 'failed':
        if any(r['test'] == 'restoration' and r['status'] == 'failed' for r in rows):
            return 'FAILED: settings restoration could not be verified; recovery.json retained.'
        return 'FAILED: the settings-persistence feature check did not hold.'
    if any(r['test'] == 'preflight' for r in rows):
        return 'BLOCKED: device prerequisites were not met; no settings were changed.'
    if any(r['test'] == 'initial-snapshot' and r['status'] == 'blocked' for r in rows):
        return 'BLOCKED: the in-app bridge did not answer; no settings were changed.'
    return 'BLOCKED: a device or bridge error interrupted the run; settings were restored.'


def _make_restart(adb, restart_timeout):
    def restart(old_pid):
        try:
            adb.shell('am', 'force-stop', PACKAGE)
            launch = adb.shell('am', 'start', '-W', '-n', ACTIVITY)
        except DeviceError as error:
            raise SettingsError('restart-failed') from error
        if 'Error:' in launch or 'Exception' in launch:
            raise SettingsError('restart-failed')
        deadline = time.monotonic() + restart_timeout
        while time.monotonic() < deadline:
            try:
                pid = adb.shell('pidof', PACKAGE)
            except DeviceError:
                pid = ''
            if DIGITS.fullmatch(pid or '') and pid != old_pid:
                return pid
            time.sleep(1.0)
        raise SettingsError('restart-no-new-process')
    return restart


def _capture_screenshot(adb, folder, rows):
    try:
        png = adb.call('exec-out', 'screencap', '-p')
        if not png.startswith(b'\x89PNG\r\n\x1a\n'):
            raise DeviceError('Screenshot was not PNG')
        (folder / 'screen.png').write_bytes(png)
    except DeviceError as error:
        rows.append(_row('screenshot', 'blocked', _safe_code(error)))


def _preflight(adb):
    """Return the app's numeric uid, or raise SettingsError with a fixed code.

    Never grants or guesses root: if non-interactive su is unavailable this blocks.
    """
    if adb.call('get-state').strip() != b'device':
        raise SettingsError('device-offline')
    if adb.shell('su', '-c', 'id -u') != '0':
        raise SettingsError('root-unavailable')
    package = adb.shell('dumpsys', 'package', PACKAGE)
    if not re.findall(r'\bversion(?:Name|Code)=\S+', package):
        raise SettingsError('package-missing')
    uid = adb.shell('su', '-c', 'stat -c %u ' + APP_DATA)
    if not DIGITS.fullmatch(uid or ''):
        raise SettingsError('app-uid-unavailable')
    return uid


def main(argv=None):
    parser = argparse.ArgumentParser(description='Rooted joystick settings-persistence regression for one ADB serial.')
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--serial', required=True, help='Exact ADB serial; never auto-selects a device')
    parser.add_argument('--execute', action='store_true',
                        help='Authorize mutating the joystick setting and cold-restarting the game')
    parser.add_argument('--timeout', type=float, default=30.0, help='Seconds to await each bridge response')
    parser.add_argument('--poll', type=float, default=1.0, help='Seconds between bridge response polls')
    parser.add_argument('--restart-timeout', type=float, default=120.0, help='Seconds to await a new process after restart')
    parser.add_argument('--screenshot', action='store_true', help='Capture a screenshot after the run (may contain game info)')
    parser.add_argument('--output', type=Path, default=Path('.artifacts/settings-regression'))
    args = parser.parse_args(argv)

    if not re.fullmatch(r'[A-Za-z0-9_.:\[\]-]+', args.serial):
        parser.error('Invalid device serial')
    if not 5 <= args.timeout <= 600 or not 0 < args.poll <= 10 or not 5 <= args.restart_timeout <= 600:
        parser.error('timeout 5..600, poll 0..10, restart-timeout 5..600')

    if not args.execute:
        actions = ['snapshot settings', 'set joystick to the opposite value', 'verify runtime readback',
                   'cold restart the game', 'verify persistence', 'restore the original', 'verify restoration']
        print(json.dumps({'device': args.serial, 'mutates': ['joystickVisible'],
                          'preserves': ['quickSpin', 'autoTransfer'],
                          'requires': ['non-interactive su (root not granted by this tool)',
                                       'test bridge APK built with pgrazor/regression-v1.enabled=v1'],
                          'actions': actions, 'note': 'Plan only. Add --execute to run.'}, indent=2))
        return 0

    folder = args.output / (dt.datetime.now(dt.timezone.utc).strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8])
    folder.mkdir(parents=True)
    recovery_path = folder / 'recovery.json'
    adb = Adb(args.adb, args.serial)
    report = {'schema': 1, 'kind': 'settings-persistence', 'serial': args.serial,
              'started_utc': dt.datetime.now(dt.timezone.utc).isoformat(), 'results': []}
    try:
        uid = _preflight(adb)
    except (SettingsError, DeviceError) as error:
        report['results'] = [_row('preflight', 'blocked', _safe_code(error))]
    else:
        bridge = Bridge(adb, uid, timeout=args.timeout, poll_seconds=args.poll)
        rows = execute(bridge, _make_restart(adb, args.restart_timeout), recovery_path)
        if args.screenshot:
            _capture_screenshot(adb, folder, rows)
        report['results'] = rows

    report['completed_utc'] = dt.datetime.now(dt.timezone.utc).isoformat()
    code = save_report(folder, report)
    print(summarize(report['results']))
    print('Report:', folder / 'report.md', flush=True)
    return code


if __name__ == '__main__':
    raise SystemExit(main())
