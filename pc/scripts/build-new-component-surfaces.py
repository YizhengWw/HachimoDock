"""Independent declarations for the three requested components, not cloned packages.
Only generated JSON, referenced PNGs and licenses enter the resource directories.
"""
import json
import shutil
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

PC=Path(__file__).resolve().parents[1]
ROOT=PC/'new-components'
def save(path,value):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
def rgb(s):
    n=int(s.lstrip('#'),16);return ((n>>19)<<11)|(((n>>10)&63)<<5)|((n>>3)&31)
INK=rgb('493D34');RED=rgb('AE4A37');IVORY=rgb('FFF9EF');MUTED=rgb('85756A')
def text(x,y,w,h,*,bind=None,label=None,size=20,color=INK,**kw):
    return dict(type='text',x=x,y=y,w=w,h=h,color=color,font_size=size,line_height=round(size*1.3),**({'bind':bind} if bind else {'text':label}))|kw
def button(x,y,w,h,action,*,bind=None,label=None,icon=None,primary=False,**kw):
    return dict(type='button',x=x,y=y,w=w,h=h,color=RED if primary else IVORY,foreground=IVORY if primary else INK,radius=16,font_size=20,padding=14,gap=8,icon_size=20,**({'action':action} if action else {}),**({'bind':bind} if bind else {'text':label}),**({'icon':icon} if icon else {}))|kw
def fonts(pack,runtime,labels):
    base=PC.parent/'firmware/managed_components/lvgl__lvgl/scripts/built_in_font'
    path=base/'SourceHanSansSC-Normal.otf'
    digits='0123456789'
    labels=''.join(dict.fromkeys(c for c in labels if c not in digits and c.isprintable()))[:49]
    specs=[]
    for name,chars,cw,ch,cols in [('digits',digits,32,64,5),('labels',labels,32,32,7)]:
        font=ImageFont.truetype(str(path),56 if name=='digits' else ch);rows=(len(chars)+cols-1)//cols
        atlas=Image.new('RGBA',(cols*cw,rows*ch));metrics=[]
        for i,c in enumerate(chars):
            cell=Image.new('L',(cw,ch));ImageDraw.Draw(cell).text((0,round(ch*.875)),c,font=font,fill=255,anchor='ls')
            bounds=cell.getbbox();metrics.append([max(4,min(32,round(font.getlength(c)*32/ch))),round(bounds[1]*32/ch) if bounds else 0,min(32,round(bounds[3]*32/ch)) if bounds else 0])
            rgba=Image.new('RGBA',cell.size,(255,255,255,0));rgba.putalpha(cell);atlas.paste(rgba,(i%cols*cw,i//cols*ch))
        atlas.save(pack/f'assets/{name}.png',optimize=True)
        runtime['surface']['images'].append(dict(id=name,asset=f'assets/{name}.png',frame_width=atlas.width,frame_height=atlas.height,frames=1,fps=1))
        specs.append(dict(image=name,chars=chars,metrics=metrics,cell=[cw,ch]))
    runtime['surface'].update(typography='p4-app-type-v1',fonts=specs)
    (pack/'licenses').mkdir(exist_ok=True)
    shutil.copyfile(base/'font_license/SourceHanSansSC/LICENSE.txt',pack/'licenses/type-OFL.txt')
def calendar():
    pack=ROOT/'daily-almanac'
    assert (pack/'assets/background.png').is_file(),'Prepare the native 640x480 image first'
    with Image.open(pack/'assets/background.png') as im:assert im.size==(640,480) and im.mode=='RGB'
    dashboard=dict(title='每日黄历',eyebrow='按北京时间 · PC 连接后可用',headline='今天，慢慢过',metricValue='—',metricLabel='回到今天',metricUnit='宜忌详情',badge='',note='连接 Pet Manager，读取本地黄历。',footer='民俗参考 · 节日不等于法定休假',visualStyle='clean',visualPalette='ember',visualLayout='tool',visualSprite='timer')
    actions={'host.today':'回到今天','host.details':'宜忌与返回','host.previous':'上一日或页','host.next':'下一日或页'}
    elements=[dict(type='image',x=0,y=0,w=640,h=480,image='background',color=65535),
        dict(type='icon',x=24,y=23,w=22,h=22,color=RED,icon='sun'),text(55,20,270,30,bind='title',size=20,weight=600),text(504,22,110,25,bind='badge',size=16,align='right'),
        text(27,73,470,32,bind='eyebrow',size=19),
        text(24,105,185,116,bind='metricValue',size=88,color=RED,host_mask=1),
        text(28,235,370,45,bind='headline',size=30,host_mask=1),
        text(28,289,368,48,bind='note',size=19,host_mask=1),
        text(28,344,370,30,bind='footer',size=17,color=MUTED,host_mask=1),
        dict(type='panel',x=20,y=108,w=600,h=268,color=IVORY,opacity=242,radius=20,host_mask=2),
        text(36,120,560,40,bind='headline',size=28,host_mask=2),
        text(36,172,568,90,bind='note',size=21,line_height=28,host_mask=2),
        text(36,270,568,90,bind='footer',size=21,line_height=28,host_mask=2),
        dict(type='panel',x=20,y=108,w=600,h=268,color=IVORY,opacity=242,radius=20,host_mask=128),
        text(36,135,560,48,bind='headline',size=28,host_mask=128),
        text(36,202,568,120,bind='note',size=21,line_height=28,host_mask=128),
        button(24,387,156,48,'host.today',bind='metricLabel',icon='clock',primary=True),
        button(190,387,156,48,'host.details',bind='metricUnit',icon='list'),
        button(478,387,62,48,'host.previous',label='‹'),
        button(550,387,62,48,'host.next',label='›'),
        dict(type='panel',x=0,y=448,w=640,h=32,color=IVORY,opacity=240),
        button(20,454,33,20,None,label='SW1',key_for='host.today',font_size=11,padding=0,radius=4),text(59,453,65,22,label='今天',size=13),
        button(125,454,33,20,None,label='SW2',key_for='host.details',font_size=11,padding=0,radius=4),text(164,453,128,22,label='详情 / 返回',size=13),
        text(295,453,180,22,label='左右切日 / 翻页',size=13),text(510,453,106,22,label='全局键退出',size=13,align='right')]
    runtime=dict(schema_version=1,engine='p4-bounded-runtime-v4',host=True,vars={},states=['idle'],initial_state='idle',transitions=[{'from':'*','on':a} for a in actions],tick=[],dashboard=dashboard,input={'engine':'p4-input-v1','swipe':{'left':'host.next','right':'host.previous'}},surface={'engine':'p4-app-surface-v1','graphics':'p4-app-graphics-v1','background':IVORY,'images':[dict(id='background',asset='assets/background.png',frame_width=640,frame_height=480,frames=1,fps=1,encoding='jpeg')],'elements':elements})
    fonts(pack,runtime,'每日黄历今天回到宜忌详情返回民俗参考年月星期一二三四五六日正冬腊初十廿卅闰—/·‹›')
    save(pack/'component.json',dict(id='daily-almanac',name='每日黄历',version='1.0.0',kind='tool',author='橘子酱',description='在设备上查看公历、农历、节气与节日，左右切日或展开传统宜忌。离线日期资料在 PC 资源包中，按北京时间更新；宜忌仅作民俗参考，不代表法定休假。'))
    save(pack/'runtime/widget.json',runtime);save(pack/'negative-screen.json',{'dashboard':dashboard})
    save(pack/'share.json',{'title':'每日黄历','author':'橘子酱','tags':['日历','生活'],'description':'2020–2035 离线黄历 · 标准 PetUI'})
    events=['button.sw1.short_press','button.sw2.short_press','knob.rotate_ccw','knob.rotate_cw']
    save(pack/'buttons.json',[dict(action=a,control='SW1' if i==0 else 'SW2' if i==1 else '前方摇杆',event=events[i],label=label) for i,(a,label) in enumerate(actions.items())])
def books(sample,catalog=None):
    pack=ROOT/'book-pocket'
    assert (pack/'assets/background.png').is_file(),'Prepare the native 640x480 image first'
    with Image.open(pack/'assets/background.png') as im:assert im.size==(640,480) and im.mode=='RGB'
    heading,body=Path(sample).read_text(encoding='utf-8').strip().split('\n',1)
    content=heading+' '+''.join(body.splitlines()).strip()
    han=sum('\u3400'<=c<='\u9fff' for c in content)
    assert 800<=han<=1500 and len(content.encode())<=6000
    green=rgb('547666')
    dashboard=dict(title='每天一本书',eyebrow='原创导读 · 单本试读版',headline='连接后，打开一本书',metricValue='试读内容 · 非完整 100 本库',metricLabel='换一本',metricUnit='',badge='',note='连接 Pet Manager 后阅读。进入设备组件会自动朗读，退出停止。',footer='音色跟随当前宠物；左右滑动或摇杆翻页。',visualStyle='clean',visualPalette='forest',visualLayout='tool',visualSprite='timer')
    actions={'host.random':'换一本 / 确认分类','host.category':'选择分类 / 返回','host.previous':'上一页或分类','host.next':'下一页或分类'}
    elements=[dict(type='image',x=0,y=0,w=640,h=480,image='background',color=65535),
        dict(type='icon',x=24,y=22,w=23,h=23,color=green,icon='book'),text(56,20,260,30,bind='title',size=20,weight=600),
        button(362,16,254,44,'host.category',bind='eyebrow',icon='list',font_size=16,padding=12,radius=14),
        text(27,75,500,42,bind='headline',size=30,weight=600),text(551,85,65,28,bind='badge',size=18,align='right'),
        text(28,121,450,26,bind='metricValue',size=16,color=MUTED),
        dict(type='panel',x=20,y=153,w=427,h=218,color=IVORY,opacity=224,radius=18),
        text(33,165,400,94,bind='note',size=20,line_height=28),
        text(33,269,400,94,bind='footer',size=20,line_height=28),
        button(24,387,174,48,'host.random',bind='metricLabel',icon='spark',primary=True,color=green),
        button(468,387,66,48,'host.previous',label='‹'),button(546,387,66,48,'host.next',label='›'),
        dict(type='panel',x=0,y=448,w=640,h=32,color=IVORY,opacity=240),
        button(20,454,33,20,None,label='SW1',key_for='host.random',font_size=11,padding=0,radius=4),text(59,453,65,22,label='换一本',size=13),
        button(135,454,33,20,None,label='SW2',key_for='host.category',font_size=11,padding=0,radius=4),text(174,453,100,22,label='分类 / 返回',size=13),
        text(296,453,172,22,label='左右翻页 / 选分类',size=13),text(510,453,106,22,label='全局键退出',size=13,align='right')]
    runtime=dict(schema_version=1,engine='p4-bounded-runtime-v4',host=True,vars={},states=['idle'],initial_state='idle',transitions=[{'from':'*','on':a} for a in actions],tick=[],dashboard=dashboard,input={'engine':'p4-input-v1','swipe':{'left':'host.next','right':'host.previous'}},surface={'engine':'p4-app-surface-v1','graphics':'p4-app-graphics-v1','background':IVORY,'images':[dict(id='background',asset='assets/background.png',frame_width=640,frame_height=480,frames=1,fps=1,encoding='jpeg')],'elements':elements})
    fonts(pack,runtime,'每天一本书系统之美全部书籍分类原创导读换确认返回页上下左右全局键退出—/·‹›')
    save(pack/'component.json',dict(id='book-pocket',name='每天一本书',version='0.1.0',kind='tool',author='橘子酱',description='单本试读版：通过原创导读理解一本商业认知书。设备内切换分类与翻页，进入自动朗读，退出停止；完整版 100 本内容尚未完成。'))
    save(pack/'runtime/widget.json',runtime);save(pack/'negative-screen.json',{'dashboard':dashboard})
    save(pack/'share.json',{'title':'每天一本书 · 试读版','author':'橘子酱','tags':['阅读','认知'],'description':'原创导读试读 · 标准 PetUI · 非完整书库'})
    events=['button.sw1.short_press','button.sw2.short_press','knob.rotate_ccw','knob.rotate_cw']
    save(pack/'buttons.json',[dict(action=a,control='SW1' if i==0 else 'SW2' if i==1 else '前方摇杆',event=events[i],label=label) for i,(a,label) in enumerate(actions.items())])
    cards=json.loads(Path(catalog).read_text()) if catalog else [dict(id='thinking-in-systems',title='系统之美',text=content,category='系统认知',source='Donella Meadows · 原创导读',url='https://www.penguinrandomhouse.com/books/801035/thinking-in-systems-by-donella-meadows/',license='Original commentary',revision='trial-reviewed-1')]
    save(pack/'pc/application.json',dict(schemaVersion=1,engine='pc-app-books-v1',service='books',autoRead=True,actions={a:a[5:] for a in actions},content=cards))
    (pack/'licenses/reading-notice.txt').write_text('本包为单本试读，非完整 100 本库。正文为原创阅读导引与评论，不是原书全文或作者授权摘要，不替代阅读原书。原书版权归各自权利人。场景与练习为原创假设，不冒称原书案例。\n书目信息：Thinking in Systems: A Primer，Donella H. Meadows，Diana Wright 编辑，2008 英文版，ISBN 9781603580557。\n公开参考：https://donellameadows.org/systems-thinking-resources/\n',encoding='utf-8')
    if catalog:
        assert len(cards)==100 and len({c['category'] for c in cards})==10
        dashboard.update(eyebrow='原创导读 · 100 本',metricValue='10 类主题 · 进入随机打开一本')
        save(pack/'runtime/widget.json',runtime);save(pack/'negative-screen.json',{'dashboard':dashboard})
        save(pack/'component.json',dict(id='book-pocket',name='每天一本书',version='1.0.2',kind='tool',author='橘子酱',description='100 本经济、商业与认知书籍的原创导读。进入随机打开一本，设备内选分类、换书与完整翻页；自动朗读跟随当前宠物音色，退出停止。'))
        save(pack/'share.json',{'title':'每天一本书','author':'橘子酱','tags':['阅读','认知'],'description':'100 本原创导读 · 10 类主题 · 标准 PetUI'})
        notice='本包包含 100 本书的原创阅读导引与评论，不是原书全文、逐章浓缩或作者授权摘要，不替代阅读原书。原书版权归各自权利人。示例为原创假设，不冒称原书案例。部分中文标题为导读展示译名，作品身份以原书名、作者和书目链接为准。内容经过模型辅助审校及针对性修订，不声称专业审定或核对过所有版本全文。财富主题为概念教育，不提供买卖、仓位或收益建议。\n'
        bib=Path(catalog).with_name('bibliography.json')
        if bib.is_file():
            notice+='\n书目索引：\n'+'\n'.join(f"{x['title']} | {x['originalTitle']} | {x['author']} | {x['url']}" for x in json.loads(bib.read_text()))
        notice+='\n系统之美 | Thinking in Systems: A Primer | Donella H. Meadows（Diana Wright 编辑） | https://donellameadows.org/systems-thinking-resources/\n'
        (pack/'licenses/reading-notice.txt').write_text(notice,encoding='utf-8')
if __name__=='__main__':
    import argparse
    args=argparse.ArgumentParser();args.add_argument('--book-sample');args.add_argument('--book-catalog');options=args.parse_args()
    calendar()
    if options.book_sample:books(options.book_sample,options.book_catalog)
