#!/usr/bin/env python3
"""Offline grant collector (Python 3.11+). Supply cargo metadata --offline --locked --filter-platform for each target. No installs/network/builds. Missing grant files remain explicit, never inferred from an SPDX name."""
from pathlib import Path
import argparse,json,hashlib,tomllib,tarfile,re
P=argparse.ArgumentParser();P.add_argument('--root',type=Path,required=True);P.add_argument('--mac-metadata',type=Path,required=True);P.add_argument('--windows-metadata',type=Path,required=True);P.add_argument('--npm-cache',type=Path,required=True);a=P.parse_args();root=a.root.resolve()
def digest(b):return hashlib.sha256(b).hexdigest()
records={};missing=[];required=set();supplemental_provenance={}
lock_snapshots={n:(root/n).read_bytes() for n in ['pc/package-lock.json','pc/src-tauri/Cargo.lock','pc/src-tauri/bridge/packages/clawd-backend-service/package-lock.json']}
supplement_path=root/'licenses/reviewed-supplemental-grants.json'
assert supplement_path.is_file() and not supplement_path.is_symlink(), 'reviewed supplemental grant manifest required'
supplements=json.loads(supplement_path.read_text())
assert supplements['schema']==1 and supplements['notGenericFallback'] is True
locked_packages=tomllib.loads((root/'pc/src-tauri/Cargo.lock').read_text())['package']
locked_pins={(p['name'],p['version']):p.get('checksum') for p in locked_packages}
license_source=root/'licenses/supplemental'
assert license_source.is_dir() and not license_source.is_symlink()

def collect(identity,base,license_expr,scope,checksum=None):
 required.add(identity)
 base=base.resolve();picked=[]
 for p in sorted(base.iterdir()):
  if p.is_file() and not p.is_symlink() and p.name.lower().startswith(('license','licence','copying','notice','unlicense','authors')):picked.append(p)
 for d in ['LICENSES','licenses']:
  if (base/d).is_dir() and not (base/d).is_symlink():
   picked.extend(p for p in sorted((base/d).rglob('*')) if p.is_file() and not p.is_symlink())
 if not picked:
  reviewed=supplements['entries'].get(identity)
  if reviewed is None:missing.append(identity);return
  assert identity.startswith('cargo:'), 'supplement must name a reviewed exact crate version'
  name,version=identity[6:].rsplit('@',1)
  assert reviewed['crateArchiveSha256']==locked_pins[(name,version)], 'supplemental crate archive binding changed'
  assert reviewed['metadataLicenseExpression']==license_expr, 'supplemental license expression changed'
  vcs_path=base/'.cargo_vcs_info.json';actual_vcs=json.loads(vcs_path.read_text()) if vcs_path.exists() else None
  assert reviewed['publishedVcs']==actual_vcs, 'supplemental VCS identity changed'
  item=records.setdefault(identity,{'licenseExpression':license_expr,'scopes':[],'texts':[]})
  if scope not in item['scopes']:item['scopes'].append(scope)
  if item['texts']:return
  assert reviewed['texts'] and reviewed['evidence'], 'reviewed grant needs original context and provenance'
  for grant in reviewed['texts']:
   assert re.fullmatch(r'[0-9a-f]{64}\.txt',grant['file']) and grant['file']==grant['sha256']+'.txt'
   file=license_source/grant['file'];assert file.is_file() and not file.is_symlink() and file.resolve().parent==license_source.resolve()
   raw=file.read_bytes();assert len(raw)==grant['bytes'] and len(raw)<=512*1024 and digest(raw)==grant['sha256']
   item['texts'].append({'file':'reviewed-upstream/'+grant['upstreamPath'],'sha256':grant['sha256'],'bytes':len(raw),'text':raw.decode('utf-8')})
  supplemental_provenance[identity]=reviewed
  return
 item=records.setdefault(identity,{'licenseExpression':license_expr,'scopes':[],'texts':[]})
 if scope not in item['scopes']:item['scopes'].append(scope)
 if item['texts']:return
 for p in picked:
  assert p.resolve().is_relative_to(base) and p.stat().st_size<=2*1024*1024
  raw=p.read_bytes();rel=p.relative_to(base).as_posix()
  if checksum is not None:assert rel in checksum and digest(raw)==checksum[rel],identity+': license checksum mismatch'
  item['texts'].append({'file':rel,'sha256':digest(raw),'bytes':len(raw),'text':raw.decode('utf-8')})
for label,mp in [('macos',a.mac_metadata),('windows',a.windows_metadata)]:
 m=json.loads(mp.read_text());pkgs={p['id']:p for p in m['packages']};nodes={n['id']:n for n in m['resolve']['nodes']};todo=[m['resolve']['root']];seen=set()
 while todo:
  nid=todo.pop()
  if nid in seen:continue
  seen.add(nid);pkg=pkgs[nid];macro=any('proc-macro' in t['kind'] for t in pkg['targets'])
  if nid!=m['resolve']['root']:
   assert pkg['source'] and pkg['source'].startswith('registry+'),'non-registry dependency needs separate licensing review'
   base=Path(pkg['manifest_path']).parent
   if (base/'.cargo-checksum.json').exists(): checks=json.loads((base/'.cargo-checksum.json').read_text())
   else:
    archive=base.parent.parent.parent/'cache'/base.parent.name/(base.name+'.crate')
    locked=tomllib.loads((root/'pc/src-tauri/Cargo.lock').read_text())['package']
    pin=next(p['checksum'] for p in locked if p['name']==pkg['name'] and p['version']==pkg['version'])
    assert digest(archive.read_bytes())==pin, 'cached crate archive mismatch'
    with tarfile.open(archive,'r:gz') as tar:
     checks={'files':{str(Path(member.name).relative_to(base.name)):digest(tar.extractfile(member).read()) for member in tar if member.isfile()}}
   collect('cargo:'+pkg['name']+'@'+pkg['version'],base,pkg.get('license') or 'SEE_INCLUDED_TEXT',label+(':code-generator' if macro else ':runtime-normal-graph'),checks['files'])
  if not macro:
   for dep in nodes[nid]['deps']:
    if any(k['kind'] is None for k in dep['dep_kinds']):todo.append(dep['pkg'])
npm_sets=[('frontend-production-declaration-superset',root/'pc/package-lock.json',a.npm_cache),('bridge-runtime-production',root/'pc/src-tauri/bridge/packages/clawd-backend-service/package-lock.json',root/'pc/src-tauri/bridge/packages/clawd-backend-service')]
for npm_scope,lock_path,cache in npm_sets:
 lock=json.loads(lock_path.read_text())
 for rel,item in sorted(lock['packages'].items()):
  if not rel or item.get('dev') or item.get('link'):continue
  p=cache/rel
  if not (p/'package.json').is_file():
   if item.get('optional'):continue
   missing.append('npm:'+rel);required.add('npm:'+rel);continue
  meta=json.loads((p/'package.json').read_text());assert meta['version']==item['version'],rel+': cached version mismatch'
  collect('npm:'+meta['name']+'@'+meta['version'],p,meta.get('license') or item.get('license') or 'SEE_INCLUDED_TEXT',npm_scope)
# Missing upstream grants remain explicit: never fabricate a generic replacement.
texts=[];manifest=[];offset=0
for ident,item in sorted(records.items()):
 for text in item['texts']:
  header='\n===== '+ident+' / '+text['file']+' =====\n'
  payload=header+text['text'];texts.append(payload)
  manifest.append({'package':ident,'licenseExpression':item['licenseExpression'],'scopes':sorted(item['scopes']),'sourceFile':text['file'],'sourceSHA256':text['sha256'],'bytes':text['bytes'],'bundleOffset':offset+len(header.encode())})
  offset+=len(payload.encode())
body=''.join(texts).encode();meta={'schema':1,'purpose':'Unchanged upstream license/notice texts, not first-party relicensing','selection':'Cargo normal dependency graph excluding build/dev, with direct proc-macro grants labeled separately; npm production-declaration superset (not a claim all packages enter frontend bundle). Original alternative grants are retained; recipients retain upstream rights.','lockfiles':{n:digest((root/n).read_bytes()) for n in ['pc/package-lock.json','pc/src-tauri/Cargo.lock','pc/src-tauri/bridge/packages/clawd-backend-service/package-lock.json']},'packages':len(records),'requiredPackages':sorted(required),'reviewedSupplementalProvenance':supplemental_provenance,'coverageComplete':not missing,'unresolvedLicensePackages':sorted(set(missing)),'texts':manifest,'bundleSHA256':digest(body),'unreviewed':'Assets/fonts/firmware/optional legacy voice and future dependency versions need separate ownership/license review. No automatic whole-project license conclusion.'}
locked=tomllib.loads((root/'pc/src-tauri/Cargo.lock').read_text())['package']
pins={(x['name'],x['version']):x.get('checksum') for x in locked}
ids=sorted(set(x['package'] for x in manifest)|set(meta['unresolvedLicensePackages']))
meta['correspondingCargoSource']={}
for ident in ids:
 if ident.startswith('cargo:'):
  name,version=ident[6:].rsplit('@',1);meta['correspondingCargoSource'][ident]={'url':f'https://static.crates.io/crates/{name}/{name}-{version}.crate','sha256':pins[(name,version)]}
meta['notCoveredByThisCollection']=['MiLoCo/music service and asset ownership (parent review)','optional legacy voice trees','future bridge dependency changes require regeneration; current pruned ws production lock included','rights questions beyond supplying original grants, including upstream objc2 Apple SDK caveat, remain explicitly recorded']
assert all((root/n).read_bytes()==raw for n,raw in lock_snapshots.items()), 'dependency lock changed during license collection'
(root/'licenses/DESKTOP-RUNTIME-LICENSES.txt').write_bytes(body)
(root/'licenses/DESKTOP-RUNTIME-DEPENDENCIES.json').write_text(json.dumps(meta,indent=2)+'\n')
print(json.dumps({'packages':len(records),'texts':len(manifest),'bytes':len(body),'bundleSHA256':digest(body)}))
