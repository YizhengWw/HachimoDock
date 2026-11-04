// Local testing builds: retain truthful dirty firmware identity, use only public build inputs.
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {readFileSync,writeFileSync} from 'node:fs';
import {resolve,join} from 'node:path';
import {randomUUID} from 'node:crypto';
import {desktopBuildPaths,macAppIdentity,windowsExeIdentity} from './desktop-build-artifact.mjs';
const cwd=fileURLToPath(new URL('..',import.meta.url));
const target=process.argv[2];
if(!['macos','windows'].includes(target))throw Error('Specify macos or windows');
const repositoryRoot=resolve(cwd,'..');
const paths=desktopBuildPaths({repositoryRoot,target,cwd});
// Resolve relative Cargo target directories once, at the build working dir.
const buildEnv={...process.env,CARGO_TARGET_DIR:paths.targetDirectory};
const version=JSON.parse(readFileSync(join(cwd,'src-tauri/tauri.conf.json'),'utf8')).version;
function receiptPath(){return join(paths.releaseRoot,`desktop-build-artifact-${randomUUID()}.json`);}
function receipt(identity){
  const path=receiptPath();
  writeFileSync(path,JSON.stringify(identity,null,2)+'\n',{flag:'wx'});return path;
}
function run(command,args,env=buildEnv){
  const r=spawnSync(command,args,{cwd,env,stdio:'inherit'});
  if(r.error||r.status!==0)throw Error(`Build failed: ${command}`);
}
run(process.execPath,['scripts/check-public-build-env.mjs']);
run(process.execPath,['scripts/prepare-desktop-resources.mjs','--target',target,'--development-firmware']);
run('./node_modules/.bin/vite',['build']);
run(process.execPath,['scripts/check-release-csp.mjs']);
const config=JSON.stringify({build:{beforeBuildCommand:''}});
if(target==='macos'){
  run('./node_modules/.bin/tauri',['build',...(buildEnv.CARGO_BUILD_TARGET?['--target',buildEnv.CARGO_BUILD_TARGET]:[]),'--bundles','app','--ci','--config',config]);
  const app=join(paths.bundleRoot,'macos/HachimiDock.app');
  const unsignedProof=receipt(macAppIdentity(app,version));
  const proof=receiptPath(); // codesign changes Mach-O signature bytes; retain both identities
  run(process.execPath,['scripts/sign-macos-local-app.mjs',app,'--receipt',unsignedProof,'--output-receipt',proof]);
  run(process.execPath,['scripts/bundle-macos-local-dmg.mjs','--receipt',proof],{...buildEnv,PET_MANAGER_DMG_FLAVOR:'DEVELOPMENT'});
  run(process.execPath,['scripts/sign-macos-local-app.mjs','--verify-only',app,'--receipt',proof]);
}else{
  run('./node_modules/.bin/tauri',['build','--runner','cargo-xwin','--target','x86_64-pc-windows-msvc','--no-bundle','--ci','--config',config]);
  const executable=join(paths.releaseRoot,'pet-manager-tauri.exe');
  const proof=receipt(windowsExeIdentity(executable,version));
  run(process.execPath,['scripts/build-windows-nsis-cross.mjs','--receipt',proof],{...buildEnv,PET_MANAGER_WINDOWS_EXE:executable,PET_MANAGER_WINDOWS_INSTALLER_FLAVOR:'DEVELOPMENT'});
}
