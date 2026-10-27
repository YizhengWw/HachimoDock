/**
 * [Input] Raw provider and per-family generation errors.
 * [Output] Regression coverage for actionable failures, reference-image policy rejection, manual MP4 guidance and skipped-action summaries.
 * [Pos] test node in pc/src/lib
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import { recordDiagnostic } from "./avatar-pipeline/diagnostics.js";
import { publicGenerationConfig, generationSessionNeedsProtection, summarizeGenerationSession, canAutoResume } from "./avatar-pipeline/session.js";
import {
  buildAllFamiliesFailedMessage,
  normalizeGenerationErrorMessage,
} from "./generation-task.js";

test('automatic task saves every clip, converts once, and conversion-only recovery makes no model calls',async()=>{
  const source=readFileSync(new URL('./generation-task.js',import.meta.url),'utf8').replace(/^import .*;\n/gm,'').replace(/^export /gm,'');
  let stored, runs=0, saves=0, replacements=0, conversions=0, failConversion=true;
  const entries=Array.from({length:11},(_,i)=>({family:`fixture-${i}`,prompt:'fixture'}));
  const context=vm.createContext({recordDiagnostic,publicGenerationConfig,generationSessionNeedsProtection,summarizeGenerationSession,canAutoResume,clearTimeout,setTimeout,
    crypto:{randomUUID:()=> 'auto-fixture'},AbortController,resolveThinkingModelName:()=> 'fixture',
    beginGenerationSession:async s=>{stored=structuredClone(s);},saveGenerationSession:async s=>{stored=structuredClone(s);},
    saveAppearance:async data=>{assert.equal(data.deferPreparation,true);saves++;return {id:'saved'};},
    replaceFamilyVideo:async data=>{assert.equal(data.deferPreparation,true);replacements++;},
    prepareSavedAppearance:async()=>{conversions++;if(failConversion)throw Error('fixture conversion failed');},
    getVolcanoInputImageRejection:()=>null,getArkImageEditPolicyRejection:()=>null,
    runAvatarPipeline:async options=>{
      runs++;assert.equal(options.automatic,true);assert.equal(options.confirmReference,undefined);assert.equal(options.confirmTrial,undefined);
      assert.equal(options.styleDescription,'水彩风格');assert.equal(options.session.reference.dataUrl,'data:image/png;base64,AQ==');
      assert.equal(options.distinctStartFrames,true);
      const families=entries.map(e=>({...e,ok:true,videoBytes:new Uint8Array([1]),taskId:e.family}));
      options.session.manifest={entries};
      for(const family of families){options.session.actions[family.family]=family;await options.onFamilyDone({persona:{},manifest:{entries},imagePayload:{},family});}
      return {families};
    },
  });
  vm.runInContext(source+'\nthis.api={run:async(input)=>{activeController=new AbortController();return runTask(input);},getGenerationTask};',context);
  const input={providerConfig:{provider:'volcengine'},appearanceName:'fixture',styleDescription:'水彩风格',referencePreview:{bytes:new Uint8Array([1]),dataUrl:'data:image/png;base64,AQ=='}};
  await context.api.run(input);
  assert.equal(runs,1);assert.equal(saves,1);assert.equal(replacements,10);assert.equal(conversions,1);
  assert.equal(stored.phase,'conversion_failed');assert.equal(stored.finished,undefined);
  failConversion=false;
  await context.api.run({...input,session:stored});
  assert.equal(runs,1);assert.equal(saves,1);assert.equal(replacements,10);assert.equal(conversions,2);
  assert.equal(stored.finished,true);assert.equal(context.api.getGenerationTask().status,'completed');
  assert.equal(context.api.getGenerationTask().progress.stage,'completed');
  assert.match(context.api.getGenerationTask().progress.message,/整套动画和设备格式已准备完成/);
  assert.doesNotMatch(context.api.getGenerationTask().progress.message,/正在准备/);
  await context.api.run(input);
  assert.equal(runs,2);assert.equal(context.api.getGenerationTask().status,'completed');
  assert.equal(context.api.getGenerationTask().progress.completed,11);
  assert.match(context.api.getGenerationTask().progress.message,/整套动画和设备格式已准备完成/);
});

test("failed preprocessing releases durable task lock and a fresh request reaches the pipeline", async () => {
  const source=readFileSync(new URL('./generation-task.js',import.meta.url),'utf8').replace(/^import .*;\n/gm,'').replace(/^export /gm,'');
  let stored={id:'legacy',imageIntent:true,actions:{}}; let runs=0; const archived=[];
  const context=vm.createContext({recordDiagnostic,publicGenerationConfig,generationSessionNeedsProtection,summarizeGenerationSession,canAutoResume,clearTimeout,setTimeout,
    crypto:{randomUUID:()=>`fresh-${runs}`},AbortController,
    loadGenerationSession:async()=>stored,saveGenerationSession:async s=>{stored=structuredClone(s);},
    beginGenerationSession:async s=>{archived.push(structuredClone(stored));stored=structuredClone(s);},
    runAvatarPipeline:async()=>{runs++;throw Error('fixture preprocessing failure');},
    getVolcanoInputImageRejection:()=>null,getArkImageEditPolicyRejection:()=>null,
  });
  vm.runInContext(source+'\nthis.api={run:async(input)=>{activeController=new AbortController();return runTask(input);},getGenerationTask};',context);
  const input={appearanceName:'new',providerConfig:{provider:'volcengine'},skipProcessing:true};
  await context.api.run(input);
  assert.equal(runs,1);assert.equal(stored.dismissed,true);assert.equal(stored.terminalFailure,true);
  assert.equal(context.api.getGenerationTask().resumable,false);
  await context.api.run(input);assert.equal(runs,2);
  stored={actions:{welcome:{intent:true,taskId:'accepted'}}};
  await context.api.run(input);
  assert.equal(runs,3);assert.equal(archived.at(-1).actions.welcome.taskId,'accepted');
  stored={id:'legacy-http-400-or-unknown',actions:{welcome:{intent:true}}};
  await context.api.run(input);
  assert.equal(runs,4);assert.equal(archived.at(-1).actions.welcome.intent,true);
});

test("wizard defaults to full subject cleaning and warns before explicit original-image mode",()=>{
  const source=readFileSync(new URL('../CustomAvatarWizard.jsx',import.meta.url),'utf8');
  assert.match(source,/\[removeBg, setRemoveBg\] = useState\(true\)/);
  assert.match(source,/skipProcessing: !removeBg/);
  assert.match(source,/图片处理费用/);
  assert.match(source,/原图中的手、桌面、人物和杂物可能进入视频/);
  assert.doesNotMatch(source,/云端不可用时会保留原图继续生成/);
});

test("restart restores saved trial progress without fabricating failure or paid work",async()=>{
  const source=readFileSync(new URL('./generation-task.js',import.meta.url),'utf8').replace(/^import .*;\n/gm,'').replace(/^export /gm,'');
  const stored={appearanceName:'2',appearanceId:'saved',skipProcessing:true,createdAt:123,
    actions:{welcome:{videoBytes:new Uint8Array([1,2])}}};
  const context=vm.createContext({recordDiagnostic,summarizeGenerationSession,canAutoResume,clearTimeout,setTimeout,loadGenerationSession:async()=>stored});
  vm.runInContext(source+'\nthis.api={initializeGenerationRecovery,getGenerationTask,acknowledgeGenerationTask};',context);
  await context.api.initializeGenerationRecovery();
  const state=context.api.getGenerationTask();
  assert.equal(state.status,'paused');assert.equal(state.appearanceId,'saved');
  assert.equal(state.progress.completed,1);assert.equal(state.progress.total,11);
  assert.match(state.progress.message,/等待确认/);assert.match(state.progress.message,/恢复不会重新去背景/);
  assert.equal(state.completionEpoch,0);
  context.api.acknowledgeGenerationTask();assert.equal(context.api.getGenerationTask().status,'idle');
});

test("terminal acknowledgement ignores older toasts and running tasks", () => {
  // Exercise the real state machine in isolation; no provider calls or user data.
  const source = readFileSync(new URL("./generation-task.js", import.meta.url), "utf8")
    .replace(/^import .*;\n/gm, "")
    .replace(/^export /gm, "");
  const context = vm.createContext({ recordDiagnostic });
  vm.runInContext(source + "\nthis.api = { setState, getGenerationTask, acknowledgeGenerationTask };", context);
  const { setState, getGenerationTask, acknowledgeGenerationTask } = context.api;
  setState({ status: "failed", completionEpoch: 2, error: "test" });
  acknowledgeGenerationTask(1);
  assert.equal(getGenerationTask().status, "failed");
  acknowledgeGenerationTask(2);
  assert.equal(getGenerationTask().status, "idle");
  setState({ status: "running", completionEpoch: 2 });
  acknowledgeGenerationTask(2);
  assert.equal(getGenerationTask().status, "running");
  setState({ status: "completed", completionEpoch: 3 });
  acknowledgeGenerationTask(2);
  assert.equal(getGenerationTask().status, "completed");
  acknowledgeGenerationTask(3);
  assert.equal(getGenerationTask().status, "idle");
});

test("all-family failures collapse repeated ModelNotOpen errors into one actionable cause", () => {
  const families = ["welcome", "idle.playing", "idle.wandering"].map((family, index) => ({
    family,
    ok: false,
    error: [
      "火山引擎模型未开通：当前 Ark 账号还没有开通 doubao-seedance-2-0-260128。",
      "请在 Ark 控制台开通该模型服务，或先在模型下拉中改用 doubao-seedance-1-5-pro-251215。",
      `原始错误：volcano submit HTTP 404: {"error":{"code":"ModelNotOpen","message":"Request id: req-${index}"}}`,
      'Volcano payload summary: {"model":"doubao-seedance-2-0-260128"}',
    ].join("\n"),
  }));

  const message = buildAllFamiliesFailedMessage(families);

  assert.match(message, /生成失败：所有动作都没有生成成功/);
  assert.equal((message.match(/火山引擎模型未开通/g) || []).length, 1);
  assert.match(message, /影响动作：welcome、idle\.playing、idle\.wandering/);
  assert.match(message, /诊断摘要/);
  assert.doesNotMatch(message, /idle\.playing: volcano submit HTTP 404/);
  assert.equal(normalizeGenerationErrorMessage(message), message);
});

test("raw provider errors are translated into product-level guidance", () => {
  assert.match(
    normalizeGenerationErrorMessage(
      'volcano submit HTTP 400: {"error":{"code":"InvalidParameter","message":"expected the height to be at least 300px, but received a 320x240px image instead"}}',
    ),
    /参考图尺寸不满足火山引擎要求/,
  );

  assert.match(
    normalizeGenerationErrorMessage(
      'thinking model HTTP 400: {"error":{"code":"InvalidParameter","message":"Model do not support image input","param":"image_url"}}',
    ),
    /文字分析模型不支持图片输入/,
  );

  assert.match(
    normalizeGenerationErrorMessage('volcano submit HTTP 401: {"error":{"message":"invalid api key"}}'),
    /API Key 无效或已过期/,
  );
});

test("portrait upload alternative survives the all-actions-failed summary", () => {
  const raw = 'volcano submit HTTP 400: {"error":{"code":"InputImageSensitiveContentDetected.PrivacyInformation","message":"input image may contain real person"}}';
  const summary = normalizeGenerationErrorMessage(buildAllFamiliesFailedMessage([
    { family: "welcome", ok: false, error: raw },
    { family: "working", ok: false, skipped: true, error: raw },
  ]));
  assert.match(summary, /参考图未通过人像审核/);
  assert.match(summary, /自行生成动态形象并导出 MP4/);
  assert.match(summary, /形象画廊 → 新建自定义形象 → 自定义上传视频/);
  assert.match(summary, /上传 MP4 替换/);
  assert.equal((summary.match(/自行生成动态形象/g) || []).length, 1);
});

test("image copyright rejection is not misreported as bad parameters or credentials", () => {
  const raw = 'volcano submit HTTP 400: {"error":{"code":"InputImageSensitiveContentDetected.PolicyViolation","message":"The input image content[1] content[2] may be related to copyright restrictions. Request id: test-id","type":"BadRequest"}}';
  const message = normalizeGenerationErrorMessage(raw);
  assert.match(message, /^图生视频失败：/);
  assert.match(message, /参考图未通过版权审核/);
  assert.match(message, /可能涉及版权限制/);
  assert.match(message, /复核/);
  assert.doesNotMatch(message, /请求参数|API Key|模型不可用/);
  const summary = buildAllFamiliesFailedMessage([
    { family: "welcome", ok: false, error: raw },
    { family: "working", ok: false, skipped: true, error: raw },
  ]);
  assert.equal((summary.match(/参考图未通过版权审核/g) || []).length, 1);
  assert.match(summary, /停止提交剩余 1 个动作/);
  assert.match(summary, /已完成的结果保留/);
  assert.match(normalizeGenerationErrorMessage('volcano submit HTTP 400: {"error":{"code":"InvalidParameter","message":"unsupported ratio"}}'), /请求参数不被当前模型接受/);
});

test("Seedream output copyright rejection stays at image editing, including truncated bodies", () => {
  const raw = '参考图处理失败。Ark image edit (doubao-seedream-5-0-flash-260915) HTTP 400: {"error":{"code":"OutputImageSensitiveContentDetected.PolicyViolation","message":"The output image may be related to copyright restrictions. Request id: req-output-test","type":"BadRequest"}}';
  for (const value of [raw, raw.slice(0, -10)]) {
    const message = normalizeGenerationErrorMessage(value);
    assert.match(message, /^参考图处理未通过版权审核/);
    assert.match(message, /处理后的生成结果/);
    assert.match(message, /尚未提交视频任务/);
    assert.match(message, /req-output-test/);
    assert.doesNotMatch(message, /请求参数不被|图生视频失败|输入参考图可能|可重试处理|关闭背景处理/);
    assert.equal(normalizeGenerationErrorMessage(message), message);
  }
});
