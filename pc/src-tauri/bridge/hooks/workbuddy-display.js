"use strict";
const fs = require("node:fs");
const path = require("node:path");
const { workbuddyPaths } = require("./workbuddy-paths");
const MAX_TAIL_BYTES = 256 * 1024;

function displayText(value, limit = 320) {
  if (typeof value !== "string") return "";
  // Display only bounded prose, not secrets, file paths, hidden reasoning or
  // executable/tool blocks. This is also applied at the Bridge ingress.
  const text = value.slice(0, 24000)
    .replace(/<(think|thinking|analysis)>[\s\S]*?(?:<\/\1>|$)/gi, "")
    .replace(/```[\s\S]*?(?:```|$)/g, "[代码省略]")
    .replace(/-----BEGIN [^-]*PRIVATE KEY-----[\s\S]*?(?:-----END [^-]*PRIVATE KEY-----|$)/g, "[敏感信息已隐藏]")
    .replace(/\b(?:sk-|ark-)[a-zA-Z0-9_-]{12,}/g, "[密钥已隐藏]")
    .replace(/\b(Bearer)\s+\S+/gi, "$1 [已隐藏]")
    .replace(/\b(api[_-]?key|access[_-]?token|refresh[_-]?token|password|secret)\b["']?\s*[:=]\s*["']?[^\s,"'}]+/gi, "$1=[已隐藏]")
    .replace(/(?:\/Users\/|\/home\/|[A-Z]:\\Users\\)[^\s"'<>，。；]+/gi, "[本地路径]")
    .replace(/[\u0000-\u001f\u007f]/g, " ")
    .replace(/\s+/g, " ").trim();
  const chars = Array.from(text);
  return chars.length > limit ? chars.slice(0, limit - 1).join("") + "…" : text;
}

function readCurrentReply(payload, options = {}) {
  // Only the transcript explicitly named by this hook, under WorkBuddy's own
  // projects directory, matching this session. No database/history scan.
  const id = payload.session_id;
  if (!/^[a-zA-Z0-9_-]{1,128}$/.test(id || "") || typeof payload.transcript_path !== "string") return "";
  let fd;
  try {
    const root = fs.realpathSync(path.join(workbuddyPaths(options).root, "projects"));
    const file = fs.realpathSync(payload.transcript_path);
    const rel = path.relative(root, file);
    if (!rel || rel.startsWith(".." + path.sep) || path.isAbsolute(rel)
        || path.basename(file) !== `${id}.jsonl`) return "";
    fd = fs.openSync(file, fs.constants.O_RDONLY | (fs.constants.O_NOFOLLOW || 0) | (fs.constants.O_NONBLOCK || 0));
    const stat = fs.fstatSync(fd);
    if (!stat.isFile()) return "";
    const start = Math.max(0, stat.size - MAX_TAIL_BYTES);
    const bytes = Buffer.alloc(Math.min(stat.size, MAX_TAIL_BYTES));
    const read = fs.readSync(fd, bytes, 0, bytes.length, start);
    const lines = bytes.subarray(0, read).toString("utf8").split("\n");
    if (start > 0) lines.shift();
    for (let i = lines.length - 1; i >= 0; i--) {
      let row; try { row = JSON.parse(lines[i]); } catch { continue; }
      if (row.sessionId !== id || row.type !== "message") continue;
      // Do not recycle an older turn's answer when this turn has none.
      if (row.role === "user") return "";
      if (row.role !== "assistant" || row.status !== "completed") continue;
      if (payload.generation_id && row.providerData?.conversationRequestId !== payload.generation_id) continue;
      if (!Array.isArray(row.content)) continue;
      const text = row.content.filter(c => c?.type === "output_text" && typeof c.text === "string")
        .map(c => c.text).join("\n");
      return displayText(text);
    }
  } catch { /* Missing, rotating or unsupported transcript: retain status. */ }
  finally { if (fd !== undefined) fs.closeSync(fd); }
  return "";
}

function hookDisplay(payload, options) {
  const event = payload.hook_event_name;
  if (event === "UserPromptSubmit") return displayText(payload.prompt);
  if (event === "Stop") return displayText(payload.last_assistant_message) || readCurrentReply(payload, options);
  if (event === "Notification" || event === "PermissionRequest") return displayText(payload.message);
  return "";
}
module.exports = { displayText, hookDisplay, readCurrentReply };
