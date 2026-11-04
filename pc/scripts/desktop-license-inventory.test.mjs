import test from 'node:test';
import assert from 'node:assert/strict';
import {mkdtempSync,rmSync,mkdirSync,copyFileSync,writeFileSync,existsSync,readFileSync,symlinkSync} from 'node:fs';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import {copyDependencyLicenses,copyNodeRuntimeLicense,dependencyLicenseResources,verifyDesktopLicenseInventory,assertDesktopLicenseCoverage} from './dependency-license-resources.mjs';
import {fileURLToPath} from 'node:url';
import {tmpdir} from 'node:os';
const root=fileURLToPath(new URL('../../',import.meta.url));
const temporary=mkdtempSync(join(tmpdir(),'hachimodock-license-'));
const digest=x=>createHash('sha256').update(x).digest('hex');
process.on('exit',()=>rmSync(temporary,{recursive:true,force:true}));
function fixture(name){const dir=join(temporary,name);mkdirSync(join(dir,'licenses'),{recursive:true});for(const {name} of dependencyLicenseResources)copyFileSync(join(root,'licenses',name),join(dir,'licenses',name));for(const name of ['pc/package-lock.json','pc/src-tauri/Cargo.lock','pc/src-tauri/bridge/packages/clawd-backend-service/package-lock.json']){mkdirSync(join(dir,name,'..'),{recursive:true});copyFileSync(join(root,name),join(dir,name));}return dir;}
test('all pinned original grants copied, including React/Lucide/Tauri and complete Node',()=>{const dst=join(temporary,'copy');const receipt=copyDependencyLicenses(join(root,'licenses'),dst);assert.equal(Object.keys(receipt).length,13);const text=readFileSync(join(dst,'DESKTOP-RUNTIME-LICENSES.txt'),'utf8');for(const value of ['npm:react@18.3.1','npm:lucide-react@0.511.0','cargo:tauri@2.11.1','Tauri Apps Contributors','Cole Bemis'])assert.ok(text.includes(value));assert.equal(digest(readFileSync(join(dst,'Node-26.7.0-LICENSE.txt'))),'e991d81497a85bb24fc6bffae0a3637a6accd6c6bc5ce1f2c5698bd555cf9d49');});
test('all original text boundaries keep original upstream byte digests; unresolved coverage is explicit',()=>{const result=verifyDesktopLicenseInventory(root);const data=JSON.parse(readFileSync(join(root,'licenses/DESKTOP-RUNTIME-DEPENDENCIES.json'),'utf8'));assert.equal(result.coverageComplete,data.unresolvedLicensePackages.length===0);assert.deepEqual(result.unresolvedLicensePackages,data.unresolvedLicensePackages);});
test('lock drift rejects before any destination write',()=>{const dir=fixture('stale');writeFileSync(join(dir,'pc/package-lock.json'),'{}');const dst=join(temporary,'reject-stale');assert.throws(()=>copyDependencyLicenses(join(dir,'licenses'),dst),/stale/);assert.equal(existsSync(dst),false);});
test('tampered upstream text rejects before destination writes',()=>{const dir=fixture('tampered');writeFileSync(join(dir,'licenses/Node-26.7.0-LICENSE.txt'),'not original');const dst=join(temporary,'reject-tamper');assert.throws(()=>copyDependencyLicenses(join(dir,'licenses'),dst),/integrity/);assert.equal(existsSync(dst),false);});
test('unknown or symlink Node cannot silently inherit reviewed vendor license',()=>{const file=join(temporary,'unknown-node');writeFileSync(file,'synthetic-only');const dst=join(temporary,'reject-node');assert.throws(()=>copyNodeRuntimeLicense(file,join(root,'licenses'),dst),/no reviewed/);assert.equal(existsSync(dst),false);const link=join(temporary,'node-link');symlinkSync(file,link);assert.throws(()=>copyNodeRuntimeLicense(link,join(root,'licenses'),dst),/no reviewed/);});

test('public packaging rejects honest incomplete coverage instead of returning false',()=>{assert.throws(()=>assertDesktopLicenseCoverage({coverageComplete:false,unresolvedLicensePackages:['cargo:missing@1'],requiredPackages:['cargo:missing@1'],packages:0,texts:[]}),/incomplete/);});
test('clearing unresolved or setting a true flag cannot hide a missing required grant',()=>{assert.throws(()=>assertDesktopLicenseCoverage({coverageComplete:true,unresolvedLicensePackages:[],requiredPackages:['cargo:present@1','cargo:missing@1'],packages:2,texts:[{package:'cargo:present@1'}]}),/grant missing/);});
test('empty required set cannot impersonate a complete public dependency inventory',()=>{assert.throws(()=>assertDesktopLicenseCoverage({coverageComplete:true,unresolvedLicensePackages:[],requiredPackages:[],packages:0,texts:[]}),/membership/);});
