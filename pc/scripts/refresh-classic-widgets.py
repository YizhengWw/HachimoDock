#!/usr/bin/env python3
"""Upgrade existing IDs in an isolated output tree; never edits the library.

Hydration loop: countdown -> due -> user acknowledgement -> next countdown.
Pause/resume does not increment records. Secondary page shows actual acknowledgements
in this run (there is no midnight-reset service). Touch reuses physical actions.
Native text/icons/buttons remain separate from the 640x480 background.
"""
import argparse
import json
import shutil
import sys
from pathlib import Path
from PIL import Image, ImageOps
from app_font_assets import add_fonts
from hachimi_layouts import box, text, label, panel, button, icon, footer, rgb

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pc/skills/petui/scripts'))
from bounded_app import initial,dashboard
from validate_generated_widget import validate_widget
from smoke_test_widget_game import smoke_test_game

def drink(root,art):
    r=json.loads((root/'runtime/widget.json').read_text())
    r['engine']='p4-bounded-runtime-v4'
    # "next" is the page-cycle opcode; avoid a page ID with the same spelling.
    r['pages']=[{'id':'countdown'},{'id':'records'}];r['initial_page']='countdown'
    d=r['dashboard']
    for v in d.values():
        if isinstance(v,dict) and 'switch_page' in v:
            old=v['switch_page'];v['switch_page']={'countdown':old.get('next',old.get('countdown')),'records':old.get('today',old.get('records'))}
    d.update(
        eyebrow={'switch_page':{'countdown':'给自己一个喝水的间隙','records':'每一次确认，都算数'}},
        headline={'switch_state':{'counting':'慢慢喝，不必等口渴。','paused':'先忙吧，提醒已暂停。','due':'休息一下，喝口水吧。'}},
        metricLabel={'switch_page':{'countdown':'距离下次提醒','records':'本次使用已记录'}},
        metricUnit={'switch_state':{'counting':'暂停提醒','paused':'继续提醒','due':'暂停提醒'}},
        note={'switch_page':{'countdown':'每隔 60 分钟提醒，可随时暂停。','records':'记录只在你确认后增加，不推算饮水量。'}},
        footer='主键记录  副键暂停/恢复  摇杆右切换  全局键退出',
        visualStyle='clean',visualPalette='ocean')
    ink='#244e58';paper='#f4faf7';accent='#377988'
    n=[box('image',0,0,640,480,paper,image='background'),
       panel(0,0,385,436,paper,opacity=210,opacity_end=40,gradient='horizontal'),
       icon('cloud',28,24,ink),label('title',59,20,260,28,18,ink,weight=600),
       button('看记录',480,16,132,paper,ink,'list','reminder.switch_view',h=44,opacity=230,padding=12,page=0),
       button('看倒计时',480,16,132,paper,ink,'clock','reminder.switch_view',h=44,opacity=230,padding=12,page=1),
       label('eyebrow',30,94,345,26,16,'#5d7e84'),
       label('headline',28,139,360,84,30,ink,weight=600,line_height=39),
       label('metricLabel',30,239,250,24,16,'#5d7e84'),
       label('metricValue',28,271,104,46,32,ink,weight=600),
       text('分钟',140,283,70,28,18,'#5d7e84',page=0),
       text('次',140,283,70,28,18,'#5d7e84',page=1),
       panel(30,330,192,5,'#d0dedc',radius=2),
       box('meter',30,330,192,5,accent,bind='progress',page=0),
       label('note',30,343,578,24,14,'#4b7077'),
       button('我喝了',28,378,156,accent,'#ffffff','check','reminder.acknowledge'),
       button('metricUnit',198,378,180,paper,ink,'clock','reminder.pause_resume',bind=True,opacity=240)]
    n+=footer('#4b7077',paper,'记录','暂停/恢复','摇杆右切换')
    for node in n:
        if node.get('text') in ('SW1','SW2'):
            node['key_for']='reminder.acknowledge' if node['text']=='SW1' else 'reminder.pause_resume'
            node['text']='—'
    r['surface']={'engine':'p4-app-surface-v1','graphics':'p4-app-graphics-v1','background':rgb(paper),
        'images':[{'id':'background','asset':'assets/background.png','frame_width':640,'frame_height':480,'frames':1,'fps':1,'encoding':'jpeg'}],
        'elements':n}
    with Image.open(art/'drink-reminder.png') as im:
        ImageOps.fit(im.convert('RGB'),(640,480),Image.Resampling.LANCZOS).save(root/'assets/background.png',optimize=True)
    add_fonts(root,r)
    manifest=json.loads((root/'component.json').read_text())
    manifest.update(version='1.2.0',description='每隔 60 分钟提醒喝水，可暂停或恢复；确认后重新计时，并显示本次使用的实际确认次数。支持触屏与实体按键，不推算饮水量。')
    preview={k:v if isinstance(v,dict) else str(v) for k,v in dashboard(r,initial(r)).items()}
    files={'component.json':manifest,'runtime/widget.json':r,
           'negative-screen.json':{'dashboard':preview}}
    for name,value in files.items():
        (root/name).write_text(json.dumps(value,ensure_ascii=False,separators=(',',':'))+'\n')
    (root/'licenses/original-art.txt').write_text('Original AI-assisted hydration illustration authored for HachimoDock, 2026. No third-party logos or UI screenshots. UI text and controls are native PetUI elements.\n')

def finish(root,r,art,ident,version,description):
    with Image.open(art/(ident+'.png')) as im:
        ImageOps.fit(im.convert('RGB'),(640,480),Image.Resampling.LANCZOS).save(root/'assets/background.png',optimize=True)
    add_fonts(root,r)
    manifest=json.loads((root/'component.json').read_text());manifest.update(version=version,description=description)
    preview={k:v if isinstance(v,dict) else str(v) for k,v in dashboard(r,initial(r)).items()}
    for name,data in {'component.json':manifest,'runtime/widget.json':r,'negative-screen.json':{'dashboard':preview}}.items():
        (root/name).write_text(json.dumps(data,ensure_ascii=False,separators=(',',':'))+'\n')
    (root/'licenses/original-art.txt').write_text('Original AI-assisted HachimoDock illustration, 2026. No third-party logos or UI screenshots. Controls and text are native PetUI, not baked into the image.\n')

def background(paper,n):
    return {'engine':'p4-app-surface-v1','graphics':'p4-app-graphics-v1','background':rgb(paper),
        'images':[{'id':'background','asset':'assets/background.png','frame_width':640,'frame_height':480,'frames':1,'fps':1,'encoding':'jpeg'}],
        'elements':[box('image',0,0,640,480,paper,image='background'),*n]}

def tomato(root,art):
    r=json.loads((root/'runtime/widget.json').read_text());r['engine']='p4-bounded-runtime-v4'
    # Skipping a focus period isn't completing it. Only the natural timer-end
    # transition increments the completion counter.
    for transition in r['transitions']:
        if transition['on']=='tomato.skip_phase':transition.pop('inc',None)
    d=r['dashboard'];d.update(visualStyle='clean',visualPalette='sunset',
        headline={'switch_state':{'focus':'把这一刻，留给专注。','focus_paused':'歇一歇，稍后继续。','rest':'做完一轮，放松一下。','rest_paused':'休息计时已暂停。'}},
        metricUnit={'switch_state':{'focus':'暂停专注','focus_paused':'继续专注','rest':'暂停休息','rest_paused':'继续休息'}},
        note='专注 25 分钟 · 休息 5 分钟',
        footer='主键暂停/继续  副键跳过  摇杆下重计时  全局键退出')
    ink='#663f30';paper='#fff6e7';accent='#b75d3e'
    n=[panel(0,0,396,436,paper,opacity=160,opacity_end=0,gradient='horizontal'),
       icon('clock',28,24,ink),label('title',59,20,230,28,18,ink,weight=600),
       text('完成轮次',470,22,88,24,15,ink),label('badge',558,19,52,30,20,ink,align='right'),
       label('eyebrow',30,94,340,25,16,'#a0745e'),
       label('headline',28,139,362,84,30,ink,weight=600,line_height=39),
       label('metricLabel',30,239,270,24,16,'#a0745e'),
       label('metricValue',28,271,280,46,32,ink,weight=600),
       panel(30,330,192,5,'#e5cdbb',radius=2),
       box('meter',30,330,192,5,accent,bind='progress'),
       label('note',30,343,578,24,14,'#906b56'),
       button('metricUnit',28,378,156,accent,'#fffaf1','clock','tomato.start_pause',bind=True),
       button('跳过本轮',198,378,156,paper,ink,'arrow','tomato.skip_phase',opacity=240),
       button('重计时',370,378,132,paper,ink,action='tomato.reset_phase',opacity=235)]
    n+=footer('#906b56',paper,'暂停/继续','跳过','下重计时')
    for node in n:
        if node.get('text') in ('SW1','SW2'):
            node['key_for']='tomato.start_pause' if node['text']=='SW1' else 'tomato.skip_phase';node['text']='—'
    r['surface']=background(paper,n)
    finish(root,r,art,'tomato-clock','1.3.0','专注 25 分钟后自动休息 5 分钟，可暂停、跳过或重新计时。只在完整专注结束时增加完成轮次；触屏与实体按键操作一致。')

def tokens(root,art):
    r=json.loads((root/'runtime/widget.json').read_text());r['engine']='p4-bounded-runtime-v4'
    r['dashboard'].update(visualStyle='clean',visualPalette='ocean',footer='主键总量  副键输入  摇杆右输出  全局键退出')
    ink='#edf0ff';paper='#101728';muted='#a2b1cf';accent='#bcb9f5'
    n=[icon('agent',28,24,ink),label('title',59,20,310,28,18,ink,weight=600),
       label('eyebrow',30,88,410,25,16,muted),
       label('headline',28,130,400,66,28,ink,line_height=35,weight=600),
       label('metricLabel',30,218,300,26,16,muted),
       label('metricValue',28,259,370,48,32,ink,weight=600),
       label('note',30,324,420,28,16,muted)]
    for page in range(3):
        for index,(caption,action,x,w) in enumerate([('总量','stats.show_total',28,124),('输入','stats.show_input',166,124),('输出','stats.show_output',304,124)]):
            selected=index==page
            n.append(button(caption,x,378,w,accent if selected else '#27334b','#17223b' if selected else ink,
                action=action,page=page,stroke=rgb('#465572'),stroke_width=0 if selected else 1))
    n+=footer(muted,paper,'总量','输入','摇杆右输出')
    for node in n:
        if node.get('text') in ('SW1','SW2'):
            node['key_for']='stats.show_total' if node['text']=='SW1' else 'stats.show_input';node['text']='—'
    r['surface']=background(paper,n)
    finish(root,r,art,'token-usage','2.3.0','展示当前 Agent 提供的真实 Token 统计，可切换总量、输入和输出；没有统计时显示等待，不生成模拟消耗。支持触屏与实体按键。')

def build(output,art,ids):
    if output.exists() and any(output.iterdir()):raise ValueError('Use an empty output directory')
    output.mkdir(parents=True,exist_ok=True)
    result=[]
    for ident in ids:
        root=output/ident
        shutil.copytree(ROOT/'pc/builtin-clawpkgs'/ident,root)
        {'drink-reminder':drink,'tomato-clock':tomato,'token-usage':tokens}[ident](root,art)
        errors=validate_widget(root)
        smoke=smoke_test_game(root) if not errors else None
        result.append({'id':ident,'errors':errors,'smoke':smoke})
    print(json.dumps(result,ensure_ascii=False,indent=2))
    return int(any(x['errors'] or not x['smoke']['ok'] for x in result))
if __name__=='__main__':
    p=argparse.ArgumentParser()
    p.add_argument('--output',type=Path,required=True);p.add_argument('--art',type=Path,required=True)
    p.add_argument('--only',nargs='+',choices=['drink-reminder','tomato-clock','token-usage'],default=['drink-reminder'])
    a=p.parse_args();raise SystemExit(build(a.output,a.art,a.only))
