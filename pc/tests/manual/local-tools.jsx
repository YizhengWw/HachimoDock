// Development-only, memory-only visual regression harness. Never loads user data.
import React, { useState } from "react";
import { createRoot } from "react-dom/client";
import ComponentPreviewModal from "../../src/component-center/ComponentPreviewModal.jsx";
import { BUILTIN_COMPONENT_CENTER } from "../../src/fixtures.js";
import "../../src/styles.css";
if (!import.meta.env.DEV) throw new Error("Manual fixture requires the development server");
let items = [{ id: "test-1", title: "测试：准备评审材料", revision: 1, dueAt: "2026-09-26T07:00:00Z", completed: false }, { id: "test-2", title: "测试：补充生活用品清单", revision: 1, dueAt: null, completed: false }, { id: "test-3", title: "测试：已完成事项", revision: 2, dueAt: null, completed: true }];
const metrics = [{ id: "cpu", label: "CPU", value: "12%", detail: "", meta: "模拟处理器占用" }, { id: "memory", label: "内存", value: "42%", detail: "6.7/16.0 GiB", meta: "模拟已用 / 总量" }, { id: "down", label: "网络接收", value: "32.1 KiB/s", detail: "", meta: "模拟接口合计" }, { id: "up", label: "网络发送", value: "4.2 KiB/s", detail: "", meta: "模拟接口合计" }, { id: "uptime", label: "开机时长", value: "2h 18m", detail: "", meta: "模拟时长" }];
window.__TAURI_INTERNALS__ = { async invoke(command, args) {
  if (command === "local_widget_snapshot") return { date: "2026-09-23", source: args.source, status: "ok", message: "", rows: args.source === "computer.status" ? metrics : items.map(i => ({ id: i.id, label: i.title, value: i.dueAt ? "09-26 15:00" : "未定日期", detail: i.completed ? "已完成" : "待完成", meta: "", tone: i.completed ? -1 : 1 })) };
  if (command !== "local_tool_execute") throw new Error("Unsupported fixture command");
  const input = args.input;
  if (input.operation === "add") items.push({ id: `test-${Date.now()}`, title: input.title, dueAt: input.dueAt || null, revision: 1, completed: false });
  else if (input.operation !== "list") {
    const item = items.find(i => i.id === input.id);
    if (!item || item.revision !== input.revision) throw new Error("待办已变化，请重新查询后操作；未覆盖新修改");
    if (input.operation === "delete") items = items.filter(i => i !== item);
    else { item.revision++; if (input.operation === "update") { item.title = input.title; item.dueAt = input.dueAt || null; } else item.completed = input.operation === "complete"; }
  }
  return structuredClone(items);
}};
function App() {
  const [id, setId] = useState("upcoming-todos");
  const [open, setOpen] = useState(true);
  const component = BUILTIN_COMPONENT_CENTER.components.find(c => c.id === id);
  return <main style={{ padding: 24 }}><h2>本地组件界面回归 · 仅模拟数据</h2><p>此页面不连接设备、不读写待办文件，不发送网络请求。</p>{["upcoming-todos", "computer-status"].map(value => <button key={value} onClick={() => { setId(value); setOpen(true); }}>{value === "upcoming-todos" ? "打开近期待办" : "打开电脑状态"}</button>)}{open && <ComponentPreviewModal component={component} kind="tool" isLocal={false} bindings={[]} deviceConnected={false} onClose={() => setOpen(false)} onInstall={() => {}} onDelete={() => {}} />}</main>;
}
createRoot(document.getElementById("root")).render(<App />);
