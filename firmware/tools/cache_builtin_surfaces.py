"""Checkpoint PC-encoded images with source and encoded SHA-256 fingerprints.

Use only after the native import/image-budget test exports PET_APP_ENCODED_DIR.
This build input is outside .clawpkg packages; no cache paths ship in metadata.
"""
import argparse,hashlib,json,shutil
from pathlib import Path

def cache(packages,encoded):
    repo=Path(__file__).resolve().parents[2]
    for package in packages.iterdir():
        if not package.is_dir() or not (package/'runtime/widget.json').is_file():continue
        widget=json.loads((package/'runtime/widget.json').read_text());images=widget.get('surface',{}).get('images',[])
        if not images:continue
        target=repo/'firmware/assets/builtin-surfaces'/package.name;target.mkdir(parents=True,exist_ok=True);index={}
        for im in images:
            data=(encoded/package.name/(im['id']+'.p4i')).read_bytes()
            if len(data)>100000 or data[:4] not in (b'P4I1',b'P4I2',b'P4J1') or int.from_bytes(data[8:12],'little')!=len(data):raise ValueError('invalid encoded image')
            index[im['id']]={'source':hashlib.sha256((package/im['asset']).read_bytes()).hexdigest(),'spec':hashlib.sha256(json.dumps(im,sort_keys=True,separators=(',',':')).encode()).hexdigest(),'encoded':hashlib.sha256(data).hexdigest()}
            shutil.copyfile(encoded/package.name/(im['id']+'.p4i'),target/(im['id']+'.p4i'))
        (target/'index.json').write_text(json.dumps(index,sort_keys=True,indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('packages',type=Path);p.add_argument('encoded',type=Path);a=p.parse_args();cache(a.packages,a.encoded)
