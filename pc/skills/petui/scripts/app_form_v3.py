"""p4-app-form-v3: multi-field text forms with the on-device keyboard engine.

Pure Python mirror of firmware/main/pet_p4_form_v3.c (the firmware pytest
cross-checks both on the shared contract vectors and a fuzz hash); the pinyin
candidates come from the generated pinyin-table.json next to this file.
"""
import json
import re
from pathlib import Path
ENGINE='p4-app-form-v3'
OPS=('previous','next','decrease','increase','accept','cancel','append','erase')
MODES=('numeric','latin','pinyin')
NUMERIC_KEYS='0123456789.-'
GROUPS=(' 0','1.-_@','abc2','def3','ghi4','jkl5','mno6','pqrs7','tuv8','wxyz9')
PHYSICAL={'button.sw1.short_press','button.sw2.short_press','button.encoder.short_press','button.encoder.long_press','knob.rotate_ccw','knob.rotate_cw','joystick.up','joystick.down'}
RESERVED={'page_toggle','page_enter','page_back','page_main','page_app','component_center','tab.next','tab.prev','data.refresh'}
BUFFER_DIGITS=6
CANDIDATES_MAX=64
PAGE=5
MAX_VARS=32
_TABLE=None
def table():
    global _TABLE
    if _TABLE is None:_TABLE=json.loads(Path(__file__).with_name('pinyin-table.json').read_text(encoding='utf-8'))['syllables']
    return _TABLE
def utf8(s,max_bytes=63,empty=True):
    try:return isinstance(s,str) and (empty or bool(s)) and len(s.encode('utf-8'))<=max_bytes and all(ord(c)>=32 and not 127<=ord(c)<160 for c in s)
    except UnicodeError:return False
def digits_ok(d):return isinstance(d,str) and 1<=len(d)<=BUFFER_DIGITS and all('2'<=c<='9' for c in d)
def candidates(digits):
    if not digits_ok(digits):return []
    out=[]
    for exact in (True,False):
        for s in table():
            code=s['t9'];match=code==digits if exact else (code!=digits and code.startswith(digits))
            if not match:continue
            for ch in s['chars']:
                if ch not in out:out.append(ch)
                if len(out)>=CANDIDATES_MAX:return out
    return out
def syllables(digits):
    if not digits_ok(digits):return ''
    out='';
    for s in table():
        if s['t9']!=digits:continue
        if len((out+('|' if out else '')+s['pinyin']).encode())>=64:break
        out=out+('|' if out else '')+s['pinyin']
    return out
def _key_count(mode):return 12 if mode=='numeric' else 10
def _default_key(mode):return 0 if mode=='numeric' else 2
def _roles(f):
    k=f['keyboard'];return [f['focus_var'],f['active_var'],k['key_var'],k['cursor_var'],k['buffer_var'],k['label_var'],k['candidates_var']]
def refresh(runtime,state):
    f=runtime['form'];v=state['vars'];k=f['keyboard'];q=f['fields'][v[f['focus_var']]];label='';cands=''
    if v[f['active_var']]==1:
        key=v[k['key_var']]
        if q['keyboard']=='numeric':label=NUMERIC_KEYS[key]
        elif q['keyboard']=='latin':
            g=GROUPS[key];cursor=v[k['cursor_var']]
            label=''.join(('[' if i==cursor else '')+('_' if c==' ' else c)+(']' if i==cursor else '') for i,c in enumerate(g))
        else:
            buffer=v[k['buffer_var']]
            if buffer:
                label=syllables(buffer) or buffer
                lst=candidates(buffer);cursor=v[k['cursor_var']];start=cursor//PAGE*PAGE
                parts=[('['+c+']' if i==cursor else c) for i,c in enumerate(lst) if start<=i<start+PAGE]
                cands=' '.join(parts)
            else:label=GROUPS[key]
    v[k['label_var']]=label[:63];v[k['candidates_var']]=cands
def _reset_keyboard(runtime,state):
    f=runtime['form'];v=state['vars'];k=f['keyboard'];q=f['fields'][v[f['focus_var']]]
    v[k['key_var']]=_default_key(q['keyboard']);v[k['cursor_var']]=0;v[k['buffer_var']]=''
def _reset(runtime,state):
    f=runtime['form'];v=state['vars']
    for q in f['fields']:v[q['draft_var']]=v[q['var']]
    v[f['active_var']]=0
    if not 0<=v[f['focus_var']]<len(f['fields']):v[f['focus_var']]=0
    _reset_keyboard(runtime,state);refresh(runtime,state)
def initialize(runtime,state):
    state['vars'][runtime['form']['focus_var']]=0;_reset(runtime,state);return state
def cancel(runtime,state):_reset(runtime,state)
def operation(runtime,action):return next((k for k,a in runtime['form']['actions'].items() if a==action),None)
def _fits(q,draft,extra):return len(draft.encode())+extra<=q['max_bytes']
def allowed(runtime,state,op):
    f=runtime['form'];v=state['vars'];k=f['keyboard']
    if state['page']!=f['page'] or op not in OPS:return False
    focus=v[f['focus_var']];q=f['fields'][focus];active=v[f['active_var']]==1;mode=q['keyboard']
    buffer=v[k['buffer_var']];key=v[k['key_var']];cursor=v[k['cursor_var']]
    if not active:
        if op=='previous':return focus>0
        if op=='next':return focus+1<len(f['fields'])
        return op=='accept'
    if op in ('cancel','decrease','increase'):return True
    draft=v[q['draft_var']]
    if op=='accept':
        if mode=='pinyin' and buffer:
            lst=candidates(buffer);return bool(lst) and cursor<len(lst) and _fits(q,draft,len(lst[cursor].encode()))
        return all(utf8(v[p['draft_var']],p['max_bytes']) for p in f['fields'])
    if op in ('previous','next'):
        if mode=='numeric':return False
        if mode=='latin':return cursor>0 if op=='previous' else cursor+1<len(GROUPS[key])
        if not buffer:return False
        count=len(candidates(buffer));return cursor>0 if op=='previous' else cursor+1<count
    if op=='append':
        if mode=='numeric':return _fits(q,draft,1)
        if mode=='latin':return cursor<len(GROUPS[key]) and _fits(q,draft,1)
        return len(buffer)<BUFFER_DIGITS and bool(candidates(buffer+str(key)))
    if op=='erase':
        return bool(buffer) if mode=='pinyin' and buffer else len(draft.encode())>0
    return False
def apply(runtime,state,op):
    if not allowed(runtime,state,op):return False
    f=runtime['form'];v=state['vars'];k=f['keyboard'];focus=v[f['focus_var']];q=f['fields'][focus];active=v[f['active_var']]==1;mode=q['keyboard']
    if not active:
        if op in ('previous','next'):v[f['focus_var']]+=1 if op=='next' else -1
        else:
            for p in f['fields']:v[p['draft_var']]=v[p['var']]
            v[f['active_var']]=1;_reset_keyboard(runtime,state)
    elif op=='cancel':
        if mode=='pinyin' and v[k['buffer_var']]:v[k['buffer_var']]='';v[k['cursor_var']]=0
        else:_reset(runtime,state);return True
    elif op in ('decrease','increase'):
        n=_key_count(mode);lo=2 if mode=='pinyin' else 0;span=n-lo
        v[k['key_var']]=lo+(v[k['key_var']]-lo+(1 if op=='increase' else span-1))%span
        if mode!='pinyin':v[k['cursor_var']]=0
    elif op in ('previous','next'):v[k['cursor_var']]+=1 if op=='next' else -1
    elif op=='append':
        if mode=='numeric':v[q['draft_var']]+=NUMERIC_KEYS[v[k['key_var']]]
        elif mode=='latin':v[q['draft_var']]+=GROUPS[v[k['key_var']]][v[k['cursor_var']]]
        else:v[k['buffer_var']]+=str(v[k['key_var']]);v[k['cursor_var']]=0
    elif op=='erase':
        if mode=='pinyin' and v[k['buffer_var']]:v[k['buffer_var']]=v[k['buffer_var']][:-1];v[k['cursor_var']]=0
        else:v[q['draft_var']]=v[q['draft_var']][:-1]
    elif op=='accept':
        if mode=='pinyin' and v[k['buffer_var']]:
            v[q['draft_var']]+=candidates(v[k['buffer_var']])[v[k['cursor_var']]];v[k['buffer_var']]='';v[k['cursor_var']]=0
        else:
            for p in f['fields']:v[p['var']]=v[p['draft_var']]
            v[f['active_var']]=0;_reset_keyboard(runtime,state)
    refresh(runtime,state);return True
def text(runtime,state,field_id):
    f=runtime['form'];q=next((q for q in f['fields'] if q['id']==field_id),None)
    if q is None:return ''
    return state['vars'][q['draft_var'] if state['vars'][f['active_var']]==1 else q['var']]
def errors(r,buttons=None):
    try:
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
        max_vars,max_string,max_pages=(32,127,12) if r.get('quota')=='p4-quota-v3' else (16,63,6) if r.get('quota')=='p4-quota-v2' else (8,63,4)
        assert no_nul(r) and isinstance(v,dict) and 1<=len(v)<=max_vars
        assert 1<=len(r['pages'])<=max_pages and f['page'] in [q['id'] for q in r['pages']]
        assert isinstance(f['fields'],list) and 1<=len(f['fields'])<=4
        owned=set();ephemeral=set();ids=set()
        def role(name,kind,initial=None,bounds=None,transient=True):
            assert isinstance(name,str) and re.fullmatch(r'[A-Za-z0-9_.-]{1,31}',name) and name not in owned and v[name]['type']==kind
            value=v[name].get('init',0 if kind=='int' else '')
            assert type(value) is int and -10**9<=value<=10**9 if kind=='int' else utf8(value)
            if initial is not None:assert value==initial
            if bounds and name in r.get('limits',{}):
                lo,hi=r['limits'][name];assert type(lo) is int and type(hi) is int and lo<=bounds[0]<=bounds[1]<=hi
            if kind=='string':assert name not in r.get('limits',{})
            owned.add(name)
            if transient:ephemeral.add(name)
            return value
        role(f['focus_var'],'int',0,(0,len(f['fields'])-1));role(f['active_var'],'int',0,(0,1))
        for q in f['fields']:
            assert isinstance(q,dict) and set(q)=={'id','var','draft_var','kind','max_bytes','keyboard'} and q['kind']=='text' and q['keyboard'] in MODES
            assert isinstance(q['id'],str) and re.fullmatch(r'[A-Za-z0-9_.-]{1,15}',q['id']) and q['id'] not in ids;ids.add(q['id'])
            assert type(q['max_bytes']) is int and 1<=q['max_bytes']<=63
            value=role(q['var'],'string',transient=False);assert utf8(value,q['max_bytes']);role(q['draft_var'],'string',value)
        k=f['keyboard'];assert isinstance(k,dict) and set(k)=={'key_var','cursor_var','buffer_var','label_var','candidates_var'}
        role(k['key_var'],'int',0,(0,11));role(k['cursor_var'],'int',0,(0,CANDIDATES_MAX-1))
        for name in ('buffer_var','label_var','candidates_var'):role(k[name],'string','')
        for d in v.values():
            assert d['type'] in ('int','string')
            if d['type']=='string':assert utf8(d.get('init',''),max_string)
            else:assert type(d.get('init',0)) is int and -10**9<=d.get('init',0)<=10**9
        actions=f['actions'];assert set(actions)==set(OPS) and len(set(actions.values()))==8
        for a in actions.values():
            assert isinstance(a,str) and re.fullmatch(r'[A-Za-z0-9_.-]{1,47}',a) and a not in RESERVED and not a.startswith(('host.','input.'))
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
    except (AssertionError,KeyError,TypeError,ValueError,AttributeError,UnicodeError,IndexError):return ['form v3 requires 1-4 bounded UTF-8 text fields with a keyboard mode, separate drafts, five keyboard vars, eight reachable actions and committed-only persistence']
def structural_disabled(runtime):
    f=runtime.get('form',{});result={}
    if f.get('engine')!=ENGINE or not f.get('fields'):return result
    if len(f['fields'])==1:
        for op in ('previous','next'):
            if all(q.get('keyboard')=='numeric' for q in f['fields']):result[f['actions'][op]]='single_numeric_field'
    return result
