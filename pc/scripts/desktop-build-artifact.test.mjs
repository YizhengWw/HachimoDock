import test from 'node:test';
import assert from 'node:assert/strict';
import * as fs from 'node:fs';
import * as path from 'node:path';
import {tmpdir} from 'node:os';
import {fileURLToPath,pathToFileURL} from 'node:url';
import vm from 'node:vm';
import {randomUUID} from 'node:crypto';
import {desktopBuildPaths,macAppIdentity,peProductVersion,windowsExeIdentity,verifyBuildReceipt,readBuildReceipt,receiptArgument} from './desktop-build-artifact.mjs';
const scripts=path.dirname(fileURLToPath(import.meta.url));
const version='0.1.173';
function fixture(t){const root=fs.mkdtempSync(path.join(tmpdir(),'desktop-artifact-test-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));return root;}
function macApp(app,v=version,bytes='fresh-code'){
  fs.mkdirSync(path.join(app,'Contents/MacOS'),{recursive:true});
  fs.writeFileSync(path.join(app,'Contents/Info.plist'),JSON.stringify({CFBundleShortVersionString:v,CFBundleExecutable:'pet-manager-tauri'}));
  fs.writeFileSync(path.join(app,'Contents/MacOS/pet-manager-tauri'),bytes);
}
const readField=(p,k)=>JSON.parse(fs.readFileSync(p,'utf8'))[k];
const macIdentity=(p,v)=>macAppIdentity(p,v,{readField});
function pe(v=version){
  const b=Buffer.alloc(1536);b.write('MZ');b.writeUInt32LE(128,60);b.write('PE\0\0',128);
  b.writeUInt16LE(0x8664,132);b.writeUInt16LE(1,134);b.writeUInt16LE(240,148);
  const opt=152;b.writeUInt16LE(0x20b,opt);b.writeUInt32LE(0x1000,opt+128);b.writeUInt32LE(512,opt+132);
  const section=opt+240;b.writeUInt32LE(0x1000,section+12);b.writeUInt32LE(1024,section+16);b.writeUInt32LE(512,section+20);
  const root=512;
  for(const [dir,id,next] of [[0,16,0x80000020],[32,1,0x80000040],[64,1033,96]]){
    b.writeUInt16LE(1,root+dir+14);b.writeUInt32LE(id,root+dir+16);b.writeUInt32LE(next,root+dir+20);
  }
  b.writeUInt32LE(0x1080,root+96);b.writeUInt32LE(92,root+100);
  b.writeUInt16LE(92,root+128);b.writeUInt16LE(52,root+130);Buffer.from('VS_VERSION_INFO\0','utf16le').copy(b,root+134);
  const fixed=root+168;b.writeUInt32LE(0xfeef04bd,fixed);
  const [major,minor,patch]=v.split('.').map(Number);b.writeUInt32LE(major*65536+minor,fixed+16);b.writeUInt32LE(patch*65536,fixed+20);
  return b;
}
function runTemplate(name,root,{env={},args=[],spawn=()=>({status:0}),extra={}}={}){
  const repo=path.join(root,'repo'),cwd=path.join(repo,'pc');fs.mkdirSync(path.join(cwd,'src-tauri'),{recursive:true});
  fs.writeFileSync(path.join(cwd,'src-tauri/tauri.conf.json'),JSON.stringify({productName:'HachimiDock',version}));
  const filename=path.join(repo,'pc/scripts',name),url=pathToFileURL(filename).href;
  const source=fs.readFileSync(path.join(scripts,name),'utf8').replace(/^import[\s\S]*?from\s+['"][^'"]+['"];\s*/gm,'').replaceAll('import.meta.url',JSON.stringify(url));
  const processStub={platform:'darwin',arch:'arm64',argv:['node',filename,...args],execPath:'node',env,cwd:()=>cwd,exit:code=>{throw Error(`exit ${code}`);}};
  const context={...fs,...path,Buffer,URL,JSON,console:{log(){},error(){}},process:processStub,tmpdir:()=>root,
    fileURLToPath,randomUUID,spawnSync:spawn,verifyVoiceBundle:()=>({enabled:false}),
    desktopBuildPaths:o=>desktopBuildPaths({...o,env,cwd}),macAppIdentity:macIdentity,windowsExeIdentity,
    verifyBuildReceipt,readBuildReceipt,receiptArgument:a=>{const i=a.indexOf('--receipt');return i>=0?receiptArgument(a):env.PET_MANAGER_BUILD_RECEIPT||null;},...extra};
  vm.runInNewContext(source,context,{filename:name,timeout:2000});return {repo,cwd};
}

test('shared/relative Cargo target and explicit Windows triple never select stale default target',t=>{
  const root=fixture(t),cwd=path.join(root,'repo/pc'),repositoryRoot=path.join(root,'repo');
  for(const target of ['macos','windows']){
    const paths=desktopBuildPaths({repositoryRoot,target,cwd,env:{CARGO_TARGET_DIR:'../../shared'}});
    assert.equal(paths.targetDirectory,path.resolve(cwd,'../../shared'));
    assert.equal(paths.releaseRoot,path.join(paths.targetDirectory,...(target==='windows'?['x86_64-pc-windows-msvc']:[]),'release'));
    assert.ok(!paths.bundleRoot.includes('/src-tauri/target/'));
  }
  assert.equal(desktopBuildPaths({repositoryRoot,target:'macos',cwd,env:{CARGO_TARGET_DIR:'/warm',CARGO_BUILD_TARGET:'aarch64-apple-darwin'}}).releaseRoot,'/warm/aarch64-apple-darwin/release');
});
test('mac version and exact executable SHA receipt reject stale/wrong-path/tampered apps',t=>{
  const root=fixture(t),fresh=path.join(root,'shared/release/bundle/macos/HachimiDock.app'),old=path.join(root,'default/HachimiDock.app');
  macApp(fresh);macApp(old,'0.1.172','old-code');const identity=macIdentity(fresh,version);
  assert.throws(()=>macIdentity(old,version),/version/);
  assert.throws(()=>verifyBuildReceipt(identity,{...identity,path:old}),/receipt/);
  fs.writeFileSync(path.join(fresh,'Contents/MacOS/pet-manager-tauri'),'different-code');
  assert.throws(()=>verifyBuildReceipt(macIdentity(fresh,version),identity),/receipt/);
  assert.throws(()=>verifyBuildReceipt(identity,{...identity,sha256:'0'.repeat(64)}),/receipt/);
});
test('copied app must retain version/executable SHA even for explicit relocation',t=>{
  const root=fixture(t),a=path.join(root,'source.app'),b=path.join(root,'copy.app');macApp(a);fs.cpSync(a,b,{recursive:true});
  const proof=macIdentity(a,version),copy=macIdentity(b,version);
  assert.throws(()=>verifyBuildReceipt(copy,proof),/receipt/);assert.equal(verifyBuildReceipt(copy,proof,{relocated:true}).sha256,proof.sha256);
});
test('Windows uses actual RT_VERSION product version; unrelated string cannot impersonate it',t=>{
  const root=fixture(t),exe=path.join(root,'fresh.exe');fs.writeFileSync(exe,pe());
  assert.deepEqual(peProductVersion(fs.readFileSync(exe)),[0,1,173,0]);assert.equal(windowsExeIdentity(exe,version).version,version);
  const stale=pe('0.1.172');stale.write('0.1.173',1400);fs.writeFileSync(exe,stale);
  assert.throws(()=>windowsExeIdentity(exe,version),/product version/);
  assert.throws(()=>peProductVersion(Buffer.from('MZ invalid')),/PE/);
});
test('actual development entry passes the resolved target and per-build receipts to both platforms',t=>{
  for(const target of ['macos','windows']){
    const root=fixture(t),shared=path.join(root,'shared'),commands=[];
    runTemplate('build-development.mjs',root,{env:{CARGO_TARGET_DIR:shared},args:[target],spawn(command,args,options){
      commands.push({command,args,env:options.env});
      if(command.endsWith('/tauri')){
        const release=path.join(shared,...(target==='windows'?['x86_64-pc-windows-msvc']:[]),'release');fs.mkdirSync(release,{recursive:true});
        if(target==='macos')macApp(path.join(release,'bundle/macos/HachimiDock.app'));
        else fs.writeFileSync(path.join(release,'pet-manager-tauri.exe'),pe());
      }return {status:0};
    }});
    assert.ok(commands.every(c=>c.env.CARGO_TARGET_DIR===shared));
    const post=commands.filter(c=>c.args[0]?.includes(target==='macos'?'sign-macos':'build-windows-nsis'));
    assert.ok(post.length);assert.ok(post.every(c=>c.args.includes('--receipt')));
    if(target==='macos')assert.ok(post[0].args.includes('--output-receipt'));
    else assert.equal(post[0].env.PET_MANAGER_WINDOWS_EXE,path.join(shared,'x86_64-pc-windows-msvc/release/pet-manager-tauri.exe'));
  }
});
test('actual signer rejects wrong version before codesign even with an otherwise valid app',t=>{
  const root=fixture(t),shared=path.join(root,'shared');macApp(path.join(shared,'release/bundle/macos/HachimiDock.app'),'0.1.172');
  let invoked=0;assert.throws(()=>runTemplate('sign-macos-local-app.mjs',root,{env:{CARGO_TARGET_DIR:shared},spawn(){invoked++;return {status:0};}}),/version/);
  assert.equal(invoked,0);
});
test('actual signer retains unsigned receipt and emits a new signed SHA after codesign changes bytes',t=>{
  const root=fixture(t),shared=path.join(root,'shared'),app=path.join(shared,'release/bundle/macos/HachimiDock.app');macApp(app);
  const before=macIdentity(app,version),unsigned=path.join(root,'unsigned.json'),signed=path.join(root,'signed.json');fs.writeFileSync(unsigned,JSON.stringify(before));
  const original=fs.readFileSync(unsigned);let signedCalls=0;
  runTemplate('sign-macos-local-app.mjs',root,{env:{CARGO_TARGET_DIR:shared},args:[app,'--receipt',unsigned,'--output-receipt',signed],spawn(_command,args){
    if(args.includes('--force')){signedCalls++;fs.appendFileSync(path.join(app,'Contents/MacOS/pet-manager-tauri'),'-signature');}
    return {status:0,stdout:'designated => identifier "com.petmanager.desktop"',stderr:''};
  }});
  assert.equal(signedCalls,1);assert.deepEqual(fs.readFileSync(unsigned),original);
  const after=readBuildReceipt(signed);assert.notEqual(after.sha256,before.sha256);verifyBuildReceipt(macIdentity(app,version),after);
});
test('actual DMG template selects only current target and checks both staged/mounted identity with stubbed tools',t=>{
  const root=fixture(t),repo=path.join(root,'repo'),shared=path.join(root,'shared');
  const app=path.join(shared,'release/bundle/macos/HachimiDock.app');macApp(app);
  const stale=path.join(repo,'pc/src-tauri/target/release/bundle/macos/HachimiDock.app');macApp(stale,'0.1.172','old-code-must-not-be-used');
  fs.mkdirSync(path.join(repo,'pc/scripts'),{recursive:true});fs.writeFileSync(path.join(repo,'pc/scripts/sign-macos-local-app.mjs'),'stub');
  const helper=path.join(shared,'release/bundle/dmg/bundle_dmg.sh');fs.mkdirSync(path.dirname(helper),{recursive:true});fs.writeFileSync(helper,'stub');
  const receipt=path.join(root,'build-receipt.json');fs.writeFileSync(receipt,JSON.stringify(macIdentity(app,version)));
  const trace=[];let staging;
  runTemplate('bundle-macos-local-dmg.mjs',root,{env:{CARGO_TARGET_DIR:shared},args:['--receipt',receipt],spawn(command,args){
    trace.push({command,args});
    if(command==='/usr/bin/ditto'){assert.equal(args[0],app);fs.cpSync(args[0],args[1],{recursive:true});}
    if(command===helper){staging=args.at(-1);fs.writeFileSync(args.at(-2),'synthetic fixture, not a DMG');}
    if(command==='/usr/bin/hdiutil'&&args[0]==='attach')fs.cpSync(path.join(staging,'HachimiDock.app'),path.join(args[args.indexOf('-mountpoint')+1],'HachimiDock.app'),{recursive:true});
    return {status:0};
  }});
  assert.equal(trace.filter(x=>x.command==='node').length,3);
  assert.ok(trace.filter(x=>x.command==='node').every(x=>x.args.includes('--receipt')));
  assert.equal(fs.readFileSync(path.join(stale,'Contents/MacOS/pet-manager-tauri'),'utf8'),'old-code-must-not-be-used');
});
test('actual DMG template rejects a changed staged executable before creating or mounting output',t=>{
  const root=fixture(t),repo=path.join(root,'repo'),shared=path.join(root,'shared'),app=path.join(shared,'release/bundle/macos/HachimiDock.app');macApp(app);
  fs.mkdirSync(path.join(repo,'pc/scripts'),{recursive:true});fs.writeFileSync(path.join(repo,'pc/scripts/sign-macos-local-app.mjs'),'stub');
  const helper=path.join(shared,'release/bundle/dmg/bundle_dmg.sh');fs.mkdirSync(path.dirname(helper),{recursive:true});fs.writeFileSync(helper,'stub');
  const receipt=path.join(root,'receipt.json');fs.writeFileSync(receipt,JSON.stringify(macIdentity(app,version)));
  const tools=[];
  assert.throws(()=>runTemplate('bundle-macos-local-dmg.mjs',root,{env:{CARGO_TARGET_DIR:shared},args:['--receipt',receipt],spawn(command,args){
    tools.push(command);
    if(command==='/usr/bin/ditto'){fs.cpSync(args[0],args[1],{recursive:true});fs.appendFileSync(path.join(args[1],'Contents/MacOS/pet-manager-tauri'),'tampered');}
    return {status:0};
  }}),/receipt/);
  assert.ok(!tools.includes(helper));assert.ok(!tools.includes('/usr/bin/hdiutil'));
});
test('existing signed output receipt is never overwritten and rejects before signing',t=>{
  const root=fixture(t),shared=path.join(root,'shared'),app=path.join(shared,'release/bundle/macos/HachimiDock.app');macApp(app);
  const output=path.join(root,'existing-receipt.json');fs.writeFileSync(output,'retain old evidence');
  let tools=0;assert.throws(()=>runTemplate('sign-macos-local-app.mjs',root,{env:{CARGO_TARGET_DIR:shared},args:[app,'--output-receipt',output],spawn(){tools++;return {status:0};}}),/output receipt/);
  assert.equal(tools,0);assert.equal(fs.readFileSync(output,'utf8'),'retain old evidence');
});
test('relocation cannot turn an incomplete receipt into build evidence',t=>{
  const root=fixture(t),app=path.join(root,'app');macApp(app);const identity=macIdentity(app,version),receipt={...identity};delete receipt.path;
  assert.throws(()=>verifyBuildReceipt(identity,receipt,{relocated:true}),/receipt/);
});
