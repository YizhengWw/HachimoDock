"""Bounded local editor: draft values never become committed without accept."""
import re
ENGINE='p4-app-form-v1'
OPS=('previous','next','decrease','increase','accept','cancel')
EVENTS={'button.sw1.short_press','button.sw2.short_press','button.encoder.short_press','button.encoder.long_press','knob.rotate_ccw','knob.rotate_cw','joystick.up','joystick.down'}
def structural_disabled(runtime):
    """Structural impossibility only; run after declaration validation."""
    f=runtime.get('form',{});result={}
    if f.get('engine')=='p4-app-form-v3':
        from app_form_v3 import structural_disabled as v3_disabled
        return v3_disabled(runtime)
    if f.get('engine') not in (ENGINE,'p4-app-form-v2') or not f.get('fields'):return result
    def mark(op,why):
        action=f.get('actions',{}).get(op)
        if isinstance(action,str):result[action]=why
    if len(f['fields'])==1:
        mark('previous','single_field');mark('next','single_field')
    if f['engine']=='p4-app-form-v2' and all(q.get('kind')=='text' for q in f['fields']) and len(f.get('keyboard',{}).get('characters',[]))==1:
        mark('decrease','single_character_keyboard');mark('increase','single_character_keyboard')
    return result
def valid(field,value):
    return type(value) is int and (value in [o['value'] for o in field['options']] if field['kind']=='choice' else field['min']<=value<=field['max'] and (value-field['min'])%field['step']==0)
def operation(runtime,action):
    return next((k for k,v in runtime.get('form',{}).get('actions',{}).items() if v==action),None)
def _v3(runtime):return runtime.get('form',{}).get('engine')=='p4-app-form-v3'
def cancel(runtime,state):
    if _v3(runtime):
        from app_form_v3 import cancel as v3_cancel
        v3_cancel(runtime,state);return
    if runtime.get('form',{}).get('engine')=='p4-app-form-v2':
        from app_form_v2 import invoke
        invoke(runtime,state,'cancel');return
    f=runtime.get('form')
    if f:
        state['vars'][f['active_var']]=0
        for field in f['fields']:state['vars'][field['draft_var']]=state['vars'][field['var']]
def initialize(runtime,state):
    if _v3(runtime):
        from app_form_v3 import initialize as v3_initialize
        return v3_initialize(runtime,state)
    if runtime.get('form',{}).get('engine')=='p4-app-form-v2':
        from app_form_v2 import invoke
        invoke(runtime,state,'initialize');return state
    cancel(runtime,state)
    if runtime.get('form'):state['vars'][runtime['form']['focus_var']]=0
    return state
def allowed(runtime,state,op):
    if _v3(runtime):
        from app_form_v3 import allowed as v3_allowed
        return v3_allowed(runtime,state,op)
    if runtime.get('form',{}).get('engine')=='p4-app-form-v2':
        from app_form_v2 import invoke
        return invoke(runtime,state,'allowed',op)
    f=runtime.get('form')
    if not f or op not in OPS or state['page']!=f['page']:return False
    v=state['vars'];focus=v[f['focus_var']];active=v[f['active_var']]==1
    if op=='previous':return focus>0
    if op=='next':return focus+1<len(f['fields'])
    if op=='cancel':return active
    if op=='accept':return not active or all(valid(q,v[q['draft_var']]) for q in f['fields'])
    q=f['fields'][focus];value=v[q['draft_var']]
    if not active or not valid(q,value):return False
    if q['kind']=='choice':
        i=[o['value'] for o in q['options']].index(value)
        return i>0 if op=='decrease' else i+1<len(q['options'])
    return value>q['min'] if op=='decrease' else value<q['max']
def apply(runtime,state,op):
    if _v3(runtime):
        from app_form_v3 import apply as v3_apply
        return v3_apply(runtime,state,op)
    if runtime.get('form',{}).get('engine')=='p4-app-form-v2':
        from app_form_v2 import invoke
        return invoke(runtime,state,'apply',op)
    if not allowed(runtime,state,op):return False
    f=runtime['form'];v=state['vars'];q=f['fields'][v[f['focus_var']]]
    if op in ('previous','next'):v[f['focus_var']]+=1 if op=='next' else -1
    elif op=='cancel':cancel(runtime,state)
    elif op=='accept':
        if v[f['active_var']]==1:
            for q in f['fields']:v[q['var']]=v[q['draft_var']]
            v[f['active_var']]=0
        else:
            for q in f['fields']:v[q['draft_var']]=v[q['var']] if valid(q,v[q['var']]) else q['options'][0]['value'] if q['kind']=='choice' else q['min']
            v[f['active_var']]=1
    elif q['kind']=='choice':
        values=[o['value'] for o in q['options']];v[q['draft_var']]=values[values.index(v[q['draft_var']])+(1 if op=='increase' else -1)]
    else:v[q['draft_var']]+=q['step']*(1 if op=='increase' else -1)
    return True
def text(runtime,state,field_id):
    if _v3(runtime):
        from app_form_v3 import text as v3_text
        return v3_text(runtime,state,field_id)
    if runtime.get('form',{}).get('engine')=='p4-app-form-v2':
        from app_form_v2 import invoke
        return invoke(runtime,state,'text',field_id)
    f=runtime.get('form',{});q=next((q for q in f.get('fields',[]) if q['id']==field_id),None)
    if q is None:return ''
    value=state['vars'][q['draft_var'] if state['vars'][f['active_var']]==1 else q['var']]
    return next((o['label'] for o in q['options'] if o['value']==value),'—') if q['kind']=='choice' else str(value)
def errors(runtime,buttons=None):
    if _v3(runtime):
        from app_form_v3 import errors as v3_errors
        return v3_errors(runtime,buttons)
    if runtime.get('form',{}).get('engine')=='p4-app-form-v2':
        from app_form_v2 import errors as mixed_errors
        return mixed_errors(runtime,buttons)
    try:
        f=runtime.get('form');nodes=runtime.get('surface',{}).get('elements',[])
        if 'form' not in runtime:
            assert not any('field_bind' in n for n in nodes)
            return []
        def no_nul(v):
            if isinstance(v,str):return '\0' not in v
            if isinstance(v,dict):return all(no_nul(k) and no_nul(x) for k,x in v.items())
            if isinstance(v,list):return all(no_nul(x) for x in v)
            return True
        assert no_nul(runtime)
        assert isinstance(f,dict) and set(f)=={'engine','page','focus_var','active_var','fields','actions'} and f['engine']==ENGINE
        s=runtime.get('surface',{});p=runtime.get('input',{})
        assert runtime.get('engine')=='p4-bounded-runtime-v4' and s.get('engine')=='p4-app-surface-v1' and s.get('graphics')=='p4-app-graphics-v1' and s.get('bindings')=='p4-variable-bindings-v1' and p.get('engine')=='p4-input-v1'
        from variable_owners import mixed
        assert ('host' not in runtime or mixed(runtime)) and not any(k in runtime for k in ('local','scene','game','data','media','instrument'))
        assert f['page'] in [q['id'] for q in runtime['pages']] and 1<=len(runtime['pages'])<=4
        fields=f['fields'];assert isinstance(fields,list) and 1<=len(fields)<=3
        names=[f['focus_var'],f['active_var']];ranges=[(0,len(fields)-1),(0,1)];ids=[]
        for q in fields:
            assert isinstance(q,dict) and isinstance(q['id'],str) and re.fullmatch(r'[A-Za-z0-9_.-]{1,15}',q['id']) and q['id'] not in ids
            ids.append(q['id']);base={'id','var','draft_var','kind'}
            if q['kind']=='choice':
                assert set(q)==base|{'options'} and isinstance(q['options'],list) and 2<=len(q['options'])<=8
                values=[]
                for o in q['options']:
                    assert set(o)=={'value','label'} and type(o['value']) is int and -10**9<=o['value']<=10**9 and o['value'] not in values
                    assert isinstance(o['label'],str) and 1<=len(o['label'].encode())<=63 and not any(ord(c)<32 or ord(c)==127 for c in o['label'])
                    values.append(o['value'])
                bounds=(min(values),max(values))
            else:
                assert q['kind']=='integer' and set(q)==base|{'min','max','step'}
                assert all(type(q[k]) is int for k in ('min','max','step')) and -10**9<=q['min']<q['max']<=10**9 and 1<=q['step']<=10**9 and (q['max']-q['min'])%q['step']==0
                bounds=(q['min'],q['max'])
            names.extend([q['var'],q['draft_var']]);ranges.extend([bounds,bounds])
        assert len(set(names))==len(names)
        variables=runtime['vars']
        assert isinstance(variables,dict) and len(variables)<=8
        assert all(isinstance(n,str) and re.fullmatch(r'[A-Za-z0-9_.-]{1,31}',n) and variables[n]['type']=='int' for n in names)
        assert all(type(variables[n].get('init',0)) is int for n in names)
        assert variables[f['focus_var']].get('init',0)==0 and variables[f['active_var']].get('init',0)==0
        for q in fields:assert valid(q,variables[q['var']].get('init',0)) and variables[q['draft_var']].get('init',0)==variables[q['var']].get('init',0)
        for n,(lo,hi) in zip(names,ranges):
            if n in runtime.get('limits',{}):assert runtime['limits'][n][0]<=lo<=hi<=runtime['limits'][n][1]
        actions=f['actions'];assert set(actions)==set(OPS) and len(set(actions.values()))==6
        reserved={'page_toggle','page_enter','page_back','page_main','page_app','component_center'}
        for a in actions.values():
            assert isinstance(a,str) and re.fullmatch(r'[A-Za-z0-9_.-]{1,47}',a) and not a.startswith(('input.','host.')) and a not in reserved
            matching=[t for t in runtime['transitions'] if t.get('on')==a]
            assert matching==[{'from':'*','on':a}]
            assert a not in p.get('press',[]) and a!=p.get('hold') and a not in p.get('confirm',{}) and a not in p.get('cooldown',{})
        assert not set([actions['accept'],actions['cancel']])&set(p.get('repeat',[]))
        ephemeral=set(names)-{q['var'] for q in fields}
        for t in runtime['transitions']:
            for branch in (t,t.get('else',{})):assert not ephemeral&(set(branch.get('set',{}))|set(branch.get('inc',{})))
        for t in runtime.get('tick',[]):
            for r in (t,t.get('then',{})):assert not set(names)&(set(r.get('set',{}))|set(r.get('inc',{})))
        assert not ephemeral&set(runtime.get('persist',{}).get('vars',[]))
        for n in nodes:
            if 'field_bind' in n:assert n['field_bind'] in ids and n.get('type') in ('text','button') and not any(k in n for k in ('text','bind','var_bind','key_for'))
        if buttons is not None:
            assert isinstance(buttons,list) and len(buttons)<=8
            events=[b.get('event') for b in buttons];bound=[b.get('action') for b in buttons]
            assert no_nul(buttons) and len(set(events))==len(events) and len(set(bound))==len(bound)
            assert all(e in EVENTS|{'screen.region.tap','screen.region.long_press'} for e in events) and not reserved&set(bound)
            for a in actions.values():assert any(b.get('action')==a and b.get('event') in EVENTS for b in buttons)
        return []
    except (AssertionError,KeyError,TypeError,ValueError,AttributeError):return ['form requires bounded local integer/choice fields, atomic actions, physical bindings and committed-only persistence']
