"""Data-only writer plan, not execution or authorization."""
import re

ENGINE='p4-variable-owners-v1'


def parse(runtime):
    variables=runtime['vars']
    assert isinstance(variables,dict) and len(variables)<=8
    if 'ownership' not in runtime:
        return {'mixed':False,'host':set(variables) if runtime.get('host') is True else set()}
    plan=runtime['ownership']
    assert isinstance(plan,dict) and set(plan)=={'engine','host'} and plan['engine']==ENGINE
    assert runtime.get('host') is True and runtime.get('engine')=='p4-bounded-runtime-v4'
    assert runtime.get('host_context')=='p4-host-context-v1' and runtime.get('input',{}).get('engine')=='p4-input-v1'
    assert runtime.get('surface',{}).get('engine')=='p4-app-surface-v1' and runtime['surface'].get('bindings')=='p4-variable-bindings-v1'
    assert not any(k in runtime for k in ('local','scene','game','media','instrument','data'))
    for key in ('form','persist','program','dashboard'):
        if key in runtime:assert isinstance(runtime[key],dict)
    def name(v):
        return isinstance(v,str) and re.fullmatch(r'[A-Za-z0-9_.-]{1,31}',v) is not None
    assert variables and all(name(k) and isinstance(v,dict) and v.get('type') in ('int','string') for k,v in variables.items())
    assert isinstance(plan['host'],list) and 1<=len(plan['host'])<=len(variables)
    assert all(name(k) and k in variables for k in plan['host'])
    host=set(plan['host']);assert len(host)==len(plan['host'])
    def owns(v):
        return isinstance(v,str) and v in host
    def native_refs(v,depth=0):
        assert depth<=16
        if isinstance(v,list):
            for value in v:native_refs(value,depth+1)
        elif isinstance(v,dict):
            for key,value in v.items():
                assert key not in ('var','fmt_mmss','fmt_hms','pct_of','of_max') or not owns(value)
                native_refs(value,depth+1)
    for key in ('transitions','tick'):
        assert key not in runtime or isinstance(runtime[key],list)
        for rule in runtime.get(key,[]):
            assert isinstance(rule,dict)
            if 'then' in rule:assert isinstance(rule['then'],dict)
            if key=='transitions' and isinstance(rule.get('on'),str) and rule['on'].startswith('host.'):
                assert set(rule)=={'from','on'} and rule['from']=='*'
            native_refs(rule)
            for body in (rule,rule.get('then')):
                if body is None:continue
                assert isinstance(body,dict)
                for op in ('set','inc'):
                    if op in body:assert isinstance(body[op],dict) and not any(owns(k) for k in body[op])
    native_refs(runtime.get('dashboard'))
    persist=runtime.get('persist',{})
    if isinstance(persist,dict) and 'vars' in persist:
        assert isinstance(persist['vars'],list) and not any(owns(v) for v in persist['vars'])
    form=runtime.get('form',{})
    if form:
        assert isinstance(form,dict)
        assert not any(owns(form.get(k)) for k in ('focus_var','active_var'))
        keyboard=form.get('keyboard',{})
        assert isinstance(keyboard,dict) and not any(owns(keyboard.get(k)) for k in ('index_var','label_var'))
        if 'fields' in form:
            assert isinstance(form['fields'],list)
            assert all(isinstance(f,dict) and not owns(f.get('var')) and not owns(f.get('draft_var')) for f in form['fields'])
    if 'program' in runtime:
        outputs=runtime['program']['outputs']
        assert isinstance(outputs,list) and len(outputs)==8 and not any(owns(v) for v in outputs)
    return {'mixed':True,'host':host}


def errors(runtime):
    if 'ownership' not in runtime:return []
    try:parse(runtime);return []
    except (AssertionError,KeyError,TypeError,ValueError,AttributeError):
        return ['variable ownership requires bounded host fields and disjoint native/program/form/save writers']


def mixed(runtime):
    return 'ownership' in runtime and parse(runtime)['mixed']
