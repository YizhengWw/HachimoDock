// Development-only synthetic state; never reads a user's library or plays audio.
import React from "react";
import { createRoot } from "react-dom/client";
import MusicPlayer from "../../src/component-center/MusicPlayer.jsx";
import "../../src/styles.css";
if (!import.meta.env.DEV) throw new Error("Development fixture only");
const track = n => ({ key: `fixture:${n}`, title: n ? "夜晚的爵士 · 模拟曲目" : "晴天里的小小旋律 · 模拟曲目", artist: "界面测试 · 非真实音源", album: "演示专辑", durationMs: 201000 });
let state = { state: "playing", current: track(0), queue: [track(0), track(1)], positionMs: 62000, volume: 65, mode: "sequence", cover: "", message: "界面模拟，不连接设备或音乐服务", connected: true };
window.__TAURI_INTERNALS__ = { async invoke(command, { input }) {
  if (command !== "music_player_request") throw new Error("No filesystem or hardware in this fixture");
  const op=input.operation;
  if (op==="search") return { tracks:[track(2)] };
  if(op==="toggle")state.state=state.state==="playing"?"paused":"playing";
  if(op==="volume")state.volume=input.volume;
  if(op==="mode")state.mode=input.mode;
  if(op==="stop")state.state="idle";
  if(op==="seek")state.positionMs=input.positionMs;
  if(op==="play") {state.current=state.queue.find(t=>t.key===input.key)||track(2);state.state="playing";}
  if(op==="enqueue"&&!state.queue.some(t=>t.key===input.key))state.queue.push(track(2));
  if(op==="remove")state.queue=state.queue.filter(t=>t.key!==input.key);
  if(op==="move"){const at=state.queue.findIndex(t=>t.key===input.key);const [t]=state.queue.splice(at,1);state.queue.splice(input.index,0,t);}
  return structuredClone(state);
}};
createRoot(document.getElementById("root")).render(<React.StrictMode><main style={{ maxWidth: 960, margin:"24px auto", padding:24, background:"white", borderRadius:18 }}><MusicPlayer /></main></React.StrictMode>);
