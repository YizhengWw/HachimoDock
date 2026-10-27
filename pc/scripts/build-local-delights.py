#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Build three original offline packages from their mechanism definitions."""
import json,sys,shutil
from pathlib import Path
from PIL import Image
from app_font_assets import add_fonts
REPO=Path(__file__).resolve().parents[2]
OUT=REPO/'pc/community-components/local-delights'
ART=REPO/'pc/design/local-delights-assets'
STATES=['ready','busy','result','editing','empty','offline']
def rgb(h):
    n=int(h,16);return ((n>>19)&31)<<11|((n>>10)&63)<<5|((n>>3)&31)
def node(t,x,y,w,h,c,**kw):return dict(type=t,x=x,y=y,w=w,h=h,color=rgb(c),**kw)
def text(x,y,w,h,c,**kw):return node('text',x,y,w,h,c,**kw)
def button(x,y,w,label,action,bg,fg,**kw):
    # The native fallback font has ASCII angle brackets, not U+2039/U+203A.
    label={'‹':'<','›':'>'}.get(label,label)
    return node('button',x,y,w,48,bg,text=label,action=action,foreground=rgb(fg),font_size=18,padding=12,radius=16,align='center',valign='center',**kw)
def items(*pairs):return [dict(title=a,note=b) for a,b in pairs]
DECKS=[
dict(title='今天吃什么',items=items(('热腾腾面条','先挑喜欢的汤底，再加一份蔬菜和蛋白质。'),('街角小馄饨','一碗热汤，把赶路的心也放慢一点。'),('来份盖浇饭','荤素搭配就好，今天不必把午饭变成考试。'),('清爽轻食碗','想吃清爽就选它，别忘了主食和蛋白质。'),('香喷喷饺子','蘸料按你的口味来，快乐不用统一配方。'),('附近家常菜','去那家一直路过的小店，给日常一点新鲜。'))),
dict(title='休息十分钟',items=items(('到窗边看远处','暂时放下屏幕，看看云和远处的树。'),('给朋友发句话','不必等到有大事，一句最近好吗就够了。'),('收拾桌上一角','只整理手边的一小块，不用启动大扫除。'),('伸个懒腰','动作轻柔，以舒服为准，别和身体较劲。'),('慢慢喝杯水','离开椅子接杯水，给忙碌按一下暂停。'),('走一小圈','在安全方便的地方走走，回来再接着做。'))),
dict(title='今晚小计划',items=items(('读上几页书','不用定宏大的目标，读到喜欢的一句就赚了。'),('做一道拿手菜','熟悉的味道，也可以是今天的奖励。'),('听完一张专辑','挑张喜欢的，让耳朵拥有一点完整时间。'),('整理三张照片','给一段好回忆起个名字，不急着全整理完。'),('散步看晚霞','天气和环境合适再出门，抬头看看天空。'),('早点休息','把没做完的写下来，今天可以先到这里。')))]
ANSWERS=items(('先走一小步','把问题缩小成今天能试的一步，答案常常藏在行动之后。'),('给自己一点余地','不必一次做对所有选择。留一条可调整的路，也是一种认真。'),('听听真实的需要','如果暂时不用满足别人的期待，你最想照顾的是什么？'),('先把事实写下来','把已知和猜测分开。纸上的问题，往往比脑海里的更容易看清。'),('不急着说可以','重要的承诺值得一点时间。想清楚边界，再给出你的回答。'),('找人聊聊看','换一个视角不是交出决定权，而是让自己的地图多一点信息。'),('休息也是进度','疲惫时先照顾身体。睡一觉后，再看这个问题是否还是原来的大小。'),('允许一个小实验','如果代价可承受，就做一次可撤回的尝试，让现实给你反馈。'),('珍惜已经拥有的','把注意力从还差什么，挪一点到已经做到了什么。'),('今天可以慢一点','慢不是落后。找到适合自己的节奏，比追赶每个声音更重要。'),('把边界说清楚','温和而明确地表达需要，让对方不用猜，也让自己少一点内耗。'),('给变化留扇门','过去的答案不必永远有效。新情况出现时，你可以重新选择。'),('先做最重要的一件','如果今天只能推进一件事，哪件会让明天的你轻松一些？'),('不确定也很正常','有些问题需要时间生长。暂时不知道，不代表你做得不够好。'),('留意那个小开心','一顿好饭、一阵风、一句问候，微小的快乐也值得认真接住。'),('决定仍然属于你','这张卡只是思考的起点，不是预测。重大决定请结合事实和专业意见。'))
SPECS=[('decision-capsule','纠结扭蛋机','capsule','橘子酱','life','在三个生活分类里随机抽一个小主意；可排除不想选的选项并恢复，选择会保存在设备。'),('answer-book','答案之书','book','土豆泥','relax','想好一个问题，揭开一张原创思考卡；换条不连续重复。仅供轻松反思，不预测结果或代替专业建议。'),('hope-train','盼头列车','train','cobi123','life','在设备上设置下班、周末或特别日子的时间，保存后显示真实倒计时。断电后连接新版客户端校时，再可离线使用。')]
def build(spec):
    id,name,art,author,category,description=spec;p=OUT/id;(p/'runtime').mkdir(parents=True,exist_ok=True);(p/'assets').mkdir(exist_ok=True)
    Image.open(ART/f'{art}.png').convert('RGB').resize((640,480),Image.Resampling.LANCZOS).save(p/'assets/background.png',optimize=True)
    train=id=='hope-train';book=id=='answer-book'
    ops=['previous','next','edit','cancel','up','down'] if train else ['draw','close'] if book else ['draw','exclude','reset','previous','next']
    action={f'local.{o}':o for o in ops}
    groups=[dict(title='下班',kind='weekdays'),dict(title='周末',kind='weekend'),dict(title='特别日子',kind='date')] if train else [dict(title='留一点时间给自己',items=ANSWERS)] if book else DECKS
    vals=[0,0,1080,0,0,0,540] if train else [0,0,-1,0,-1,0,-1]
    dark='eee0c5' if book else '34483d' if train else '5c3926';muted='b7ad9f' if book else '66705c' if train else '92745e';accent='dbc18e' if book else '466b52' if train else 'bd592b';light='1c2635' if book else 'fff9ed'
    dashboard=dict(title=name,eyebrow=groups[0]['title'],headline='等待时间校准' if train else '把问题放在心里' if book else '把纠结交给小扭蛋',note='连接新版客户端校时' if train else '仅供思考，不替你作决定。' if book else '左右换分类，抽到不喜欢的可以排除。',metricValue='—' if train else str(len(groups[0]['items'])),metricLabel='可选内容',metricUnit='',badge=f'1/{len(groups)}',footer='全局键退出',visualStyle='clean',visualPalette='forest' if train else 'ember',visualLayout='tool')
    dashboard['visualSprite']='timer' if train else 'star'
    r=dict(schema_version=1,engine='p4-bounded-runtime-v4',vars={f'local{i}':dict(type='int',init=v) for i,v in enumerate(vals)},states=STATES,initial_state='ready',transitions=[dict(on=a,**{'from':'*'}) for a in action],tick=[],dashboard=dashboard,persist=dict(version=1,vars=[f'local{i}' for i in range(7)],states={s:'ready' for s in STATES}),input=dict(engine='p4-input-v1'),local=dict(engine='p4-local-tools-v1',kind='countdown' if train else 'deck',groups=groups,actions=action,reveal_ms=900,idle=dashboard['headline'],busy='翻开这一页…' if book else '小主意正在出发…',hint=dashboard['note']))
    if not train and not book:r['input']['confirm']={'local.reset':'恢复当前分类的全部选项？'}
    if train:r['input']['repeat']=['local.up','local.down']
    n=[node('image',0,0,640,480,'000000',image='bg'),node('icon',24,22,24,24,dark,icon='clock' if train else 'book' if book else 'spark'),text(58,20,300,30,dark,bind='title',font_size=22,weight=600)]
    if train:
        n += [button(24,72,48,'‹','local.previous',light,dark),node('button',80,72,184,48,light,bind='eyebrow',foreground=rgb(dark),action='local.edit',font_size=20,padding=12,radius=16,align='center',valign='center'),button(272,72,48,'›','local.next',light,dark),text(24,145,580,44,dark,bind='headline',font_size=30,weight=600),text(24,200,430,64,dark,bind='metricValue',font_size=46,weight=600),text(448,215,80,36,dark,bind='metricUnit',font_size=24),text(24,274,570,50,muted,bind='note',font_size=18,line_height=25)]
        n += [button(24,360,174,'设置时间','local.edit',accent,'fff9ed',icon='clock',state_mask=55),button(24,360,130,'保存','local.edit',accent,'fff9ed',icon='check',state_mask=8),button(166,360,100,'取消','local.cancel',light,dark,state_mask=8),button(280,360,74,'−','local.down',light,dark,state_mask=8),button(366,360,74,'+','local.up',light,dark,state_mask=8)]
        footer='SW1 设置/保存  SW2 取消  左右切换  上下调整'
    else:
        x=300 if not book else 266;w=640-x-24
        n += [text(x,95,w,30,muted,bind='eyebrow',font_size=18),text(x,140,w,88,dark,bind='headline',font_size=28,line_height=40,weight=600),text(x,238,w,94,muted,bind='note',font_size=18,line_height=26),node('meter',x,340,w,4,accent,bind='progress')]
        n += [button(x,365,w if book else 158,'再抽一条' if book else '抽一个','local.draw',accent,'1c2635' if book else 'fff9ed',icon='spark',state_mask=21)]
        if book:n += [button(x,365,w,'翻开答案','local.draw',accent,'1c2635',icon='book',state_mask=1),button(28,365,180,'重新提问','local.close',light,dark,icon='book')];n[-3]['state_mask']=20
        else:
            n += [button(470,365,146,'排除重抽','local.exclude',light,dark,state_mask=4),button(470,365,146,'恢复选项','local.reset',light,dark,state_mask=17),button(24,365,64,'‹','local.previous',light,dark),button(104,365,64,'›','local.next',light,dark),text(184,371,85,32,muted,bind='badge',font_size=18)]
        footer='SW1 揭晓/换条  SW2 重新提问' if book else 'SW1 抽取  SW2 排除  左右换分类'
    primary='edit' if train else 'draw';secondary='cancel' if train else 'close' if book else 'exclude'
    n += [node('panel',0,432,640,48,light,opacity=238),node('button',20,445,40,22,light,text='SW1',key_for=f'local.{primary}',foreground=rgb(muted),font_size=11,padding=2,radius=4,stroke=rgb(muted),stroke_width=1),text(66,447,102,22,muted,text='设置/保存' if train else '揭晓/换条' if book else '抽取',font_size=13),node('button',176,445,40,22,light,text='SW2',key_for=f'local.{secondary}',foreground=rgb(muted),font_size=11,padding=2,radius=4,stroke=rgb(muted),stroke_width=1),text(222,447,84,22,muted,text='取消' if train else '重新提问' if book else '排除',font_size=13),text(310,447,182,22,muted,text='左右选项 · 上下调整' if train else '静心一刻，再做决定' if book else '左右换分类',font_size=13),text(502,447,114,22,muted,text='全局键退出',font_size=14,align='right')]
    if not train and not book:
        n.insert(1,node('panel',0,0,640,64,light,opacity=240,opacity_end=20,gradient='vertical'))
        badge=next(n for n in n if n.get('bind')=='badge');badge.update(color=rgb(dark),align='center')
        n.insert(n.index(badge),node('panel',180,365,88,48,light,opacity=235,radius=16))
    if train:
        n.insert(1,node('panel',12,132,392,196,light,opacity=225,opacity_end=100,gradient='horizontal',radius=20))
        next(n for n in n if n.get('bind')=='note')['w']=360
        next(n for n in n if n.get('bind')=='eyebrow')['state_mask']=55
        n.append(text(80,72,184,48,dark,bind='eyebrow',font_size=20,align='center',valign='center',state_mask=8))
    r['surface']=dict(engine='p4-app-surface-v1',graphics='p4-app-graphics-v1',background=rgb(light),images=[dict(id='bg',asset='assets/background.png',frame_width=640,frame_height=480,frames=1,fps=1,encoding='jpeg')],elements=n)
    add_fonts(p,r,max_chars=49)
    events={'draw':('SW1','button.sw1.short_press'),'close':('SW2','button.sw2.short_press'),'exclude':('SW2','button.sw2.short_press'),'previous':('前方摇杆','knob.rotate_ccw'),'next':('前方摇杆','knob.rotate_cw'),'edit':('SW1','button.sw1.short_press'),'cancel':('SW2','button.sw2.short_press'),'up':('前方摇杆','joystick.up'),'down':('前方摇杆','joystick.down'),'reset':('前方摇杆','button.encoder.short_press')}
    labels={'draw':'抽取','close':'重新提问','exclude':'排除重抽','previous':'上一项','next':'下一项','edit':'设置或保存','cancel':'取消','up':'增加','down':'减少','reset':'恢复选项'}
    bindings=[dict(action=a,control=events[o][0],event=events[o][1],label=labels[o]) for a,o in action.items()]
    docs={'component.json':dict(id=id,name=name,version='1.0.0' if book else '1.0.1',kind='tool',description=description,author=author),'negative-screen.json':dict(dashboard=dashboard),'runtime/widget.json':r,'buttons.json':bindings,'share.json':dict(title=name,description=description,author=author,category=category,license='HachimoDock Project License / OFL-1.1')}
    shutil.copyfile(REPO/'LICENSE',p/'LICENSE.txt')
    (p/'licenses').mkdir(exist_ok=True);(p/'licenses/content.txt').write_text('Original text and generated background artwork: HachimoDock Project License (see LICENSE.txt). Font atlas: SIL OFL-1.1 (see petui-type-OFL.txt).\n',encoding='utf-8')
    for f,d in docs.items():(p/f).write_text(json.dumps(d,ensure_ascii=False,separators=(',',':'))+'\n',encoding='utf-8')
    print(id,len(json.dumps(r,ensure_ascii=False,separators=(',',':')).encode()),'runtime bytes',len(n),'nodes')
if __name__=='__main__':
    for s in SPECS:build(s)
