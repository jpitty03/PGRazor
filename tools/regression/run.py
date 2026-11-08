"""Explicit-device smoke regression runner. Standard library only; no root required."""
import argparse
import datetime as dt
import json
import re
import subprocess
import time
import uuid
from pathlib import Path

PACKAGE = 'com.nianticlabs.pokemongo'
ACTIVITY = PACKAGE + '/com.nianticproject.holoholo.libholoholo.unity.UnityMainActivity'
MARKERS = {
    'native-bootstrap-ready', 'native-scheduler-ready', 'account-session-verified',
    'native-bootstrap-unavailable', 'native-scheduler-unavailable',
}
DEFERRED = {
    'quick-catch': 'Needs a catchable encounter and inventory/result assertions.',
    'quick-spin': 'Needs an eligible nearby stop and inventory/result assertions.',
    'settings-persistence': 'Needs settings control and before/after readback.',
    'account-switch': 'Needs explicitly configured test profiles.',
    'backup-restore': 'Needs a disposable settings fixture and restoration checks.',
    'cutscene-skip': 'Needs a supported scene and skip completion evidence.',
    'routes': 'Needs an available route and progress/reward assertions.',
    'virtual-go-plus': 'Needs eligible targets and observed catch/spin outcomes.',
    'shundo-sniper': 'Needs opt-in travel/catch testing and a suitable encounter.',
    'combat': 'Needs a suitable battle and combat outcome assertions.',
    'transfer': 'Needs explicit destructive-test opt-in and disposable inventory.',
}


class DeviceError(RuntimeError):
    pass


class Adb:
    def __init__(self, executable, serial):
        self.prefix = [executable, '-s', serial]

    def call(self, *args, timeout=20):
        try:
            result = subprocess.run(self.prefix + list(args), capture_output=True, timeout=timeout)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise DeviceError(type(error).__name__) from error
        if result.returncode:
            # Never include arbitrary device output (it may contain authentication data).
            raise DeviceError('ADB command failed: ' + args[0])
        return result.stdout

    def shell(self, *args):
        return self.call('shell', *args).decode('utf-8', errors='replace').strip()


def evidence(text, pid):
    """Only fixed event labels from PGRazor in this process; discard event bodies."""
    found = set()
    for line in text.splitlines():
        match = re.match(r'^\s*\d+-\d+\s+\S+\s+(\d+)\s+\d+\s+[VDIWEF]\s+PGRazor\s*:\s+.*?\[([a-z-]+)\]', line)
        if match and match[1] == pid and match[2] in MARKERS:
            found.add(match[2])
    return found


def classify(markers):
    runtime = {'native-bootstrap-ready', 'native-scheduler-ready'} <= markers
    failed = bool({'native-bootstrap-unavailable', 'native-scheduler-unavailable'} & markers)
    return [
        {'test': 'native-runtime', 'status': 'failed' if failed else 'passed' if runtime else 'blocked',
         'reason': 'Native startup error observed.' if failed else 'Both native readiness events observed.' if runtime else 'Readiness evidence missing; unsupported/old build or startup incomplete.'},
        {'test': 'authenticated-session', 'status': 'passed' if 'account-session-verified' in markers else 'blocked',
         'reason': 'Game identity matched the active profile.' if 'account-session-verified' in markers else 'No verified session event; inspect screenshot. Old builds lack this event.'},
    ]


def cycle(adb, folder, timeout):
    # A timestamp fence avoids clearing the user's logcat buffer and excludes earlier launches.
    stamp = adb.shell('date', '+%m-%d_%H:%M:%S.000').replace('_', ' ')
    if not re.fullmatch(r'\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.000', stamp):
        raise DeviceError('Device clock unavailable')
    adb.shell('am', 'force-stop', PACKAGE)
    launch = adb.shell('am', 'start', '-W', '-n', ACTIVITY)
    if 'Error:' in launch or 'Exception' in launch:
        raise DeviceError('Game activity could not start')
    pid = adb.shell('pidof', PACKAGE)
    if not re.fullmatch(r'\d+', pid):
        raise DeviceError('Expected exactly one game process')
    markers = set()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        current = adb.shell('pidof', PACKAGE, '||', 'true')
        if current != pid:
            return [{'test': 'process-stability', 'status': 'failed', 'reason': 'Game process changed during observation.'}]
        # logcat is an adb command here; keep the timestamp as one argv element.
        logs = adb.call('logcat', '-d', '-v', 'threadtime', '-T', stamp, '-s', 'PGRazor:I').decode('utf-8', errors='replace')
        markers |= evidence(logs, pid)
        if all(row['status'] == 'passed' for row in classify(markers)):
            break
        if any('unavailable' in marker for marker in markers):
            break
        time.sleep(min(2, max(0, deadline - time.monotonic())))
    (folder / 'events.json').write_text(json.dumps(sorted(markers), indent=2) + '\n')
    return classify(markers)


def save_report(folder, report):
    rows = report['results']
    report['status'] = ('failed' if any(r['status'] == 'failed' for r in rows) else
                        'blocked' if any(r['status'] == 'blocked' for r in rows) else 'passed')
    (folder / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    lines = ['# Device regression', '', 'Overall: ' + report['status'], '',
             '| Cycle | Test | Result | Detail |', '|---|---|---|---|']
    for row in rows:
        values = [str(row.get('cycle', '-')), row['test'], row['status'], row['reason']]
        lines.append('| ' + ' | '.join(v.replace('|', '\\|').replace('\n', ' ') for v in values) + ' |')
    lines += ['', 'Screenshots may contain personal game information. Keep this directory private.',
              'A verified session does not prove that map rendering or every gameplay feature works.']
    (folder / 'report.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return 1 if report['status'] == 'failed' else 2 if report['status'] == 'blocked' else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--serial', required=True, help='Exact ADB serial; never auto-selects a device')
    parser.add_argument('--execute', action='store_true', help='Authorize stopping and launching the game on this device')
    parser.add_argument('--cycles', type=int, default=2)
    parser.add_argument('--timeout', type=int, default=90, help='Observation seconds per launch')
    parser.add_argument('--idle-seconds', type=int, default=0, help='Home-screen idle before subsequent cold launches')
    parser.add_argument('--output', type=Path, default=Path('.artifacts/device-regression'))
    args = parser.parse_args(argv)
    if not 1 <= args.cycles <= 100 or not 5 <= args.timeout <= 600 or not 0 <= args.idle_seconds <= 86400:
        parser.error('cycles must be 1..100, timeout 5..600, idle-seconds 0..86400')
    if not re.fullmatch(r'[A-Za-z0-9_.:\[\]-]+', args.serial):
        parser.error('Invalid device serial')
    if not args.execute:
        print(json.dumps({'device': args.serial, 'cycles': args.cycles, 'actions': ['force-stop', 'launch', 'observe events', 'screenshot'],
                          'unimplemented': DEFERRED, 'note': 'Plan only. Add --execute to run.'}, indent=2))
        return 0
    folder = args.output / (dt.datetime.now(dt.timezone.utc).strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8])
    folder.mkdir(parents=True)
    adb = Adb(args.adb, args.serial)
    report = {'schema': 1, 'serial': args.serial, 'started_utc': dt.datetime.now(dt.timezone.utc).isoformat(),
              'configuration': {'cycles': args.cycles, 'timeout': args.timeout, 'idle_seconds': args.idle_seconds}, 'results': []}
    try:
        if adb.call('get-state').strip() != b'device':
            raise DeviceError('Device is not online/authorized')
        report['device'] = {name: adb.shell('getprop', prop) for name, prop in
                            [('model', 'ro.product.model'), ('abi', 'ro.product.cpu.abi'), ('android', 'ro.build.version.release')]}
        package = adb.shell('dumpsys', 'package', PACKAGE)
        report['game_version'] = re.findall(r'\bversion(?:Name|Code)=[^\s]+', package)
        if not report['game_version']:
            raise DeviceError('Game package not installed')
        for number in range(1, args.cycles + 1):
            if number > 1 and args.idle_seconds:
                adb.shell('input', 'keyevent', 'KEYCODE_HOME')
                for elapsed in range(0, args.idle_seconds, 30):
                    print('Idle renewal check:', elapsed, '/', args.idle_seconds, flush=True)
                    time.sleep(min(30, args.idle_seconds - elapsed))
            case = folder / f'cycle-{number}'
            case.mkdir()
            print('Running cycle', number, 'on', args.serial, flush=True)
            try:
                rows = cycle(adb, case, args.timeout)
            except DeviceError as error:
                rows = [{'test': 'device-execution', 'status': 'blocked', 'reason': str(error)}]
            for row in rows:
                row['cycle'] = number
            report['results'].extend(rows)
            try:
                png = adb.call('exec-out', 'screencap', '-p')
                if not png.startswith(b'\x89PNG\r\n\x1a\n'):
                    raise DeviceError('Screenshot was not PNG')
                (case / 'screen.png').write_bytes(png)
            except DeviceError as error:
                report['results'].append({'cycle': number, 'test': 'screenshot', 'status': 'blocked', 'reason': str(error)})
            save_report(folder, report)
            if any(row['status'] != 'passed' for row in rows):
                break
    except (DeviceError, KeyboardInterrupt) as error:
        report['results'].append({'test': 'preflight-or-interruption', 'status': 'blocked', 'reason': str(error) or 'Interrupted'})
    for name, reason in DEFERRED.items():
        report['results'].append({'test': name, 'status': 'skipped', 'reason': reason})
    report['completed_utc'] = dt.datetime.now(dt.timezone.utc).isoformat()
    code = save_report(folder, report)
    print('Report:', folder / 'report.md', flush=True)
    return code


if __name__ == '__main__':
    raise SystemExit(main())
