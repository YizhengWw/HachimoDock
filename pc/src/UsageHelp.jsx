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
        <p>先回到设备的宠物界面，再开始聊天。使用支持双向语音的固件时，可在回复期间直接说话打断，也可以说「停」；旧固件请等它说完再说。60 秒无人说话时自动结束。</p>
        <p>普通聊天无需打开 Agent；设备显示双方字幕，结束后恢复 Agent 跟随状态。</p>
        <p>需要在电脑上做事时，直接告诉宠物，例如「把桌面的会议记录整理成文档」。宠物会判断是否调用桌面 Agent。目前支持 ChatGPT（Codex）和 WorkBuddy，默认直接使用设备当前跟随的 Agent，用户只说任务即可，无需点名；也支持明确说「让 Work 帮我……」（Work 即 WorkBuddy）。尚未支持的跟随目标会提示限制，不自动换成 Codex。请在目标客户端打开对话并保留一个主窗口，无需绑定具体会话或再按确认键；已有草稿不会覆盖。执行中可按客户端提供的引导或排队入口追加任务。可追问「刚才的任务完成了吗」。</p>
        <p>聊天仍在进行时，会语音提示已确认的桌面任务结果或等待授权。WorkBuddy 需要启用对应 Agent 通道及状态 Hook；旧版未提供完整轮次回执时只能确认收到，不能自动确认完成。打断宠物说话不等于取消桌面任务；已发送任务的授权和取消请在原客户端处理。</p>
        <p>派发时会把本次语音对话上下文同步到目标 Agent 聊天中，用于理解后续补充与确认。宠物说“已派发”应以实际任务回执为准；完成通知后的“好的”不会作为重新执行的要求。</p>
      </>}
      {onOpenApiSettings && <button type="button" className="btn-ghost btn-sm" onClick={onOpenApiSettings}>去配置</button>}
    </details>
  </div>;
}
