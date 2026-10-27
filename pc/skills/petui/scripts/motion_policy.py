"""Resource-only declaration checks for the generic fixed-step motion profile."""
def motion_errors(runtime):
    try:
        s=runtime.get('scene',{})
        # The structural validator reports invalid/null scene values. Never
        # crash while validating an unrelated malformed package.
        if not isinstance(s,dict): return []
        m=s.get('motion');rules=s.get('rules',[])
        if m is None:
            assert all(r.get('on')!='landed' and all(o.get('op')!='charge' for o in r.get('do',[])) for r in rules)
            return []
        assert isinstance(m,dict) and set(m)-{'track'}=={'engine','body','platforms','gravity','lift','speed','charge_ms','charge_var'}
        assert m['engine']=='p4-motion-v1' and type(s['tick_ms']) is int and 16<=s['tick_ms']<=40
        es={e['id']:e for e in s['entities']};b=es[m['body']]
        ps=m['platforms'];assert isinstance(ps,list) and 1<=len(ps)<=11 and len(set(ps))==len(ps)
        assert m['body'] not in ps and all(p in es for p in ps)
        for k,lo,hi in [('gravity',4,64),('lift',2,16),('charge_ms',300,3000)]:assert type(m[k]) is int and lo<=m[k]<=hi
        speed=m['speed'];assert isinstance(speed,list) and len(speed)==2 and all(type(x) is int for x in speed) and 1<=speed[0]<=speed[1]<=24
        assert runtime['vars'][m['charge_var']]['type']=='int' and m['charge_var'] not in (s.get('score_var'),runtime.get('lifecycle',{}).get('record_var'))
        assert not s.get('sprites') and not s['grid'].get('solid_tones') and not runtime.get('input',{}).get('drag')
        assert all(e.get('vx',0)==e.get('vy',0)==0 for e in es.values())
        assert b.get('active',True) and any(es[p].get('active',True) and b['y']+b.get('height',1)==es[p]['y'] and es[p]['x']<=b['x']+b.get('width',1)/2<es[p]['x']+es[p].get('width',1) for p in ps)
        if 'track' in m:
            t=m['track'];assert isinstance(t,dict) and set(t)=={'engine','camera_x','gap','width','tones'} and t['engine']=='p4-platform-track-v1'
            assert 4<=len(ps)<=8 and len(es)==len(ps)+1
            assert type(t['camera_x']) is int and 2<=t['camera_x']<=s['grid']['width']//2
            for key,lo,hi in [('gap',1,6),('width',2,4)]:
                a=t[key];assert isinstance(a,list) and len(a)==2 and all(type(v) is int for v in a) and lo<=a[0]<=a[1]<=hi
            assert (t['gap'][1]+t['width'][1])*m['gravity']<=2*m['lift']*m['speed'][1]
            ts=t['tones'];assert isinstance(ts,list) and 2<=len(ts)<=4 and all(type(v) is int and 1<=v<=4 for v in ts) and len(set(ts))==len(ts)
            assert all(es[p].get('active',True) and es[p]['y']==b['y']+b.get('height',1) for p in ps)
        hold=runtime['input']['hold'];charging=False
        for r in rules:
            assert r['on'] not in ('tick','collision','blocked')
            if r['on']=='landed':assert r['entity']==m['body'] and r['with'] in ps
            for o in r['do']:
                assert o['op'] not in ('move','velocity','accelerate','bounce')
                if 'track' in m:assert o['op'] not in ('place','show','hide')
                if o['op']=='charge':
                    assert o['entity']==m['body'] and r['on']==hold
                    charging=True
        assert charging
        return []
    except (KeyError,TypeError,ValueError,AssertionError,AttributeError):
        return ['motion 需要 p4-motion-v1、静态平台、初始落地角色、独立蓄力变量和 SW1 按住/松开动作']
