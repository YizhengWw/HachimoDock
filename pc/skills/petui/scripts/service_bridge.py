"""Service bridge import contract; no provider calls, paths, network or playback."""
import re
import hashlib
import struct
import math

ENGINE = 'pc-service-bridge-v1'
DEVICE_ENGINE = 'p4-service-bridge-v1'
def binding_contract(declaration,runtime):
    def frame(v):
        if v is None:return b'n'
        if type(v) is bool:return b't' if v else b'f'
        if type(v) is int:
            if not -2**63<=v<2**64:raise ValueError('integer outside JSON contract range')
            return b'i'+str(v).encode()+b';'
        if type(v) is float:
            if not math.isfinite(v):raise ValueError('non-finite contract number')
            if v==0:return b'i0;'
            return b'd'+struct.pack('>d',v)
        if isinstance(v,str):
            data=v.encode('utf-8');return b's'+str(len(data)).encode()+b':'+data
        if isinstance(v,list):return b'a'+str(len(v)).encode()+b'['+b''.join(frame(x) for x in v)+b']'
        if isinstance(v,dict):return b'o'+str(len(v)).encode()+b'{'+b''.join(frame(k)+frame(v[k]) for k in sorted(v))+b'}'
        raise ValueError('unsupported contract value')
    device={k:v for k,v in runtime.items() if k!='host_contract'}
    host={k:declaration[k] for k in ('schemaVersion','engine','service','actions','content')}
    return hashlib.sha256(frame([host,device])).hexdigest()
def name(s,n):return isinstance(s,str) and re.fullmatch(r'[A-Za-z0-9_.-]{1,'+str(n)+'}',s) is not None
def pointer(s):return isinstance(s,str) and len(s.encode())<=128 and (not s or s.startswith('/')) and not re.search(r'~(?:[^01]|$)',s) and not any(ord(c)<32 or 127<=ord(c)<=159 for c in s)
def scalar(v,t):
    return type(v) is int and -10**9<=v<=10**9 if t=='int' else isinstance(v,str) and len(v.encode())<=63 and not any(ord(c)<32 or 127<=ord(c)<=159 for c in v) if t=='string' else type(v) is bool if t=='bool' else False
def obj(v,required,optional=()):assert isinstance(v,dict) and set(required)<=set(v)<=set(required)|set(optional)

def surface_errors(s,r=None):
    try:
        declared='services' in s
        if declared:
            assert s['services']==DEVICE_ENGINE and s['engine']=='p4-app-surface-v1' and s['graphics']=='p4-app-graphics-v1' and s['bindings']=='p4-variable-bindings-v1'
            if r is not None:assert r.get('host') is True and r.get('host_context')=='p4-host-context-v1' and r.get('input',{}).get('engine')=='p4-input-v1'
            if r is not None:assert isinstance(r.get('host_contract'),str) and re.fullmatch('[0-9a-f]{64}',r['host_contract'])
        elif r is not None:assert 'host_contract' not in r
        for n in s.get('elements',[]):
            if 'image_bind' in n:
                assert declared and n.get('type')=='image' and name(n['image_bind'],15)
                assert not any(k in n for k in ('tile','motion','rotate_ms'))
                assert not any(im.get('id')==n.get('image') and 'encoding' in im for im in s.get('images',[]))
            if 'value_param' in n:
                assert declared and n.get('type')=='meter' and name(n['value_param'],31) and name(n.get('action'),47) and n['action'].startswith('host.')
                lo,hi=n.get('value_min',0),n.get('value_max',100)
                assert type(lo) is int and type(hi) is int and -10**9<=lo<hi<=10**9
                if r is not None:assert n['action'] not in r.get('input',{}).get('confirm',{})
            else:assert 'value_min' not in n and 'value_max' not in n
        return []
    except (AssertionError,KeyError,TypeError,ValueError,AttributeError):return ['服务桥图形需要显式能力、合法图片引用或带参数的宿主滑杆']

def methods(source):
    obj(source,{'kind'},{'declaration','runtime','parameters','status'})
    if source['kind']=='local-media':
        assert set(source)=={'kind'}
        result={s:({},False) for s in ('toggle','pause','resume','stop','previous','next','volume_up','volume_down','cycle_mode')}
        for method,param,t in [('play','key','string'),('remove','key','string'),('seek','positionMs','int'),('seek_percent','percent','int'),('volume','volume','int'),('mode','mode','string')]:result[method]=({param:t},method=='remove')
        return result
    assert source['kind']=='service' and {'declaration','runtime'}<=set(source)
    d,r=source['declaration'],source['runtime']
    assert d.get('engine')!=ENGINE and not d.get('autoRead',False)
    from app_services import application_errors
    assert not application_errors(d,r)
    status=source.get('status')
    if status is not None:
        obj(status,{'pointer','phases'});assert pointer(status['pointer']) and isinstance(status['phases'],dict) and 1<=len(status['phases'])<=12
        assert all(isinstance(k,str) and len(k.encode())<=31 and not any(ord(c)<32 or 127<=ord(c)<=159 for c in k) and v in ('idle','preparing','ready','running','paused','completed','interrupted','unavailable','failed','cancelled') for k,v in status['phases'].items())
    parameters=source.get('parameters',{});assert isinstance(parameters,dict) and all(not fields for fields in parameters.values())
    assert all(k in d['actions'] and isinstance(v,dict) and len(v)<=4 and all(name(f,31) and t in ('int','string','bool') for f,t in v.items()) for k,v in parameters.items())
    return {a:(parameters.get(a,{}),a in r.get('input',{}).get('confirm',{})) for a in d['actions']}

def bridge_errors(d,r):
    try:
        obj(d,{'schemaVersion','engine','service','actions','content'},{'autoRead','location'})
        assert type(d['schemaVersion']) is int and d['schemaVersion']==1 and d['engine']==ENGINE and d['service']=='bridge' and d.get('autoRead',False) is False and d.get('location') is None
        assert r.get('host') is True and r.get('host_context')=='p4-host-context-v1' and r.get('input',{}).get('engine')=='p4-input-v1' and r.get('surface',{}).get('bindings')=='p4-variable-bindings-v1'
        assert 'persist' not in r and r.get('tick')==[] and 'ownership' not in r
        assert isinstance(d['content'],list) and len(d['content'])==1
        p=d['content'][0];obj(p,{'services','bindings','commands'},{'timelines','images','collections'})
        services,bindings,commands=p['services'],p['bindings'],p['commands']
        assert isinstance(services,dict) and 1<=len(services)<=4 and all(name(a,16) for a in services)
        catalog={a:methods(v) for a,v in services.items()}
        assert sum(v['kind']=='service' and v['declaration'].get('service')=='notes' for v in services.values())<=1
        assert sum(v['kind']=='local-media' or v['kind']=='service' and v['declaration'].get('service') in ('knowledge','books','soundscape','news') for v in services.values())<=1
        assert isinstance(bindings,dict) and set(bindings)==set(r['vars']) and len(bindings)<=8 and all(v.get('type') in ('int','string') for v in r['vars'].values())
        for variable,b in bindings.items():
            obj(b,{'service','pointer','fallback'})
            assert b['service'] in services and pointer(b['pointer']) and scalar(b['fallback'],r['vars'][variable]['type'])
            if variable in r.get('limits',{}):assert type(b['fallback']) is int and r['limits'][variable][0]<=b['fallback']<=r['limits'][variable][1]
        collections=p.get('collections',{});assert isinstance(collections,dict) and len(collections)<=1
        for a,c in collections.items():
            obj(c,{'service','pointer','key','label','detail','visible'})
            assert name(a,16) and a not in services and c['service'] in services and all(pointer(c[k]) for k in ('pointer','key','label','detail')) and c['key']
            assert type(c['visible']) is int and 1<=c['visible']<=8
        assert isinstance(commands,dict) and len(commands)<=16 and isinstance(d['actions'],dict) and set(commands)==set(d['actions'])
        transitions=r['transitions'];assert len(transitions)==len(commands) and all(isinstance(t,dict) and set(t)=={'from','on'} and t['from']=='*' and t['on'] in commands for t in transitions)
        event_args=False
        for action,c in commands.items():
            obj(c,{'service','method'},{'arguments'})
            assert name(action,47) and action.startswith('host.') and name(c['method'],47) and d['actions'][action]==c['service']+':'+c['method']
            args=c.get('arguments',{});assert isinstance(args,dict) and len(args)<=4
            if c['service'] in collections:
                assert not args and c['method'] in ('page_next','page_previous','up','down','clear_selection')+tuple('row_'+str(i) for i in range(collections[c['service']]['visible']))
                continue
            expected,confirm=catalog[c['service']][c['method']]
            assert set(args)==set(expected)
            if confirm:assert isinstance(r.get('input',{}).get('confirm',{}).get(action),str) and r['input']['confirm'][action].strip()
            for key,spec in args.items():
                obj(spec,{'type','value'},{'min','max'})
                assert name(key,31) and spec['type']==expected[key] and spec['type'] in ('int','string','bool')
                lo,hi=spec.get('min'),spec.get('max')
                assert (lo is None or type(lo) is int and -2**31<=lo<2**31) and (hi is None or type(hi) is int and -2**31<=hi<2**31) and (lo is None or hi is None or lo<=hi)
                assert spec['type']=='int' or lo is None and hi is None
                value=spec['value']
                if isinstance(value,dict):
                    assert len(value)==1
                    if 'event' in value:assert name(value['event'],31);event_args=True
                    else:assert 'selection' in value and spec['type']=='string' and value['selection'] in collections
                else:
                    assert scalar(value,spec['type']) and (type(value) is not int or (lo is None or value>=lo) and (hi is None or value<=hi))
        timelines=p.get('timelines',{});images=p.get('images',{})
        assert isinstance(timelines,dict) and len(timelines)<=2 and isinstance(images,dict) and len(images)<=2
        if timelines or images or event_args or any(s['kind']=='local-media' for s in services.values()):assert r['surface'].get('services')==DEVICE_ENGINE
        for a,t in timelines.items():
            obj(t,{'service','position','duration','running','horizonMs'},{'projection'})
            assert name(a,31) and r['vars'][a]['type']=='int' and t['service'] in services and all(pointer(t[k]) for k in ('position','duration','running'))
            assert type(t['horizonMs']) is int and 100<=t['horizonMs']<=10000 and t.get('projection','percent') in ('percent','position','remaining')
        for a,b in images.items():
            obj(b,{'service','pointer','fallback'})
            assert name(a,15) and b['service'] in services and pointer(b['pointer']) and b['fallback'] is None
        for n in r['surface'].get('elements',[]):
            if 'image_bind' in n:assert n['image_bind'] in images
            if 'value_param' in n:
                c=commands[n['action']]
                assert any(arg['type']=='int' and isinstance(arg['value'],dict) and arg['value'].get('event')==n['value_param'] for arg in c.get('arguments',{}).values())
        if 'collection_view' in r['surface']:
            c=r['surface']['collection_view'];view=collections[c['source']]
            assert c['visible']==view['visible'] and all(commands[a]['service']==c['source'] and commands[a]['method']=='row_'+str(i) for i,a in enumerate(c['actions']))
        if r['surface'].get('services')==DEVICE_ENGINE:assert r.get('host_contract')==binding_contract(d,r)
        return surface_errors(r['surface'],r)
    except (AssertionError,KeyError,TypeError,ValueError,AttributeError):return ['服务桥需要有界服务、类型化命令、精确变量绑定和真实操作上下文']
