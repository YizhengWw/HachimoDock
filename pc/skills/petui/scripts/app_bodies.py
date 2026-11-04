"""p4-bodies-v1 scene declaration checks; simulation lives only in game C/WASM."""
def bodies_errors(runtime):
    scene=runtime.get('scene',{})
    if not isinstance(scene,dict):return []
    rules=scene.get('rules',[])
    try:
        ops=[op for rule in rules for op in rule['do']]
        if 'bodies' not in scene:
            assert 'gravity' not in scene and not any(o.get('op')=='impulse' for o in ops)
            return []
        integer=lambda v,lo,hi:type(v) is int and lo<=v<=hi
        assert runtime.get('engine')=='p4-bounded-runtime-v4'
        assert 'motion' not in scene and integer(scene.get('tick_ms'),20,200)
        grid=scene['grid'];assert isinstance(grid,dict) and set(grid)=={'width','height'}
        assert integer(grid['width'],4,16) and integer(grid['height'],4,16)
        assert integer(scene.get('gravity',0),-64,64)
        entities=scene['entities'];assert isinstance(entities,list) and 1<=len(entities)<=12
        ids={e['id']:e for e in entities};assert len(ids)==len(entities)
        bodies=scene['bodies'];assert isinstance(bodies,list) and 1<=len(bodies)<=8
        seen=set()
        for b in bodies:
            assert isinstance(b,dict) and set(b)<={'entity','mass','restitution','static','rolling'}
            name=b['entity'];assert name in ids and name not in seen;seen.add(name);e=ids[name]
            assert integer(b.get('mass',1),1,64) and integer(b.get('restitution',100),0,100)
            assert all(type(b[k]) is bool for k in ('static','rolling') if k in b)
            assert e.get('collidable',True) is True and e.get('bounds','clamp')=='clamp'
            if b.get('rolling',False):assert e.get('shape','rect') in ('rect','circle','ball','coin','capsule','triangle','diamond')
            if b.get('static',False):assert not e.get('vx',0) and not e.get('vy',0)
        for e in entities:
            if e['id'] not in seen:assert not e.get('vx',0) and not e.get('vy',0)
        for o in ops:
            assert o.get('op') not in ('move','place','velocity','accelerate','bounce','charge','launch','cancel')
            if o.get('op')=='impulse':
                assert set(o)=={'op','entity','dx','dy'} and o['entity'] in seen
                assert integer(o['dx'],-64,64) and integer(o['dy'],-64,64) and (o['dx'] or o['dy'])
        return []
    except (AssertionError,KeyError,TypeError,ValueError,AttributeError):
        return ['bodies 需要开放网格、20..200ms、1..8 个唯一刚体、有界质量/弹性/重力及 impulse 操作']
