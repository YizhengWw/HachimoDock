/** Per-computer LLM networking; internal CA status never exposes certificate contents. */
import React, { useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { open } from "@tauri-apps/plugin-dialog";

export default function LlmNetworkSettings() {
  const [settings, setSettings] = useState({ mode: "system", proxyUrl: "", certificateConfigured: false });
  const [caFile, setCaFile] = useState(null);
  const [clearCertificate, setClearCertificate] = useState(false);
  const [busy, setBusy] = useState(true);
  const [dirty, setDirty] = useState(false);
  const [message, setMessage] = useState("");
  const [results, setResults] = useState([]);
  useEffect(() => {
    let cancelled = false;
    invoke("load_llm_network_settings").then((value) => { if (!cancelled) setSettings(value); })
      .catch((error) => { if (!cancelled) setMessage(`读取网络设置失败：${error}`); })
      .finally(() => { if (!cancelled) setBusy(false); });
    return () => { cancelled = true; };
  }, []);
  const change = (patch) => { setSettings((current) => ({ ...current, ...patch })); setDirty(true); setResults([]); setMessage(""); };
  const chooseCertificate = async () => {
    try {
      const selected = await open({ multiple: false, directory: false, filters: [{ name: "PEM 公共证书", extensions: ["pem", "crt", "cer"] }] });
      if (typeof selected === "string") { setCaFile(selected); setClearCertificate(false); setDirty(true); setResults([]); }
    } catch (error) { setMessage(`选择证书失败：${error}`); }
  };
  const save = async () => {
    setBusy(true); setResults([]);
    try {
      const status = await invoke("save_llm_network_settings", { input: { mode: settings.mode, proxyUrl: settings.proxyUrl, caFile, clearCertificate } });
      setSettings(status); setCaFile(null); setClearCertificate(false); setDirty(false);
      setMessage("已保存到这台电脑；新的云服务请求生效，正在进行的语音连接请重新进入。请点击测试连接。");
    } catch (error) { setMessage(`保存失败：${error}`); }
    finally { setBusy(false); }
  };
  const test = async () => {
    setBusy(true); setMessage("正在检测火山与 DeepSeek 的 HTTPS 连接…"); setResults([]);
    try {
      setResults(await invoke("test_llm_network_settings"));
      setMessage("测试不发送 API Key，也不验证模型额度；HTTPS 可达不等于已获得服务权限。");
    } catch (error) { setMessage(`检测失败：${error}`); }
    finally { setBusy(false); }
  };
  return (
    <details className="llm-network-settings">
      <summary>云服务网络（内网 / 代理 / 企业证书）</summary>
      <p className="muted small">设置只保存在当前电脑，适用于模型列表、图片与视频生成、素材下载、语音识别与合成、实时对话。不会内置或扫描任何代理地址，不影响设备与本机 Agent 通信。</p>
      <div className="api-settings__form">
        <label className="ui-field" htmlFor="llm-network-mode">
          <span className="ui-field__label">连接方式</span>
          <select id="llm-network-mode" className="ui-control" value={settings.mode} disabled={busy} onChange={(e) => change({ mode: e.target.value })}>
            <option value="system">跟随系统（推荐）</option>
            <option value="direct">直接连接（不使用代理）</option>
            <option value="manual">手动配置这台电脑的代理</option>
          </select>
        </label>
        {settings.mode === "manual" && <label className="ui-field" htmlFor="llm-network-proxy">
          <span className="ui-field__label">HTTP / HTTPS 代理地址</span>
          <input id="llm-network-proxy" className="ui-control" autoComplete="off" value={settings.proxyUrl} disabled={busy}
            placeholder="填写网络管理员提供的地址与端口" onChange={(e) => change({ proxyUrl: e.target.value })} />
        </label>}
        <p className="muted small">跟随系统会读取当前进程的代理环境变量或系统 HTTP/HTTPS 代理。若公司只提供 PAC 自动配置或需要特殊认证，请向 IT 确认可用代理设置，不会自动绕过公司网络策略。</p>
        <div className="llm-network-settings__actions">
          <button type="button" className="btn-ghost btn-sm" disabled={busy} onClick={chooseCertificate}>导入企业 CA 证书</button>
          <button type="button" className="btn-ghost btn-sm" disabled={busy || (!caFile && !settings.certificateConfigured)} onClick={() => { setCaFile(null); setClearCertificate(true); setDirty(true); setResults([]); }}>移除导入的证书</button>
          <span className="muted small">{caFile ? `待导入：${caFile.split(/[\\/]/).pop()}` : clearCertificate ? "保存后移除" : settings.certificateConfigured ? "已导入企业 CA" : "使用系统信任证书"}</span>
        </div>
        <p className="muted small">仅导入 IT 提供并确认可信的 PEM 公共证书，不能导入私钥。证书仅用于此应用，不修改系统信任设置，也不关闭 TLS 校验。</p>
        {settings.bundledCertificateConfigured && <p className="muted small">此内部版已内置企业 CA 公共证书，无需重复导入。移除手动导入的证书不会移除内置证书。</p>}
        <div className="llm-network-settings__actions">
          <button type="button" className="btn-primary btn-sm" disabled={busy} onClick={save}>保存网络设置</button>
          <button type="button" className="btn-ghost btn-sm" disabled={busy || dirty} onClick={test}>测试已保存配置</button>
          {dirty && <span className="muted small">请先保存再测试</span>}
        </div>
      </div>
      {message && <p className="muted small" role="status">{message}</p>}
      <ul className="llm-network-settings__results" aria-live="polite">
        {results.map((result) => <li key={result.provider} className={`api-settings__result ${result.reachable ? "is-success" : "is-error"}`}>
          {result.provider}：{result.message}
        </li>)}
      </ul>
    </details>
  );
}
