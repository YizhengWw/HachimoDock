"""Strict schema for bounded offline content pools and civil-time countdowns."""
import re
STATES=['ready','busy','result','editing','empty','offline']
VARS=[f'local{i}' for i in range(7)]
def local_errors(r):
    if 'local' not in r:return []
    try:
        c=r['local'];assert type(c) is dict and set(c)=={'engine','kind','groups','actions','reveal_ms','idle','busy','hint'}
        assert c['engine']=='p4-local-tools-v1' and c['kind'] in ('deck','countdown')
        assert type(c['reveal_ms']) is int and 300<=c['reveal_ms']<=2000
        def text(s,n):return type(s) is str and 0<len(s.encode())<n and all(ord(x)>=32 and ord(x)!=127 for x in s)
        assert all(text(c[k],145) for k in ('idle','busy','hint'))
        assert type(c['groups']) is list and 1<=len(c['groups'])<=3
        for g in c['groups']:
            assert type(g) is dict and text(g['title'],31)
            if c['kind']=='deck':
                assert set(g)=={'title','items'} and type(g['items']) is list and 2<=len(g['items'])<=(64 if r.get('quota')=='p4-quota-v3' else 24 if r.get('quota')=='p4-quota-v2' else 16)
                assert len({i['title'] for i in g['items']})==len(g['items'])
                for i in g['items']:assert set(i)=={'title','note'} and text(i['title'],61) and text(i['note'],145)
            else:assert set(g)=={'title','kind'} and g['kind'] in ('weekdays','weekend','date')
        a=c['actions'];assert type(a) is dict and 2<=len(a)<=8 and len(set(a.values()))==len(a)
        assert all(re.fullmatch('[a-z0-9_.-]{1,47}',k) for k in a)
        ops=set(a.values())
        if c['kind']=='countdown':assert ops=={'previous','next','edit','cancel','up','down'}
        else:
            assert ops<= {'draw','exclude','reset','close','previous','next'} and 'draw' in ops
            assert 'exclude' not in ops or 'reset' in ops
            assert len(c['groups'])==1 or {'previous','next'}<=ops
        assert r.get('states')==STATES and r.get('initial_state')=='ready'
        assert not any(k in r for k in ('game','scene','host','media','instrument','data','pages')) and not r.get('tick')
        assert r.get('surface',{}).get('engine')=='p4-app-surface-v1' and r.get('input',{}).get('engine')=='p4-input-v1'
        wide=c['kind']=='deck' and any(len(g['items'])>24 for g in c['groups'])
        names=[f'local{i}' for i in range(13)] if wide else VARS
        assert not wide or r.get('quota')=='p4-quota-v3'
        assert set(r.get('vars',{}))==set(names) and all(v.get('type')=='int' for v in r['vars'].values())
        assert r.get('persist')=={'version':2 if wide else 1,'vars':names,'states':{k:'ready' for k in STATES}}
        t=r['transitions'];assert len(t)==len(a) and {x['on'] for x in t}==set(a)
        assert all(x=={'from':'*','on':x['on']} for x in t)
        return []
    except (AssertionError,KeyError,TypeError,ValueError,AttributeError):return ['local: 必须遵循 p4-local-tools-v1 有界内容池/日期编辑与持久化契约']

def local_capability_errors(runtime, capabilities):
    """Target admission, independently of the old quota/maxLocalItems claim."""
    if 'local' not in runtime:
        return []
    if capabilities.get('widgetLocalTools') != 'p4-local-tools-v1':
        return ['离线抽取与日期编辑需要 p4-local-tools-v1，请先升级固件']
    local = runtime['local']
    extended = (isinstance(local, dict) and local.get('kind') == 'deck'
                and isinstance(local.get('groups'), list)
                and any(isinstance(group, dict) and isinstance(group.get('items'), list)
                        and len(group['items']) > 16 for group in local['groups']))
    if extended and capabilities.get('widgetLocalToolsMasks') != 'p4-local-masks-v2':
        return ['超过 16 项的离线内容池需要 p4-local-masks-v2 安全存档能力，请先升级设备固件；未写入组件']
    return []
