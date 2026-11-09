"""Sanitized server-result evidence for gameplay adapters (not inventory proof)."""
import json
import re


LINE = re.compile(r'^\s*\d+-\d+\s+\S+\s+(\d+)\s+\d+\s+[VDIWEF]\s+PGRazor\s*:\s+\S+\s+\[([a-z-]+)\]\s+(.*)$')


def parse_results(text, pid):
    """Discard all raw payloads and account/encounter identifiers.

    Caller must fence logcat by the test's start timestamp and check process
    continuity. These events establish server response outcomes, not inventory
    reconciliation, screen rendering, or which caller initiated the action.
    """
    result = []
    for line in text.splitlines():
        match = LINE.fullmatch(line)
        if not match or match[1] != str(pid) or match[2] not in ('vgp-result', 'quick-ball-outcome', 'quick-close'):
            continue
        try:
            data = json.loads(match[3])
        except (ValueError, TypeError):
            continue
        if not isinstance(data, dict):
            continue
        # Compiled encounter-host telemetry wraps VGP payloads in quick-close.
        if match[2] == 'quick-close' and (data.get('kind') != 'vgp-result' or data.get('backend') != 'native'):
            continue
        if match[2] == 'quick-ball-outcome':
            if (data.get('kind') != 'outcome-confirmed' or data.get('backend') != 'native'
                    or data.get('outcome') not in ('caught', 'broke-free', 'fled')
                    or type(data.get('status')) is not int):
                continue
            result.append({'event': 'quick-ball-outcome', 'action': 'catch',
                           'success': data['outcome'] == 'caught', 'status': data['status'],
                           'outcome': data['outcome']})
            continue
        if data.get('action') not in ('catch', 'spin') or type(data.get('success')) is not bool:
            continue
        if type(data.get('status')) is not int:
            continue
        result.append({'event': 'vgp-result', 'action': data['action'],
                       'success': data['success'], 'status': data['status']})
    return result


def assess_action(events, action, *, inventory_confirmed=False, event='vgp-result'):
    """No-target/flee/refusal cannot by themselves establish a feature defect."""
    if action not in ('catch', 'spin'):
        raise ValueError('Unsupported action')
    if event not in ('vgp-result', 'quick-ball-outcome'):
        raise ValueError('Unsupported event source')
    matches = [e for e in events if e['event'] == event and e['action'] == action and e['success'] is True]
    if not matches:
        return {'status': 'blocked', 'reason': 'No successful matching server result observed.'}
    if not inventory_confirmed:
        return {'status': 'blocked', 'reason': 'Server success observed; inventory reconciliation remains unverified.'}
    return {'status': 'passed', 'reason': 'Server success and inventory reconciliation verified.'}
