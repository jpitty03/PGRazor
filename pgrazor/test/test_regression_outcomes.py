import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('outcomes', Path(__file__).resolve().parents[2] / 'tools/regression/outcomes.py')
outcomes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(outcomes)


class OutcomeTests(unittest.TestCase):
    def line(self, payload, pid=42, tag='PGRazor', event='vgp-result'):
        return f'11-09 06:00:00.000 {pid} 43 I {tag} : 2026-11-09T06:00:00Z [{event}] {payload}'

    def test_sanitizes_server_response(self):
        rows = outcomes.parse_results(self.line('{"action":"catch","success":true,"status":1,"trainer":"private","token":"secret"}'), 42)
        self.assertEqual(rows, [{'event':'vgp-result','action':'catch','success':True,'status':1}])

    def test_foreign_process_tag_and_nested_marker_rejected(self):
        body = '{"action":"catch","success":true,"status":1}'
        text = '\n'.join([self.line(body, pid=41), self.line(body, tag='Other'), self.line('[vgp-result] '+body, event='unrelated')])
        self.assertEqual(outcomes.parse_results(text, 42), [])

    def test_invalid_json_and_coerced_values_rejected(self):
        for data in ['[]', 'null', '{', '{"action":"catch","success":"true","status":1}',
                     '{"action":"catch","success":true,"status":true}',
                     '{"action":"transfer","success":true,"status":1}']:
            self.assertEqual(outcomes.parse_results(self.line(data), 42), [])

    def test_server_success_alone_does_not_pass_e2e(self):
        events = outcomes.parse_results(self.line('{"action":"catch","success":true,"status":1}'), 42)
        self.assertEqual(outcomes.assess_action(events, 'catch')['status'], 'blocked')
        self.assertEqual(outcomes.assess_action(events, 'spin', inventory_confirmed=True)['status'], 'blocked')
        self.assertEqual(outcomes.assess_action(events, 'catch', inventory_confirmed=True)['status'], 'passed')

    def test_failed_game_outcome_not_automatically_feature_defect(self):
        events = outcomes.parse_results(self.line('{"action":"catch","success":false,"status":3}'), 42)
        self.assertEqual(outcomes.assess_action(events, 'catch', inventory_confirmed=True)['status'], 'blocked')

    def test_native_catch_outcome_requires_storage_and_matching_source(self):
        payload = {'kind':'outcome-confirmed','backend':'native','status':1,
                   'outcome':'caught','tid':123,'encounter':'private'}
        events = outcomes.parse_results(self.line(json.dumps(payload), event='quick-ball-outcome'), 42)
        self.assertEqual(events, [{'event':'quick-ball-outcome','action':'catch',
                                  'success':True,'status':1,'outcome':'caught'}])
        self.assertEqual(outcomes.assess_action(events, 'catch', event='quick-ball-outcome')['status'], 'blocked')
        self.assertEqual(outcomes.assess_action(events, 'catch', inventory_confirmed=True)['status'], 'blocked')
        self.assertEqual(outcomes.assess_action(events, 'catch', event='quick-ball-outcome', inventory_confirmed=True)['status'], 'passed')
        vgp = outcomes.parse_results(self.line('{"action":"catch","success":true,"status":1}'), 42)
        self.assertEqual(outcomes.assess_action(vgp, 'catch', event='quick-ball-outcome', inventory_confirmed=True)['status'], 'blocked')

    def test_native_observation_is_not_confirmation(self):
        base = {'kind':'outcome-confirmed','backend':'native','status':1,'outcome':'caught'}
        for change in ({'kind':'outcome-suppressed'}, {'kind':'outcome-unavailable'},
                       {'kind':'catch-observer-complete'}, {'backend':'other'},
                       {'status':True}, {'status':'1'}, {'outcome':None}, {'outcome':'unknown'}):
            with self.subTest(change=change):
                self.assertEqual(outcomes.parse_results(self.line(json.dumps(base | change), event='quick-ball-outcome'), 42), [])
        self.assertEqual(outcomes.parse_results(self.line(json.dumps(base), event='quick-ball'), 42), [])
        self.assertEqual(outcomes.parse_results(self.line(json.dumps(base), pid=41, event='quick-ball-outcome'), 42), [])

    def test_native_escape_and_flee_are_not_catches(self):
        for result in ('broke-free', 'fled'):
            payload = {'kind':'outcome-confirmed','backend':'native','status':2,'outcome':result}
            events = outcomes.parse_results(self.line(json.dumps(payload), event='quick-ball-outcome'), 42)
            self.assertEqual(len(events), 1)
            self.assertFalse(events[0]['success'])
            self.assertEqual(outcomes.assess_action(events, 'catch', event='quick-ball-outcome', inventory_confirmed=True)['status'], 'blocked')

    def test_compiled_vgp_channel_is_normalized_only_for_confirmed_result_kind(self):
        base = {'kind':'vgp-result','backend':'native','action':'spin','success':True,'status':0}
        events = outcomes.parse_results(self.line(json.dumps(base), event='quick-close'), 42)
        self.assertEqual(events, [{'event':'vgp-result','action':'spin','success':True,'status':0}])
        for change in ({'kind':'vgp-request'}, {'kind':'vgp-marker'}, {'backend':'other'}, {'success':'true'}):
            self.assertEqual(outcomes.parse_results(self.line(json.dumps(base | change), event='quick-close'), 42), [])


if __name__ == '__main__':
    unittest.main()
