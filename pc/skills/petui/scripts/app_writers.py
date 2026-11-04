"""Read-only derived count mirrors; sources, conditions and persistence retain their profiles."""
def derived_errors(r):
    try:
        f=r.get('form',{});roles={v for v in [f.get('focus_var'),f.get('active_var'),*[q.get(k) for q in f.get('fields',[]) for k in ('var','draft_var')],*[v for k,v in f.get('keyboard',{}).items() if k.endswith('_var')]] if isinstance(v,str)}
        seen=set();program=r.get('program',{}).get('outputs',[])
        for n in r.get('surface',{}).get('elements',[]):
            if n.get('type')!='longtext' or 'count_var' not in n:continue
            k=n['count_var'];host=r.get('host') is True and ('ownership' not in r or k in r['ownership'].get('host',[]))
            assert isinstance(k,str) and r.get('vars',{}).get(k,{}).get('type')=='int' and k!=n.get('page_var') and k not in seen|roles and k not in program and not host
            seen.add(k)
        if seen:
            rules=[branch for t in r.get('transitions',[]) for branch in (t,t.get('else',{}))]+[branch for t in r.get('tick',[]) for branch in (t,t.get('then',{}))]
            for rule in rules:
                for op in ('set','inc'):
                    if seen.intersection(rule.get(op,{})):return ['longtext count_var is a read-only derived writer, not a guest set/inc target']
        return []
    except (AssertionError,TypeError,KeyError,AttributeError):return ['longtext count_var requires an exclusive derived integer writer']
