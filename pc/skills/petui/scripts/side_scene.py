"""Strict opt-in continuous tile-world contract; no scripts or component IDs."""
import re

ENGINE='p4-side-scene-v1'
SCENE_KEYS={'engine','width','height','rows','tiles','actors','physics','actions','art','active_state','result_state','win_state','score_var','coins_var','lives_var','checkpoint_var'}
ACTOR_KEYS={'id','behavior','contact','x','y','width','height','skin','speed','range_min','range_max','value','vertical','solid','stompable','edge_turn'}
def integer(v,low,high):return type(v)is int and low<=v<=high
def identifier(v,n):return isinstance(v,str) and re.fullmatch(r'[A-Za-z0-9_.-]{1,%d}'%n,v) is not None
def fields(o,allowed,required=()):return isinstance(o,dict) and not set(o)-set(allowed) and set(required)<=set(o)

def side_scene_errors(runtime,capabilities=None,buttons=None):
    try:
        s=runtime['scene'];assert fields(s,SCENE_KEYS,SCENE_KEYS) and s['engine']==ENGINE
        if capabilities is not None:assert capabilities.get('widgetSideScene')==ENGINE
        assert runtime.get('engine')=='p4-bounded-runtime-v4' and runtime.get('tick')==[]
        assert not any(k in runtime for k in ('host','game','data','media','instrument','local','lifecycle','pages'))
        assert runtime['surface']['engine']=='p4-app-surface-v1' and runtime['surface']['graphics']=='p4-app-graphics-v1'
        assert not any(k in runtime['surface'] for k in ('game_view','scene_view','data_view'))
        w,h=s['width'],s['height'];assert integer(w,20,160) and integer(h,8,16)
        assert isinstance(s['rows'],list) and len(s['rows'])==h
        tiles=s['tiles'];assert isinstance(tiles,list) and 1<=len(tiles)<=16
        for i,t in enumerate(tiles):
            assert fields(t,('flags','reward','replacement'),('flags',))
            flags=t['flags'];assert integer(flags,0,15) and integer(t.get('reward',0),0,10000) and integer(t.get('replacement',0),0,len(tiles)-1)
            assert not (flags&2 and flags&5)
            if i==0:assert flags==0 and not t.get('reward',0)
        for row in s['rows']:assert isinstance(row,str) and len(row)==w and all(c in '0123456789abcdef'[:len(tiles)] for c in row)
        assert all(not t.get('reward',0) or not tiles[t.get('replacement',0)].get('reward',0) for t in tiles)
        p=s['physics'];bounds={'run_speed':(32,240),'acceleration':(100,2000),'friction':(100,2400),'gravity':(200,1800),'jump_speed':(100,600),'terminal_speed':(240,640),'lives':(1,9)}
        assert fields(p,bounds,bounds) and all(integer(p[k],*b) for k,b in bounds.items())
        actors=s['actors'];assert isinstance(actors,list) and 1<=len(actors)<=24
        names=set();controlled=0
        for a in actors:
            assert fields(a,ACTOR_KEYS,('id','behavior','contact','x','y','width','height','skin'))
            assert identifier(a['id'],15) and a['id'] not in names;names.add(a['id'])
            behavior=a['behavior'];assert behavior in ('controlled','walker','static','platform') and a['contact'] in ('none','damage','collect','goal','checkpoint')
            assert integer(a['x'],0,w*16) and integer(a['y'],0,h*16) and integer(a['width'],1,32) and integer(a['height'],1,32) and a['x']+a['width']<=w*16 and a['y']+a['height']<=h*16 and integer(a['skin'],0,7)
            for key in ('vertical','solid','stompable','edge_turn'):assert key not in a or type(a[key])is bool
            assert integer(a.get('speed',0),-160,160) and integer(a.get('range_min',0),0,2560) and integer(a.get('range_max',0),0,2560) and integer(a.get('value',0),0,10000)
            controlled+=behavior=='controlled'
            if behavior=='controlled':
                assert a['contact']=='none' and not a.get('solid') and not a.get('speed')
                assert all(not tiles[int(s['rows'][y][x],16)]['flags']&1 for y in range(a['y']//16,(a['y']+a['height']-1)//16+1) for x in range(a['x']//16,(a['x']+a['width']-1)//16+1))
            if behavior=='static':assert not a.get('speed') and not a.get('solid')
            if behavior in ('walker','platform'):
                lo,hi=a.get('range_min',0),a.get('range_max',0);vertical=a.get('vertical',False)
                assert a.get('speed',0)!=0 and lo<=hi and lo<=a['y' if vertical else 'x']<=hi and hi+a['height' if vertical else 'width']<=(h if vertical else w)*16
                if behavior=='walker':assert not vertical and not a.get('solid')
                else:assert a.get('solid') and a['contact']=='none'
        assert controlled==1
        states=[s[k] for k in ('active_state','result_state','win_state')]
        assert len(set(states))==3 and all(identifier(v,23) and v in runtime['states'] for v in states)
        variables=[s[k] for k in ('score_var','coins_var','lives_var','checkpoint_var')]
        assert len(set(variables))==4 and all(identifier(v,31) and runtime['vars'].get(v,{}).get('type')=='int' for v in variables)
        for key,maximum in zip(variables,(10**9,65535,p['lives'],len(actors))):
            if key in runtime.get('limits',{}):assert runtime['limits'][key][0]<=0 and runtime['limits'][key][1]>=maximum
        actions=s['actions'];assert fields(actions,('left','right','jump','pause'),('left','right','jump','pause'))
        assert len(set(actions.values()))==4 and all(identifier(v,47) for v in actions.values())
        assert runtime['input']['engine']=='p4-input-v1' and runtime['input'].get('hold')==actions['jump']
        transitions=runtime['transitions'];assert all(t['on'] in actions.values() and not set(t)-{'from','on','to'} for t in transitions)
        assert set(t['on'] for t in transitions)==set(actions.values())
        pauses=[t['to'] for t in transitions if t['on']==actions['pause'] and t['from']==s['active_state'] and t.get('to') in runtime['states'] and t['to'] not in states]
        assert len(pauses)==1 and any(t['on']==actions['pause'] and t['from']==pauses[0] and t.get('to')==s['active_state'] for t in transitions)
        assert runtime.get('initial_state') in runtime['states'] and runtime['initial_state'] not in [*states,pauses[0]]
        if buttons is not None:
            events={b['action']:b['event'] for b in buttons}
            assert events.get(actions['left'])=='knob.rotate_ccw' and events.get(actions['right'])=='knob.rotate_cw' and events.get(actions['jump'])=='button.sw1.short_press' and events.get(actions['pause'])=='button.sw2.short_press'
        art=s['art'];assert fields(art,('image','columns','rows','scale','x','y','w','h','sky','tiles','skins'),('image','columns','rows','scale','x','y','w','h','sky','tiles','skins'))
        assert identifier(art['image'],15) and integer(art['columns'],1,32) and integer(art['rows'],1,15) and art['columns']*art['rows']<=256
        assert integer(art['scale'],1,3) and integer(art['x'],0,639) and integer(art['y'],0,479) and integer(art['w'],160,640-art['x']) and integer(art['h'],128,480-art['y']) and integer(art['sky'],0,65535)
        assert art['h']>=h*16*art['scale']
        images=runtime['surface'].get('images',[]);im=next(im for im in images if im['id']==art['image'])
        assert 'encoding' not in im and im['frame_width']==art['columns']*16 and im['frame_height']==art['rows']*16
        assert isinstance(art['tiles'],list) and len(art['tiles'])==len(tiles) and all(integer(v,0,art['columns']*art['rows']-1) for v in art['tiles'])
        skins=art['skins'];assert isinstance(skins,list) and 1<=len(skins)<=8 and all(a['skin']<len(skins) for a in actors)
        for skin in skins:
            assert fields(skin,('idle','walk','air','hurt','fps','width','height'),('idle','walk','air','hurt','fps'))
            assert integer(skin['fps'],1,20) and integer(skin.get('width',16),8,32) and integer(skin.get('height',16),8,32)
            for key in ('idle','walk','air','hurt'):assert isinstance(skin[key],list) and 1<=len(skin[key])<=8 and all(integer(v,0,art['columns']*art['rows']-1) for v in skin[key])
        return []
    except (AssertionError,KeyError,TypeError,ValueError,StopIteration,AttributeError):
        return ['side scene 需要明确能力、有界地图/角色、类型安全状态与动作、匹配的 PNG 图集；不支持脚本或未知字段']
