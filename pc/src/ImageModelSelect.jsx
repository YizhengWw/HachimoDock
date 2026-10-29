import React, { useEffect, useState } from "react";
import { imageModelOptions, listImageModelCatalog } from "./lib/image-models.js";
import { DEFAULT_VOLCANO_IMAGE_MODEL } from "./lib/avatar-pipeline/providers/volcano-image.js";

export default function ImageModelSelect({ value, onChange, apiKey = "", baseUrl, disabled = false, saveHint = "修改后点击保存。" }) {
  const [revision, setRevision] = useState(0);
  const [manual, setManual] = useState(false);
  const [state, setState] = useState({ models: [], shutdownModels: [], loading: false, error: "" });
  useEffect(() => {
    const controller = new AbortController();
    setState(previous => ({ models: [],
      shutdownModels: previous.apiKey === apiKey && previous.baseUrl === baseUrl ? previous.shutdownModels : [],
      apiKey, baseUrl, loading: Boolean(apiKey.trim()), error: "" }));
    const timer = setTimeout(() => {
      if (!apiKey.trim()) return;
      listImageModelCatalog({ apiKey, baseUrl, signal: controller.signal }).then(catalog => {
        if (!controller.signal.aborted) setState({ ...catalog, apiKey, baseUrl, loading: false, error: "" });
      }).catch(() => {
        if (!controller.signal.aborted) setState(previous => ({ ...previous, models: [], loading: false,
          error: "模型列表暂不可用，可选择内置候选或手动填写；不会替换已选模型。" }));
      });
    }, 600);
    return () => { clearTimeout(timer); controller.abort(); };
  }, [apiKey, baseUrl, revision]);
  const models = imageModelOptions(state, value);
  const currentModel = value || DEFAULT_VOLCANO_IMAGE_MODEL;
  const selectedShutdown = state.shutdownModels.includes(currentModel);
  const defaultShutdown = state.shutdownModels.includes(DEFAULT_VOLCANO_IMAGE_MODEL);
  return <div className="text-model-select">
    <div className="video-model-row">
      <select className="ui-control field-input" aria-label="图片处理模型" value={selectedShutdown ? "" : value || ""}
        disabled={disabled} onChange={event => onChange(event.target.value)}>
        <option value="" disabled={selectedShutdown || defaultShutdown}>{selectedShutdown ? "当前模型已下线，请重新选择"
          : defaultShutdown ? "默认模型已下线，请选择其他模型" : `默认 · ${DEFAULT_VOLCANO_IMAGE_MODEL}`}</option>
        {models.map(id => <option key={id} value={id}>{id}</option>)}
      </select>
      <button type="button" className="btn-ghost btn-sm" disabled={disabled || state.loading || !apiKey.trim()}
        onClick={() => setRevision(n => n + 1)}>{state.loading ? "获取中…" : "刷新列表"}</button>
    </div>
    <label className="field-helper"><input type="checkbox" disabled={disabled} checked={manual}
      onChange={event => setManual(event.target.checked)} /> 手动填写模型 / 接入点 ID</label>
    {manual && <input className="ui-control field-input" aria-label="图片处理模型自定义 ID" disabled={disabled}
      value={value} onChange={event => onChange(event.target.value)} placeholder="模型名称或 ep- 接入点 ID" />}
    <div className="muted small">用于图片预览生成、背景处理和独立动作首帧。需支持图片编辑；列表不代表已开通权限。{saveHint}</div>
    {selectedShutdown && <div role="alert" className="muted small">已保存的模型 {currentModel} 已下线，请重新选择并保存。</div>}
    {state.error && <div role="status" className="muted small">{state.error}</div>}
  </div>;
}
