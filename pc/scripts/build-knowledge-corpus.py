#!/usr/bin/env python3
"""Build-time import of openly licensed text. No fetcher/code ships in the package.

Keeps complete sentences from the beginning of the source; never pads short
articles, invents citations, or presents machine-extracted text as original work.
Raw API responses and rejection report are retained outside the published package.
"""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import subprocess
import threading
import time
from urllib.parse import urlencode
import mwparserfromhell
from opencc import OpenCC

SIMPLIFIED=OpenCC('t2s')

API='https://zh.wikipedia.org/w/api.php'
LOCK=threading.Lock()
LAST=0.0
def han(s):return len(re.findall(r'[\u3400-\u9fff]',s))
def cache_file(params,cache):
    return cache/(hashlib.sha256(json.dumps(params,sort_keys=True).encode()).hexdigest()+'.json')
def request(params,cache):
    global LAST
    file=cache_file(params,cache)
    if file.exists():return json.loads(file.read_text())
    for attempt in range(3):
        with LOCK:
            wait=max(0,.35-(time.monotonic()-LAST))
            if wait:time.sleep(wait)
            LAST=time.monotonic()
        result=subprocess.run(['curl','--compressed','--fail','--silent','--show-error','--max-time','35','-A','HachimoDockKnowledge/1.0 (offline educational reader; https://www.mediawiki.org/wiki/API:Etiquette)',API+'?'+urlencode({'format':'json','formatversion':2,'maxlag':5,**params})],capture_output=True)
        try:
            value=json.loads(result.stdout.decode('utf-8'))
            if result.returncode or 'error' in value:raise ValueError('request unavailable')
            file.write_text(json.dumps(value,ensure_ascii=False),encoding='utf-8')
            return value
        except (ValueError,UnicodeError,json.JSONDecodeError):
            if attempt==2:raise ValueError('source unavailable')
            time.sleep(2**attempt)

def extract(raw):
    # TextExtracts excludes HTML; remove section labels, not factual sentences.
    raw=re.split(r'\n(?:==+\s*)?(?:参考文献|参考资料|參考文獻|參考資料|外部链接|外部連結|延伸阅读|延伸閱讀|参见|參見)(?:\s*==+)?\s*\n',raw)[0]
    pieces=[]
    for line in raw.splitlines():
        line=line.strip()
        if not line or line.startswith('=') or han(line)<12 or line.endswith(('：',':')):continue
        # Formula fragments and maintenance notices do not make readable cards.
        if any(s in line for s in ['\\displaystyle','\\begin','\\frac','{\\','本条目','本條目','消歧义','消歧義','可能指','下列各项','下列各項','如下表','如下图','如下圖','下式','上式','式中','参见下','參見下']):continue
        line=re.sub(r'\[\d+\]','',line)
        pieces.append(line)
    raw=' '.join(pieces)
    sentences=re.findall(r'.+?(?:[。！？](?:[”’」』])?|$)',raw)
    selected=[]
    for sentence in sentences:
        sentence=sentence.strip()
        if not sentence:continue
        if sentence in selected:continue
        if len((' '.join(selected+[sentence])).encode())>5500:break
        selected.append(sentence)
        if han(' '.join(selected))>=360:break
    text=' '.join(selected)
    if han(text)<300:raise ValueError('fewer than 300 Chinese characters')
    if not re.search(r'[。！？][”’」』]?$',text):raise ValueError('incomplete ending')
    if len(text.encode())>6000:raise ValueError('too long')
    return text

def query(topic):
    return {'action':'query','prop':'extracts|info|revisions|pageprops','titles':topic,'redirects':1,'converttitles':1,'variant':'zh-cn','explaintext':1,'exsectionformat':'plain','rvprop':'ids|timestamp','inprop':'url','ppprop':'disambiguation'}

def prewarm(names,cache):
    pending=[n for n in names if not cache_file({**query(n),'exintro':1},cache).exists()]
    for start in range(0,len(pending),20):
        group=pending[start:start+20]
        try:
            data=request({**query('|'.join(group)),'exintro':1,'exlimit':20},cache)
        except ValueError:
            continue # individual bounded retries remain available
        q=data.get('query',{})
        aliases={p['from']:p['to'] for field in ('normalized','converted','redirects') for p in q.get(field,[])}
        pages={p['title']:p for p in q.get('pages',[])}
        for name in group:
            title=name;visited=set()
            while title in aliases and title not in visited:
                visited.add(title);title=aliases[title]
            p=pages.get(title)
            if p is not None and ('extract' in p or p.get('missing') or 'disambiguation' in p.get('pageprops',{})):
                cache_file({**query(name),'exintro':1},cache).write_text(json.dumps({'query':{'pages':[p]}},ensure_ascii=False),encoding='utf-8')

def revision_query(topic):
    return {'action':'query','prop':'revisions|pageprops','titles':topic,'redirects':1,'converttitles':1,'rvprop':'ids|content','rvslots':'main','ppprop':'disambiguation'}

def prewarm_revisions(names,cache):
    pending=[]
    for name in names:
        intro=cache_file({**query(name),'exintro':1},cache)
        if intro.exists():
            pages=json.loads(intro.read_text()).get('query',{}).get('pages',[])
            if pages and (pages[0].get('missing') or 'disambiguation' in pages[0].get('pageprops',{})):continue
            try:
                extract(pages[0].get('extract',''))
                continue
            except (ValueError,IndexError):pass
        if cache_file({**query(name),'exchars':14000},cache).exists() or cache_file({**query(name),'exchars':1200},cache).exists():continue
        if not cache_file(revision_query(name),cache).exists():pending.append(name)
    # Revisions supports content for multiple titles, unlike full TextExtracts.
    # This avoids hundreds of expensive single-page rendering requests.
    for start in range(0,len(pending),20):
        group=pending[start:start+20]
        data=request(revision_query('|'.join(group)),cache);q=data.get('query',{})
        aliases={p['from']:p['to'] for field in ('normalized','converted','redirects') for p in q.get(field,[])}
        pages={p['title']:p for p in q.get('pages',[])}
        for name in group:
            title=name;visited=set()
            while title in aliases and title not in visited:
                visited.add(title);title=aliases[title]
            if title in pages:
                cache_file(revision_query(name),cache).write_text(json.dumps({'query':{'pages':[pages[title]]}},ensure_ascii=False),encoding='utf-8')

def prose(wiki):
    # Conservative plain prose only. Do not silently delete a formula or an
    # inline template and turn the remaining fragment into a factual statement.
    wiki=re.split(r'\n==+\s*(?:参考文献|参考资料|參考文獻|參考資料|外部链接|外部連結|延伸阅读|延伸閱讀|参见|參見|References|External links)\s*==+\s*\n',wiki,flags=re.I)[0]
    code=mwparserfromhell.parse(wiki)
    for tag in reversed(code.filter_tags()):
        name=str(tag.tag).lower()
        if name in ('ref','references'):code.replace(tag,'')
        elif name in ('math','table','gallery','timeline','score'):code.replace(tag,' OMITTEDMARKUP ')
    for link in reversed(code.filter_wikilinks()):
        if re.match(r'(?i)^(file|image|category|文件|檔案|分类|分類):',str(link.title)):code.replace(link,'')
    for template in reversed(code.filter_templates(recursive=False)):
        code.replace(template,' OMITTEDMARKUP ')
    for heading in reversed(code.filter_headings()):
        code.replace(heading,'\n\n')
    raw=str(code)
    paragraphs=[]
    for paragraph in re.split(r'\n\s*\n',raw):
        if 'OMITTEDMARKUP' in paragraph or '-{' in paragraph or re.search(r'(?m)^[*#:;{|!]',paragraph):continue
        paragraphs.append(mwparserfromhell.parse(paragraph).strip_code())
    return SIMPLIFIED.convert('\n'.join(paragraphs))

def article(topic,cache):
    common=query(topic)
    first=request({**common,'exintro':1},cache)
    pages=first.get('query',{}).get('pages',[])
    if len(pages)!=1 or pages[0].get('missing'):raise ValueError('missing source')
    p=pages[0]
    if 'disambiguation' in p.get('pageprops',{}):raise ValueError('disambiguation')
    try:text=extract(p.get('extract',''))
    except ValueError:
        # Earlier cached requests used an overlarge value which MediaWiki capped
        # to 1200. Reuse their valid response without repeating the network read.
        old=cache_file({**common,'exchars':14000},cache)
        full=cache_file({**common,'exchars':1200},cache)
        if old.exists() or full.exists():
            p=json.loads((old if old.exists() else full).read_text())['query']['pages'][0]
            text=extract(p.get('extract',''))
        else:
            p=request(revision_query(topic),cache)['query']['pages'][0]
            text=extract(prose(p.get('revisions',[{}])[0].get('slots',{}).get('main',{}).get('content','')))
    revision=p.get('revisions',[{}])[0].get('revid')
    if not revision:raise ValueError('missing revision')
    return {'id':'wiki-'+str(p['pageid']),'title':topic,'text':text,'source':'中文维基百科贡献者',
            'url':f"https://zh.wikipedia.org/w/index.php?oldid={revision}",'license':'CC BY-SA 4.0','revision':str(revision)}

def main():
    a=argparse.ArgumentParser();a.add_argument('--cache',type=Path,required=True);a.add_argument('--output',type=Path,required=True)
    args=a.parse_args();args.cache.mkdir(parents=True,exist_ok=True)
    folder=Path(__file__).resolve().parents[1]/'content/knowledge'
    topics=json.loads((folder/'topics.json').read_text())
    excluded=json.loads((folder/'exclusions.json').read_text())
    for category,extra in json.loads((folder/'supplements.json').read_text()).items():
        topics[category]+='|'+extra
    result=[];rejected=[];seen=set();summary={}
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        for category,raw in topics.items():
            chosen=[]
            # Ordered results keep a reproducible editorial selection despite concurrent fetch.
            names=[n for n in dict.fromkeys(raw.split('|')) if n not in excluded]
            rejected.extend({'category':category,'title':n,'reason':excluded[n]} for n in raw.split('|') if n in excluded)
            prewarm(names,args.cache)
            prewarm_revisions(names,args.cache)
            def get(topic):
                try:return topic,article(topic,args.cache),None
                except Exception as e:return topic,None,str(e)
            for topic,item,error in pool.map(get,names):
                if error:rejected.append({'category':category,'title':topic,'reason':error});continue
                if len(chosen)>=100:continue
                if item['id'] in seen:
                    rejected.append({'category':category,'title':topic,'reason':'duplicate resolved article'});continue
                seen.add(item['id']);item['category']=category;chosen.append(item)
            result.extend(chosen);summary[category]=len(chosen)
            print(json.dumps({'category':category,'count':len(chosen),'minimumHan':min([han(c['text']) for c in chosen],default=0)},ensure_ascii=False),flush=True)
    report={'categories':summary,'total':len(result),'rejected':rejected,'license':'CC BY-SA 4.0','method':'source excerpts; paragraph whitespace normalized; complex markup/formula paragraphs omitted when plain text is unavailable; traditional-to-simplified conversion for raw revision prose; no generated padding'}
    (args.cache/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    # Keep a clearly labelled candidate for review; only complete corpus becomes production input.
    (args.cache/'candidate.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    if len(topics)!=10 or any(n!=100 for n in summary.values()):raise SystemExit('Incomplete corpus; production catalog NOT replaced. Review report and add suitable topics.')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(result,ensure_ascii=False,separators=(',',':'))+'\n',encoding='utf-8')
    print('COMPLETE: 10 × 100 unique articles, each >=300 Chinese characters; attribution retained',flush=True)
if __name__=='__main__':main()
