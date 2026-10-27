/**
 * [Input] Shared avatar provider configs including persisted video parameters, native Volcengine ASR credential commands, and optional return navigation.
 * [Output] Independent LLM/ASR reads, honest credential-save errors and retained navigation drafts; saved ASR changes are broadcast with save vs. test results distinguished.
 * [Pos] top-level page node in pc/src
 * [Sync] If this file changes, update this header and `pc/src/.folder.md`.
 */

import React, { useEffect, useMemo, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import {
  AlertCircle,
  ArrowLeft,
  CheckCircle2,
  Eye,
  EyeOff,
  ExternalLink,
  KeyRound,
  Loader2,
  ShieldCheck,
  Sparkles,
} from "lucide-react";
import PageShell from "./shell/PageShell.jsx";
import Card from "./shell/Card.jsx";
import LlmNetworkSettings from "./LlmNetworkSettings.jsx";
import GenerationDiagnostics from "./GenerationDiagnostics.jsx";
import TextModelSelect from "./TextModelSelect.jsx";
import { VideoModelSelect } from "./VideoGenerationSettings.jsx";
import { useUsageHelp } from "./shell/DeviceContext.jsx";
import UsageHelp from "./UsageHelp.jsx";
import { useExitProtection } from "./lib/exit-protection.js";
import {
  VIDEO_PROVIDERS,
  loadProviderConfig,
  saveProviderConfig,
} from "./lib/avatar-pipeline/provider-config.js";
import {
  LLM_PRESETS,
  DEFAULT_LLM_PROVIDER,
  describeLlmPreset,
  selectLlmProvider,
  changeLlmEndpoint,
  emitApiConfigurationUpdated,
  providerCredentialsConfigured,
} from "./lib/api-configuration.js";

function loadVideoConfigs() {
  return Object.fromEntries(
    VIDEO_PROVIDERS.map((provider) => [provider.id, loadProviderConfig(provider.id)]),
  );
}
function persistedProviderConfig(config) {
  return {
    apiKey: config.apiKey || "",
    accessKey: config.accessKey || "",
    secretKey: config.secretKey || "",
    baseUrl: config.baseUrl || "",
    model: config.model || "",
    thinkingModel: config.thinkingModel || "",
    fastGeneration: config.fastGeneration !== false,
    videoParameters: config.videoParameters || {},
    advanced: config.advanced || {},
  };
}

function resultToneClass(tone) {
  return tone ? ` is-${tone}` : "";
}

export default function ApiSettings({ onBack, configReadError = "" }) {
  const usageHelp = useUsageHelp();
  const [showSecrets, setShowSecrets] = useState(false);
  const [videoConfigs, setVideoConfigs] = useState(loadVideoConfigs);
  const [savedVideoConfigs, setSavedVideoConfigs] = useState(() => videoConfigs);
  const [providerResults, setProviderResults] = useState({});
  const [providerPending, setProviderPending] = useState({});
  const [savedLlm, setSavedLlm] = useState(null);
  const [asrApiKey, setAsrApiKey] = useState("");
  const [asrState, setAsrState] = useState({
    loading: true,
    pending: false,
    configured: false,
    tone: "muted",
    message: "正在读取语音识别配置…",
  });
  // 实时对话：服务预填默认模型并允许修改；豆包 Seed TTS 2.0 复用识别 Key。
  const [voiceChat, setVoiceChat] = useState({
    loading: true,
    llmPending: false,
    llmConfigured: false,
    llmProvider: DEFAULT_LLM_PROVIDER,
    llmBaseUrl: "",
    llmModel: LLM_PRESETS.find(preset => preset.id === DEFAULT_LLM_PROVIDER).model,
    llmWebSearch: true,
    llmApiKey: "",
    llmApiKeyMasked: "",
    llmApiKeyMasks: {},
    llmCustomBaseUrl: "",
    llmTone: "muted",
    llmMessage: "正在读取对话大模型配置…",
  });
  const llmDraft = JSON.stringify([voiceChat.llmProvider, voiceChat.llmBaseUrl, voiceChat.llmModel,
    voiceChat.llmWebSearch, voiceChat.llmApiKey]);
  useEffect(() => { if (!voiceChat.loading && savedLlm === null) setSavedLlm(llmDraft); }, [voiceChat.loading, savedLlm, llmDraft]);
  const dirty = JSON.stringify(videoConfigs) !== JSON.stringify(savedVideoConfigs)
    || !!asrApiKey || (savedLlm !== null && savedLlm !== llmDraft);
  const saving = voiceChat.llmPending || asrState.pending || Object.values(providerPending).some(Boolean);
  useExitProtection(saving ? "API 配置正在保存或测试" : dirty ? "API 配置有未保存的修改" : "");

  const applyVoiceChatStatus = (status, patch = {}) => {
    setVoiceChat((current) => ({
      ...current,
      ...patch,
      loading: false,
      llmConfigured: status?.llmConfigured === true,
      llmWebSearch: status?.llmWebSearch !== false,
      llmProvider: status?.llmProvider || current.llmProvider,
      llmBaseUrl: status?.llmBaseUrl || "",
      llmModel: status?.llmModel || "",
      llmApiKeyMasked: status?.llmApiKeyMasked || "",
      llmApiKeyMasks: status?.llmApiKeyMasks || {},
      llmCustomBaseUrl: status?.llmCustomBaseUrl || "",
    }));
  };

  const saveLlm = async () => {
    setVoiceChat((current) => ({ ...current, llmPending: true, llmTone: "muted", llmMessage: "正在保存对话大模型配置…" }));
    try {
      const custom = voiceChat.llmProvider === "custom";
      const status = await invoke("save_voice_chat_settings", {
        input: {
          llmProvider: voiceChat.llmProvider,
          llmWebSearch: voiceChat.llmWebSearch,
          llmBaseUrl: custom ? voiceChat.llmBaseUrl : null,
          llmModel: voiceChat.llmModel.trim(),
          llmApiKey: voiceChat.llmApiKey.trim() || null,
        },
      });
      emitApiConfigurationUpdated({ providerId: "voice-chat-llm", configured: status?.llmConfigured === true });
      applyVoiceChatStatus(status, {
        llmPending: false,
        llmApiKey: "",
        llmTone: status?.llmConfigured ? "success" : "warning",
        llmMessage: status?.llmConfigured ? "配置已保存，开始聊天时将使用所选模型。" : "已保存，但还没有 API Key",
      });
      setSavedLlm(null);
    } catch (error) {
      setVoiceChat((current) => ({ ...current, llmPending: false, llmTone: "error", llmMessage: `保存失败：${error}` }));
    }
  };

  useEffect(() => {
    let cancelled = false;
    invoke("load_voice_chat_settings")
      .then((status) => {
        if (!cancelled) applyVoiceChatStatus(status, {
          llmTone: status?.llmConfigured ? "success" : "muted",
          llmMessage: status?.llmConfigured ? "对话大模型已配置" : "选一家大模型，填入 API Key 即可",
        });
      })
      .catch((error) => {
        if (!cancelled) setVoiceChat((current) => ({ ...current, loading: false,
          llmTone: "error", llmMessage: `读取对话模型配置失败：${error}` }));
      });
    // Independent services: a failed LLM read must not suppress the ASR read.
    invoke("load_device_asr_settings")
      .then((status) => {
        if (cancelled) return;
        setAsrState({
          loading: false,
          pending: false,
          configured: status?.configured === true,
          tone: status?.configured ? "success" : "muted",
          message: status?.message || "尚未配置火山引擎语音识别 API Key",
        });
      })
      .catch((error) => {
        if (cancelled) return;
        setAsrState({
          loading: false,
          pending: false,
          configured: false,
          tone: "error",
          message: `读取语音识别配置失败：${error}`,
        });
      });
    return () => {
      cancelled = true;
    };
  }, []);

  const configuredProviderCount = useMemo(
    () => VIDEO_PROVIDERS.filter((provider) =>
      providerCredentialsConfigured(provider.id, savedVideoConfigs[provider.id]),
    ).length,
    [savedVideoConfigs],
  );
  const configuredCount = configuredProviderCount + (asrState.configured ? 1 : 0)
    + (voiceChat.llmConfigured ? 1 : 0);

  const updateProvider = (providerId, patch) => {
    setVideoConfigs((current) => ({
      ...current,
      [providerId]: {
        ...current[providerId],
        ...patch,
      },
    }));
    setProviderResults((current) => ({ ...current, [providerId]: null }));
  };

  const saveProvider = async (providerId) => {
    if (providerPending[providerId]) return;
    const config = videoConfigs[providerId];
    if (!providerCredentialsConfigured(providerId, config)) {
      setProviderResults((current) => ({
        ...current,
        [providerId]: {
          tone: "warning",
          message: providerId === "kling"
            ? "请填写 Access Key 和 Secret Key。"
            : "请填写 API Key。",
        },
      }));
      return;
    }
    if (providerId === "custom") {
      try {
        new URL(config.baseUrl);
      } catch {
        setProviderResults((current) => ({
          ...current,
          [providerId]: { tone: "error", message: "请填写完整且有效的 Base URL。" },
        }));
        return;
      }
    }
    setProviderPending(current => ({ ...current, [providerId]: true }));
    try {
      await saveProviderConfig(providerId, persistedProviderConfig(config));
    } catch (error) {
      setProviderResults((current) => ({ ...current,
        [providerId]: { tone: "error", message: error.message || "配置未保存，请重试。" },
      }));
      return;
    } finally {
      setProviderPending(current => ({ ...current, [providerId]: false }));
    }
    setSavedVideoConfigs(current => ({ ...current, [providerId]: config }));
    setProviderResults((current) => ({
      ...current,
      [providerId]: { tone: "success", message: "已保存到当前客户端。" },
    }));
    emitApiConfigurationUpdated({ providerId });
  };

  const saveAndTestAsr = async () => {
    if (!asrState.configured && !asrApiKey.trim()) {
      setAsrState((current) => ({
        ...current,
        tone: "warning",
        message: "请填写火山引擎语音识别 API Key。",
      }));
      return;
    }
    let saved = null;
    setAsrState((current) => ({
      ...current,
      pending: true,
      tone: "muted",
      message: "正在保存并测试语音识别服务…",
    }));
    try {
      saved = await invoke("save_device_asr_settings", {
        input: {
          apiKey: asrApiKey.trim() || null,
        },
      });
      emitApiConfigurationUpdated({
        providerId: "volcengine-asr",
        configured: saved?.configured === true,
      });
      setAsrApiKey("");
      const probe = await invoke("test_device_asr_settings");
      setAsrApiKey("");
      setAsrState({
        loading: false,
        pending: false,
        configured: saved?.configured === true,
        tone: "success",
        message: `Key 已保存，语音识别测试通过（${probe?.latencyMs ?? 0} ms）。可到「人设与声音」试听，检查语音合成是否可用。`,
      });
    } catch (error) {
      setAsrState((current) => ({
        ...current,
        loading: false,
        pending: false,
        configured: saved?.configured === true || current.configured,
        tone: "error",
        message: `${saved ? "共享 Key 已保存，但识别测试失败" : "保存失败"}：${error}`,
      }));
    }
  };

  return (
    <div className="api-settings-page">
    <PageShell
      title="API 配置"
      subtitle="按需配置语音输入、宠物聊天和形象生成服务"
      actions={(
        <div className="api-settings__page-actions">
          <button
            type="button"
            className="btn-secondary btn-sm"
            onClick={() => setShowSecrets((current) => !current)}
          >
            {showSecrets ? <EyeOff size={14} /> : <Eye size={14} />}
            {showSecrets ? "隐藏输入" : "显示输入"}
          </button>
          {onBack && (
            <button type="button" className="btn-ghost btn-sm" onClick={onBack}>
              <ArrowLeft size={14} />
              返回
            </button>
          )}
        </div>
      )}
    >
      {dirty && <p role="status" className="api-settings__draft">有未保存的修改。切换页面会保留草稿；关闭客户端前请保存。</p>}
      <section className="api-settings__overview" aria-label="API 配置概览">
        <div className="api-settings__overview-icon" aria-hidden="true">
          <ShieldCheck size={22} />
        </div>
        <div className="api-settings__overview-copy">
          <strong>{configReadError ? "已有配置暂时无法读取（不是配置丢失）" : `${configuredCount} / ${VIDEO_PROVIDERS.length + 2} 项凭据已配置`}</strong>
          <span>按需配置即可：给 Agent 语音输入只需语音识别；和宠物聊天还需语音合成与对话大模型；生成形象需配置形象生成服务。</span>
        </div>
        <div className="api-settings__storage-note">
          <KeyRound size={14} aria-hidden="true" />
          API Key 请勿分享给他人
        </div>
      </section>

      <Card
        title="语音识别与合成"
        subtitle={(
          <>
            让设备听懂你说的话，并让宠物开口回应。识别与合成共用一个 API Key。
            <a
              className="api-settings__credential-link"
              href="https://docs.volcengine.com/docs/DoubaoVoice/APIKeyUsage?lang=zh"
              target="_blank"
              rel="noreferrer"
            >
              Key 获取方式
              <ExternalLink size={11} aria-hidden="true" />
            </a>
          </>
        )}
        actions={(
          <span className={`api-settings__status${asrState.configured ? " is-success" : ""}`}>
            {asrState.loading ? <Loader2 size={13} className="spin" /> : <CheckCircle2 size={13} />}
            {asrState.loading ? "读取中" : asrState.configured ? "已配置" : "未配置"}
          </span>
        )}
      >
        <div className="usage-help">
          <p><strong>语音输入：</strong>{usageHelp.voice} {usageHelp.confirm}</p>
          <p><strong>实时对话：</strong>{usageHelp.chat} 还需配置下方「对话大模型」。</p>
          {usageHelp.pending && <p className="usage-help__notice">按键配置有修改，请先同步到设备。</p>}
          <details><summary>使用前需要什么</summary><p>请保持设备连接电脑，并让 Pet Manager 持续运行。默认使用豆包 ASR 2.0 和 TTS 2.0，请为该 Key 开通对应服务。</p></details>
        </div>
        <div className="api-settings__form api-settings__form--speech">
          <label className="ui-field" htmlFor="api-settings-asr-key">
            <span className="ui-field__label">语音 API Key（识别与合成共用）</span>
            <input
              id="api-settings-asr-key"
              className="ui-control api-settings__secret-input"
              type={showSecrets ? "text" : "password"}
              autoComplete="new-password"
              value={asrApiKey}
              onChange={(event) => setAsrApiKey(event.target.value)}
              placeholder={asrState.configured ? "已安全保存；留空可直接复测" : "输入火山引擎豆包语音 API Key"}
              disabled={asrState.pending || asrState.loading}
            />
          </label>
          <button
            type="button"
            className="btn-primary btn-sm api-settings__save"
            onClick={saveAndTestAsr}
            disabled={asrState.pending || asrState.loading}
          >
            {asrState.pending ? <Loader2 size={14} className="spin" /> : <CheckCircle2 size={14} />}
            保存并测试
          </button>
        </div>
        <div className={`api-settings__result${resultToneClass(asrState.tone)}`} role="status">
          {asrState.tone === "error" || asrState.tone === "warning"
            ? <AlertCircle size={13} />
            : <CheckCircle2 size={13} />}
          {asrState.message}
        </div>
      </Card>

      <Card title="对话大模型" subtitle="决定宠物在「实时对话」中如何理解和回答。选择一家服务并填写 API Key 即可。此配置用于和宠物聊天，不影响 ChatGPT、Claude 等 Agent 的语音输入。">
        <UsageHelp />
        <div className="api-settings__presets" role="radiogroup" aria-label="对话大模型">
          {[...LLM_PRESETS, { id: "custom", label: "自定义" }].map((preset) => (
            <button
              key={preset.id}
              type="button"
              role="radio"
              aria-checked={voiceChat.llmProvider === preset.id}
              className={"btn-ghost btn-sm api-settings__preset" + (voiceChat.llmProvider === preset.id ? " is-active" : "")}
              onClick={() => setVoiceChat((current) => selectLlmProvider(current, preset.id))}
              disabled={voiceChat.llmPending || voiceChat.loading}
            >
              {preset.label}
            </button>
          ))}
        </div>
        <div className="api-settings__form api-settings__form--llm">
          {voiceChat.llmProvider === "custom" ? (
            <>
              <label className="ui-field" htmlFor="api-settings-llm-url">
                <span className="ui-field__label">接口地址（OpenAI 兼容）</span>
                <input
                  id="api-settings-llm-url"
                  className="ui-control"
                  value={voiceChat.llmBaseUrl}
                  onChange={(event) => setVoiceChat((current) => changeLlmEndpoint(current, event.target.value))}
                  placeholder="https://your-endpoint/v1"
                  disabled={voiceChat.llmPending || voiceChat.loading}
                />
              </label>
            </>
          ) : (
            <div className="api-settings__preset-summary muted small">
              {describeLlmPreset(voiceChat.llmProvider, voiceChat.llmModel)}
            </div>
          )}
          <label className="ui-field" htmlFor="api-settings-llm-model">
            <span className="ui-field__label">模型名称 / 接入点 ID</span>
            <TextModelSelect value={voiceChat.llmModel}
              onChange={llmModel => setVoiceChat(current => ({ ...current, llmModel }))}
              provider={voiceChat.llmProvider} baseUrl={voiceChat.llmBaseUrl}
              apiKey={voiceChat.llmApiKey} configured={!voiceChat.loading && voiceChat.llmConfigured}
              disabled={voiceChat.llmPending || voiceChat.loading} />
            <span id="api-settings-llm-model-help" className="muted small">可填写账号已开通的模型名称或接入点 ID；留空使用默认模型。</span>
          </label>
          <label className="ui-field" htmlFor="api-settings-llm-key">
            <span className="ui-field__label">API Key</span>
            <input
              id="api-settings-llm-key"
              className="ui-control api-settings__secret-input"
              type={showSecrets ? "text" : "password"}
              autoComplete="new-password"
              value={voiceChat.llmApiKey}
              onChange={(event) => setVoiceChat((current) => ({ ...current, llmApiKey: event.target.value }))}
              placeholder={voiceChat.llmConfigured ? `已保存 ${voiceChat.llmApiKeyMasked}；留空保持不变` : "粘贴这家模型的 API Key"}
              disabled={voiceChat.llmPending || voiceChat.loading}
            />
          </label>
          <button
            type="button"
            className="btn-primary btn-sm api-settings__save"
            onClick={saveLlm}
            disabled={voiceChat.llmPending || voiceChat.loading}
          >
            {voiceChat.llmPending ? <Loader2 size={14} className="spin" /> : <CheckCircle2 size={14} />}
            保存
          </button>
        </div>
        <div className={`api-settings__result${resultToneClass(voiceChat.llmTone)}`} role="status">
          {voiceChat.llmTone === "error" || voiceChat.llmTone === "warning"
            ? <AlertCircle size={13} />
            : <CheckCircle2 size={13} />}
          {voiceChat.llmMessage}
        </div>
        <label className="ui-field api-settings__search-option">
          <span><input type="checkbox" checked={voiceChat.llmWebSearch}
            disabled={voiceChat.llmPending || voiceChat.loading}
            onChange={event => setVoiceChat(current => ({ ...current, llmWebSearch: event.target.checked }))} /> 按需联网检索</span>
          <span className="muted small">询问最新消息时查询，闲聊和家居控制不查询。使用当前服务商的 Key；模型需支持原生搜索，账号可能需开通权限并产生额外费用。查询后由宠物直接回答。修改后点击保存，下次开始聊天生效。</span>
        </label>
        <LlmNetworkSettings />
      </Card>

      <Card title="形象生成" subtitle="用于新形象生成与单状态视频替换">
        <GenerationDiagnostics />
        <div className="api-settings__provider-grid">
          {VIDEO_PROVIDERS.map((provider) => {
            const config = videoConfigs[provider.id];
            const configured = providerCredentialsConfigured(provider.id, savedVideoConfigs[provider.id]);
            const result = providerResults[provider.id];
            const isKling = provider.id === "kling";
            const isCustom = provider.id === "custom";
            return (
              <section className="api-settings__provider" key={provider.id}>
                <header className="api-settings__provider-head">
                  <span className="api-settings__service-mark" aria-hidden="true">
                    <Sparkles size={18} />
                  </span>
                  <span className="api-settings__provider-title">
                    <strong>{provider.label}</strong>
                    <small>
                      {provider.sub}
                      {provider.id === "volcengine" && (
                        <a
                          className="api-settings__credential-link"
                          href="https://ark.volcengine.com/model/detail?name=doubao-seedance-2-0-mini"
                          target="_blank"
                          rel="noreferrer"
                        >
                          Key 获取方式
                          <ExternalLink size={11} aria-hidden="true" />
                        </a>
                      )}
                    </small>
                  </span>
                  <span className={`api-settings__status${configured ? " is-success" : ""}`}>
                    {configReadError ? "读取失败" : configured ? "已配置" : "未配置"}
                  </span>
                </header>

                <fieldset className="api-settings__provider-fields" disabled={Boolean(configReadError)} style={{border:0,padding:0,margin:0,minWidth:0}}>
                  {isKling ? (
                    <>
                      <label className="ui-field" htmlFor="api-settings-kling-access">
                        <span className="ui-field__label">Access Key</span>
                        <input
                          id="api-settings-kling-access"
                          className="ui-control api-settings__secret-input"
                          type={showSecrets ? "text" : "password"}
                          autoComplete="new-password"
                          value={config.accessKey}
                          onChange={(event) => updateProvider(provider.id, { accessKey: event.target.value })}
                          placeholder="Kling Access Key"
                        />
                      </label>
                      <label className="ui-field" htmlFor="api-settings-kling-secret">
                        <span className="ui-field__label">Secret Key</span>
                        <input
                          id="api-settings-kling-secret"
                          className="ui-control api-settings__secret-input"
                          type={showSecrets ? "text" : "password"}
                          autoComplete="new-password"
                          value={config.secretKey}
                          onChange={(event) => updateProvider(provider.id, { secretKey: event.target.value })}
                          placeholder="Kling Secret Key"
                        />
                      </label>
                    </>
                  ) : (
                    <label className="ui-field" htmlFor={`api-settings-${provider.id}-key`}>
                      <span className="ui-field__label">API Key</span>
                      <input
                        id={`api-settings-${provider.id}-key`}
                        className="ui-control api-settings__secret-input"
                        type={showSecrets ? "text" : "password"}
                        autoComplete="new-password"
                        value={config.apiKey === "hachimidock-bundled-ark-v1" ? "" : config.apiKey}
                        onChange={(event) => updateProvider(provider.id, { apiKey: event.target.value })}
                        placeholder={config.apiKey === "hachimidock-bundled-ark-v1" ? "内置服务已启用，可填写自己的 Key 替换" : `输入${provider.label} API Key`}
                      />
                    </label>
                  )}
                  {isCustom && (
                    <label className="ui-field" htmlFor="api-settings-custom-url">
                      <span className="ui-field__label">Base URL</span>
                      <input
                        id="api-settings-custom-url"
                        className="ui-control api-settings__secret-input"
                        value={config.baseUrl}
                        onChange={(event) => updateProvider(provider.id, { baseUrl: event.target.value })}
                        placeholder="https://api.example.com"
                      />
                    </label>
                  )}
                  {provider.id === "volcengine" && <div className="ui-field">
                    <span className="ui-field__label">视频生成模型</span>
                    <VideoModelSelect apiKey={config.apiKey} baseUrl={config.baseUrl}
                      model={config.model} onModel={model => updateProvider(provider.id, { model })}
                      autoSelectDefault={false} />
                    <span className="muted small">用于生成形象视频；与下方提示词模型独立。修改后点击保存。</span>
                  </div>}
                  {provider.id === "volcengine" && <div className="ui-field">
                    <span className="ui-field__label">图像理解 / 提示词模型</span>
                    <TextModelSelect vision label="图像理解模型" value={config.thinkingModel}
                      onChange={thinkingModel => updateProvider(provider.id, { thinkingModel })}
                      apiKey={config.apiKey} baseUrl={config.baseUrl} />
                  </div>}
                </fieldset>

                <footer className="api-settings__provider-foot">
                  <span className={`api-settings__provider-result${resultToneClass(result?.tone)}`}>
                    {result?.message || (configReadError ? "原配置保留，暂不可修改" : configured ? "可用于形象生成" : "等待填写凭据")}
                  </span>
                  <button
                    type="button"
                    className="btn-secondary btn-sm"
                    onClick={() => saveProvider(provider.id)}
                    disabled={providerPending[provider.id] === true || Boolean(configReadError)}
                  >
                    {providerPending[provider.id] ? "保存中…" : "保存"}
                  </button>
                </footer>
              </section>
            );
          })}
        </div>
      </Card>
    </PageShell>
    </div>
  );
}
