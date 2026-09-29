import React, { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { open } from "@tauri-apps/plugin-dialog";
import { Disc3, Play, Pause, SkipBack, SkipForward, ListMusic, Volume2, Search, Plus, FolderPlus, Trash2, Music2, ArrowUp, ArrowDown } from "lucide-react";
import { EMPTY_MUSIC, MUSIC_STATES, musicTime, musicProgress } from "./music-player-model.js";
import { MUSIC_SOURCE_URL, openMusicSourceLink } from "./music-source-link.js";
import "./music-player.css";

function useMusic(active = true) {
  const [snapshot, setSnapshot] = useState(EMPTY_MUSIC);
  useEffect(() => {
    if (!active || !window.__TAURI_INTERNALS__) return;
    let stopped = false, timer;
    const refresh = async () => {
      try { const next = await invoke("music_player_request", { input: { operation: "status" } }); if (!stopped) setSnapshot(next); }
      catch (e) { if (!stopped) setSnapshot(s => ({ ...s, message: String(e) })); }
      if (!stopped) timer = setTimeout(refresh, 800);
    };
    refresh(); return () => { stopped = true; clearTimeout(timer); };
  }, [active]);
  return [snapshot, setSnapshot];
}

export function MusicScreen({ snapshot = EMPTY_MUSIC, onCommand, compact = false }) {
  const s = snapshot, playing = s.state === "playing", current = s.current;
  return <div className={`music-screen${compact ? " music-screen--compact" : ""}`}>
    <header><span><Music2 /> 随身听</span><span className="music-screen__status"><i className={playing ? "is-playing" : ""} />{MUSIC_STATES[s.state] || "等待连接"}</span></header>
    <div className="music-screen__main">
      <div className="music-screen__cover">{s.cover ? <img src={s.cover} alt="当前专辑封面" /> : <div className="music-screen__record"><div><Music2 /></div></div>}</div>
      <div className="music-screen__track"><span className="music-screen__eyebrow">HACHIMO · MUSIC</span><h3 title={current?.title}>{current?.title || "音乐，陪你一会儿"}</h3><p>{current?.artist || "搜索歌曲 · 语音点播"}</p><span className="music-screen__output">哈基米扬声器</span></div>
    </div>
    <div className="music-screen__timeline"><div className="music-screen__rail"><i style={{ width: `${musicProgress(s)}%` }} /></div><div><span>{musicTime(s.positionMs)}</span><span>{current?.durationMs ? musicTime(current.durationMs) : "--:--"}</span></div></div>
    <div className="music-screen__controls"><span className="music-screen__volume"><Volume2 /> {s.volume}%</span><div><button aria-label="上一首" disabled={!onCommand} onClick={() => onCommand?.({ operation: "previous" })}><SkipBack /></button><button aria-label={playing ? "暂停" : "播放"} className="music-screen__play" disabled={!onCommand} onClick={() => onCommand?.({ operation: "toggle" })}>{playing ? <Pause /> : <Play />}</button><button aria-label="下一首" disabled={!onCommand} onClick={() => onCommand?.({ operation: "next" })}><SkipForward /></button></div><span className="music-screen__queue"><ListMusic /> {s.queue?.length || 0}</span></div>
    <footer>{s.message || "摇杆切歌与音量 · 全局键退出"}</footer>
  </div>;
}
export function MusicScreenPreview({ active = false }) {
  const [snapshot] = useMusic(active);
  return <MusicScreen snapshot={snapshot} compact />;
}
export default function MusicPlayer() {
  const [snapshot, setSnapshot] = useMusic();
  const [query, setQuery] = useState(""), [results, setResults] = useState([]);
  const [busy, setBusy] = useState(false), [error, setError] = useState(""), [searched, setSearched] = useState(false);
  const [sourceLinkError, setSourceLinkError] = useState("");
  const mounted = useRef(true), searchEpoch = useRef(0);
  useEffect(() => { mounted.current = true; return () => { mounted.current = false; }; }, []);
  async function command(input) {
    setError("");
    try { const reply = await invoke("music_player_request", { input }); if (mounted.current) setSnapshot(reply.accepted ? reply.state : reply); }
    catch (e) { if (mounted.current) setError(String(e)); }
  }
  async function search(e) {
    e.preventDefault(); if (!query.trim() || busy) return;
    const epoch = ++searchEpoch.current; setBusy(true); setError("");
    try { const reply = await invoke("music_player_request", { input: { operation: "search", query: query.trim() } }); if (mounted.current && epoch === searchEpoch.current) { setResults(reply.tracks || []); setSearched(true); } }
    catch (e) { if (mounted.current && epoch === searchEpoch.current) setError(String(e)); }
    finally { if (mounted.current && epoch === searchEpoch.current) setBusy(false); }
  }
  async function importFiles() {
    setError("");
    try { const paths = await open({ multiple: true, directory: false, filters: [{ name: "音频", extensions: ["mp3", "m4a", "wav", "flac", "ogg", "aac"] }] }); if (paths) { const next = await invoke("music_player_import", { paths: Array.isArray(paths) ? paths : [paths] }); if (mounted.current) setSnapshot(next); } }
    catch (e) { if (mounted.current) setError(String(e)); }
  }
  return <section className="music-library" aria-label="随身听曲库">
    <div className="music-library__intro"><div><h3>让喜欢的音乐，留在桌边</h3>
      <p className="music-library__credit">感谢 <a href={MUSIC_SOURCE_URL} target="_blank" rel="noopener noreferrer" onClick={event => openMusicSourceLink(event, { isDesktop: Boolean(window.__TAURI_INTERNALS__), invokeExternal: invoke, onError: message => { if (mounted.current) setSourceLinkError(message); } })}>GD 音乐台</a> 提供 API 服务。仅供学习参考，禁止下载、传播或商用；可播放情况取决于音源。搜索按需发起，避免频繁请求。</p>
      {sourceLinkError && <p className="music-library__source-error" role="alert">{sourceLinkError}</p>}
      <p>在组件内使用设备全局设置的实时对话快捷键（默认长按 SW2），说出想听的歌曲；也可在这里搜索、添加。声音由哈基米本机播放，请保持 PC 运行并连接 USB。</p><p>长按 SW1 进入或退出纯歌词模式，短按仍为播放/暂停；无同步歌词时会提示。若该长按已改绑实时对话或退出，则优先执行全局设置。</p><p>退出随身听可继续后台播放；回到宠物主页后，再按一次退出键即可停止音乐（需固件 0.7.69 或更新版本）。退出键跟随设备全局设置，默认短按 SW3。</p></div><button onClick={importFiles}><FolderPlus size={16} />添加本地音频</button></div>
    <div className="music-library__layout"><MusicScreen snapshot={snapshot} onCommand={command} /><div className="music-library__settings"><h4>播放设置</h4><label>音乐音量 <output>{snapshot.volume}%</output><input aria-label="音乐音量" type="range" min="0" max="100" value={snapshot.volume} onChange={e => setSnapshot(s => ({ ...s, volume: Number(e.target.value) }))} onPointerUp={e => command({ operation: "volume", volume: Number(e.target.value) })} onKeyUp={e => command({ operation: "volume", volume: Number(e.target.value) })} /></label><label>播放模式<select value={snapshot.mode} onChange={e => command({ operation: "mode", mode: e.target.value })}><option value="sequence">顺序播放</option><option value="loop">列表循环</option><option value="single">单曲循环</option><option value="shuffle">随机播放</option></select></label><div className="music-library__seek"><button onClick={() => command({ operation: "seek", positionMs: Math.max(0, snapshot.positionMs - 10000) })} disabled={!snapshot.current}>后退 10 秒</button><button onClick={() => command({ operation: "seek", positionMs: Math.min(3600000, snapshot.positionMs + 10000) })} disabled={!snapshot.current}>快进 10 秒</button></div><button onClick={() => command({ operation: "stop" })}>停止播放</button><p>进入语音交互会暂停音乐，不与宠物说话声混播。返回播放器可继续播放。</p></div></div>
    <form onSubmit={search} className="music-library__search"><Search size={18} /><input value={query} maxLength={100} onChange={e => setQuery(e.target.value)} placeholder="搜索歌曲、歌手或音乐风格" aria-label="搜索歌曲" /><button disabled={busy || !query.trim()} type="submit">{busy ? "搜索中…" : "搜索"}</button></form>
    {error && <p className="music-library__error" role="alert">{error}</p>}
    {searched && <section className="music-library__list"><h4>搜索结果 <span>{results.length}</span></h4>{results.length ? results.map(track => <div key={track.key}><Disc3 /><span><strong>{track.title}</strong><small>{track.artist} · {track.album}</small></span><button aria-label={`播放 ${track.title}`} onClick={() => command({ operation: "play", key: track.key })}><Play size={17} /></button><button aria-label={`添加 ${track.title}`} onClick={() => command({ operation: "enqueue", key: track.key })}><Plus size={17} /></button></div>) : <p>没有找到歌曲，请调整歌名或歌手后再试。</p>}</section>}
    <section className="music-library__list"><h4>我的播放列表 <span>{snapshot.queue.length}/20</span></h4>{snapshot.queue.length ? snapshot.queue.map((track, index) => <div key={track.key} className={snapshot.current?.key === track.key ? "is-current" : ""}><Music2 /><span><strong>{track.title}</strong><small>{track.artist || "未知歌手"}</small></span><button aria-label={`播放 ${track.title}`} onClick={() => command({ operation: "play", key: track.key })}><Play size={17} /></button><button aria-label={`上移 ${track.title}`} disabled={index === 0} onClick={() => command({ operation: "move", key: track.key, index: index - 1 })}><ArrowUp size={16} /></button><button aria-label={`下移 ${track.title}`} disabled={index === snapshot.queue.length - 1} onClick={() => command({ operation: "move", key: track.key, index: index + 1 })}><ArrowDown size={16} /></button><button aria-label={`移除 ${track.title}`} disabled={snapshot.current?.key === track.key && !["idle", "ended", "interrupted", "error"].includes(snapshot.state)} onClick={() => command({ operation: "remove", key: track.key })}><Trash2 size={16} /></button></div>) : <p>还没有歌曲。搜索后添加，或导入自己的音频。</p>}</section>
  </section>;
}
