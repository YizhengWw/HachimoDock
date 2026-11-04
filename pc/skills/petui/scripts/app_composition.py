"""Strict bounded composition contract; no executable package code or credentials."""
import ipaddress
import json
import re
from urllib.parse import parse_qsl, urlsplit

ENGINE = 'pc-app-composition-v1'
ENGINE_V2 = 'pc-app-composition-v2'
ENGINE_V3 = 'pc-app-composition-v3'
OPERATIONS = {'constant': set(), 'clock': set(), 'counter': {'increment', 'decrement', 'reset'},
              'timer': {'start', 'pause', 'reset'}, 'json-get': {'refresh'},
              'ledger': {'append','remove_latest','previous','next'},'computed':set(),'document':{'set','append','remove'},
              'workflow': {'start', 'resume', 'cancel'},
              'collection': {'page_next','page_previous','up','down','clear_selection'} | {'row_'+str(i) for i in range(8)}}

def name(value, limit):
    return isinstance(value, str) and re.fullmatch(r'[A-Za-z0-9_.-]{1,%d}' % limit, value) is not None

def scalar(value, kind):
    return (kind == 'int' and type(value) is int and -10**9 <= value <= 10**9 or
            kind == 'string' and isinstance(value, str) and len(value.encode()) <= 63 and
            not any(ord(c) < 32 or 127 <= ord(c) <= 159 for c in value))

def public_get(endpoint, hosts):
    assert isinstance(endpoint, str) and len(endpoint.encode()) <= 2048
    url = urlsplit(endpoint)
    assert url.scheme == 'https' and not url.username and not url.password and not url.fragment and url.port in (None, 443)
    assert isinstance(hosts, list) and 1 <= len(hosts) <= 4
    for host in hosts:
        assert isinstance(host, str) and len(host) <= 253 and '.' in host
        assert all(len(part) <= 63 and re.fullmatch(r'[a-z0-9](?:[a-z0-9-]*[a-z0-9])?', part) for part in host.split('.'))
        try: ipaddress.ip_address(host)
        except ValueError: pass
        else: raise ValueError('literal IP')
    assert url.hostname in hosts
    assert not any(k.lower() in {'key', 'api_key', 'apikey', 'token', 'access_token', 'password', 'secret', 'authorization'} for k, _ in parse_qsl(url.query))

def json_pointer(ptr):
    return isinstance(ptr, str) and len(ptr.encode()) <= 128 and (not ptr or ptr.startswith('/')) and re.search(r'~(?:[^01]|$)', ptr) is None

def validate_async_workflows(workflows, sources, actions):
    from app_expr import validate, project
    assert isinstance(workflows, dict) and len(workflows) <= 4
    for flow, steps in workflows.items():
        aliases = [alias for alias, source in sources.items() if source.get('kind') == 'workflow' and source['workflow'] == flow]
        assert name(flow, 16) and isinstance(steps, list) and 1 <= len(steps) <= 8 and len(aliases) == 1
        assert all(aliases[0] + ':' + op in actions.values() for op in ('start', 'resume', 'cancel'))
        outputs = set()
        for step in steps:
            assert isinstance(step, dict)
            if 'get' in step:
                assert set(step) == {'get', 'result'} and name(step['result'], 16) and step['result'] not in outputs
                outputs.add(step['result'])
                get = step['get']; assert isinstance(get, dict) and set(get) == {'endpoint', 'hosts', 'pointer'}
                public_get(get['endpoint'], get['hosts']); assert json_pointer(get['pointer'])
            else:
                assert 'target' in step and set(step) <= {'target', 'pointer', 'value', 'when'}
                assert isinstance(step['target'], str) and step['target'].count(':') == 1
                alias, op = step['target'].split(':'); kind = sources[alias]['kind']
                if kind == 'counter':
                    assert op in ('increment', 'decrement') and 'pointer' not in step and 'value' not in step
                else:
                    assert kind == 'document' and op in ('set', 'append') and 'value' in step
                    ptr = step['pointer']; assert json_pointer(ptr) and ptr
                    assert op != 'set' or not isinstance(project(sources[alias]['initial'], ptr), (list, dict))
                for field in ('value', 'when'):
                    if field in step: validate(step[field], set(sources))

def collection_dependencies(expression, sources, depth=0):
    from app_expr import validate
    assert depth <= 8
    def transient(v):
        if isinstance(v, dict):
            ref=v.get('ref')
            return (isinstance(ref,str) and (ref=='/event' or ref.startswith('/event/'))) or any(transient(child) for child in v.values())
        return isinstance(v,list) and any(transient(child) for child in v)
    assert not transient(expression)
    dependencies=validate(expression,set(sources))
    for alias in dependencies:
        source=sources[alias]
        if source['kind']=='computed':collection_dependencies(source['expression'],sources,depth+1)
        else:assert source['kind'] in ('document','constant')
    return dependencies

def composition_errors(value, runtime):
    try:
        assert isinstance(value, dict) and not set(value) - {'schemaVersion', 'engine', 'service', 'actions', 'content', 'autoRead'}
        assert type(value['schemaVersion']) is int and value['schemaVersion'] == 1
        assert value['engine'] in (ENGINE,ENGINE_V2,ENGINE_V3) and value['service'] == 'composition'
        v2=value['engine']!=ENGINE
        v3=value['engine']==ENGINE_V3
        assert value.get('autoRead', False) is False
        assert len(json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode()) <= (32768 if v2 else 16384)
        assert runtime['host'] is True and runtime['surface']['bindings'] == 'p4-variable-bindings-v1'
        from variable_owners import parse as variable_owners
        writers=variable_owners(runtime)
        assert not writers['mixed'] or v2
        assert writers['mixed'] or runtime.get('tick') == [] and 'persist' not in runtime
        assert not v2 or runtime.get('host_context')=='p4-host-context-v1' and runtime.get('input',{}).get('engine')=='p4-input-v1'
        assert writers['mixed'] or all(isinstance(v, str) for v in runtime.get('dashboard', {}).values())
        variables = runtime['vars']
        assert isinstance(variables, dict) and 1 <= len(variables) <= 8
        assert isinstance(value['content'], list) and len(value['content']) == 1
        plan = value['content'][0]
        assert isinstance(plan, dict) and {'sources','bindings'}<=set(plan) and set(plan)<={'sources','bindings','workflows','asyncWorkflows'} and (v2 or 'workflows' not in plan) and (v3 or 'asyncWorkflows' not in plan)
        sources, bindings = plan['sources'], plan['bindings']
        assert isinstance(sources, dict) and 1 <= len(sources) <= (8 if v2 else 4)
        assert not v2 or 'flow' not in sources
        workflows=plan.get('workflows',{})
        async_workflows=plan.get('asyncWorkflows',{})
        assert isinstance(workflows,dict) and len(workflows)<=8
        dependencies={}
        assert isinstance(bindings, dict) and set(bindings) == writers['host']
        if 'collection_view' in runtime.get('surface',{}):
            from host_collection import config
            c=config(runtime['surface']['collection_view'],runtime['surface'],runtime)
            assert v2 and sources.get(c['source'],{}).get('kind')=='collection' and sources[c['source']]['view']['page_size']==c['visible']
            assert all(value['actions'].get(action)==f"{c['source']}:row_{slot}" for slot,action in enumerate(c['actions']))
        for alias, source in sources.items():
            assert name(alias, 16) and isinstance(source, dict)
            assert v2 or any(b.get('source') == alias for b in bindings.values()) or any(isinstance(a, str) and a.split(':')[0] == alias for a in value['actions'].values())
            kind = source['kind']; assert kind in OPERATIONS
            assert v2 or kind not in ('computed','document','collection')
            assert v3 or kind != 'workflow'
            fields = {'kind'} | {'constant': {'value'}, 'clock': set(), 'counter': {'initial', 'min', 'max', 'step'},
                                 'timer': {'seconds'}, 'json-get': {'endpoint', 'hosts', 'refresh_sec'},
                                 'ledger': {'unit','target','capacity','label'},'computed':{'expression'},'document':{'initial'},'workflow':{'workflow'},'collection':{'expression','view'}}[kind]
            assert set(source) == fields
            if kind == 'constant':
                assert len(json.dumps(source['value'], ensure_ascii=False, separators=(',', ':')).encode()) <= 4096
            elif kind == 'workflow':
                assert source['workflow'] in async_workflows
            elif kind == 'collection':
                view=source['view'];assert isinstance(view,dict) and {'key','label','page_size'}<=set(view)<= {'key','label','page_size','detail'}
                assert type(view['page_size']) is int and 1<=view['page_size']<=8
                for field in ('key','label','detail'):
                    ptr=view.get(field)
                    if field=='detail' and ptr is None:continue
                    assert json_pointer(ptr) and ptr and not any(ord(c)<32 or 127<=ord(c)<=159 for c in ptr)
                dependencies[alias]=collection_dependencies(source['expression'],sources)
            elif kind in ('computed','document'):
                from app_expr import validate,bounded
                if kind=='computed':dependencies[alias]=validate(source['expression'],set(sources))
                else:assert bounded(source['initial'])
            elif kind == 'counter':
                assert all(type(source[k]) is int for k in ('initial', 'min', 'max', 'step'))
                assert -10**9 <= source['min'] <= source['initial'] <= source['max'] <= 10**9
                assert 1 <= source['step'] <= 10**9
            elif kind == 'timer':
                assert type(source['seconds']) is int and 1 <= source['seconds'] <= 86400
            elif kind == 'ledger':
                assert source['unit'] in ('day','event') and type(source['target']) is int and type(source['capacity']) is int
                assert 1<=source['target']<=source['capacity']<=128
                assert isinstance(source['label'],str) and source['label'].strip() and len(source['label'].encode())<=24 and not any(ord(c)<32 or 127<=ord(c)<=159 for c in source['label'])
                assert all(action in runtime.get('input',{}).get('confirm',{}) for action,target in value['actions'].items() if target==alias+':remove_latest')
            elif kind == 'json-get':
                public_get(source['endpoint'], source['hosts'])
                assert type(source['refresh_sec']) is int and 30 <= source['refresh_sec'] <= 86400
        remaining={alias:dependencies.get(alias,set()) for alias in sources};done=set()
        while remaining:
            ready=next((alias for alias,deps in remaining.items() if deps<=done),None)
            assert ready is not None
            done.add(ready);remaining.pop(ready)
        for flow,steps in workflows.items():
            assert name(flow,16) and isinstance(steps,list) and 1<=len(steps)<=8
            assert 'flow:'+flow in value['actions'].values()
            destructive=False
            for step in steps:
                assert isinstance(step,dict) and 'target' in step and set(step)<={'target','pointer','value','when'}
                assert isinstance(step['target'],str) and step['target'].count(':')==1
                alias,op=step['target'].split(':')
                assert alias in sources and op in OPERATIONS[sources[alias]['kind']] and sources[alias]['kind'] not in ('json-get','workflow','collection')
                if sources[alias]['kind']=='document':
                    ptr=step['pointer'];assert isinstance(ptr,str) and len(ptr.encode())<=128 and (not ptr or ptr.startswith('/')) and re.search(r'~(?:[^01]|$)',ptr) is None
                    assert (op=='remove')==('value' not in step) and (op!='remove' or ptr!='')
                    from app_expr import project
                    destructive|=op=='remove' or op=='set' and (ptr=='' or isinstance(project(sources[alias]['initial'],ptr),(list,dict)))
                else:assert 'pointer' not in step and 'value' not in step
                destructive|=op in ('remove_latest','reset')
                from app_expr import validate
                for key in ('value','when'):
                    if key in step:validate(step[key],set(sources))
            if destructive:assert all(isinstance(runtime.get('input',{}).get('confirm',{}).get(action),str) and runtime['input']['confirm'][action].strip() for action,target in value['actions'].items() if target=='flow:'+flow)
        validate_async_workflows(async_workflows, sources, value['actions'])
        for key, binding in bindings.items():
            assert name(key, 31) and isinstance(binding, dict) and set(binding) == {'source', 'pointer', 'fallback'}
            assert binding['source'] in sources
            pointer = binding['pointer']
            assert isinstance(pointer, str) and len(pointer.encode()) <= 128 and (not pointer or pointer.startswith('/'))
            assert re.search(r'~(?:[^01]|$)', pointer) is None
            assert scalar(binding['fallback'], variables[key]['type'])
            if key in runtime.get('limits', {}):
                lo, hi = runtime['limits'][key]; assert lo <= binding['fallback'] <= hi
        actions = value['actions']
        assert isinstance(actions, dict) and len(actions) <= (16 if runtime.get('input', {}).get('engine') == 'p4-input-v1' else 8)
        for action, target in actions.items():
            assert isinstance(action, str) and re.fullmatch(r'host\.[a-z_]{1,32}', action)
            assert isinstance(target, str) and target.count(':') == 1
            alias, operation = target.split(':')
            assert alias=='flow' and operation in workflows or alias in sources and sources[alias]['kind']!='document' and operation in OPERATIONS[sources[alias]['kind']]
        rules = [r for r in runtime['transitions'] if not writers['mixed'] or isinstance(r.get('on'),str) and r['on'].startswith('host.')]
        assert len(rules) == len(actions) and {r['on'] for r in rules} == set(actions)
        assert all(set(r) == {'from', 'on'} and r['from'] == '*' for r in rules)
        return []
    except (AssertionError, KeyError, TypeError, ValueError, AttributeError):
        return ['组合应用需要有界数据源、精确变量绑定和已声明动作；不允许脚本、凭据或任意 IPC']
