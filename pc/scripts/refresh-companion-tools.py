#!/usr/bin/env python3
"""Same-ID four-package visual upgrade. Work directory only.

Frog: move / pet / feed -> immediate entity + bond feedback, tick clears reaction.
Pong: start -> move paddle -> real collisions score -> miss ends -> restart.
Data: existing PC provider -> five-row page -> previous/next/home, honest empty/stale.
Layouts, palette, fonts, touch regions and artwork belong to each resource package.
"""
import argparse,json,shutil,sys
from pathlib import Path
from PIL import Image,ImageOps
from hachimi_layouts import box,text,label,panel,button,icon,rgb
from app_font_assets import add_fonts
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pc/skills/petui/scripts'))
from bounded_app import initial,dashboard
from validate_generated_widget import validate_widget
from smoke_test_widget_game import smoke_test_game

IDS=['bloomfrog_companion','two-key-pong','upcoming-todos','computer-status']
def write(root,name,value):
    (root/name).write_text(json.dumps(value,ensure_ascii=False,separators=(',',':'))+'\n')
def base(root,art,name,paper):
    with Image.open(art/(name+'.png')) as im:
        ImageOps.fit(im.convert('RGB'),(640,480),Image.Resampling.LANCZOS).save(root/'assets/background.png',optimize=True)
    return dict(engine='p4-app-surface-v1',graphics='p4-app-graphics-v1',background=rgb(paper),
        images=[dict(id='background',asset='assets/background.png',encoding='jpeg',frame_width=640,frame_height=480,frames=1,fps=1)],
        elements=[box('image',0,0,640,480,paper,image='background')])
def hint(ink,paper,first,second=None):
    out=[panel(0,446,640,34,paper,opacity=235)]
    for x,(caption,action,key) in zip([24,218],[first]+([second]if second else [])):
        out+=[box('button',x,455,36,17,paper,text=key,key_for=action,font_size=10,line_height=13,padding=2,radius=3,foreground=rgb(ink),opacity=0,stroke=rgb(ink),stroke_width=1),text(caption,x+43,451,146,24,13,ink)]
    out.append(text('全局键退出',504,451,112,24,13,ink,align='right'))
    return out
def frog(root,r,art):
    ink='#284f46';paper='#eff5e5';accent='#416d51'
    s=base(root,art,'frog',paper)
    r['scene'].pop('sprites',None)
    for e in r['scene']['entities']:e.pop('sprite',None)
    next(e for e in r['scene']['entities'] if e['id']=='pet').update(x=4,y=2,width=8,height=8)
    # Replace old four low-resolution frames with the original high-resolution
    # mascot. Movement/reaction rules are unchanged; no fake animation loop.
    with Image.open(art/'frog-character.png') as im:
        ImageOps.contain(im.convert('RGBA'),(192,192),Image.Resampling.LANCZOS).save(root/'assets/bloomfrog.png',optimize=True)
    with Image.open(root/'assets/bloomfrog.png') as im:w,h=im.size
    s['images'].append(dict(id='frog',asset='assets/bloomfrog.png',frame_width=w,frame_height=h,frames=1,fps=1))
    s['scene_view']=dict(engine='p4-scene-surface-v1',x=32,y=76,w=384,h=240,
        colors=[rgb(c)for c in [ink,'#54886a','#719f73','#d5ad69','#d78293']],
        sprites=[dict(entity='pet',image='frog',columns=1,fps=1)])
    r['dashboard'].update(eyebrow='小小绿意，慢慢相伴',visualStyle='clean')
    s['elements'] += [panel(0,0,640,64,paper,opacity=210),
        panel(442,89,174,244,'#f8fbef',radius=22,opacity=230),
        icon('leaf',26,22,ink),label('title',56,18,270,28,20,ink,weight=600),
        label('eyebrow',334,22,278,25,14,ink,align='right'),
        label('metricLabel',462,109,130,26,16,ink),
        label('metricValue',460,150,130,55,38,ink,weight=600),
        label('metricUnit',462,210,130,24,15,ink),
        panel(462,246,132,5,'#d6e1cc',radius=2),box('meter',462,246,132,5,accent,bind='progress'),
        label('headline',460,271,140,48,16,ink),
        button('上',100,326,52,paper,ink,action='move_up',padding=8),
        button('左',40,382,52,paper,ink,action='move_left',padding=8),
        button('下',100,382,52,paper,ink,action='move_down',padding=8),
        button('右',160,382,52,paper,ink,action='move_right',padding=8),
        button('摸摸',310,382,142,accent,'#ffffff','heart','pet_pet'),
        button('喂食',466,382,146,'#fcf9ed',ink,'leaf','feed_pet')]
    s['elements']+=hint(ink,paper,('抚摸','pet_pet','SW1'),('喂食','feed_pet','SW2'))
    r['surface']=s
    return '1.4.0','陪伴植物系蛙蛙，移动、抚摸和喂食都会获得即时反馈与亲密度变化。触屏与实体按键均可操作。',49
def pong(root,r,art):
    ink='#eef7f3';paper='#10252e';accent='#a5debd'
    s=base(root,art,'pong',paper)
    r['dashboard'].update(visualStyle='clean',visualPalette='ocean',metricUnit={'switch_state':{'ready':'开始接球','playing':'接球中','result':'再来一局'}},
        headline={'switch_state':{'ready':'准备接球','playing':'守住底线','result':'本局结束'}},footer='摇杆左右移动 · 主键开始/重开 · 全局键退出')
    s['scene_view']=dict(engine='p4-scene-surface-v1',x=28,y=88,w=348,h=272,
        colors=[rgb(c)for c in [ink,'#96cfc5',accent,'#f0d7a1','#f6e0a6']],sprites=[])
    s['elements'] += [panel(28,88,348,272,'#173f49',radius=20,opacity=185),
        panel(404,88,208,272,'#0b2029',radius=20,opacity=210),
        icon('spark',26,23,accent),label('title',57,18,320,30,20,ink,weight=600),
        text('一拍，一次专注。',378,22,234,25,14,'#a3bebc',align='right'),
        label('metricLabel',424,112,168,26,16,'#a3bebc'),
        label('metricValue',420,159,170,64,48,ink,weight=600),
        label('headline',424,255,168,30,20,accent),
        text('漏球后可重新开始',424,302,170,25,14,'#a3bebc'),
        button('向左',28,384,166,'#234d56',ink,action='shift_left'),
        button('向右',210,384,166,'#234d56',ink,action='shift_right'),
        button('metricUnit',404,384,208,accent,'#153a35','play','start',bind=True)]
    s['elements']+=hint('#b7ceca',paper,('开始/重开','start','SW1'))
    s['elements'].append(text('摇杆左右移动',218,451,260,24,13,'#b7ceca'))
    r['surface']=s
    return '1.2.0','移动球拍接住反弹小球，每次接中得分；漏过底线结束本局，可立即重新开始。支持触屏和摇杆移动。',91
def data(root,r,art,todos):
    paper='#f7f3e9'if todos else '#101d2e';ink='#434b50'if todos else '#edf2fc'
    muted='#7f8587'if todos else '#9eafc8';accent='#357cc1'if todos else '#9bc3f0'
    s=base(root,art,'todos'if todos else 'computer',paper)
    s['data_view']=dict(engine='p4-data-surface-v1',layout='todos'if todos else 'metrics',x=28,y=66,w=584,h=330,
        colors=[rgb(c)for c in [ink,muted,accent,'#428b66','#fffcf5'if todos else '#1e3048']])
    s['elements'] += [icon('list'if todos else 'desk',28,23,accent),
        label('title',60,18,320,30,22,ink,weight=600),
        text('把重要的事，放在眼前。'if todos else '电脑状态 · 实时同步',350,24,262,24,13,muted,align='right'),
        button('上一页',28,402,176,'#e8e8e1'if todos else '#273e58',ink,action='previous_page',h=40,padding=10),
        button('回首页',218,402,184,accent,'#ffffff'if todos else '#142638','home','first_page',h=40,padding=10),
        button('下一页',416,402,196,'#e8e8e1'if todos else '#273e58',ink,action='next_page',h=40,padding=10)]
    s['elements']+=hint(muted,paper,('回首页','first_page','SW1'))
    s['elements'].append(text('摇杆左右翻页',218,451,260,24,13,muted))
    r['surface']=s;r['dashboard'].update(visualStyle='clean',footer='主键回首页 · 摇杆左右翻页 · 全局键退出')
    return ('1.1.0','显示真实近期待办，蓝色为未完成、绿色为已完成；日期不显示年份，可在设备触屏或摇杆翻页。',91) if todos else ('1.1.0','显示电脑提供的真实运行状态，支持设备触屏或摇杆翻页；断连或数据失效时明确提示。',91)
def build(output,art):
    if output.exists() and any(output.iterdir()):raise ValueError('Use empty output directory')
    output.mkdir(parents=True,exist_ok=True);reports=[]
    for ident in IDS:
        root=output/ident;shutil.copytree(ROOT/'pc/builtin-clawpkgs'/ident,root)
        r=json.loads((root/'runtime/widget.json').read_text());r['engine']='p4-bounded-runtime-v4'
        version,desc,chars=frog(root,r,art)if ident==IDS[0]else pong(root,r,art)if ident==IDS[1]else data(root,r,art,ident==IDS[2])
        add_fonts(root,r,max_chars=chars)
        m=json.loads((root/'component.json').read_text());assert m['id']==ident;m.update(version=version,description=desc)
        write(root,'runtime/widget.json',r);write(root,'component.json',m)
        write(root,'negative-screen.json',{'dashboard':{k:v if isinstance(v,dict) else str(v) for k,v in dashboard(r,initial(r)).items()}})
        (root/'licenses/original-art.txt').write_text('Original AI-assisted artwork for HachimoDock, 2026. Text, buttons and live values are native PetUI elements; no third-party logos or UI screenshots.\n')
        errors=validate_widget(root);smoke=smoke_test_game(root)if not errors else None
        reports.append(dict(id=ident,errors=errors,smoke=smoke))
    print(json.dumps(reports,ensure_ascii=False,indent=2));return int(any(r['errors']or not r['smoke']['ok']for r in reports))
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--art',type=Path,default=ROOT/'pc/art-source/companion-tools-20261020');a=p.parse_args()
    raise SystemExit(build(a.output,a.art))
