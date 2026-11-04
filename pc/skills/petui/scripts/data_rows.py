"""p4-data-rows-v1: generic paged/filtered/selectable projection of a bounded data list (<=20 rows)."""
import re
ENGINE='p4-data-rows-v1'
FIELDS=('label','value','detail','meta','tone')
BIND_FIELDS=('label','value','detail','meta')
PAGE=5
REFRESH='data.refresh'
def _name(v):return isinstance(v,str) and re.fullmatch(r'[A-Za-z0-9_.-]{1,31}',v) is not None
def config(data,runtime=None):
    """Strict data extension; raises ValueError. Returns the normalized config."""
    def need(q):
        if not q:raise ValueError('Invalid data rows declaration')
    need(isinstance(data,dict) and {'source','page_var'}<=set(data)<={'source','page_var','select_var','filters','refresh_action'})
    out={'select_var':data.get('select_var'),'filters':[],'refresh_action':data.get('refresh_action')}
    variables=runtime.get('vars',{}) if runtime else None
    def int_var(name):
        need(_name(name) and name!=data['page_var'])
        if variables is not None:need(variables.get(name,{}).get('type')=='int')
    if 'select_var' in data:int_var(data['select_var'])
    if 'refresh_action' in data:
        need(data['refresh_action']==REFRESH)
        if runtime is not None:need(any(isinstance(t,dict) and t.get('on')==REFRESH for t in runtime.get('transitions',[])))
    if 'filters' in data:
        need(isinstance(data['filters'],list) and 1<=len(data['filters'])<=4);seen=set()
        for f in data['filters']:
            need(isinstance(f,dict) and set(f)=={'var','field','values'} and f['field'] in FIELDS and f['var'] not in seen);seen.add(f['var'])
            int_var(f['var']);values=f['values']
            need(isinstance(values,list) and 1<=len(values)<=8)
            norm=[]
            for v in values:
                if f['field']=='tone' and type(v) is int and -1<=v<=1:norm.append(str(v))
                else:need(isinstance(v,str) and 1<=len(v.encode())<=24 and not any(ord(c)<32 or ord(c)==127 for c in v));norm.append(v)
            out['filters'].append({'var':f['var'],'field':f['field'],'values':norm})
    return out
def errors(runtime):
    if not isinstance(runtime,dict):return []
    data=runtime.get('data');s=runtime.get('surface',{}) if isinstance(runtime.get('surface'),dict) else {}
    extended=isinstance(data,dict) and any(k in data for k in ('select_var','filters','refresh_action'))
    rows_layout=isinstance(s.get('data_view'),dict) and s['data_view'].get('layout')=='rows'
    row_binds=[n for n in s.get('elements',[]) if isinstance(n,dict) and 'row_bind' in n]
    if not extended and not rows_layout and not row_binds:return []
    bad=['通用数据行需要 p4-data-rows-v1：data_view、整数筛选/选择变量、data.refresh 保留动作与 row_bind 文本节点']
    try:
        assert isinstance(data,dict) and isinstance(s.get('data_view'),dict) and s.get('bindings')=='p4-variable-bindings-v1'
        if extended:config(data,runtime)
        for n in row_binds:
            assert n['row_bind'] in BIND_FIELDS and n.get('type') in ('text','button') and not any(k in n for k in ('text','bind','var_bind','field_bind','key_for'))
        return []
    except (AssertionError,KeyError,TypeError,ValueError,AttributeError):return bad
def field(row,name):
    return str(row.get('tone',0)) if name=='tone' else str(row.get(name,''))
def matches(row,f,value):
    if value<=0:return True
    if value>len(f['values']):return False
    return field(row,f['field'])==f['values'][value-1]
def filter_rows(rows,cfg,values):
    """Visible rows (source order) for the current int filter values."""
    out=[]
    for row in rows[:20]:
        if all(matches(row,f,int(values.get(f['var'],0) or 0)) for f in (cfg or {}).get('filters',[])):out.append(row)
    return out
def pages(count):return max(1,(count+PAGE-1)//PAGE)
def page_index(count,value):return value%pages(count)
def select(count,value):
    if not count:return -1
    return 0 if value<0 else min(count-1,value)
def view(rows,cfg,variables,page_var,select_var=None):
    """Filtered rows, current page slice, page info and the selected row (or None)."""
    visible=filter_rows(rows,cfg,variables);total=pages(len(visible));at=page_index(len(visible),int(variables.get(page_var,0) or 0))
    chosen=select(len(visible),int(variables.get(select_var,0) or 0)) if select_var else -1
    return {'rows':visible[at*PAGE:(at+1)*PAGE],'page':at,'pages':total,'count':len(visible),'selected':visible[chosen] if chosen>=0 else None}
