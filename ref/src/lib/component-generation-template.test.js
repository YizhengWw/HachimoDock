import test from "node:test";
import assert from "node:assert/strict";
import {
  buildComponentGenerationPrompt,
  createAgentPrompt,
  createSkillTriggerPrompt,
  labelForComponentGenerationAgent,
} from "./component-generation-template.js";

test("prompt 内嵌 .clawpkg 结构、COMPONENT_DASHBOARD_V1 槽位与字节上限", () => {
  const prompt = buildComponentGenerationPrompt({ description: "做一个会议计时", agentId: "codex" });
  assert.match(prompt, /COMPONENT_DASHBOARD_V1/);
  assert.match(prompt, /component\.json/);
  assert.match(prompt, /negative-screen\.json/);
  assert.match(prompt, /headline/);
  assert.match(prompt, /156/);          // 槽位字节上限被写进 prompt
  assert.match(prompt, /做一个会议计时/); // 用户描述被嵌入
});

test("prompt 含 worked example(token 消耗)与缺信息追问要求", () => {
  const prompt = buildComponentGenerationPrompt({ description: "x", agentId: "codex" });
  assert.match(prompt, /token|Token/);
  assert.match(prompt, /信息不足|追问|向用户澄清/);
});

test("prompt 和设备端一致: widget 只可配置屏幕，红钮不可用，旋钮固定音量", () => {
  const prompt = buildComponentGenerationPrompt({ description: "x", agentId: "codex" });
  assert.match(prompt, /切到负一屏就是进入这个组件场景/);
  assert.match(prompt, /默认状态必须自己成立/);
  assert.match(prompt, /安装前可把每个 action 绑定到屏幕点击或屏幕长按/);
  assert.match(prompt, /红钮或旋钮/);
  assert.match(prompt, /旋钮旋转固定用于系统音量/);
  assert.match(prompt, /buttons\.json\s+— 屏幕按钮功能绑定表/);
  assert.doesNotMatch(prompt, /button\.primary\.short_press/);
  assert.doesNotMatch(prompt, /button\.primary\.long_press/);
  assert.doesNotMatch(prompt, /knob\.rotate_cw \/ knob\.rotate_ccw/);
});

test("createAgentPrompt 对空描述返回澄清式 prompt", () => {
  const prompt = createAgentPrompt("");
  assert.match(prompt, /请先描述/);
  assert.match(prompt, /点击屏幕开始\/暂停/);
  assert.doesNotMatch(prompt, /屏幕前红色编码旋钮调时长/);
});

test("labelForComponentGenerationAgent 映射 agent 显示名", () => {
  assert.equal(labelForComponentGenerationAgent("codex"), "Codex");
  assert.equal(labelForComponentGenerationAgent("claude-code"), "Claude Code");
});

test("createAgentPrompt 对非空描述返回含描述的完整生成 prompt", () => {
  const prompt = createAgentPrompt("做一个会议计时");
  assert.equal(typeof prompt, "string");
  assert.match(prompt, /做一个会议计时/);       // 描述被嵌入
  assert.match(prompt, /COMPONENT_DASHBOARD_V1/); // 走了完整生成分支
  assert.doesNotMatch(prompt, /请先描述/);       // 没走空描述分支
});

test("skill trigger prompt 要求按钮配置不清时先追问", () => {
  const prompt = createSkillTriggerPrompt("做一个会议计时");
  assert.match(prompt, /切到负一屏后的默认场景/);
  assert.match(prompt, /不能依赖红钮或旋钮/);
  assert.match(prompt, /screen\.region\.tap/);
  assert.match(prompt, /screen\.region\.long_press/);
  assert.match(prompt, /红钮不可用/);
  assert.doesNotMatch(prompt, /button\.primary\.short_press/);
  assert.match(prompt, /旋钮固定用于系统音量/);
  assert.doesNotMatch(prompt, /knob\.rotate_cw/);
  assert.match(prompt, /点击.*长按.*分别/);
  assert.match(prompt, /按钮配置.*不清楚.*先追问/);
});
