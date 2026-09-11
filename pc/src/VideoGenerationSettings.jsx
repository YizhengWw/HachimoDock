/** [Input] API configuration and video overrides. [Output] Refreshable catalog, latest Fast default for empty selection, and editable generation parameters.
 * [Sync] Keep pc/src/.folder.md and catalog regression tests aligned. */
import React, { useEffect, useState } from "react";
import { RefreshCw } from "lucide-react";
import { listVideoModels, selectDefaultVideoModel } from "./lib/avatar-pipeline/video-models.js";
import { resolveGenerationSpeedConfig } from "./lib/avatar-pipeline/pipeline-defaults.js";
import { VIDEO_MODEL_ACCESS_CHANGED, clearVolcanoModelRejections } from "./lib/avatar-pipeline/providers/volcano.js";

export function VideoModelSelect({ apiKey, baseUrl, model, onModel, onReady }) {
  const [revision, setRevision] = useState(0);
  const [manual, setManual] = useState(false);
  const [state, setState] = useState({ models: [], loading: true, error: "" });
  useEffect(() => {
    const refresh = () => setRevision((value) => value + 1);
    window.addEventListener(VIDEO_MODEL_ACCESS_CHANGED, refresh);
    window.addEventListener("focus", refresh);
    return () => {
      window.removeEventListener(VIDEO_MODEL_ACCESS_CHANGED, refresh);
      window.removeEventListener("focus", refresh);
    };
  }, []);
  useEffect(() => {
    const controller = new AbortController();
    setState({ models: [], loading: true, error: "" });
    listVideoModels({ apiKey, baseUrl, signal: controller.signal }).then((models) => {
      if (controller.signal.aborted) return;
      setState({ models, apiKey, baseUrl, loading: false, error: models.length ? "" : "当前 Key 未返回可用的视频模型。" });
    }).catch((error) => {
      if (!controller.signal.aborted) setState({ models: [], loading: false, error: error.message });
    });
    return () => controller.abort();
  }, [apiKey, baseUrl, revision, onReady]);
  useEffect(() => {
    if (state.loading || state.error || state.apiKey !== apiKey || state.baseUrl !== baseUrl) return;
    const next = selectDefaultVideoModel(state.models, model, manual);
    if (next && next !== model) onModel(next);
  }, [state, apiKey, baseUrl, model, manual, onModel]);
  const valid = manual ? Boolean(apiKey?.trim() && model?.trim())
    : state.apiKey === apiKey && state.baseUrl === baseUrl
      && !state.loading && state.models.some((item) => item.id === model);
  useEffect(() => { onReady(valid); }, [valid, onReady]);
  return <>
    <div className="video-model-row">
      <select aria-label="视频生成模型" className="field-input" value={!manual && valid ? model : ""}
        disabled={state.loading} onChange={(event) => onModel(event.target.value)}>
        <option value="" disabled>{state.loading ? "正在获取模型…" : "请选择可用模型"}</option>
        {state.models.map((item) => <option key={item.id} value={item.id}>{item.label}</option>)}
      </select>
      <button type="button" className="btn-ghost" title="刷新可用模型" aria-label="刷新可用模型"
        disabled={state.loading} onClick={() => {
          clearVolcanoModelRejections(apiKey);
          setRevision((value) => value + 1);
        }}><RefreshCw size={18} /></button>
    </div>
    <label className="field-helper"><input type="checkbox" checked={manual}
      onChange={(event) => setManual(event.target.checked)} /> 手动填写模型 / 接入点 ID</label>
    {manual && <input aria-label="模型或接入点 ID" className="field-input" value={model}
      placeholder="填写模型 ID 或 ep- 接入点 ID" onChange={(event) => onModel(event.target.value)} />}
    <div className="field-helper">首次默认选择列表中最新的 Fast 模型，也可自行更换。实际调用权限以平台响应为准；列表不可用时可手动填写。</div>
    {state.error && <div role="alert" className="field-helper">{state.error}</div>}
    {!manual && !state.loading && !state.error && model && !valid && <div role="alert" className="field-helper">列表中没有之前选择的模型，请重新选择或手动确认模型 ID。</div>}
  </>;
}

export function VideoGenerationSettings({ value = {}, onChange, fastGeneration }) {
  const effective = resolveGenerationSpeedConfig({ provider: "volcengine", fastGeneration, ...value }).providerConfig;
  const update = (key, next) => onChange({ ...value, [key]: next });
  return <details className="video-generation-settings">
    <summary>视频参数</summary>
    <div className="video-parameters-grid">
      <label>时长<select aria-label="时长" className="field-input" value={effective.duration ?? "auto"} onChange={(e) => update("duration", e.target.value === "auto" ? "auto" : Number(e.target.value))}>
        <option value="auto">模型默认</option>{[5, 10].map((n) => <option key={n} value={n}>{n} 秒</option>)}
      </select></label>
      <label>分辨率<select aria-label="分辨率" className="field-input" value={effective.resolution ?? "auto"} onChange={(e) => update("resolution", e.target.value)}>
        <option value="auto">模型默认</option>{["480p", "720p", "1080p"].map((r) => <option key={r}>{r}</option>)}
      </select></label>
      <label>随机种子<input aria-label="随机种子" className="field-input" type="number" min={-1} max={4294967295} step={1}
        value={value.seed ?? -1} onChange={(e) => update("seed", e.target.value === "" ? -1 : Number(e.target.value))} /></label>
      <label>画面比例<input className="field-input" value="4:3" readOnly /></label>
    </div>
    <div className="video-parameter-toggles">
      {[["cameraFixed", "固定镜头"], ["generateAudio", "生成音频"], ["watermark", "水印"]].map(([key, label]) =>
        <label key={key}><input type="checkbox" checked={value[key] ?? false} onChange={(e) => update(key, e.target.checked)} />{label}</label>)}
    </div>
    <button className="btn-ghost" type="button" onClick={() => onChange({})}>恢复默认参数</button>
  </details>;
}
