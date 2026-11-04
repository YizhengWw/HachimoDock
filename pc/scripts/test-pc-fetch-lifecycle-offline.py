"""Test the production PC-fetch lifecycle with a fake transport; no Cargo/network/USB."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
source=ROOT/'pc/src-tauri/src/pc_fetch_lifecycle.rs'
rustc=os.environ.get('PET_FETCH_RUSTC') or shutil.which('rustc')
if not rustc:
    candidates=list((Path.home()/'.rustup/toolchains').glob('stable-*/bin/rustc'))
    if candidates:rustc=str(candidates[0])
if not rustc:raise SystemExit('Set PET_FETCH_RUSTC to an existing rustc; no toolchain is downloaded')
with tempfile.TemporaryDirectory(prefix='pet-fetch-lifecycle-') as temp:
    binary=Path(temp)/('tests.exe' if os.name=='nt' else 'tests')
    subprocess.run([rustc,'--edition','2021','--test',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=20)
for name in ['pc/src-tauri/src/pc_fetch_lifecycle.rs','pc/src-tauri/src/pc_fetch_host.rs','pc/src-tauri/src/component_app.rs','pc/src-tauri/src/lib.rs','pc/src/component-center/ComponentAppPanel.jsx']:
    print(hashlib.sha256((ROOT/name).read_bytes()).hexdigest(),name)
