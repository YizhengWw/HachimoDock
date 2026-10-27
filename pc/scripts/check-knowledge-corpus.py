#!/usr/bin/env python3
"""Offline acceptance of the released corpus. Never fetches or alters content."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
from urllib.parse import urlsplit,parse_qs

def check(path):
    raw=path.read_bytes();cards=json.loads(raw);errors=[];warnings=[]
    expected='物理 化学 生物 历史 人文 经济 心理 健康 天文 地理'.split()
    counts=Counter(c.get('category') for c in cards)
    if counts!={k:100 for k in expected}:errors.append('expected ten categories with 100 items each')
    if len(cards)!=1000 or len({c['id'] for c in cards})!=1000:errors.append('duplicate/missing IDs')
    if len({c['text'] for c in cards})!=1000:errors.append('duplicate text')
    lengths=[]
    for c in cards:
        t=c['text'];n=len(re.findall(r'[\u3400-\u9fff]',t));lengths.append(n)
        if n<300 or len(t.encode())>6000:errors.append(c['id']+': length')
        if any(ord(ch)<32 or 127<=ord(ch)<=159 for ch in t):errors.append(c['id']+': control character')
        if not re.search(r'[。！？][”’」』]?$',t):errors.append(c['id']+': incomplete sentence')
        if re.search(r'OMITTEDMARKUP|\{\{|\}\}|<ref|</|\\displaystyle|\[\[|\]\]',t):errors.append(c['id']+': leftover markup')
        if c['license']!='CC BY-SA 4.0' or c['source']!='中文维基百科贡献者':errors.append(c['id']+': attribution')
        u=urlsplit(c['url'])
        if u.scheme!='https' or u.hostname!='zh.wikipedia.org' or parse_qs(u.query).get('oldid')!=[c['revision']]:errors.append(c['id']+': source revision')
        sentences=[s.strip() for s in t.split('。') if len(s.strip())>20]
        if len(sentences)!=len(set(sentences)):warnings.append(c['id']+': repeated source sentence; review')
        if re.search(r'维基百科|維基百科|本条目|本條目|待补充|待補充|来源请求|來源請求',t):warnings.append(c['id']+': editorial wording; review')
    return {'ok':not errors,'count':len(cards),'categories':dict(counts),'minimumHan':min(lengths),'maximumHan':max(lengths),'totalHan':sum(lengths),'utf8Bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest(),'errors':errors,'reviewWarnings':warnings}
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('catalog',type=Path);args=p.parse_args()
    report=check(args.catalog);print(json.dumps(report,ensure_ascii=False,indent=2));raise SystemExit(0 if report['ok'] else 1)
