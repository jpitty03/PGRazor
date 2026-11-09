"""Opt-in live inventory freshness and real settings serializer/import checks."""
import argparse
import datetime as dt
import json
from pathlib import Path
import re
import uuid

import settings as s


def execute(bridge, feature):
    before = bridge.snapshot()
    if feature == 'inventory':
        first, second = bridge.inventory(), bridge.inventory()
        checks = {
            'sameProcess': before['pid'] == first['pid'] == second['pid'],
            'freshReadAcknowledged': second['inventoryRevision'] > first['inventoryRevision'],
            'settingsUnchanged': before['settings'] == first['settings'] == second['settings'],
        }
    else:
        result = bridge.backup(recover=feature == 'backup-recover')
        checks = dict(result['backup'])
        checks['sameProcess'] = before['pid'] == result['pid']
        if feature != 'backup-recover':
            checks['settingsUnchanged'] = before['settings'] == result['settings']
    return {'status': 'passed' if all(checks.values()) else 'failed', 'checks': checks}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--feature', required=True, choices=['inventory','backup','backup-recover'])
    parser.add_argument('--execute', action='store_true')
    parser.add_argument('--output', type=Path, default=Path('.artifacts/feature-regression'))
    args = parser.parse_args(argv)
    if not re.fullmatch(r'[A-Za-z0-9_.:\[\]-]+', args.serial):
        parser.error('Invalid device serial')
    if not args.execute:
        print(json.dumps({'feature':args.feature,'serial':args.serial,'planOnly':True,
                          'requires':'Private regression bridge; explicit --execute to run.'}))
        return 0
    folder = args.output / (dt.datetime.now(dt.timezone.utc).strftime('%Y%m%d-%H%M%S')+'-'+uuid.uuid4().hex[:8])
    folder.mkdir(parents=True)
    report = {'schema':1,'feature':args.feature,'serial':args.serial}
    try:
        adb = s.Adb(args.adb, args.serial)
        bridge = s.Bridge(adb, s._preflight(adb))
        report.update(execute(bridge, args.feature))
    except (s.SettingsError, s.DeviceError) as error:
        report.update(status='blocked', reason=s._safe_code(error))
    report['completedUtc'] = dt.datetime.now(dt.timezone.utc).isoformat()
    (folder/'report.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
    print('Report:',folder/'report.json')
    return {'passed':0,'failed':1,'blocked':2}[report['status']]


if __name__ == '__main__':
    raise SystemExit(main())
