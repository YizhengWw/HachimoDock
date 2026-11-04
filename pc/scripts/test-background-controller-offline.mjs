// Exact allowlists of synthetic writer/raw-reader tests in the real crate. No host
// initialization, ignored tests, serial enumeration, GUI, audio or services.
import {readFileSync,writeFileSync,mkdtempSync,statSync,mkdirSync} from 'node:fs';
import {resolve,dirname,join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
const pc=resolve(dirname(fileURLToPath(import.meta.url)),'..');
const [directory,label='run',mode='background']=process.argv.slice(2);
if(!directory||!/^[\w-]+$/.test(label)||!['background','control-integrity'].includes(mode)||process.argv.length>5)throw Error('explicit_output_required');
const out=resolve(directory);mkdirSync(out,{recursive:true});const run=mkdtempSync(join(out,`background-controller-${label}-`));
const cargo=process.env.PET_BACKGROUND_CARGO||'cargo';
const target=resolve(process.env.PET_BACKGROUND_TARGET_DIR||join(out,'rust-target'));
const env={...process.env,CARGO_TARGET_DIR:target,CARGO_NET_OFFLINE:'true',TAURI_CONFIG:JSON.stringify({bundle:{active:false,resources:[]}})};
const backgroundTests=['background_controller_exact_readonly_preflight_then_one_start_and_one_stop',
 'background_controller_unknown_never_replays_or_replaces_observed_owner',
 'background_controller_legacy_busy_or_incomplete_inventory_emits_no_mutation'].map(n=>'usb_serial::tests::'+n);
const integrityTests=[
 'usb_serial::control_integrity::tests::exact_bytes_not_json_syntax_or_lossy_text_are_the_integrity_boundary',
 'usb_serial::control_integrity::tests::malformed_overbound_header_and_embedded_json_cannot_be_recovered',
 'usb_serial::control_integrity::tests::negotiated_control_never_downgrades_or_exempts_receipts',
 'usb_serial::control_integrity::tests::shared_c_and_rust_utf8_vectors_are_exact_original_bytes',
 'usb_serial::control_integrity::tests::offered_integrity_requires_this_probes_nonce_and_a_framed_confirmation',
 'usb_serial::control_integrity::tests::ack_preserves_confirmed_session_without_including_payload_or_identity_logs',
 'usb_serial::windows_configuration::tests::win32_stage_and_original_error_code_survive_without_private_context',
 ...['actual_reader_checks_control_bytes_before_json_and_retains_fragmented_tail',
 'invalid_utf8_json_is_never_repaired_into_a_protocol_message',
 'serial_frames_survive_every_timeout_split_and_repeated_timeouts',
 'serial_probe_handoff_retains_prefetched_frames',
 'probe_and_connected_reads_share_bounded_transient_policy_without_losing_prefix',
 'screenshot_serial_frames_preserve_base64_across_fragments_and_timeouts',
 'serial_noise_is_bounded_and_eof_discards_an_incomplete_frame'].map(n=>'usb_serial::tests::'+n)];
const tests=mode==='control-integrity'?integrityTests:backgroundTests;
const sources=['app_program_background.rs','usb_serial.rs','usb_serial/transaction_waiters.rs','lib.rs',
 ...(mode==='control-integrity'?['usb_serial/control_integrity.rs','usb_serial/windows_configuration.rs','usb_serial/windows_serial.rs','graphics_acceptance.rs']:[])];
const hashes=()=>Object.fromEntries([
 ...sources.map(f=>[f,createHash('sha256').update(readFileSync(join(pc,'src-tauri/src',f))).digest('hex')]),
 ...(mode==='control-integrity'?['main/pet_p4_control_integrity.h','tests/vectors/p4_control_integrity.json'].map(f=>
  ['firmware/'+f,createHash('sha256').update(readFileSync(join(pc,'../firmware',f))).digest('hex')]):[])]);
const report={schema:1,status:'running',mode,tests:[],sourceBefore:hashes(),sourceAfter:null,sourceConsistent:false,
 deviceUsed:false,audioUsed:false,cloudCalls:0,guiStarted:false,bundleDisabled:true,packaged:false,releaseReady:false};
try{
 const build=spawnSync(cargo,['test','--offline','--locked','--no-run','--lib','--message-format=json-render-diagnostics'],
   {cwd:join(pc,'src-tauri'),env,encoding:'utf8',maxBuffer:32*1024*1024,timeout:300000});
 writeFileSync(join(run,'compiler.private.log'),build.stderr||'');
 if(build.status!==0||build.error||build.signal)throw Error('compile_failed');
 const artifacts=(build.stdout||'').split('\n').map(s=>{try{return JSON.parse(s);}catch{return null;}})
   .filter(v=>v?.reason==='compiler-artifact'&&v.target?.name==='pet_manager_tauri_lib'&&v.profile?.test&&v.executable);
 if(artifacts.length!==1)throw Error('test_artifact_not_unique');const binary=resolve(artifacts[0].executable);
 if(!binary.startsWith(target+'/debug/deps/pet_manager_tauri_lib-')||statSync(binary).size>1024*1024*1024)throw Error('binary_bounds');
 report.binarySha256=createHash('sha256').update(readFileSync(binary)).digest('hex');
 for(const name of tests){
   const r=spawnSync(binary,[name,'--exact','--test-threads=1'],{cwd:pc,env,encoding:'utf8',maxBuffer:1024*1024,timeout:15000});
   const log=(r.stdout||'')+'\n'+(r.stderr||'');writeFileSync(join(run,name.split('::').at(-1)+'.log'),log);
   const passed=r.status===0&&!r.signal&&!r.error&&/test result: ok\. 1 passed; 0 failed; 0 ignored;/.test(log)&&log.includes(name+' ... ok');
   report.tests.push({name,passed});if(!passed)throw Error('exact_synthetic_test_failed');
   process.stdout.write(r.stdout||'');
 }
 report.status='passed';
}catch(error){report.status='failed';report.reason=error.message;}
report.sourceAfter=hashes();report.sourceConsistent=JSON.stringify(report.sourceBefore)===JSON.stringify(report.sourceAfter);
if(!report.sourceConsistent)report.status='source_changed';
writeFileSync(join(out,label+'.json'),JSON.stringify(report,null,2)+'\n');
console.log(JSON.stringify({report:join(out,label+'.json'),status:report.status,deviceUsed:false}));process.exitCode=report.status==='passed'?0:1;
