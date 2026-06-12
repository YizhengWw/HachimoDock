/**
 * [Input] 设备端 fb_speech_overlay.c 的 STATS_DASHBOARD_V1 版式与 br_stats_dashboard_model 字节上限。
 * [Output] .clawpkg 包结构常量、COMPONENT_DASHBOARD_V1 槽位 schema、清单校验函数。
 * [Pos] lib node in ref/src
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

export const CLAWPKG_FILES = [
  { name: "component.json", role: "组件元数据:id、name、version、author、capabilities、入口" },
  { name: "negative-screen.json", role: "负一屏:COMPONENT_DASHBOARD_V1 槽位映射" },
  { name: "buttons.json", role: "默认硬件绑定表" },
  { name: "runtime/", role: "声明式运行逻辑(首版只引用受控能力)" },
  { name: "assets/", role: "图标/声音等静态素材" },
  { name: "share.json", role: "社区分享卡片元数据" },
];

// 槽位上限来自设备端 br_stats_dashboard_model 结构(UTF-8 字节;留 4 字节安全余量)。
export const COMPONENT_DASHBOARD_V1_SLOTS = [
  { id: "title", maxBytes: 60, role: "左上角无外框标题" },
  { id: "eyebrow", maxBytes: 90, role: "标题下小号说明" },
  { id: "headline", maxBytes: 156, role: "右上角状态句或正文高亮句" },
  { id: "metricLabel", maxBytes: 90, role: "指标面板标题" },
  { id: "metricValue", maxBytes: 60, role: "指标大号数值" },
  { id: "metricUnit", maxBytes: 30, role: "数值单位" },
  { id: "badge", maxBytes: 12, role: "右上角绿色圆内数字" },
  { id: "note", maxBytes: 156, role: "指标面板内小号说明行" },
  { id: "footer", maxBytes: 156, role: "底部硬件操作提示行" },
  { id: "progress", maxBytes: 64, role: "进度条 '<0-100>:<label>' 格式,可选" },
];

const SLOT_BY_ID = new Map(COMPONENT_DASHBOARD_V1_SLOTS.map((s) => [s.id, s]));
const utf8Bytes = (text) => new TextEncoder().encode(String(text ?? "")).length;

export function validateClawpkgManifest(manifest) {
  const errors = [];
  for (const file of CLAWPKG_FILES) {
    if (!(file.name in (manifest || {}))) errors.push(`缺少 ${file.name}`);
  }
  const meta = manifest?.["component.json"];
  if ("component.json" in (manifest || {}) && !(meta?.id && meta?.name && meta?.version)) {
    errors.push("component.json 必须含 id、name、version");
  }
  const dashboard = manifest?.["negative-screen.json"]?.dashboard;
  if (dashboard) {
    for (const [slot, value] of Object.entries(dashboard)) {
      const def = SLOT_BY_ID.get(slot);
      if (!def) {
        errors.push(`negative-screen.json 含未知槽位 ${slot}`);
      } else if (utf8Bytes(value) > def.maxBytes) {
        errors.push(`槽位 ${slot} 超出 ${def.maxBytes} 字节上限`);
      }
    }
  }
  return { valid: errors.length === 0, errors };
}
