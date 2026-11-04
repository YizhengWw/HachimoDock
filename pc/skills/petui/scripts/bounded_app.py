"""Deterministic v4 application model for preview generation and behavioral checks."""
import copy
from collections import deque
from device_json import strings_valid_text,value_within_depth,DEVICE_JSON_MAX_DEPTH
from app_navigation import allowed as navigation_allowed,apply as navigate
from app_form import initialize as form_initialize,operation as form_operation,apply as form_apply,cancel as form_cancel,text as form_text,structural_disabled
from input_policy import cooldown_allowed,action_accepted

def initial(runtime):
    if not strings_valid_text(runtime):raise ValueError('Device JSON strings cannot contain NUL or unpaired Unicode surrogates')
    if not value_within_depth(runtime):raise ValueError(f'Device JSON exceeds container depth budget ({DEVICE_JSON_MAX_DEPTH})')
    return form_initialize(runtime,{'state':runtime['initial_state'],'page':runtime.get('initial_page',runtime['pages'][0]['id'] if runtime.get('form') else ''),
            'vars':{k:v.get('init',0 if v['type']=='int' else '') for k,v in runtime['vars'].items()},'ms':0,'ticks':[0]*len(runtime['tick'])})

_COMPARE={'lte':lambda a,b:a<=b,'lt':lambda a,b:a<b,'gte':lambda a,b:a>=b,'gt':lambda a,b:a>b,'eq':lambda a,b:a==b,'ne':lambda a,b:a!=b}
def condition_holds(when,variables):
    """p4-logic-v1 guard shared by transitions, ticks and scene rules; same table as pet_p4_game_compare."""
    if not when:return True
    if isinstance(when.get('all'),list):return all(condition_holds(c,variables) for c in when['all'])
    left=int(variables.get(when.get('var'),0))
    if 'lte' in when:return left<=int(when['lte'])
    value=when.get('value');right=int(variables.get(value['var'],0)) if isinstance(value,dict) else int(value)
    return _COMPARE.get(when.get('op','lte'),lambda a,b:False)(left,right)

def effects(runtime,s,rule):
    original=dict(s['vars'])
    def clamp(k,v):
        if not isinstance(v,int):return v
        lo,hi=runtime.get('limits',{}).get(k,[-10**9,10**9]);return max(lo,min(hi,v))
    for k,v in rule.get('set',{}).items():s['vars'][k]=clamp(k,original[v['var']] if isinstance(v,dict) else v)
    for k,v in rule.get('inc',{}).items():s['vars'][k]=clamp(k,max(-10**9,min(10**9,s['vars'][k]+(original[v['var']] if isinstance(v,dict) else v))))
    for k,(lo,hi) in runtime.get('limits',{}).items():s['vars'][k]=max(lo,min(hi,s['vars'][k]))

def action(runtime,state,name):
    if not cooldown_allowed(runtime,state,name):return state
    s=copy.deepcopy(state)
    op=form_operation(runtime,name)
    if op:form_apply(runtime,s,op);return s
    for found in runtime['transitions']:
        if found['on']!=name or found['from'] not in ('*',s['state']) or 'from_page' in found and found['from_page']!=s['page']:continue
        # A false guard without else keeps searching, like an unavailable navigation rule.
        taken=condition_holds(found.get('when'),s['vars'])
        if taken and not navigation_allowed(runtime,s,found):continue
        if not taken and not found.get('else'):continue
        r=found if taken else {**found['else'],'on':found['on'],'from':found['from']}
        form_cancel(runtime,s)
        if r.get('to',s['state'])!=s['state']:
            s['state']=r['to'];s['ticks']=[s['ms']]*len(s['ticks'])
        if taken:navigate(runtime,s,r)
        elif r.get('page'):
            pages=[q['id'] for q in runtime.get('pages',[])]
            if pages:
                i=pages.index(s['page']) if s['page'] in pages else 0
                s['page']=pages[(i+1)%len(pages)] if r['page']=='next' else pages[(i-1)%len(pages)] if r['page']=='prev' else r['page']
                if 'navigation' in runtime:s['history']=[]
        effects(runtime,s,r);action_accepted(runtime,s,name);break
    return s

def advance(runtime,state,ms):
    s=copy.deepcopy(state);s['ms']+=max(0,ms)
    for i,t in enumerate(runtime['tick']):
        if t.get('while_state',s['state'])!=s['state']:s['ticks'][i]=s['ms'];continue
        steps=min(10,(s['ms']-s['ticks'][i])//t['every_ms'])
        for _ in range(steps):
            effects(runtime,s,t);s['ticks'][i]+=t['every_ms']
            if 'when' in t and condition_holds(t['when'],s['vars']):
                s['state']=t.get('then',{}).get('to',s['state']);effects(runtime,s,t.get('then',{}));s['ticks']=[s['ms']]*len(s['ticks']);break
    return s

def dashboard(runtime,state):
    from validate_generated_widget import evaluate_dashboard_value
    return {k:evaluate_dashboard_value(v,state=state['state'],page=state['page'],variables=state['vars']) for k,v in runtime['dashboard'].items()}

def visible(runtime,state):
    from app_viewport import project
    values=dashboard(runtime,state)
    page=max(0,next((i for i,p in enumerate(runtime.get('pages',[])) if p['id']==state['page']),0))
    result=[]
    for i,n in enumerate(runtime['surface']['elements']):
        projected=project(runtime['surface'],n,state['vars'])
        if projected is None:continue
        if n.get('page',4) not in (page,4):continue
        show=n.get('show')
        if n.get('state_mask') and not n['state_mask'] & (1<<runtime['states'].index(state['state'])):continue
        if show and not show['min']<=state['vars'][show['var']]<=show['max']:continue
        tile=n.get('tile');frame=None
        if tile:
            v=state['vars'][tile['var']];cols=tile['columns']
            if 'max_var' in tile:
                total=state['vars'][tile['max_var']]
                frame=max(0,min(cols-1,v*cols//total)) if total>0 else 0
            else:frame=v%cols
        value=form_text(runtime,state,n['field_bind']) if n.get('field_bind') else state['vars'].get(n['var_bind'],'') if n.get('var_bind') else values.get(n.get('bind')) if n.get('bind') else n.get('text',n.get('color'))
        if n['type']=='meter' and n.get('var_bind'):value=max(0,min(100,value))
        result.append((i,value,frame,(projected['x'],projected['y'])) if n.get('in_view') or 'view' in n else (i,value,frame))
    return result

def smoke(runtime,buttons):
    if 'program' in runtime:
        import json,shutil,subprocess
        from pathlib import Path
        node=shutil.which('node')
        if not node:return {'ok':False,'errors':['Component programs require the shared native Node.js verification']}
        try:
            result=subprocess.run([node,str(Path(__file__).with_name('component-program-host.mjs'))],input=json.dumps({'runtime':runtime,'buttons':buttons}),text=True,capture_output=True,timeout=30)
            return json.loads(result.stdout) if result.returncode==0 else {'ok':False,'errors':['Shared native component program verification failed; no services were called']}
        except (ValueError,subprocess.TimeoutExpired):
            return {'ok':False,'errors':['Shared native component program verification unavailable; no services were called']}
    if runtime.get('form',{}).get('engine')=='p4-app-form-v2':
        import json,shutil,subprocess
        from pathlib import Path
        node=shutil.which('node')
        if not node:return {'ok':False,'errors':['Mixed form requires Node.js shared native verification']}
        result=subprocess.run([node,str(Path(__file__).with_name('form-v2-host.mjs'))],input=json.dumps({'mode':'smoke','runtime':runtime,'buttons':buttons}),text=True,capture_output=True,timeout=30)
        return json.loads(result.stdout) if result.returncode==0 else {'ok':False,'errors':['Shared native form verification failed; no services were called']}
    if runtime.get('host') and not runtime.get('ownership'):
        actions={b['action'] for b in buttons}
        errors=[]
        if runtime.get('input',{}).get('engine')=='p4-input-v1':
            actions.update(n['action'] for n in runtime.get('surface',{}).get('elements',[]) if n.get('action'))
            actions.update(runtime.get('input',{}).get('swipe',{}).values())
            surface=runtime.get('surface',{})
            if 'collection_view' in surface:
                from host_collection import config
                try:
                    rows=config(surface['collection_view'],surface,runtime)
                    actions.update(rows['actions'])
                except (ValueError,TypeError,KeyError):
                    errors.append('宿主集合控件声明无效')
        declared={r['on'] for r in runtime['transitions']}
        if actions!=declared or not all(a.startswith('host.') for a in actions):
            errors.append('宿主动作与物理按键映射不一致')
        return {'ok':not errors,'mode':'host-application-contract','actions':sorted(actions),'errors':errors,'deviceVerified':False,'hostServicesVerified':False,'note':'仅验证受控动作声明；须另外验收宿主服务，不能视为联网、录音或播放已通过。'}
    # Values are part of a tool's observable state: a counter's clear action
    # becomes effective only after incrementing, even on the same page/state.
    # Bound the search so an unlimited counter cannot grow the work queue.
    actions=sorted({b['action'] for b in buttons}|{n['action'] for n in runtime.get('surface',{}).get('elements',[]) if n.get('action')}|set(runtime.get('input',{}).get('swipe',{}).values()))
    host_actions=[a for a in actions if runtime.get('ownership') and a.startswith('host.')]
    actions=[a for a in actions if a not in host_actions]
    disabled=structural_disabled(runtime);required=set(actions)-set(disabled)
    queue=deque([(initial(runtime),0)]);seen=set();effective=set();progressed=False;completion=set();truncated=False
    def key(s):return (s['state'],s['page'],tuple(s.get('history',[])),tuple(sorted(s['vars'].items())),tuple(cooldown_allowed(runtime,s,a) for a in actions))
    def enqueue(s,depth):
        nonlocal truncated
        if key(s) in seen:return
        if depth>32:truncated=True;return
        queue.append((s,depth))
    while queue:
        s,depth=queue.popleft();identity=key(s)
        if identity in seen:continue
        if len(seen)>=512:truncated=True;break
        seen.add(identity)
        for name in actions:
            next_state=action(runtime,s,name)
            if visible(runtime,s)!=visible(runtime,next_state):effective.add(name)
            enqueue(next_state,depth+1)
        waits=[ms-(s['ms']-s.get('actionTimes',{}).get(a,s['ms'])) for a,ms in runtime.get('input',{}).get('cooldown',{}).items() if not cooldown_allowed(runtime,s,a)]
        if waits:enqueue(advance(runtime,s,max(waits)),depth+1)
        running=s
        for _ in range(10001):
            active=[t for t in runtime['tick'] if t.get('while_state',running['state'])==running['state']]
            if not active:break
            next_state=advance(runtime,running,min(t['every_ms'] for t in active))
            progressed=progressed or visible(runtime,running)!=visible(runtime,next_state)
            for name in actions:
                if visible(runtime,next_state)!=visible(runtime,action(runtime,next_state,name)):effective.add(name)
            if next_state['state']!=running['state']:
                completion.add(next_state['state']);enqueue(next_state,depth+1);break
            if next_state['vars']==running['vars']:break
            running=next_state
        if required<=effective and set(runtime['states'])<={s[0] for s in seen} and (not runtime['tick'] or progressed):break
    errors=[]
    if required-effective:errors.append('这些动作在可达状态中没有可见反馈: '+', '.join(sorted(required-effective)))
    if runtime['tick'] and not progressed:errors.append('声明的 tick 没有推动可见状态')
    unreachable=set(runtime['states'])-{s[0] for s in seen}
    if unreachable:errors.append('存在不可达状态: '+', '.join(sorted(unreachable)))
    return {'ok':not errors,'mode':'application','visibleActions':sorted(effective),'disabledActions':disabled,'reachableStates':sorted({s[0] for s in seen}),'completionStates':sorted(completion),'ticksAdvanced':progressed,'explorationStates':len(seen),'explorationTruncated':truncated,'errors':errors,'deviceVerified':False,'hostServicesVerified':False,**({'syntheticHostInputs':True,'hostActionsContractOnly':host_actions} if runtime.get('ownership') else {})}
