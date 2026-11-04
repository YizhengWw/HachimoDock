import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {webcrypto} from 'node:crypto';
import {deviceDeletePolicy} from './component-transfer-policy.js';
const source=name=>readFileSync(new URL(name,import.meta.url),'utf8');
test('UI confirmations stay inside app and publication sources retain custom variants',()=>{
  const community=source('CommunityCenter.jsx'),center=source('../ComponentCenter.jsx');
  for(const ui of [community,center])assert.doesNotMatch(ui,/\bwindow\s*\.\s*(?:confirm|prompt|alert)\s*\(/);
  assert.match(community,/WorkflowConfirm title="放弃未提交修改/);
  assert.match(center,/WorkflowConfirm title="设备版本更高/);
  assert.match(community,/localItems=\{publicationItems\.filter/);
  assert.match(community,/publicationItems=localItems\.flatMap\(item=>item\.sourceAlternatives\?\.filter\(source=>source\.isLocal\)/);
  assert.match(community,/setCovers\(\[\]\);\s*pending\.current=null;\s*dirty\.current=false/);
  const publish=community.slice(community.indexOf('function Publish('),community.indexOf('export default function CommunityCenter('));
  assert.doesNotMatch(publish,/\bset(?:Rights|Privacy|Tested)\b|type=["']checkbox["']/);
  assert.doesNotMatch(publish,/field\(\s*["']license["']/);
  assert.match(publish,/license:\s*""/);
  assert.match(publish,/license:\s*old\?\.license\s*\|\|\s*""/);
});

// Execute the actual submit handler against an in-memory boundary only. These
// fixtures never call Tauri, the community service, or real localStorage.
function publicationHarness({license='',canPublish=true}={}){
  const community=source('CommunityCenter.jsx');
  const handler=community.slice(community.indexOf('  async function submit(event)'),community.indexOf('  const field =',community.indexOf('  async function submit(event)')));
  const calls=[],published=[],errors=[],storage=new Map();
  const form={author:'fixture author',summary:'fixture summary',description:'fixture description',category:'tool',releaseNotes:'fixture notes',sourceUrl:''};
  if(license!==undefined)form.license=license;
  const env={
    busy:false,submitting:{current:false},prepared:{id:'fixture-component',canPublish,contentHash:'a'.repeat(64)},
    path:'/fixture/component',form,covers:['data:image/png;base64,c2NyZWVuc2hvdA=='],pending:{current:null},
    TextEncoder,Uint8Array,crypto:{subtle:webcrypto.subtle,randomUUID:()=> 'fixture-publication-request'},
    localStorage:{getItem:key=>storage.get(key)??null,setItem:(key,value)=>storage.set(key,value),removeItem:key=>storage.delete(key)},
    NAME_KEY:'fixture-author',setBusy:()=>{},setError:value=>errors.push(value),errorText:String,
    request:async(action,input)=>{calls.push({action,input});return {id:'fixture-component',status:'pending'};},
    onPublished:value=>published.push(value),
  };
  const submit=new Function(...Object.keys(env),`${handler};return submit;`)(...Object.values(env));
  return {submit:()=>submit({preventDefault:()=>{}}),calls,published,errors,env};
}

test('publication accepts empty or omitted license without fabricating human acknowledgments',async()=>{
  for(const license of ['',undefined,'legacy owned label']){
    const h=publicationHarness({license});
    if(license===undefined)delete h.env.form.license;
    await h.submit();
    assert.deepEqual(h.errors,['']);
    assert.equal(h.calls.length,1);
    const {action,input}=h.calls[0];
    assert.equal(action,'publish');
    assert.deepEqual(Object.keys(input).sort(),['covers','expectedContentHash','metadata','path','requestId']);
    assert.deepEqual(input.metadata,h.env.form);
    assert.deepEqual(input.covers,['c2NyZWVuc2hvdA==']);
    assert.equal(input.expectedContentHash,'a'.repeat(64));
    assert.equal(input.requestId,'fixture-publication-request');
    assert.deepEqual(h.published,[{id:'fixture-component',status:'pending'}]);
  }
  const blocked=publicationHarness({canPublish:false});await blocked.submit();
  assert.deepEqual(blocked.calls,[]);assert.deepEqual(blocked.published,[]);
});

test('optional publication license retains screenshot, scan, integrity and moderation gates',()=>{
  const community=source('CommunityCenter.jsx'),native=source('../../src-tauri/src/community.rs');
  const publish=native.slice(native.indexOf('        "publish" => {'),native.indexOf('        "download" => {'));
  assert.match(community,/accept="image\/png,image\/jpeg,image\/webp"/);
  assert.match(community,/files\.length > 3/);
  assert.match(community,/file\.size > 2 \* 1024 \* 1024/);
  assert.match(community,/!prepared\?\.canPublish\s*\|\|\s*!covers\.length\s*\|\|\s*checking/);
  assert.match(community,/新作品和更新均需审核，审核通过后才会公开/);
  assert.match(native,/"canPublish":true/);
  assert.match(native,/collect\(&canonical, &canonical, &mut files, &mut expanded\)\?/);
  assert.match(native,/if sensitive_json\(&v\)/);
  assert.match(native,/if sensitive_text\(std::str::from_utf8\(&bytes\)/);
  assert.match(publish,/let prepared = prepare\(home,/);
  assert.match(publish,/input\.expected_content_hash\.as_deref\(\) != Some\(prepared\.hash\.as_str\(\)\)/);
  assert.match(publish,/covers\.is_empty\(\)[\s\S]*covers\.len\(\) > 3/);
  assert.match(publish,/if sensitive_json\(&metadata\)/);
  assert.match(publish,/result\["contentHash"\] != prepared\.hash/);
  assert.doesNotMatch(publish,/rights_confirmed|privacy_confirmed|LICENSE\.txt|"(?:rightsConfirmed|privacyConfirmed)":true/);
  assert.doesNotMatch(native,/"canPublish":license|组件缺少 LICENSE\.txt/);
});
test('background refresh retains same-identity data but never reuses a different query result',()=>{
  const community=source('CommunityCenter.jsx');
  assert.match(community,/identityDependencies = dependencies/);
  assert.match(community,/data:sameRequest\?old\.data:null/);
  const publicWeb=readFileSync(new URL('../../../server/community/web/app.js',import.meta.url),'utf8');
  assert.match(publicWeb,/if\(turn!==epoch\)return/);
  assert.match(publicWeb,/key!==`\$\{state.metric\}:\$\{state.window\}`/);
});
test('transport cancellation, retries and target changes are guarded before sending',()=>{
  const center=source('../ComponentCenter.jsx');
  assert.match(center,/if\(transferInFlight\.current\)return false/);
  assert.match(center,/finally \{\s*transferInFlight\.current=false/);
  assert.match(center,/连接方式或目标已变化/);
  assert.match(center,/options\.epoch!==installEpoch\.current/);
});

// Execute the real deletion handler with an in-memory native boundary. No GUI,
// device, library mutation or user configuration is involved.
function deletionHarness({clearSave=true,mode='device-only',reply={ok:true},busy=false,capabilities={widgetDeletePreserveSave:true}}={}){
  const center=source('../ComponentCenter.jsx');
  const handler=center.slice(center.indexOf('  async function confirmComponentAction('),center.indexOf('  function resolveControlOption('));
  const calls=[],prompts=[],errors=[];
  const env={
    pendingComponentAction:{component:{id:'test-component',path:'/fixture/component'},target:{transport:'usb',boardDeviceId:'fixture-board'},installedOnDevice:true,isLocal:true},
    componentActionPending:busy,clearDeviceSave:clearSave,
    setClearSaveConfirmationMode:value=>prompts.push(value),
    setComponentActionPending:()=>{},setComponentActionError:value=>errors.push(value),
    deviceDeletePolicy,usb:{capabilities},
    invoke:async(command,input)=>{calls.push({command,input});return reply;},
    clearActiveComponentState:()=>{},setDeviceInventory:()=>{},setLocalComponents:()=>{},
    previewComponent:null,setPreviewComponent:()=>{},refreshComponentLibrary:async()=>{},
    push:()=>{},refreshDeviceInventory:async()=>{},setPendingComponentAction:()=>{},
    restoreComponentActionFocus:()=>{},componentActionErrorMessage:error=>String(error),
  };
  const confirm=new Function(...Object.keys(env),`${handler};return confirmComponentAction;`)(...Object.values(env));
  const dialog=center.match(/clearSaveConfirmationMode && <WorkflowConfirm([\s\S]*?)\/>/)[1];
  const cancel=new Function('setClearSaveConfirmationMode',`return ()=>${dialog.match(/onCancel=\{\(\)=>((?:[^\n])+?)\}/)[1]};`)(env.setClearSaveConfirmationMode);
  let submission;
  const acceptClick=new Function('clearSaveConfirmationMode','setClearSaveConfirmationMode','confirmComponentAction',`return ()=>{${dialog.match(/onConfirm=\{\(\)=>\{([^\n]+)\}\}/)[1]}};`)(mode,env.setClearSaveConfirmationMode,(...args)=>{submission=confirm(...args);return submission;});
  const accept=async()=>{acceptClick();await submission;};
  return {confirm,cancel,accept,calls,prompts,errors};
}

test('clearing device saves requires an in-app second confirmation; cancellation sends nothing',async()=>{
  for(const mode of ['device-only','delete-library']){
    const h=deletionHarness({mode});
    await h.confirm(mode);
    assert.deepEqual(h.prompts,[mode]);
    assert.deepEqual(h.calls,[]);
    h.cancel();
    assert.deepEqual(h.prompts,[mode,null]);
    assert.deepEqual(h.calls,[]);
  }
});

test('confirmed save clearing sends exact intent once and archives only after device success',async()=>{
  for(const mode of ['device-only','delete-library']){
    const h=deletionHarness({mode});
    await h.confirm(mode);await h.accept();
    assert.deepEqual(h.calls[0],{command:'remove_widget_from_device',input:{input:{preserveSave:false,clearDeviceSaveConfirmed:true,componentId:'test-component',transport:'usb',boardDeviceId:'fixture-board',sshHost:''}}});
    assert.deepEqual(h.calls.map(call=>call.command),mode==='device-only'?['remove_widget_from_device']:['remove_widget_from_device','delete_component_from_library']);
    const rejected=deletionHarness({mode,reply:{ok:false,error:'fixture rejection'}});
    await rejected.confirm(mode);await rejected.accept();
    assert.deepEqual(rejected.calls.map(call=>call.command),['remove_widget_from_device']);
    assert.match(rejected.errors.at(-1),/fixture rejection/);
  }
});

test('ordinary deletion preserves saves and pending or invalid actions cannot submit',async()=>{
  const h=deletionHarness({clearSave:false});await h.confirm('device-only');
  assert.deepEqual(h.prompts,[]);
  assert.equal(h.calls[0].input.input.preserveSave,true);
  assert.equal(h.calls[0].input.input.clearDeviceSaveConfirmed,false);
  for(const [busy,mode] of [[true,'device-only'],[false,'invalid']]){
    const blocked=deletionHarness({busy});await blocked.confirm(mode,true);
    assert.deepEqual(blocked.calls,[]);assert.deepEqual(blocked.prompts,[]);
  }
  const unsupported=deletionHarness({clearSave:false,capabilities:{}});await unsupported.confirm('device-only');
  assert.deepEqual(unsupported.calls,[]);assert.match(unsupported.errors.at(-1),/未删除/);
});
