// Produce real, clean default-state previews and staged official packages.
// Never reads user configuration or uploads anything. Pass an output directory
// outside the repository; run Vite locally on port 4177 first.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {chromium} from 'playwright';

const pc=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const repo=path.dirname(pc),out=path.resolve(process.argv[2]||'');
if(!process.argv[2]||out===repo||out.startsWith(repo+path.sep))throw Error('Choose an output directory outside the repository');
fs.mkdirSync(out,{recursive:true,mode:0o700});
const roots=fs.readdirSync(path.join(pc,'builtin-clawpkgs'),{withFileTypes:true}).filter(e=>e.isDirectory()).map(e=>path.join(pc,'builtin-clawpkgs',e.name));
const browser=await chromium.launch({headless:true,executablePath:process.env.PET_TEST_CHROMIUM||'/Applications/360Chrome.app/Contents/MacOS/360Chrome'});
const batch=[],reports=[];
try {
 const context=await browser.newContext({viewport:{width:400,height:300},deviceScaleFactor:2});
 const page=await context.newPage();
 const errors=[];page.on('pageerror',e=>errors.push(e.message));
 await page.route('**/__official-preview',route=>route.fulfill({contentType:'text/html',body:'<html lang="zh"><body style="margin:0;background:#fff"><div id="root"></div></body></html>'}));
 for(const source of roots){
  const manifest=JSON.parse(fs.readFileSync(path.join(source,'component.json')));
  const runtime=JSON.parse(fs.readFileSync(path.join(source,'runtime/widget.json')));
  const stage=path.join(out,manifest.id);fs.mkdirSync(stage,{recursive:true});
  const allowed=name=>['component.json','negative-screen.json','buttons.json','runtime/widget.json','share.json','pc/extension.json','assets/.keep'].includes(name)||/^assets\/[\w-]+\.png$/.test(name);
  function copy(dir,prefix=''){for(const e of fs.readdirSync(dir,{withFileTypes:true})){
   const rel=prefix+e.name;if(e.isSymbolicLink())throw Error('Symlink in official source');
   if(e.isDirectory())copy(path.join(dir,e.name),rel+'/');
   else if(allowed(rel)){fs.mkdirSync(path.dirname(path.join(stage,rel)),{recursive:true});fs.copyFileSync(path.join(dir,e.name),path.join(stage,rel));}
   else if(!['runtime/.keep','.DS_Store'].includes(rel))throw Error('Unexpected source file: '+manifest.id+'/'+rel);
  }}copy(source);
  fs.copyFileSync(path.join(repo,'LICENSE'),path.join(stage,'LICENSE.txt'));
  fs.copyFileSync(path.join(repo,'THIRD_PARTY_NOTICES.md'),path.join(stage,'THIRD_PARTY_NOTICES.txt'));
  fs.mkdirSync(path.join(stage,'licenses'),{recursive:true});
  for(const name of fs.readdirSync(path.join(repo,'licenses')))if(/\.(?:txt|md)$/.test(name))fs.copyFileSync(path.join(repo,'licenses',name),path.join(stage,'licenses',name));
  const sprites=[...(runtime.scene?.sprites||[]),...(runtime.surface?.images||[])].map(s=>({id:s.id,dataUrl:'data:image/png;base64,'+fs.readFileSync(path.join(stage,s.asset)).toString('base64'),frameWidth:s.frame_width,frameHeight:s.frame_height,frames:s.frames,fps:s.fps}));
  const component={...manifest,dashboard:JSON.parse(fs.readFileSync(path.join(stage,'negative-screen.json'))).dashboard||runtime.dashboard,scene:runtime.scene,sceneSprites:sprites,gameType:runtime.game?.type,dataSource:runtime.data?.source,mediaSource:runtime.media?.source,mediaSurface:runtime.surface,instrumentSource:runtime.instrument?.source};
  // No desktop bridge: default/empty states only, never actual user holdings,
  // tasks, host metrics, playlists, account identity or broadcasting content.
  await page.goto('http://127.0.0.1:4177/__official-preview');
  await page.evaluate(async component=>{
   const Refresh=(await import('/@react-refresh')).default;Refresh.injectIntoGlobalHook(window);window.$RefreshReg$=()=>{};window.$RefreshSig$=()=>t=>t;window.__vite_plugin_react_preamble_installed__=true;
   const React=(await import('/node_modules/.vite/deps/react.js')).default;
   const dom=await import('/node_modules/.vite/deps/react-dom_client.js');
   const {default:Preview}=await import('/src/component-center/DeviceScreenPreview.jsx');
   const link=document.createElement('link');link.rel='stylesheet';link.href='/src/styles.css';document.head.append(link);
   const style=document.createElement('style');style.textContent='#capture{width:400px;height:300px;padding:0;overflow:hidden}#capture>.component-device-screen{width:100%;height:100%;border-radius:0}';document.head.append(style);
   (dom.createRoot||dom.default.createRoot)(document.getElementById('root')).render(React.createElement('div',{id:'capture'},React.createElement(Preview,{component})));
   await document.fonts.ready;
  },component);
  await page.locator('#capture').waitFor();
  await page.waitForTimeout(700);
  if(errors.length)throw Error(manifest.id+': '+errors.join('; '));
  const cover=await page.locator('#capture').screenshot({path:path.join(out,manifest.id+'.png')});
  const zip=path.join(out,manifest.id+'.clawpkg');
  const files=fs.readdirSync(stage).sort();
  const packed=spawnSync('python3',['-m','zipfile','-c',zip,...files],{cwd:stage,encoding:'utf8'});if(packed.status!==0)throw Error(packed.stderr);
  const data=fs.readFileSync(zip);
  const category=manifest.kind==='game'?'game':(['wooden-fish','music-player'].includes(manifest.id)?'relax':(['tomato-clock','upcoming-todos','token-usage'].includes(manifest.id)?'productivity':manifest.id==='drink-reminder'?'life':'tool'));
  batch.push({approve:manifest.id!=='music-player',publication:{requestId:'official-'+createHash('sha256').update(data).update(cover).digest('hex').slice(0,48),rightsConfirmed:true,privacyConfirmed:true,package:data.toString('base64'),covers:[cover.toString('base64')],metadata:{author:'哈基米桌搭',summary:manifest.description.slice(0,160),description:manifest.description+'\n截图为组件真实默认预览，不包含用户数据。设备需安装支持相应能力的固件。'+(manifest.id==='music-player'?'\n此版本仅供管理员审核测试，音源尚未开放，不对外上架。':''),category,license:'HachimoDock Project License 1.0（非商业）',sourceUrl:'https://github.com/YizhengWw/HachimoDock',releaseNotes:'官方组件首次发布到社区，保留现有功能与组件身份。'}}});
  reports.push({id:manifest.id,version:manifest.version,source:path.relative(repo,source),status:manifest.id==='music-player'?'pending':'approve-requested',bytes:data.length});
  console.log(manifest.id,manifest.version,'preview and archive ready');
 }
 fs.writeFileSync(path.join(out,'batch.json'),JSON.stringify(batch),{mode:0o600});
 fs.writeFileSync(path.join(out,'manifest.json'),JSON.stringify(reports,null,2)+'\n',{mode:0o600});
} finally {await browser.close();}
