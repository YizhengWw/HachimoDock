"""Execute the real pre-build script with an inert SCons env, no PIO/device."""
from pathlib import Path
from unittest.mock import patch
import pytest

ROOT=Path(__file__).resolve().parents[1]
SCRIPT=ROOT/'scripts/platformio_add_p4_toolchain.py'

class Env(dict):
    def __init__(self,root,family):
        super().__init__({'ENV':{}});self.root=str(root);self.family=family;self.post=[]
    def subst(self,value):
        assert value in ('$PROJECT_DIR','$BUILD_DIR');return self.root if value=='$PROJECT_DIR' else str(Path(self.root)/'.pio/build/test')
    def Replace(self,**values):self.update(values)
    def GetProjectOption(self,key,default):
        return self.family if key=='custom_p4_chip_family' else default
    def AddPostAction(self,target,action):
        self.post.append((target,action))
    def PioPlatform(self):return self
    def get_package_dir(self,name):
        assert name=='toolchain-riscv32-esp';return None

def load(root,family):
    env=Env(root,family);callbacks=[]
    namespace={'env':env,'Import':lambda _:None}
    with patch('atexit.register',side_effect=lambda fn,*args:callbacks.append((fn,args))):
        exec(compile(SCRIPT.read_text(),str(SCRIPT),'exec'),namespace)
    assert len(callbacks)==1 and len(env.post)==1
    return env,callbacks[0]

@pytest.mark.parametrize('family,name',[('v1','dependencies.lock'),('v3','dependencies.v3.lock')])
def test_incremental_build_exit_restores_local_paths_without_binary_post_action(tmp_path,family,name):
    components=tmp_path/'components';components.mkdir();(components/'driver').mkdir()
    native=str(components/'driver');windows=native.replace('/','\\')
    content=f'''dependencies:
  local_a:
    source:
      path: {native}
    version: '1.2.3'
    component_hash: ffffffffffffffff
  local_b:
    source:
      path: "{windows}"
  unrelated:
    source:
      path: {tmp_path}-other/components/driver
  metadata:
    note: {native}
'''
    path=tmp_path/name;path.write_text(content)
    other=tmp_path/('dependencies.v3.lock' if family=='v1' else 'dependencies.lock');other.write_text('untouched\n')
    env,(callback,args)=load(tmp_path,family)
    # Simulate the successful no-relink build: never invoke env.post callbacks.
    callback(*args)
    expected=content.replace('path: '+native,'path: $PET_P4_PROJECT_DIR/components/driver').replace('path: "'+windows+'"','path: $PET_P4_PROJECT_DIR/components/driver')
    assert path.read_text()==expected and other.read_text()=='untouched\n'
    first=path.read_bytes();callback(*args);assert path.read_bytes()==first
    assert env['ENV']['PET_P4_PROJECT_DIR']==str(tmp_path)

def test_failed_or_interrupted_build_cleanup_does_not_guess_foreign_or_nested_dependency_paths(tmp_path):
    (tmp_path/'components').mkdir();(tmp_path/'components/driver').mkdir()
    lock=tmp_path/'dependencies.lock';original=f'  path: {tmp_path}/components/driver/nested\n  version: 9.8.7\n'
    lock.write_text(original);_,(callback,args)=load(tmp_path,'v1')
    with pytest.raises(RuntimeError,match='Unexpected project-local'):callback(*args)
    assert lock.read_text()==original
    lock.write_text('  path: /another/project/components/driver\n  version: 9.8.7\n');callback(*args)
    assert lock.read_text()=='  path: /another/project/components/driver\n  version: 9.8.7\n'
