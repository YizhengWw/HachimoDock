"""Pure declarative application layout. No scripts or host calls in this layer."""
import copy
import re

ENGINE = 'p4-app-surface-v1'
BINDINGS = {'title','eyebrow','headline','metricLabel','metricValue','metricUnit','badge','note','footer','progress'}
ICONS = {'home','leaf','desk','agent','compass','sun','mic','book','sound','spark','heart','pause','play','check','plus','list','arrow','cloud','clock','stretch'}

def graphics_errors(s, runtime=None):
    """Graphics v1 is opt-in. Old media/image budgets are deliberately unchanged."""
    def num(o,k,lo,hi,default=None):
        v=o.get(k,default)
        if type(v) is not int or not lo<=v<=hi:raise ValueError(k)
        return v
    try:
        if set(s)-{'engine','graphics','background','images','elements','typography','fonts','interaction','game_view','data_view','scene_view'} or s['graphics']!='p4-app-graphics-v1' or s['engine']!=ENGINE:raise ValueError('root')
        if 'scene_view' in s:
            v=s['scene_view']
            if not isinstance(v,dict) or set(v)!={'engine','x','y','w','h','colors','sprites'} or v['engine']!='p4-scene-surface-v1' or any(k in s for k in ('game_view','data_view')):raise ValueError('scene view')
            x=num(v,'x',0,639);y=num(v,'y',0,479)
            num(v,'w',64,640-x);num(v,'h',64,480-y)
            if not isinstance(v['colors'],list) or len(v['colors'])!=5 or any(type(c)is not int or not 0<=c<=65535 for c in v['colors']):raise ValueError('scene colors')
            if runtime is not None and ('scene' not in runtime or any(k in runtime for k in ('host','data','game','media','instrument')) or runtime['scene'].get('engine')=='p4-touch-slice-v1' or 'sprites' in runtime['scene'] or any('sprite' in e for e in runtime['scene'].get('entities',[]))):raise ValueError('scene owner')
            if runtime is not None and any(k in runtime['scene']['grid'] for k in ('rows','solid_tones')):raise ValueError('scene surface v1 requires open grid')
            if not isinstance(v['sprites'],list) or len(v['sprites'])>4:raise ValueError('scene images')
            seen_entities=set()
            for sp in v['sprites']:
                if set(sp)!={'entity','image','columns','fps'} or not isinstance(sp['entity'],str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,15}',sp['entity']) or sp['entity'] in seen_entities:raise ValueError('scene image')
                seen_entities.add(sp['entity'])
                if runtime is not None and sp['entity'] not in {e['id'] for e in runtime['scene']['entities']}:raise ValueError('scene entity')
                cols=num(sp,'columns',1,8);num(sp,'fps',1,20)
                im=next((im for im in s.get('images',[]) if im['id']==sp['image']),None)
                if not im or 'encoding' in im or im['frame_width']%cols or im['frame_width']//cols<8:raise ValueError('scene atlas')
        if 'data_view' in s:
            v=s['data_view']
            if not isinstance(v,dict) or set(v)!={'engine','x','y','w','h','layout','colors'} or v['engine']!='p4-data-surface-v1' or v['layout'] not in ('todos','metrics') or 'game_view' in s:raise ValueError('data view')
            x=num(v,'x',0,639);y=num(v,'y',0,479)
            num(v,'w',320,640-x);num(v,'h',300,480-y)
            if not isinstance(v['colors'],list) or len(v['colors'])!=5 or any(type(c)is not int or not 0<=c<=65535 for c in v['colors']):raise ValueError('data colors')
            if runtime is not None and ('data' not in runtime or any(k in runtime for k in ('host','scene','game','media','instrument'))):raise ValueError('data owner')
        if 'game_view' in s:
            v=s['game_view']
            if not isinstance(v,dict) or set(v)!={'engine','x','y','w','h'} or v['engine']!='p4-game-surface-v1':raise ValueError('game view')
            x=num(v,'x',0,639);y=num(v,'y',0,479)
            num(v,'w',64,640-x);num(v,'h',64,480-y)
            if runtime is not None and (runtime.get('game',{}).get('type') not in ('snake','blocks','flappy') or any(k in runtime for k in ('host','scene','data','media','instrument'))):raise ValueError('game owner')
        typo=s.get('typography')=='p4-app-type-v1'
        interactive=s.get('interaction')=='p4-app-interaction-v1'
        if 'interaction' in s and (not interactive or not typo):raise ValueError('interaction')
        if 'typography' in s and not typo:raise ValueError('typography')
        num(s,'background',0,65535)
        images=s.get('images',[]);ids={};pixels=0
        if not isinstance(images,list) or ('images' in s and not 1<=len(images)<=4):raise ValueError('images')
        for i,im in enumerate(images):
            if set(im)-{'id','asset','frame_width','frame_height','frames','fps','encoding'}:raise ValueError('image fields')
            if not isinstance(im['id'],str) or not re.fullmatch(r'[a-z0-9_-]{1,15}',im['id']) or im['id'] in ids:raise ValueError('id')
            if not isinstance(im['asset'],str) or not re.fullmatch(r'assets/[A-Za-z0-9_.-]+\.png',im['asset']) or '..' in im['asset']:raise ValueError('asset')
            jpeg=im.get('encoding')=='jpeg'
            if 'encoding' in im and not jpeg:raise ValueError('encoding')
            w=num(im,'frame_width',8,640 if jpeg else 512);h=num(im,'frame_height',8,480 if jpeg else 240)
            num(im,'frames',1,1);num(im,'fps',1,1)
            if jpeg and (i!=0 or (w,h)!=(640,480)):raise ValueError('background')
            if not jpeg:pixels+=w*h
            ids[im['id']]=jpeg
        if pixels>98304:raise ValueError('image budget')
        if 'fonts' in s:
            if not typo or not isinstance(s['fonts'],list) or not 1<=len(s['fonts'])<=2:raise ValueError('fonts')
            seen=set();seen_images=set()
            for f in s['fonts']:
                if set(f)-{'image','chars','metrics','cell'} or not {'image','chars','metrics'}<=set(f) or f['image'] not in ids or ids[f['image']] or f['image'] in seen_images:raise ValueError('font image')
                seen_images.add(f['image']);im=next(im for im in images if im['id']==f['image'])
                w,h=im['frame_width'],im['frame_height'];chars=f['chars'];metrics=f['metrics']
                cell=f.get('cell',[32,32])
                if not isinstance(cell,list) or len(cell)!=2 or any(type(v) is not int for v in cell) or cell[0] not in (16,32,48,64) or cell[1] not in (32,48,64) or cell[0]>cell[1]:raise ValueError('font cell size')
                cw,ch=cell
                if w>240 or h>240 or w%cw or h%ch or not isinstance(chars,str) or not 1<=len(chars)<=49 or len(chars)>(w//cw)*(h//ch) or not isinstance(metrics,list) or len(metrics)!=len(chars):raise ValueError('font cells')
                for c,m in zip(chars,metrics):
                    if c in seen or ord(c)<32 or 127<=ord(c)<160 or ord(c)>65535 or 0xD800<=ord(c)<=0xDFFF:raise ValueError('font character')
                    seen.add(c)
                    if not isinstance(m,list) or len(m)!=3 or any(type(v) is not int for v in m) or not 4<=m[0]<=32 or not 0<=m[1]<=m[2]<=32:raise ValueError('font metrics')
        nodes=s['elements']
        if not isinstance(nodes,list) or not 1<=len(nodes)<=32:raise ValueError('elements')
        allowed={'type','x','y','w','h','color','page','scale','radius','bind','text','image','action','motion','opacity','opacity_end','font_size','line_height','align','valign','padding','gap','icon_size','weight','foreground','stroke','stroke_width','icon','gradient','key_for','tile','show','state_mask','host_mask'}
        actions={r.get('on') for r in runtime.get('transitions',[])} if runtime is not None else None
        for i,n in enumerate(nodes):
            if set(n)-allowed or n['type'] not in {'panel','text','meter','image','icon','button'}:raise ValueError('node')
            t=n['type'];x=num(n,'x',0,639);y=num(n,'y',0,479);w=num(n,'w',1,640-x);h=num(n,'h',1,480-y);num(n,'color',0,65535)
            if 'state_mask' in n:
                mask=num(n,'state_mask',1,63)
                if runtime is not None and (runtime.get('input',{}).get('engine')!='p4-input-v1' or mask>>len(runtime.get('states',[]))):raise ValueError('state visibility')
            if 'host_mask' in n:
                num(n,'host_mask',1,255)
                if runtime is not None and (runtime.get('host') is not True or runtime.get('input',{}).get('engine')!='p4-input-v1'):raise ValueError('host visibility')
            num(n,'radius',0,min(24,w//2,h//2),0);num(n,'scale',1,3,1)
            p=num(n,'page',0,4,4)
            if runtime is not None and p!=4 and p>=max(1,len(runtime.get('pages',[]))):raise ValueError('page')
            for key in ('opacity','opacity_end'):
                if key in n and (t not in ({'panel','button'} if key=='opacity' else {'panel'}) or not 0<=num(n,key,0,255)<=255):raise ValueError(key)
            size=num(n,'font_size',10,96,16);num(n,'line_height',size,120,(size*5+3)//4)
            pad=num(n,'padding',0,32,16 if t=='button' else 0);num(n,'gap',0,24,9);isz=num(n,'icon_size',8,48,20)
            for key in ('foreground','stroke'):num(n,key,0,65535,n['color'])
            num(n,'stroke_width',0,2,0)
            if n.get('weight',400) not in (400,600) or type(n.get('weight',400)) is not int:raise ValueError('weight')
            if n.get('align','left') not in ('left','center','right') or n.get('valign','top') not in ('top','center','bottom'):raise ValueError('align')
            if 'gradient' in n and (t!='panel' or n['gradient'] not in ('horizontal','vertical')):raise ValueError('gradient')
            if 'icon' in n and (t not in ('icon','button') or n['icon'] not in ICONS):raise ValueError('icon')
            if t=='icon' and ('icon' not in n or w>48 or h>48):raise ValueError('icon box')
            if 'text' in n and (not isinstance(n['text'],str) or len(n['text'].encode())>120 or any(ord(c)<32 or ord(c)==127 for c in n['text'])):raise ValueError('text')
            if 'bind' in n and n['bind'] not in BINDINGS:raise ValueError('bind')
            if t in ('text','button') and (('text' in n)==('bind' in n) or h<size):raise ValueError('content')
            if t=='button' and (2*pad>=w or ('icon' in n and isz>h)):raise ValueError('button box')
            if t=='meter' and n.get('bind')!='progress':raise ValueError('meter')
            if 'action' in n and (not isinstance(n['action'],str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,47}',n['action']) or (actions is not None and n['action'] not in actions)):raise ValueError('action')
            if 'key_for' in n and (not typo or t!='button' or 'action' in n or 'bind' in n or not isinstance(n['key_for'],str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,47}',n['key_for']) or (actions is not None and n['key_for'] not in actions)):raise ValueError('key hint')
            if 'motion' in n and (n['motion']!='float' or t!='image' or y<4 or y+h>476):raise ValueError('motion')
            if t=='image':
                if n.get('image') not in ids:raise ValueError('image ref')
                if ids[n['image']] and (i!=0 or (x,y,w,h,p)!=(0,0,640,480,4) or any(k in n for k in ('action','motion','tile','show','state_mask','host_mask'))):raise ValueError('static background')
            elif 'image' in n:raise ValueError('image fields')
            for key in ('tile','show'):
                if key not in n:continue
                spec=n[key]
                if not interactive or not isinstance(spec,dict):raise ValueError(key)
                refs=[spec.get('var')]+([spec['max_var']] if 'max_var' in spec else [])
                if any(not isinstance(v,str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,31}',v) or (runtime is not None and runtime.get('vars',{}).get(v,{}).get('type')!='int') for v in refs):raise ValueError('variable')
                if key=='show':
                    if set(spec)!={'var','min','max'}:raise ValueError('show')
                    lo=num(spec,'min',-10**9,10**9);num(spec,'max',lo,10**9)
                else:
                    if t!='image' or ids[n['image']] or set(spec)-{'columns','rows','row','var','max_var'}:raise ValueError('tile')
                    cols=num(spec,'columns',1,8);rows=num(spec,'rows',1,8);num(spec,'row',0,rows-1)
                    im=next(im for im in images if im['id']==n['image'])
                    if im['frame_width']%cols or im['frame_height']%rows or im['frame_width']//cols<8 or im['frame_height']//rows<8:raise ValueError('tile geometry')
        return []
    except (ValueError,KeyError,TypeError,AttributeError):return ['app graphics 需要有界图标/字体/按钮及单张原生 640×480 静态背景']

def persistence_errors(runtime):
    p=runtime.get('persist')
    if p is None:return []
    try:
        assert runtime.get('surface',{}).get('engine')==ENGINE
        assert isinstance(p,dict) and {'version','vars','states'}<=set(p) and set(p)<={'version','vars','states','page'}
        if 'page' in p:assert type(p['page']) is bool and (not p['page'] or runtime.get('pages'))
        assert type(p['version']) is int and 1<=p['version']<=65535
        assert isinstance(p['vars'],list) and 1<=len(p['vars'])<=8
        assert all(isinstance(v,str) for v in p['vars']) and len(set(p['vars']))==len(p['vars'])
        assert set(p['vars'])<=set(runtime.get('vars',{}))
        assert all(runtime['vars'][v].get('type')=='int' for v in p['vars'])
        assert isinstance(p['states'],dict) and set(p['states'])==set(runtime['states'])
        assert all(isinstance(v,str) and v in runtime['states'] for v in p['states'].values())
        return []
    except (AssertionError,TypeError,KeyError,AttributeError):return ['persist 需要版本、唯一整数变量子集与完整的安全恢复状态映射']

def app_runtime_errors(runtime):
    errors=persistence_errors(runtime)
    try:
        if 'lifecycle' in runtime:
            life=runtime['lifecycle'];g=runtime.get('game',runtime.get('scene',{}))
            assert isinstance(life,dict) and not set(life)-{'pause_action','record_var'}
            active=g.get('playing_state',g.get('active_state'));result=g.get('result_state')
            assert active in runtime['states'] and result in runtime['states']
            assert isinstance(life.get('pause_action'),str) and life['pause_action']
            assert any(t.get('on')==life['pause_action'] and t.get('from')==active and t.get('to') in runtime['states'] and t['to'] not in (active,result) and any(b.get('on')==life['pause_action'] and b.get('from')==t['to'] and b.get('to')==active for b in runtime['transitions']) for t in runtime['transitions'])
            if 'record_var' in life:
                record=life['record_var']
                assert runtime['vars'].get(record,{}).get('type')=='int' and record!=g.get('score_var') and record in runtime.get('persist',{}).get('vars',[])
        app=runtime.get('surface',{}).get('engine')==ENGINE
        variables=runtime.get('vars',{})
        limits=runtime.get('limits',{})
        assert isinstance(limits,dict) and (app or not limits)
        for k,bounds in limits.items():
            assert k in variables and variables[k].get('type')=='int'
            assert isinstance(bounds,list) and len(bounds)==2 and all(type(n) is int for n in bounds)
            assert -1000000000<=bounds[0]<=variables[k].get('init',0)<=bounds[1]<=1000000000
        if not app:return errors
        for rule in [*runtime.get('transitions',[]),*runtime.get('tick',[]),*[t['then'] for t in runtime.get('tick',[]) if 'then' in t]]:
            for op in ('set','inc'):
                assert isinstance(rule.get(op,{}),dict)
                for name,value in rule.get(op,{}).items():
                    assert name in variables
                    kind=variables[name]['type']
                    if op=='set' and isinstance(value,dict):
                        assert set(value)=={'var'} and isinstance(value['var'],str) and variables.get(value['var'],{}).get('type')==kind=='int'
                    elif kind=='int':assert type(value) is int and -1000000000<=value<=1000000000
                    else:assert op=='set' and isinstance(value,str) and len(value.encode())<=63
        return errors
    except (AssertionError,TypeError,KeyError,AttributeError):return errors+['app runtime 仅允许已声明的有界整数限制、整数复制与类型匹配的 set/inc']

def app_surface_errors(surface, runtime=None):
    from pc_extension import surface_errors
    if isinstance(surface,dict) and 'graphics' in surface:return graphics_errors(surface,runtime)
    try:
        if not isinstance(surface,dict) or surface.get('engine')!=ENGINE:
            raise ValueError('engine')
        normalized=copy.deepcopy(surface)
        normalized.pop('engine')
        nodes=normalized.get('elements')
        if not isinstance(nodes,list): raise ValueError('elements')
        actions={r.get('on') for r in runtime.get('transitions',[]) if isinstance(r,dict)} if isinstance(runtime,dict) else None
        pages=len(runtime.get('pages',[])) if isinstance(runtime,dict) else 4
        for n in nodes:
            if not isinstance(n,dict) or n.get('type') not in {'panel','text','meter','image'}: raise ValueError('type')
            if 'rotate_ms' in n: raise ValueError('rotation')
            for key in ('opacity','opacity_end'):
                if key in n:
                    value=n.pop(key)
                    if n['type']!='panel' or type(value) is not int or not 0<=value<=255: raise ValueError('opacity')
            if 'bind' in n:
                if n['bind'] not in BINDINGS: raise ValueError('bind')
                if n['type']=='meter' and n['bind']!='progress': raise ValueError('meter')
                n['bind']='level' if n['type']=='meter' else 'title'
            if 'page' in n:
                p=n['page']
                if type(p) is not int or not 0<=p<=4: raise ValueError('page')
                if runtime is not None and p!=4 and p>=max(1,pages): raise ValueError('unreachable page')
                n['page']=2 if p==4 else min(p,1)
            if 'action' in n:
                a=n.pop('action')
                if not isinstance(a,str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,47}',a) or (actions is not None and a not in actions): raise ValueError('action')
            if 'motion' in n:
                if n.pop('motion')!='float' or n['type']!='image' or type(n.get('y')) is not int or type(n.get('h')) is not int or n['y']<4 or n['y']+n['h']>476: raise ValueError('motion')
        return surface_errors(normalized)
    except (ValueError,TypeError,KeyError,AttributeError):
        return ['app surface 需要受控 dashboard 绑定、有效页面/动作和有界局部动效']
