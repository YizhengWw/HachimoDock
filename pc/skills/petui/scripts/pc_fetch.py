"""pc-fetch-v1: generic PC HTTP(S) adapter declared in pc/application.json.

The PC polls each declared source (GET, https only, host in the user-managed
allowlist), caches it, maps JSON pointers into runtime vars (variable bindings)
or into data-list rows (widget/data) and pushes honest states. No URL is ever
fetched by the device and no credential may appear in the package.
"""
import ipaddress
import json
import math
import re
from urllib.parse import parse_qsl, urlsplit
ENGINE='pc-fetch-v1'
CAPABILITY='widgetPcFetch'
SERVICE='fetch'
MAX_SOURCES=4
MAX_BYTES=65536
MIN_INTERVAL=60
SAFE_HEADERS={'Accept':{'application/json','text/plain'},'Accept-Language':None,'User-Agent':None,'Cache-Control':{'no-cache'}}
SECRET_QUERY={'key','api_key','apikey','token','access_token','password','secret','authorization','sig','signature'}
ROW_FIELDS=('id','label','value','detail','meta','tone')
ROW_LIMITS={'id':23,'label':36,'value':24,'detail':24,'meta':63}
def _name(v,n):return isinstance(v,str) and re.fullmatch(r'[A-Za-z0-9_.-]{1,%d}'%n,v) is not None
def pointer_ok(p):return isinstance(p,str) and len(p.encode())<=128 and (p=='' or p.startswith('/')) and re.search(r'~(?:[^01]|$)',p) is None
def host_ok(h):
    if not isinstance(h,str) or not 1<=len(h)<=253 or '.' not in h:return False
    if not all(re.fullmatch(r'[a-z0-9](?:[a-z0-9-]*[a-z0-9])?',part) and len(part)<=63 for part in h.split('.')):return False
    try:ipaddress.ip_address(h)
    except ValueError:return True
    return False
def url_host(url):
    try:
        u=urlsplit(url)
        port=u.port
    except (ValueError,TypeError,AttributeError):return None
    if u.scheme!='https' or not u.hostname or not host_ok(u.hostname) or u.username or u.password or u.fragment or port not in (None,443):return None
    if any(k.lower() in SECRET_QUERY for k,_ in parse_qsl(u.query,keep_blank_values=True)):return None
    return u.hostname
def host_allowed(url,allowlist):
    """Exact host match against the user's allowlist; no wildcards, no IP literals."""
    h=url_host(url);return h is not None and host_ok(h) and h in {a for a in allowlist if host_ok(a)}
def source_errors(src,runtime,used_ids):
    assert isinstance(src,dict) and {'id','url','interval_sec','map'}<=set(src)<={'id','url','method','headers','interval_sec','ttl_sec','max_bytes','map'}
    assert _name(src['id'],16) and src['id'] not in used_ids;used_ids.add(src['id'])
    assert isinstance(src['url'],str) and len(src['url'].encode())<=2048 and url_host(src['url'])
    assert src.get('method','GET')=='GET'
    headers=src.get('headers',{});assert isinstance(headers,dict) and len(headers)<=4
    for k,v in headers.items():
        assert k in SAFE_HEADERS and isinstance(v,str) and 1<=len(v.encode())<=120 and not any(ord(c)<32 or ord(c)==127 for c in v)
        assert SAFE_HEADERS[k] is None or v in SAFE_HEADERS[k]
    interval=src['interval_sec'];assert type(interval) is int and MIN_INTERVAL<=interval<=86400
    ttl=src.get('ttl_sec',interval*3);assert type(ttl) is int and interval<=ttl<=604800
    assert type(src.get('max_bytes',MAX_BYTES)) is int and 1024<=src.get('max_bytes',MAX_BYTES)<=MAX_BYTES
    m=src['map'];assert isinstance(m,dict) and len(m)==1 and set(m)<={'vars','rows'}
    if 'vars' in m:
        variables=runtime.get('vars',{});assert isinstance(m['vars'],dict) and 1<=len(m['vars'])<=8
        for var,ptr in m['vars'].items():assert variables.get(var,{}).get('type') in ('int','string') and pointer_ok(ptr)
        assert runtime.get('host') is True and runtime.get('surface',{}).get('bindings')=='p4-variable-bindings-v1'
        return set(m['vars'])
    rows=m['rows'];assert isinstance(rows,dict) and {'items','id','label'}<=set(rows)<=set(('items',)+ROW_FIELDS)
    for k,ptr in rows.items():assert pointer_ok(ptr)
    data=runtime.get('data');assert isinstance(data,dict) and data.get('source')=='fetch.'+src['id']
    return set()
def fetch_errors(value,runtime):
    try:
        assert isinstance(value,dict) and set(value)<={'schemaVersion','engine','service','actions','content'}
        assert value['schemaVersion']==1 and value['engine']==ENGINE and value['service']==SERVICE
        content=value.get('content',[]);assert isinstance(content,list) and 1<=len(content)<=MAX_SOURCES
        assert len(json.dumps(value,ensure_ascii=False,separators=(',',':')).encode())<=16384
        actions=value.get('actions',{});assert isinstance(actions,dict) and len(actions)<=4
        used=set();bound=set();row_sources=0
        for src in content:
            bound|=source_errors(src,runtime,used)
            row_sources+='rows' in src['map']
        assert row_sources<=1
        for a,op in actions.items():
            assert re.fullmatch(r'host\.[a-z_]{1,32}',a) and op in {f"{sid}:refresh" for sid in used}
            assert any(isinstance(t,dict) and t.get('on')==a and set(t)=={'from','on'} and t['from']=='*' for t in runtime.get('transitions',[]))
        variables=runtime.get('vars',{})
        if bound:
            assert runtime.get('host') is True and runtime.get('surface',{}).get('bindings')=='p4-variable-bindings-v1' and not runtime.get('tick') and 'persist' not in runtime
            assert set(variables)==bound
        else:assert runtime.get('host') is None or runtime.get('host') is True
        return []
    except (AssertionError,KeyError,TypeError,ValueError,AttributeError):return ['pc-fetch-v1 需要 ≤4 个 https 来源、允许主机、≥60 s 间隔、≤64 KiB 响应与 JSON 指针到变量或数据行的映射']
def resolve_pointer(doc,ptr):
    if ptr=='':return doc
    cur=doc
    for part in ptr.split('/')[1:]:
        part=part.replace('~1','/').replace('~0','~')
        if isinstance(cur,dict):
            if part not in cur:return None
            cur=cur[part]
        elif isinstance(cur,list):
            if not re.fullmatch(r'0|[1-9][0-9]*',part) or int(part)>=len(cur):return None
            cur=cur[int(part)]
        else:return None
    return cur
def scalar_for(value,kind):
    """Project a JSON leaf to a device var; None means 'not representable' (binding error)."""
    if kind=='int':
        if type(value) is bool or value is None:return None
        if type(value) is int:return value if -10**9<=value<=10**9 else None
        if isinstance(value,float) and value.is_integer():return int(value) if -10**9<=value<=10**9 else None
        if isinstance(value,str) and re.fullmatch(r'-?\d{1,10}',value.strip()):n=int(value);return n if -10**9<=n<=10**9 else None
        return None
    if isinstance(value,str):return ''.join(c for c in value if ord(c)>=32 and not 127<=ord(c)<160).encode()[:63].decode('utf-8','ignore')
    if type(value) is bool:return 'true' if value else 'false'
    if isinstance(value,(int,float)) and value is not None:return str(value) if (not isinstance(value,float) or math.isfinite(value)) and len(str(value))<=63 else None
    return None
def map_vars(doc,mapping,variables):
    """vars <- JSON pointers; returns (values, errors) with typed fallbacks never invented."""
    out={};errors=[]
    for var,ptr in mapping.items():
        kind=variables.get(var,{}).get('type');value=scalar_for(resolve_pointer(doc,ptr),kind)
        if value is None:errors.append(var)
        else:out[var]=value
    return out,errors
def _row_text(value,limit):
    if value is None or isinstance(value,(dict,list)):return ''
    s=value if isinstance(value,str) else ('true' if value is True else 'false' if value is False else str(value))
    s=''.join(c for c in s if ord(c)>=32 and ord(c)!=127);raw=s.encode()[:limit]
    return raw.decode('utf-8','ignore')
def map_rows(doc,mapping):
    """rows <- items pointer + per-item field pointers; <=20 rows, ids unique and non-empty."""
    items=resolve_pointer(doc,mapping['items'])
    if not isinstance(items,list):return None
    rows=[];seen=set()
    for item in items:
        row={'id':_row_text(resolve_pointer(item,mapping['id']),ROW_LIMITS['id']),'label':_row_text(resolve_pointer(item,mapping['label']),ROW_LIMITS['label'])}
        for k in ('value','detail','meta'):row[k]=_row_text(resolve_pointer(item,mapping[k]),ROW_LIMITS[k]) if k in mapping else ''
        tone=resolve_pointer(item,mapping['tone']) if 'tone' in mapping else 0
        if isinstance(tone,str) and re.fullmatch(r'-?\d+',tone.strip()):tone=int(tone)
        row['tone']=1 if isinstance(tone,(int,float)) and not isinstance(tone,bool) and tone>0 else -1 if isinstance(tone,(int,float)) and not isinstance(tone,bool) and tone<0 else 0
        if not row['id'] or row['id'] in seen:continue
        seen.add(row['id']);rows.append(row)
        if len(rows)==20:break
    return rows
def snapshot(source_id,state,now_ms):
    """Honest widget/data snapshot for a cached fetch result: 等待数据 / 数据已过期 / 上游错误."""
    src='fetch.'+source_id;rows=state.get('rows');fetched=state.get('fetched_ms');ttl=state.get('ttl_ms',180000)
    if fetched is None:return {'schema':1,'source':src,'ttlMs':30000,'status':'error' if state.get('error') else 'empty','message':'上游错误' if state.get('error') else '等待数据','rows':[]}
    stale=now_ms-fetched>ttl
    if state.get('error') and rows is None:return {'schema':1,'source':src,'ttlMs':30000,'status':'error','message':'上游错误：'+state['error'][:40],'rows':[]}
    return {'schema':1,'source':src,'ttlMs':min(300000,max(5000,ttl)),'status':'error' if stale else ('ok' if rows else 'empty'),'message':'数据已过期' if stale else ('' if rows else '暂无数据'),'rows':rows or []}
