#!/usr/bin/env python3
"""Read-only USB component inventory; never bind, reset, install or play audio."""
import argparse,json,subprocess,time,sys
import serial

def main():
    args=argparse.ArgumentParser();args.add_argument('port');args.add_argument('--baud',type=int,default=4_000_000);o=args.parse_args()
    occupied=subprocess.run(['lsof',o.port],capture_output=True,text=True)
    if occupied.returncode==0:raise SystemExit('Serial port is occupied; disconnect Pet Manager first.')
    port=serial.Serial();port.port=o.port;port.baudrate=o.baud;port.timeout=.15;port.write_timeout=2
    # Same steady line state as the shipped desktop client; no reset pulse.
    port.dtr=sys.platform!='win32';port.rts=sys.platform!='win32'
    result={};request='component-inventory-readonly'
    port.open()
    try:
        port.write(b'\n');sent=0
        deadline=time.monotonic()+15
        while time.monotonic()<deadline and len(result)<2:
            if time.monotonic()-sent>2:
                for topic in ('diagnostics/query','widget/list'):
                    port.write((json.dumps({'topic':topic,'payload':{'requestId':request}})+'\n').encode())
                port.flush();sent=time.monotonic()
            line=port.readline().decode('utf-8',errors='replace');start=line.find('{')
            if start<0:continue
            try:message=json.loads(line[start:])
            except ValueError:continue
            p=message.get('payload',{})
            if p.get('requestId')!=request:continue
            if message.get('topic')=='widget/inventory':result['inventory']=p
            elif message.get('topic')=='diagnostics/status':
                r=p.get('runtime',{});result['diagnostics']={'firmware':r.get('firmware'),'buildId':r.get('buildId'),'chipRevision':r.get('chipRevision'),'imageState':r.get('imageState'),'storage':p.get('storage'),'bootCount':p.get('bootCount'),'faultResetCount':p.get('faultResetCount')}
        print(json.dumps(result,ensure_ascii=False,indent=2))
        if len(result)!=2:raise SystemExit('Read-only query incomplete; no device changes were made.')
    finally:port.close()

if __name__=='__main__':main()
