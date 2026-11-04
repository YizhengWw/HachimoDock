"""Pure declarative application layout. No scripts or host calls in this layer."""
import copy
import re

ENGINE = 'p4-app-surface-v1'
GRAPHICS_V2 = 'p4-graphics-v2'
PARTICLES_ENGINE = 'p4-particles-v1'
KEYFRAME_ENGINE = 'p4-keyframe-bg-v1'
# Resource ceilings per package profile: v1 for every package, v2 only with runtime.graphics="p4-graphics-v2".
GRAPHICS_V1_LIMITS = {'images':4,'imagePixels':98304,'imageBytes':128*1024,'fonts':2,'fontChars':49,'fontWidth':240,'sceneSprites':4,'spritePixels':4096,'keyframes':1}
GRAPHICS_V2_LIMITS = {'images':6,'imagePixels':196608,'imageBytes':256*1024,'fonts':4,'fontChars':98,'fontWidth':512,'sceneSprites':8,'spritePixels':16384,'keyframes':4}
PARTICLE_OPS = ('lte','lt','gte','gt','eq','ne')

def graphics_limits(runtime):
    """p4-graphics-v2 ceilings apply only when the package names the profile."""
    return GRAPHICS_V2_LIMITS if isinstance(runtime,dict) and runtime.get('graphics')==GRAPHICS_V2 else GRAPHICS_V1_LIMITS

def keyframe_declaration(surface):
    """(frames, fps) of a keyframe background, or None for a static one."""
    images=surface.get('images',[]) if isinstance(surface,dict) else []
    im=images[0] if images and isinstance(images[0],dict) else None
    if im is None or im.get('encoding')!='jpeg' or im.get('frames',1)==1:return None
    return (im.get('frames'),im.get('fps'))

def keyframe_asset(asset,frame):
    """Source PNG of frame k>=2: assets/bg.png -> assets/bg.2.png (frame 1 is the declared asset)."""
    return asset if frame<2 else asset[:-4]+f'.{frame}.png'

def particle_errors(s,runtime=None):
    """p4-particles-v1: 1-2 bounded decorative emitters inside a graphics surface."""
    def num(o,k,lo,hi,default=None):
        v=o.get(k,default)
        if type(v) is not int or not lo<=v<=hi:raise ValueError(k)
        return v
    try:
        emitters=s['particles']
        if s.get('graphics')!='p4-app-graphics-v1' or not isinstance(emitters,list) or not 1<=len(emitters)<=2:raise ValueError('particles')
        actions={r.get('on') for r in runtime.get('transitions',[])} if runtime is not None else None
        for e in emitters:
            if not isinstance(e,dict) or set(e)-{'x','y','w','h','count','life_ms','speed','spread','gravity','color','shape','trigger','budget','when','page'}:raise ValueError('emitter')
            if not {'x','y','w','h','count','life_ms','speed','spread','gravity','color','shape','trigger','budget'}<=set(e):raise ValueError('emitter fields')
            x=num(e,'x',0,639);y=num(e,'y',0,479);num(e,'w',1,640-x);num(e,'h',1,480-y)
            count=num(e,'count',1,64);num(e,'life_ms',100,4000);num(e,'speed',1,240);num(e,'spread',0,360);num(e,'gravity',-240,240);num(e,'color',0,65535);num(e,'budget',1,count)
            if e['shape'] not in ('dot','spark'):raise ValueError('shape')
            p=num(e,'page',0,4,4)
            if runtime is not None and p!=4 and p>=max(1,len(runtime.get('pages',[]))):raise ValueError('page')
            t=e['trigger']
            if not isinstance(t,dict) or set(t) not in ({'action'},{'tick'}):raise ValueError('trigger')
            if 'tick' in t:num(t,'tick',50,2000)
            else:
                a=t['action']
                if not isinstance(a,str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,47}',a) or (actions is not None and a not in actions):raise ValueError('trigger action')
            if 'when' in e:
                w=e['when']
                if s.get('bindings')!='p4-variable-bindings-v1' or not isinstance(w,dict) or set(w)!={'var','op','value'} or w['op'] not in PARTICLE_OPS:raise ValueError('when')
                if not isinstance(w['var'],str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,31}',w['var']):raise ValueError('when var')
                num(w,'value',-10**9,10**9)
                if runtime is not None and runtime.get('vars',{}).get(w['var'],{}).get('type')!='int':raise ValueError('when type')
        return []
    except (ValueError,KeyError,TypeError,AttributeError):return ['particles 需要 1-2 个有界装饰发射器：矩形内、count/budget ≤64、已声明动作或 tick 触发、整数变量守卫']

BINDINGS = {'title','eyebrow','headline','metricLabel','metricValue','metricUnit','badge','note','footer','progress'}
ICONS = {'home','leaf','desk','agent','compass','sun','mic','book','sound','spark','heart','pause','play','check','plus','list','arrow','cloud','clock','stretch'}

def graphics_errors(s, runtime=None):
    from service_bridge import surface_errors
    extension_errors=surface_errors(s,runtime)
    if extension_errors:return extension_errors
    """Graphics v1 is opt-in. Old media/image budgets are deliberately unchanged."""
    def num(o,k,lo,hi,default=None):
        v=o.get(k,default)
        if type(v) is not int or not lo<=v<=hi:raise ValueError(k)
        return v
    try:
        if set(s)-{'engine','graphics','background','images','elements','typography','fonts','interaction','game_view','data_view','scene_view','bindings','viewport','viewports','canvas','collection_view','shell','tabs','texts','particles','services'} or s['graphics']!='p4-app-graphics-v1' or s['engine']!=ENGINE:raise ValueError('root')
        limits=graphics_limits(runtime)
        if 'particles' in s and particle_errors(s,runtime):raise ValueError('particles')
        if 'collection_view' in s:
            from host_collection import config
            config(s['collection_view'],s,runtime)
        # p4-app-shell-v1 / p4-longtext-v1 / p4-data-rows-v1 surface extensions (capability-gated by the caller).
        shell=s.get('shell')=='p4-app-shell-v1'
        if 'shell' in s and (not shell or s.get('bindings')!='p4-variable-bindings-v1'):raise ValueError('shell')
        if 'tabs' in s:
            from app_shell import tabs_config
            if not shell:raise ValueError('tabs')
            tabs_config(s['tabs'],s,runtime)
        texts={}
        if 'texts' in s:
            from text_flow import valid_text
            if s.get('bindings')!='p4-variable-bindings-v1' or not isinstance(s['texts'],list) or not 1<=len(s['texts'])<=2:raise ValueError('texts')
            for x in s['texts']:
                if not isinstance(x,dict) or set(x)!={'id','text'} or not isinstance(x['id'],str) or not re.fullmatch(r'[a-z0-9_-]{1,15}',x['id']) or x['id'] in texts or not valid_text(x['text']):raise ValueError('text')
                texts[x['id']]=x['text']
        used_texts=set();expanded=len(s['tabs']['items']) if isinstance(s.get('tabs'),dict) and isinstance(s['tabs'].get('items'),list) else 0
        if 'bindings' in s and s['bindings']!='p4-variable-bindings-v1':raise ValueError('binding engine')
        if 'canvas' in s:
            from app_canvas import validate_canvas
            if s.get('bindings')!='p4-variable-bindings-v1':raise ValueError('canvas bindings')
            validate_canvas(s['canvas'],runtime)
        vp=s.get('viewport')
        multi=s.get('viewports');views={}
        if 'viewports' in s:
            if 'viewport' in s or not isinstance(multi,dict) or set(multi)!={'engine','items'} or multi['engine']!='p4-app-viewports-v1' or not isinstance(multi['items'],list) or not 1<=len(multi['items'])<=3:raise ValueError('viewports')
        declared=multi['items'] if multi is not None else [vp] if 'viewport' in s else []
        offsets=set()
        for v in declared:
            expected={'id','x','y','w','h','content_height','offset_var'} if multi is not None else {'engine','x','y','w','h','content_height','offset_var'}
            if not isinstance(v,dict) or set(v)!=expected or s.get('bindings')!='p4-variable-bindings-v1' or any(k in s for k in ('game_view','scene_view','data_view')):raise ValueError('viewport')
            if multi is None and v['engine']!='p4-app-viewport-v1':raise ValueError('viewport engine')
            vx=num(v,'x',0,639);vy=num(v,'y',0,479);num(v,'w',64,640-vx);vh=num(v,'h',64,480-vy);num(v,'content_height',vh,2048)
            if not isinstance(v['offset_var'],str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,31}',v['offset_var']) or v['offset_var'] in offsets:raise ValueError('viewport variable')
            offsets.add(v['offset_var'])
            if runtime is not None and (runtime.get('input',{}).get('engine')!='p4-input-v1' or runtime.get('vars',{}).get(v['offset_var'],{}).get('type')!='int' or any(k in runtime for k in ('scene','game','data','media','instrument'))):raise ValueError('viewport owner')
            if multi is not None:
                if not isinstance(v['id'],str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,15}',v['id']) or v['id'] in views:raise ValueError('viewport id')
                if any(vx<other['x']+other['w'] and vx+v['w']>other['x'] and vy<other['y']+other['h'] and vy+v['h']>other['y'] for other in views.values()):raise ValueError('overlapping viewports')
                views[v['id']]=v
        if 'scene_view' in s:
            v=s['scene_view']
            if not isinstance(v,dict) or set(v)!={'engine','x','y','w','h','colors','sprites'} or v['engine']!='p4-scene-surface-v1' or any(k in s for k in ('game_view','data_view')):raise ValueError('scene view')
            x=num(v,'x',0,639);y=num(v,'y',0,479)
            num(v,'w',64,640-x);num(v,'h',64,480-y)
            if not isinstance(v['colors'],list) or len(v['colors'])!=5 or any(type(c)is not int or not 0<=c<=65535 for c in v['colors']):raise ValueError('scene colors')
            if runtime is not None and ('scene' not in runtime or any(k in runtime for k in ('host','data','game','media','instrument')) or runtime['scene'].get('engine')=='p4-touch-slice-v1' or 'sprites' in runtime['scene'] or any('sprite' in e for e in runtime['scene'].get('entities',[]))):raise ValueError('scene owner')
            if runtime is not None and any(k in runtime['scene']['grid'] for k in ('rows','solid_tones')):raise ValueError('scene surface v1 requires open grid')
            if not isinstance(v['sprites'],list) or len(v['sprites'])>limits['sceneSprites']:raise ValueError('scene images')
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
            if not isinstance(v,dict) or set(v)!={'engine','x','y','w','h','layout','colors'} or v['engine']!='p4-data-surface-v1' or v['layout'] not in ('todos','metrics','rows') or 'game_view' in s:raise ValueError('data view')
            x=num(v,'x',0,639);y=num(v,'y',0,479)
            num(v,'w',320,640-x);num(v,'h',300,480-y)
            if not isinstance(v['colors'],list) or len(v['colors'])!=5 or any(type(c)is not int or not 0<=c<=65535 for c in v['colors']):raise ValueError('data colors')
            if runtime is not None and ('data' not in runtime or any(k in runtime for k in ('host','scene','game','media','instrument'))):raise ValueError('data owner')
        if 'game_view' in s:
            v=s['game_view']
            if not isinstance(v,dict) or set(v)!={'engine','x','y','w','h'} or v['engine']!='p4-game-surface-v1':raise ValueError('game view')
            x=num(v,'x',0,639);y=num(v,'y',0,479)
            num(v,'w',64,640-x);num(v,'h',64,480-y)
            if runtime is not None and (runtime.get('game',{}).get('type') not in ('snake','blocks','flappy','gomoku') or any(k in runtime for k in ('host','scene','data','media','instrument'))):raise ValueError('game owner')
        typo=s.get('typography')=='p4-app-type-v1'
        interactive=s.get('interaction')=='p4-app-interaction-v1'
        if 'interaction' in s and (not interactive or not typo):raise ValueError('interaction')
        if 'typography' in s and not typo:raise ValueError('typography')
        num(s,'background',0,65535)
        images=s.get('images',[]);ids={};pixels=0
        if not isinstance(images,list) or ('images' in s and not 1<=len(images)<=limits['images']):raise ValueError('images')
        for i,im in enumerate(images):
            if set(im)-{'id','asset','frame_width','frame_height','frames','fps','encoding'}:raise ValueError('image fields')
            if not isinstance(im['id'],str) or not re.fullmatch(r'[a-z0-9_-]{1,15}',im['id']) or im['id'] in ids:raise ValueError('id')
            if not isinstance(im['asset'],str) or not re.fullmatch(r'assets/[A-Za-z0-9_.-]+\.png',im['asset']) or '..' in im['asset']:raise ValueError('asset')
            jpeg=im.get('encoding')=='jpeg'
            if 'encoding' in im and not jpeg:raise ValueError('encoding')
            w=num(im,'frame_width',8,640 if jpeg else 512);h=num(im,'frame_height',8,480 if jpeg else 240)
            if jpeg and i==0 and limits['keyframes']>1:
                frames=num(im,'frames',1,limits['keyframes']);num(im,'fps',1,2 if frames>1 else 1)
                if frames>1 and len(im['id'])>12:raise ValueError('keyframe id')
            else:num(im,'frames',1,1);num(im,'fps',1,1)
            if jpeg and (i!=0 or (w,h)!=(640,480)):raise ValueError('background')
            if not jpeg:pixels+=w*h
            ids[im['id']]=jpeg
        if pixels>limits['imagePixels']:raise ValueError('image budget')
        if 'fonts' in s:
            if not typo or not isinstance(s['fonts'],list) or not 1<=len(s['fonts'])<=limits['fonts']:raise ValueError('fonts')
            seen=set();seen_images=set()
            for f in s['fonts']:
                if set(f)-{'image','chars','metrics','cell'} or not {'image','chars','metrics'}<=set(f) or f['image'] not in ids or ids[f['image']] or f['image'] in seen_images:raise ValueError('font image')
                seen_images.add(f['image']);im=next(im for im in images if im['id']==f['image'])
                w,h=im['frame_width'],im['frame_height'];chars=f['chars'];metrics=f['metrics']
                cell=f.get('cell',[32,32])
                if not isinstance(cell,list) or len(cell)!=2 or any(type(v) is not int for v in cell) or cell[0] not in (16,32,48,64) or cell[1] not in (32,48,64) or cell[0]>cell[1]:raise ValueError('font cell size')
                cw,ch=cell
                if w>limits['fontWidth'] or h>240 or w%cw or h%ch or not isinstance(chars,str) or not 1<=len(chars)<=limits['fontChars'] or len(chars)>(w//cw)*(h//ch) or not isinstance(metrics,list) or len(metrics)!=len(chars):raise ValueError('font cells')
                for c,m in zip(chars,metrics):
                    if c in seen or ord(c)<32 or 127<=ord(c)<160 or ord(c)>65535 or 0xD800<=ord(c)<=0xDFFF:raise ValueError('font character')
                    seen.add(c)
                    if not isinstance(m,list) or len(m)!=3 or any(type(v) is not int for v in m) or not 4<=m[0]<=32 or not 0<=m[1]<=m[2]<=32:raise ValueError('font metrics')
        nodes=s['elements']
        quota=runtime.get('quota') if runtime is not None else None
        node_cap=64 if quota=='p4-quota-v3' else 48 if quota=='p4-quota-v2' else 32
        if not isinstance(nodes,list) or not 1<=len(nodes)<=node_cap:raise ValueError('elements')
        allowed={'type','x','y','w','h','color','page','scale','radius','bind','text','image','action','motion','opacity','opacity_end','font_size','line_height','align','valign','padding','gap','icon_size','weight','foreground','stroke','stroke_width','icon','gradient','key_for','tile','show','state_mask','host_mask'}
        shared_page=12 if shell else 4
        actions={r.get('on') for r in runtime.get('transitions',[])} if runtime is not None else None
        for i,n in enumerate(nodes):
            if set(n)-(allowed|{'var_bind','in_view','view','field_bind','row_bind','text_id','page_var','count_var','caption','text_at','columns','cursor','image_bind','value_param','value_min','value_max'}) or n['type'] not in {'panel','text','meter','image','icon','button','longtext','figure','cells'}:raise ValueError('node')
            extension_keys={'row_bind','text_id','page_var','count_var','caption'}&set(n)
            if extension_keys and s.get('bindings')!='p4-variable-bindings-v1':raise ValueError('extension bindings')
            if (extension_keys-{'row_bind'}) and n['type'] not in ('longtext','figure'):raise ValueError('extension keys')
            inside='in_view' in n or 'view' in n
            view=views.get(n['view']) if isinstance(n.get('view'),str) else vp if 'view' not in n else None
            if inside and (not view or 'in_view' in n and (n['in_view'] is not True or 'view' in n) or 'motion' in n):raise ValueError('viewport node')
            nw,nh=(view['w'],view['content_height']) if inside else (640,480)
            t=n['type'];x=num(n,'x',0,nw-1);y=num(n,'y',0,nh-1);w=num(n,'w',1,nw-x);h=num(n,'h',1,min(480,nh-y));num(n,'color',0,65535)
            if 'state_mask' in n:
                mask=num(n,'state_mask',1,63)
                if runtime is not None and (runtime.get('input',{}).get('engine')!='p4-input-v1' or mask>>len(runtime.get('states',[]))):raise ValueError('state visibility')
            if 'host_mask' in n:
                num(n,'host_mask',1,255)
                if runtime is not None and (runtime.get('host') is not True or runtime.get('input',{}).get('engine')!='p4-input-v1'):raise ValueError('host visibility')
            num(n,'radius',0,min(24,w//2,h//2),0);num(n,'scale',1,3,1)
            p=num(n,'page',0,11 if shell else 4) if 'page' in n else shared_page
            if runtime is not None and p!=shared_page and p>=max(1,len(runtime.get('pages',[]))):raise ValueError('page')
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
            if 'var_bind' in n:
                name=n['var_bind']
                if s.get('bindings')!='p4-variable-bindings-v1' or t not in ('text','button','meter') or not isinstance(name,str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,31}',name) or any(k in n for k in ('bind','text','key_for')):raise ValueError('variable binding')
                if runtime is not None and (runtime.get('vars',{}).get(name,{}).get('type') not in ('int','string') or t=='meter' and runtime['vars'][name]['type']!='int'):raise ValueError('variable binding type')
            if 'text_at' in n:
                name=n['text_at']
                if s.get('bindings')!='p4-variable-bindings-v1' or t not in ('text','button') or not isinstance(name,str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,31}',name) or any(k in n for k in ('text','bind','var_bind','field_bind','key_for','row_bind')):raise ValueError('text_at')
                if runtime is not None:
                    from texts import declared
                    if not declared(runtime) or runtime.get('vars',{}).get(name,{}).get('type')!='int':raise ValueError('text_at reference')
            if 'field_bind' in n:
                if s.get('bindings')!='p4-variable-bindings-v1' or t not in ('text','button') or not isinstance(n['field_bind'],str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,15}',n['field_bind']) or any(k in n for k in ('text','bind','var_bind','key_for')):raise ValueError('field binding')
                if runtime is not None and n['field_bind'] not in [q.get('id') for q in runtime.get('form',{}).get('fields',[])]:raise ValueError('field reference')
            if t in ('text','button') and (sum(k in n for k in ('text','bind','var_bind','field_bind','row_bind','text_at'))!=1 or h<size):raise ValueError('content')
            if 'row_bind' in n:
                from data_rows import BIND_FIELDS
                if t not in ('text','button') or n['row_bind'] not in BIND_FIELDS or runtime is not None and (not isinstance(runtime.get('data'),dict) or 'data_view' not in s):raise ValueError('row binding')
            if t=='longtext':
                from text_flow import per_page
                if set(n)-{'type','x','y','w','h','color','page','text_id','page_var','count_var','font_size','line_height','align','weight','state_mask','host_mask','show'} or n.get('text_id') not in texts or n['text_id'] in used_texts:raise ValueError('longtext')
                used_texts.add(n['text_id']);lh=n.get('line_height',(size*5+3)//4)
                if h<lh or w<size or w>40*size or h//lh>24:raise ValueError('longtext box')
                refs=[n.get('page_var')]+([n['count_var']] if 'count_var' in n else [])
                if any(not isinstance(v,str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,31}',v) for v in refs) or len(set(refs))!=len(refs):raise ValueError('longtext vars')
                if runtime is not None and any(runtime.get('vars',{}).get(v,{}).get('type')!='int' for v in refs):raise ValueError('longtext var type')
                expanded+=per_page(h,lh)-1
            elif t=='figure':
                if set(n)-{'type','x','y','w','h','color','page','image','caption','font_size','line_height','align','weight','state_mask','host_mask','show','radius'} or n.get('image') not in ids or ids[n['image']]:raise ValueError('figure')
                cap=n.get('caption');lh=n.get('line_height',(size*5+3)//4)
                if not isinstance(cap,str) or len(cap.encode())>120 or any(ord(c)<32 or ord(c)==127 for c in cap) or h<lh+8:raise ValueError('caption')
                expanded+=1
            if t=='button' and (2*pad>=w or ('icon' in n and isz>h)):raise ValueError('button box')
            if t=='meter' and n.get('bind')!='progress' and 'var_bind' not in n:raise ValueError('meter')
            if 'action' in n and (not isinstance(n['action'],str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,47}',n['action']) or (actions is not None and n['action'] not in actions)):raise ValueError('action')
            if 'key_for' in n and (not typo or t!='button' or 'action' in n or 'bind' in n or not isinstance(n['key_for'],str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,47}',n['key_for']) or (actions is not None and n['key_for'] not in actions)):raise ValueError('key hint')
            if 'motion' in n and (n['motion']!='float' or t!='image' or y<4 or y+h>476):raise ValueError('motion')
            if t=='image':
                if n.get('image') not in ids:raise ValueError('image ref')
                if ids[n['image']] and (inside or i!=0 or (x,y,w,h,p)!=(0,0,640,480,shared_page) or any(k in n for k in ('action','motion','tile','show','state_mask','host_mask'))):raise ValueError('static background')
            elif t=='cells':
                if s.get('bindings')!='p4-variable-bindings-v1' or any(k in n for k in ('tile','action','motion','rotate_ms','text','bind','var_bind','text_at','field_bind','key_for')):raise ValueError('cells node')
                from cells import grid
                shape=grid(runtime) if runtime is not None else None
                if runtime is not None and (shape is None or w%shape[0] or h%shape[1] or w//shape[0]<4 or h//shape[1]<4):raise ValueError('cells geometry')
                if 'image' in n:
                    if n['image'] not in ids or ids[n['image']]:raise ValueError('cells image')
                    cols=num(n,'columns',2,(1<<shape[2]) if shape else 16)
                    im=next(im for im in images if im['id']==n['image'])
                    if im['frame_width']%cols or im['frame_width']//cols<4:raise ValueError('cells atlas')
                elif 'columns' in n:raise ValueError('cells columns')
                if 'cursor' in n:
                    cursor=n['cursor']
                    if not isinstance(cursor,dict) or set(cursor)!={'x_var','y_var','color'}:raise ValueError('cells cursor')
                    num(cursor,'color',0,65535)
                    for k in ('x_var','y_var'):
                        name=cursor[k]
                        if not isinstance(name,str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,31}',name) or runtime is not None and runtime.get('vars',{}).get(name,{}).get('type')!='int':raise ValueError('cells cursor var')
            elif 'image' in n and t!='figure':raise ValueError('image fields')
            if t!='cells' and any(k in n for k in ('columns','cursor')):raise ValueError('cells-only fields')
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
        if len(nodes)+expanded>node_cap:raise ValueError('expanded node budget')
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
        assert isinstance(p['vars'],list) and 1<=len(p['vars'])<=(32 if runtime.get('quota')=='p4-quota-v3' else 16 if runtime.get('quota')=='p4-quota-v2' else 8)
        assert all(isinstance(v,str) for v in p['vars']) and len(set(p['vars']))==len(p['vars'])
        assert set(p['vars'])<=set(runtime.get('vars',{}))
        text_fields={q.get('var') for q in runtime.get('form',{}).get('fields',[]) if q.get('kind')=='text'} if runtime.get('form',{}).get('engine') in ('p4-app-form-v2','p4-app-form-v3') else set()
        assert all(runtime['vars'][v].get('type')=='int' or ((runtime.get('quota')=='p4-quota-v3' or v in text_fields) and runtime['vars'][v].get('type')=='string') for v in p['vars'])
        assert isinstance(p['states'],dict) and set(p['states'])==set(runtime['states'])
        assert all(isinstance(v,str) and v in runtime['states'] for v in p['states'].values())
        from store_budget import errors as budget_errors
        return budget_errors(runtime)
    except (AssertionError,TypeError,KeyError,AttributeError):return ['persist 需要版本、唯一整数变量子集与完整的安全恢复状态映射']

def app_runtime_errors(runtime):
    from app_writers import derived_errors
    errors=persistence_errors(runtime)+derived_errors(runtime)
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
        for rule in [*runtime.get('transitions',[]),*[t['else'] for t in runtime.get('transitions',[]) if 'else' in t],*runtime.get('tick',[]),*[t['then'] for t in runtime.get('tick',[]) if 'then' in t]]:
            for op in ('set','inc'):
                assert isinstance(rule.get(op,{}),dict)
                for name,value in rule.get(op,{}).items():
                    assert name in variables
                    kind=variables[name]['type']
                    if isinstance(value,dict):
                        assert op=='set' or runtime.get('logic')=='p4-logic-v1'
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
        if runtime is not None and runtime.get('quota')!='p4-quota-v2' and len(nodes)>32: raise ValueError('elements')
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
