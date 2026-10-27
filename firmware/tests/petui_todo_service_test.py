"""Cross-language declaration boundary; no device, personal records or services."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'pc/skills/petui/scripts'))
from app_services import application_errors

class TodoServiceContract(unittest.TestCase):
    def test_confirmation_and_operation_whitelist(self):
        d={'schemaVersion':1,'engine':'pc-app-services-v1','service':'todos','actions':{'host.open':'open','host.toggle':'toggle'}}
        r={'host':True,'surface':{'engine':'p4-app-surface-v1'},'vars':{},'tick':[],
           'transitions':[{'from':'*','on':a} for a in d['actions']],
           'input':{'engine':'p4-input-v1','confirm':{'host.toggle':'确认完成或撤销？'}}}
        self.assertEqual(application_errors(d,r),[])
        r['input']['confirm']={}
        self.assertTrue(application_errors(d,r))
        d['actions']['host.toggle']='delete'
        self.assertTrue(application_errors(d,r))

if __name__=='__main__': unittest.main()
