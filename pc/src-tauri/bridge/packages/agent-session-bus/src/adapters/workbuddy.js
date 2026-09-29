"use strict";
const fs = require("node:fs");
const { BaseAdapter } = require("./base");
const { workbuddyPaths } = require("../../../../hooks/workbuddy-paths");
class WorkBuddyAdapter extends BaseAdapter {
  constructor(options = {}) { super({ agentId: "workbuddy", log: options.log }); this.paths = workbuddyPaths(options); }
  async isAvailable() {
    const installed = this.paths.apps.some(p => fs.existsSync(p)) || fs.existsSync(this.paths.root);
    return { ready: installed, reason: installed ? "支持状态与气泡跟随；请打开 WorkBuddy 目标对话使用按键语音输入" : "未检测到 WorkBuddy" };
  }
  // Live sessions are provided by the existing session-status tracker. Do not
  // scrape authentication stores or assume private desktop database schemas.
  async listSessions() { return []; }
  async openNew() { throw new Error("请在 WorkBuddy 中新建会话"); }
  async *inject() {
    yield { kind: "error", code: "AGENT_INPUT_UNSUPPORTED", message: "WorkBuddy 语音输入使用桌面客户端的当前可见输入框，不支持后台会话注入；不会转发到其他 Agent。" };
  }
}
module.exports = { WorkBuddyAdapter };
