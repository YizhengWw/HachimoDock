"""Deterministic v4 application model for preview generation and behavioral checks."""
import copy
from collections import deque

def initial(runtime):
    return {'state':runtime['initial_state'],'page':runtime.get('initial_page',''),
            'vars':{k:v.get('init',0 if v['type']=='int' else '') for k,v in runtime['vars'].items()},'ms':0,'ticks':[0]*len(runtime['tick'])}

def effects(runtime,s,rule):
    original=dict(s['vars'])
    def clamp(k,v):
        if not isinstance(v,int):return v
        lo,hi=runtime.get('limits',{}).get(k,[-10**9,10**9]);return max(lo,min(hi,v))
    for k,v in rule.get('set',{}).items():s['vars'][k]=clamp(k,original[v['var']] if isinstance(v,dict) else v)
    for k,v in rule.get('inc',{}).items():s['vars'][k]=clamp(k,s['vars'][k]+v)
    for k,(lo,hi) in runtime.get('limits',{}).items():s['vars'][k]=max(lo,min(hi,s['vars'][k]))

def action(runtime,state,name):
    s=copy.deepcopy(state)
    for r in runtime['transitions']:
        if r['on']!=name or r['from'] not in ('*',s['state']):continue
        if r.get('to',s['state'])!=s['state']:
            s['state']=r['to'];s['ticks']=[s['ms']]*len(s['ticks'])
        if 'page' in r:
            pages=[p['id'] for p in runtime['pages']];i=pages.index(s['page'])
            s['page']=pages[(i+1)%len(pages)] if r['page']=='next' else pages[(i-1)%len(pages)] if r['page']=='prev' else r['page']
        effects(runtime,s,r);break
    return s

def advance(runtime,state,ms):
    s=copy.deepcopy(state);s['ms']+=ms
    for i,t in enumerate(runtime['tick']):
        if t.get('while_state',s['state'])!=s['state']:s['ticks'][i]=s['ms'];continue
        steps=min(10,(s['ms']-s['ticks'][i])//t['every_ms'])
        for _ in range(steps):
            effects(runtime,s,t);s['ticks'][i]+=t['every_ms']
            if 'when' in t and s['vars'][t['when']['var']]<=t['when']['lte']:
                s['state']=t.get('then',{}).get('to',s['state']);effects(runtime,s,t.get('then',{}));s['ticks']=[s['ms']]*len(s['ticks']);break
    return s

def dashboard(runtime,state):
    from validate_generated_widget import evaluate_dashboard_value
    return {k:evaluate_dashboard_value(v,state=state['state'],page=state['page'],variables=state['vars']) for k,v in runtime['dashboard'].items()}

def visible(runtime,state):
    values=dashboard(runtime,state)
    page=max(0,next((i for i,p in enumerate(runtime.get('pages',[])) if p['id']==state['page']),0))
    result=[]
    for i,n in enumerate(runtime['surface']['elements']):
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
        result.append((i,values.get(n.get('bind')) if n.get('bind') else n.get('text',n.get('color')),frame))
    return result

def smoke(runtime,buttons):
    if runtime.get('host'):
        actions={b['action'] for b in buttons}
        if runtime.get('input',{}).get('engine')=='p4-input-v1':
            actions.update(n['action'] for n in runtime.get('surface',{}).get('elements',[]) if n.get('action'))
            actions.update(runtime.get('input',{}).get('swipe',{}).values())
        declared={r['on'] for r in runtime['transitions']}
        errors=[] if actions==declared and all(a.startswith('host.') for a in actions) else ['宿主动作与物理按键映射不一致']
        return {'ok':not errors,'mode':'host-application-contract','actions':sorted(actions),'errors':errors,'deviceVerified':False,'hostServicesVerified':False,'note':'仅验证受控动作声明；须另外验收宿主服务，不能视为联网、录音或播放已通过。'}
    actions=[b['action'] for b in buttons];queue=deque([initial(runtime)]);seen=set();effective=set();progressed=False;completion=set()
    while queue:
        s=queue.popleft();key=(s['state'],s['page'])
        if key in seen:continue
        seen.add(key)
        for name in actions:
            next_state=action(runtime,s,name)
            if visible(runtime,s)!=visible(runtime,next_state):effective.add(name)
            if (next_state['state'],next_state['page']) not in seen:queue.append(next_state)
        running=s
        for _ in range(10001):
            active=[t for t in runtime['tick'] if t.get('while_state',running['state'])==running['state']]
            if not active:break
            next_state=advance(runtime,running,min(t['every_ms'] for t in active))
            progressed=progressed or visible(runtime,running)!=visible(runtime,next_state)
            for name in actions:
                if visible(runtime,next_state)!=visible(runtime,action(runtime,next_state,name)):effective.add(name)
            if next_state['state']!=running['state']:
                completion.add(next_state['state']);queue.append(next_state);break
            if next_state['vars']==running['vars']:break
            running=next_state
    errors=[]
    if set(actions)-effective:errors.append('这些物理动作在可达状态中没有可见反馈: '+', '.join(sorted(set(actions)-effective)))
    if runtime['tick'] and not progressed:errors.append('声明的 tick 没有推动可见状态')
    unreachable=set(runtime['states'])-{s for s,p in seen}
    if unreachable:errors.append('存在不可达状态: '+', '.join(sorted(unreachable)))
    return {'ok':not errors,'mode':'application','visibleActions':sorted(effective),'reachableStates':sorted({s for s,p in seen}),'completionStates':sorted(completion),'ticksAdvanced':progressed,'errors':errors,'deviceVerified':False,'hostServicesVerified':False}
