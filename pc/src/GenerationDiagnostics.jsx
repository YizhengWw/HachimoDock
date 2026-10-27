import React, { useState } from "react";
import { getGenerationDiagnostics, formatDiagnostic } from "./lib/avatar-pipeline/diagnostics.js";

export default function GenerationDiagnostics({ since = 0 }) {
  const [, refresh] = useState(0);
  const rows = getGenerationDiagnostics(since).slice(-20).reverse();
  const outcomes = { timeout: "超时", transport_error: "传输失败", http_error: "服务返回错误", configuration_error: "配置错误", invalid_output: "检查结论格式异常", success: "成功", failed: "失败", stage: "阶段结束" };
  return <details className="muted small" onToggle={(event) => { if (event.currentTarget.open) refresh((v) => v + 1); }} style={{ marginTop: 12, overflowWrap: "anywhere" }}>
    <summary>形象生成诊断（本机脱敏记录）</summary>
    <p>仅记录阶段、耗时、状态和追踪编号，不记录 Key、地址、图片或提示词。请求编号未取得时，不代表服务端没有执行。</p>
    <button type="button" className="btn-secondary btn-sm" onClick={() => refresh((v) => v + 1)}>刷新诊断</button>
    {rows.length ? <ul>{rows.map((row, index) => <li key={`${row.localId}-${row.timestampMs}-${index}`} style={{ marginTop: 8 }}>
      {new Date(row.timestampMs).toLocaleString()} · {outcomes[row.outcome] || "处理中"}<br />{formatDiagnostic(row)}
    </li>)}</ul> : <p>暂无诊断记录；旧版本已经发生的失败无法补录。</p>}
  </details>;
}
