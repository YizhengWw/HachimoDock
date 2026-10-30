// Wire geometry matches the device's session card; images contain alpha only.
export const BUBBLE_STYLES = Object.freeze({
  1: { width: 474, height: 38, px: 32, lines: 1, lineHeight: 38, maxPages: 1 },
  2: { width: 496, height: 54, px: 24, lines: 2, lineHeight: 27, maxPages: 16 },
  3: { width: 348, height: 38, px: 32, lines: 1, lineHeight: 38, maxPages: 1 },
  4: { width: 496, height: 54, px: 24, lines: 2, lineHeight: 27, maxPages: 1 },
});
const encoder = new TextEncoder();
const FONT_FAMILY = '"Microsoft YaHei", "PingFang SC", "Noto Sans CJK SC", sans-serif';

export function clampBubbleText(value, limit) {
  let result = "", size = 0;
  for (const char of String(value ?? "").split("\0", 1)[0]) {
    const bytes = encoder.encode(char).length;
    if (size + bytes > limit) break;
    result += char; size += bytes;
  }
  return result;
}

export function bubblePixelKey(style, text) {
  let hash = BigInt("14695981039346656037");
  for (const byte of [style, ...encoder.encode(text)]) {
    hash = BigInt.asUintN(64, (hash ^ BigInt(byte)) * BigInt("1099511628211"));
  }
  return hash.toString(16).padStart(16, "0");
}

export function bubbleChecksum(bytes) {
  let hash = 2166136261;
  for (const byte of bytes) hash = Math.imul(hash ^ byte, 16777619) >>> 0;
  return hash.toString(16).padStart(8, "0");
}

export function encodeBubbleAlpha(rgba) {
  const out = [];
  let previous = -1, count = 0;
  for (let i = 3; i < rgba.length; i += 4) {
    const alpha = Math.round(rgba[i] / 17);
    if (alpha === previous && count < 16) { count++; continue; }
    if (count) out.push((previous << 4) | (count - 1));
    previous = alpha; count = 1;
  }
  if (count) out.push((previous << 4) | (count - 1));
  return Uint8Array.from(out);
}

export function parseBubbleMarkdown(text) {
  const runs = [];
  const add = (value, bold = false) => {
    if (!value) return;
    if (runs.at(-1)?.bold === bold) runs.at(-1).text += value;
    else runs.push({ text: value, bold });
  };
  const inline = (value, bold = false, depth = 0) => {
    if (depth > 4) { add(value, bold); return; }
    for (let i = 0; i < value.length;) {
      if (value[i] === "\\" && /[\\`*_[\]{}()#+.!>~|\-]/.test(value[i + 1] || "")) {
        add(value[i + 1], bold); i += 2; continue;
      }
      if (value[i] === "`") {
        const ticks = value.slice(i).match(/^`+/)[0], end = value.indexOf(ticks, i + ticks.length);
        if (end >= 0) { add(value.slice(i + ticks.length, end), bold); i = end + ticks.length; continue; }
      }
      const link = value.slice(i).match(/^!?\[([^\]\n]*)\]\(/);
      if (link) {
        let end = i + link[0].length, nesting = 1;
        while (end < value.length && nesting) {
          if (value[end] === "\\") { end += 2; continue; }
          if (value[end] === "(") nesting++;
          if (value[end] === ")") nesting--;
          end++;
        }
        if (!nesting) { inline(link[1] || "图片", bold, depth + 1); i = end; continue; }
      }
      const autolink = value.slice(i).match(/^<((?:https?:\/\/|mailto:)[^>\s]+)>/);
      if (autolink) { add(autolink[1].replace(/^mailto:/, ""), bold); i += autolink[0].length; continue; }
      const marker = ["***", "___", "**", "__", "~~", "*", "_"].find(mark => value.startsWith(mark, i)
        && (mark[0] !== "_" || !/[_\p{L}\p{N}]/u.test(value[i - 1] || "")));
      if (marker && value[i + marker.length] && !/\s/.test(value[i + marker.length])) {
        const end = value.indexOf(marker, i + marker.length);
        if (end > i + marker.length) {
          inline(value.slice(i + marker.length, end), bold || marker.length >= 2 && marker !== "~~", depth + 1);
          i = end + marker.length; continue;
        }
        // Streaming replies often stop between an opening ** and its closing **.
        if (marker.length >= 2 && marker !== "~~") { inline(value.slice(i + marker.length), true, depth + 1); break; }
      }
      const char = String.fromCodePoint(value.codePointAt(i)); add(char, bold); i += char.length;
    }
  };
  let fence = "", emitted = false;
  for (let line of text.replace(/\r\n?/g, "\n").split("\n")) {
    const boundary = line.match(/^\s*(`{3,}|~{3,})/);
    if (boundary) {
      if (!fence) { fence = boundary[1][0]; continue; }
      if (fence === boundary[1][0]) { fence = ""; continue; }
    }
    if (!line.trim()) continue;
    if (!fence && (/^\s*(?:[-*_]\s*){3,}$/.test(line)
        || /^\s*\|?\s*:?-{3,}:?\s*(?:\|\s*:?-{3,}:?\s*)+\|?\s*$/.test(line))) continue;
    if (emitted) add("\n");
    emitted = true;
    if (fence) { add(line); continue; }
    line = line.replace(/^\s*(?:>\s*)+/, "");
    const heading = line.match(/^\s{0,3}#{1,6}\s+(.*)$/);
    if (heading) { inline(heading[1].replace(/\s+#+\s*$/, ""), true); continue; }
    line = line.replace(/^\s*[-+*]\s+\[([ xX])\]\s*/, (_, checked) => checked === " " ? "[ ] " : "[x] ")
      .replace(/^\s*[-+*]\s+/, "• ");
    if (/^\s*\|.*\|\s*$/.test(line)) line = line.trim().slice(1, -1).split("|").map(cell => cell.trim()).join(" · ");
    inline(line);
  }
  return runs;
}

export function wrapBubbleRuns(runs, measure, width, maxLines) {
  const lines = [];
  let line = [], truncated = false;
  const characters = runs.flatMap(run => Array.from(run.text, char => ({ char, bold: run.bold })));
  const tokens = [];
  for (const item of characters) {
    const word = /[A-Za-z0-9_'./:@+-]/.test(item.char);
    if (word && tokens.at(-1)?.word) tokens.at(-1).items.push(item);
    else tokens.push({ word, items: [item] });
  }
  const grouped = items => {
    const result = [];
    for (const item of items) {
      if (result.at(-1)?.bold === item.bold) result.at(-1).text += item.char;
      else result.push({ text: item.char, bold: item.bold });
    }
    return result;
  };
  const measured = items => grouped(items).reduce((sum, run) => sum + measure(run.text, run.bold), 0);
  outer: for (const token of tokens) {
    if (token.items.length > 1 && measured(token.items) <= width && line.length
        && measured([...line, ...token.items]) > width) {
      lines.push(line); line = [];
      if (lines.length === maxLines) { truncated = true; break; }
    }
    for (const item of token.items) {
      if (item.char === "\n" || (line.length && measured([...line, item]) > width)) {
        lines.push(line); line = [];
        if (lines.length === maxLines) { truncated = true; break outer; }
        if (item.char === "\n") continue;
      }
      line.push(item);
    }
  }
  if (line.length && lines.length < maxLines) lines.push(line);
  if (truncated) {
    const last = lines.at(-1), dots = Array.from("...", char => ({ char, bold: false }));
    while (last.length && measured([...last, ...dots]) > width) last.pop();
    last.push(...dots);
  }
  return lines.length ? lines.map(grouped) : [[]];
}

export function wrapBubbleText(text, measure, width, maxLines) {
  return wrapBubbleRuns([{ text: text.replace(/\r\n?/g, "\n"), bold: false }], measure, width, maxLines)
    .map(line => line.map(run => run.text).join(""));
}

export function renderBubblePixels(style, text, createCanvas = () => document.createElement("canvas")) {
  const spec = BUBBLE_STYLES[style];
  if (!spec || !text || encoder.encode(text).length > 511) throw Error("Invalid bubble text");
  const canvas = createCanvas(); canvas.width = spec.width; canvas.height = spec.height;
  const context = canvas.getContext("2d", { willReadFrequently: true });
  if (!context) throw Error("Canvas text rendering is unavailable");
  const font = bold => `${bold ? 700 : 500} ${spec.px}px ${FONT_FAMILY}`;
  const measure = (value, bold) => { context.font = font(bold); return context.measureText(value).width; };
  context.textBaseline = "alphabetic";
  const lines = wrapBubbleRuns(parseBubbleMarkdown(text), measure,
    spec.width, spec.lines * spec.maxPages);
  const chunks = [], pageSizes = [];
  for (let first = 0; first < lines.length; first += spec.lines) {
    context.clearRect(0, 0, spec.width, spec.height);
    context.fillStyle = "white";
    for (let row = 0; row < spec.lines && first + row < lines.length; row++) {
      const line = lines[first + row];
      const metrics = line.map(run => { context.font = font(run.bold); return context.measureText(run.text); });
      const ascent = Math.max(spec.px * 0.85, ...metrics.map(metric => metric.actualBoundingBoxAscent || 0));
      const descent = Math.max(0, ...metrics.map(metric => metric.actualBoundingBoxDescent || 0));
      const baseline = Math.round(row * spec.lineHeight + (spec.lineHeight + ascent - descent) / 2);
      let x = 0;
      for (const run of line) {
        context.font = font(run.bold); context.fillText(run.text, x, baseline);
        x += context.measureText(run.text).width;
      }
    }
    const data = encodeBubbleAlpha(context.getImageData(0, 0, spec.width, spec.height).data);
    chunks.push(data); pageSizes.push(data.length);
  }
  const total = pageSizes.reduce((sum, size) => sum + size, 0);
  if (total > 128 * 1024) throw Error("Bubble image exceeds device limit");
  const bytes = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.length; }
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return { id: bubblePixelKey(style, text), style, pageSizes,
    checksum: bubbleChecksum(bytes), data: btoa(binary) };
}

export function collectBubbleTexts(topic, payload) {
  if (!payload || typeof payload !== "object" || Array.isArray(payload)) return [];
  const requests = new Map();
  const add = (style, value, limit) => {
    const text = clampBubbleText(value, limit);
    if (text) requests.set(bubblePixelKey(style, text), { style, text });
  };
  const pair = (title, body, titleLimit = 127, bodyLimit = 511, bodyStyle = 4) => {
    add(1, title, titleLimit); add(bodyStyle, body, bodyLimit);
  };
  const first = (...keys) => keys.map(key => payload?.[key]).find(value => typeof value === "string" && value);
  add(3, "休息中", 511);
  if (topic === "session/current") {
    // Selected cards go last so the newest pending work gets rendered first.
    const sessions = (Array.isArray(payload.sessions) ? payload.sessions : [])
      .filter(session => session && typeof session === "object" && !Array.isArray(session)).slice(0, 8);
    sessions.sort((a, b) => Number(a.id === payload.sessionId) - Number(b.id === payload.sessionId));
    for (const session of sessions) pair(session.title, session.content || session.state, 191, 383);
  } else if (topic === "ui/conversation") {
    add(1, payload.name || "宠物", 63);
    for (const hint of ["我在听", "想一想", "准备回答", "准备中"]) add(2, hint, 511);
    pair(payload.role === "user" ? "你" : payload.name || "宠物", payload.text, 63, 511, 2);
  } else if (topic === "audio/play_subtitle") {
    add(2, payload.text, 511);
  } else if (topic === "speech/text" || topic.startsWith("state/")) {
    const body = first("displayContent", "text", "message", "speech", "summary", "statusText");
    pair(first("displayTitle", "sessionName", "title", "agent") || "pet", body);
    if (payload.sessionId) add(4, body, 383);
    add(3, body, 511);
  }
  return [...requests.values()];
}

export async function startBubblePixelService({ listen, invoke, render = renderBubblePixels,
  interval = 180, now = () => Date.now() }) {
  let stopped = false, busy = false, connection = "", timer = null;
  const pending = new Map(), sent = new Map(), versions = new Map();
  const schedule = () => { if (!stopped && !timer && pending.size) timer = setTimeout(pump, interval); };
  async function pump() {
    timer = null;
    if (stopped || busy || !pending.size) return;
    busy = true;
    const [key, job] = [...pending.entries()].at(-1);
    pending.delete(key);
    try {
      const image = job.image || (job.image = render(job.style, job.text));
      if (!stopped && job.connection === connection) {
        await invoke("usb_send_bubble_pixels", { frame: { ...image,
          boardDeviceId: job.boardDeviceId, connectionId: job.connectionId } });
        if (job.connection === connection) sent.set(key, now());
        while (sent.size > 64) sent.delete(sent.keys().next().value);
      }
    } catch (error) {
      // The original text remains usable during asset/audio traffic or reconnects.
      const message = String(error).toLowerCase();
      const retry = message.includes("busy") || ((message.includes("timed out")
        || message.includes("rejected")) && (job.attempts || 0) < 2);
      if (!pending.has(key) && job.connection === connection && !stopped
          && retry && versions.get(job.topic) === job.version) {
        job.attempts = (job.attempts || 0) + 1;
        pending.set(key, job);
      }
    } finally { busy = false; schedule(); }
  }
  const unlisten = await listen("usb-bubble-render", ({ payload: event }) => {
    const nextConnection = `${event.boardDeviceId}:${event.connectionId}`;
    if (nextConnection !== connection) { connection = nextConnection; pending.clear(); sent.clear(); versions.clear(); }
    const version = (versions.get(event.topic) || 0) + 1;
    versions.set(event.topic, version);
    for (const [key, job] of pending) if (job.topic === event.topic) pending.delete(key);
    for (const request of collectBubbleTexts(event.topic, event.payload)) {
      const key = bubblePixelKey(request.style, request.text);
      if (sent.has(key) && now() - sent.get(key) < 15000) continue;
      pending.delete(key);
      pending.set(key, { ...request, ...event, connection, version });
    }
    while (pending.size > 48) pending.delete(pending.keys().next().value);
    schedule();
  });
  try { await invoke("usb_bubble_pixels_ready", { ready: true }); }
  catch (error) { stopped = true; clearTimeout(timer); unlisten(); throw error; }
  return () => {
    stopped = true; clearTimeout(timer); pending.clear(); unlisten();
    void invoke("usb_bubble_pixels_ready", { ready: false }).catch(() => {});
  };
}
