/** Durable generation checkpoint, private to this app origin. No credentials.
 * IDB transactions atomically commit source/reference images, prompts and cloud IDs.
 * A submission intent without an ID is deliberately not replayable. */
import { CUSTOM_GENERATION_FAMILIES } from "./families.js";

// Reconstruct display state from durable evidence, never from an absent UI progress object.
export function summarizeGenerationSession(session) {
  const ids = session.manifest?.entries?.map(e => e.family) || CUSTOM_GENERATION_FAMILIES.map(e => e.family);
  const actions = session.actions || {};
  const completed = ids.filter(id => actions[id]?.videoBytes?.byteLength > 0).length;
  const failed = ids.filter(id => actions[id]?.failed && !actions[id]?.videoBytes?.byteLength).length;
  const total = ids.length;
  let status = "paused", label = "任务尚未完成";
  if (session.finished && completed === total) { status = "completed"; label = "生成完成"; }
  else if (session.dismissed) label = "已停止继续";
  else if (session.phase === "converting" || session.phase === "conversion_failed") label = "动画已保存，设备格式待完成";
  else if (session.phase === "waiting_network") label = "等待网络恢复，已提交任务不会重复生成";
  else if (session.terminalFailure || session.phase === "failed") { status = "failed"; label = completed ? "部分已保存，后续生成失败" : "生成失败"; }
  else if (completed > 0 && !session.trialApproved) label = "试片已保存，等待确认";
  else if (session.reference && !session.referenceApproved) label = "参考图已准备，等待确认";
  else if (completed > 0) label = "部分已保存，可继续";
  const families = Object.fromEntries(ids.map(id => [id, { status: actions[id]?.videoBytes?.byteLength > 0 ? "done" : actions[id]?.failed ? "failed" : actions[id]?.taskId ? "polling" : "pending" }]));
  return { status, label, completed, total, failed, families };
}

export function canAutoResume(session) {
  if(!session?.automatic || session.finished || session.dismissed || session.pausedByUser || (session.autoResumes||0)>=3)return false;
  if(['failed','conversion_failed'].includes(session.phase))return false;
  if(session.imageIntent&&!session.reference || session.thinkingIntent&&!session.manifest || session.referenceQualityIntent&&!session.referenceQuality)return false;
  if(session.referenceQuality?.pass===false)return false;
  return !Object.values(session.actions||{}).some(a=>a.failed || a.quality?.pass===false || a.qualityIntent&&!a.quality || a.intent&&!a.taskId&&!a.videoBytes
    || a.startFrameIntent&&!a.startFrameRaw || a.startFrameQuality?.pass===false || a.startFrameQualityIntent&&!a.startFrameQuality);
}
export function serialQueue() {
  let tail = Promise.resolve();
  return (operation) => {
    const result = tail.then(operation);
    tail = result.catch(() => {});
    return result;
  };
}

export function publicGenerationConfig(config = {}) {
  return Object.fromEntries(["provider", "model", "thinkingModel", "fastGeneration", "duration", "resolution", "ratio", "generateAudio", "cameraFixed", "watermark", "seed"]
    .filter(key => config[key] !== undefined).map(key => [key, config[key]]));
}

export function assertResumableAction(action, provider) {
  if (action?.videoBytes) return;
  if (action?.taskId && provider === "volcengine") return;
  if (action?.intent) throw new Error("此动作曾提交，但无法确认云端结果；不会自动重复收费。请先在服务控制台核对，再决定单独重做。");
}

// A new, explicit user request may replace pre-video work. Never discard an
// accepted/unknown video submission or a partially saved appearance implicitly.
export function generationSessionNeedsProtection(session) {
  if (!session || session.finished || session.dismissed) return false;
  if (session.appearanceId || session.reference) return true;
  return Object.values(session.actions || {}).some(action =>
    action && (action.intent || action.taskId || action.videoBytes || action.videoUrl));
}

async function database() {
  if (!globalThis.indexedDB) throw new Error("本地任务存储不可用，已停止生成，避免无法恢复的付费请求。");
  return new Promise((resolve, reject) => {
    const request = indexedDB.open("pet-avatar-generation", 1);
    request.onupgradeneeded = () => request.result.createObjectStore("jobs");
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(new Error("无法打开本地生成任务存储"));
  });
}

export async function loadGenerationSession(key = "current") {
  const db = await database();
  try {
    return await new Promise((resolve, reject) => {
      const tx = db.transaction("jobs", "readonly");
      const request = tx.objectStore("jobs").get(key);
      tx.oncomplete = () => resolve(request.result || null);
      tx.onabort = () => reject(new Error("读取生成任务失败"));
    });
  } finally { db.close(); }
}

export async function listGenerationHistory() {
  const db = await database();
  try {
    return await new Promise((resolve, reject) => {
      const tx = db.transaction("jobs", "readonly");
      const store = tx.objectStore("jobs");
      const current = store.get("current");
      const rows = [];
      const cursor = store.openCursor();
      cursor.onsuccess = () => {
        const entry = cursor.result;
        if (!entry) return;
        if (String(entry.key).startsWith("archive:")) {
          const s = entry.value;
          rows.push({ key: entry.key, id: s.id, appearanceName: s.appearanceName,
            createdAt: s.createdAt, finished: !!s.finished, dismissed: !!s.dismissed,
            ...summarizeGenerationSession(s) });
        }
        entry.continue();
      };
      tx.oncomplete = () => resolve(rows.filter(s => s.id !== current.result?.id)
        .sort((a,b) => (b.createdAt || 0) - (a.createdAt || 0)));
      tx.onabort = () => reject(new Error("读取生成历史失败"));
    });
  } finally { db.close(); }
}

export async function saveGenerationSession(session) {
  const db = await database();
  try {
    await new Promise((resolve, reject) => {
      const tx = db.transaction("jobs", "readwrite");
      tx.objectStore("jobs").put(structuredClone(session), "current");
      tx.oncomplete = resolve;
      tx.onabort = () => reject(new Error("保存生成任务失败，后续付费请求已停止；请检查磁盘空间。"));
    });
  } finally { db.close(); }
}

// A user-created task is a distinct request, not a retry of the previous one.
// Retain old checkpoints atomically instead of imposing a global failed-job lock.
export async function beginGenerationSession(session) {
  const db = await database();
  try {
    await new Promise((resolve, reject) => {
      const tx = db.transaction("jobs", "readwrite");
      const store = tx.objectStore("jobs");
      const request = store.get("current");
      request.onsuccess = () => {
        const previous = request.result;
        if (previous && previous.id !== session.id) {
          store.put(previous, `archive:${previous.id || crypto.randomUUID()}`);
        }
        store.put(structuredClone(session), "current");
      };
      tx.oncomplete = resolve;
      tx.onabort = () => reject(new Error("无法保留上一任务记录，新任务尚未提交。"));
    });
  } finally { db.close(); }
}
