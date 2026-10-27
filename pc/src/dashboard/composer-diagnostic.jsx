import React from "react";
import { createRoot } from "react-dom/client";
import VoiceComposerCheck from "./VoiceComposerCheck.jsx";
import "../styles.css";

function Diagnostic() {
  return <main style={{maxWidth:920,margin:"24px auto",padding:24}}>
    <h1>输入框定位 · 同环境对照</h1>
    <p>仅供排查，不是修复版客户端。不运行尚未证实的候选修复，使用 0.1.146 定位规则，只补充脱敏结构证据。</p>
    <ol>
      <li>保留出错时的 Codex 对话，不用新建会话，也不要改动已有草稿。</li>
      <li>保持无输入框焦点的出错状态，不用先点击 Codex 输入框，直接在这里点击一次检查。</li>
      <li>复制下方结果文本用于排查两个可见候选。不写入、不发送，不要求重新录音。</li>
    </ol>
    <p>无 USB 自动连接、录音、语音识别、模型调用或配置迁移。不采集聊天正文、草稿或密钥。</p>
    <VoiceComposerCheck agentId="codex" />
  </main>;
}
createRoot(document.getElementById("root")).render(<Diagnostic />);
