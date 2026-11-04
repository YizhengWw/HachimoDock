"""Focused pure contract checks; no compiler, credentials, network or device."""
import copy
import json
import unittest
from pathlib import Path
import bounded_app
import data_rows
import validate_generated_widget as validator

SUITE = json.loads(Path(__file__).parent.parent.joinpath('contracts/pc-contract-regressions-v1.json').read_text())

class PcContractRegressions(unittest.TestCase):
    def test_scene_guard_slots_match_complete_runtime_validator(self):
        for case in SUITE['sceneGuards']:
            with self.subTest(case=case['name']):
                errors = []
                # A v3 authoring target must explicitly negotiate v3. The
                # omitted-capability negative below keeps that gate intact.
                capabilities = {**validator.DEFAULT_CAPABILITIES, 'widgetQuota': case['runtime']['quota']}
                validator.validate_runtime(case['runtime'], 'game', Path('.'), capabilities, errors)
                self.assertEqual(not errors, case['expectedOk'], errors)
                if errors:
                    self.assertTrue(any('int' in e for e in errors), errors)

    def test_quota_v3_still_requires_explicit_target_capability(self):
        runtime = next(c['runtime'] for c in SUITE['sceneGuards'] if c['expectedOk'] and c['runtime']['quota']=='p4-quota-v3')
        errors = []
        validator.validate_runtime(runtime, 'game', Path('.'), validator.DEFAULT_CAPABILITIES, errors)
        self.assertTrue(any('widgetQuota=p4-quota-v3' in error for error in errors), errors)

    def test_navigation_actual_branch_and_from_page(self):
        for case in SUITE['navigation']:
            with self.subTest(case=case['name']):
                runtime = case['runtime']
                errors = []
                validator.validate_runtime(runtime, 'tool', Path('.'), validator.DEFAULT_CAPABILITIES, errors)
                self.assertEqual(errors, [])
                state = bounded_app.initial(runtime)
                state['history'] = list(case['initialHistory'])
                before = copy.deepcopy(state)
                result = bounded_app.action(runtime, state, case['action'])
                expected = case['expected']
                self.assertEqual((result['page'], result['vars']['hits'], result.get('history', [])),
                                 (expected['page'], expected['hits'], expected['history']))
                self.assertEqual(state, before)

    def test_data_projection_matches_filtered_pages(self):
        for case in SUITE['dataPaging']:
            with self.subTest(case=case['name']):
                runtime = case['runtime']
                variables = {k: v['init'] for k, v in runtime['vars'].items()}
                view = data_rows.view(case['rows'], {'filters': runtime['data']['filters']}, variables, 'page')
                expected = case['expected']
                self.assertEqual((view['count'], view['pages'], view['page']),
                                 (expected['count'], expected['pages'], expected['page']))

if __name__ == '__main__':
    unittest.main()
