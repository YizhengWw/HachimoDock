"""Pure bounded program declaration/ownership schema. No execution or IO."""
import math
import re
from variable_owners import parse as variable_owners
ENGINE='p4-component-program-v1'
OPS=('HALT','CONST','MOVE','ADD','SUB','MUL','DIV','MOD','LT','EQ','JUMP','JZ','LOAD','STORE','EVENT','OUTPUT')
def number(v,lo,hi):return type(v) in (int,float) and math.isfinite(v) and lo<=v<=hi and int(v)==v
def name(v,limit):return isinstance(v,str) and re.fullmatch(r'[A-Za-z0-9_.-]{1,%d}'%limit,v) is not None
def need(v):
    if not v:raise ValueError('Invalid bounded component program or output ownership')
def image(p, runtime=None):
    v2=isinstance(p,dict) and p.get('engine')=='p4-integer-program-v2'
    ops=OPS+('TEXT','CELLGET','CELLSET') if v2 else OPS
    need(isinstance(p,dict) and set(p)=={'engine','schema','cells','fuel','code'} and p['engine'] in ('p4-integer-program-v1','p4-integer-program-v2'))
    need(number(p['schema'],1,4294967295) and number(p['cells'],0,256 if v2 else 64) and number(p['fuel'],1,16384 if v2 else 4096))
    code=p['code'];need(isinstance(code,list) and 1<=len(code)<=(1024 if v2 else 256))
    for row in code:
        need(isinstance(row,list) and len(row)==5 and row[0] in ops and all(number(v,0,255) for v in row[1:4]) and number(row[4],-2147483648,2147483647))
        op,a,b,c,imm=row
        if op=='HALT':need(a==b==c==imm==0)
        elif op=='CONST':need(a<16 and b==c==0)
        elif op in ('MOVE','LOAD','STORE'):need(a<16 and b<16 and c==imm==0)
        elif op in ('ADD','SUB','MUL','DIV','MOD','LT','EQ'):need(a<16 and b<16 and c<16 and imm==0)
        elif op in ('JUMP','JZ'):need(a<16 and (op!='JUMP' or a==0) and b==c==0 and 0<=imm<len(code))
        elif op=='EVENT':need(a<16 and b==c==0 and 0<=imm<4)
        elif op=='TEXT':need(a<16 and b<16 and c==imm==0)
        elif op in ('CELLGET','CELLSET'):
            need(a<16 and b<16 and c<16 and imm==0)
            if runtime is not None:
                from cells import grid
                shape=grid(runtime);need(shape is not None and shape[0]*shape[1]<=p['cells'])
        elif op=='OUTPUT':need(a<8 and b<16 and c==imm==0)
    return p
def validate(runtime):
    if 'program' not in runtime:return None
    p=runtime['program'];need(isinstance(p,dict) and set(p)=={'engine','image','outputs','actions','timers'} and p['engine'] in (ENGINE,'p4-component-program-v2'))
    need(runtime.get('engine')=='p4-bounded-runtime-v4' and runtime.get('surface',{}).get('engine')=='p4-app-surface-v1' and runtime.get('input',{}).get('engine')=='p4-input-v1')
    writers=variable_owners(runtime)
    need(('host' not in runtime or writers['mixed']) and not any(k in runtime for k in ('local','scene','game','media','instrument','data')));image(p['image'],runtime)
    image_v2=p['image']['engine']=='p4-integer-program-v2'
    need(not image_v2 or p['engine']=='p4-component-program-v2')
    if 'cells' in runtime:
        from cells import grid
        shape=grid(runtime);need(image_v2 and shape is not None and shape[0]*shape[1]<=p['image']['cells'])
    if 'texts' in runtime:need(image_v2 and isinstance(runtime['texts'],list) and 1<=len(runtime['texts'])<=64)
    vars=runtime.get('vars');need(isinstance(vars,dict) and 1<=len(vars)<=8 and all(name(k,31) and isinstance(v,dict) and v.get('type') in ('int','string') for k,v in vars.items()))
    outputs=p['outputs'];need(isinstance(outputs,list) and len(outputs)==8)
    owned=[k for k in outputs if k is not None]
    need(owned and all(name(k,31) and vars.get(k,{}).get('type')=='int' for k in owned) and len(set(owned))==len(owned))
    for k,v in vars.items():
        if v['type']=='int' and k in runtime.get('limits',{}):
            bounds=runtime['limits'][k];need(isinstance(bounds,list) and len(bounds)==2 and number(bounds[0],-10**9,10**9) and number(bounds[1],bounds[0],10**9))
    for rule in [*runtime.get('transitions',[]),*runtime.get('tick',[])]:
        for body in [rule,rule.get('then',{}),rule.get('else',{})]:
            need(not any(k in owned for k in [*body.get('set',{}),*body.get('inc',{})]))
    need(not any(k in owned for k in runtime.get('persist',{}).get('vars',[])))
    form=runtime.get('form',{});need(not any(form.get(k) in owned for k in ('focus_var','active_var')))
    need(not any(form.get('keyboard',{}).get(k) in owned for k in ('index_var','label_var')))
    need(not any(f.get(k) in owned for f in form.get('fields',[]) for k in ('var','draft_var')))
    actions=p['actions'];need(isinstance(actions,dict) and 1<=len(actions)<=24)
    for action,words in actions.items():
        need(action not in form.get('actions',{}).values())
        need(name(action,47) and not action.startswith('host.') and isinstance(words,list) and len(words)<=4)
        rules=[t for t in runtime.get('transitions',[]) if t.get('on')==action]
        need(rules and all(set(t)=={'from','on'} and t['from']=='*' for t in rules))
        for word in words:
            if number(word,-2147483648,2147483647):continue
            need(isinstance(word,dict) and set(word)=={'var'} and isinstance(word['var'],str) and vars.get(word['var'],{}).get('type')=='int')
    timers=p['timers'];need(isinstance(timers,list) and len(timers)<=8);ids=set()
    for t in timers:
        need(isinstance(t,dict) and set(t)=={'id','delayMs','intervalMs','words'} and number(t['id'],1,65535) and t['id'] not in ids)
        need(number(t['delayMs'],0,4294967295) and number(t['intervalMs'],0,4294967295) and (t['intervalMs']==0 or t['intervalMs']>=100))
        need(isinstance(t['words'],list) and len(t['words'])<=4 and all(number(v,-2147483648,2147483647) for v in t['words']));ids.add(t['id'])
    return p
def errors(runtime):
    try:validate(runtime);return []
    except (AssertionError,ValueError,TypeError,KeyError,AttributeError,OverflowError):return ['组件程序需要有界纯整数指令、明确输入和唯一输出所有权；已有数据保留']
