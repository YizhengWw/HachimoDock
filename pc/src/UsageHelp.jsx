/** Short, shared voice instructions; key names follow the current device mapping. */
import React from "react";
import { useUsageHelp } from "./shell/DeviceContext.jsx";

export default function UsageHelp({ mode = "chat", help: override, onOpenApiSettings }) {
  const current = useUsageHelp();
  const help = override || current;
  return <div className="usage-help">
    {mode === "voice" ? <>
      <p>用设备麦克风给 Agent 输入文字，不会自动发送。没说完可以继续按住说，内容会接在后面。</p>
      <p>{help.voice} {help.confirm}</p>
    </> : <p>{help.chat} 首次使用，请先配置语音服务和对话大模型。</p>}
    {help.pending && <p className="usage-help__notice">按键配置有修改，以上提示在同步到设备后生效。</p>}
    <details>
      <summary>如何使用</summary>
      <p>请保持设备连接电脑，并让 Pet Manager 持续运行。</p>
      {mode === "voice" ? <>
        <p>Agent 已在前台时，输入到当前对话；未在前台时，会打开 Agent，并优先进入设备选中的会话。设备没有会话时，使用 Agent 当前对话。</p>
        <p>首次使用请配置语音识别。Mac 用户还需允许 Pet Manager 的「辅助功能」权限。</p>
      </> : <>
        <p>先回到设备的宠物界面，再开始聊天。等它说完再说，当前暂不支持说话打断；60 秒无人说话时自动结束。</p>
        <p>聊天时设备显示双方字幕；结束后恢复 Agent 跟随状态。无需打开 ChatGPT、Claude 等 Agent。</p>
      </>}
      {onOpenApiSettings && <button type="button" className="btn-ghost btn-sm" onClick={onOpenApiSettings}>去配置</button>}
    </details>
  </div>;
}
