"""Build the independent daily-news PetUI declaration, never clone another package.
Mechanism: real device entry -> bounded PC search/generation -> nine verified
items -> reading/pet-voice narration. SW1 refresh, SW2 stop/read; joystick
left/right selects an item, up/down reads overflow. Same actions on touch.
No microphone, no desktop task dispatch, no credentials in the package.
"""
import json
import shutil
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont

ROOT=Path(__file__).resolve().parents[1]/'builtin-clawpkgs/daily-news'
def save(name,value):
    path=ROOT/name;path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n')
def rgb(value):
    n=int(value,16);return ((n>>19)<<11)|(((n>>10)&63)<<5)|((n>>3)&31)
INK=rgb('263D3C');TEAL=rgb('355D59');PALE=rgb('FFF9ED');MUTED=rgb('7E796E')
def text(x,y,w,h,*,bind=None,label=None,size=20,color=INK,**kw):
    return dict(type='text',x=x,y=y,w=w,h=h,color=color,font_size=size,line_height=round(size*1.3),**({'bind':bind} if bind else {'text':label}),**kw)
def button(x,w,action,label=None,bind=None,primary=False):
    return dict(type='button',x=x,y=384,w=w,h=48,color=TEAL if primary else PALE,foreground=PALE if primary else INK,radius=14,padding=8,font_size=18,action=action,**({'bind':bind} if bind else {'text':label}))
dashboard=dict(title='每日新鲜事',eyebrow='AI 科技 · 商业财经 · 娱乐',headline='听听世界的新鲜事',note='进入设备组件后自动整理九条资讯；需连接 PC。',footer='核对来源与发布时间，不用旧闻凑数。',metricValue='',metricLabel='更新资讯',metricUnit='播报',badge='',visualStyle='clean',visualPalette='forest',visualLayout='tool',visualSprite='star')
ops=['refresh','toggle','previous','next','up','down']
elements=[dict(type='image',x=0,y=0,w=640,h=480,color=65535,image='background'),
    dict(type='icon',x=24,y=22,w=22,h=22,color=TEAL,icon='spark'),
    text(56,18,280,34,bind='title',size=22,weight=600),text(24,70,450,30,bind='eyebrow',size=18,color=MUTED),
    text(24,110,470,72,bind='headline',size=27,weight=600),
    dict(type='panel',x=16,y=190,w=608,h=182,color=PALE,opacity=220,radius=18),
    text(30,201,578,78,bind='note',size=22),text(30,281,578,78,bind='footer',size=22),
    text(528,348,82,24,bind='badge',size=14,color=MUTED,align='right'),
    button(20,144,'host.refresh',bind='metricLabel',primary=True),button(174,110,'host.toggle',bind='metricUnit'),
    button(300,66,'host.previous','上条'),button(374,66,'host.next','下条'),button(456,76,'host.up','上页'),button(540,76,'host.down','下页'),
    dict(type='panel',x=0,y=445,w=640,h=35,color=PALE,opacity=230),
    dict(type='button',x=20,y=451,w=33,h=22,color=PALE,foreground=MUTED,radius=4,padding=0,font_size=11,text='SW1',key_for='host.refresh'),text(59,451,65,22,label='刷新',size=14,color=MUTED),
    dict(type='button',x=125,y=451,w=33,h=22,color=PALE,foreground=MUTED,radius=4,padding=0,font_size=11,text='SW2',key_for='host.toggle'),text(164,451,110,22,label='播报 / 停止',size=14,color=MUTED),
    text(290,451,190,22,label='左右换条 · 上下翻页',size=14,color=MUTED),text(511,451,109,22,label='全局键退出',size=14,color=MUTED)]
runtime=dict(schema_version=1,engine='p4-bounded-runtime-v4',host=True,vars={},states=['ready'],initial_state='ready',transitions=[dict(on='host.'+op,**{'from':'*'}) for op in ops],tick=[],dashboard=dashboard,input=dict(engine='p4-input-v1',swipe=dict(left='host.next',right='host.previous',up='host.down',down='host.up')),surface=dict(engine='p4-app-surface-v1',graphics='p4-app-graphics-v1',elements=elements,images=[dict(id='background',asset='assets/background.png',frame_width=640,frame_height=480,frames=1,fps=1,encoding='jpeg')]))
fontroot=ROOT.parents[2]/'firmware/managed_components/lvgl__lvgl/scripts/built_in_font'
# ROOT is pc/builtin-clawpkgs/id; repository root is parents[2].
chars=''.join(dict.fromkeys('每日新鲜事听世界的更新资讯播报停止上下一条页刷新全局键退出'))
font=ImageFont.truetype(str(fontroot/'SourceHanSansSC-Normal.otf'),32)
atlas=Image.new('RGBA',(224,((len(chars)+6)//7)*32));metrics=[]
for i,c in enumerate(chars):
    cell=Image.new('L',(32,32));ImageDraw.Draw(cell).text((0,28),c,font=font,fill=255,anchor='ls')
    bounds=cell.getbbox();metrics.append([max(4,min(32,round(font.getlength(c)))),bounds[1] if bounds else 0,bounds[3] if bounds else 0])
    rgba=Image.new('RGBA',(32,32),(255,255,255,0));rgba.putalpha(cell);atlas.paste(rgba,((i%7)*32,(i//7)*32))
(ROOT/'assets').mkdir(parents=True,exist_ok=True);atlas.save(ROOT/'assets/labels.png',optimize=True)
(ROOT/'licenses').mkdir(exist_ok=True);shutil.copyfile(fontroot/'font_license/SourceHanSansSC/LICENSE.txt',ROOT/'licenses/type-OFL.txt')
runtime['surface']['images'].append(dict(id='labels',asset='assets/labels.png',frame_width=atlas.width,frame_height=atlas.height,frames=1,fps=1))
runtime['surface'].update(background=PALE,typography='p4-app-type-v1',fonts=[dict(image='labels',chars=chars,metrics=metrics)])
save('component.json',dict(id='daily-news',name='每日新鲜事',version='1.0.0',kind='tool',description='进入后由 PC 检索并整理过去24小时的九条资讯：AI科技、商业财经、娱乐各三条，用当前宠物的声音自然播报。可换条、翻页和停止，来源不完整时明确提示。'))
save('runtime/widget.json',runtime);save('negative-screen.json',dict(dashboard=dashboard));save('share.json',dict(title='每日新鲜事'))
save('buttons.json',[dict(action='host.'+op,event=event,control=control,label=label)for op,event,control,label in [
    ('refresh','button.sw1.short_press','SW1','更新资讯'),('toggle','button.sw2.short_press','SW2','播报或停止'),('previous','knob.rotate_ccw','前方摇杆','上一条资讯'),('next','knob.rotate_cw','前方摇杆','下一条资讯'),('up','joystick.up','前方摇杆','上一页正文'),('down','joystick.down','前方摇杆','下一页正文')]])
save('pc/application.json',dict(schemaVersion=1,engine='pc-app-news-v1',service='news',autoRead=True,actions={'host.'+op:op for op in ops},content=[dict(id='ai',title='AI 与科技',query='人工智能与科技新闻'),dict(id='business',title='商业与财经',query='商业与财经新闻'),dict(id='entertainment',title='娱乐新鲜事',query='娱乐行业影视音乐公开新闻')]))
