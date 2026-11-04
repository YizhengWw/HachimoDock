"""Pinned P4 outgoing AHB-reset backport; only a build-directory V1 copy.
V3 already has the native pair and is returned unchanged. Unknown input rejects.
No installation, dependency resolution or SDK/package mutation.
"""
import argparse,hashlib
from pathlib import Path
REL=Path('components/esp_system/port/soc/esp32p4/system_internal.c')
V1='c7749ec9448ac9d67407ff45f36acd30dc4a97f14c5f86fb5249f46d177eeb3d'
V3='666e2e86221fceb2eb6371065f8417eb5ae30470f819d0e9c6aa627058f7fafa'
SET='    SET_PERI_REG_MASK(HP_SYS_CLKRST_HP_RST_EN1_REG, HP_SYS_CLKRST_REG_RST_EN_AHB_PDMA);\n'
CLEAR='    CLEAR_PERI_REG_MASK(HP_SYS_CLKRST_HP_RST_EN1_REG, HP_SYS_CLKRST_REG_RST_EN_AHB_PDMA);\n'
def transform(data):
    digest=hashlib.sha256(data).hexdigest();text=data.decode()
    if digest==V3:
        if text.count(SET)!=1 or text.count(CLEAR)!=1:raise ValueError('native V3 AHB pair missing')
        return data,False
    if digest!=V1:raise ValueError('unrecognized pinned P4 system_internal.c')
    for anchor,line in [('    // Set Peripheral clk rst\n',SET),('    // Clear Peripheral clk rst\n',CLEAR)]:
        if text.count(anchor)!=1 or line in text:raise ValueError('V1 reset anchor changed')
        text=text.replace(anchor,anchor+line,1)
    return text.encode(),True

def select(sdk_dir,build_dir):
    sdk=Path(sdk_dir).resolve();build=Path(build_dir).resolve();source=sdk/REL
    if source.is_symlink():raise ValueError('SDK source symlink refused')
    data=source.read_bytes();generated,changed=transform(data)
    if not changed:return source
    firmware=Path(__file__).resolve().parents[1]
    forbidden=[sdk,firmware,*[firmware/n for n in ('main','tools','tests','components','managed_components')]]
    if any(build==p or p in build.parents for p in forbidden if p!=firmware) or build==firmware or build in firmware.parents:
        raise ValueError('output must be a separate build directory')
    folder=build/'pet_p4_restart_backport';dest=folder/'system_internal.c'
    for p in (build,folder,dest):
        if p.is_symlink():raise ValueError('generated destination symlink refused')
    if dest.exists() and dest.read_bytes()!=generated:raise ValueError('unexpected generated source; use a fresh build directory')
    folder.mkdir(parents=True,exist_ok=True)
    if not dest.exists():
        with dest.open('xb') as f:f.write(generated)
    if source.read_bytes()!=data:raise ValueError('SDK input changed during generation')
    return dest
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sdk-dir',type=Path,required=True);p.add_argument('--build-dir',type=Path,required=True);a=p.parse_args()
    print(select(a.sdk_dir,a.build_dir))
