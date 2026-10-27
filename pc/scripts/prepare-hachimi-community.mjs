// Stage the eight retained resource packages with real 640x480 runtime previews.
// No user configuration, microphone, task dispatch, or network publication.
import fs from 'node:fs';
import path from 'node:path';
import {createHash} from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {chromium} from 'playwright';
const [sourceArg,outArg]=process.argv.slice(2);
if(!sourceArg||!outArg)throw Error('Usage: source-directory output-directory');
const source=path.resolve(sourceArg),out=path.resolve(outArg);
if(out===source||out.startsWith(source+path.sep)||fs.existsSync(out))throw Error('Choose a new output directory outside the packages');
const ids=['home','garden','travel','weather','notes','learn','sounds','break'].map(n=>'hachimi-'+n);
fs.mkdirSync(out,{recursive:true,mode:0o700});
const browser=await chromium.launch({headless:true,executablePath:process.env.PET_TEST_BROWSER||'/Applications/360Chrome.app/Contents/MacOS/360Chrome'});
try{
 const page=await browser.newPage({viewport:{width:1440,height:1100},deviceScaleFactor:1}),errors=[];
 page.on('pageerror',e=>errors.push(e.message));
 await page.goto(process.env.PET_APP_PREVIEW_URL||'http://127.0.0.1:4192/app-surface-preview.html');
 await page.locator('article').last().waitFor();
 const fixtures=await page.evaluate(()=>fetch('/__app_fixtures').then(r=>r.json()));
 if(ids.some(id=>!fixtures.some(f=>f.manifest.id===id)))throw Error('Missing retained component preview');
 await page.evaluate(()=>document.fonts.ready);
 const batch=[],reports=[];
 for(const id of ids){
  const dir=path.join(source,id),manifest=JSON.parse(fs.readFileSync(path.join(dir,'component.json'))),runtime=JSON.parse(fs.readFileSync(path.join(dir,'runtime/widget.json')));
  const fixture=fixtures.find(f=>f.manifest.id===id);
  if(manifest.id!==id||JSON.stringify(runtime)!==JSON.stringify(fixture?.runtime))throw Error('Preview/package mismatch: '+id);
  const files=[];
  function walk(root,prefix=''){for(const e of fs.readdirSync(root,{withFileTypes:true})){
   const name=prefix+e.name;
   if(e.isSymbolicLink())throw Error('No symlinks');
   if(e.isDirectory()){walk(path.join(root,e.name),name+'/');continue;}
   if(!/^(?:component\.json|buttons\.json|negative-screen\.json|share\.json|runtime\/widget\.json|pc\/application\.json|LICENSE\.txt|THIRD_PARTY_NOTICES\.txt|assets\/[\w-]+\.png|licenses\/[\w.-]+\.(?:txt|md))$/.test(name))throw Error('Unexpected file: '+name);
   files.push(name);
  }}walk(dir);
  const screen=page.locator(`[data-id="${id}"] .screen`);
  await screen.scrollIntoViewIfNeeded();await page.waitForTimeout(100);
  const cover=await screen.screenshot({path:path.join(out,id+'.png')});
  if(errors.length)throw Error(errors.join('; '));
  const zip=path.join(out,id+'.clawpkg');
  // zipfile's CLI strips parent directories from individual paths. Supply
  // validated top-level entries so runtime/assets/licenses keep their paths.
  execFileSync('python3',['-m','zipfile','-c',zip,...fs.readdirSync(dir).sort()],{cwd:dir});
  const data=fs.readFileSync(zip),pending=id==='hachimi-notes';
  batch.push({approve:!pending,publication:{requestId:'hachimi-'+createHash('sha256').update(data).update(cover).digest('hex').slice(0,48),rightsConfirmed:true,privacyConfirmed:true,package:data.toString('base64'),covers:[cover.toString('base64')],metadata:{author:'哈基米桌搭',summary:manifest.description.slice(0,160),description:manifest.description+'\n640×480 统一 PetUI 资源包；截图是资源包的默认运行预览。需要支持应用界面、触屏交互和存档的新固件；宿主服务需要新版 Pet Manager 保持连接。'+(pending?'\n录音转写实测延后，本条保留待审核，不作完整验收通过发布。':''),category:'hachimi',license:'MIT；字体 OFL（见包内声明）',sourceUrl:'https://github.com/YizhengWw/HachimoDock',releaseNotes:'应用级界面、图标按钮与触屏操作；完善本地状态、存档及受控宿主服务。'}}});
  reports.push({id,version:manifest.version,bytes:data.length,coverResolution:[640,480],status:pending?'pending':'approval-requested'});
 }
 fs.writeFileSync(path.join(out,'batch.json'),JSON.stringify(batch),{mode:0o600});
 fs.writeFileSync(path.join(out,'manifest.json'),JSON.stringify(reports,null,2)+'\n',{mode:0o600});
 console.log(JSON.stringify(reports));
}finally{await browser.close();}
