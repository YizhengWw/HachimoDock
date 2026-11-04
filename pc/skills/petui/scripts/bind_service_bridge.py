"""Bind the authored PC declaration to its device runtime; no install or network."""
from pathlib import Path
import argparse
import json
from service_bridge import binding_contract,ENGINE,DEVICE_ENGINE

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('component',type=Path)
    parser.add_argument('--write',action='store_true')
    args=parser.parse_args();root=args.component.resolve()
    runtime_path=root/'runtime/widget.json'
    runtime=json.loads(runtime_path.read_text())
    declaration=json.loads((root/'pc/application.json').read_text())
    if declaration.get('engine')!=ENGINE or runtime.get('surface',{}).get('services')!=DEVICE_ENGINE:raise ValueError('not a service-bridge package')
    digest=binding_contract(declaration,runtime)
    if args.write:
        library=(Path.home()/'.claw-pet/components/library').resolve()
        if root==library or library in root.parents:raise ValueError('bind the independent work package, never the formal library')
        runtime['host_contract']=digest
        runtime_path.write_text(json.dumps(runtime,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps({'contract':digest,'matches':runtime.get('host_contract')==digest,'written':args.write}))
if __name__=='__main__':main()
