"""Complete authoring-directory paths for phase2; no compiler, device or provider."""
import copy,json,tempfile,unittest,zipfile
from pathlib import Path
import validate_generated_widget as v
import text_flow
from app_form import errors as form_errors
from app_writers import derived_errors
ROOT=Path(__file__).parent.parent/'contracts'
SUITE=json.loads((ROOT/'phase2-pc-contracts-v1.json').read_text())
CAPS={**v.DEFAULT_CAPABILITIES,'widgetQuota':'p4-quota-v3','widgetAppFormV2':'p4-app-form-v2','widgetAppFormV3':'p4-app-form-v3','widgetLongText':'p4-longtext-v1','widgetDataRows':'p4-data-rows-v1','widgetTexts':'p4-texts-v1','widgetProgram':'p4-component-program-v1','widgetVariableOwners':'p4-variable-owners-v1'}
def runtime(case):
    r=copy.deepcopy(SUITE['bases'][case['base']])
    for patch in case['patches']:
        at=r
        for k in patch['path'][:-1]:at=at[k]
        at[patch['path'][-1]]=patch['value']
    return r

def manifest(r,base):
    original=ROOT/(base+'.json');fixture=json.loads(original.read_text()) if original.exists() else {}
    buttons=fixture.get('buttons')
    if buttons is None:
        events=['button.sw1.short_press','button.sw2.short_press','button.encoder.short_press','button.encoder.long_press','knob.rotate_ccw','knob.rotate_cw','joystick.up','joystick.down']
        actions=list(dict.fromkeys(t['on'] for t in r['transitions']))
        buttons=[{'event':e,'action':a,'control':'joystick' if e.startswith(('knob.','joystick.')) else 'encoder' if 'encoder' in e else 'sw1' if 'sw1' in e else 'sw2','label':a} for e,a in zip(events,actions)]
    dashboard={k:v.evaluate_dashboard_value(x,state=r['initial_state'],page=r.get('initial_page',r.get('pages',[{}])[0].get('id','')),variables=v.initial_var_values(r),texts=r.get('texts')) for k,x in r['dashboard'].items()}
    dashboard={k:str(x) if isinstance(x,(int,float)) else x for k,x in dashboard.items()}
    result={'component.json':{'id':'phase2-contract','name':'Contract','version':'1.0.0','kind':'tool','description':'Bounded contract fixture'},'negative-screen.json':{'dashboard':dashboard},'runtime/widget.json':r,'buttons.json':buttons,'share.json':{'title':'Contract'}}
    if base in SUITE.get('applications',{}):result['pc/application.json']=SUITE['applications'][base]
    return result
def write_package(root,files):
    for name,value in files.items():
        path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(value,ensure_ascii=False,separators=(',',':')))
    (root/'assets').mkdir(exist_ok=True);(root/'assets/.keep').write_bytes(b'')
class Phase2Contracts(unittest.TestCase):
    def test_shared_catalog_control_boundaries(self):
        from pc_extension import catalog_errors
        suite=json.loads((ROOT/'pc-catalog-controls-v1.json').read_text())
        for case in suite['cases']:
            with self.subTest(case=case['name']):
                row={**suite['row'],**case.get('extraFields',{}),'name':case['value']}
                raw=json.dumps([row],ensure_ascii=False,separators=(',',':')).encode('utf-8')
                errors=catalog_errors(raw)
                self.assertEqual(not errors,case['ok'],errors)
                if not case['ok']:self.assertIn('包内电台目录的文本字段无效',errors)

    def test_shared_catalog_controls_through_complete_package(self):
        from pc_extension import catalog_errors
        suite=json.loads((ROOT/'pc-catalog-controls-v1.json').read_text())
        fixture=Path(__file__).resolve().parents[3]/'builtin-clawpkgs/city-radio'
        with tempfile.TemporaryDirectory() as tmp:
            package=Path(tmp)
            for name in suite['packageFiles']:
                path=package/name;path.parent.mkdir(parents=True,exist_ok=True)
                path.write_bytes((fixture/name).read_bytes())
            for case in suite['cases']:
                with self.subTest(case=case['name']):
                    row={**suite['row'],**case.get('extraFields',{}),'name':case['value']}
                    raw=json.dumps([row],ensure_ascii=False,separators=(',',':')).encode('utf-8')
                    (package/'pc/catalog.json').write_bytes(raw)
                    errors=v.validate_widget(package,v.DEFAULT_CAPABILITIES)
                    self.assertEqual(not errors,case['ok'],errors)
                    if not case['ok']:
                        for error in catalog_errors(raw):self.assertIn(error,errors)

    def test_complete_directory_packages(self):
        for case in SUITE['cases']:
            with self.subTest(case=case['name']),tempfile.TemporaryDirectory() as tmp:
                r=runtime(case)
                if case['name'].startswith('app-form-') and not case['ok'] and '-derived-' not in case['name']:self.assertTrue(form_errors(r),case['name'])
                if '-derived-' in case['name'] or case['name'].startswith('derived-'):self.assertTrue(derived_errors(r),case['name'])
                root=Path(tmp);write_package(root,manifest(r,case['base']))
                errors=v.validate_widget(root,CAPS);self.assertEqual(not errors,case['ok'],errors)
    def test_real_zip_roundtrip_into_authoring_directory(self):
        # Python's production authoring API takes a directory. Native tests below
        # exercise its actual ZIP collector rather than a test extraction adapter.
        for base in ('data-rows-v1','texts-v1','longtext'):
            with self.subTest(base=base),tempfile.TemporaryDirectory() as tmp:
                source=Path(tmp)/'source';source.mkdir();write_package(source,manifest(SUITE['bases'][base],base))
                archive=Path(tmp)/'fixture.clawpkg'
                with zipfile.ZipFile(archive,'w') as z:
                    for f in source.rglob('*'):
                        if f.is_file():z.write(f,f.relative_to(source))
                target=Path(tmp)/'import';target.mkdir()
                with zipfile.ZipFile(archive) as z:z.extractall(target)
                self.assertEqual(v.validate_widget(target,CAPS),[])
    def test_explicit_capabilities_stay_closed(self):
        for base,key in [('data-rows-v1','widgetDataRows'),('texts-v1','widgetTexts'),('longtext','widgetLongText')]:
            with self.subTest(base=base),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);write_package(root,manifest(SUITE['bases'][base],base))
                for value in (None,'unknown',[CAPS[key]],True):
                    caps=CAPS.copy();caps.pop(key)
                    if value is not None:caps[key]=value
                    errors=v.validate_widget(root,caps);self.assertTrue(any(CAPS[key] in error for error in errors),errors)
    def test_shared_byte_flow_and_pages(self):
        s=json.loads((ROOT/'longtext-v1.json').read_text())
        for case in s['flow']:
            with self.subTest(case=case.get('name',case['text'][:15])):
                rows,truncated=text_flow.lines(case['text'],case['width'],case['font_size'])
                self.assertEqual((rows,truncated),(case['lines'],case['truncated']))
                self.assertTrue(all(len(line.encode())<=120 for line in rows))
        self.assertEqual(text_flow.page_lines('á'*120,640,16,20,20,-1),(['á'*60],2))
if __name__=='__main__':unittest.main()
