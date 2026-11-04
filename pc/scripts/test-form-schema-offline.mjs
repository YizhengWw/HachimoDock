// Execute actual strict form/variable/device-string schemas without a desktop host.
import {readFileSync,writeFileSync,mkdtempSync,mkdirSync} from 'node:fs';
import {join,resolve,dirname} from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
const [output,label='run']=process.argv.slice(2);if(!output||!/^[\w-]+$/.test(label))throw Error('Provide evidence directory and safe label');
const evidence=resolve(output),pc=resolve(dirname(fileURLToPath(import.meta.url)),'..'),source=join(pc,'src-tauri/src');mkdirSync(evidence,{recursive:true});
const root=mkdtempSync(join(evidence,'form-schema-')),raw=readFileSync(join(source,'clawpkg.rs'),'utf8');
function section(from,to){const a=raw.indexOf(from),b=raw.indexOf(to,a+from.length);if(a<0||b<0)throw Error('Production variable validator boundary changed');return raw.slice(a,b);}
const constants=section('const P4_WIDGET_MAX_VARS:','const COMPONENT_BUTTON_MAX_BINDINGS:');
const variables=section('fn valid_p4_var_name(','fn validate_p4_widget_vars_shape(');
const deviceFiles=section('fn validate_p4_device_json_strings(','fn valid_p4_var_name(');
const stringContract=join(pc,'skills/petui/contracts/device-json-strings-v1.json');
const stringTests=join(pc,'scripts/fixtures/device-json-tests.rs');
writeFileSync(join(root,'Cargo.toml'),'[package]\nname="form-schema-offline-check"\nversion="0.0.0"\nedition="2021"\n[lib]\npath="lib.rs"\n[dependencies]\nserde={version="1",features=["derive"]}\nserde_json={version="1",features=["preserve_order"]}\n[profile.dev]\ndebug=0\nincremental=false\n');
writeFileSync(join(root,'lib.rs'),`#![allow(dead_code)]
#[path=${JSON.stringify(join(source,'app_variable_owners.rs'))}] mod app_variable_owners;
mod clawpkg{use std::collections::HashMap;${constants}\n${deviceFiles}\n${variables}
 #[cfg(test)] #[path=${JSON.stringify(stringTests)}] mod device_json_tests;
}
mod component_extension{
 #[path=${JSON.stringify(join(source,'app_form.rs'))}] pub mod app_form;
 // These transport-level paths must be unreachable in the selected pure test.
 pub fn validate_app_runtime(_: &serde_json::Value)->Result<(),String>{panic!("Full runtime validation is outside pure schema harness");}
 pub fn validate_app_surface(_: &serde_json::Value,_: Option<&serde_json::Value>)->Result<(),String>{panic!("Surface transport is outside pure schema harness");}
}
`);
const filters=['app_form::legacy_quota_boundary_tests::','app_form::text_fields::tests::','app_form::mixed::tests::','clawpkg::device_json_tests::'];
const results=filters.map(filter=>spawnSync(process.env.PET_WORKFLOW_CARGO||'cargo',['test','--offline','--manifest-path',join(root,'Cargo.toml'),filter,'--','--test-threads=1'],{cwd:pc,encoding:'utf8',maxBuffer:8*1024*1024,env:{...process.env,CARGO_TARGET_DIR:process.env.PET_WORKFLOW_TARGET_DIR||join(evidence,'rust-target')}}));
const failed=results.find(r=>r.status!==0||r.error||r.signal),status=failed?(failed.status||1):0;
const log=results.map(r=>(r.stdout||'')+'\n'+(r.stderr||'')).join('\n');writeFileSync(join(evidence,`${label}.log`),log);process.stdout.write(log);
writeFileSync(join(evidence,`${label}.json`),JSON.stringify({status,cargoOffline:true,deviceUsed:false,servicesUsed:false,sourceSha256:Object.fromEntries(['clawpkg.rs','app_form.rs','app_form_v2.rs','app_form_v3.rs','app_variable_owners.rs'].map(file=>[file,createHash('sha256').update(readFileSync(join(source,file))).digest('hex')])),contractSha256:createHash('sha256').update(readFileSync(stringContract)).digest('hex'),unicodeContractSha256:createHash('sha256').update(readFileSync(join(pc,'skills/petui/contracts/device-json-unicode-v1.json'))).digest('hex'),depthContractSha256:createHash('sha256').update(readFileSync(join(pc,'skills/petui/contracts/device-json-depth-v1.json'))).digest('hex'),testModuleSha256:createHash('sha256').update(readFileSync(stringTests)).digest('hex'),selectedTestFilters:filters,scope:'Actual pure mixed form, variable and device-string validation functions; not full package/desktop execution',unreachableStubs:['full app runtime validator','surface validator'],fullDesktopTypecheckSeparate:true},null,2));process.exitCode=status;
