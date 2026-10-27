"""Boundary regression for the newly authored package (never reads old packs)."""
import copy,json,sys
from pathlib import Path
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'skills/petui/scripts'))
from motion_policy import motion_errors
from input_policy import input_errors
from validate_generated_widget import load_capabilities,validate_widget
package=root/'new-components/cloud-hop'
r=json.loads((package/'runtime/widget.json').read_text())
b=json.loads((package/'buttons.json').read_text())
assert not motion_errors(r) and not input_errors(r,b)
assert not validate_widget(package,load_capabilities(None))
caps=load_capabilities(None);caps.pop('widgetMotion')
assert any('p4-motion-v1' in e for e in validate_widget(package,caps))
caps=load_capabilities(None);caps.pop('widgetPlatformTrack')
assert any('p4-platform-track-v1' in e for e in validate_widget(package,caps))
for key,value in [('camera_x',0),('camera_x',True),('gap',[5,2]),('width',[2,5]),('tones',[1,1]),('tones',[1]),('engine','unknown')]:
    bad=copy.deepcopy(r);bad['scene']['motion']['track'][key]=value;assert motion_errors(bad)
bad=copy.deepcopy(r);bad['scene']['motion']['track']=None;assert motion_errors(bad)
bad=copy.deepcopy(r);bad['scene']['rules'][1]['do']=[{'op':'place','entity':'puppy','x':2,'y':4}];assert motion_errors(bad)
cases=[('body','missing'),('platforms',['home','home']),('platforms',['puppy']),
       ('gravity',0),('gravity',65),('lift',True),('charge_ms',299),
       ('speed',[13,2]),('speed',[0,2]),('charge_var','score'),('charge_var','best')]
for key,value in cases:
    bad=copy.deepcopy(r);bad['scene']['motion'][key]=value
    assert motion_errors(bad),(key,value)
for value in (None,[],1,'bad'):
    bad=copy.deepcopy(r);bad['scene']=value
    motion_errors(bad) # structure validator owns these errors; no exception
for key,value in [('press',['jump.charge']),('repeat',['jump.charge']),('cooldown',{'jump.charge':500}),('confirm',{'jump.charge':'确定？'})]:
    bad=copy.deepcopy(r);bad['input'][key]=value
    assert input_errors(bad,b)
bad=copy.deepcopy(b);bad[0]['event']='button.sw2.short_press'
assert input_errors(r,bad)
bad=copy.deepcopy(r);bad['scene']['entities'][0]['y']=2
assert motion_errors(bad)
bad=copy.deepcopy(r);bad['scene']['rules'][1]['with']='puppy'
assert motion_errors(bad)
print('PASS motion policy: valid package, old-device rejection, malformed scenes, unsupported body/platforms/physics, independent variables, input conflicts and mapping')
