// Cloud refresh of existing maintained works only; no user state or credentials.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {chromium} from 'playwright';

const pc=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..'),repo=path.dirname(pc);
const sourceRoot=process.env.PET_REFRESH_SOURCE?path.resolve(process.env.PET_REFRESH_SOURCE):path.join(pc,'builtin-clawpkgs');
const out=path.resolve(process.argv[2]||'');
if(!process.argv[2]||out===repo||out.startsWith(repo+path.sep)||fs.existsSync(path.join(out,'batch.json')))throw Error('Use a fresh output directory outside repository');
fs.mkdirSync(out,{recursive:true,mode:0o700});
const res=await fetch('https://115.191.67.48/api/v1/works?limit=50');
if(!res.ok)throw Error('Cannot read public catalog');
const before=await res.json();
if(before.nextOffset!=null)throw Error('Catalog pagination must be handled before refresh');
fs.writeFileSync(path.join(out,'before.json'),JSON.stringify(before,null,2));
const targets=before.items.filter(w=>w.official&&fs.existsSync(path.join(sourceRoot,w.id)));
if(targets.length!==21||targets.some(w=>['music-player','hachimi-notes','hachimi-agent','hachimi-desk'].includes(w.id)))throw Error('Unexpected publication scope');
const browser=await chromium.launch({headless:true,executablePath:process.env.PET_TEST_CHROMIUM||'/Applications/360Chrome.app/Contents/MacOS/360Chrome'});
const batch=[],report=[];
try{
 const page=await browser.newPage({viewport:{width:720,height:900},deviceScaleFactor:1});
 const errors=[];page.on('pageerror',e=>errors.push(e.message));
 await page.goto('http://127.0.0.1:4197/component-covers-preview.html');
 await page.locator('.cover-qa-grid article').first().waitFor();
 await page.addStyleTag({content:'.cover-qa{width:640px;max-width:none;margin:0;padding:0}.cover-qa-grid{display:block}.cover-qa article{width:640px;margin-bottom:24px}.component-cover{width:640px;height:480px;border-radius:0;box-shadow:none}'});
 await page.evaluate(async()=>{await document.fonts.ready;await Promise.all([...document.images].map(im=>im.decode().catch(()=>{})));});
 for(const previous of targets){
  const id=previous.id,source=path.join(sourceRoot,id),stage=path.join(out,'packages',id);
  const manifest=JSON.parse(fs.readFileSync(path.join(source,'component.json')));
  if(manifest.id!==id)throw Error('ID mismatch');
  const a=manifest.version.split('.').map(Number),b=previous.version.split('.').map(Number);
  const cmp=a.reduce((r,n,i)=>r||Math.sign(n-b[i]),0);
  if(cmp<0)throw Error('Local version is older: '+id);
  if(cmp===0)manifest.version=[b[0],b[1],b[2]+1].join('.');
  const allowed=n=>['component.json','negative-screen.json','buttons.json','runtime/widget.json','share.json','pc/extension.json','pc/application.json','assets/.keep','LICENSE.txt','THIRD_PARTY_NOTICES.txt'].includes(n)||/^assets\/[\w-]+\.png$/.test(n)||/^licenses\/[\w.-]+\.(txt|md)$/.test(n);
  function copy(dir,prefix=''){for(const e of fs.readdirSync(dir,{withFileTypes:true})){
   const rel=prefix+e.name;if(e.isSymbolicLink())throw Error('Symlink: '+rel);
   if(e.isDirectory())copy(path.join(dir,e.name),rel+'/');
   else if(allowed(rel)){fs.mkdirSync(path.dirname(path.join(stage,rel)),{recursive:true});fs.copyFileSync(path.join(dir,e.name),path.join(stage,rel));}
   else if(!['runtime/.keep','.DS_Store'].includes(rel))throw Error('Unexpected file: '+id+'/'+rel);
  }}copy(source);
  fs.writeFileSync(path.join(stage,'component.json'),JSON.stringify(manifest,null,2)+'\n');
  if(!fs.existsSync(path.join(stage,'LICENSE.txt'))){
   fs.copyFileSync(path.join(repo,'LICENSE'),path.join(stage,'LICENSE.txt'));
   fs.copyFileSync(path.join(repo,'THIRD_PARTY_NOTICES.md'),path.join(stage,'THIRD_PARTY_NOTICES.txt'));
   fs.mkdirSync(path.join(stage,'licenses'),{recursive:true});
   for(const n of fs.readdirSync(path.join(repo,'licenses')))if(/\.(txt|md)$/.test(n))fs.copyFileSync(path.join(repo,'licenses',n),path.join(stage,'licenses',n));
  }
  const coverLocator=page.locator(`article[data-id="${id}"] .component-cover`);
  await coverLocator.scrollIntoViewIfNeeded();
  await page.waitForTimeout(350);
  if(errors.length)throw Error(errors.join(';'));
  const box=await coverLocator.boundingBox();
  if(box.width!==640||box.height!==480)throw Error('Cover dimensions: '+id);
  // Element screenshots round fractional page coordinates outwards to 481 px.
  // Put the real cover at an integral viewport origin; capture exactly 640×480.
  await page.evaluate(()=>scrollTo(0,0));
  await coverLocator.evaluate(el=>{el.style.position='fixed';el.style.left='0px';el.style.top='0px';el.style.zIndex='9999';});
  const cover=await page.screenshot({clip:{x:0,y:0,width:640,height:480},path:path.join(out,id+'.png')});
  await coverLocator.evaluate(el=>el.removeAttribute('style'));
  if(cover.readUInt32BE(16)!==640||cover.readUInt32BE(20)!==480)throw Error('PNG dimensions: '+id);
  const zip=path.join(out,id+'.clawpkg');
  const packed=spawnSync('python3',['-m','zipfile','-c',zip,...fs.readdirSync(stage).sort()],{cwd:stage,encoding:'utf8'});
  if(packed.status)throw Error(packed.stderr);
  const data=fs.readFileSync(zip),license=fs.readFileSync(path.join(stage,'LICENSE.txt'),'utf8');
  batch.push({approve:true,publication:{
   requestId:'refresh-'+createHash('sha256').update(data).update(cover).digest('hex').slice(0,48),
   rightsConfirmed:true,privacyConfirmed:true,package:data.toString('base64'),covers:[cover.toString('base64')],
   metadata:{author:manifest.author||previous.author,summary:manifest.description.slice(0,160),
    description:manifest.description+'\n封面为当前资源包的 640×480 默认界面，不包含用户数据。建议使用 Pet Manager 0.1.131 及固件 0.7.90 或更新版本；联网和语音功能需要配置对应服务。',
    category:id.startsWith('hachimi-')?'hachimi':previous.category,
    license:license.includes('MIT License')?(id==='hachimi-learn'?'MIT / CC BY-SA 4.0 / OFL-1.1':'MIT / OFL-1.1'):previous.license,
    sourceUrl:previous.sourceUrl,releaseNotes:'同步最新组件界面与功能资源，更新完整 4:3 封面；保留原组件 ID，支持覆盖更新。'}
  }});
  report.push({id,name:manifest.name,from:previous.version,to:manifest.version,bytes:data.length,coverWidth:640,coverHeight:480});
  console.log(id,previous.version,'->',manifest.version,'ready');
 }
 fs.writeFileSync(path.join(out,'batch.json'),JSON.stringify(batch),{mode:0o600});
 fs.writeFileSync(path.join(out,'manifest.json'),JSON.stringify(report,null,2));
}finally{await browser.close();}
