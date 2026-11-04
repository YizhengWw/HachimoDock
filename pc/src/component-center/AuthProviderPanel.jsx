import React, { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";

/* PC-side authorization for authenticated widget/data providers. Tokens never reach this
   panel: status carries only booleans, the user code and the verification URL. */
export default function AuthProviderPanel({ source, request = invoke }) {
  const [status, setStatus] = useState(null);
  const [snapshot, setSnapshot] = useState(null);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const [clientId, setClientId] = useState("");
  const [phase, setPhase] = useState("idle");
  const mounted = useRef(false);
  const provider = status?.providers?.find(p => p.source === source);
  async function refresh() {
    try {
      const s = await request("auth_provider_status");
      if (!mounted.current) return;
      setStatus(s);
      const snap = await request("local_widget_snapshot", { source });
      if (mounted.current) setSnapshot(snap);
    } catch (e) { if (mounted.current) setError(String(e)); }
  }
  useEffect(() => { mounted.current = true; refresh(); const timer = setInterval(refresh, 5000); return () => { mounted.current = false; clearInterval(timer); }; }, [source]);
  async function run(name, args = {}) {
    setBusy(true); setError("");
    try { const v = await request(name, args); if (name === "auth_provider_poll") setPhase(v?.phase || "idle"); await refresh(); return v; }
    catch (e) { setError(String(e)); }
    finally { setBusy(false); }
  }
  if (!provider) return <section className="component-app" aria-label="鉴权数据源"><p>{error || "读取提供器状态…"}</p></section>;
  return (
    <section className="component-app" aria-label={`${provider.title}授权`}>
      <div className="component-app__heading"><div><h3>{provider.title}</h3><p>鉴权数据源 · 令牌只保存在这台电脑，不进资源包、设备或日志</p></div><span>{!provider.verified ? "接口待确认" : provider.authorized ? "已授权" : "未授权"}</span></div>
      {!provider.verified && <p role="alert">此数据源的接口契约尚未确认，提供器未启用；组件在设备上显示“接口契约尚未确认”。确认端点、鉴权方式与使用条款后再接入，不会猜测或冒充其他客户端。</p>}
      {provider.verified && <>
        <p>允许联网的主机：{provider.hosts.join("、")}。仅发送你创建的 OAuth 应用 client_id、设备码和授权后的 Bearer 令牌；不发送聊天、待办或设备信息。</p>
        {!provider.clientIdConfigured && <form onSubmit={e => { e.preventDefault(); run("auth_provider_configure", { source, clientId }); }}>
          <label>OAuth 应用 client_id <input value={clientId} onChange={e => setClientId(e.target.value)} maxLength={128} disabled={busy} /></label>
          <button disabled={busy || !clientId.trim()}>保存 client_id</button>
        </form>}
        {provider.clientIdConfigured && !provider.authorized && !provider.pending && <button disabled={busy} onClick={() => run("auth_provider_begin", { source })}>开始设备码授权</button>}
        {provider.pending && !provider.authorized && <div>
          <p>在浏览器打开 <code>{provider.pending.verificationUri}</code>，输入代码 <strong>{provider.pending.userCode}</strong>，然后点“我已授权，检查”。{provider.pending.expired ? "（此代码已过期，请重新开始）" : ""}</p>
          <button disabled={busy} onClick={() => run("auth_provider_poll", { source })}>我已授权，检查</button>
          <span role="status">{{ pending: "等待授权…", authorized: "已授权", expired: "代码已过期", denied: "你拒绝了授权", error: "授权出错" }[phase] || ""}</span>
        </div>}
        {provider.authorized && <button disabled={busy} onClick={() => { if (window.confirm("删除这台电脑上的授权令牌？设备上的数据会显示未授权。")) run("auth_provider_forget", { source }); }}>撤销授权并删除令牌</button>}
      </>}
      {snapshot && <div aria-label="数据预览"><p role="status">{snapshot.status === "ok" ? `最近更新 ${snapshot.date}` : snapshot.message}</p>
        {snapshot.rows?.map(r => <div key={r.id}><b>{r.label}</b> {r.value} <small>{r.detail} {r.meta}</small></div>)}</div>}
      {error && <p role="alert">{error}</p>}
    </section>
  );
}
