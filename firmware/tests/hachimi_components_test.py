"""Authoring-level interaction loops, separate from hardware acceptance."""
import sys
from pathlib import Path
import importlib.util
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pc/scripts'))
spec=importlib.util.spec_from_file_location('hachimi_builder',ROOT/'pc/scripts/build-hachimi-components.py')
builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)
from bounded_app import initial,action,advance,visible

def runtime(factory,ident):
    r=factory()[0];r['surface']=builder.layout(ident,r);return r

def test_home_independent_furnishings_and_placement():
    r=runtime(builder.home,'hachimi-home');s=initial(r)
    s=action(r,s,'secondary');assert s['state']=='plant'
    for _ in range(2):s=action(r,s,'primary')
    assert s['vars']['plant']==2 and s['vars']['object']==0
    s=action(r,s,'secondary');assert s['state']=='object'
    s=action(r,s,'primary');assert s['vars']['object']==1
    s=action(r,s,'secondary');assert s['state']=='place'
    old=visible(r,s);s=action(r,s,'primary');assert visible(r,s)!=old
    s=action(r,s,'secondary');assert s['state']=='idle'
    s=action(r,s,'primary');assert s['vars']['pats']==1
    for _ in range(3):s=advance(r,s,1000)
    assert s['state']=='idle' and s['vars']['plant']==2 and s['vars']['object']==1
    assert set(r['persist']['vars'])=={'pats','plant','object'} and r['persist']['page']

def test_garden_growth_pause_single_harvest_and_restart():
    r=runtime(builder.garden,'hachimi-garden');s=initial(r);s['vars']['duration']=300
    before=visible(r,s);s=action(r,s,'primary')
    for _ in range(100):s=advance(r,s,1000)
    assert s['vars']['elapsed']==100 and s['vars']['left']==200
    assert visible(r,s)!=before
    s=action(r,s,'primary');s=advance(r,s,5000)
    assert s['vars']['elapsed']==100
    s=action(r,s,'primary')
    for _ in range(200):s=advance(r,s,1000)
    assert s['state']=='done' and s['vars']['flowers']==1
    s=advance(r,s,10000);assert s['vars']['flowers']==1
    s=action(r,s,'secondary');assert s['page']=='garden'
    harvest=[n for n in r['surface']['elements'] if n.get('show')]
    assert len(harvest)==3 and sum(n['show']['min']<=s['vars']['flowers'] for n in harvest)==1
    s=action(r,s,'primary');assert s['page']=='focus' and s['vars']['elapsed']==0 and s['vars']['flowers']==1

def test_desk_adjustment_completion_and_break_pause():
    r=runtime(builder.desk,'hachimi-desk');s=action(r,initial(r),'primary');assert s['state']=='focus'
    s=action(r,s,'more');assert s['vars']['left']==1800 and s['vars']['duration']==1800
    s=advance(r,s,1000);assert s['vars']['left']==1799
    s=action(r,s,'secondary');assert s['vars']['left']==1800
    for _ in range(1800):s=advance(r,s,1000)
    assert s['state']=='done' and s['vars']['rounds']==1
    s=advance(r,s,10000);assert s['vars']['rounds']==1
    r=runtime(builder.stretch,'hachimi-break');s=action(r,initial(r),'primary')
    s=advance(r,s,1000);s=action(r,s,'primary');before=s['vars']['left'];s=advance(r,s,5000)
    assert s['state']=='paused' and s['vars']['left']==before
    old=visible(r,s);s=action(r,s,'next');assert visible(r,s)!=old
    s=action(r,s,'secondary');assert s['state']=='later' and s['vars']['left']==300
    for _ in range(300):s=advance(r,s,1000)
    assert s['state']=='idle' and s['vars']['left']==30
