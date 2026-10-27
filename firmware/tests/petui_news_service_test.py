"""News package declaration parity/negative cases; never calls a paid model."""
from pathlib import Path
import copy,json,sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pc/skills/petui/scripts'))
from app_services import application_errors

def test_bounded_news_declaration_rejects_extra_actions_urls_and_categories():
    root=ROOT/'pc/builtin-clawpkgs/daily-news'
    d=json.loads((root/'pc/application.json').read_text())
    r=json.loads((root/'runtime/widget.json').read_text())
    assert not application_errors(d,r)
    for field,value in [('engine','pc-app-services-v1'),('service','books'),('autoRead','yes'),('url','https://example.org')]:
        bad=copy.deepcopy(d);bad[field]=value;assert application_errors(bad,r)
    bad=copy.deepcopy(d);bad['content'].append(bad['content'][0]);assert application_errors(bad,r)
    bad=copy.deepcopy(d);bad['content'][0]['query']='https://private.invalid';assert application_errors(bad,r)
    bad=copy.deepcopy(d);bad['content'][1]['id']=bad['content'][0]['id'];assert application_errors(bad,r)
    bad=copy.deepcopy(d);bad['actions']['host.refresh']='execute';assert application_errors(bad,r)
    bad=copy.deepcopy(r);del bad['input'];assert application_errors(d,bad)
