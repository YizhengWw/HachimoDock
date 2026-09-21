/** User-owned watchlist editor. Native polling continues after this panel unmounts. */
import React, { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { ArrowDown, ArrowUp, Plus, RefreshCw, Trash2 } from "lucide-react";
import "./stock-watchlist.css";

export default function StockWatchlist({ usb }) {
  const [status, setStatus] = useState(null);
  const [market, setMarket] = useState("sh");
  const [code, setCode] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(true);
  const [results, setResults] = useState([]);
  const [searching, setSearching] = useState(false);
  const [searchError, setSearchError] = useState("");
  const [searched, setSearched] = useState(false);
  const [composing, setComposing] = useState(false);
  const searchEpoch = useRef(0);
  const isName = /[^a-zA-Z0-9.\-]/.test(code.trim());
  const editing = useRef(false);
  const requestEpoch = useRef(0);
  useEffect(() => {
    let stopped = false;
    async function poll() {
      const epoch = requestEpoch.current;
      try {
        const next = await invoke("stock_watchlist_status");
        if (!stopped && !editing.current && epoch === requestEpoch.current) setStatus(next);
      } catch (e) { if (!stopped) setError(String(e)); }
      finally { if (!stopped) setLoading(false); }
    }
    poll(); const timer = setInterval(poll, 2000);
    return () => { stopped = true; clearInterval(timer); };
  }, []);
  const symbols = status?.settings?.symbols || [];
  useEffect(() => {
    const epoch = ++searchEpoch.current;
    const query = code.trim();
    setResults([]); setSearchError(""); setSearched(false); setSearching(false);
    if (!query || composing) return;
    setSearching(true);
    const timer = setTimeout(async () => {
      try {
        const rows = await invoke("stock_search", { query });
        if (epoch === searchEpoch.current) { setResults(rows); setSearched(true); }
      } catch (e) { if (epoch === searchEpoch.current) setSearchError(String(e)); }
      finally { if (epoch === searchEpoch.current) setSearching(false); }
    }, 350);
    return () => { clearTimeout(timer); ++searchEpoch.current; };
  }, [code, composing]);
  async function action(work) {
    if (editing.current) return;
    editing.current = true; requestEpoch.current++; setBusy(true); setError("");
    try { await work(); } catch (e) { setError(String(e)); }
    finally { editing.current = false; setBusy(false); }
  }
  async function save(next) {
    setStatus(await invoke("stock_watchlist_save", { input: { symbols: next } }));
  }
  function addSymbol(symbol) {
    action(async () => {
      const quote = await invoke("stock_quote_lookup", { symbol });
      if (symbols.includes(quote.symbol)) throw new Error("这只股票已经在自选股中");
      await save([...symbols, quote.symbol]); setCode("");
    });
  }
  function add(event) {
    event.preventDefault();
    if (!composing && !isName) addSymbol(`${market}${code.trim()}`);
  }
  function move(index, delta) {
    const next = [...symbols]; [next[index], next[index + delta]] = [next[index + delta], next[index]];
    action(() => save(next));
  }
  return <details className="stock-watchlist" open>
    <summary>自选股行情 <span className="muted small">{symbols.length}/20 · 腾讯行情</span></summary>
    <div className="stock-watchlist__body">
      <p className="muted small">添加股票后，打开设备上的「自选股行情」组件查看。PC 每 2 秒批量更新，请保持 Pet Manager 运行并连接 USB；各市场可能有行情延迟，以显示的行情时间为准。</p>
      {usb?.connected && usb?.capabilities?.widgetData !== "p4-data-list-v1" && <p className="stock-watchlist__notice">当前设备固件尚不支持实时数据组件，请先升级固件。</p>}
      <form className="stock-watchlist__add" onSubmit={add}>
        <select aria-label="股票市场" value={market} onChange={(e) => setMarket(e.target.value)} disabled={busy}>
          <option value="sh">沪市</option><option value="sz">深市</option><option value="bj">北交所</option><option value="hk">港股</option><option value="us">美股</option>
        </select>
        <input aria-label="股票名称或代码" placeholder="中文名称或代码，如 贵州茅台、腾讯、600519" value={code}
          onChange={(e) => { ++searchEpoch.current; setResults([]); setCode(e.target.value); }}
          onCompositionStart={() => setComposing(true)} onCompositionEnd={() => setComposing(false)}
          maxLength={32} disabled={busy} autoComplete="off" />
        <button className="btn-primary btn-sm" disabled={busy || loading || !status || !code.trim() || isName || symbols.length >= 20}><Plus size={14} />{busy ? "处理中…" : "添加股票"}</button>
        <button className="btn-ghost btn-sm" type="button" disabled={busy || !symbols.length} onClick={() => action(async () => setStatus(await invoke("stock_watchlist_refresh")))}><RefreshCw size={14} />刷新行情</button>
      </form>
      {!!code.trim() && <div className="stock-watchlist__search" aria-label="股票搜索结果" aria-live="polite">
        <p className="muted small">中文名称跨市场检索，点击结果直接添加；手动输入代码时按左侧市场添加。</p>
        {searching && <p className="muted small">正在检索…</p>}
        {searchError && <p className="stock-watchlist__notice">{searchError}</p>}
        {searched && !results.length && <p className="muted small">未找到匹配股票，请换个名称或直接输入代码。</p>}
        {results.map((item) => <button key={item.symbol} type="button" className="stock-watchlist__result"
          disabled={busy || loading || !status || symbols.length >= 20 || symbols.includes(item.symbol)}
          onClick={() => addSymbol(item.symbol)}>
          <strong>{item.name}</strong><span>{item.market} · {item.symbol}</span>
          <small>{symbols.includes(item.symbol) ? "已添加" : "添加"}</small>
        </button>)}
      </div>}
      {(error || status?.error) && <p className="stock-watchlist__notice" role="alert">{error || status.error}</p>}
      {!symbols.length && <p className="muted small">{loading ? "读取自选股…" : "还没有自选股。输入中文名称检索，或选择市场并输入代码添加，列表和顺序会自动保存。"}</p>}
      {!!symbols.length && <div className="stock-watchlist__table"><table>
        <thead><tr><th>股票</th><th>最新价</th><th>涨跌幅</th><th>行情时间</th><th>管理</th></tr></thead>
        <tbody>{symbols.map((symbol, index) => {
          const q = status.quotes.find((item) => item.symbol === symbol);
          const tone = !q || q.stale ? "" : q.tone > 0 ? "stock-up" : q.tone < 0 ? "stock-down" : "";
          return <tr key={symbol}>
            <td><strong>{q?.name || symbol}</strong><small>{symbol} {q?.currency || ""}</small></td>
            <td className={tone}>{q?.price || "—"}</td>
            <td className={tone}>{q?.changePercent ? `${Number(q.changePercent) > 0 && !q.changePercent.startsWith("+") ? "+" : ""}${q.changePercent}%` : "—"}</td>
            <td><small>{q?.quoteTime || "等待数据"}{q?.stale ? " · 已过期" : ""}</small></td>
            <td><div className="stock-watchlist__controls">
              <button className="icon-btn" aria-label={`上移 ${symbol}`} disabled={busy || index === 0} onClick={() => move(index, -1)}><ArrowUp size={14} /></button>
              <button className="icon-btn" aria-label={`下移 ${symbol}`} disabled={busy || index === symbols.length - 1} onClick={() => move(index, 1)}><ArrowDown size={14} /></button>
              <button className="icon-btn" aria-label={`删除 ${symbol}`} disabled={busy} onClick={() => action(() => save(symbols.filter((s) => s !== symbol)))}><Trash2 size={14} /></button>
            </div></td>
          </tr>;
        })}</tbody>
      </table></div>}
      <p className="muted small">仅展示行情，不下单、不连接证券账户。修改股票无需重新安装组件。</p>
    </div>
  </details>;
}
