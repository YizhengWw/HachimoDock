"""Bounded local page history. No scripts, services or package-defined URLs."""
ENGINE='p4-app-navigation-v1'
MAX_DEPTH=16
def errors(runtime):
    rules=runtime.get('transitions',[]);rules=rules if isinstance(rules,list) else []
    extended='navigation' in runtime or any(isinstance(t,dict) and ('from_page' in t or isinstance(t.get('page'),dict)) for t in rules)
    if not extended:return []
    bad=['页面导航需要 p4-app-navigation-v1、本地应用、统一输入和有效页面操作']
    pages=runtime.get('pages',[])
    from variable_owners import mixed
    try:mixed_host=mixed(runtime)
    except (AssertionError,KeyError,TypeError,ValueError,AttributeError):return bad
    if runtime.get('navigation')!=ENGINE or runtime.get('host') is not None and not mixed_host or runtime.get('local') is not None or runtime.get('surface',{}).get('engine')!='p4-app-surface-v1' or runtime.get('input',{}).get('engine')!='p4-input-v1' or not isinstance(pages,list) or not 2<=len(pages)<=(12 if runtime.get('quota')=='p4-quota-v3' else 6 if runtime.get('quota')=='p4-quota-v2' else 4):return bad
    ids=[p.get('id') for p in pages if isinstance(p,dict)]
    if len(ids)!=len(pages) or any(k in runtime for k in ('media','instrument','data','game','scene')):return bad
    for t in rules:
        if not isinstance(t,dict):return bad
        if 'from_page' in t and t['from_page'] not in ids:return bad
        p=t.get('page')
        if isinstance(p,dict):
            if len(p)!=1:return bad
            op=next(iter(p))
            if op=='back':
                if p[op] is not True:return bad
            elif op not in ('push','replace','reset') or p[op] not in ids:return bad
        elif 'page' in t and (not isinstance(p,str) or p not in ids+['next','prev']):return bad
    return []
def allowed(runtime,state,rule):
    if 'from_page' in rule and rule['from_page']!=state['page']:return False
    p=rule.get('page')
    if not isinstance(p,dict):return True
    if runtime.get('navigation')!=ENGINE:return False
    history=state.get('history',[])
    return bool(history) if p.get('back') is True else len(history)<MAX_DEPTH if 'push' in p else True
def apply(runtime,state,rule):
    if not allowed(runtime,state,rule):return False
    p=rule.get('page');pages=[p['id'] for p in runtime.get('pages',[])]
    if p is None or not pages:return True
    if isinstance(p,str):
        i=pages.index(state['page']);state['page']=pages[(i+1)%len(pages)] if p=='next' else pages[(i-1)%len(pages)] if p=='prev' else p
        if 'navigation' in runtime:state['history']=[]
    else:
        history=state.setdefault('history',[])
        if p.get('back') is True:state['page']=history.pop()
        elif 'push' in p:history.append(state['page']);state['page']=p['push']
        elif 'replace' in p:state['page']=p['replace']
        else:history.clear();state['page']=p['reset']
    return True
