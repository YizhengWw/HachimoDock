"""Prepare private source-only HIL drafts from existing contracts/assets. Never install/publish."""
from pathlib import Path
import copy
import hashlib
import json
import shutil
import sys

ROOT = Path(__file__).resolve().parents[2]
DEST = ROOT / 'firmware/tests/fixtures/track_c_private'
SCRIPTS = ROOT / 'pc/skills/petui/scripts'
sys.path.insert(0, str(SCRIPTS))
from validate_generated_widget import DEFAULT_CAPABILITIES, validate_widget


def prepare():
    if DEST.exists():
        raise SystemExit(f'Refusing to overwrite existing fixtures: {DEST}')
    DEST.mkdir(parents=True)
    caps = dict(DEFAULT_CAPABILITIES)
    caps.update(widgetSequence='p4-sequence-v1', widgetSamples='p4-samples-v1', widgetClip='p4-clip-v1', widgetBigImage='p4-big-image-v1', widgetEvents='p4-events-v1')
    records = []
    def vector(name, index=0):
        return copy.deepcopy(json.loads((ROOT/f'pc/skills/petui/contracts/{name}-v1.json').read_text())['vectors'][index]['runtime'])
    def save(role, runtime, actions, assets=()):
        ident = 'private-cext-' + role
        path = DEST/ident
        (path/'runtime').mkdir(parents=True)
        (path/'assets').mkdir()
        runtime['schema_version'] = 1
        if role != 'big':
            runtime['vars']['n'] = {'type':'int','init':0}
        runtime['dashboard'] = {'title':ident, 'metricValue':{'var':'ox' if role=='big' else 'n'}, 'footer':'Private Track C acceptance only; global key exits', 'visualStyle':'clean', 'visualPalette':'mono','visualLayout':'tool','visualSprite':'music' if role in ('sequence','sample','clip') else 'timer'}
        surface = runtime.setdefault('surface', {'engine':'p4-app-surface-v1','graphics':'p4-app-graphics-v1','background':0,'elements':[]})
        surface['elements'] += [
            {'type':'text','x':340,'y':16,'w':290,'h':48,'color':65535,'font_size':18,'bind':'title'},
            {'type':'text','x':360,'y':100,'w':260,'h':48,'color':65535,'font_size':24,'bind':'metricValue'},
            {'type':'text','x':10,'y':420,'w':620,'h':40,'color':65535,'font_size':16,'bind':'footer'}]
        events = ['button.sw1.short_press','button.sw2.short_press','knob.rotate_cw','knob.rotate_ccw','joystick.up','joystick.down']
        buttons = [{'action':a,'control':'SW1' if i==0 else 'SW2' if i==1 else 'joystick','event':events[i],'label':a[:30]} for i,a in enumerate(actions)]
        files = {'component.json':{'id':ident,'name':ident,'version':'0.0.1','kind':'tool','description':'Private new-capability acceptance fixture; not a supported public component.'},
                 'negative-screen.json':{'dashboard':{'title':ident,'metricValue':'0','visualStyle':'clean','visualPalette':'mono','visualLayout':'tool','visualSprite':'music' if role in ('sequence','sample','clip') else 'timer'}},'buttons.json':buttons,'share.json':{'title':ident},'runtime/widget.json':runtime}
        for filename,value in files.items():
            (path/filename).write_text(json.dumps(value,ensure_ascii=False,separators=(',',':'))+'\n')
        provenance=[]
        for source,filename in assets:
            shutil.copyfile(ROOT/source,path/'assets'/filename)
            provenance.append({'source':source,'asset':'assets/'+filename,'sha256':hashlib.sha256((path/'assets'/filename).read_bytes()).hexdigest()})
        if not assets: (path/'assets/.keep').write_text('')
        errors = validate_widget(path,caps)
        records.append({'id':ident,'path':ident,'status':'offline-valid' if not errors else 'blocked-by-validation','validation_errors':errors,'assets':provenance})
    r=vector('sequence');r['sequence']['loop']=False
    r['transitions'][0]['inc']={'n':1};r['transitions'][1]['set']={'n':0}
    save('sequence',r,['start','stop'])
    r=vector('samples');r['vars']={'n':{'type':'int','init':0}};r['tick']=[];r['samples']['items']=r['samples']['items'][:1];r['transitions'][0]['inc']={'n':1}
    save('sample',r,['pet'],[('pc/public/instruments/wooden-fish/knock.wav','ding.wav')])
    r=vector('clip');r['transitions']=[{'from':'*','on':'show','to':'show','inc':{'n':1}},{'from':'*','on':'hide','to':'hide','inc':{'n':1}}];r['states']=['show','hide'];r['initial_state']='show'
    r['surface']['elements'][1].update(x=0,y=80,state_mask=1);r['input']={'engine':'p4-input-v1'}
    save('clip',r,['show','hide'],[('pc/public/terrier-clips/touch.lick.mp4','intro.mp4')])
    r=vector('big-image');r['surface']['big_images'][0].update(width=640,height=480);r['surface']['elements'][0].update(x=0,y=80,w=320,h=240)
    r['transitions']=[{'from':'*','on':'origin','set':{'ox':0,'oy':0}},{'from':'*','on':'bottom','set':{'ox':320,'oy':240}},{'from':'*','on':'right','inc':{'ox':240}},{'from':'*','on':'left','inc':{'ox':-240}},{'from':'*','on':'up','inc':{'oy':-240}},{'from':'*','on':'down','inc':{'oy':240}}]
    save('big',r,['origin','bottom','right','left','up','down'],[('firmware/assets/component-center/background.png','map.png')])
    r=vector('events');r['transitions']=[{'from':'*','on':'pet','emit':'done','inc':{'n':1}},{'from':'*','on':'clear','set':{'n':0}}]
    save('event-send',r,['pet','clear'])
    r=vector('events');r['events']['emit']=[];r['transitions']=[{'from':'*','on':'clear','set':{'n':0}},{'from':'*','on':'event:done','inc':{'n':1}}]
    save('event-recv',r,['clear'])
    r=vector('events',1);r['alarms']=[r['alarms'][1]];r['alarms'][0].update(background=True,text='Track C one minute')
    r['transitions']=[{'from':'*','on':'set','set':{'remind_in':60000}},{'from':'*','on':'cancel','set':{'remind_in':0}},{'from':'*','on':'ring','inc':{'n':1}}]
    r['persist']['vars'].append('n')
    save('alarm',r,['set','cancel'])
    manifest={'schemaVersion':1,'private':True,'installed':False,'published':False,'deviceEvidence':False,'authoringCapabilitiesAreNotDeviceReport':True,'packages':records}
    (DEST/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps({'output':str(DEST),'packages':len(records),'valid':sum(not p['validation_errors'] for p in records),'blocked':[{p['id']:p['validation_errors']} for p in records if p['validation_errors']]},ensure_ascii=False))


if __name__=='__main__':prepare()
