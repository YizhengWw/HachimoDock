export const MUSIC_SOURCE = "audio.player";
export const MUSIC_STATES = Object.freeze({ idle: "准备播放", queued: "等待语音结束", loading: "正在准备", buffering: "正在缓冲", playing: "正在播放", paused: "已暂停", interrupted: "语音已暂停音乐", ended: "播放结束", error: "播放失败" });
export const EMPTY_MUSIC = Object.freeze({ state: "idle", queue: [], positionMs: 0, volume: 65, mode: "sequence", current: null, cover: "", message: "搜索喜欢的音乐，让哈基米唱给你听", connected: false });
export function musicTime(ms) {
  if (!Number.isFinite(ms) || ms < 0) return "--:--";
  const seconds = Math.floor(ms / 1000);
  return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, "0")}`;
}
export function musicProgress(snapshot) {
  const duration = Number(snapshot?.current?.durationMs || 0);
  return duration > 0 ? Math.max(0, Math.min(100, Number(snapshot.positionMs || 0) / duration * 100)) : 0;
}
