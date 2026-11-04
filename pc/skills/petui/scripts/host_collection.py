"""Bounded PC-fed collection declaration/snapshot validation; no IO."""
import re
import math
ENGINE='p4-host-collection-v1'
def _name(value,maximum):return isinstance(value,str) and re.fullmatch(r'[A-Za-z0-9_.-]{1,%d}'%maximum,value) is not None
def _num(value,lo,hi):return (type(value)is int and lo<=value<=hi) or (type(value)is float and math.isfinite(value) and value.is_integer() and lo<=value<=hi)
def _text(value,nonempty=False):
    return isinstance(value,str) and (not nonempty or bool(value)) and not any(ord(c)<32 or 127<=ord(c)<160 or 0xd800<=ord(c)<=0xdfff for c in value) and len(value.encode('utf-8'))<=63
def config(value,surface=None,runtime=None):
    def need(q):
        if not q:raise ValueError('Invalid bounded host collection')
    need(isinstance(value,dict) and set(value)=={'engine','source','x','y','w','row_height','gap','visible','page','actions','colors'})
    c=value;need(c['engine']==ENGINE and _name(c['source'],16) and _num(c['x'],0,639) and _num(c['y'],0,479) and _num(c['w'],128,640-c['x']))
    need(_num(c['visible'],1,8) and _num(c['row_height'],48,96) and _num(c['gap'],0,16) and _num(c['page'],0,4))
    height=c['visible']*c['row_height']+(c['visible']-1)*c['gap'];need(c['y']+height<=480)
    need(isinstance(c['actions'],list) and len(c['actions'])==c['visible'] and all(isinstance(a,str) and a.startswith('host.') and _name(a[5:],42) for a in c['actions']))
    need(len(set(c['actions']))==len(c['actions']) and isinstance(c['colors'],list) and len(c['colors'])==6 and all(_num(x,0,65535) for x in c['colors']))
    if surface is not None:
        need(surface.get('engine')=='p4-app-surface-v1' and surface.get('graphics')=='p4-app-graphics-v1' and isinstance(surface.get('elements'),list) and 1<=len(surface['elements']) and len(surface['elements'])+3*c['visible']<=32)
        need(not any(k in surface for k in ('game_view','scene_view','data_view','input')))
        def overlap(v):return c['x']<v['x']+v['w'] and c['x']+c['w']>v['x'] and c['y']<v['y']+v['h'] and c['y']+height>v['y']
        views=surface.get('viewports',{}).get('items',[surface['viewport']] if 'viewport' in surface else [])
        need(not any(overlap(v) for v in views) and not any(overlap(n) for n in surface['elements'] if 'action' in n and 'view' not in n and 'in_view' not in n))
    if runtime is not None:
        need(runtime.get('host')is True and runtime.get('host_context')=='p4-host-context-v1' and runtime.get('input',{}).get('engine')=='p4-input-v1')
        need((c['page']==4 or c['page']<max(1,len(runtime.get('pages',[])))) and all(any(t.get('on')==a for t in runtime.get('transitions',[])) for a in c['actions']))
    return {**c,**{k:int(c[k]) for k in ('x','y','w','row_height','gap','visible','page')},'colors':[int(x) for x in c['colors']]}
def snapshot(s):
    def need(q):
        if not q:raise ValueError('Invalid bounded host collection snapshot')
    need(isinstance(s,dict) and set(s)=={'engine','source','visible','count','offset','selectionKey','rows'})
    need(s['engine']==ENGINE and _name(s['source'],16) and _num(s['visible'],1,8) and _num(s['count'],0,128) and _num(s['offset'],0,127))
    last=max(0,s['count']-1)//s['visible']*s['visible']
    need(s['offset']<=last and s['offset']%s['visible']==0 and (s['selectionKey']is None or _text(s['selectionKey'],True)))
    need(isinstance(s['rows'],list) and len(s['rows'])==s['visible']);keys=set()
    for i,r in enumerate(s['rows']):
        need(isinstance(r,dict) and set(r)=={'present','key','label','detail','index','selected','truncated'})
        need(all(type(r[k])is bool for k in ('present','selected','truncated')) and r['present']==(s['offset']+i<s['count']))
        need(_text(r['key'],r['present']) and _text(r['label']) and _text(r['detail']) and _num(r['index'],-1,127))
        if r['present']:
            need(r['key'] not in keys and r['index']==s['offset']+i and r['selected']==(s['selectionKey']==r['key']));keys.add(r['key'])
        else:need(r['key']==r['label']==r['detail']=='' and r['index']==-1 and not r['selected'] and not r['truncated'])
    return s
