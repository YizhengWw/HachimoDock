// Local testing builds: retain truthful dirty firmware identity, never inject keys or private CA.
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
const cwd=fileURLToPath(new URL('..',import.meta.url));
const target=process.argv[2];
if(!['macos','windows'].includes(target))throw Error('Specify macos or windows');
function run(command,args,env=process.env){
  const r=spawnSync(command,args,{cwd,env,stdio:'inherit'});
  if(r.error||r.status!==0)throw Error(`Build failed: ${command}`);
}
run(process.execPath,['scripts/check-public-build-env.mjs']);
run(process.execPath,['scripts/prepare-desktop-resources.mjs','--target',target,'--internal-development-firmware']);
run('./node_modules/.bin/vite',['build']);
run(process.execPath,['scripts/check-release-csp.mjs']);
const config=JSON.stringify({build:{beforeBuildCommand:''}});
if(target==='macos'){
  run('./node_modules/.bin/tauri',['build','--bundles','app','--ci','--config',config]);
  run(process.execPath,['scripts/sign-macos-local-app.mjs']);
  run(process.execPath,['scripts/bundle-macos-local-dmg.mjs'],{...process.env,PET_MANAGER_DMG_FLAVOR:'INTERNAL'});
  run(process.execPath,['scripts/sign-macos-local-app.mjs','--verify-only']);
}else{
  run('./node_modules/.bin/tauri',['build','--runner','cargo-xwin','--target','x86_64-pc-windows-msvc','--no-bundle','--ci','--config',config]);
  run(process.execPath,['scripts/build-windows-nsis-cross.mjs'],{...process.env,PET_MANAGER_WINDOWS_INSTALLER_FLAVOR:'INTERNAL'});
}
