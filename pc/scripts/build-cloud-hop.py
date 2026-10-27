"""Original resource-authored hold/release game; no native preset or copied pack."""
import importlib.util,json,sys
from pathlib import Path
from PIL import Image,ImageOps
spec=importlib.util.spec_from_file_location('surface',Path(__file__).with_name('build-new-component-surfaces.py'))
s=importlib.util.module_from_spec(spec);spec.loader.exec_module(s)
pack=s.ROOT/'cloud-hop';(pack/'assets').mkdir(parents=True,exist_ok=True)
hero,bg=map(Path,sys.argv[1:3])
ImageOps.fit(Image.open(bg).convert('RGB'),(640,480),method=Image.Resampling.LANCZOS).save(pack/'assets/background.png',optimize=True)
im=Image.open(hero).convert('RGBA');box=im.getchannel('A').getbbox();im=im.crop(box);im.thumbnail((78,78),Image.Resampling.LANCZOS)
out=Image.new('RGBA',(80,80));out.alpha_composite(im,((80-im.width)//2,80-im.height));out.save(pack/'assets/hero.png',optimize=True)
ink=s.rgb('493D34');green=s.rgb('548879');cream=s.rgb('FFF9EF');gold=s.rgb('D49D62')
actions={'jump.charge':'按住蓄力，松开跳跃','session.pause':'暂停 / 继续','session.restart':'重开本局（需确认）'}
states=['ready','playing','paused','result']
dash=dict(title='云端跳一跳',eyebrow='蓄一点力，向前一点点',headline={'switch_state':dict(ready='陪小狗，跳过下一朵云',playing='松开，出发！',paused='歇一下，云朵会等你',result='没关系，再跳一次')},metricValue={'var':'score'},metricLabel={'switch_state':dict(ready='按住开始',playing='按住蓄力',paused='已暂停',result='按住重来')},metricUnit={'switch_state':dict(ready='暂停',playing='暂停',paused='继续',result='暂停')},badge={'var':'best'},note='按住越久，跳得越远',footer='落上下一座平台得 1 分 · 掉落结束',progress={'pct_of':'charge','of_max':'hundred'},visualStyle='clean',visualPalette='mint',visualLayout='game',visualSprite='pet')
elements=[dict(type='image',x=0,y=0,w=640,h=480,image='background',color=65535),
 dict(type='icon',x=24,y=22,w=24,h=24,color=green,icon='cloud'),s.text(58,20,260,30,bind='title',size=22,weight=600),
 s.text(444,22,72,24,label='最佳',size=16),s.text(528,18,84,32,bind='badge',size=24,align='right'),
 s.text(26,64,470,38,bind='headline',size=24),s.text(542,63,70,56,bind='metricValue',size=44,align='right'),
 s.text(26,109,360,25,bind='note',size=16,color=green),
 dict(type='panel',x=24,y=361,w=592,h=8,color=s.rgb('DEDCD0'),radius=4),dict(type='meter',x=24,y=361,w=592,h=8,color=green,bind='progress'),
 s.button(24,387,280,48,'jump.charge',bind='metricLabel',icon='arrow',primary=True,color=green,state_mask=11),
 s.button(24,387,280,48,None,label='已暂停 · 点右侧继续',color=cream,state_mask=4),
 s.button(318,387,142,48,'session.pause',bind='metricUnit',icon='pause'),
 s.button(474,387,142,48,'session.restart',label='重开'),
 dict(type='panel',x=0,y=448,w=640,h=32,color=cream,opacity=235),
 s.button(20,454,33,20,None,label='SW1',key_for='jump.charge',font_size=11,padding=0,radius=4),s.text(59,453,166,22,label='按住蓄力 · 松开跳',size=13),
 s.button(246,454,33,20,None,label='SW2',key_for='session.pause',font_size=11,padding=0,radius=4),s.text(285,453,96,22,label='暂停 / 继续',size=13),
 s.button(395,454,54,20,None,label='中按',key_for='session.restart',font_size=11,padding=0,radius=4),s.text(455,453,46,22,label='重开',size=13),s.text(510,453,106,22,label='全局键退出',size=13,align='right')]
runtime=dict(schema_version=1,engine='p4-bounded-runtime-v4',vars={k:dict(type='int',init=v) for k,v in dict(score=0,best=0,charge=0,hundred=100).items()},states=states,initial_state='ready',
 transitions=[dict(**{'from':a,'on':'jump.charge','to':'playing'}) for a in ('ready','playing','result')]+[{'from':'playing','on':'session.pause','to':'paused'},{'from':'paused','on':'session.pause','to':'playing'},{'from':'*','on':'session.restart','to':'playing','set':{'charge':0}}],tick=[],dashboard=dash,
 input={'engine':'p4-input-v1','hold':'jump.charge','confirm':{'session.restart':'重新开始本局？最高分会保留。'}},lifecycle={'pause_action':'session.pause','record_var':'best'},
 persist={'version':1,'vars':['best'],'states':{x:'ready' for x in states}},
 scene={'tick_ms':20,'active_state':'playing','result_state':'result','score_var':'score','grid':{'width':16,'height':8},
  'motion':{'engine':'p4-motion-v1','body':'puppy','platforms':['home','next','ahead','far'],'gravity':18,'lift':7,'speed':[2,15],'charge_ms':1200,'charge_var':'charge',
    'track':{'engine':'p4-platform-track-v1','camera_x':4,'gap':[2,5],'width':[2,4],'tones':[1,2,3,4]}},
  'entities':[{'id':'puppy','x':2,'y':4,'width':2,'height':2,'tone':1,'shape':'character','bounds':'stop'}, {'id':'home','x':1,'y':6,'width':4,'height':1,'tone':2,'shape':'capsule'},
    {'id':'next','x':8,'y':6,'width':3,'height':1,'tone':3,'shape':'capsule'}, {'id':'ahead','x':8,'y':6,'width':2,'height':1,'tone':1,'shape':'capsule'}, {'id':'far','x':12,'y':6,'width':3,'height':1,'tone':4,'shape':'capsule'}],
  'rules':[{'on':'session.restart','do':[{'op':'restart'}]},{'on':'jump.charge','do':[{'op':'charge','entity':'puppy'}]},
    *[{'on':'landed','entity':'puppy','with':p,'do':[{'op':'score','add':1}]} for p in ['next','home','ahead','far']],
    {'on':'edge','entity':'puppy','edge':'any','do':[{'op':'stop'}]}]},
 surface={'engine':'p4-app-surface-v1','graphics':'p4-app-graphics-v1','background':cream,
  'images':[dict(id='background',asset='assets/background.png',frame_width=640,frame_height=480,frames=1,fps=1,encoding='jpeg'),dict(id='hero',asset='assets/hero.png',frame_width=80,frame_height=80,frames=1,fps=1)],
  'scene_view':dict(engine='p4-scene-surface-v1',x=24,y=138,w=592,h=216,colors=[45366,green,gold,32092,s.rgb('AEB8AD')],sprites=[dict(entity='puppy',image='hero',columns=1,fps=1)]),'elements':elements})
dash['visualLayout']='scoreboard';dash['visualSprite']='star'
s.fonts(pack,runtime,'云端跳一陪小狗过下朵蓄力松开出发暂停继续最佳按住开始重来歇会等你没关系再次分局键退出·')
s.save(pack/'runtime/widget.json',runtime)
s.save(pack/'component.json',dict(id='cloud-hop',name='云端跳一跳',version='1.1.1',kind='game',author='小豆包',description='按住蓄力、松开跳跃，陪小狗连续向前探索。不同距离、大小和颜色的平台不断生成；落上新平台得分，掉落结束，支持暂停、确认重开和最佳成绩存档。'))
s.save(pack/'buttons.json',[dict(action=a,control=f'SW{i+1}' if i<2 else '前方摇杆',event=f'button.sw{i+1}.short_press' if i<2 else 'button.encoder.short_press',label=label) for i,(a,label) in enumerate(actions.items())])
s.save(pack/'negative-screen.json',{'dashboard':{k:v if isinstance(v,str) else '0' if k in ('metricValue','badge') else '按住开始' if k=='metricLabel' else '暂停' if k=='metricUnit' else '陪小狗，跳过下一朵云' if k=='headline' else {'value':0,'label':''} for k,v in dash.items()}})
s.save(pack/'share.json',{'title':'云端跳一跳','author':'小豆包','tags':['游戏','治愈'],'description':'原创云端蓄力跳跃 · 标准 PetUI'})
(pack/'licenses/artwork.txt').write_text('背景和小狗为本项目原创 AI 辅助生成素材。无第三方游戏截图、商标或角色。静态小狗 PNG，不宣称逐帧角色动画。\n',encoding='utf8')
s.save(s.ROOT/'source-art/cloud-hop-provenance.json',dict(tool='image_gen',hero='../cloud-hop/assets/hero.png',background='../cloud-hop/assets/background.png',processing='Native 640x480 background, cropped transparent 80x80 hero; no text baked into artwork.'))
print(json.dumps({'component':str(pack),'compactBytes':len(json.dumps(runtime,ensure_ascii=False,separators=(',',':')).encode()),'images':len(runtime['surface']['images'])}))
