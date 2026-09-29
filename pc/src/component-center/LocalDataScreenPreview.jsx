import React, { useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import "./local-data-preview.css";
import { todoHeaderDate } from "./local-widget-model.js";

export default function LocalDataScreenPreview({ source, active, name }) {
  const [snapshot, setSnapshot] = useState(null);
  useEffect(() => {
    if (!active || !window.__TAURI_INTERNALS__) return undefined;
    let stopped = false;
    let timer;
    async function poll() {
      try {
        const value = await invoke("local_widget_snapshot", { source });
        if (!stopped) setSnapshot(value);
      } catch { if (!stopped) setSnapshot(previous => ({ ...previous, status: "error", message: "数据暂不可用" })); }
      if (!stopped) timer = setTimeout(poll, 2000);
    }
    poll();
    return () => { stopped = true; clearTimeout(timer); };
  }, [source, active]);
  const rows = snapshot?.rows?.slice(0, 5) || [];
  const todos = source === "todos.upcoming";
  return <div className="local-data-screen" data-source={source}>
    <header><strong>{name}</strong><span>{todos ? todoHeaderDate(snapshot?.date) : snapshot?.date || "等待数据"}</span></header>
    <div className="local-data-screen__rows">{rows.map(row => todos
      ? <div key={row.id} className={`local-todo-preview-row ${row.tone < 0 ? "is-done" : "is-pending"}`}><strong>{row.label}{row.meta}</strong><div><small>{row.value}</small><small>{row.detail}</small></div></div>
      : <div key={row.id}><div><strong>{row.label}</strong><small>{row.meta}</small></div><span>{row.value}<small>{row.detail}</small></span></div>)}</div>
    {!rows.length && <p>{snapshot?.message || (source === "todos.upcoming" ? "还没有待办 · 在详情中添加" : "等待电脑状态")}</p>}
    <footer>{snapshot?.status === "error" ? snapshot.message : "摇杆左右翻页 · 主操作回首页"}</footer>
  </div>;
}
