/**
 * [Input] Shared avatar provider configs, platform-specific native Volcengine ASR credential commands, and optional return navigation.
 * [Output] Dedicated API configuration page that owns every user-entered API/Access/Secret key field and explains macOS private-file versus Windows credential storage.
 * [Pos] top-level page node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import React, { useEffect, useMemo, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import {
  AlertCircle,
  ArrowLeft,
  CheckCircle2,
  Cloud,
  Eye,
  EyeOff,
  KeyRound,
  Loader2,
  ShieldCheck,
  Sparkles,
} from "lucide-react";
import PageShell from "./shell/PageShell.jsx";
import Card from "./shell/Card.jsx";
import {
  VIDEO_PROVIDERS,
  loadProviderConfig,
  saveProviderConfig,
} from "./lib/avatar-pipeline/provider-config.js";
import {
  ASR_RESOURCE_OPTIONS,
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
    advanced: config.advanced || {},
  };
}

function resultToneClass(tone) {
  return tone ? ` is-${tone}` : "";
}

export default function ApiSettings({ onBack }) {
  const [showSecrets, setShowSecrets] = useState(false);
  const [videoConfigs, setVideoConfigs] = useState(loadVideoConfigs);
  const [providerResults, setProviderResults] = useState({});
  const [asrApiKey, setAsrApiKey] = useState("");
  const [asrResourceId, setAsrResourceId] = useState(ASR_RESOURCE_OPTIONS[0].id);
  const [asrState, setAsrState] = useState({
    loading: true,
    pending: false,
    configured: false,
    tone: "muted",
    message: "正在读取语音识别配置…",
  });

  useEffect(() => {
    let cancelled = false;
    invoke("load_device_asr_settings")
      .then((status) => {
        if (cancelled) return;
        setAsrResourceId(status?.resourceId || ASR_RESOURCE_OPTIONS[0].id);
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
      providerCredentialsConfigured(provider.id, videoConfigs[provider.id]),
    ).length,
    [videoConfigs],
  );
  const configuredCount = configuredProviderCount + (asrState.configured ? 1 : 0);

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

  const saveProvider = (providerId) => {
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
    saveProviderConfig(providerId, persistedProviderConfig(config));
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
          resourceId: asrResourceId,
        },
      });
      const probe = await invoke("test_device_asr_settings");
      setAsrApiKey("");
      setAsrState({
        loading: false,
        pending: false,
        configured: saved?.configured === true,
        tone: "success",
        message: `${probe?.message || "火山引擎云端 ASR 已就绪"}（${probe?.latencyMs ?? 0} ms）`,
      });
      emitApiConfigurationUpdated({ providerId: "volcengine-asr" });
    } catch (error) {
      setAsrState((current) => ({
        ...current,
        loading: false,
        pending: false,
        configured: saved?.configured === true || current.configured,
        tone: "error",
        message: `语音识别服务测试失败：${error}`,
      }));
    }
  };

  return (
    <PageShell
      title="API 配置"
      subtitle="统一管理语音识别与形象生成需要的用户凭据"
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
      <section className="api-settings__overview" aria-label="API 配置概览">
        <div className="api-settings__overview-icon" aria-hidden="true">
          <ShieldCheck size={22} />
        </div>
        <div className="api-settings__overview-copy">
          <strong>{configuredCount} / {VIDEO_PROVIDERS.length + 1} 项凭据已配置</strong>
          <span>业务页面只读取配置状态，不再接收或展示 API Key。</span>
        </div>
        <div className="api-settings__storage-note">
          <KeyRound size={14} aria-hidden="true" />
          ASR：macOS 使用用户私有文件；Windows 使用系统凭据
        </div>
      </section>

      <Card title="语音识别" subtitle="用于设备麦克风语音转文字">
        <div className="api-settings__service-row">
          <div className="api-settings__service-mark is-voice" aria-hidden="true">
            <Cloud size={19} />
          </div>
          <div className="api-settings__service-heading">
            <strong>火山引擎豆包 ASR</strong>
            <span>macOS 不使用钥匙串；API Key 仅由当前用户读取，不额外加密</span>
          </div>
          <span className={`api-settings__status${asrState.configured ? " is-success" : ""}`}>
            {asrState.loading ? <Loader2 size={13} className="spin" /> : <CheckCircle2 size={13} />}
            {asrState.loading ? "读取中" : asrState.configured ? "已配置" : "未配置"}
          </span>
        </div>
        <div className="api-settings__form api-settings__form--asr">
          <label className="ui-field" htmlFor="api-settings-asr-resource">
            <span className="ui-field__label">识别模型</span>
            <span className="ui-control-shell">
              <select
                id="api-settings-asr-resource"
                className="ui-control ui-control--select"
                value={asrResourceId}
                onChange={(event) => setAsrResourceId(event.target.value)}
                disabled={asrState.pending || asrState.loading}
              >
                {ASR_RESOURCE_OPTIONS.map((option) => (
                  <option key={option.id} value={option.id}>{option.label}</option>
                ))}
              </select>
            </span>
          </label>
          <label className="ui-field" htmlFor="api-settings-asr-key">
            <span className="ui-field__label">API Key</span>
            <input
              id="api-settings-asr-key"
              className="ui-control api-settings__secret-input"
              type={showSecrets ? "text" : "password"}
              autoComplete="new-password"
              value={asrApiKey}
              onChange={(event) => setAsrApiKey(event.target.value)}
              placeholder={asrState.configured ? "已安全保存；留空可直接复测" : "输入火山引擎 ASR API Key"}
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

      <Card title="形象生成" subtitle="用于新形象生成与单状态视频替换">
        <div className="api-settings__provider-grid">
          {VIDEO_PROVIDERS.map((provider) => {
            const config = videoConfigs[provider.id];
            const configured = providerCredentialsConfigured(provider.id, config);
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
                    <small>{provider.sub}</small>
                  </span>
                  <span className={`api-settings__status${configured ? " is-success" : ""}`}>
                    {configured ? "已配置" : "未配置"}
                  </span>
                </header>

                <div className="api-settings__provider-fields">
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
                        value={config.apiKey}
                        onChange={(event) => updateProvider(provider.id, { apiKey: event.target.value })}
                        placeholder={`输入${provider.label} API Key`}
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
                </div>

                <footer className="api-settings__provider-foot">
                  <span className={`api-settings__provider-result${resultToneClass(result?.tone)}`}>
                    {result?.message || (configured ? "可用于形象生成" : "等待填写凭据")}
                  </span>
                  <button
                    type="button"
                    className="btn-secondary btn-sm"
                    onClick={() => saveProvider(provider.id)}
                  >
                    保存
                  </button>
                </footer>
              </section>
            );
          })}
        </div>
      </Card>
    </PageShell>
  );
}
