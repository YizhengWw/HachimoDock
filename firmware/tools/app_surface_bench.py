"""Capture an explicitly selected application sample; no flashing or recording.

Uses the read-only framebuffer diagnostic: 320×240 transport thumbnail by
default, or actual 640×480 pixels with --full. Do not run during USB audio.
"""
import argparse
import base64
import json
import struct
import time
import zlib
from pathlib import Path
import serial

class PassiveSerial(serial.Serial):
    def _update_dtr_state(self):pass
    def _update_rts_state(self):pass

def run(args):
    connection=PassiveSerial(port=None,baudrate=4000000,timeout=.15,write_timeout=3)
    connection.dtr=connection.rts=False;connection.exclusive=True;connection.port=args.port
    def send(topic,payload):
        raw=(json.dumps({'topic':topic,'payload':payload},separators=(',',':'))+'\n').encode()
        for at in range(0,len(raw),64):connection.write(raw[at:at+64]);time.sleep(.0005)
    def receive(topic,match=lambda p:True,timeout=20):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            raw=connection.readline().replace(b'\0',b'')
            if b'Guru Meditation' in raw or b'Stack protection fault' in raw:raise RuntimeError('device panic')
            start=raw.find(b'{')
            if start<0:continue
            try:msg=json.loads(raw[start:])
            except ValueError:continue
            p=msg.get('payload',{})
            if msg.get('topic')=='debug/screenshot_error':raise RuntimeError(p.get('error'))
            if msg.get('topic')==topic and match(p):return p
        raise TimeoutError(topic)
    def request(topic,reply,payload=None):
        rid='app-display-'+str(time.time_ns());send(topic,dict(payload or {},requestId=rid))
        p=receive(reply,lambda p:p.get('requestId')==rid);assert p.get('ok') is not False,p;return p
    def capture(name):
        send('debug/screenshot',{'fullResolution':args.full});begin=receive('debug/screenshot_begin');identity=begin['id']
        w,h=begin['width'],begin['height'];assert begin['format']=='rgb565le' and (w,h)==((640,480) if args.full else (320,240))
        raw=bytearray();index=0
        while len(raw)<w*h*2:
            part=receive('debug/screenshot_chunk',lambda p:p.get('id')==identity);assert part['index']==index
            raw.extend(base64.b64decode(part['data'],validate=True));index+=1
        end=receive('debug/screenshot_end',lambda p:p.get('id')==identity);assert len(raw)==w*h*2 and end['chunks']==index
        checksum=2166136261;rgb=bytearray()
        for y in range(h):
            rgb.append(0)
            for x in range(w):
                px=struct.unpack_from('<H',raw,(y*w+x)*2)[0];checksum=((checksum^px)*16777619)&0xffffffff
                r,g,b=(px>>11)&31,(px>>5)&63,px&31;rgb.extend(((r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2)))
        assert f'{checksum:08x}'==end['checksum']
        def chunk(k,b):return struct.pack('>I',len(b))+k+b+struct.pack('>I',zlib.crc32(k+b)&0xffffffff)
        png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(rgb,9))+chunk(b'IEND',b'')
        (args.output/name).write_bytes(png);print('Captured '+name,flush=True)
    try:
        connection.open();before=request('diagnostics/query','diagnostics/status')
        assert before['boardDeviceId']==args.board and before['runtime']['firmware']==args.firmware
        assert before['runtime']['imageState']=='valid'
        inventory=request('widget/list','widget/inventory');assert inventory['activeWidgetId']=='hachimi-home'
        request('control/screen-page','protocol/ack',{'page':'app'});time.sleep(.6)
        args.output.mkdir(parents=True,exist_ok=True);capture('device-home.png')
        # No synthetic pats or theme changes: preserve the user's saved interaction count.
        for page in ['main','app','main','app']:
            request('control/screen-page','protocol/ack',{'page':page});time.sleep(.6)
        capture('device-home-reentered.png')
        after=request('diagnostics/query','diagnostics/status')
        assert before['bootCount']==after['bootCount'] and before['faultResetCount']==after['faultResetCount']
        assert before['storage']['activeAppearanceSlot']==after['storage']['activeAppearanceSlot']
        report={'firmware':args.firmware,'noReset':True,'bootCount':after['bootCount'],'faultResetCount':after['faultResetCount'],'captureResolution':[640,480] if args.full else [320,240],'logicalResolution':[640,480]}
        (args.output/'report.json').write_text(json.dumps(report,indent=2));print(json.dumps(report),flush=True)
    finally:connection.close()

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--port',required=True);p.add_argument('--board',required=True);p.add_argument('--firmware',required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--full',action='store_true');run(p.parse_args())
