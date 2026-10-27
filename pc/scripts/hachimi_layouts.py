"""Individually authored 640×480 layouts from the approved concept sheets.

Only layout is shared; behavior and honest live/empty states stay in each package.
"""
def rgb(s):
    n=int(s.lstrip('#'),16);return ((n>>19)<<11)|(((n>>10)&63)<<5)|((n>>3)&31)
def box(t,x,y,w,h,color,**kw):return dict(type=t,x=x,y=y,w=w,h=h,color=rgb(color),**kw)
def text(value,x,y,w,h=24,size=16,color='#544031',bind=False,**kw):
    return box('text',x,y,w,h,color,font_size=size,**({'bind':value} if bind else {'text':value}),**kw)
def icon(name,x,y,color,size=21):return box('icon',x,y,size,size,color,icon=name)
def panel(x,y,w,h,color,**kw):return box('panel',x,y,w,h,color,**kw)
def button(value,x,y,w,color,ink,ico=None,action=None,bind=False,h=48,**kw):
    return box('button',x,y,w,h,color,**dict(font_size=17,radius=16,foreground=rgb(ink),padding=18,weight=600,**({'bind':value} if bind else {'text':value}),**({'icon':ico} if ico else {}),**({'action':action} if action else {}))|kw)
def label(value,x,y,w,h,size,color,**kw):return text(value,x,y,w,h,size,color,bind=True,**kw)
def footer(ink,paper,a,b,extra=''):
    nodes=[panel(0,436,640,44,paper,opacity=240),panel(0,436,640,1,ink,opacity=24)]
    x=26
    for key,caption in [('SW1',a),('SW2',b)]:
        nodes.append(box('button',x,449,32,18,paper,text=key,font_size=11,line_height=14,radius=4,padding=2,opacity=0,foreground=rgb(ink),stroke=rgb('#b7b0a2'),stroke_width=1))
        width=len(caption)*15+4;nodes.append(text(caption,x+38,446,width,24,15,ink));x+=38+width+21
    if extra and x+len(extra)*14<510:nodes.append(text(extra,x,446,510-x,24,14,ink))
    nodes.append(text('全局键退出',510,446,104,24,14,ink,align='right'))
    return nodes
def layout(ident,r):
    themes={
      'home':('#544031','#fff8eb','#9b613f','home'),
      'garden':('#203f2f','#eef4e8','#315f44','leaf'),
      'desk':('#563321','#fcf1df','#b95d31','desk'),
      'agent':('#f1f1ed','#101b25','#eac785','agent'),
      'travel':('#344e41','#f4f2df','#506b50','compass'),
      'weather':('#233c55','#eef3f8','#4d7899','sun'),
      'notes':('#70423a','#fff1e8','#b96952','mic'),
      'learn':('#f0eeff','#1d1e36','#d6d1fb','book'),
      'sounds':('#edf2e8','#102c23','#d8dfbd','sound'),
      'break':('#4d3f63','#f4eff8','#7e619c','stretch')}
    kind=ident.removeprefix('hachimi-');ink,paper,accent,brand=themes[kind];dark=kind in ('agent','learn','sounds')
    n=[box('image',0,0,640,480,paper,image='background')]
    if kind=='home':
        n += [panel(0,0,640,115,'#fff7e4',opacity=221,opacity_end=138,gradient='vertical'),panel(0,115,640,111,'#fff5e2',opacity=138,opacity_end=0,gradient='vertical'),panel(0,336,640,144,'#f5e8ce',opacity=0,opacity_end=170,gradient='vertical')]
    else:
        shade={'garden':'#ecf3e6','desk':'#fff2de','agent':'#15212d','travel':'#f3f0d9','weather':'#e6f0f7','notes':'#fff0e7','learn':'#242346','sounds':'#102e25','break':'#f3edf9'}[kind]
        n += [panel(0,0,290,436,shade,opacity=245,opacity_end=200),panel(290,0,350,436,shade,opacity=200,opacity_end=60 if dark else 0)]
    n += [icon(brand,26,21,ink),label('title',56,18,300,28,18,ink,weight=600)]
    action=lambda a:('host.' if r.get('host') else '')+a
    def actions(first_icon,second_icon,first_width=142,second_width=148,y=366):
        return [button('metricLabel',26,y,first_width,accent,'#27352d' if dark else '#fffaf1',first_icon,action('primary'),True),button('metricUnit',36+first_width,y,second_width,'#394653' if dark else '#fffaf1',ink,second_icon,action('secondary'),True,opacity=170 if dark else 230,stroke=rgb('#69737a' if dark else '#ffffff'),stroke_width=1)]
    if kind=='home':
        n += [label('eyebrow',426,20,188,24,14,ink,align='right'),label('headline',28,78,580,48,32,ink,weight=600),text('在小屋里，陪你度过平凡的一天。',28,131,430,27,17,ink),button('在家陪你',496,175,118,'#fff6df',ink,h=38,opacity=220,padding=12,stroke=rgb('#fff9ee'),stroke_width=1)]
        # Real independent decorations. Three authored placements avoid implying
        # an unsupported drag gesture; tapping either object selects it.
        for page,(px,py,ox,oy) in enumerate([(510,270,228,228),(196,222,506,284),(74,230,526,276)]):
            n += [box('image',px,py,82,82,ink,image='decor',page=page,tile={'columns':3,'rows':3,'row':0,'var':'plant'},action='previous'),box('image',ox,oy,78,78,ink,image='decor',page=page,tile={'columns':3,'rows':3,'row':1,'var':'object'},action='next')]
        n += [panel(28,309,326,42,'#fff9ee',opacity=225,radius=12,stroke=rgb('#ffffff'),stroke_width=1),label('note',42,318,300,24,15,ink)]
        n += actions('heart','home',126,144)
        n += footer(ink,paper,'互动/更换','布置/完成')
    elif kind=='garden':
        r['dashboard'].update(progress={'pct_of':'left','of_max':'duration','label':'剩余时间'})
        n += [text('专注与花园，都按自己的节奏。',356,20,258,24,13,ink,align='right'),label('eyebrow',28,88,390,25,16,ink),label('metricValue',24,120,340,96,76,ink,page=0),label('badge',28,120,300,96,76,ink,page=1),label('headline',28,220,350,28,17,ink),icon('leaf',28,273,ink,18),text('完成收获；花园展示最近三株。',54,268,354,28,16,ink),panel(28,337,135,5,'#b2c8a9',radius=2),box('meter',28,337,135,5,'#527654',bind='progress'),text('不急，一次只做一件事。',176,327,350,26,14,ink)]
        n += [panel(400,102,204,216,'#f6f9ed',radius=22,opacity=235),box('image',412,114,178,178,ink,image='decor',page=0,tile={'columns':3,'rows':3,'row':2,'var':'elapsed','max_var':'duration'})]
        for i,(x,y) in enumerate([(404,106),(508,178),(406,248)]):
            n += [box('image',x,y,94,94,ink,image='decor',page=1,tile={'columns':3,'rows':3,'row':2,'var':'bloom'},show={'var':'flowers','min':i+1,'max':1000000000})]
        n += actions('clock','leaf',142,148)+footer(ink,paper,'开始/暂停','花园','左右调时')
    elif kind=='desk':
        n += [text('状态由你决定',390,20,224,24,14,ink,align='right'),label('eyebrow',30,98,424,24,14,'#bc632b'),label('headline',28,137,430,80,55,ink,weight=600),text('当前状态剩余',30,231,160,28,18,ink),label('metricValue',194,222,200,46,30,ink),label('note',30,282,390,56,17,ink)]
        n += actions('desk','clock',178,170)+footer(ink,paper,'切换状态','重计时','左右调时')
    elif kind=='agent':
        n += [text('真实回执 · 隐私优先',380,20,234,24,14,'#abb8bd',align='right'),label('eyebrow',28,88,560,28,15,'#b6c3cb'),label('headline',28,130,562,78,28,ink,weight=600),panel(26,224,588,116,'#263642',opacity=210,radius=12,stroke=rgb('#42515c'),stroke_width=1),icon('list',44,242,'#c8d2d8',20),label('note',76,239,518,92,17,'#c8d2d8')]
        n += actions('plus','list',148,146)+footer('#b6c3cb',paper,'补充要求','进展')
    elif kind=='travel':
        n[1:3]=[panel(0,0,332,304,paper,opacity=200,opacity_end=0)]
        n += [label('eyebrow',28,87,278,28,14,ink),label('headline',28,126,278,48,32,ink,weight=600),label('metricValue',28,185,278,42,26,ink),label('note',28,236,270,96,16,ink),button('metricLabel',26,338,132,accent,'#fffaf1',action=action('primary'),bind=True,h=44,padding=8),button('metricUnit',168,338,132,'#fffaf1',ink,action=action('secondary'),bind=True,h=44,padding=8,opacity=220)]
        n += [button('上一站',26,388,132,'#fffaf1',ink,action=action('previous'),h=44,padding=10,opacity=230),button('下一站',168,388,132,'#fffaf1',ink,action=action('next'),h=44,padding=10,opacity=230)]
        n += footer(ink,paper,'出发/确认','明信片/取消')
    elif kind=='weather':
        n += [button('eyebrow',374,12,240,'#fffaf1',ink,'compass',action('choose'),True,h=48,opacity=230,padding=12),label('metricValue',26,81,362,112,88,ink),label('headline',28,207,550,60,26,ink),icon('cloud',28,285,ink,22),label('note',63,276,548,70,17,ink)]
        n += actions('sun','list',148,156)
        n += [button('上一城',398,366,102,'#fffaf1',ink,action=action('previous'),h=48,padding=10,opacity=230),button('下一城',510,366,104,'#fffaf1',ink,action=action('next'),h=48,padding=10,opacity=230)]
        n += footer(ink,paper,'更新/确认','待办/取消','左右选城')
    elif kind=='notes':
        n += [label('eyebrow',345,20,269,24,13,ink,align='right'),label('headline',28,87,420,88,34,ink,weight=600),text('一句话，也值得留下来。',28,182,380,28,17,ink),panel(26,242,588,108,'#fff8f2',opacity=226,radius=16,stroke=rgb('#ffffff'),stroke_width=1),icon('mic',44,258,'#a57b6e',18),text('语音转写 · 确认后才保存',71,252,400,24,13,'#a57b6e'),label('note',44,286,550,56,18,ink)]
        n += actions('mic','check',148,148)+footer(ink,paper,'录音/停止','保存','上下待办/删除')
    elif kind=='learn':
        # Preserve the approved pet/waterdrop illustration on the right.
        # Two three-line body regions hold the existing bounded page segments.
        n[1:3]=[panel(0,0,390,436,'#242346',opacity=210,opacity_end=120),
                 panel(390,0,250,436,'#242346',opacity=120,opacity_end=0)]
        n += [button('eyebrow',430,12,184,'#393653',ink,'list',action('secondary'),True,h=48,padding=12),
              label('headline',28,80,352,62,26,ink,weight=600),
              label('note',28,151,352,84,21,'#e5e1f1',line_height=28),
              label('footer',28,239,352,84,21,'#e5e1f1',line_height=28),
              label('metricValue',28,337,352,24,13,'#b4afd9'),
              label('badge',492,335,120,28,16,'#d6d1fb',align='right')]
        n += [button('metricLabel',26,374,142,accent,'#27243e','spark',action('primary'),True,padding=16),
              icon('sound',190,388,'#b9b7cf',19),
              text('进入自动朗读',218,385,150,28,15,'#b9b7cf')]
        n += footer('#b9b7cf',paper,'换条/确认','分类/返回','左右翻页')
    elif kind=='sounds':
        n += [text('AMBIENT / 自然声景',28,95,440,25,13,'#adbd9f'),label('headline',28,133,560,112,43,ink),label('eyebrow',28,252,555,30,16,'#c0cbb8'),label('note',28,300,560,48,16,'#c8d2be')]
        n += actions('sound','clock',155,194)+footer('#b8cbb3',paper,'播放/停止','定时','摇杆切换/音量')
    else:
        n += [text('轻休息 · 舒适最重要',372,20,242,24,14,ink,align='right'),label('eyebrow',28,90,410,25,14,'#9b88ad'),label('headline',28,134,410,60,37,ink,weight=600),label('metricValue',26,210,338,89,69,ink),label('note',28,318,510,30,14,'#897794')]
        n += actions('stretch','clock',148,174)+footer('#897794',paper,'开始/暂停','稍后','左右换动作')
    for node in n:
        if node.get('text') in ('SW1','SW2'):
            node['key_for']=action('primary' if node['text']=='SW1' else 'secondary')
            node['text']='—'
    return {'engine':'p4-app-surface-v1','graphics':'p4-app-graphics-v1',**({'interaction':'p4-app-interaction-v1'} if kind in ('home','garden') else {}),'background':rgb(paper),'images':[{'id':'background','asset':'assets/background.png','frame_width':640,'frame_height':480,'frames':1,'fps':1,'encoding':'jpeg'}],'elements':n}
