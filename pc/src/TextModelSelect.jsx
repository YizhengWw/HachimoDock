/** Shared selectable text/vision catalog with an explicit manual-ID escape hatch.
 * Refresh never changes an existing choice or persists credentials. */
import React, { useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { DOUBAO_TEXT_MODELS, DEFAULT_LITE_MODEL, latestLiteModel, listThinkingModels, normalizeTextModels } from "./lib/text-models.js";

export default function TextModelSelect({ value, onChange, provider = "doubao", baseUrl, apiKey = "", configured = false,
  vision = false, disabled = false, label = "对话模型" }) {
  const [revision, setRevision] = useState(0);
  const [manual, setManual] = useState(false);
  const [state, setState] = useState({ models: [], loading: false, error: "" });
  useEffect(() => {
    let cancelled = false;
    const controller = new AbortController();
    setState({ models: [], loading: Boolean(apiKey.trim() || configured), error: "" });
    const timer = setTimeout(() => { if (apiKey.trim() || configured) {
      const request = vision ? listThinkingModels({ apiKey, baseUrl, signal: controller.signal })
        : invoke("list_voice_chat_models", { input: { llmProvider: provider,
          llmBaseUrl: provider === "custom" ? baseUrl : null, llmApiKey: apiKey.trim() || null } });
      request.then(models => { if (!cancelled) setState({ models: normalizeTextModels(models), loading: false, error: "" }); })
        .catch(() => { if (!cancelled) setState({ models: [], loading: false,
          error: "模型列表暂不可用，可刷新或手动填写；不会替换已选模型。" }); });
    } }, 600);
    return () => { cancelled = true; clearTimeout(timer); controller.abort(); };
  }, [provider, baseUrl, apiKey, configured, vision, revision]);
  const models = [...new Set([...(provider === "doubao" ? DOUBAO_TEXT_MODELS : []), ...state.models, ...(value ? [value] : [])])];
  const latest = latestLiteModel(state.models) || DEFAULT_LITE_MODEL;
  return <div className="text-model-select">
    <div className="video-model-row">
      <select className="ui-control field-input" aria-label={label} value={value || ""}
        disabled={disabled} onChange={event => onChange(event.target.value || (provider === "doubao" ? latest : ""))}>
        <option value="">{provider === "doubao" ? `默认 Lite · ${DEFAULT_LITE_MODEL}` : "服务默认模型"}</option>
        {models.map(id => <option key={id} value={id}>{id}{id === latest ? " · 推荐 Lite" : ""}</option>)}
      </select>
      <button type="button" className="btn-ghost btn-sm" disabled={disabled || state.loading || !(apiKey.trim() || configured)}
        onClick={() => setRevision(n => n + 1)}>{state.loading ? "获取中…" : "刷新列表"}</button>
    </div>
    {provider === "doubao" && <button type="button" className="btn-ghost btn-sm" disabled={disabled}
      onClick={() => onChange(latest)}>使用最新 Lite</button>}
    <label className="field-helper"><input type="checkbox" disabled={disabled} checked={manual}
      onChange={event => setManual(event.target.checked)} /> 手动填写模型 / 接入点 ID</label>
    {manual && <input className="ui-control field-input" aria-label={`${label}自定义 ID`} disabled={disabled}
      value={value} onChange={event => onChange(event.target.value)} placeholder="模型名称或 ep- 接入点 ID" />}
    <div className="muted small">列表不代表账号已开通调用权限；Pro 仅在你主动选择后使用。修改后需保存。</div>
    {state.error && <div role="status" className="muted small">{state.error}</div>}
  </div>;
}
