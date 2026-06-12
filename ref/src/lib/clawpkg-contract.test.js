import test from "node:test";
import assert from "node:assert/strict";
import {
  CLAWPKG_FILES,
  COMPONENT_DASHBOARD_V1_SLOTS,
  validateClawpkgManifest,
} from "./clawpkg-contract.js";

test("clawpkg 六件套结构齐全", () => {
  assert.deepEqual(
    CLAWPKG_FILES.map((f) => f.name),
    ["component.json", "negative-screen.json", "buttons.json", "runtime/", "assets/", "share.json"],
  );
});

test("COMPONENT_DASHBOARD_V1 暴露 10 个槽位 (9 必填 + 1 可选 progress) 且带 maxBytes", () => {
  const ids = COMPONENT_DASHBOARD_V1_SLOTS.map((s) => s.id);
  assert.deepEqual(ids, ["title", "eyebrow", "headline", "metricLabel", "metricValue", "metricUnit", "badge", "note", "footer", "progress"]);
  assert.ok(COMPONENT_DASHBOARD_V1_SLOTS.every((s) => Number.isInteger(s.maxBytes) && s.maxBytes > 0));
});

test("validateClawpkgManifest 缺文件报错、超字节报错、合法通过", () => {
  const ok = validateClawpkgManifest({
    "component.json": { id: "x", name: "X", version: "1.0.0" },
    "negative-screen.json": { dashboard: { title: "X", headline: "你好" } },
    "buttons.json": [], "runtime/": {}, "assets/": {}, "share.json": { title: "X" },
  });
  assert.equal(ok.valid, true);

  const missing = validateClawpkgManifest({ "component.json": { id: "x", name: "X", version: "1.0.0" } });
  assert.equal(missing.valid, false);
  assert.match(missing.errors.join(" "), /negative-screen\.json/);

  const tooLong = validateClawpkgManifest({
    "component.json": { id: "x", name: "X", version: "1.0.0" },
    "negative-screen.json": { dashboard: { badge: "1234567890123" } },
    "buttons.json": [], "runtime/": {}, "assets/": {}, "share.json": { title: "X" },
  });
  assert.equal(tooLong.valid, false);
  assert.match(tooLong.errors.join(" "), /badge/);
});

test("validateClawpkgManifest component.json 为 null 时报字段缺失错误", () => {
  const result = validateClawpkgManifest({
    "component.json": null,
    "negative-screen.json": {},
    "buttons.json": [],
    "runtime/": {},
    "assets/": {},
    "share.json": {},
  });
  assert.equal(result.valid, false);
  assert.ok(result.errors.some((e) => e.includes("component.json 必须含 id、name、version")));
});
