"""PC package contract regression; no network/device/user storage."""
import copy
import json
import sys
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pc/skills/petui/scripts'))
from location_profile import validate_location_profile
from app_services import application_errors

class Contract(unittest.TestCase):
    def setUp(self):
        self.profile=json.loads((ROOT/'pc/component-profiles/weather.json').read_text())
    def test_profile_and_v2_gate(self):
        validate_location_profile(self.profile)
        runtime={'host':True,'vars':{},'surface':{'engine':'p4-app-surface-v1'},'transitions':[{'from':'*','on':'host.refresh'}]}
        app={'schemaVersion':1,'engine':'pc-app-services-v2','service':'weather','actions':{'host.refresh':'refresh'},'location':self.profile}
        self.assertEqual(application_errors(app,runtime),[])
        app['engine']='pc-app-services-v1';self.assertTrue(application_errors(app,runtime))
        del app['location'];self.assertEqual(application_errors(app,runtime),[])
        app['engine']='pc-app-services-v2';self.assertTrue(application_errors(app,runtime))
    def test_bad_urls_scripts_and_metadata(self):
        for endpoint in ['http://example.com/','https://127.0.0.1/','https://x.local/','https://user:pass@example.com/','https://example.com:444/','https://example.com/?secret=x']:
            with self.subTest(endpoint=endpoint):
                p=copy.deepcopy(self.profile);p['geo']['request']['endpoint']=endpoint
                with self.assertRaises(AssertionError):validate_location_profile(p)
        for alter in [
            lambda p:p['geo']['request']['params'].update(key='$secret'),
            lambda p:p['text'].update(readyHeadline='{eval}'),
            lambda p:p['cities'][0].update(lat=True),
            lambda p:p['geo'].update(script='return fetch()'),
            lambda p:p['forecast'].update(refreshSec=1),
        ]:
            p=copy.deepcopy(self.profile);alter(p)
            with self.assertRaises(AssertionError):validate_location_profile(p)
    def test_package_only_variations(self):
        p=self.profile;p['cities']=[{'city':'青岛','lat':36.067,'lon':120.383}]
        p['text']['readyHeadline']='此刻 · {condition}'
        p['forecast']['request']['endpoint']='https://forecast.example.com/v2'
        p['forecast']['value']='/current/celsius'
        validate_location_profile(p)
    def test_picker_requires_complete_actions_and_labels(self):
        actions={f'host.{op}':op for op in ['choose_city','previous','next','refresh','todo']}
        app={'schemaVersion':1,'engine':'pc-app-services-v2','service':'weather','actions':actions,'location':self.profile}
        runtime={'host':True,'vars':{},'surface':{'engine':'p4-app-surface-v1'},'transitions':[{'from':'*','on':a} for a in actions]}
        self.assertEqual(application_errors(app,runtime),[])
        del app['location']['text']['pickerCancel']
        self.assertTrue(application_errors(app,runtime))
        # The pre-picker v2 profile remains importable, without claiming city controls.
        for key in 'cityButton emptyCityButton pickerNote pickerConfirm refreshButton todoButton'.split():
            del app['location']['text'][key]
        app['actions']={'host.refresh':'refresh'}
        runtime['transitions']=[{'from':'*','on':'host.refresh'}]
        self.assertEqual(application_errors(app,runtime),[])
if __name__=='__main__':unittest.main()
