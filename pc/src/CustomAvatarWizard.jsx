/**
 * [Input] User-uploaded image + centrally managed provider config; orchestrates `lib/avatar-pipeline/run.js`.
 * [Output] Optional visual-style description and cancellable single-image preview, reused by animation generation; on success persists via `lib/appearance-store.js`, with clear GIF first-frame copy,
 *          fixed-size reference upload preview, native macOS Photos/file-promise import with bounded local conversion, shared provider-config persistence with read-only credential readiness linked to the API configuration page,
 *          entry-page automatic Ark model discovery (including bundled-key fresh installs), scoped cancellation and retry, plus persisted video parameters and Seedream cloud background editing,
 *          fast low-resolution defaults, reusable step components, optional inline progress, and preflight generation/portrait authorization requirements with a manual MP4 upload alternative.
 * [Pos] component node in pc/src
 * [Sync] If this file changes, update this header and `pc/src/.folder.md`.
 */

import React, { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { ArrowLeft, ImageUp, AlertCircle, CheckCircle2, KeyRound } from "lucide-react";
import {
  isGenerationRunning,
  startGenerationTask,
  subscribeGenerationTask,
} from "./lib/generation-task.js";
import {
  FAST_VIDEO_GENERATION_PROFILE,
  PIPELINE_OUTPUT_ASPECT_RATIO,
} from "./lib/avatar-pipeline/pipeline-defaults.js";
import {
  DEFAULT_ADVANCED,
  DEFAULT_PROVIDER_ID,
  VIDEO_PROVIDERS,
  VOLCENGINE_BASE_URL,
  VOLCENGINE_THINKING_MODEL,
  loadArkImageConfig,
  loadProviderConfig,
  saveProviderConfig,
} from "./lib/avatar-pipeline/provider-config.js";
import {
  API_CONFIGURATION_UPDATED_EVENT,
  providerCredentialsConfigured,
} from "./lib/api-configuration.js";
import HelpTooltip from "./HelpTooltip.jsx";
import { VideoModelSelect, VideoGenerationSettings } from "./VideoGenerationSettings.jsx";
import TextModelSelect from "./TextModelSelect.jsx";
import ImageModelSelect from "./ImageModelSelect.jsx";
import useReferenceImageImport from "./useReferenceImageImport.js";
import { discoverDefaultVideoModel } from "./lib/avatar-pipeline/video-models.js";
import { createReferencePreviewController, normalizeStyleDescription } from './lib/avatar-pipeline/reference-preview.js';
import { resolveArkBackgroundConfig } from './lib/avatar-pipeline/run.js';

const FAST_REFERENCE_HEIGHT = Math.round(
  (FAST_VIDEO_GENERATION_PROFILE.imageMaxDimension * PIPELINE_OUTPUT_ASPECT_RATIO.height) /
    PIPELINE_OUTPUT_ASPECT_RATIO.width,
);

const HELP_TEXT = {
  baseUrl:
    "填写接口根地址，例如平台给出的 API 域名。请保留 https://，不要把具体接口路径重复填进来。",
  model:
    "根据当前 API Key 获取视频模型列表；实际调用权限以平台响应为准，也可手动填写模型或接入点 ID。",
};

export default function CustomAvatarWizard({ onExit, onOpenApiSettings }) {
  const [step, setStep] = useState(0);
  const [file, setFile] = useState(null);
  const [previewUrl, setPreviewUrl] = useState("");
  const [appearanceName, setAppearanceName] = useState("");
  const [personality, setPersonality] = useState("");
  const [styleDescription, setStyleDescription] = useState('');
  const [referencePreview, setReferencePreview] = useState({busy:false,result:null,error:''});
  const previewController = useRef(null);
  const previewImageModel = useRef("");
  if(!previewController.current)previewController.current=createReferencePreviewController(setReferencePreview);

  const [providerId, setProviderId] = useState(DEFAULT_PROVIDER_ID);
  const provider = useMemo(
    () => VIDEO_PROVIDERS.find((item) => item.id === providerId) || VIDEO_PROVIDERS[0],
    [providerId],
  );
  const isVolcengine = providerId === "volcengine";

  const [apiKey, setApiKey] = useState("");
  const [accessKey, setAccessKey] = useState("");
  const [secretKey, setSecretKey] = useState("");
  const [baseUrl, setBaseUrl] = useState(provider.baseUrl);
  const [model, setModel] = useState(provider.models[0] || "");
  const [thinkingModel, setThinkingModel] = useState("");
  const [imageModel, setImageModel] = useState("");
  const [advancedOpen, setAdvancedOpen] = useState(false);
  const [openaiCompat, setOpenaiCompat] = useState(true);
  const [advanced, setAdvanced] = useState({ ...DEFAULT_ADVANCED });
  const [removeBg, setRemoveBg] = useState(true);
  const [manualReview, setManualReview] = useState(false);
  const [fastGeneration, setFastGeneration] = useState(true);
  const [videoParameters, setVideoParameters] = useState({});
  const [modelDiscovery, setModelDiscovery] = useState({ loading: false, error: "" });
  const [modelRetry, setModelRetry] = useState(0);
  useEffect(()=>{previewController.current.invalidate();return ()=>previewController.current.invalidate();},[file,styleDescription,removeBg,apiKey,providerId,fastGeneration,imageModel]);
  const validPreview=referencePreview.result?.source===file && referencePreview.result?.style===normalizeStyleDescription(styleDescription)
    ? referencePreview.result : null;
  const handleStylePreview=()=>previewController.current.run({imageFile:file,styleDescription,
    cloudBackground:resolveArkBackgroundConfig({provider:providerId,apiKey,baseUrl,
      imageEdit:{...loadArkImageConfig(),...(isVolcengine?{model:imageModel.trim()}:{})}}),
    maxDimension:fastGeneration?FAST_VIDEO_GENERATION_PROFILE.imageMaxDimension:undefined});

  // The default page must prepare its own model; the advanced selector is not mounted there.
  useEffect(() => {
    if (step !== 0 || !isVolcengine || !apiKey.trim() || model.trim()) return;
    const controller = new AbortController();
    setModelDiscovery({ loading: true, error: "" });
    discoverDefaultVideoModel({ apiKey, baseUrl, signal: controller.signal })
      .then(next => {
        if (controller.signal.aborted) return;
        setModel(current => current.trim() ? current : next);
        setModelDiscovery({ loading: false, error: "" });
      })
      .catch(error => {
        if (!controller.signal.aborted) setModelDiscovery({ loading: false, error: error.message || "视频模型加载失败，请重试。" });
      });
    return () => controller.abort();
  }, [step, isVolcengine, apiKey, baseUrl, model, modelRetry]);

  const [submitError, setSubmitError] = useState("");
  const [starting, setStarting] = useState(false);
  const startingRef = useRef(false);
  const mountedRef = useRef(true);
  useEffect(() => { mountedRef.current = true; return () => { mountedRef.current = false; }; }, []);
  const [taskRunning, setTaskRunning] = useState(() => isGenerationRunning());

  useEffect(
    () =>
      subscribeGenerationTask((state) => {
        setTaskRunning(state.status === "running");
      }),
    [],
  );

  const refreshProviderConfig = useCallback(() => {
    const saved = loadProviderConfig(providerId);
    const currentImageModel = loadArkImageConfig().model;
    if (previewImageModel.current !== currentImageModel) {
      previewImageModel.current = currentImageModel;
      previewController.current.invalidate();
    }
    setApiKey(saved.apiKey);
    setAccessKey(saved.accessKey);
    setSecretKey(saved.secretKey);
    setBaseUrl(saved.baseUrl);
    setModel(saved.model);
    setThinkingModel(saved.thinkingModel);
    setImageModel(saved.imageModel);
    setFastGeneration(saved.fastGeneration);
    setVideoParameters(saved.videoParameters);
    setAdvanced(saved.advanced);
  }, [providerId]);

  useEffect(() => {
    refreshProviderConfig();
  }, [refreshProviderConfig]);

  useEffect(() => {
    window.addEventListener(API_CONFIGURATION_UPDATED_EVENT, refreshProviderConfig);
    return () => {
      window.removeEventListener(API_CONFIGURATION_UPDATED_EVENT, refreshProviderConfig);
    };
  }, [refreshProviderConfig]);

  useEffect(() => {
    if (!file) {
      setPreviewUrl("");
      return undefined;
    }
    const reader = new FileReader();
    let cancelled = false;
    reader.onload = () => {
      if (!cancelled) setPreviewUrl(typeof reader.result === "string" ? reader.result : "");
    };
    reader.onerror = () => {
      if (!cancelled) setPreviewUrl("");
    };
    reader.readAsDataURL(file);
    return () => {
      cancelled = true;
      if (reader.readyState === FileReader.LOADING) {
        try {
          reader.abort();
        } catch {
          /* noop */
        }
      }
    };
  }, [file]);

  const handleFile = useCallback((selected) => {
    if (!selected) return;
    setFile(selected);
  }, []);

  const inputRef = useRef(null);
  const onPickClick = useCallback(() => inputRef.current?.click(), []);

  const onDrop = useCallback(
    (event) => {
      event.preventDefault();
      const picked = event.dataTransfer.files?.[0];
      if (picked) handleFile(picked);
    },
    [handleFile],
  );

  const canAdvanceFromStep1 = Boolean(file);
  const generationReadyIssue = useMemo(() => {
    if (!manualReview && !isVolcengine) return "此服务暂不支持自动视觉检查，请在高级设置开启逐步确认模式。";
    if (providerId === "kling") {
      if (!accessKey.trim() || !secretKey.trim()) {
        return "请先在 API 配置中保存 Kling Access Key 和 Secret Key。";
      }
      if (!baseUrl.trim() || !model.trim()) {
        return "请先填写接口地址和视频生成模型。";
      }
      return "";
    }

    if (isVolcengine) {
      if (!apiKey.trim()) return "请先在 API 配置中保存火山引擎 API Key。";
      if (!model.trim()) return modelDiscovery.loading || !modelDiscovery.error
        ? "正在自动选择视频模型，请稍候…" : modelDiscovery.error;
    }
    if (!apiKey.trim()) return "请先在 API 配置中保存当前服务的 API Key。";
    if (!baseUrl.trim() || !model.trim()) return "请先填写接口地址和视频生成模型。";
    return "";
  }, [providerId, accessKey, secretKey, apiKey, baseUrl, model, isVolcengine, manualReview, modelDiscovery]);

  const canStartGenerate = !generationReadyIssue;

  const persistConfig = useCallback(() => {
    return saveProviderConfig(providerId, {
      apiKey,
      accessKey,
      secretKey,
      baseUrl,
      model,
      thinkingModel,
      imageModel: imageModel.trim(),
      fastGeneration,
      videoParameters,
      advanced,
    });
  }, [providerId, apiKey, accessKey, secretKey, baseUrl, model, thinkingModel, imageModel, fastGeneration, videoParameters, advanced]);

  const handleStartGenerate = useCallback(async () => {
    if (!file || startingRef.current || referencePreview.busy) return;
    if (generationReadyIssue) { setSubmitError(generationReadyIssue); return; }
    if (taskRunning) {
      setSubmitError("已有一个生成任务正在进行中，请等待当前任务完成后再开始。");
      return;
    }
    startingRef.current = true;
    setStarting(true);
    try { await persistConfig(); } catch (error) {
      startingRef.current = false;
      setStarting(false);
      setSubmitError(error.message || "配置未保存，请重试。");
      return;
    }
    // Leaving the wizard while the vault is opening must not start a paid task later.
    if (!mountedRef.current) { startingRef.current = false; return; }
    setSubmitError("");

    const trimmedModel = model.trim();
    const providerConfig = {
      provider: providerId,
      apiKey,
      accessKey,
      secretKey,
      baseUrl: isVolcengine ? VOLCENGINE_BASE_URL : baseUrl,
      model: trimmedModel,
      imageEdit: loadArkImageConfig(),
      thinkingModel: thinkingModel.trim() || (isVolcengine ? VOLCENGINE_THINKING_MODEL : trimmedModel),
      fastGeneration,
      ...(isVolcengine ? videoParameters : {}),
      advanced:
        providerId === "custom"
          ? {
              ...advanced,
              authHeader: openaiCompat ? "Authorization" : advanced.authHeader,
              authPrefix: openaiCompat ? "Bearer" : advanced.authPrefix,
            }
          : undefined,
    };

    try {
      startGenerationTask({
        imageFile: file,
        appearanceName,
        personality,
        styleDescription:normalizeStyleDescription(styleDescription),
        referencePreview:validPreview ? {bytes:validPreview.bytes,mime:validPreview.mime,base64:validPreview.base64,dataUrl:validPreview.dataUrl,filename:validPreview.filename,master:validPreview.master} : null,
        providerConfig,
        skipProcessing: !removeBg,
        manualReview,
      });
    } catch (error) {
      startingRef.current = false;
      setStarting(false);
      setSubmitError(error?.message || String(error));
      return;
    }

    startingRef.current = false;
    setStarting(false);
    onExit?.();
  }, [
    file,
    generationReadyIssue,
    taskRunning,
    providerId,
    isVolcengine,
    thinkingModel,
    persistConfig,
    apiKey,
    accessKey,
    secretKey,
    baseUrl,
    model,
    fastGeneration,
    videoParameters,
    advanced,
    openaiCompat,
    appearanceName,
    personality,
    styleDescription,
    validPreview,
    referencePreview.busy,
    removeBg,
    manualReview,
    onExit,
  ]);

  return (
    <div className="page page-appearance-wizard">
      <div className="page-toolbar" style={{ marginBottom: 4 }}>
        <button className="btn-ghost" onClick={onExit}>
          <ArrowLeft size={16} />
          返回宠物图册
        </button>
      </div>

      <div className="ca-wizard">
        <div className="ca-tabs">
          {["选图并生成", "高级设置（可选）"].map((label, index) => (
            <div
              key={label}
              className={`ca-tab ${step === index ? "active" : step > index ? "done" : ""}`}
            >
              {step > index ? "OK" : index + 1} · {label}
            </div>
          ))}
        </div>

        {taskRunning && (
          <div className="message-banner" style={{ marginBottom: 12, background: "var(--surface-muted)" }}>
            <AlertCircle size={14} />
            已有一个形象正在后台生成，完成后会在宠物图册里收到通知。
          </div>
        )}

        {step === 0 && (
          <AvatarWizardStep1
            file={file}
            previewUrl={previewUrl}
            appearanceName={appearanceName}
            personality={personality}
            onAppearanceName={setAppearanceName}
            onPersonality={setPersonality}
            onPickClick={onPickClick}
            onDrop={onDrop}
            inputRef={inputRef}
            onFileChange={handleFile}
            onCancel={onExit}
            canAdvance={canAdvanceFromStep1 && canStartGenerate && !taskRunning && !starting && !referencePreview.busy}
            title="选一张完整人物或角色图，自动生成整套动画"
            nextLabel={starting ? "正在准备…" : "一键生成整套 · 11 段"}
            onNext={handleStartGenerate}
          >
            <ReferenceStyleEditor value={styleDescription} onChange={setStyleDescription}
              preview={{...referencePreview,result:validPreview}} onPreview={handleStylePreview}
              onCancel={()=>previewController.current.invalidate()}
              disabled={!file || !removeBg || taskRunning || starting}
              processingEnabled={removeBg}/>
            <p className="muted small">默认自动清理主体并检查参考图，再为 11 个动作分别生成不同的起始姿势图片，检查后生成对应动画，不需要中途确认。会产生参考图、每个动作起始图、检查和视频费用；已有结果自动复用。请保持客户端运行，完成后可一键应用到设备。</p>
            <p className="muted small">支持人物、动物、玩偶和机器人。留空使用默认风格；预览可选，生成动画时会复用当前预览图片。保留角色服装、配饰及自身肢体，移除背景及其他人物。</p>
            <p className="muted small">{manualReview ? "当前：逐步确认模式" : "当前：全自动模式"} · {removeBg ? "完整主体清理已开启" : "使用原图，未清理背景"}</p>
            {generationReadyIssue && <p role="alert">{generationReadyIssue}</p>}
            {isVolcengine && apiKey.trim() && !model.trim() && modelDiscovery.error && !modelDiscovery.loading &&
              <button type="button" className="btn-secondary" onClick={() => setModelRetry(value => value + 1)}>重新加载视频模型</button>}
            {isVolcengine && model.trim() && <p className="muted small">视频模型：{model}</p>}
            {submitError && <p role="alert">{submitError}</p>}
            <button type="button" className="btn-secondary" onClick={()=>setStep(1)}>高级设置 / 模型与逐步确认</button>
            {generationReadyIssue && <button type="button" className="btn-ghost" onClick={onOpenApiSettings}>前往 API 配置</button>}
          </AvatarWizardStep1>
        )}

        {step === 1 && (<>
          <label className="row"><input type="checkbox" checked={manualReview} onChange={e=>setManualReview(e.target.checked)}/>逐步确认参考图和试片（默认关闭）</label>
          <AvatarWizardStep2
            apiKey={apiKey}
            videoParameters={videoParameters}
            onVideoParameters={setVideoParameters}
            providerId={providerId}
            credentialConfigured={providerCredentialsConfigured(providerId, {
              apiKey,
              accessKey,
              secretKey,
            })}
            onOpenApiSettings={onOpenApiSettings}
            baseUrl={baseUrl}
            onBaseUrl={setBaseUrl}
            model={model}
            onModel={setModel}
            thinkingModel={thinkingModel}
            onThinkingModel={setThinkingModel}
            imageModel={imageModel}
            onImageModel={setImageModel}
            advancedOpen={advancedOpen}
            onAdvancedOpen={setAdvancedOpen}
            openaiCompat={openaiCompat}
            onOpenaiCompat={setOpenaiCompat}
            advanced={advanced}
            onAdvanced={setAdvanced}
            onPickProvider={setProviderId}
            canStart={canStartGenerate && !taskRunning && !starting && !referencePreview.busy}
            generationReadyIssue={generationReadyIssue}
            submitError={submitError}
            removeBg={removeBg}
            onRemoveBg={setRemoveBg}
            fastGeneration={fastGeneration}
            onFastGeneration={setFastGeneration}
            onBack={() => setStep(0)}
            onStart={handleStartGenerate}
            startLabel="生成整套 · 11 段"
          />
        </>)}
      </div>
    </div>
  );
}

export function ReferenceStyleEditor({value,onChange,preview,onPreview,onCancel,disabled,processingEnabled=true}) {
  return <section className="field" aria-label="形象风格描述与预览">
    <label className="field-label" htmlFor="avatar-style-description">形象风格描述（可选）</label>
    <textarea id="avatar-style-description" className="field-input" rows={3} maxLength={1000}
      placeholder="例如：白皙肤色、大头大眼、可爱的 Q 版，保留原来的发型和服装"
      value={value} onChange={event=>onChange(event.target.value)} style={{padding:'10px 12px',resize:'vertical'}}/>
    <p className="muted small">不填也能直接生成。默认人物为可爱 Q 版；填写后按你的描述处理外观。预览仅生成一张参考图，会产生图片处理费用。</p>
    {!processingEnabled && <p className="muted small">已选择使用原图；开启高级设置中的主体清理后，才能应用风格和生成预览。</p>}
    <div style={{display:'flex',gap:10}}>
      <button type="button" className="btn-secondary" disabled={disabled||preview.busy} onClick={onPreview}>{preview.busy?'正在生成预览…':preview.result?'重新生成图片预览':'生成图片预览'}</button>
      {preview.busy && <button type="button" className="btn-ghost" onClick={onCancel}>取消预览</button>}
    </div>
    {preview.busy && <p className="muted small" role="status">正在处理图片；完成前可以修改描述或取消。取消已提交的请求不保证退回模型费用。</p>}
    {preview.error && <p role="alert">{preview.error}</p>}
    {preview.result && <figure style={{margin:'12px 0'}}>
      <img src={preview.result.dataUrl} alt="按形象风格描述生成的参考图预览" style={{display:'block',width:'100%',maxHeight:360,objectFit:'contain',background:'#000',borderRadius:12}}/>
      <figcaption className="muted small">生成动画将使用这张参考图，无需再次处理图片。修改描述或更换原图后需重新预览。</figcaption>
    </figure>}
  </section>;
}

export function AvatarWizardStep1({
  file,
  previewUrl,
  appearanceName,
  personality,
  onAppearanceName,
  onPersonality,
  onPickClick,
  onDrop,
  inputRef,
  onFileChange,
  onCancel,
  canAdvance,
  onNext,
  title = "第 1 步 · 上传参考图",
  identityFields = true,
  cancelLabel = "取消",
  nextLabel = "下一步",
  children,
}) {
  const imageImport = useReferenceImageImport(onFileChange);
  const pickFile = () => imageImport.native ? imageImport.action("file") : onPickClick();
  return (
    <div className="ca-card">
      <h3>{title}</h3>
      <div
        className="dropzone"
        ref={imageImport.zoneRef}
        role="button"
        tabIndex={0}
        aria-label="选择参考图片文件"
        aria-busy={imageImport.busy}
        onKeyDown={event => { if (!imageImport.busy && (event.key === "Enter" || event.key === " ")) { event.preventDefault(); pickFile(); } }}
        onClick={() => { if (!imageImport.busy) pickFile(); }}
        onDrop={imageImport.drop}
        onDragOver={(event) => event.preventDefault()}
      >
        {previewUrl ? (
          <>
            <img key={previewUrl} className="dropzone__preview" src={previewUrl} alt="预览" />
            <div className="dropzone__filename">{file?.name || "更换图片"}</div>
            <div className="muted small">点击更换图片</div>
          </>
        ) : (
          <>
            <span className="dropzone__icon">
              <ImageUp size={28} />
            </span>
            <div>点击或拖拽选择本地图片</div>
            <div className="muted small">支持 PNG / JPEG / WebP / GIF；Mac 支持 HEIC。GIF 会取首帧作为参考图，实况照片取静态画面。</div>
          </>
        )}
        <input
          ref={inputRef}
          type="file"
          accept="image/png,image/jpeg,image/webp,image/gif,image/*,.heic,.heif"
          style={{ display: "none" }}
          onChange={(event) => {
            imageImport.acceptFile(event.target.files?.[0]);
            event.target.value = "";
          }}
        />
      </div>

      {imageImport.native && <div style={{ display: "flex", gap: 10, marginTop: 12 }}>
        <button type="button" className="btn-secondary btn-sm" disabled={imageImport.busy} onClick={pickFile}>选择图片文件</button>
        <button type="button" className="btn-secondary btn-sm" disabled={imageImport.busy} onClick={() => imageImport.action("photos")}>从照片图库选择</button>
      </div>}
      {imageImport.busy && <div className="muted small" role="status" style={{ marginTop: 8 }}>正在读取并转换图片，iCloud 原图可能需要下载… <button type="button" className="btn-secondary btn-sm" onClick={imageImport.cancel}>取消读取</button></div>}
      {imageImport.error && <div role="alert" style={{ color: "var(--danger, #c33)", marginTop: 8 }}>{imageImport.error}</div>}
      {imageImport.diagnostic && <details className="muted small" style={{marginTop:8}}><summary>图片导入诊断（仅格式、耗时和错误码）</summary><pre style={{whiteSpace:'pre-wrap',overflowWrap:'anywhere'}}>{JSON.stringify(imageImport.diagnostic,null,2)}</pre></details>}

      {identityFields ? (
        <>
          <div className="field">
            <label className="field-label">形象名称</label>
            <input
              className="field-input"
              placeholder="给这个形象起个名字"
              value={appearanceName}
              onChange={(event) => onAppearanceName(event.target.value)}
            />
          </div>
          <div className="field">
            <label className="field-label">性格描述（可选）</label>
            <textarea
              className="field-input"
              style={{ height: 80, padding: "10px 12px", resize: "vertical" }}
              placeholder="例如：安静、敏捷、喜欢互动"
              value={personality}
              onChange={(event) => onPersonality(event.target.value)}
            />
          </div>
        </>
      ) : null}
      {children}
      <div className="ca-actions">
        <button className="btn-ghost" onClick={onCancel}>
          {cancelLabel}
        </button>
        <button className="btn-primary" onClick={onNext} disabled={!canAdvance || imageImport.busy}>
          {nextLabel}
        </button>
      </div>
    </div>
  );
}

export function AvatarWizardStep2({
  apiKey,
  videoParameters,
  onVideoParameters,
  providerId,
  credentialConfigured,
  onOpenApiSettings,
  baseUrl,
  onBaseUrl,
  model,
  onModel,
  thinkingModel,
  onThinkingModel,
  imageModel,
  onImageModel,
  advancedOpen,
  onAdvancedOpen,
  openaiCompat,
  onOpenaiCompat,
  advanced,
  onAdvanced,
  onPickProvider,
  canStart,
  generationReadyIssue,
  submitError,
  removeBg,
  onRemoveBg,
  fastGeneration,
  onFastGeneration,
  onBack,
  onStart,
  progress,
  title = "第 2 步 · 生成配置",
  backLabel = "上一步",
  startLabel = "开始生成",
}) {
  const provider = VIDEO_PROVIDERS.find((item) => item.id === providerId) || VIDEO_PROVIDERS[0];
  const isCustom = providerId === "custom";
  const isKling = providerId === "kling";
  const isVolcengine = providerId === "volcengine";
  const [modelReady, setModelReady] = useState(false);

  return (
    <div className="ca-card">
      <h3>{title}</h3>
      <div className="provider-grid">
        {VIDEO_PROVIDERS.map((item) => (
          <button
            key={item.id}
            className={`provider-card ${item.id === providerId ? "active" : ""}`}
            onClick={() => onPickProvider(item.id)}
            type="button"
          >
            <div className="provider-card__name">{item.label}</div>
            <div className="provider-card__sub">{item.sub}</div>
          </button>
        ))}
      </div>

      <div className={`generation-api-status${credentialConfigured ? " is-configured" : ""}`}>
        <span className="generation-api-status__icon" aria-hidden="true">
          {credentialConfigured ? <CheckCircle2 size={18} /> : <KeyRound size={18} />}
        </span>
        <span className="generation-api-status__copy">
          <strong>{credentialConfigured ? "API 凭据已配置" : "API 凭据未配置"}</strong>
          <small>
            {credentialConfigured
              ? `${provider.label} 凭据已从统一配置读取`
              : `请先在 API 配置中补充${isKling ? " Access Key 和 Secret Key" : " API Key"}`}
          </small>
        </span>
        <button type="button" className="btn-secondary btn-sm" onClick={onOpenApiSettings}>
          打开 API 配置
        </button>
      </div>

      {!isVolcengine && (
        <FieldWithHelp label="Base URL" help={HELP_TEXT.baseUrl}>
          <input
            className="field-input"
            value={baseUrl}
            onChange={(event) => onBaseUrl(event.target.value)}
          />
        </FieldWithHelp>
      )}

      <FieldWithHelp label="视频生成模型" help={HELP_TEXT.model}>
        {isVolcengine ? (
          <VideoModelSelect apiKey={apiKey} baseUrl={baseUrl}
            model={model} onModel={onModel} onReady={setModelReady} />
        ) : provider.models.length > 0 ? (
          <select className="field-input" value={model} onChange={(event) => onModel(event.target.value)}>
            {provider.models.map((item) => (
              <option key={item} value={item}>
                {item}
              </option>
            ))}
          </select>
        ) : (
          <input
            className="field-input"
            placeholder="填写模型名称"
            value={model}
            onChange={(event) => onModel(event.target.value)}
          />
        )}
      </FieldWithHelp>

      {isVolcengine && <FieldWithHelp label="图像理解 / 提示词模型" help="默认使用 Lite；可主动选择 2.1 Pro。不会在失败后自动换模型。">
        <TextModelSelect vision label="图像理解模型" value={thinkingModel || ""} onChange={onThinkingModel}
          apiKey={apiKey} baseUrl={baseUrl} />
      </FieldWithHelp>}

      {isVolcengine && <FieldWithHelp label="图片处理模型" help="用于图片预览生成、背景处理和独立动作首帧，需支持图片编辑。">
        <ImageModelSelect value={imageModel || ""} onChange={onImageModel}
          apiKey={apiKey} baseUrl={baseUrl} disabled={!removeBg}
          saveHint="开始生成时保存并同步到 API 配置。" />
        {!removeBg && <p className="muted small">当前使用原图，不调用图片处理模型。</p>}
      </FieldWithHelp>}

      {isCustom && (
        <>
          <label className="row" style={{ display: "flex", gap: 8, alignItems: "center", margin: "4px 0" }}>
            <input
              type="checkbox"
              checked={openaiCompat}
              onChange={(event) => onOpenaiCompat(event.target.checked)}
            />
            <span>OpenAI 兼容鉴权</span>
          </label>
          <details
            open={advancedOpen}
            onToggle={(event) => onAdvancedOpen(event.currentTarget.open)}
            style={{ background: "var(--surface-muted)", borderRadius: 10, padding: "10px 14px" }}
          >
            <summary style={{ cursor: "pointer", fontWeight: 600 }}>高级设置</summary>
            <div style={{ marginTop: 10 }}>
              <AdvField label="鉴权 Header" value={advanced.authHeader} onChange={(value) => onAdvanced({ ...advanced, authHeader: value })} />
              <AdvField label="鉴权前缀" value={advanced.authPrefix} onChange={(value) => onAdvanced({ ...advanced, authPrefix: value })} />
              <AdvField label="创建任务路径" value={advanced.createPath} onChange={(value) => onAdvanced({ ...advanced, createPath: value })} />
              <AdvField label="查询任务路径" value={advanced.queryPath} onChange={(value) => onAdvanced({ ...advanced, queryPath: value })} />
              <AdvField label="Webhook URL（可选）" value={advanced.webhookUrl} onChange={(value) => onAdvanced({ ...advanced, webhookUrl: value })} />
              <div style={{ display: "flex", gap: 12 }}>
                <div className="grow">
                  <AdvField label="超时（毫秒）" value={String(advanced.timeoutMs)} onChange={(value) => onAdvanced({ ...advanced, timeoutMs: Number(value) || 0 })} />
                </div>
                <div className="grow">
                  <AdvField label="轮询间隔（毫秒）" value={String(advanced.pollingIntervalMs)} onChange={(value) => onAdvanced({ ...advanced, pollingIntervalMs: Number(value) || 0 })} />
                </div>
              </div>
              <AdvField label="结果字段路径" value={advanced.resultPath} onChange={(value) => onAdvanced({ ...advanced, resultPath: value })} />
            </div>
          </details>
        </>
      )}

      <label className="row" style={{ display: "flex", gap: 8, alignItems: "center", margin: "12px 0 4px" }}>
        <input
          type="checkbox"
          checked={removeBg}
          onChange={(event) => onRemoveBg(event.target.checked)}
        />
        <span>
          提取完整宠物主体（默认开启）：移除真人手、背景及所有杂物，保留宠物自身肢体和部件，置于纯黑背景。会产生图片处理费用；处理失败会停止，不自动改用原图。
        </span>
      </label>
      {!removeBg && <p role="alert" className="muted small">你已关闭主体清理：原图中的手、桌面、人物和杂物可能进入视频。仅当原图已经干净且宠物完整时使用；下一步仍需确认参考图。</p>}
      <label className="row" style={{ display: "flex", gap: 8, alignItems: "center", margin: "8px 0 4px" }}>
        <input
          type="checkbox"
          checked={fastGeneration}
          onChange={(event) => onFastGeneration(event.target.checked)}
        />
        <span>
          快速生成模式：开启背景处理时合成黑底 4:3 参考帧，分辨率约 {FAST_VIDEO_GENERATION_PROFILE.imageMaxDimension}x{FAST_REFERENCE_HEIGHT}；关闭时保留原图。
          默认使用 5 秒低清视频，视频参数可单独调整。
        </span>
      </label>

      {isVolcengine && (
        <p className="muted small">
          真人照片可能触发 Seedance 的人像或版权审核。真人素材需按火山要求完成认证与肖像授权；当前客户端暂未接入可信素材库。
          图片背景处理成功不代表图生视频审核通过。建议使用符合平台要求的原创、已获授权素材；重复重试无法解决授权问题。
          你也可以使用有权使用的素材，自行生成动态形象并导出 MP4，在「形象画廊 → 新建自定义形象 → 自定义上传视频」中上传；其他动作可在形象详情页通过「上传 MP4 替换」补充。
        </p>
      )}
      {isVolcengine && <VideoGenerationSettings value={videoParameters} onChange={onVideoParameters} fastGeneration={fastGeneration} model={model} />}

      {submitError && (
        <div className="message-banner message-banner--error" style={{ marginTop: 8, whiteSpace: "pre-wrap" }}>
          <AlertCircle size={14} /> {submitError}
        </div>
      )}
      {!submitError && generationReadyIssue && (
        <div className="message-banner message-banner--muted" style={{ marginTop: 8 }}>
          <AlertCircle size={14} /> {generationReadyIssue}
        </div>
      )}

      <div className="ca-actions">
        <button className="btn-ghost" onClick={onBack}>
          {backLabel}
        </button>
        <button className="btn-primary" onClick={onStart} disabled={!canStart || (isVolcengine && !modelReady)} title={generationReadyIssue || startLabel}>
          {startLabel}
        </button>
      </div>
      {progress && (
        <div
          className={`ca-inline-progress ${progress.tone === "success" ? "is-complete" : ""}`}
          role="status"
          aria-live="polite"
        >
          <div className="ca-inline-progress__meta">
            <span>{progress.label}</span>
            <strong>{Math.round(progress.percent)}%</strong>
          </div>
          <div className="ca-inline-progress__bar" aria-hidden="true">
            <span style={{ width: `${Math.max(0, Math.min(100, progress.percent))}%` }} />
          </div>
          {progress.message && <div className="ca-inline-progress__message">{progress.message}</div>}
        </div>
      )}
    </div>
  );
}

function FieldWithHelp({ label, help, children }) {
  return (
    <div className="field">
      <div className="field-label field-label--with-help">
        <span>{label}</span>
        <HelpTooltip content={help} label={`${label} 说明`} />
      </div>
      {children}
    </div>
  );
}

function AdvField({ label, value, onChange }) {
  return (
    <div className="field">
      <label className="field-label">{label}</label>
      <input className="field-input" value={value || ""} onChange={(event) => onChange(event.target.value)} />
    </div>
  );
}
