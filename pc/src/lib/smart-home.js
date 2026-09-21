/** Display-only helpers. Authority and MIOT validation live in the native runtime. */
export const HOME_STATUSES = { verified: "已核验", accepted: "已受理，状态待确认", completed: "步骤已结束", stopped: "已停止", cancelled: "已取消", interrupted: "应用中断，请确认设备现状；不会自动重放", unconfirmed: "请求结果未确认，请先检查设备现状", running: "执行中" };
export function visibleDevices(devices, home, room, query) {
  const q = query.trim().toLocaleLowerCase();
  return devices.filter(d => (!home || String(d.homeId) === home) && (!room || d.room === room)
    && `${d.name} ${d.room} ${d.model}`.toLocaleLowerCase().includes(q));
}
export function stepLabel(step, devices = [], scenes = []) {
  const device = devices.find(d => d.did === step.did);
  const name = device ? `${device.name}${device.room ? `（${device.home || "我的家"} · ${device.room}）` : ""}` : step.did || "设备";
  if (step.kind === "wait") return `等待 ${step.seconds} 秒`;
  if (step.kind === "scene") return `运行场景「${scenes.find(s => s.id === step.sceneId)?.name || step.sceneId}」`;
  const capability = step.label || `${step.siid}.${step.piid ?? step.aiid}`;
  if (step.kind === "check") return `检查 ${name} · ${capability} ${({ eq: "等于", ne: "不等于", gt: "大于", ge: "大于等于", lt: "小于", le: "小于等于" })[step.comparison] || step.comparison} ${JSON.stringify(step.value)}，不满足则停止`;
  return step.kind === "set" ? `${name} · ${capability} → ${JSON.stringify(step.value)}` : `${name} · ${capability}（${(step.args || []).map(v => JSON.stringify(v)).join("，")}）`;
}
export function parsePropertyValue(property, value) {
  if (property.format === "bool") return value === "true";
  if (property.format === "string") return value;
  if (String(value).trim() === "" || !Number.isFinite(Number(value))) throw new Error("请填写有效数字");
  return Number(value);
}
