/** PC-only cover: default Xiaomi/Alibaba rows plus read-only cached native quotes. */
import React, { useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { stockPreviewRows } from "./stock-preview.js";
import "./stock-screen-preview.css";

export default function StockScreenPreview({ active }) {
  const [status, setStatus] = useState(null);
  useEffect(() => {
    if (!active || !window.__TAURI_INTERNALS__) return undefined;
    let stopped = false;
    let timer;
    async function poll() {
      try {
        const next = await invoke("stock_watchlist_status");
        if (!stopped) setStatus(next);
      } catch {
        if (!stopped) setStatus(previous => ({ ...previous, error: "行情暂不可用" }));
      }
      if (!stopped) timer = window.setTimeout(poll, 2000);
    }
    poll();
    return () => { stopped = true; window.clearTimeout(timer); };
  }, [active]);
  const rows = stockPreviewRows(status);
  const date = rows.map(row => row.date).filter(Boolean).sort()[0];
  const notice = rows.some(row => row.stale) ? "缓存行情 · 已过期"
    : rows.some(row => !row.available) ? "暂无行情的股票显示 —" : "以实际行情日期为准";
  return <div className="cds-stocks">
    <header className="cds-stocks__header"><strong>自选股行情</strong><span>2 只</span></header>
    <div className="cds-stocks__caption"><span>默认股票预览</span><span>{date || "等待行情"}</span></div>
    <div className="cds-stocks__table" role="table" aria-label="默认股票行情预览">
      <div className="cds-stocks__columns" role="row"><span role="columnheader">股票</span><span role="columnheader">最新价</span><span role="columnheader">涨跌幅</span></div>
      {rows.map(row => <div className="cds-stocks__row" role="row" key={row.symbol}>
        <div role="cell" className="cds-stocks__identity"><strong>{row.name}</strong><small>{row.code} · 港股</small></div>
        <strong role="cell" data-tone={row.tone}>{row.price}</strong>
        <strong role="cell" data-tone={row.tone}>{row.change}</strong>
      </div>)}
    </div>
    <footer className="cds-stocks__footer"><span>{notice}</span><span>在详情中管理自选股</span></footer>
  </div>;
}
