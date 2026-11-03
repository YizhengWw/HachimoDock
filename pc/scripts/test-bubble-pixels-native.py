#!/usr/bin/env python3
"""Small production-controller/method tests using ONLY already built rlibs.
Run inside the reviewer's synthetic-HOME OS sandbox; never invokes Cargo.
"""
import argparse,hashlib,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--deps',type=Path,required=True);p.add_argument('--rustc',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[1];a.output.mkdir(parents=True,exist_ok=False);(a.output/'transport').mkdir()
module=root/'src-tauri/src/usb_serial/bubble_pixels.rs';manager=root/'src-tauri/src/usb_serial.rs';fixture=root/'tests/bubble_pixels_transfer_host.rs'
(a.output/'transport/production_bubble.rs').write_bytes(module.read_bytes())
s=manager.read_text();begin=s.index('    pub(crate) fn send_bubble_pixels(');end=s.index('    /// Low-priority snapshots',begin)
(a.output/'production_transfer.inc').write_text('impl UsbSerialManager {\n'+s[begin:end]+'\n}\n')
(a.output/'fixture.rs').write_bytes(fixture.read_bytes())
def sha(x):return hashlib.sha256(x.read_bytes()).hexdigest()
libs={}
for name in ('serde','serde_json','base64','uuid'):
    available=list(a.deps.glob('lib'+name+'-*.rlib'))
    if not available:raise SystemExit('Missing PREBUILT dependency: '+name+'; do not install or rebuild here')
    libs[name]=max(available,key=lambda f:f.stat().st_mtime_ns)
command=[str(a.rustc),'--edition=2021','--test',str(a.output/'fixture.rs'),'-L','dependency='+str(a.deps),'-o',str(a.output/'bubble-tests')]
for name,path in libs.items():command+=['--extern',name+'='+str(path)]
(a.output/'inputs.json').write_text(json.dumps({'sources':{str(x.relative_to(root)):{'sha256':sha(x)} for x in (module,manager,fixture)},'prebuiltRlibs':{k:{'path':str(v),'sha256':sha(v)} for k,v in libs.items()},'command':command},indent=2)+'\n')
subprocess.run(command,check=True)
subprocess.run([str(a.output/'bubble-tests'),'--test-threads=1','--nocapture'],check=True,timeout=30)
