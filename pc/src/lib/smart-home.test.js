import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { visibleDevices, stepLabel, parsePropertyValue, HOME_STATUSES } from "./smart-home.js";
test("home guidance explains realtime voice, examples and spoken confirmation", () => {
  const page = readFileSync(new URL("../SmartHome.jsx", import.meta.url), "utf8");
  for (const text of ["切换到「实时对话」", "播放一首爵士乐", "换一首歌", "5 步以内", "无需在本页点击"]) assert.ok(page.includes(text));
  assert.ok(!page.includes("轻量单步操作直接执行，编排任务确认后执行"));
});
test("home filtering uses home ID, not ambiguous names", () => {
  const devices = [{ did: "1", homeId: "a", name: "音箱", room: "客厅", model: "speaker" }, { did: "2", homeId: "b", name: "音箱", room: "卧室", model: "speaker" }];
  assert.deepEqual(visibleDevices(devices, "a", "", "音箱").map(d => d.did), ["1"]);
  assert.equal(visibleDevices(devices, "", "卧室", "speaker").length, 1);
});
test("value parsing preserves false and rejects blank/non-numeric numbers", () => {
  assert.equal(parsePropertyValue({ format: "bool" }, "false"), false);
  assert.equal(parsePropertyValue({ format: "uint8" }, "20"), 20);
  for (const v of ["", "nan", "Infinity"]) assert.throws(() => parsePropertyValue({ format: "uint8" }, v));
});
test("receipt language distinguishes acceptance from verified state", () => {
  assert.match(HOME_STATUSES.accepted, /待确认/);
  assert.match(HOME_STATUSES.verified, /核验/);
  assert.equal(stepLabel({ kind: "wait", seconds: 2 }), "等待 2 秒");
  assert.match(stepLabel({ kind: "set", did: "1", siid: 2, piid: 1, value: false }, [{ did: "1", name: "音箱" }]), /音箱.*false/);
});
