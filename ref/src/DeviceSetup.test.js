/**
 * [Input] DeviceSetup.jsx onboarding source.
 * [Output] Static Node coverage for the two-method binding entry, Ethernet mock path, dev skip-provisioning path, connection-mode-aware retry, final single-channel agent-appearance confirmation, and primary completed-state return action.
 * [Pos] test node in ref/src
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const srcDir = dirname(fileURLToPath(import.meta.url));

function readSource(fileName) {
  return readFileSync(join(srcDir, fileName), "utf8");
}

test("idle setup presents Ethernet before Wi-Fi without tabs", () => {
  const source = readSource("DeviceSetup.jsx");

  const ethernetIndex = source.indexOf("方式一：插网线绑定");
  const wifiIndex = source.indexOf("方式二：Wi‑Fi 配网");

  assert.notEqual(ethernetIndex, -1, "Expected an Ethernet binding method card");
  assert.notEqual(wifiIndex, -1, "Expected a Wi-Fi binding method card");
  assert.ok(ethernetIndex < wifiIndex, "Ethernet method should appear above the Wi-Fi method");
  assert.doesNotMatch(source, /role="tab"|className="[^"]*tab/);
});

test("Ethernet method uses a mock detection and binding path", () => {
  const source = readSource("DeviceSetup.jsx");

  assert.match(source, /EthernetPort/);
  assert.match(source, /const ETHERNET_BINDING_LABEL = "USB直连";/);
  assert.match(source, /const startEthernetBinding = useCallback\(async \(\) =>/);
  assert.match(source, /phase: "ethernet_detecting"/);
  assert.match(source, /phase: "ethernet_binding"/);
  assert.match(source, /onClick=\{startEthernetBinding\}/);
});

test("dev setup can skip provisioning without running USB or Wi-Fi config", () => {
  const source = readSource("DeviceSetup.jsx");

  assert.match(source, /const canSkipProvisioning = import\.meta\.env\.DEV;/);
  assert.match(source, /const SKIP_PROVISIONING_DEVICE = \{/);
  assert.match(source, /boardDeviceId: "board-skip-setup-001"/);
  assert.match(source, /const skipProvisioning = useCallback\(async \(\) =>/);
  assert.match(source, /if \(!canSkipProvisioning\) return;/);
  assert.match(source, /wifiSsid: SKIP_PROVISIONING_SSID/);
  assert.match(source, /connectionMode: "skip"/);
  assert.match(source, /onClick=\{skipProvisioning\}/);
  assert.match(source, /跳过配网/);
}
);

test("retry keeps the failed binding method instead of always restarting Wi-Fi", () => {
  const source = readSource("DeviceSetup.jsx");

  assert.match(source, /const retryConnectionMode = state\.connectionMode;/);
  assert.match(source, /retryConnectionMode === "ethernet" \? startEthernetBinding : startSetup/);
  assert.match(source, /const errorEyebrow = isEthernetFlow \? "插线绑定中断" : "配网中断";/);
  assert.match(source, /const errorTitle = state\.message \|\| \(isEthernetFlow \? "插线绑定失败" : "配网失败"\);/);
  assert.match(source, /eyebrow=\{errorEyebrow\}/);
  assert.match(source, /title=\{errorTitle\}/);
});

test("setup shell presents two product methods without a legacy mock skip entry", () => {
  const source = readSource("App.jsx");

  assert.match(source, /插网线或 Wi‑Fi 绑定/);
  assert.match(source, /DEV_DIRECT_DASHBOARD_BINDING/);
  assert.doesNotMatch(source, /跳过配网 \(mock\)/);
  assert.doesNotMatch(source, /save_device_binding",\s*\{\s*binding:\s*\{\s*boardDeviceId: `mock-/s);
});

test("setup copy stays concise and avoids instructional paragraphs", () => {
  const source = readSource("DeviceSetup.jsx");

  assert.match(source, /title="插网线绑定"/);
  assert.match(source, /title="Wi‑Fi 绑定"/);
  assert.doesNotMatch(source, /如果桌宠已经接入有线网络/);
  assert.doesNotMatch(source, /当前主要流程/);
  assert.doesNotMatch(source, /不需要你手动切换系统 Wi/);
  assert.doesNotMatch(source, /密码仅下发到桌宠用于联网/);
  assert.doesNotMatch(source, /请保持网线连接到桌宠，应用会把必要/);
  assert.doesNotMatch(source, /绑定完成后会进入验证结果/);
});

test("setup requires choosing one device display channel before entering dashboard", () => {
  const source = readSource("DeviceSetup.jsx");

  assert.match(source, /choose_agent_appearance/);
  assert.match(source, /confirm_appearance/);
  assert.match(source, /detect_local_agents/);
  assert.match(source, /listAppearances/);
  assert.match(source, /pickFirstDetectedAgentId/);
  assert.match(source, /assignAppearanceToAgent\(\{\}, defaultAgentId, defaultAppearanceId\)/);
  assert.match(source, /saveAgentAppearanceMap/);
  assert.match(source, /saveEnabledAgents/);
  assert.match(source, /syncSetupAgentAppearanceMapToBridge/);
  assert.match(source, /选择设备展示渠道/);
  assert.match(source, /设备端当前只能展示一个 agent 渠道/);
  assert.match(source, /默认形象/);
  assert.match(source, /未检测到可用 CLI agent/);
  assert.match(source, /完成绑定/);
  // The set_result → choose_agent_appearance and set_completed → completed
  // transitions now live in device-setup-state.js and are covered behaviorally
  // in device-setup-state.test.js.
  assert.match(source, /setup-agent-channel-list/);
  assert.match(source, /setup-agent-channel-card/);
  assert.match(source, /role="radiogroup"/);
  assert.match(source, /role="radio"/);
  assert.match(source, /setup-agent-default-panel/);
  assert.doesNotMatch(source, /每个已检测到的 agent 都可以单独选择形象/);
  assert.doesNotMatch(source, /setup-agent-appearance-list/);
  assert.doesNotMatch(source, /<option value="">未配置形象<\/option>/);
  assert.doesNotMatch(source, /setup-agent-active-channel/);
  assert.doesNotMatch(source, /state\.phase === "completed" && onComplete/);
});

test("completed setup puts return to dashboard on the primary right-side action", () => {
  const source = readSource("DeviceSetup.jsx");
  const completedStart = source.lastIndexOf('phase === "completed"');
  assert.notEqual(completedStart, -1, "Expected the completed setup view");
  const completedBlock = source.slice(completedStart);

  const testButtonIndex = completedBlock.indexOf('className="btn-ghost"');
  const returnButtonIndex = completedBlock.indexOf('className="btn-primary"');
  const returnTextIndex = completedBlock.indexOf("返回主界面");

  assert.ok(testButtonIndex !== -1, "Expected a secondary test-message action");
  assert.ok(returnButtonIndex !== -1, "Expected a primary return action");
  assert.ok(testButtonIndex < returnButtonIndex, "Return action should sit on the right side of the footer");
  assert.ok(returnButtonIndex < returnTextIndex, "Return action should be the primary orange button");
});

test("setup completion refreshes shared device context before dashboard render", () => {
  const source = readSource("App.jsx");

  assert.match(source, /useDeviceContext/);
  assert.match(source, /const\s+\{\s*refresh\s*\}\s*=\s*useDeviceContext\(\)/);
  assert.match(source, /const\s+handleSetupCompleteWithRefresh\s*=\s*useCallback\(\s*async\s*\(\)\s*=>/);
  assert.match(source, /await\s+refresh\(\)/);
  assert.match(source, /<DeviceSetup\s+onComplete=\{handleSetupCompleteWithRefresh\}/);
});
