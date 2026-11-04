"""Bounded expression reference model. Never eval/exec or access host resources."""
import json
import re

LIMIT = 2**53-1
def bounded(v):
    nodes = [0]
    def visit(v, depth):
        nodes[0] += 1
        if depth > 8 or nodes[0] > 1024: return False
        if v is None or type(v) is bool: return True
        if type(v) is int: return -LIMIT <= v <= LIMIT
        if isinstance(v, str): return safe_string(v)
        if isinstance(v, list): return len(v) <= 128 and all(visit(x, depth+1) for x in v)
        if isinstance(v, dict): return len(v) <= 32 and all(isinstance(k,str) and k and safe_string(k) and visit(x,depth+1) for k,x in v.items())
        return False
    return visit(v,0) and len(json.dumps(v,ensure_ascii=False,separators=(',',':')).encode()) <= 8192
def safe_string(v):
    return len(v.encode()) <= 512 and not any(ord(c)<32 or 127<=ord(c)<=159 for c in v)
def pointer(v):
    return isinstance(v,str) and len(v.encode())<=160 and v.startswith('/') and not any(ord(c)<32 or 127<=ord(c)<=159 for c in v) and re.search(r'~(?:[^01]|$)',v) is None
def arity(op,n):
    if op in ('if','slice','clamp'): return n==3
    if op in ('sub','div','mod','eq','ne','lt','lte','gt','gte','get'): return n==2
    if op in ('not','len','str','sum','filter','map','sort'): return n==1
    return op in ('add','mul','min','max','and','or','concat','coalesce','array') and 1<=n<=16
def validate(expression,sources):
    count=[0]; deps=set()
    def walk(v,depth,row):
        count[0]+=1
        if depth>12 or count[0]>192: raise ValueError('expression budget')
        if not isinstance(v,dict):
            if not (v is None or type(v) in (bool,int,str)) or not bounded(v): raise ValueError('expression scalar')
            return
        if set(v)=={'literal'}:
            if not bounded(v['literal']): raise ValueError('literal budget')
            return
        if set(v)=={'ref'}:
            p=v['ref']
            if not pointer(p): raise ValueError('pointer')
            parts=p.split('/')
            if parts[1]=='sources':
                if len(parts)<3 or parts[2] not in sources: raise ValueError('unknown source')
                deps.add(parts[2])
            elif parts[1]=='event': pass
            elif parts[1] in ('row','index') and row: pass
            else: raise ValueError('reference scope')
            return
        if set(v)=={'object'}:
            if not isinstance(v['object'],dict) or len(v['object'])>16: raise ValueError('object')
            for k,e in v['object'].items():
                if not isinstance(k,str) or not k or not safe_string(k): raise ValueError('object key')
                walk(e,depth+1,row)
            return
        op=v.get('op'); args=v.get('args'); iteration=op in ('filter','map','sort')
        if not isinstance(args,list) or not arity(op,len(args)) or set(v)!=( {'op','args','value'} if iteration else {'op','args'}): raise ValueError('operator')
        for e in args: walk(e,depth+1,row)
        if iteration: walk(v['value'],depth+1,True)
    walk(expression,0,False)
    return deps
def project(value,path):
    if path=='': return value
    for part in path.split('/')[1:]:
        part=part.replace('~1','/').replace('~0','~')
        if isinstance(value,dict): value=value.get(part)
        elif isinstance(value,list) and re.fullmatch(r'0|[1-9][0-9]*',part): value=value[int(part)] if int(part)<len(value) else None
        else: return None
    return value
def integer(v):
    if type(v) is not int or not -LIMIT<=v<=LIMIT: raise ValueError('integer type or range')
    return v
def boolean(v):
    if type(v) is not bool: raise ValueError('boolean type')
    return v
def equal(a,b):
    if type(a) is not type(b): return False
    if isinstance(a,list): return len(a)==len(b) and all(equal(x,y) for x,y in zip(a,b))
    if isinstance(a,dict): return set(a)==set(b) and all(equal(a[k],b[k]) for k in a)
    return a==b
def evaluate(expression,context,sources):
    validate(expression,sources);fuel=[8192]
    def run(v,row=None,index=None):
        fuel[0]-=1
        if fuel[0]<0: raise ValueError('evaluation budget')
        if not isinstance(v,dict): return v
        if 'literal' in v: return v['literal']
        if 'ref' in v:
            p=v['ref']
            if p=='/index': return index
            if p=='/row' or p.startswith('/row/'): return project(row,p[4:])
            return project(context,p)
        if 'object' in v: return {k:run(e,row,index) for k,e in v['object'].items()}
        op,args=v['op'],v['args']
        if op=='if': return run(args[1 if boolean(run(args[0],row,index)) else 2],row,index)
        if op in ('and','or'):
            for e in args:
                value=boolean(run(e,row,index))
                if value==(op=='or'): return value
            return op=='and'
        if op=='coalesce':
            for e in args:
                value=run(e,row,index)
                if value is not None: return value
            return None
        a=[run(e,row,index) for e in args]
        if op=='array': result=a
        elif op in ('add','mul'):
            result=0 if op=='add' else 1
            for x in a: result=integer(result+integer(x) if op=='add' else result*integer(x))
        elif op in ('sub','div','mod'):
            x,y=map(integer,a)
            if op=='sub': result=integer(x-y)
            else:
                if y==0: raise ValueError('zero divisor')
                q=(abs(x)//abs(y))*(-1 if (x<0)!=(y<0) else 1)
                result=integer(q if op=='div' else x-q*y)
        elif op in ('min','max'): result=(min if op=='min' else max)(map(integer,a))
        elif op=='clamp':
            x,lo,hi=map(integer,a)
            if lo>hi: raise ValueError('clamp bounds')
            result=max(lo,min(hi,x))
        elif op in ('eq','ne'): result=equal(*a)==(op=='eq')
        elif op in ('lt','lte','gt','gte'):
            x,y=a
            if not isinstance(x,str) or not isinstance(y,str): x,y=map(integer,a)
            result={'lt':lambda:x<y,'lte':lambda:x<=y,'gt':lambda:x>y,'gte':lambda:x>=y}[op]()
        elif op=='not': result=not boolean(a[0])
        elif op=='len':
            if not isinstance(a[0],(str,list,dict)): raise ValueError('length type')
            result=len(a[0])
        elif op=='str':
            if isinstance(a[0],(list,dict)): raise ValueError('string type')
            result=a[0] if isinstance(a[0],str) else json.dumps(a[0],ensure_ascii=False,separators=(',',':'))
        elif op=='concat':
            if not all(isinstance(x,str) for x in a): raise ValueError('concat type')
            result=''.join(a)
        elif op=='get':
            if isinstance(a[1],str): result=a[0].get(a[1]) if isinstance(a[0],dict) else None
            else:
                i=integer(a[1])
                if i<0: raise ValueError('index')
                result=a[0][i] if isinstance(a[0],list) and i<len(a[0]) else None
        elif op=='slice':
            start,count=map(integer,a[1:])
            if not isinstance(a[0],list) or start<0 or not 0<=count<=128: raise ValueError('slice')
            result=a[0][start:start+count]
        elif op=='sum':
            if not isinstance(a[0],list): raise ValueError('sum type')
            result=0
            for x in a[0]: result=integer(result+integer(x))
        elif op in ('filter','map','sort'):
            if not isinstance(a[0],list) or len(a[0])>128: raise ValueError('collection')
            pairs=[(x,run(v['value'],x,i)) for i,x in enumerate(a[0])]
            if op=='filter': result=[x for x,b in pairs if boolean(b)]
            elif op=='map': result=[y for _,y in pairs]
            else:
                if pairs:
                    strings=isinstance(pairs[0][1],str)
                    if any(isinstance(k,str)!=strings for _,k in pairs): raise ValueError('sort key')
                    if not strings:
                        for _,k in pairs:integer(k)
                result=[x for x,_ in sorted(pairs,key=lambda p:p[1])]
        else: raise ValueError('unknown operator')
        if not bounded(result): raise ValueError('result budget')
        return result
    result=run(expression)
    if not bounded(result): raise ValueError('result budget')
    return result
