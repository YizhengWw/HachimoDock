"""Data-only bounded drawing declaration. No IO or executable expressions."""
import re

ENGINE='p4-canvas-v1'
ENGINE_V2='p4-canvas-v2'
# canvas.md cost table; p4-canvas-v1 declarations keep the shipped numbers on every device.
BUDGETS={ENGINE:{'commands':16,'names':8,'spans':2048,'points':0},ENGINE_V2:{'commands':64,'names':16,'spans':8192,'points':64}}

def canvas_capability_accepts(engine,capability):
    """A v1 declaration runs on a v1 or v2 device; a v2 declaration needs widgetCanvas=p4-canvas-v2."""
    return capability==ENGINE_V2 if engine==ENGINE_V2 else capability in (ENGINE,ENGINE_V2)

def validate_canvas(c, runtime=None):
    def need(value):
        if not value:raise ValueError('invalid bounded canvas')
    def integer(v,lo,hi):return type(v) is int and lo<=v<=hi
    need(isinstance(c,dict) and {'engine','x','y','w','h','commands'}<=set(c)<= {'engine','x','y','w','h','commands','page'} and c['engine'] in BUDGETS)
    v2=c['engine']==ENGINE_V2;budget=BUDGETS[c['engine']]
    page=c.get('page',4);need(integer(page,0,4) and (runtime is None or page==4 or page<max(1,len(runtime.get('pages',[])))))
    need(integer(c['x'],0,639) and integer(c['y'],0,479) and integer(c['w'],1,320) and integer(c['h'],1,240))
    w,h=c['w'],c['h'];need(c['x']+w<=640 and c['y']+h<=480)
    need(isinstance(c['commands'],list) and 1<=len(c['commands'])<=budget['commands'])
    names=[];cost=0;points=0;radius=min(w,h)//2;side=max(w,h)
    def operand(v,lo,hi):
        if type(v) is int:need(lo<=v<=hi);return v
        need(isinstance(v,dict) and set(v)=={'var'} and isinstance(v['var'],str) and re.fullmatch(r'[A-Za-z0-9_.-]{1,31}',v['var']) is not None)
        name=v['var']
        if name not in names:names.append(name)
        need(len(names)<=budget['names'] and (runtime is None or runtime.get('vars',{}).get(name,{}).get('type')=='int'))
        return None
    for p in c['commands']:
        need(isinstance(p,dict));op=p.get('op')
        ops=('line','rect','circle')+(('polyline','arc','ring') if v2 else ())
        keys={'op','color'}|({'points'} if op=='polyline' else {'x','y'})|({'x2','y2'} if op=='line' else {'w','h'} if op=='rect' else {'r'} if op=='circle' else {'r','start','end'} if op=='arc' else {'r','inner'} if op=='ring' else set())
        need(op in ops and set(p)==keys and integer(p['color'],0,65535))
        if op=='polyline':
            pts=p['points'];need(isinstance(pts,list) and 3<=len(pts)<=16)
            points+=len(pts);need(points<=budget['points'])
            for pt in pts:
                need(isinstance(pt,list) and len(pt)==2);operand(pt[0],0,w-1);operand(pt[1],0,h-1)
            cost+=(len(pts)-1)*side
        else:
            operand(p['x'],0,w-1);operand(p['y'],0,h-1)
            if op=='line':operand(p['x2'],0,w-1);operand(p['y2'],0,h-1);cost+=side
            elif op=='rect':operand(p['w'],1,w);operand(p['h'],1,h);cost+=1
            elif op=='circle':
                r=operand(p['r'],1,radius);cost+=2*(radius if r is None else r)+1
            elif op=='arc':
                r=operand(p['r'],1,radius);operand(p['start'],0,359);operand(p['end'],0,359);cost+=6*(radius if r is None else r)+8
            else:
                r=operand(p['r'],1,radius);inner=operand(p['inner'],0,radius-1)
                need(r is None or inner is None or inner<r);cost+=4*(radius if r is None else r)+2
        need(cost<=budget['spans'])
    return names
