/** Explicit focus/readiness check. Never records, writes drafts or submits.
 * Ignore results after unmount or Agent change; no auto checks on mount. */
import React, { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";

export default function VoiceComposerCheck({ agentId, disabled = false }) {
  const sequence = useRef(0);
  const [state, setState] = useState({ pending: false, error: false, message: "" });
  const [results, setResults] = useState([]);
  useEffect(() => {
    sequence.current++;
    setState({ pending: false, error: false, message: "" });
    setResults([]);
    return () => { sequence.current++; };
  }, [agentId]);
  const check = async () => {
    const current = ++sequence.current;
    setState({ pending: true, error: false, message: "正在定位当前输入框，不写入也不发送文字…" });
    try {
      const result = await invoke("check_voice_composer_readiness", { agentId });
      if (sequence.current === current) {
        setResults(previous => [...previous.slice(-3), result]);
        setState({ pending: false, error: false,
          message: "对照完成。结果不代表语音写入已通过；请将下面的结果文本发回聊天。" });
      }
    } catch (error) {
      if (sequence.current === current) setState({ pending: false, error: true, message: String(error) });
    }
  };
  return <div className="voice-panel__composer-check">
    <button type="button" className="btn-secondary btn-sm" onClick={check} disabled={disabled || state.pending}>
      {state.pending ? "正在检查输入框…" : "只检查输入框（不写入、不发送）"}
    </button>
    <p className="muted small">会将目标应用置前；不切换会话，不调用语音识别或模型。</p>
    {state.message && <div role="status" className={`message-banner ${state.error ? "message-banner--error" : "message-banner--muted"}`}>{state.message}</div>}
    {results.length > 0 && <label className="ui-field" style={{marginTop:16}}>
      <span>最近 {results.length} 次结果（可以全选复制；不含聊天正文、草稿或密钥）</span>
      <textarea aria-label="输入框定位对照结果" className="ui-control" readOnly rows={18}
        style={{fontFamily:"monospace",width:"100%",fontSize:12}} value={JSON.stringify(results,null,2)} />
    </label>}
    {results.map((result, index) => <section key={index} aria-label={`第 ${index + 1} 次检查明细`}>
      <h3>第 {index + 1} 次检查</h3>
      {result.voiceSelection && <p>实际语音定位规则：{result.voiceSelection.ok
        ? "已定位（未写入、未发送；仍需真实语音验收）"
        : `未定位：${result.voiceSelection.error || "目标输入框尚未确认"}`}</p>}
      <p>旧规则窗口：{result.legacyWindow ?? "未找到"}；当前规则窗口：{result.currentWindow ?? "未找到"}</p>
      {[...(result.snapshots || []).map(s => ({label:`置前前 · 窗口 ${s.index}`, data:s.beforeFocus})),
        {label:"置前后", data:result.afterFocus}].filter(s => s.data).map(({label,data}) => <div key={label}>
        <p>{label}：扫描 {data.nodes} 个节点，候选 {data.releasedMatches} 个；焦点在扫描树内：{String(data.focusedInScan ?? "未采集")}</p>
        {data.candidateStructure && <details open><summary>候选位置与结构（不含正文）</summary>
          <pre style={{whiteSpace:"pre-wrap",fontSize:12}}>{JSON.stringify(data.candidateStructure,null,2)}</pre>
        </details>}
        {(data.textControls || []).map(control => <pre key={control.ordinal} style={{whiteSpace:"pre-wrap",fontSize:12}}>
          {JSON.stringify(control,null,2)}
        </pre>)}
      </div>)}
    </section>)}
  </div>;
}
