"""Upgrade the existing todo package in an isolated directory, never the user library."""
import argparse
import json
import shutil
from pathlib import Path
from app_font_assets import add_fonts

p = argparse.ArgumentParser()
p.add_argument('source', type=Path)
p.add_argument('destination', type=Path)
args = p.parse_args()
source, root = args.source.resolve(), args.destination.resolve()
assert not root.exists(), 'Use a new isolated output directory'
manifest = json.loads((source/'component.json').read_text())
assert manifest['id'] == 'upcoming-todos', 'Only upgrade the existing checklist'
shutil.copytree(source, root)
r = json.loads((root/'runtime/widget.json').read_text())
surface = r['surface']
surface.pop('data_view', None)
surface.pop('fonts', None)
surface['images'] = [im for im in surface['images'] if im['id'] == 'background']
ops = ['open','toggle','filter','previous','next','up','down','row_a','row_b','row_c','row_d','row_e']
app = dict(schemaVersion=1, engine='pc-app-services-v1', service='todos', actions={f'host.{op}':op for op in ops})
for key in ['data','persist']:
    r.pop(key, None)
r.update(host=True, vars={}, transitions=[{'from':'*','on':f'host.{op}'} for op in ops], tick=[])
r['input'] = {'engine':'p4-input-v1','swipe':{'left':'host.next','right':'host.previous'},
              'confirm':{'host.toggle':'确认切换这条待办的完成状态？可在详情中撤销。'}}
r['dashboard'] = dict(title='近期待办', headline='等待电脑连接', note='请保持 Pet Manager 运行',
                      metricValue='真实待办 · 本机保存', visualStyle='clean', visualPalette='mint', visualLayout='tool', visualSprite='target')
ink, muted, accent, paper, card = 16970, 31792, 13304, 63389, 65534
def node(kind,x,y,w,h,**kw):
    return dict(type=kind,x=x,y=y,w=w,h=h,color=ink,**kw)
def text(x,y,w,h,**kw):
    return node('text',x,y,w,h,font_size=18,**kw)
def button(x,y,w,h,action,**kw):
    return {**node('button',x,y,w,h,font_size=18,radius=14,foreground=ink,padding=12,action=f'host.{action}'), 'color':card, **kw}
e = [dict(type='image',x=0,y=0,w=640,h=480,color=paper,image='background'),
     {**node('icon',28,20,24,24,icon='list'),'color':accent},
     {**text(64,16,340,32,bind='title'), 'font_size':24,'weight':600},
     text(28,58,310,26,bind='metricValue'),
     {**text(350,58,78,26,bind='badge'),'align':'right','color':muted},
     button(448,48,164,44,'filter',text='切换筛选',icon='list',host_mask=39)]
for i,slot in enumerate(['eyebrow','headline','metricLabel','note','footer']):
    e.append(button(28,100+i*56,584,50,f'row_{chr(97+i)}',bind=slot,align='left',host_mask=1))
e += [dict(type='panel',x=28,y=100,w=584,h=276,color=card,radius=18,host_mask=34),
      {**text(48,112,544,28,bind='eyebrow',host_mask=34),'color':muted},
      {**text(48,156,544,90,bind='headline',host_mask=34),'font_size':22,'line_height':30,'valign':'top'},
      {**text(48,256,544,90,bind='note',host_mask=34),'font_size':22,'line_height':30,'valign':'top'},
      {**text(48,162,544,64,bind='headline',host_mask=132),'font_size':24,'align':'center'},
      {**text(48,240,544,90,bind='note',host_mask=132),'color':muted,'align':'center'},
      {**text(48,190,544,70,text='正在与电脑同步…',host_mask=64),'align':'center'},
      button(28,394,118,44,'previous',text='上一页'),
      button(154,394,118,44,'next',text='下一页'),
      button(286,394,148,44,'open',text='查看全文',host_mask=5,icon='book'),
      button(286,394,148,44,'open',text='返回清单',host_mask=34,icon='list'),
      {**button(448,394,164,44,'toggle',bind='metricUnit',host_mask=34,icon='check'),'color':accent,'foreground':65535},
      dict(type='panel',x=0,y=446,w=640,h=34,color=paper,opacity=245),
      dict(type='button',x=18,y=456,w=32,h=16,color=paper,foreground=muted,text='SW1',key_for='host.open',font_size=10,line_height=12,padding=1,radius=3,stroke=muted,stroke_width=1),
      {**text(56,452,98,22,text='查看 / 返回'),'font_size':12},
      dict(type='button',x=158,y=456,w=32,h=16,color=paper,foreground=muted,text='SW2',key_for='host.toggle',font_size=10,line_height=12,padding=1,radius=3,stroke=muted,stroke_width=1,host_mask=34),
      {**text(196,452,100,22,text='完成 / 撤销',host_mask=34),'font_size':12},
      dict(type='button',x=158,y=456,w=32,h=16,color=paper,foreground=muted,text='中键',key_for='host.filter',font_size=10,line_height=12,padding=1,radius=3,stroke=muted,stroke_width=1,host_mask=5),
      {**text(196,452,100,22,text='切换筛选',host_mask=5),'font_size':12},
      {**text(308,452,310,22,text='摇杆选项 / 翻页 · 全局键退出'),'font_size':12,'align':'right','color':muted}]
surface['elements'] = e
assert len(e)<=32
add_fonts(root,r,app)
manifest['version']='1.2.0'
manifest['description']='真实待办清单：在设备选中事项查看全文、确认完成或撤销完成；支持未完成与已完成筛选。需保持 PC 连接，本机统一保存。'
bindings=[]
for op,event,label in [('open','button.sw1.short_press','查看或返回'),('toggle','button.sw2.short_press','完成或撤销'),('filter','button.encoder.short_press','切换筛选'),('up','joystick.up','上一项'),('down','joystick.down','下一项'),('previous','knob.rotate_ccw','上一页'),('next','knob.rotate_cw','下一页')]:
    control='SW1' if op=='open' else 'SW2' if op=='toggle' else '前方摇杆'
    bindings.append(dict(action=f'host.{op}',control=control,event=event,label=label))
(root/'pc').mkdir(exist_ok=True)
for file,value in [('component.json',manifest),('runtime/widget.json',r),('buttons.json',bindings),('pc/application.json',app),('negative-screen.json',{'dashboard':r['dashboard']}),('share.json',{'title':manifest['name'],'description':manifest['description'],'summary':manifest['description']})]:
    (root/file).write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n')
print(json.dumps({'id':manifest['id'],'nodes':len(e),'runtimeBytes':len(json.dumps(r,ensure_ascii=False,separators=(',',':')).encode())}))
