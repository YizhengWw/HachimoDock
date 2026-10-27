#!/usr/bin/env python3
"""Build the authored Hachimi packages. No installed/built-in package is used as a template.

Run with --art-dir (the approved original artwork) and an empty --output directory.
The scripts and original artwork are build inputs; only bounded JSON/PNG/license ship.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

SCRIPTS=Path(__file__).resolve().parents[1]/'skills/petui/scripts'
sys.path.insert(0,str(SCRIPTS))
from bounded_app import initial,dashboard
from validate_generated_widget import validate_widget
from smoke_test_widget_game import smoke_test_game
from hachimi_layouts import layout

def rgb(hex_value):
    n=int(hex_value.lstrip('#'),16);return ((n>>19)<<11)|(((n>>10)&63)<<5)|((n>>3)&31)
def integer(n):return {'type':'int','init':n}
def state_map(**values):return {'switch_state':values}
def page_map(**values):return {'switch_page':values}
def text(bind,x,y,w,h=32,scale=1,color='#463b30',**extra):
    return {'type':'text','x':x,'y':y,'w':w,'h':h,'color':rgb(color),'scale':scale,'bind':bind,**extra}
def literal(value,x,y,w,h=32,scale=1,color='#463b30',**extra):
    n=text('',x,y,w,h,scale,color,**extra);del n['bind'];n['text']=value;return n
def panel(x,y,w,h,color,radius=18,**extra):return {'type':'panel','x':x,'y':y,'w':w,'h':h,'color':rgb(color),'radius':radius,**extra}
def rule(on,source='*',target=None,**extra):
    return {'from':source,'on':on,**({'to':target} if target else {}),**extra}
def timer(state,target,**effects):return {'every_ms':1000,'while_state':state,'inc':{'left':-1},'when':{'var':'left','lte':0},'then':{'to':target,**effects}}
def buttons(entries):
    inputs=[('SW1','button.sw1.short_press'),('SW2','button.sw2.short_press'),('前方摇杆','knob.rotate_ccw'),('前方摇杆','knob.rotate_cw'),('前方摇杆','joystick.up'),('前方摇杆','joystick.down')]
    return [{'action':a,'label':label,'control':inputs[i][0],'event':inputs[i][1]} for i,(a,label) in enumerate(entries)]
def base(name,palette='sunset',ink='#463b30',paper='#fff7e8',accent='#995f41'):
    return {'schema_version':1,'engine':'p4-bounded-runtime-v4','vars':{},'states':['idle'],'initial_state':'idle','transitions':[],'tick':[],
            'dashboard':{'title':name,'visualStyle':'clean','visualPalette':palette,'visualLayout':'tool','visualSprite':'star'},
            'surface':{'engine':'p4-app-surface-v1','background':rgb(paper),'images':[{'id':f'b{i}','asset':f'assets/b{i}.png','frame_width':160,'frame_height':120,'frames':1,'fps':1} for i in range(4)],
                'elements':[
                  *[{'type':'image','x':i%2*320,'y':i//2*240,'w':320,'h':240,'color':0,'image':f'b{i}'} for i in range(4)],
                  panel(0,0,640,480,paper,0,opacity=255,opacity_end=0),
                  text('title',28,24,350,color=ink),text('eyebrow',28,88,450,color=ink),
                  text('headline',28,126,392,64,2,ink),text('metricValue',28,214,330,96,3,ink),
                  text('note',28,324,584,32,color=ink),
                  panel(28,378,180,48,accent,14,action='primary'),text('metricLabel',46,392,150,color=paper),
                  panel(220,378,180,48,paper,14,action='secondary',opacity=230),text('metricUnit',238,392,150,color=ink),
                  panel(0,444,640,36,paper,0,opacity=242),text('footer',20,448,600,color=ink)]}}
def labels(r,primary,secondary):
    r['dashboard'].update(metricLabel=primary,metricUnit=secondary)
def binding_node(r,bind):return next(n for n in r['surface']['elements'] if n.get('bind')==bind)
def save(r,variables,mapping=None,page=False):
    r['persist']={'version':1,'vars':variables,'states':mapping or {s:s for s in r['states']}}
    if page:r['persist']['page']=True
def pages(r,ids):r['pages']=[{'id':p} for p in ids];r['initial_page']=ids[0]

def home():
    r=base('哈基米小屋');r['vars']={'pats':integer(0),'left':integer(0),'plant':integer(0),'object':integer(0)}
    r['states']=['idle','glad','plant','object','place'];pages(r,['oak','mint','night'])
    # First matching rule wins. Selection and replacement are separate actions;
    # each independent furnishing is persisted, not a background tint.
    r['transitions']=[rule('primary','plant',inc={'plant':1}),rule('primary','object',inc={'object':1}),rule('primary','place',page='next'),rule('primary',target='glad',inc={'pats':1},set={'left':3}),rule('secondary','plant','object'),rule('secondary','object','place'),rule('secondary','place','idle'),rule('secondary',target='plant'),rule('previous',target='plant'),rule('next',target='object')]
    r['tick']=[timer('glad','idle')];r['limits']={'pats':[0,1000000000],'left':[0,3],'plant':[0,1000000000],'object':[0,1000000000]}
    r['dashboard'].update(eyebrow=page_map(oak='窗边相伴',mint='茶桌小景',night='左右相望'),headline=state_map(idle='今天，也一起慢慢来。',glad='摸摸收到啦，喜欢你。',plant='选一株喜欢的绿植。',object='给小屋添一件好物。',place='把喜欢的摆在身边。'),metricValue={'var':'pats'},note=state_map(idle='摸摸头，留下今天的一次陪伴。',glad='今天的陪伴，已经记下来啦。',plant='主键换绿植，副键继续选摆件。',object='主键换摆件，副键调整摆放。',place='主键换位置，副键完成布置。'),footer='主键互动/更换  副键布置/完成  全局键退出')
    labels(r,state_map(idle='摸摸头',glad='再摸摸',plant='换一株',object='换一件',place='换个位置'),state_map(idle='布置小屋',glad='布置小屋',plant='选摆件',object='调位置',place='完成布置'))
    save(r,['pats','plant','object'],{s:'idle' for s in r['states']},page=True)
    return r,buttons([('primary','互动或更换'),('secondary','布置或完成'),('previous','选择绿植'),('next','选择摆件')]),'摸摸头获得回应，独立选择三种绿植、三种摆件及三种摆放组合；选择和陪伴次数在本机保留，无饥饿惩罚。'

def garden():
    r=base('专注花园','forest','#244934','#edf3e4','#3e7354');r['vars']={'duration':integer(1500),'left':integer(1500),'flowers':integer(0),'elapsed':integer(0),'bloom':integer(2)};r['states']=['ready','run','paused','done'];r['initial_state']='ready';pages(r,['focus','garden'])
    r['limits']={'duration':[300,3600],'left':[0,3600],'flowers':[0,1000000000]}
    r['transitions']=[rule('primary','ready','run',set={'left':{'var':'duration'}}),rule('primary','run','paused'),rule('primary','paused','run'),rule('primary','done','run',set={'left':{'var':'duration'}}),rule('secondary',page='next'),rule('less','ready',inc={'duration':-300}),rule('more','ready',inc={'duration':300}),rule('less','done','ready',inc={'duration':-300}),rule('more','done','ready',inc={'duration':300})]
    r['tick']=[timer('run','done',inc={'flowers':1})]
    r['tick'][0]['inc']['elapsed']=1
    r['limits'].update(elapsed=[0,3600],bloom=[2,2])
    for t in r['transitions']:
        if t.get('from') in ('ready','done') and t['on']=='primary':t['set']['elapsed']=0;t['page']='focus'
        if t.get('from')=='done' and t['on'] in ('less','more'):t['set']={'elapsed':0}
    r['dashboard'].update(eyebrow=page_map(focus='只做眼前这一件事',garden='花园 · 完成一次长一株'),headline=state_map(ready='准备专注',run='慢慢生长',paused='休息一下',done='又长出一株'),metricValue=state_map(ready={'fmt_mmss':'duration'},run={'fmt_mmss':'left'},paused={'fmt_mmss':'left'},done='完成'),badge={'var':'flowers'},note='左右调节 5–60 分钟；完成才计入花园。',footer='SW1 开始/暂停  SW2 花园  左右调时  全局键退出')
    binding_node(r,'metricValue')['page']=0
    r['surface']['elements'].append(text('badge',34,214,220,96,3,'#244934',page=1))
    labels(r,state_map(ready='开始专注',run='暂停',paused='继续',done='再种一株'),page_map(focus='我的花园',garden='返回专注'))
    save(r,['duration','left','flowers','elapsed'],{'ready':'ready','run':'paused','paused':'paused','done':'done'})
    return r,buttons([('primary','开始或暂停'),('secondary','查看花园'),('less','减少五分钟'),('more','增加五分钟')]),'用 5–60 分钟专注养一株花，可暂停继续；倒计时完成后只结算一次，花园成果在本机保留。'

def desk():
    r=base('工位搭子','sunset','#563321','#fff2de','#b95d31');r['vars']={'left':integer(0),'rounds':integer(0),'duration':integer(1500)};r['states']=['idle','focus','meeting','away','rest','done']
    durations={'focus':1500,'meeting':1800,'away':600,'rest':300};cycle=['idle','focus','meeting','away','rest','done'];r['limits']={'left':[0,1800],'rounds':[0,1000000000]}
    r['transitions']=[rule('primary',s,'focus' if s in ('idle','done') else cycle[cycle.index(s)+1] if s!='rest' else 'idle',set={'left':durations.get('focus' if s in ('idle','done') else cycle[cycle.index(s)+1] if s!='rest' else 'idle',0)}) for s in cycle]
    for t in r['transitions']:t['set']['duration']=max(300,t['set']['left'])
    r['limits'].update(left=[0,7200],duration=[300,7200])
    r['transitions'] += [rule('secondary','idle','focus',set={'left':{'var':'duration'}}),rule('secondary','done','focus',set={'left':{'var':'duration'}}),rule('secondary',set={'left':{'var':'duration'}}),rule('less',inc={'left':-300,'duration':-300}),rule('more',inc={'left':300,'duration':300})]
    r['tick']=[timer(s,'done',inc={'rounds':1}) for s in durations]
    r['dashboard'].update(eyebrow='手动状态 · 不读取日历或窗口',headline=state_map(idle='随时找我',focus='专注中',meeting='会议中',away='暂时离开',rest='休息一下',done='时间到了'),metricValue={'fmt_mmss':'left'},note='状态由你决定；到点只在屏幕温柔提示。',footer='SW1 切换状态  SW2 重计时/确认  全局键退出')
    labels(r,'切换工位状态','重计时 / 确认');save(r,['rounds','duration'],{s:'idle' for s in r['states']})
    return r,buttons([('primary','切换工位状态'),('secondary','重计时或确认'),('less','减少五分钟'),('more','增加五分钟')]),'手动展示专注、会议、离开或休息状态，左右按五分钟调节当前计时，完成后可重新开始；不读取你的日历和电脑窗口。'

def stretch():
    r=base('活力间歇','candy','#4d3f63','#f3edf9','#7e619c');r['vars']={'left':integer(30),'rounds':integer(0)};r['states']=['idle','run','paused','done','later'];pages(r,['shoulders','eyes','hands']);r['limits']={'left':[0,300],'rounds':[0,1000000000]}
    r['transitions']=[rule('primary','idle','run',set={'left':30}),rule('primary','run','paused'),rule('primary','paused','run'),rule('primary','done','run',set={'left':30}),rule('primary','later','run',set={'left':30}),rule('secondary',target='later',set={'left':300}),rule('previous',page='prev'),rule('next',page='next')]
    r['tick']=[timer('run','done',inc={'rounds':1}),timer('later','idle',set={'left':30})]
    r['dashboard'].update(eyebrow=state_map(idle='给自己半分钟',run='按舒服的节奏',paused='已暂停',done='轻休息完成',later='五分钟后再提醒'),headline=page_map(shoulders='放松肩膀',eyes='看看远处',hands='轻松握放'),metricValue={'fmt_mmss':'left'},note=page_map(shoulders='轻轻抬肩，再慢慢放下；不适就停止。',eyes='目光离开屏幕，看看远处；自然眨眼。',hands='双手轻轻张开、握拢；不适就停止。'),footer='SW1 开始/暂停  SW2 稍后  左右换动作  全局键退出')
    labels(r,state_map(idle='开始休息',run='暂停',paused='继续',done='再来一次',later='现在开始'),'稍后五分钟');save(r,['rounds'],{s:'idle' for s in r['states']})
    return r,buttons([('primary','开始或暂停'),('secondary','五分钟后提醒'),('previous','上一个动作'),('next','下一个动作')]),'跟随三种简短提示做 30 秒轻休息，可暂停或延后五分钟；以自身舒适为准，不作健康诊断。'

def hosted(name,service,actions,palette,ink,paper,accent,headline,note,description,content=None,reading=False):
    r=base(name,palette,ink,paper,accent);r['host']=True
    r['transitions']=[rule('host.'+a) for a,op,label in actions]
    for n in r['surface']['elements']:
        if 'action' in n:n['action']='host.'+n['action']
    r['dashboard'].update(eyebrow='PC 通用服务 · 连接后即可使用',headline=headline,metricValue='',note=note,footer='默认：主键操作  副键管理  全局键退出')
    labels(r,actions[0][2],actions[1][2])
    if reading:
        binding_node(r,'headline').update(y=116,h=64,w=570)
        binding_node(r,'note').update(y=224,w=570,h=128)
        r['surface']['elements'].remove(binding_node(r,'metricValue'))
        r['surface']['elements'][4].update(opacity_end=180)
    config={'schemaVersion':1,'engine':'pc-app-services-v1','service':service,'actions':{'host.'+a:op for a,op,label in actions}}
    if content:config['content']=content
    return r,buttons([('host.'+a,label) for a,op,label in actions]),description,config

def agent():
    return hosted('桌面任务台','desktop-tasks',[('primary','voice','语音补充'),('secondary','details','查看进展'),('previous','previous','上一项任务'),('next','next','下一项任务')],'ocean','#2b4456','#edf3f6','#517c98','等一个新任务','只展示当前设备跟随 Agent 的真实回执。','查看当前跟随 Agent 的真实任务状态，可开启实时对话补充要求；不停止旧任务，不自动确认敏感操作。',reading=True)
def travel():
    cards=[{'id':'coast','title':'风铃海岸','text':'海风绕过风铃，小西捡到一枚圆圆的贝壳。明信片上写着：慢一点也很好。','minutes':15},{'id':'forest','title':'苔藓森林','text':'阳光落在苔藓小路上。小西停下来听树叶沙沙响，把绿色的一天寄给你。','minutes':30},{'id':'hill','title':'云朵山丘','text':'小西登上软绵绵的云朵山丘，看见远处屋顶。明信片上画着一条回家的路。','minutes':45},{'id':'night','title':'星光小镇','text':'晚风亮起街灯，小西在小镇邮局盖下星星邮戳。下一次旅行，也要记得回家。','minutes':60}]
    result=hosted('宠物旅行','journey',[('primary','depart','出发或确认下一站'),('secondary','collection','明信片或取消选地'),('previous','previous','上个目的地'),('next','next','下个目的地')],'mint','#46563c','#f4f0da','#7b8758','在设备上选一站','左右挑选目的地，确认后出发；也可以先看看明信片。','目的地直接在设备通过触屏或摇杆选择，确认后开始计时；旅途中可预选下一站，不中断当前旅程。需 PC 保持运行，到期收获原创明信片。',cards)
    labels(result[0],'选目的地','明信片')
    return result
def weather():
    result=hosted('今日气象','weather',[('primary','refresh','更新或确认'),('secondary','todo','待办或取消'),('previous','previous','上个城市'),('next','next','下个城市'),('choose','choose_city','选择城市'),('details','details','预报说明')],'ocean','#264b60','#edf5f5','#4d8999','连接后获取城市天气','点击右上角选城；自动估算仅在未手动选择时使用。','城市始终显示，点击地点进入选城，左右浏览后确认保存，取消保留原地点；支持触屏与摇杆。需 PC 连接提供真实预报，用户城市不随资源包分发。')
    result[0]['dashboard']['eyebrow']='选择城市'
    labels(result[0],'更新预报','近期待办')
    result[3]['engine']='pc-app-services-v2'
    result[3]['location']=json.loads((Path(__file__).resolve().parent.parent/'component-profiles/weather.json').read_text())
    return result
def notes():
    return hosted('灵感胶囊','notes',[('primary','record','开始录制'),('secondary','save','保存笔记'),('previous','previous','上一条笔记'),('next','next','下一条笔记'),('todo','todo','转为待办'),('delete','delete','删除确认')],'candy','#59415f','#faf0f7','#9f648f','接住一个想法','录音转写后，由你确认保存或转为待办。','使用设备麦克风记录想法，云端识别后需确认才保存；笔记仅存本机，可转为统一待办，删除需再次确认。',reading=True)
def learn():
    import re
    from collections import Counter
    cards=json.loads((Path(__file__).resolve().parents[1]/'content/knowledge/catalog.json').read_text())
    expected={'物理','化学','生物','历史','人文','经济','心理','健康','天文','地理'}
    counts=Counter(c.get('category') for c in cards)
    if set(counts)!=expected or any(n!=100 for n in counts.values()) or len({c['id'] for c in cards})!=1000:
        raise ValueError('每日新知需要十类各100条独立知识，不允许使用缺失或重复的知识库')
    if any(len(re.findall(r'[\u3400-\u9fff]',c['text']))<300 for c in cards):
        raise ValueError('知识解释不足300个汉字')
    result=hosted('每日新知','knowledge',[('primary','random','换一条或确认分类'),('secondary','category','选择分类或返回'),('previous','previous','上一页或上一分类'),('next','next','下一页或下一分类')],'mono','#f0eeff','#1d1e36','#d6d1fb','每天多一点好奇','连接 PC 后阅读完整知识库。','十个分类各100条知识，每条不少于300个汉字。进入设备组件跟随当前宠物音色朗读，换条后读新内容；退出或开启实时对话即停止。保留分类与换条入口，摇杆左右翻页。需新版 PC 客户端连接并配置语音服务。',cards,reading=True)
    result[3]['engine']='pc-app-services-v3'
    result[3]['autoRead']=True
    result[0]['dashboard'].update(eyebrow='选择分类',footer='触屏或摇杆左右翻页，全文不会截断。',metricValue='1000 条 · 百科节选',badge='— / —')
    labels(result[0],'换一条','读本页')
    return result
def sounds():
    return hosted('自然声景','soundscape',[('primary','toggle','开始播放'),('secondary','timer','十五分钟'),('previous','previous','上种声景'),('next','next','下种声景'),('up','up','提高音量'),('down','down','降低音量')],'forest','#334c41','#edf3e9','#567e69','窗边细雨','原创合成声景，退出或开启语音即停止。','四种原创合成自然声景，通过 PC 与 USB 播放；支持音量与 15/30/60 分钟渐止，退出或开始语音即停止。')

BUILDERS=[('hachimi-home','01-home',home),('hachimi-garden','02-garden',garden),('hachimi-travel','05-travel',travel),('hachimi-weather','06-weather',weather),('hachimi-notes','07-capsule',notes),('hachimi-learn','08-learn',learn),('hachimi-sounds','09-sound',sounds),('hachimi-break','10-break',stretch)]
def build(art,output,only=None,version='1.0.0'):
    from PIL import Image
    from app_font_assets import add_fonts
    if output.exists() and any(output.iterdir()):raise ValueError('输出目录必须为空，避免覆盖已有组件')
    report=[]
    for ident,source,factory in BUILDERS:
        if only and ident not in only:continue
        result=factory();runtime,bindings,description=result[:3];application=result[3] if len(result)>3 else None
        runtime['surface']=layout(ident,runtime)
        root=output/ident;(root/'assets').mkdir(parents=True);(root/'runtime').mkdir()
        # Mechanical production-size conversion. UI text is rendered natively, never baked in.
        # Asset compilation only: one deterministic resize, then exact pixel tiles.
        # No generative changes, retouching, or embedded UI text.
        with Image.open(art/(source+'.png')) as original:
            # Match the concept's object-fit:cover without stretching its art.
            from PIL import ImageOps
            full=ImageOps.fit(original.convert('RGB'),(640,480),Image.Resampling.LANCZOS)
            full.save(root/'assets/background.png',optimize=True)
        if ident in ('hachimi-home','hachimi-garden'):
            # Mechanical atlas packing: preserve the generated transparent art.
            with Image.open(art/'decor-atlas.png') as atlas:
                atlas.convert('RGBA').resize((192,192),Image.Resampling.LANCZOS).save(root/'assets/decor.png',optimize=True)
            runtime['surface']['images'].append({'id':'decor','asset':'assets/decor.png','frame_width':192,'frame_height':192,'frames':1,'fps':1})
        add_fonts(root,runtime,application,max_chars=49 if ident in ('hachimi-home','hachimi-garden') else 91)
        manifest={'id':ident,'name':runtime['dashboard']['title'],'version':version,'kind':'tool','description':description}
        preview={k:v if isinstance(v,dict) else str(v) for k,v in dashboard(runtime,initial(runtime)).items()}
        files={'component.json':manifest,'runtime/widget.json':runtime,'buttons.json':bindings,'negative-screen.json':{'dashboard':preview},'share.json':{'title':manifest['name']}}
        if application:(root/'pc').mkdir();files['pc/application.json']=application
        for name,data in files.items():(root/name).write_text(json.dumps(data,ensure_ascii=False,separators=(',',':'))+'\n',encoding='utf-8')
        if application and application['service']=='weather':
            (root/'THIRD_PARTY_NOTICES.txt').write_text('Weather forecast data: MET Norway (The Norwegian Meteorological Institute).\nSource: https://api.met.no/weatherapi/locationforecast/2.0/\nLicense: Creative Commons Attribution 4.0 International, https://creativecommons.org/licenses/by/4.0/\nThe PC host selects a forecast time and translates condition names. This is a forecast, not an on-site observation. No weather data is bundled.\n',encoding='utf-8')
        if application and application['service']=='knowledge':
            (root/'THIRD_PARTY_NOTICES.txt').write_text('Knowledge article text: Chinese Wikipedia contributors, licensed under Creative Commons Attribution-ShareAlike 4.0 International (CC BY-SA 4.0).\nLicense: https://creativecommons.org/licenses/by-sa/4.0/\nLegal code: https://creativecommons.org/licenses/by-sa/4.0/legalcode\nEach item in pc/application.json retains a source revision URL, attribution, and license. Follow the source page history for individual contributors. Modifications: source excerpts, whitespace normalization, omitted headings and formula-only paragraphs. No generated padding. Share adaptations of this article text under CC BY-SA 4.0; preserve attribution and indicate changes. No institutional logos or endorsement included. Articles are educational excerpts, not medical, psychological, legal, or investment advice.\n本文为中文维基百科节选，来源版本与署名随每条内容保留。知识文本适用 CC BY-SA 4.0，不适用本包配置和美术的 MIT 许可。\n',encoding='utf-8')
        (root/'LICENSE.txt').write_text('Copyright (c) 2026 HachimoDock contributors.\nThis package contains original project-authored configuration and AI-assisted artwork.\nLicensed under the MIT License.\n\nPermission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:\n\nThe above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.\n\nTHE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.\n',encoding='utf-8')
        if application and application['service']=='knowledge':
            license_file=root/'LICENSE.txt'
            license_file.write_text('Knowledge article text in pc/application.json is NOT covered by the MIT license below. It remains CC BY-SA 4.0; see THIRD_PARTY_NOTICES.txt and the attribution in each article.\n\n'+license_file.read_text(),encoding='utf-8')
        errors=validate_widget(root);smoke=smoke_test_game(root) if not errors else None
        report.append({'id':ident,'runtimeBytes':len((root/'runtime/widget.json').read_bytes())-1,'errors':errors,'smoke':smoke})
    print(json.dumps(report,ensure_ascii=False,indent=2))
    return 1 if any(r['errors'] or not (r['smoke'] or {}).get('ok') for r in report) else 0
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--art-dir',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--only',nargs='+',choices=[b[0] for b in BUILDERS]);p.add_argument('--version',default='1.0.0');a=p.parse_args();raise SystemExit(build(a.art_dir,a.output,a.only,a.version))
