"""Explicitly authorized OTA and three-package bench; no audio or user-partition erase."""
import argparse,base64,hashlib,json,struct,time,zlib
from pathlib import Path
import serial

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT.parent/'builds/local-delights-20261022/device'
BOARD='p4-80f1b2d75dfd'
IDS=('decision-capsule','answer-book','hope-train')
class Passive(serial.Serial):
    def _update_dtr_state(self):pass
    def _update_rts_state(self):pass
class Bench:
    def __init__(self,port):
        self.s=Passive(port=None,baudrate=4000000,timeout=.15,write_timeout=3,exclusive=True)
        self.s.port=port;self.s.dtr=False;self.s.rts=False;self.s.open()
    def send(self,t,p):
        raw=(json.dumps({'topic':t,'payload':p},ensure_ascii=False,separators=(',',':'))+'\n').encode()
        for i in range(0,len(raw),64):self.s.write(raw[i:i+64]);time.sleep(.0007)
    def recv(self,t,match=lambda p:True,timeout=15):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            raw=self.s.readline().replace(b'\0',b'')
            if b'Guru Meditation' in raw or b'Stack protection fault' in raw:raise RuntimeError('Device panic')
            start=raw.find(b'{')
            if start<0:continue
            try:m=json.loads(raw[start:])
            except ValueError:continue
            p=m.get('payload',{})
            if m.get('topic')==t and match(p):
                assert p.get('ok') is not False,p
                return p
        raise TimeoutError(t)
    def request(self,t,p=None,reply='protocol/ack',timeout=15):
        rid='delights-'+str(time.time_ns());self.send(t,dict(p or {},requestId=rid))
        return self.recv(reply,lambda p:p.get('requestId')==rid,timeout)
    def diag(self):
        p=self.request('diagnostics/query',reply='diagnostics/status');assert p['boardDeviceId']==BOARD;return p
    def inventory(self):return self.request('widget/list',reply='widget/inventory')
    def state(self):
        self.send('miniapp/query',{});return self.recv('miniapp/state')
    def action(self,a):return self.request('miniapp/event',{'action':a})
    def transfer(self,kind,phase,p,timeout=20):
        self.send(kind+'/'+phase,p)
        return self.recv('widget-install-ack' if kind=='widget' else kind+'/ack',lambda a:a.get('transferId')==p['transferId'] and a.get('phase')==phase and (phase!='chunk' or (a.get('seq')==p.get('seq') if kind=='firmware' else a.get('path')==p.get('path') and str(a.get('index'))==str(p.get('index')))),timeout)
    def capture(self,name):
        self.send('debug/screenshot',{'fullResolution':True});start=self.recv('debug/screenshot_begin');w,h=start['width'],start['height'];assert (w,h)==(640,480) and start['format']=='rgb565le'
        raw=bytearray();index=0
        while len(raw)<w*h*2:
            chunk=self.recv('debug/screenshot_chunk',lambda p:p['id']==start['id']);assert chunk['index']==index
            raw.extend(base64.b64decode(chunk['data'],validate=True));index+=1
        end=self.recv('debug/screenshot_end',lambda p:p['id']==start['id']);assert end['chunks']==index and len(raw)==w*h*2
        checksum=2166136261;rgb=bytearray()
        for y in range(h):
            rgb.append(0)
            for x in range(w):
                px=struct.unpack_from('<H',raw,(y*w+x)*2)[0];checksum=((checksum^px)*16777619)&0xffffffff
                r,g,bl=(px>>11)&31,(px>>5)&63,px&31;rgb.extend(((r<<3)|(r>>2),(g<<2)|(g>>4),(bl<<3)|(bl>>2)))
        assert f'{checksum:08x}'==end['checksum']
        def part(k,v):return struct.pack('>I',len(v))+k+v+struct.pack('>I',zlib.crc32(k+v)&0xffffffff)
        png=b'\x89PNG\r\n\x1a\n'+part(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+part(b'IDAT',zlib.compress(rgb,9))+part(b'IEND',b'')
        OUT.mkdir(parents=True,exist_ok=True);(OUT/(name+'.png')).write_bytes(png);print('Captured',name,flush=True)
def save(name,p):
    OUT.mkdir(parents=True,exist_ok=True);(OUT/name).write_text(json.dumps(p,ensure_ascii=False,indent=2))
def ota(b):
    before=b.diag();catalog=b.inventory();save('before-upgrade.json',{'diagnostics':before,'inventory':catalog})
    image=(ROOT.parent/'builds/local-delights-20261022/firmware-0.7.102-p4-v3.bin').read_bytes()
    sha=hashlib.sha256(image).hexdigest();assert sha=='b9bf712f8e3d52da43e924b5a036f45facbecbf77257c3adfa94d1f1fa396a1c'
    low,high=struct.unpack_from('<HH',image,15);assert low<=before['runtime']['chipRevision']<=high
    version=image[48:80].split(b'\0')[0].decode();assert version=='0.7.102-p4'
    assert before['runtime']['firmware']!='0.7.102-p4','Already upgraded; inspect rather than repeat'
    status=b.request('firmware/query',reply='firmware/status');assert status['supported'] and not status['active'] and not status['pendingVerify'],status
    tid='delights-ota-'+str(time.time_ns());begun=commit_sent=False
    try:
        b.transfer('firmware','begin',dict(transferId=tid,size=len(image),sha256=sha,chipRevisionMin=low,chipRevisionMax=high),30);begun=True
        for seq,i in enumerate(range(0,len(image),4092)):
            data=image[i:i+4092];p=dict(transferId=tid,seq=seq,decodedSize=len(data),data=base64.b64encode(data).decode())
            # Only same-sequence chunks are idempotent. Never repeat commit.
            for attempt in range(3):
                try:a=b.transfer('firmware','chunk',p);break
                except TimeoutError:
                    if attempt==2:raise
            assert a['receivedBytes']==i+len(data),a
            if seq%100==0:print('OTA',i+len(data),'/',len(image),flush=True)
        commit_sent=True;a=b.transfer('firmware','commit',{'transferId':tid},30)
        assert a['sha256']==sha and a['version']==version;save('ota-commit.json',a)
    finally:
        if begun and not commit_sent:b.send('firmware/abort',{'transferId':tid})
    print('Committed; verifying stable boot',flush=True);time.sleep(2)
    end=time.monotonic()+60
    while time.monotonic()<end:
        try:after=b.diag()
        except TimeoutError:continue
        r=after['runtime']
        if r['firmware']==version and r['imageState']=='valid':break
        time.sleep(1)
    else:raise RuntimeError('Boot validity not confirmed; do not retry update')
    assert after['storage']['activeAppearanceSlot']==before['storage']['activeAppearanceSlot']
    assert after['faultResetCount']==before['faultResetCount']
    inventory=b.inventory();save('after-upgrade.json',{'diagnostics':after,'inventory':inventory})
    assert [v['id'] for v in inventory['items']]==[v['id'] for v in catalog['items']]
    print(json.dumps({'firmware':r['firmware'],'imageState':r['imageState'],'touchReady':r['touchReady'],'inventory':inventory},ensure_ascii=False),flush=True)

def fnv(data):
    n=0xcbf29ce484222325
    for v in data:n=((n^v)*0x100000001b3)&0xffffffffffffffff
    return f'{n:016x}'
def install(b,id):
    root=ROOT/'pc/community-components/local-delights'/id
    tid='delights-widget-'+str(time.time_ns());b.transfer('widget','begin',{'transferId':tid,'widgetId':id})
    entries=[]
    for name in ('component.json','negative-screen.json','share.json','buttons.json','runtime/widget.json'):
        value=json.loads((root/name).read_text());entries.append((name,json.dumps(value,ensure_ascii=False,separators=(',',':')).encode()))
    for p in sorted((ROOT.parent/'builds/local-delights-20261022/encoded'/id).glob('*.p4i')):entries.append(('runtime/images/'+p.name,p.read_bytes()))
    for name,data in entries:
        step=4096 if name.startswith('runtime/images/') else len(data)
        for index,i in enumerate(range(0,len(data),step)):
            part=data[i:i+step];b.transfer('widget','chunk',dict(transferId=tid,path=name,index=str(index),decodedSize=len(part),checksum=fnv(part),data=base64.b64encode(part).decode()))
    b.transfer('widget','commit',{'transferId':tid,'widgetId':id},30)
    time.sleep(.7);state=b.state();assert state['widgetId']==id;save(id+'-installed.json',state);print('Installed',id,flush=True)

def test(b,id):
    before=b.diag();states=[]
    def state():
        s=b.state();states.append(s);return s
    b.capture(id+'-initial')
    if id!='hope-train':
        last=None
        for _ in range(5):
            b.action('local.draw');assert state()['state']=='busy'
            try:b.action('local.draw')
            except AssertionError as e:assert 'miniapp_event_unhandled' in str(e)
            else:raise AssertionError('rapid draw was not rejected')
            time.sleep(1.1);s=state();assert s['state']=='result' and s['headline']!=last;last=s['headline']
        b.capture(id+'-result')
        if id=='decision-capsule':
            excluded=[]
            for _ in range(6):
                excluded.append(state()['headline']);b.action('local.exclude');time.sleep(1.1);s=state()
                assert s['state']=='empty' or s['headline'] not in excluded
            assert s['state']=='empty';b.action('local.reset');assert state()['state']=='empty'
            b.action('input.cancel');assert state()['state']=='empty'
            b.action('local.reset');b.action('input.confirm');assert state()['metricValue']=='6'
            b.action('local.next');assert state()['badge']=='2/3';b.action('local.previous');assert state()['badge']=='1/3'
        else:b.action('local.close');assert state()['state']=='ready'
    else:
        assert state()['state']=='offline'
        offset=-time.timezone//60 if not time.localtime().tm_isdst else -time.altzone//60
        b.request('system/heartbeat',{'clock':{'unixSeconds':int(time.time()),'utcOffsetMinutes':offset}});time.sleep(.3)
        original=state();assert original['state']!='offline'
        b.action('local.edit');assert state()['state']=='editing';b.action('local.up');b.action('local.cancel');assert state()['note']==original['note']
        b.action('local.edit');b.action('local.up');b.action('local.edit');assert state()['note']!=original['note']
        b.request('control/screen-page',{'page':'main'});time.sleep(.3);b.request('control/screen-page',{'page':'app'});time.sleep(.3)
        assert state()['note']!=original['note']
        b.action('local.edit');b.action('local.down');b.action('local.edit');assert state()['note']==original['note']
        b.action('local.next');assert state()['badge']=='2/3';b.action('local.next');assert state()['badge']=='3/3'
        b.action('local.edit');b.action('local.up');b.action('local.next');b.action('local.down');b.capture(id+'-editor');b.action('local.cancel')
        assert state()['metricValue']=='—';b.action('local.next');assert state()['badge']=='1/3'
        b.capture(id+'-synced')
    after=b.diag();assert before['bootCount']==after['bootCount'] and before['faultResetCount']==after['faultResetCount']
    save(id+'-actions.json',{'states':states,'noReset':True,'physicalInputVerified':False});print('Device action tests passed',id,flush=True)

def main():
    p=argparse.ArgumentParser();p.add_argument('--port',required=True);p.add_argument('mode',choices=['probe','ota','sync']);args=p.parse_args();b=Bench(args.port)
    try:
        if args.mode=='probe':print(json.dumps({'diagnostics':b.diag(),'inventory':b.inventory()},ensure_ascii=False))
        elif args.mode=='ota':ota(b)
        else:
            d=b.diag();assert d['runtime']['firmware']=='0.7.102-p4' and d['runtime']['imageState']=='valid'
            b.send('bind',{'desktopDeviceId':'petui-local-acceptance'});hello=b.recv('hello');assert hello['boardDeviceId']==BOARD
            assert hello['capabilities']['widgetLocalTools']=='p4-local-tools-v1' and hello['capabilities']['touchInput']['ready']
            save('capabilities.json',hello['capabilities'])
            before=b.inventory();save('before-sync.json',before)
            existing={v['id'] for v in before['items']};assert not existing.intersection(IDS),'Existing target records require preserving saved bindings before replacement'
            for id in IDS:install(b,id);test(b,id)
            after=b.inventory();save('after-sync.json',after)
            assert {v['id'] for v in after['items']}==existing|set(IDS)
            print(json.dumps(after,ensure_ascii=False))
    finally:b.s.close()
if __name__=='__main__':main()
