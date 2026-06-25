/**
 * [Input] User-uploaded image + provider config; orchestrates `lib/avatar-pipeline/run.js`.
 * [Output] On success persists via `lib/appearance-store.js`, with clear GIF first-frame copy,
 *          fixed-size reference upload preview, unified field help, shared provider-config persistence,
 *          Volcengine Ark API-key-only product-fit model dropdown with Seedance 1.5 first and 2.0 activation guidance,
 *          fast low-resolution defaults, reusable step components, and preflight generation requirements.
 * [Pos] component node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import React, { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { ArrowLeft, ImageUp, AlertCircle } from "lucide-react";
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
  VOLCENGINE_CUSTOM_MODEL_OPTION,
  VOLCENGINE_THINKING_MODEL,
  loadProviderConfig,
  saveProviderConfig,
} from "./lib/avatar-pipeline/provider-config.js";
import HelpTooltip from "./HelpTooltip.jsx";

const FAST_REFERENCE_HEIGHT = Math.round(
  (FAST_VIDEO_GENERATION_PROFILE.imageMaxDimension * PIPELINE_OUTPUT_ASPECT_RATIO.height) /
    PIPELINE_OUTPUT_ASPECT_RATIO.width,
);

const HELP_TEXT = {
  apiKey:
    "填写视频服务或聚合平台提供的 API Key。通常可以在控制台的 API Keys、密钥管理、Access Token 或应用凭证页面找到。",
  baseUrl:
    "填写接口根地址，例如平台给出的 API 域名。请保留 https://，不要把具体接口路径重复填进来。",
  model:
    "下拉只保留当前新形象流程适配过的图生视频模型。火山 Ark 可用的新模型可以通过“自定义模型名称”兜底填写。",
};

export default function CustomAvatarWizard({ onExit }) {
  const [step, setStep] = useState(0);
  const [file, setFile] = useState(null);
  const [previewUrl, setPreviewUrl] = useState("");
  const [appearanceName, setAppearanceName] = useState("");
  const [personality, setPersonality] = useState("");

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
  const [advancedOpen, setAdvancedOpen] = useState(false);
  const [openaiCompat, setOpenaiCompat] = useState(true);
  const [advanced, setAdvanced] = useState({ ...DEFAULT_ADVANCED });
  const [testFeedback, setTestFeedback] = useState(null);
  const [removeBg, setRemoveBg] = useState(true);
  const [fastGeneration, setFastGeneration] = useState(true);

  const [submitError, setSubmitError] = useState("");
  const [taskRunning, setTaskRunning] = useState(() => isGenerationRunning());

  useEffect(
    () =>
      subscribeGenerationTask((state) => {
        setTaskRunning(state.status === "running");
      }),
    [],
  );

  useEffect(() => {
    const saved = loadProviderConfig(providerId);
    setApiKey(saved.apiKey);
    setAccessKey(saved.accessKey);
    setSecretKey(saved.secretKey);
    setBaseUrl(saved.baseUrl);
    setModel(saved.model);
    setThinkingModel(saved.thinkingModel);
    setFastGeneration(saved.fastGeneration);
    setAdvanced(saved.advanced);
    setTestFeedback(null);
  }, [providerId]);

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
    if (providerId === "kling") {
      if (!accessKey.trim() || !secretKey.trim()) {
        return "请先填写 Kling Access Key 和 Secret Key。";
      }
      if (!baseUrl.trim() || !model.trim()) {
        return "请先填写接口地址和视频生成模型。";
      }
      return "";
    }

    if (isVolcengine && (!apiKey.trim() || !model.trim())) {
      return "请先填写 API Key 和视频生成模型。";
    }
    if (!apiKey.trim()) return "请先填写 API Key。";
    if (!baseUrl.trim() || !model.trim()) return "请先填写接口地址和视频生成模型。";
    return "";
  }, [providerId, accessKey, secretKey, apiKey, baseUrl, model, isVolcengine]);

  const canStartGenerate = !generationReadyIssue;

  const persistConfig = useCallback(() => {
    saveProviderConfig(providerId, {
      apiKey,
      accessKey,
      secretKey,
      baseUrl,
      model,
      thinkingModel,
      fastGeneration,
      advanced,
    });
  }, [providerId, apiKey, accessKey, secretKey, baseUrl, model, thinkingModel, fastGeneration, advanced]);

  const handleTestConnection = useCallback(() => {
    const key = providerId === "kling" ? accessKey.trim() : apiKey.trim();
    const normalizedBaseUrl = baseUrl.trim();
    if (isVolcengine) {
      if (!key) {
        setTestFeedback({ tone: "warning", text: "请先填写 API Key。" });
        return;
      }
      setTestFeedback({
        tone: "success",
        text: `火山 Ark 地址已固定，请求会发送到 ${VOLCENGINE_BASE_URL}`,
      });
      return;
    }
    if (!key || !normalizedBaseUrl) {
      setTestFeedback({ tone: "warning", text: "请先填写 API Key 和 Base URL。" });
      return;
    }
    try {
      const url = new URL(
        /^https?:\/\//i.test(normalizedBaseUrl) ? normalizedBaseUrl : `https://${normalizedBaseUrl}`,
      );
      setTestFeedback({
        tone: "success",
        text: `基础地址校验通过，请求会发送到 ${url.origin}`,
      });
    } catch {
      setTestFeedback({ tone: "danger", text: "Base URL 格式不正确。" });
    }
  }, [providerId, apiKey, accessKey, baseUrl, isVolcengine]);

  const handleStartGenerate = useCallback(() => {
    if (!file) return;
    if (taskRunning) {
      setSubmitError("已有一个生成任务正在进行中，请等待当前任务完成后再开始。");
      return;
    }
    persistConfig();
    setSubmitError("");

    const trimmedModel = model.trim();
    const providerConfig = {
      provider: providerId,
      apiKey,
      accessKey,
      secretKey,
      baseUrl: isVolcengine ? VOLCENGINE_BASE_URL : baseUrl,
      model: trimmedModel,
      thinkingModel: isVolcengine ? VOLCENGINE_THINKING_MODEL : thinkingModel.trim() || trimmedModel,
      fastGeneration,
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
        providerConfig,
        skipProcessing: !removeBg,
      });
    } catch (error) {
      setSubmitError(error?.message || String(error));
      return;
    }

    onExit?.();
  }, [
    file,
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
    advanced,
    openaiCompat,
    appearanceName,
    personality,
    removeBg,
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
          {["上传参考图", "生成配置"].map((label, index) => (
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
            canAdvance={canAdvanceFromStep1}
            onNext={() => setStep(1)}
          />
        )}

        {step === 1 && (
          <AvatarWizardStep2
            providerId={providerId}
            apiKey={apiKey}
            onApiKey={setApiKey}
            accessKey={accessKey}
            onAccessKey={setAccessKey}
            secretKey={secretKey}
            onSecretKey={setSecretKey}
            baseUrl={baseUrl}
            onBaseUrl={setBaseUrl}
            model={model}
            onModel={setModel}
            advancedOpen={advancedOpen}
            onAdvancedOpen={setAdvancedOpen}
            openaiCompat={openaiCompat}
            onOpenaiCompat={setOpenaiCompat}
            advanced={advanced}
            onAdvanced={setAdvanced}
            testFeedback={testFeedback}
            onPickProvider={setProviderId}
            onTestConnection={handleTestConnection}
            canStart={canStartGenerate && !taskRunning}
            generationReadyIssue={generationReadyIssue}
            submitError={submitError}
            removeBg={removeBg}
            onRemoveBg={setRemoveBg}
            fastGeneration={fastGeneration}
            onFastGeneration={setFastGeneration}
            onBack={() => setStep(0)}
            onStart={handleStartGenerate}
          />
        )}
      </div>
    </div>
  );
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
  return (
    <div className="ca-card">
      <h3>{title}</h3>
      <div
        className="dropzone"
        onClick={onPickClick}
        onDrop={onDrop}
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
            <div className="muted small">支持 PNG / JPEG / WebP / GIF，GIF 会取首帧作为参考图。</div>
          </>
        )}
        <input
          ref={inputRef}
          type="file"
          accept="image/png,image/jpeg,image/webp,image/gif,image/*"
          style={{ display: "none" }}
          onChange={(event) => {
            onFileChange(event.target.files?.[0]);
            event.target.value = "";
          }}
        />
      </div>

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
      ) : (
        children
      )}
      <div className="ca-actions">
        <button className="btn-ghost" onClick={onCancel}>
          {cancelLabel}
        </button>
        <button className="btn-primary" onClick={onNext} disabled={!canAdvance}>
          {nextLabel}
        </button>
      </div>
    </div>
  );
}

export function AvatarWizardStep2({
  providerId,
  apiKey,
  onApiKey,
  accessKey,
  onAccessKey,
  secretKey,
  onSecretKey,
  baseUrl,
  onBaseUrl,
  model,
  onModel,
  advancedOpen,
  onAdvancedOpen,
  openaiCompat,
  onOpenaiCompat,
  advanced,
  onAdvanced,
  testFeedback,
  onPickProvider,
  onTestConnection,
  canStart,
  generationReadyIssue,
  submitError,
  removeBg,
  onRemoveBg,
  fastGeneration,
  onFastGeneration,
  onBack,
  onStart,
  title = "第 2 步 · 生成配置",
  backLabel = "上一步",
  startLabel = "开始生成",
}) {
  const provider = VIDEO_PROVIDERS.find((item) => item.id === providerId) || VIDEO_PROVIDERS[0];
  const isCustom = providerId === "custom";
  const isKling = providerId === "kling";
  const isVolcengine = providerId === "volcengine";
  const isVolcengineKnownModel = isVolcengine && provider.models.includes(model);
  const volcengineModelSelectValue = isVolcengineKnownModel
    ? model
    : VOLCENGINE_CUSTOM_MODEL_OPTION;

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

      {isKling ? (
        <>
          <div className="field">
            <label className="field-label">Access Key</label>
            <input
              className="field-input"
              type="password"
              placeholder="Kling Access Key"
              value={accessKey}
              onChange={(event) => onAccessKey(event.target.value)}
            />
          </div>
          <div className="field">
            <label className="field-label">Secret Key</label>
            <input
              className="field-input"
              type="password"
              placeholder="Kling Secret Key"
              value={secretKey}
              onChange={(event) => onSecretKey(event.target.value)}
            />
          </div>
        </>
      ) : (
        <FieldWithHelp label="API Key" help={HELP_TEXT.apiKey}>
          <input
            className="field-input"
            type="password"
            placeholder="输入 API Key"
            value={apiKey}
            onChange={(event) => onApiKey(event.target.value)}
          />
        </FieldWithHelp>
      )}

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
          <>
            <select
              className="field-input"
              value={volcengineModelSelectValue}
              onChange={(event) => {
                const nextModel = event.target.value;
                onModel(nextModel === VOLCENGINE_CUSTOM_MODEL_OPTION ? "" : nextModel);
              }}
            >
              {provider.models.map((item) => (
                <option key={item} value={item}>
                  {item}
                </option>
              ))}
              <option value="__custom__">自定义模型名称</option>
            </select>
            {volcengineModelSelectValue === VOLCENGINE_CUSTOM_MODEL_OPTION && (
              <input
                className="field-input"
                style={{ marginTop: 8 }}
                placeholder="填写火山 Ark 模型名称"
                value={model}
                onChange={(event) => onModel(event.target.value)}
              />
            )}
            <div className="field-helper">
              默认只列出适配当前单图新形象生成流程的 Seedance 模型；Seedance 2.0 如果返回 ModelNotOpen，需要先在 Ark 控制台开通。新的兼容模型名可通过“自定义模型名称”填写。
            </div>
          </>
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
        <span>自动去除背景并替换为黑色，首次加载会下载约 40MB 模型。</span>
      </label>
      <label className="row" style={{ display: "flex", gap: 8, alignItems: "center", margin: "8px 0 4px" }}>
        <input
          type="checkbox"
          checked={fastGeneration}
          onChange={(event) => onFastGeneration(event.target.checked)}
        />
        <span>
          快速生成模式：抠图后合成黑底 4:3 参考帧，分辨率约 {FAST_VIDEO_GENERATION_PROFILE.imageMaxDimension}x{FAST_REFERENCE_HEIGHT}，
          使用 5 秒低清视频与更快链路。
        </span>
      </label>

      <div className="row" style={{ display: "flex", alignItems: "center", marginTop: 8 }}>
        <button className="btn-secondary btn-sm" onClick={onTestConnection}>
          测试连接
        </button>
        {testFeedback && (
          <span className={`test-feedback test-feedback--${testFeedback.tone}`}>
            {testFeedback.text}
          </span>
        )}
      </div>

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
        <button className="btn-primary" onClick={onStart} disabled={!canStart} title={generationReadyIssue || startLabel}>
          {startLabel}
        </button>
      </div>
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
