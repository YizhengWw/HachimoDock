import React, { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import Card from "../shell/Card.jsx";
import Button from "../shell/Button.jsx";
import { p4SessionDisplayTitle } from "../lib/p4-session-service.js";

const STATE_LABELS = {
  working: "工作中", thinking: "思考中", tool_running: "执行工具",
  waiting_user: "等待确认", speaking: "正在回复", done: "已完成",
  error: "出错", stopped: "已停止", idle: "休息中",
};

export default function StopwatchSessionPanel({ usb, sessionSync }) {
  const [report, setReport] = useState(null);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState("");
  const read = useRef(() => {});
  const board = usb.boardDeviceId;
  const scope = JSON.stringify([board, usb.connectionId, usb.bootId || usb.capabilities?.debugBootId]);
  useEffect(() => {
    let disposed = false, inFlight = false, loaded = false;
    setReport(null);
    setError("");
    setPending(false);
    const load = async (manual = false) => {
      if (inFlight || !usb.connected || !board) return;
      inFlight = true;
      if (manual || !loaded) setPending(true);
      try {
        const next = await invoke("usb_get_diagnostics", { expectedBoardDeviceId: board });
        if (disposed) return;
        setReport(next);
        setError("");
        loaded = true;
      } catch (failure) {
        if (!disposed) setError(String(failure));
      } finally {
        inFlight = false;
        if (!disposed) setPending(false);
      }
    };
    read.current = () => load(true);
    void load();
    const timer = setInterval(() => void load(), 2000);
    return () => { disposed = true; clearInterval(timer); read.current = () => {}; };
  }, [scope, usb.connected, board]);

  const sessions = sessionSync.selectableSessions || [];
  const selected = sessionSync.sessionId === "auto"
    ? sessions[0]
    : sessions.find(item => item.id === sessionSync.sessionId);
  const runtime = report?.runtime || {};
  const current = !report ? (pending ? "正在读取…" : "尚未读取设备状态")
    : runtime.sessionTitle || (runtime.sessionCount === 0 ? "暂无活跃会话" : "固件未提供会话标题");
  const state = STATE_LABELS[runtime.agentState] || runtime.agentState || "";

  return <Card title="会话跟随" subtitle="使用当前 Agent 的真实会话；任务完成或出错后保留显示约 60 秒。"
    actions={<Button variant="ghost" size="small" loading={pending} loadingLabel="读取中…"
      disabled={!usb.connected} onClick={() => read.current()}>刷新设备状态</Button>}>
    <div className="field">
      <label className="field-label" htmlFor="stopwatch-session">电脑选中的会话</label>
      <select className="field-input" id="stopwatch-session" value={sessionSync.sessionId}
        disabled={!usb.connected || !sessionSync.sessionsLoaded}
        onChange={event => sessionSync.onSessionChange(event.target.value)}
        aria-describedby="stopwatch-session-help">
        <option value="auto">自动跟随</option>
        {sessions.map(item => <option key={item.id} value={item.id}>
          {p4SessionDisplayTitle(item) || "未命名会话"} · {STATE_LABELS[item.state] || item.state}
        </option>)}
        {sessionSync.sessionId !== "auto" && !selected &&
          <option value={sessionSync.sessionId}>正在更新会话…</option>}
      </select>
      <p id="stopwatch-session-help">也可以用设备按键或左右滑动切换会话。</p>
    </div>
    <div aria-live="polite">
      <p>电脑当前：{selected ? p4SessionDisplayTitle(selected) : "暂无活跃会话"}</p>
      <p>设备当前：{usb.connected ? current : "设备未连接"}{state ? " · "+state : ""}</p>
    </div>
    {error && <p role="alert" className="message-banner message-banner--error">设备状态读取失败：{error}</p>}
    <details>
      <summary>运行状态</summary>
      <p>设备会话数：{runtime.sessionCount ?? "未知"}；选中位置：{runtime.selectedSession ?? "未知"}</p>
      <p>显示提交：{report?.display?.frames ?? "未知"}；显示错误：{report?.display?.error ?? "未知"}</p>
      <p>本次启动：{report?.debugBootId || "未知"}；运行秒数：{Number.isFinite(runtime.uptimeMs) ? Math.floor(runtime.uptimeMs / 1000) : "未知"}</p>
    </details>
  </Card>;
}
