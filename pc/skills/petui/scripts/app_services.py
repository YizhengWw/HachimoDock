"""Host application contract. Packages select bounded services, never execute code."""
import re
import datetime
from urllib.parse import urlsplit
from location_profile import validate_location_profile

OPERATIONS={
    'desktop-tasks':{'voice','details','previous','next'},
    'journey':{'depart','collection','previous','next'},
    'weather':{'refresh','todo','details','choose_city','previous','next'},
    'notes':{'record','save','todo','delete','previous','next'},
    'todos':{'open','toggle','filter','previous','next','up','down','row_a','row_b','row_c','row_d','row_e'},
    'knowledge':{'speak','favorite','previous','next','random','category'},
    'books':{'previous','next','random','category'},
    'news':{'refresh','toggle','previous','next','up','down'},
    'calendar':{'today','details','previous','next'},
    'soundscape':{'toggle','timer','previous','next','up','down','rain','sea','fire','insects'},
}
def bounded_string(value,limit):
    return isinstance(value,str) and bool(value.strip()) and len(value.encode())<=limit and not any(ord(c)<32 or 127<=ord(c)<=159 for c in value)
def application_errors(value,runtime):
    try:
        assert isinstance(value,dict) and set(value)<={'schemaVersion','engine','service','actions','content','location','autoRead'}
        assert type(value['schemaVersion']) is int and value['schemaVersion']==1 and value['engine'] in ('pc-app-services-v1','pc-app-services-v2','pc-app-services-v3','pc-app-calendar-v1','pc-app-books-v1','pc-app-news-v1')
        service=value['service'];assert service in OPERATIONS
        books=value['engine']=='pc-app-books-v1'
        assert books==(service=='books')
        library=value['engine']=='pc-app-services-v3' or books
        assert not library or service in {'knowledge','books'}
        calendar=value['engine']=='pc-app-calendar-v1'
        assert calendar==(service=='calendar')
        news=value['engine']=='pc-app-news-v1'
        assert news==(service=='news')
        assert type(value.get('autoRead',False)) is bool
        assert not value.get('autoRead',False) or news or library and service in {'knowledge','books'}
        assert (value['engine']=='pc-app-services-v2')==('location' in value)
        if 'location' in value:
            assert service=='weather'
            validate_location_profile(value['location'])
        assert runtime.get('host') is True and runtime.get('surface',{}).get('engine')=='p4-app-surface-v1'
        assert not runtime.get('vars') and not runtime.get('tick') and 'persist' not in runtime
        input_v1=runtime.get('input',{}).get('engine')=='p4-input-v1'
        actions=value['actions'];assert isinstance(actions,dict) and 1<=len(actions)<=(16 if input_v1 else 8)
        assert input_v1 or service!='soundscape' or not set(actions.values())&{'rain','sea','fire','insects'}
        assert all(re.fullmatch(r'host\.[a-z_]{1,32}',k) and v in OPERATIONS[service] for k,v in actions.items())
        assert len(set(actions.values()))==len(actions)
        if service=='todos':
            assert input_v1
            assert all(isinstance(runtime.get('input',{}).get('confirm',{}).get(a),str) and runtime['input']['confirm'][a].strip() for a,op in actions.items() if op=='toggle')
        if service=='weather' and set(actions.values())&{'choose_city','previous','next'}:
            assert {'choose_city','previous','next','refresh','todo'}<=set(actions.values())
            assert set('cityButton emptyCityButton pickerNote pickerConfirm pickerCancel refreshButton todoButton'.split())<=set(value['location']['text'])
        assert {r['on'] for r in runtime['transitions']}==set(actions)
        assert all(set(r)=={'from','on'} and r['from']=='*' for r in runtime['transitions'])
        content=value.get('content',[])
        if news:
            assert input_v1 and set(actions.values())==OPERATIONS['news']
            assert isinstance(content,list) and len(content)==3
            for topic in content:
                assert isinstance(topic,dict) and set(topic)=={'id','title','query'}
                assert isinstance(topic['id'],str) and re.fullmatch(r'[a-z-]{1,24}',topic['id'])
                for k,n in [('title',30),('query',120)]:
                    assert bounded_string(topic[k],n) and '://' not in topic[k]
            assert len({t['id'] for t in content})==3
            return []
        if calendar:
            assert input_v1 and set(actions.values())==OPERATIONS['calendar']
            assert isinstance(content,list) and 1<=len(content)<=8000
            previous=None
            for card in content:
                assert isinstance(card,dict) and set(card)=={'date','lunar','ganzhi','term','festivals','yi','ji'}
                date=datetime.date.fromisoformat(card['date'])
                assert card['date']==date.isoformat() and 1901<=date.year<=2100
                assert previous is None or date-previous==datetime.timedelta(days=1)
                previous=date
                assert bounded_string(card['lunar'],60) and bounded_string(card['ganzhi'],60)
                assert card['term']=='' or bounded_string(card['term'],30)
                for key in ('festivals','yi','ji'):
                    assert isinstance(card[key],list) and len(card[key])<=32 and all(bounded_string(s,90) for s in card[key])
            return []
        assert isinstance(content,list) and len(content)<=(100 if books else 1000 if library else 20)
        assert bool(content)==(service in {'journey','knowledge','books'})
        for card in content:
            keys={'id','title','text','source','url'} if service in {'knowledge','books'} else {'id','title','text','minutes'}
            if library:keys|={'category','license','revision'}
            assert isinstance(card,dict) and set(card)==keys
            assert isinstance(card['id'],str) and re.fullmatch(r'[a-z0-9_-]{1,32}',card['id'])
            for k,limit in [('title',120),('text',6000 if library else 600)]:assert bounded_string(card[k],limit)
            if library:
                assert bounded_string(card['category'],40)
                assert bounded_string(card['revision'],32)
                if books:
                    assert card['license']=='Original commentary' and card['category']!='全部书籍'
                else:
                    assert card['license'] in ('CC BY-SA 4.0','CC BY 4.0','CC0 1.0')
                assert len(re.findall(r'[\u3400-\u9fff]',card['text']))>=(800 if books else 300)
            if service in {'knowledge','books'}:
                assert bounded_string(card['source'],80) and bounded_string(card['url'],512)
                u=urlsplit(card['url']);assert u.scheme=='https' and u.hostname and '.' in u.hostname and not u.username and not u.password and u.port in (None,443)
            else:assert type(card['minutes']) is int and 1<=card['minutes']<=1440
        assert len({c['id'] for c in content})==len(content)
        if library:
            assert len({c['category'] for c in content})<=16
            assert {'random','category','previous','next'}<=set(actions.values())
        return []
    except (AssertionError,KeyError,TypeError,ValueError,AttributeError):return ['PC 应用需要受控服务、完整动作映射与有界内容；v2 位置预报需合法公网 GET 映射；不允许脚本或私有数据']
