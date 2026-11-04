"""p4-app-shell-v1: pages 0..11 behind quota v3, a bounded tab bar and reserved tab actions."""
import re
ENGINE='p4-app-shell-v1'
PAGES=12
TABS=(2,5)
RESERVED=('tab.next','tab.prev')
ICONS={'home','leaf','desk','agent','compass','sun','mic','book','sound','spark','heart','pause','play','check','plus','list','arrow','cloud','clock','stretch'}
def _int(v,lo,hi):return type(v) is int and lo<=v<=hi
def tabs_config(tabs,surface=None,runtime=None):
    """Strict tab bar declaration; raises ValueError."""
    def need(q):
        if not q:raise ValueError('Invalid app shell tab bar')
    need(isinstance(tabs,dict) and set(tabs)=={'items','var','x','y','w','h','colors'})
    items=tabs['items'];need(isinstance(items,list) and TABS[0]<=len(items)<=TABS[1])
    need(isinstance(tabs['var'],str) and re.fullmatch(r'[A-Za-z0-9_.-]{1,31}',tabs['var']))
    need(_int(tabs['x'],0,639) and _int(tabs['y'],0,479) and _int(tabs['w'],64*len(items),640-tabs['x']) and _int(tabs['h'],32,96) and tabs['y']+tabs['h']<=480)
    need(isinstance(tabs['colors'],list) and len(tabs['colors'])==4 and all(_int(c,0,65535) for c in tabs['colors']))
    pages=set()
    for item in items:
        need(isinstance(item,dict) and {'label','page'}<=set(item)<={'label','page','icon'})
        need(isinstance(item['label'],str) and 1<=len(item['label'].encode())<=24 and not any(ord(c)<32 or ord(c)==127 for c in item['label']))
        need('icon' not in item or item['icon'] in ICONS)
        need(_int(item['page'],0,PAGES-1) and item['page'] not in pages);pages.add(item['page'])
    if surface is not None:
        need(surface.get('shell')==ENGINE and surface.get('bindings')=='p4-variable-bindings-v1' and not any(k in surface for k in ('game_view','scene_view','input')))
        for n in surface.get('elements',[]):
            if 'action' in n and 'view' not in n and 'in_view' not in n:
                need(not(tabs['x']<n['x']+n['w'] and tabs['x']+tabs['w']>n['x'] and tabs['y']<n['y']+n['h'] and tabs['y']+tabs['h']>n['y']))
    if runtime is not None:
        count=max(1,len(runtime.get('pages',[])))
        need(all(item['page']<count for item in items) and runtime.get('vars',{}).get(tabs['var'],{}).get('type')=='int')
        on={t.get('on') for t in runtime.get('transitions',[]) if isinstance(t,dict)}
        need(all(a in on for a in RESERVED))
        for t in runtime.get('transitions',[]):
            if isinstance(t,dict) and t.get('on') in RESERVED:need(set(t)=={'from','on'} and t['from']=='*')
    return tabs
def errors(runtime):
    s=runtime.get('surface',{}) if isinstance(runtime,dict) else {}
    if not isinstance(s,dict) or ('shell' not in s and 'tabs' not in s):return []
    bad=['应用壳需要 p4-app-shell-v1、quota v3、通用绑定、2–5 个标签页与 tab.next/tab.prev 保留动作']
    try:
        assert s.get('shell')==ENGINE and s.get('graphics')=='p4-app-graphics-v1' and s.get('bindings')=='p4-variable-bindings-v1'
        assert runtime.get('quota')=='p4-quota-v3' and 1<=len(runtime.get('pages',[]))<=PAGES
        assert not any(k in runtime for k in ('media','instrument','game','scene')) and 'canvas' not in s
        if 'tabs' in s:tabs_config(s['tabs'],s,runtime)
        return []
    except (AssertionError,KeyError,TypeError,ValueError,AttributeError):return bad
def tab_operation(action):return 1 if action=='tab.next' else 2 if action=='tab.prev' else 0
def tab_index(tabs,value):return value%len(tabs['items'])
def apply_tab(runtime,state,action):
    """Reserved tab action: wrap the tab var, jump to its page, clear history."""
    op=tab_operation(action);tabs=runtime.get('surface',{}).get('tabs')
    if not op or not tabs:return False
    current=tab_index(tabs,int(state['vars'].get(tabs['var'],0)));count=len(tabs['items'])
    nxt=(current+1)%count if op==1 else (current+count-1)%count
    state['vars'][tabs['var']]=nxt;state['page']=runtime['pages'][tabs['items'][nxt]['page']]['id'];state['history']=[]
    return True
def node_visible(node,page_index,shell):
    """Shell packages: omitted page = shared; legacy packages keep 4 = shared."""
    p=node.get('page')
    if shell:return p is None or p==page_index
    return p is None or p==4 or p==page_index
