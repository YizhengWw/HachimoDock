#!/usr/bin/env python3
"""Upgrade existing preset games, preserving identity, rules and controls."""
import argparse,json,shutil,sys
from pathlib import Path
from PIL import Image,ImageOps
from app_font_assets import add_fonts
from hachimi_layouts import box,panel,label,text,button,rgb
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pc/skills/petui/scripts'))
from bounded_app import initial,dashboard
from validate_generated_widget import validate_widget
from smoke_test_widget_game import smoke_test_game

def revise_flappy_speed(root):
    """Package owns the vertical speed; keep horizontal tick and artwork."""
    manifest=json.loads((root/'component.json').read_text())
    assert manifest['id']=='flappy-bird'
    r=json.loads((root/'runtime/widget.json').read_text())
    r['game']['vertical_speed_percent']=50
    manifest['version']='1.1.2'
    for name,data in [('runtime/widget.json',r),('component.json',manifest)]:
        (root/name).write_text(json.dumps(data,ensure_ascii=False,separators=(',',':'))+'\n')

def revise_block_controls(root):
    """Migrate only the existing block game's binding and generated hints."""
    assert json.loads((root/'component.json').read_text())['id']=='block-combo'
    r=json.loads((root/'runtime/widget.json').read_text())
    bindings=json.loads((root/'buttons.json').read_text())
    rotation=next(b for b in bindings if b['action']=='blocks.rotate')
    rotation.update(control='SW2',event='button.sw2.short_press')
    r['dashboard']['footer']={'switch_state':{
        'ready':'SW1开始 · SW2旋转 · 摇杆左右/下 · 全局键退出',
        'playing':'SW2旋转 · 摇杆左右移动/下落 · 全局键退出',
        'result':'SW1重开 · 全局键退出'}}
    nodes=r['surface']['elements']
    # Remove the old footer nodes, retain the footer panel and game controls.
    nodes[:]=[n for n in nodes if n['y']<447]
    paper='#24243e';ink='#f3efff'
    def key(action,title,x):
        return box('button',x,451,40,18,paper,text=title,key_for=action,font_size=11,
                   line_height=14,padding=2,radius=4,foreground=rgb(ink),
                   opacity=0,stroke=rgb(ink),stroke_width=1)
    nodes.extend([key('blocks.start','SW1',22),
                  text('开始 / 重开',70,447,112,26,14,ink),
                  key('blocks.rotate','SW2',196),
                  text('旋转',244,447,44,26,14,ink),
                  text('摇杆左右 / 下落',306,447,180,26,14,ink),
                  text('全局键退出',505,447,108,26,14,ink,align='right')])
    preview={k:v if isinstance(v,dict) else str(v) for k,v in dashboard(r,initial(r)).items()}
    for name,data in [('runtime/widget.json',r),('buttons.json',bindings),
                      ('negative-screen.json',{'dashboard':preview})]:
        (root/name).write_text(json.dumps(data,ensure_ascii=False,separators=(',',':'))+'\n')

def build(art,out):
    if out.exists() and any(out.iterdir()):raise ValueError('Output must be empty')
    specs=[('snake-turn','snake','#344f40','#f2f2df','#4a7956'),('block-combo','blocks','#f3efff','#24243e','#8077c7'),('flappy-bird','flappy','#3d5864','#f0f6ee','#b67945')]
    result=[]
    for ident,kind,ink,paper,accent in specs:
        root=out/ident;shutil.copytree(ROOT/'pc/builtin-clawpkgs'/ident,root)
        r=json.loads((root/'runtime/widget.json').read_text());manifest=json.loads((root/'component.json').read_text())
        r['engine']='p4-bounded-runtime-v4';r['dashboard'].update(visualStyle='clean',visualPalette='mono' if kind=='blocks' else 'mint' if kind=='snake' else 'ocean')
        # Original names and identities stay unchanged, including historical
        # names containing "像素". This is a visual revision, not a new game.
        start=r['game']['actions'].get('start',r['game']['actions'].get('flap'))
        r['dashboard']['headline']={'switch_state':{'ready':'准备好了吗','playing':'飞得轻一点' if kind=='flappy' else '保持节奏','result':'再试一次吧'}}
        r['dashboard']['metricUnit']={'switch_state':{'ready':'拍翅开始' if kind=='flappy' else '开始游戏','playing':'轻轻拍翅' if kind=='flappy' else '游戏进行中','result':'重新开始'}}
        view={'engine':'p4-game-surface-v1','x':36,'y':102,'w':432,'h':270}
        if kind=='blocks':view.update(x=96,y=74,w=220,h=352)
        sx=378 if kind=='blocks' else 492;sw=236 if kind=='blocks' else 122
        nodes=[box('image',0,0,640,480,paper,image='background'),
               panel(view['x']-4,view['y']-4,view['w']+8,view['h']+8,'#171a31' if kind=='blocks' else paper,radius=16,opacity=155 if kind=='blocks' else 60),
               panel(sx,86,sw,158,paper,radius=18,opacity=220)]
        nodes += [label('title',26,20,420,32,23,ink,weight=600),
                  label('metricLabel',sx+16,104,sw-32,26,15,ink),
                  label('metricValue',sx+16,138,sw-32,48,32,ink,weight=600),
                  label('headline',sx+12,199,sw-24,36,14,ink,align='center')]
        nodes += [button('metricUnit',sx,263,sw,accent,'#fffaf1',action=start,bind=True,padding=8,h=48,font_size=16)]
        actions=r['game']['actions']
        if kind=='snake':
            nodes += [button('左转',sx,324,sw,paper,ink,action=actions['left'],h=44,padding=8),button('右转',sx,376,sw,paper,ink,action=actions['right'],h=44,padding=8)]
        elif kind=='blocks':
            for title,key,x,y in [('左移','left',sx,324),('右移','right',sx+124,324),('旋转','rotate',sx,376),('落下','drop',sx+124,376)]:
                nodes.append(button(title,x,y,112,paper,ink,action=actions[key],h=44,padding=8))
        nodes += [panel(0,438,640,42,paper,opacity=235),box('button',22,451,40,18,paper,text='SW1',key_for=start,font_size=11,line_height=14,padding=2,radius=4,foreground=rgb(ink),opacity=0,stroke=rgb(ink),stroke_width=1),text('拍翅 / 重开' if kind=='flappy' else '开始 / 重开',70,447,160,26,14,ink),text('摇杆四向操作' if kind=='blocks' else '摇杆左右转向' if kind=='snake' else '也可点击拍翅按钮',244,447,240,26,14,ink),text('全局键退出',505,447,108,26,14,ink,align='right')]
        r['surface']={'engine':'p4-app-surface-v1','graphics':'p4-app-graphics-v1','background':rgb(paper),'game_view':view,'images':[{'id':'background','asset':'assets/background.png','encoding':'jpeg','frame_width':640,'frame_height':480,'frames':1,'fps':1}],'elements':nodes}
        (root/'assets').mkdir(exist_ok=True)
        with Image.open(art/(kind+'.png')) as image:ImageOps.fit(image.convert('RGB'),(640,480),Image.Resampling.LANCZOS).save(root/'assets/background.png',optimize=True)
        add_fonts(root,r,max_chars=91)
        version=[int(x) for x in manifest['version'].split('.')];version[1]+=1;version[2]=0;manifest['version']='.'.join(map(str,version))
        preview={k:v if isinstance(v,dict) else str(v) for k,v in dashboard(r,initial(r)).items()}
        for path,data in [('component.json',manifest),('runtime/widget.json',r),('negative-screen.json',{'dashboard':preview})]:
            (root/path).write_text(json.dumps(data,ensure_ascii=False,separators=(',',':'))+'\n')
        if kind=='blocks':revise_block_controls(root)
        errors=validate_widget(root);smoke=smoke_test_game(root) if not errors else {}
        result.append({'id':ident,'version':manifest['version'],'bytes':len(json.dumps(r,ensure_ascii=False,separators=(',',':')).encode()),'errors':errors,'smoke':smoke})
    print(json.dumps(result,ensure_ascii=False,indent=2))
    return int(any(x['errors'] or not x['smoke'].get('ok') for x in result))
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--art-dir',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();raise SystemExit(build(a.art_dir,a.output))
