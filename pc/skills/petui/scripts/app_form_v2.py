"""Strict opt-in mixed form validation; executable semantics come from shared C."""
import re
def invoke(runtime,state,operation,argument=None):
    """Use product C/WASM, not a fourth Python implementation of editor logic."""
    import json,shutil,subprocess
    from pathlib import Path
    node=shutil.which('node')
    if not node:raise RuntimeError('Mixed form verification requires Node.js shared native core')
    result=subprocess.run([node,str(Path(__file__).with_name('form-v2-host.mjs'))],input=json.dumps({'mode':'operation','runtime':runtime,'state':state,'operation':operation,'argument':argument}),text=True,capture_output=True,timeout=10)
    if result.returncode:raise ValueError('Shared native mixed form rejected the declaration or snapshot')
    response=json.loads(result.stdout);state.clear();state.update(response['state']);return response['result']
ENGINE='p4-app-form-v2'
OPS=('previous','next','decrease','increase','accept','cancel','append','erase')
PHYSICAL={'button.sw1.short_press','button.sw2.short_press','button.encoder.short_press','button.encoder.long_press','knob.rotate_ccw','knob.rotate_cw','joystick.up','joystick.down'}
def utf8(s,max_bytes=63,empty=True):
    try:return isinstance(s,str) and (empty or bool(s)) and len(s.encode('utf-8'))<=max_bytes and all(ord(c)>=32 and not 127<=ord(c)<160 for c in s)
    except UnicodeError:return False
def errors(r,buttons=None):
    try:
        from app_form import valid
        f=r['form'];v=r['vars'];s=r['surface'];p=r['input']
        assert isinstance(f,dict) and set(f)=={'engine','page','focus_var','active_var','fields','actions','keyboard'} and f['engine']==ENGINE
        assert r['engine']=='p4-bounded-runtime-v4' and s['engine']=='p4-app-surface-v1' and s['graphics']=='p4-app-graphics-v1' and s['bindings']=='p4-variable-bindings-v1' and p['engine']=='p4-input-v1'
        from variable_owners import mixed
        assert ('host' not in r or mixed(r)) and not any(k in r for k in ('local','scene','game','data','media','instrument'))
        def no_nul(x):
            if isinstance(x,str):return '\0' not in x
            if isinstance(x,list):return all(no_nul(a) for a in x)
            if isinstance(x,dict):return all(no_nul(k) and no_nul(a) for k,a in x.items())
            return True
        assert no_nul(r) and isinstance(v,dict) and len(v)<=8 and 1<=len(r['pages'])<=4 and f['page'] in [q['id'] for q in r['pages']]
        assert isinstance(f['fields'],list) and 1<=len(f['fields'])<=3
        owned=set();ephemeral=set();ids=set();strings=set()
        def role(name,kind,initial=None,bounds=None,transient=True):
            assert isinstance(name,str) and re.fullmatch(r'[A-Za-z0-9_.-]{1,31}',name) and name not in owned and v[name]['type']==kind
            value=v[name].get('init',0 if kind=='int' else '')
            assert type(value) is int and -10**9<=value<=10**9 if kind=='int' else utf8(value)
            if initial is not None:assert value==initial and type(value) is type(initial)
            if bounds and name in r.get('limits',{}):
                lo,hi=r['limits'][name];assert type(lo) is int and type(hi) is int and -10**9<=lo<=bounds[0]<=bounds[1]<=hi<=10**9
            if kind=='string':assert name not in r.get('limits',{})
            owned.add(name)
            if transient:ephemeral.add(name)
            return value
        role(f['focus_var'],'int',0,(0,len(f['fields'])-1));role(f['active_var'],'int',0,(0,1))
        for q in f['fields']:
            assert isinstance(q,dict) and isinstance(q['id'],str) and re.fullmatch(r'[A-Za-z0-9_.-]{1,15}',q['id']) and q['id'] not in ids
            ids.add(q['id']);base={'id','var','draft_var','kind'}
            if q['kind']=='text':
                assert set(q)==base|{'max_bytes'} and type(q['max_bytes']) is int and 1<=q['max_bytes']<=63
                value=role(q['var'],'string',transient=False);assert utf8(value,q['max_bytes']);role(q['draft_var'],'string',value);strings.add(q['var'])
            else:
                if q['kind']=='integer':
                    assert set(q)==base|{'min','max','step'} and all(type(q[k]) is int for k in ('min','max','step'))
                    assert -10**9<=q['min']<q['max']<=10**9 and 1<=q['step']<=10**9 and (q['max']-q['min'])%q['step']==0
                    bounds=(q['min'],q['max'])
                else:
                    assert q['kind']=='choice' and set(q)==base|{'options'} and isinstance(q['options'],list) and 2<=len(q['options'])<=8
                    values=[]
                    for o in q['options']:
                        assert set(o)=={'value','label'} and type(o['value']) is int and -10**9<=o['value']<=10**9 and o['value'] not in values and utf8(o['label'],63,False);values.append(o['value'])
                    bounds=(min(values),max(values))
                value=role(q['var'],'int',bounds=bounds,transient=False);assert valid(q,value);role(q['draft_var'],'int',value,bounds)
        assert any(q['kind']=='text' for q in f['fields'])
        k=f['keyboard'];assert isinstance(k,dict) and {'index_var','label_var','characters'}<=set(k)<= {'index_var','label_var','characters','space_label'}
        chars=k['characters'];assert isinstance(chars,list) and 1<=len(chars)<=96 and all(utf8(c,4,False) and len(c)==1 for c in chars) and len(set(chars))==len(chars)
        space=k.get('space_label','Space');assert utf8(space,16,False)
        role(k['index_var'],'int',0,(0,len(chars)-1));role(k['label_var'],'string',space if chars[0]==' ' else chars[0])
        # All remaining declarations still obey the native slot representation.
        for d in v.values():
            assert d['type'] in ('int','string')
            if d['type']=='string':assert utf8(d.get('init',''))
            else:assert type(d.get('init',0)) is int and -10**9<=d.get('init',0)<=10**9
        actions=f['actions'];assert set(actions)==set(OPS) and len(set(actions.values()))==8
        reserved={'page_toggle','page_enter','page_back','page_main','page_app','component_center'}
        for a in actions.values():
            assert isinstance(a,str) and re.fullmatch(r'[A-Za-z0-9_.-]{1,47}',a) and a not in reserved and not a.startswith(('host.','input.'))
            assert [t for t in r['transitions'] if t.get('on')==a]==[{'from':'*','on':a}]
            assert a not in p.get('press',[]) and a!=p.get('hold') and a not in p.get('confirm',{}) and a not in p.get('cooldown',{})
        assert not {actions[k] for k in ('accept','cancel','append','erase')}&set(p.get('repeat',[]))
        for t in r['transitions']:
            for branch in (t,t.get('else',{})):assert not owned&(set(branch.get('set',{}))|set(branch.get('inc',{})))
        for t in r.get('tick',[]):
            for x in (t,t.get('then',{})):assert not owned&(set(x.get('set',{}))|set(x.get('inc',{})))
        assert not ephemeral&set(r.get('persist',{}).get('vars',[]))
        for n in s.get('elements',[]):
            if 'field_bind' in n:assert n['field_bind'] in ids and n.get('type') in ('text','button') and not any(k in n for k in ('text','bind','var_bind','key_for'))
        if buttons is not None:
            assert no_nul(buttons) and isinstance(buttons,list) and len(buttons)==8 and {b.get('event') for b in buttons}==PHYSICAL and {b.get('action') for b in buttons}==set(actions.values())
        return []
    except (AssertionError,KeyError,TypeError,ValueError,AttributeError,UnicodeError):return ['form v2 requires bounded UTF-8 fields, separate drafts, eight reachable actions and committed-only persistence']
