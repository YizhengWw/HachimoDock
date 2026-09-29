import React, { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { deadlineIso, localDateTime, todoMutation, todoDateLabel } from "./local-widget-model.js";
import "./local-data.css";

export default function LocalDataPanel({ source }) {
  const isTodos = source === "todos.upcoming";
  const [items, setItems] = useState([]);
  const [snapshot, setSnapshot] = useState(null);
  const [error, setError] = useState("");
  const [pollError, setPollError] = useState("");
  const [busy, setBusy] = useState(false);
  const [loading, setLoading] = useState(true);
  const [title, setTitle] = useState("");
  const [due, setDue] = useState("");
  const [editing, setEditing] = useState(null);
  const [filter, setFilter] = useState("pending");
  const [query, setQuery] = useState("");
  const [deleting, setDeleting] = useState(null);
  const epoch = useRef(0);
  const writing = useRef(false);
  const mounted = useRef(false);
  useEffect(() => {
    mounted.current = true;
    let stopped = false;
    let timer;
    async function poll() {
      const request = epoch.current;
      if (!writing.current) {
        try {
          const next = isTodos
            ? await invoke("local_tool_execute", { name: "todo_manage", input: { operation: "list", filter: "all" } })
            : await invoke("local_widget_snapshot", { source });
          if (!stopped && request === epoch.current) {
            if (isTodos) setItems(next); else setSnapshot(next);
            setPollError("");
          }
        } catch (e) { if (!stopped && request === epoch.current) setPollError(String(e)); }
        finally { if (!stopped) setLoading(false); }
      }
      if (!stopped) timer = setTimeout(poll, 2000);
    }
    poll();
    return () => { stopped = true; mounted.current = false; clearTimeout(timer); };
  }, [source, isTodos]);
  async function mutate(input, reset = false) {
    if (writing.current) return;
    writing.current = true; ++epoch.current; setBusy(true); setError("");
    try {
      const next = await invoke("local_tool_execute", { name: "todo_manage", input });
      if (mounted.current) {
        setItems(next); setDeleting(null);
        if (reset) { setTitle(""); setDue(""); setEditing(null); }
      }
    } catch (e) { if (mounted.current) setError(String(e)); }
    finally { writing.current = false; if (mounted.current) setBusy(false); }
  }
  const visible = items.filter(item => (filter === "all" || item.completed === (filter === "completed")) && item.title.includes(query.trim()));
  function submit(event) {
    event.preventDefault();
    try { mutate(todoMutation(editing ? "update" : "add", editing, { title: title.trim(), dueAt: deadlineIso(due) }), true); }
    catch (e) { setError(String(e)); }
  }
  return <section className="local-data-panel" aria-label={isTodos ? "近期待办管理" : "电脑状态详情"}>
    <header><h4>{isTodos ? "近期待办" : "电脑状态"}</h4><span>{isTodos ? `${items.filter(i => !i.completed).length} 项未完成` : "每 2 秒更新"}</span></header>
    <p>{isTodos ? "进入设备实时对话后，可以说“添加待办：明天下午三点交报告”“把交报告改到周五”或“完成买牛奶”。也可在这里管理；设备优先显示未完成事项，最多 20 项，蓝色未完成、绿色已完成。退出实时对话后同步设备列表。" : "本机资源状态实时同步到设备。请保持 Pet Manager 运行并连接 USB；网络速率为所有接口合计，包含虚拟接口，不等于互联网测速。"}</p>
    {(error || pollError) && <p role="alert" className="local-data-error">{error || pollError}</p>}
    {loading && <p role="status">正在读取…</p>}
    {isTodos ? <>
      <form onSubmit={submit} className="local-todo-form">
        <label>待办内容<input required maxLength={120} value={title} onChange={e => setTitle(e.target.value)} placeholder="例如：准备周会材料" disabled={busy} /></label>
        <label>截止时间（可不填）<input type="datetime-local" value={due} onChange={e => setDue(e.target.value)} disabled={busy} /></label>
        <button type="submit" disabled={busy || loading || !title.trim()}>{busy ? "保存中…" : editing ? "保存修改" : "添加待办"}</button>
        {editing && <button type="button" disabled={busy} onClick={() => { setEditing(null); setTitle(""); setDue(""); }}>取消编辑</button>}
      </form>
      <div className="local-todo-filters"><select aria-label="待办筛选" value={filter} onChange={e => setFilter(e.target.value)}><option value="pending">未完成</option><option value="completed">已完成</option><option value="all">全部</option></select><input aria-label="搜索待办" placeholder="搜索待办" value={query} onChange={e => setQuery(e.target.value)} /></div>
      {!loading && !visible.length && <p className="local-data-empty">{items.length ? "没有符合条件的待办" : "还没有待办，添加一项开始吧"}</p>}
      <ul className="local-todo-list">{visible.map(item => <li key={item.id}>
        <label className="local-todo-check"><input type="checkbox" checked={item.completed} disabled={busy || editing?.id === item.id} aria-label={`${item.completed ? "恢复" : "完成"}${item.title}`} onChange={() => mutate(todoMutation(item.completed ? "reopen" : "complete", item))} /></label>
        <div className="local-todo-content"><strong className={item.completed ? "is-done" : "is-pending"}>{item.title}</strong><small>{todoDateLabel(item.dueAt)} · {item.completed ? "已完成" : item.dueAt && new Date(item.dueAt) < new Date() ? "已逾期" : "待完成"}</small></div>
        {deleting?.id === item.id ? <div className="local-todo-actions"><span>删除这项？</span><button disabled={busy} onClick={() => mutate(todoMutation("delete", deleting))}>确认删除</button><button disabled={busy} onClick={() => setDeleting(null)}>取消</button></div> : <div className="local-todo-actions"><button disabled={busy} onClick={() => { setEditing(item); setTitle(item.title); setDue(localDateTime(item.dueAt)); }}>编辑</button><button disabled={busy || editing?.id === item.id} onClick={() => setDeleting(item)}>删除</button></div>}
      </li>)}</ul>
      <p className="local-data-footnote">保存在这台电脑，不跨电脑同步。截止时间用于排序与展示，暂不提供到点提醒。实时对话会把完成请求所需的待办内容发送给你配置的大模型。</p>
    </> : <div className="local-metrics">{snapshot?.rows?.map(row => <div key={row.id}><span>{row.label}</span><strong>{row.value}</strong><small>{row.detail || row.meta}</small></div>)}</div>}
  </section>;
}
