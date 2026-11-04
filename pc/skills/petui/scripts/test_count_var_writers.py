"""Read-only derived target admission through complete authoring package paths."""
import copy,json,tempfile,unittest,zipfile
from pathlib import Path
import validate_generated_widget as validator
from app_writers import derived_errors
from test_phase2_contracts import CAPS,write_package
ROOT=Path(__file__).parent.parent/'contracts'
SUITE=json.loads((ROOT/'count-var-writers-v1.json').read_text())
def runtime(c):
    r=copy.deepcopy(SUITE['bases'][c['base']])
    for p in c['patches']:
        at=r
        for k in p['path'][:-1]:at=at[k]
        at[p['path'][-1]]=copy.deepcopy(p['value'])
    return r
class CountVarWriters(unittest.TestCase):
    def test_complete_directory_and_zip_roundtrip(self):
        for c in SUITE['cases']:
            with self.subTest(case=c['name']),tempfile.TemporaryDirectory() as tmp:
                r=runtime(c);files=copy.deepcopy(SUITE['packages'][c['base']]);files['runtime/widget.json']=r
                source=Path(tmp)/'package';source.mkdir();write_package(source,files)
                errors=validator.validate_widget(source,CAPS);self.assertEqual(not errors,c['ok'],errors)
                self.assertEqual(not derived_errors(r),c['ok'])
                if not c['ok']:self.assertTrue(any('guest set/inc target' in e for e in errors),errors)
                archive=Path(tmp)/'contract.clawpkg'
                with zipfile.ZipFile(archive,'w') as z:
                    for f in source.rglob('*'):
                        if f.is_file():z.write(f,f.relative_to(source))
                imported=Path(tmp)/'import';imported.mkdir()
                with zipfile.ZipFile(archive) as z:z.extractall(imported)
                self.assertEqual(validator.validate_widget(imported,CAPS),errors)
if __name__=='__main__':unittest.main()
