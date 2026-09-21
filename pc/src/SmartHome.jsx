/** Smart home: account consent, capability-driven controls, approval queue and execution receipts. */
import React, { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { Home, RefreshCw, ShieldCheck, Lightbulb, MessageCircle, X, Plus, ArrowUp, Trash2 } from "lucide-react";
import PageShell from "./shell/PageShell.jsx";
import Card from "./shell/Card.jsx";
import { HOME_STATUSES, visibleDevices, stepLabel, parsePropertyValue } from "./lib/smart-home.js";
import "./smart-home.css";

const empty = { connected: false, devices: [], scenes: [], tasks: [], pending: [], records: [] };
const request = (operation, input = {}) => invoke("smart_home_request", { operation, input });

function ValueInput({ property, value, onChange }) {
  const values = property.values || (property.format === "bool" ? [{ value: true, description: "开启" }, { value: false, description: "关闭" }] : null);
  if (values?.length) return <select aria-label={property.name} value={value} onChange={e => onChange(e.target.value)}><option value="">请选择</option>{values.map(v => <option key={String(v.value)} value={String(v.value)}>{v.description || String(v.value)}</option>)}</select>;
  return <input aria-label={property.name} type={property.format === "string" ? "text" : "number"} value={value} onChange={e => onChange(e.target.value)} min={property.range?.[0]} max={property.range?.[1]} step={property.range?.[2] || "any"} placeholder={property.range ? `${property.range[0]}–${property.range[1]} ${property.unit || ""}` : property.format} />;
}

export default function SmartHome() {
  const [data, setData] = useState(empty), [tab, setTab] = useState("devices");
  const [busy, setBusy] = useState(false), [error, setError] = useState(""), [notice, setNotice] = useState("");
  const [login, setLogin] = useState(""), [payload, setPayload] = useState("");
  const [home, setHome] = useState(""), [room, setRoom] = useState(""), [query, setQuery] = useState("");
  const [selected, setSelected] = useState(null), [spec, setSpec] = useState(null), [props, setProps] = useState([]), [values, setValues] = useState({});
  const [steps, setSteps] = useState([]), [title, setTitle] = useState("我的家居任务");
  const [text, setText] = useState(""), [messages, setMessages] = useState([]);
  const lock = useRef(false), alive = useRef(true), polling = useRef(false);
  async function refresh() { if (polling.current) return; polling.current = true; try { const next = await request("status"); if (alive.current) setData(next); } finally { polling.current = false; } }
  useEffect(() => { alive.current = true; refresh().catch(e => setError(String(e))); const timer = setInterval(() => { if (!lock.current) refresh().catch(() => {}); }, 5000); return () => { alive.current = false; clearInterval(timer); }; }, []);
  async function run(fn) {
    if (lock.current) return;
    lock.current = true; setBusy(true); setError(""); setNotice("");
    try { await fn(); await refresh(); } catch (e) { if (alive.current) setError(String(e)); }
    finally { lock.current = false; if (alive.current) setBusy(false); }
  }
  const homes = [...new Map(data.devices.map(d => [String(d.homeId), d.home || "我的家"])).entries()];
  const rooms = [...new Set(data.devices.filter(d => !home || String(d.homeId) === home).map(d => d.room))];
  function add(step) { if (steps.length >= 12) { setError("每个任务最多 12 步"); return; } setSteps(s => [...s, step]); setNotice("已加入任务草稿，尚未执行。可继续添加其他设备的步骤。"); }
  async function openDevice(d) { setSelected(d); setSpec(null); setValues({}); setProps([]); await run(async () => { const s = await request("describe", { did: d.did }); setSpec(s); setProps(await request("read", { did: d.did })); }); }
  const label = step => stepLabel(step, data.devices, data.scenes);
  function execute(step) { run(async () => { const result = await request("control", { title: stepLabel(step, data.devices, data.scenes), steps: [step] }); if (result.status === "awaiting_confirmation") { setSelected(null); setNotice("此操作需要确认，请核对待确认任务。"); } else { setNotice(result.status === "completed" ? "操作已处理，请查看执行记录中的核验结果。" : "操作未完成，请查看执行记录。"); if (selected) setProps(await request("read", { did: selected.did })); } }); }

  return <div className="smart-home"><PageShell title="智能家居" subtitle="智能家居能力来自 Xiaomi MiLoCo，让哈基米通过自然语言帮你查状态、控设备、完成家居任务。" actions={<>
    {data.connected && <button className="btn" disabled={busy} onClick={() => run(async () => { const r = await request("sync"); setNotice(r.warnings?.join("；") || "设备和场景已刷新"); })}><RefreshCw size={15} />刷新设备</button>}
  </>}>
    {error && <div className="home-message is-error" role="alert">{error}</div>}
    {notice && <div className="home-message" role="status">{notice}</div>}
    {!data.connected ? <Card><div className="home-welcome"><span className="home-mark"><Home size={30} /></span><h2>让哈基米帮你照顾家</h2><p>连接账号后，即可通过实时对话或任务页面使用账号已授权的设备与场景。</p>
      <button className="btn btn-primary" disabled={busy} onClick={() => run(async () => { const r = await request("login"); setLogin(r.url); await invoke("open_external_url", { url: r.url }); })}>连接米家账号</button>
      {login && <div className="home-auth"><p>在浏览器完成授权，将授权页提供的结果复制到这里。</p><button className="btn" onClick={() => invoke("open_external_url", { url: login }).catch(e => setError(String(e)))}>重新打开授权页</button><textarea aria-label="授权结果" value={payload} onChange={e => setPayload(e.target.value)} placeholder="粘贴授权结果或回调链接（仅用于本次连接）" autoComplete="off" /><button className="btn btn-primary" disabled={busy || !payload.trim()} onClick={() => run(async () => { await request("authorize", { payload }); setPayload(""); setLogin(""); await refresh(); await request("sync"); })}>完成连接</button></div>}
    </div></Card> : <>
      <Card title="家庭已连接" subtitle={`${data.devices.length} 个设备 · 最近同步：${data.syncedAt ? new Date(data.syncedAt * 1000).toLocaleString() : "尚未同步"}`} actions={<button className="btn" disabled={busy} onClick={() => { if (window.confirm("断开账号将清除本机家居任务和记录，并停止后续控制。确定断开？")) run(async () => { await request("disconnect"); setSelected(null); setSpec(null); setMessages([]); setSteps([]); }); }}>断开账号</button>}>
        <p className="home-availability">将设备切换到「实时对话」模式后，就可以直接通过语音让哈基米机控制本页的智能家居，例如「播放一首爵士乐」「换一首歌」「把客厅灯调暗，再拉上窗帘」。5 步以内的明确指令直接执行；超过 5 步或保存常用任务时，哈基米会说明任务，你直接说「确认」或「取消」即可，无需在本页点击。</p>
      </Card>
      <div className="home-tabs" role="tablist" aria-label="智能家居内容">{[["devices", "设备"], ["tasks", "场景与任务"], ["records", "执行记录"]].map(([id, name]) => <button role="tab" aria-selected={tab === id} key={id} className={tab === id ? "is-active" : ""} onClick={() => setTab(id)}>{name}</button>)}</div>
      {!!data.pending.length && <Card title={`待你确认 · ${data.pending.length}`} subtitle="请核对设备及操作。场景可能影响多个设备；涉及门锁、安防或电器启动时请特别留意。任务 5 分钟后过期。">{data.pending.map(p => <div className="home-plan" key={p.id}><strong>{p.title}</strong><ol>{p.steps.map((s, i) => <li key={i}>{label(s)}</li>)}</ol><div className="home-actions"><button className="btn btn-primary" disabled={busy} onClick={() => run(async () => { const r = await request("approve", { id: p.id }); setNotice(r.status === "completed" ? "任务步骤已处理，请在执行记录查看核验结果。" : "任务已停止，请查看执行记录。"); setTab("records"); })}>确认执行</button><button className="btn" disabled={busy} onClick={() => run(async () => { await request("save_task", { id: p.id }); setNotice("已保存为常用任务，尚未执行"); })}>保存为常用任务</button><button className="btn" disabled={busy} onClick={() => run(() => request("reject", { id: p.id }))}>取消</button></div></div>)}</Card>}
      {busy && <div className="home-message" role="status">正在处理，请稍候。<button className="btn" onClick={() => invoke("smart_home_cancel").then(() => setNotice("已请求停止后续执行；已发送的命令无法撤回。")).catch(e => setError(String(e)))}>停止后续执行</button></div>}
      {tab === "devices" && <>
        <div className="home-filters"><select aria-label="选择家庭" value={home} onChange={e => { setHome(e.target.value); setRoom(""); }}><option value="">全部家庭</option>{homes.map(([id, name]) => <option key={id} value={id}>{name}</option>)}</select><select aria-label="选择房间" value={room} onChange={e => setRoom(e.target.value)}><option value="">全部房间</option>{rooms.map(r => <option key={r}>{r}</option>)}</select><input aria-label="搜索设备" placeholder="搜索设备、房间或型号" value={query} onChange={e => setQuery(e.target.value)} /></div>
        <div className="home-grid">{visibleDevices(data.devices, home, room, query).map(d => <section className="home-device" key={d.did}><div className="home-device-head"><span className="home-mark"><Lightbulb size={20} /></span><span className={d.online ? "home-online" : "home-offline"}>{d.online ? "在线" : "离线"}</span></div><h3>{d.name}</h3><p>{d.home} · {d.room}</p><button className="btn" disabled={busy} onClick={() => openDevice(d)}>查看状态与控制</button></section>)}</div>
        {!visibleDevices(data.devices, home, room, query).length && <div className="home-empty">没有匹配的设备。请刷新列表，或调整家庭和搜索条件。</div>}
      </>}
      {tab === "tasks" && <>
        <Card title="告诉哈基米，你想做什么" subtitle="例如：把客厅灯调暗，再关上窗帘。也可以长按设备上的实时对话快捷键提出需求。语音输入仍用于给外部 Agent 输入文字。">
          <div className="home-chat">{messages.map((m, i) => <p key={i} className={m.role === "user" ? "is-user" : ""}><strong>{m.role === "user" ? "你" : "哈基米"}</strong>{m.content}</p>)}</div>
          <form className="home-chat-input" onSubmit={e => { e.preventDefault(); const content = text.trim(); if (!content) return; run(async () => { const reply = await invoke("smart_home_chat", { text: content, history: messages.slice(-10) }); setMessages(m => [...m, { role: "user", content }, { role: "assistant", content: reply }].slice(-12)); setText(""); }); }}><input aria-label="家居需求" value={text} onChange={e => setText(e.target.value)} maxLength={2000} placeholder="例如：帮我准备观影环境" /><button className="btn btn-primary" disabled={busy || !data.connected || !text.trim()}><MessageCircle size={15} />交给哈基米</button></form><button className="btn" disabled={busy} onClick={() => setMessages([])}>清空对话</button>
        </Card>
        <Card title="已有场景" subtitle="来自米家。场景内部动作由米家执行，哈基米无法逐个验证设备的最终状态。"><div className="home-grid">{data.scenes.map(s => <div className="home-plan" key={s.id}><strong>{s.name}</strong><p>{s.home}</p><button className="btn" disabled={busy} onClick={() => add({ kind: "scene", sceneId: s.id })}><Plus size={14} />加入任务</button></div>)}</div>{!data.scenes.length && <p>暂无可用场景。可先在米家创建，或使用设备控制组合任务。</p>}</Card>
        <Card title="常用任务">{data.tasks.map(t => <div className="home-plan" key={t.id}><strong>{t.title}</strong><ol>{t.steps.map((s, i) => <li key={i}>{label(s)}</li>)}</ol><div className="home-actions"><button className="btn" disabled={busy} onClick={() => { setSteps(t.steps); setTitle(t.title); }}>编辑副本</button><button className="btn btn-primary" disabled={busy || !data.connected} onClick={() => run(() => request("plan", t))}>核对并运行</button><button className="btn" disabled={busy} onClick={() => run(() => request("delete_task", { id: t.id }))}>删除任务</button></div></div>)}{!data.tasks.length && <p>组合设备操作后，可以保存到这里，下次直接使用。</p>}</Card>
      </>}
      {!!steps.length && <Card title="任务草稿" subtitle="按顺序执行，失败后停止后续步骤；不会自动撤回已完成的操作。"><input aria-label="任务名称" value={title} maxLength={80} onChange={e => setTitle(e.target.value)} /><ol className="home-draft">{steps.map((s, i) => <li key={i}><span>{label(s)}</span><button className="icon-btn" aria-label={`上移第 ${i + 1} 步`} disabled={!i || busy} onClick={() => setSteps(old => { const next = [...old]; [next[i - 1], next[i]] = [next[i], next[i - 1]]; return next; })}><ArrowUp size={14} /></button><button className="icon-btn" aria-label={`删除第 ${i + 1} 步`} disabled={busy} onClick={() => setSteps(old => old.filter((_, j) => j !== i))}><Trash2 size={14} /></button></li>)}</ol><div className="home-actions"><button className="btn" disabled={busy || steps.length >= 12} onClick={() => add({ kind: "wait", seconds: 2 })}>添加等待 2 秒</button><button className="btn btn-primary" disabled={busy || !data.connected} onClick={() => run(async () => { await request("plan", { title, steps }); setSteps([]); })}>检查任务</button><button className="btn" disabled={busy} onClick={() => setSteps([])}>清空草稿</button></div></Card>}
      {tab === "records" && <Card title="执行记录" subtitle="最多保留最近 50 次任务，仅保存在本机。已受理不代表设备已经完成动作。">{data.records.map(r => <div className="home-plan" key={r.id}><strong>{r.title}</strong><p>{new Date(r.at * 1000).toLocaleString()} · {HOME_STATUSES[r.status] || r.status}</p><ol>{r.results.map((row, i) => <li key={i}>{label(row.step)}<p className={row.error ? "home-error-text" : ""}>{row.error || HOME_STATUSES[row.result?.status] || "状态未确认"}</p></li>)}</ol></div>)}{!data.records.length && <div className="home-empty"><ShieldCheck size={28} /><p>还没有执行记录。任务确认后，结果会显示在这里。</p></div>}</Card>}
    </>}
    {selected && <div className="home-overlay" onClick={() => !busy && setSelected(null)}><section className="home-detail" role="dialog" aria-modal="true" aria-label={`${selected.name}的设备控制`} onClick={e => e.stopPropagation()}><header><div><h2>{selected.name}</h2><p>{selected.room} · {selected.model}</p></div><button className="icon-btn" aria-label="关闭设备详情" onClick={() => setSelected(null)}><X size={20} /></button></header>
      <p>控制项来自设备实际能力。轻量操作可直接执行，也可加入任务统一编排。离线设备可能无法取得最新状态。</p>
      {error && <div className="home-message is-error" role="alert">{error}</div>}{notice && <div className="home-message" role="status">{notice}</div>}{spec ? <>{spec.properties.map(p => { const key = `${p.siid}.${p.piid}`; const current = props.find(v => v.siid === p.siid && v.piid === p.piid); return <div className="home-control" key={key}><label>{p.service} · {p.name}</label><small>{current?.code === 0 ? `当前：${String(current.value)} ${p.unit || ""}` : "状态未获取或不可读"}</small>{p.access?.includes("write") && <div className="home-actions"><ValueInput property={p} value={values[key] ?? ""} onChange={value => setValues(v => ({ ...v, [key]: value }))} /><button className="btn btn-primary" disabled={busy || values[key] === undefined || values[key] === ""} onClick={() => { try { execute({ kind: "set", did: selected.did, siid: p.siid, piid: p.piid, value: parsePropertyValue(p, values[key]) }); } catch (e) { setError(e.message); } }}>执行本次</button><button className="btn" disabled={busy || values[key] === undefined || values[key] === ""} onClick={() => { try { add({ kind: "set", did: selected.did, siid: p.siid, piid: p.piid, value: parsePropertyValue(p, values[key]), label: p.name }); } catch (e) { setError(e.message); } }}>加入任务</button></div>}</div>; })}
        {spec.actions.map(a => { const key = `a${a.siid}.${a.aiid}`; const inputs = (a.in || []).map(id => spec.properties.find(p => p.siid === a.siid && p.piid === id)); return <div className="home-control" key={key}><label>{a.service} · {a.name}</label>{inputs.map((p, i) => p ? <ValueInput key={i} property={p} value={values[`${key}.${i}`] ?? ""} onChange={value => setValues(v => ({ ...v, [`${key}.${i}`]: value }))} /> : <p key={i}>此动作参数暂无法解析</p>)}<button className="btn btn-primary" disabled={busy || inputs.some((p, i) => !p || values[`${key}.${i}`] === undefined || values[`${key}.${i}`] === "")} onClick={() => { try { execute({ kind: "action", did: selected.did, siid: a.siid, aiid: a.aiid, args: inputs.map((p, i) => parsePropertyValue(p, values[`${key}.${i}`])) }); } catch (e) { setError(e.message); } }}>执行本次</button><button className="btn" disabled={busy || inputs.some((p, i) => !p || values[`${key}.${i}`] === undefined || values[`${key}.${i}`] === "")} onClick={() => { try { add({ kind: "action", did: selected.did, siid: a.siid, aiid: a.aiid, args: inputs.map((p, i) => parsePropertyValue(p, values[`${key}.${i}`])), label: a.name }); } catch (e) { setError(e.message); } }}>加入任务</button></div>; })}<button className="btn btn-primary" onClick={() => { setSelected(null); setTab("tasks"); }}>查看任务草稿</button></> : <p>{busy ? "正在获取设备能力……" : "暂时无法获取设备能力，请关闭后重试。"}</p>}
    </section></div>}
  </PageShell></div>;
}
