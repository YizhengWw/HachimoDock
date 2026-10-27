"""Bounded package-owned location forecast mapping. No expressions or executable code."""
import ipaddress
import math
import re
from urllib.parse import urlsplit

TEXT_KEYS=set('readyEyebrow readyHeadline readyValue sourceNote detailsNote cachedNote cityEyebrow loadingHeadline failedHeadline locatingHeadline chooseHeadline emptyValue autoMode manualMode locatingNote locateFailedNote loadingNote failedNote deprecatedNote todoTitle todoEmpty autoButton selectLabel selectPlaceholder customLabel cityLabel latLabel lonLabel saveButton autoPending unspecified syncHint dataNotice geoNotice forecastNotice'.split())
def short(s,n):
    return isinstance(s,str) and bool(s.strip()) and len(s.encode())<=n and not any(ord(c)<32 or 127<=ord(c)<=159 for c in s)
def pointer(s):
    return short(s,128) and s.startswith('/') and all(t.startswith(('0','1')) for t in s.split('~')[1:])
def url(s):
    assert short(s,512)
    u=urlsplit(s);h=u.hostname or ''
    assert u.scheme=='https' and not u.username and not u.password and u.port in (None,443) and not u.query and not u.fragment and '?' not in s and '#' not in s
    try:ipaddress.ip_address(h)
    except ValueError:pass
    else:raise AssertionError('IP literals not allowed')
    assert len(h)<=253 and '.' in h and not h.endswith('.') and not h.endswith(tuple('.'+s for s in ('local','localhost','internal','lan','home','test','invalid','onion')))
    assert all(re.fullmatch(r'[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?',s) for s in h.split('.'))
def exact(v,keys):
    assert isinstance(v,dict) and set(v)==set(keys.split())
def location(v):
    exact(v,'city lat lon');assert short(v['city'],60)
    for k,b in [('lat',90),('lon',180)]:
        assert type(v[k]) in (int,float) and math.isfinite(v[k]) and abs(v[k])<=b
def request(v,coordinates):
    exact(v,'endpoint params');url(v['endpoint']);p=v['params']
    assert isinstance(p,dict) and len(p)<=12
    for k,val in p.items():
        assert short(k,40) and re.fullmatch(r'[A-Za-z0-9_-]+',k) and short(val,256)
        assert '$' not in val or coordinates and val in ('$lat','$lon')
    if coordinates:assert '$lat' in p.values() and '$lon' in p.values()
def validate_location_profile(p):
    exact(p,'schemaVersion cities geo forecast text links')
    assert type(p['schemaVersion']) is int and p['schemaVersion']==1
    assert isinstance(p['cities'],list) and 1<=len(p['cities'])<=64
    for c in p['cities']:location(c)
    assert len({c['city'] for c in p['cities']})==len(p['cities'])
    g=p['geo'];exact(g,'request fields require aliases cooldownSec');request(g['request'],False)
    exact(g['fields'],'city lat lon');assert all(pointer(s) for s in g['fields'].values())
    assert isinstance(g['require'],dict) and len(g['require'])<=4
    assert all(pointer(k) and (type(v) is bool or short(v,64)) for k,v in g['require'].items())
    assert isinstance(g['aliases'],dict) and len(g['aliases'])<=64 and all(short(k,60) and short(v,60) for k,v in g['aliases'].items())
    assert type(g['cooldownSec']) is int and 86400<=g['cooldownSec']<=604800
    f=p['forecast'];exact(f,'request rows time value conditions labels fallbackLabel refreshSec maxAgeSec');request(f['request'],True)
    assert all(pointer(f[k]) for k in ('rows','time','value'))
    assert isinstance(f['conditions'],list) and 1<=len(f['conditions'])<=4 and all(pointer(s) for s in f['conditions'])
    assert isinstance(f['labels'],list) and len(f['labels'])<=32
    for item in f['labels']:
        exact(item,'contains label');assert short(item['contains'],64) and short(item['label'],60)
    assert short(f['fallbackLabel'],60)
    assert type(f['refreshSec']) is int and 1800<=f['refreshSec']<=86400
    assert type(f['maxAgeSec']) is int and 3600<=f['maxAgeSec']<=86400
    assert isinstance(p['text'],dict) and TEXT_KEYS<=set(p['text'])<=TEXT_KEYS|set('cityButton emptyCityButton pickerNote pickerConfirm pickerCancel refreshButton todoButton'.split())
    for text in p['text'].values():
        assert short(text,600)
        tokens=re.findall(r'\{([^{}]*)\}',text)
        assert all(t in ('city','time','value','condition','mode') for t in tokens)
        rest=re.sub(r'\{[^{}]*\}','',text);assert '{' not in rest and '}' not in rest
    assert isinstance(p['links'],list) and len(p['links'])<=4
    for link in p['links']:
        exact(link,'label url');assert short(link['label'],90);url(link['url'])
