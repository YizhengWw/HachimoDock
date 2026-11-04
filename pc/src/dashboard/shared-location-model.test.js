import test from "node:test";
import assert from "node:assert/strict";
import {
  SHARED_LOCATION_POLL_MS, SHARED_LOCATION_SUGGEST_MS, buildLocationInput, createSharedLocationController,
  describeSharedLocation, draftFromSnapshot, parseLocationSnapshot, parseLocationSuggestions, validateLocationDraft,
} from "./shared-location-model.js";

// All snapshots below are synthetic; no native commands or real configuration are read.
const snapshot = (patch = {}) => ({
  mode: "auto", status: "unknown", source: null, city: "", region: "", country: "",
  lat: null, lon: null, updatedAt: null, expiresAt: null, error: "", ...patch,
});
const manual = (patch = {}) => snapshot({
  mode: "manual", status: "ready", source: "manual", city: "测试城", lat: 0, lon: 0, ...patch,
});
const draft = (patch = {}) => ({ mode: "manual", city: "测试城", lat: "0", lon: "0", ...patch });
const settle = () => new Promise((resolve) => setImmediate(resolve));

function harness() {
  const requests = [];
  const timers = new Map();
  const timeouts = new Map();
  let timerId = 0, now = 0, inFlight = 0, maximumInFlight = 0;
  const controller = createSharedLocationController({
    invoke(...args) {
      let resolve, reject, settled = false;
      const suggestion = args[0] === "shared_location_suggest";
      if (suggestion) maximumInFlight = Math.max(maximumInFlight, ++inFlight);
      const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
      const finish = (callback, value) => {
        if (settled) return;
        settled = true;
        if (suggestion) --inFlight;
        callback(value);
      };
      requests.push({ args, resolve: value => finish(resolve, value), reject: error => finish(reject, error) });
      return promise;
    },
    setInterval(callback, delay) { timers.set(++timerId, { callback, delay }); return timerId; },
    clearInterval(id) { timers.delete(id); },
    setTimeout(callback, delay) { timeouts.set(++timerId, { callback, at: now + delay }); return timerId; },
    clearTimeout(id) { timeouts.delete(id); },
  });
  return {
    controller, requests, timers, timeouts,
    calls(command) { return requests.filter(({ args }) => args[0] === command); },
    get inFlight() { return inFlight; },
    get maximumInFlight() { return maximumInFlight; },
    async tick() { timers.forEach(({ callback }) => callback()); await settle(); },
    async advance(ms) {
      const target = now + ms;
      for (;;) {
        const next = [...timeouts.entries()].filter(([, value]) => value.at <= target).sort((a, b) => a[1].at - b[1].at)[0];
        if (!next) break;
        const [id, value] = next;
        now = value.at;
        timeouts.delete(id);
        value.callback();
        await settle();
      }
      now = target;
      await settle();
    },
    async activate(value = snapshot()) {
      controller.setActive(true);
      requests.filter(({ args }) => args[0] === "shared_location_status").at(-1).resolve(value);
      await settle();
    },
  };
}

test("默认只有 auto 草稿，不把预设或 IP 快照填成手动位置", () => {
  assert.deepEqual(draftFromSnapshot(null), { mode: "auto", city: "", lat: "", lon: "" });
  assert.deepEqual(draftFromSnapshot(snapshot({ source: "ip", city: "测试城", lat: 1, lon: 2 })),
    { mode: "auto", city: "", lat: "", lon: "" });
  assert.deepEqual(draftFromSnapshot(manual()), draft());
  assert.equal(describeSharedLocation(null).available, false);
  assert.equal(describeSharedLocation(snapshot()).available, false);
  for (const city of ["样例区", "样例省样例市样例县", "样例县,样例市,样例省", "样例县，样例市，样例省"]) {
    assert.deepEqual(validateLocationDraft(draft({ city, lat: "", lon: "" })), {});
  }
});

test("手动地区必填，双坐标留空是 city-only，不隐式变成零", () => {
  assert.deepEqual(Object.keys(validateLocationDraft(draft({ city: " \n", lat: "", lon: " " }))), ["city"]);
  for (const [lat, lon] of [["", ""], [" ", "  "], [null, undefined]]) {
    const value = draft({ lat, lon });
    assert.deepEqual(validateLocationDraft(value), {});
    assert.deepEqual(buildLocationInput(value), { mode: "manual", manual: { city: "测试城" } });
    assert.equal(Object.hasOwn(buildLocationInput(value).manual, "lat"), false);
    assert.equal(Object.hasOwn(buildLocationInput(value).manual, "lon"), false);
  }
  assert.deepEqual(validateLocationDraft(draft()), {});
  assert.deepEqual(validateLocationDraft(draft({ lat: "-90", lon: "180" })), {});
  assert.deepEqual(validateLocationDraft(draft({ lat: "+90", lon: "-180" })), {});
  assert.deepEqual(validateLocationDraft(draft({ lat: ".25", lon: "-1.2e2" })), {});
});

test("拒绝越界、非有限值和非十进制输入", () => {
  for (const value of ["NaN", "Infinity", "-Infinity", "1e999", "0x10", "0b10", "0o10", "12abc"]) {
    assert.ok(validateLocationDraft(draft({ lat: value, lon: value })).lat, String(value));
    assert.ok(validateLocationDraft(draft({ lat: value, lon: value })).lon, String(value));
  }
  assert.ok(validateLocationDraft(draft({ lat: "90.0001" })).lat);
  assert.ok(validateLocationDraft(draft({ lon: "-180.0001" })).lon);
  assert.ok(validateLocationDraft(draft({ mode: "gps" })).mode);
  assert.throws(() => buildLocationInput(draft({ lat: "" })));
});

test("单边坐标不能补零或静默退回 city-only", () => {
  for (const empty of ["", " ", null, undefined]) {
    const missingLatitude = draft({ lat: empty, lon: "0" });
    const missingLongitude = draft({ lat: "0", lon: empty });
    assert.deepEqual(Object.keys(validateLocationDraft(missingLatitude)), ["lat"]);
    assert.deepEqual(Object.keys(validateLocationDraft(missingLongitude)), ["lon"]);
    assert.throws(() => buildLocationInput(missingLatitude));
    assert.throws(() => buildLocationInput(missingLongitude));
  }
});

test("地区仍按 UTF-8 60字节限制并拒绝控制字符和网址", () => {
  assert.deepEqual(validateLocationDraft(draft({ city: "汉".repeat(20), lat: "", lon: "" })), {});
  for (const city of ["汉".repeat(21), "https://example.test", "样例\n区", "样例\u0000区", "样例\u202e区"]) {
    assert.ok(validateLocationDraft(draft({ city, lat: "", lon: "" })).city);
  }
});

test("配置参数严格符合 input/manual 契约，非手动模式不携带草稿坐标", () => {
  assert.deepEqual(buildLocationInput(draft({ city: " 测试城 ", lat: "-12.5", lon: "0" })),
    { mode: "manual", manual: { city: "测试城", lat: -12.5, lon: 0 } });
  for (const mode of ["auto", "off"]) {
    assert.deepEqual(buildLocationInput(draft({ mode, lat: "bad" })), { mode, manual: null });
  }
});

test("仅展示有效、未过期且来源匹配的 ready 位置", () => {
  assert.equal(describeSharedLocation(manual()).available, true);
  for (const status of ["locating", "unknown", "off", "error"]) {
    assert.equal(describeSharedLocation(manual({ status })).available, false);
  }
  for (const patch of [{ mode: "off" }, { city: " " }, { lat: null }, { lon: NaN }, { source: "ip" }]) {
    assert.equal(describeSharedLocation(manual(patch)).available, false);
  }
  const now = 1_800_000_000_000;
  for (const expiresAt of [now / 1000, now, 0]) {
    assert.equal(describeSharedLocation(manual({ expiresAt }), now).available, false);
  }
  for (const expiresAt of [(now + 10_000) / 1000, now + 10_000]) {
    assert.equal(describeSharedLocation(manual({ expiresAt }), now).available, true);
  }
});

test("拒绝不符合契约的快照，不伪造可用状态", () => {
  assert.deepEqual(parseLocationSnapshot(manual()), manual());
  for (const patch of [{ mode: "gps" }, { status: "ok" }, { source: "gps" }, { lat: 91 }, { lon: "2" }, { updatedAt: NaN }, { expiresAt: -1 }]) {
    assert.throws(() => parseLocationSnapshot(snapshot(patch)));
  }
});

test("inactive 不请求、不写入、不创建计时器", async () => {
  const h = harness();
  await h.controller.refresh();
  await h.controller.save();
  h.controller.setActive(false);
  assert.equal(h.requests.length, 0);
  assert.equal(h.timers.size, 0);
});

test("active 首次只读无 args，10 秒轮询且慢查询不重叠", async () => {
  const h = harness();
  h.controller.setActive(true);
  h.controller.setActive(true);
  assert.deepEqual(h.requests[0].args, ["shared_location_status"]);
  assert.equal(h.timers.size, 1);
  assert.equal([...h.timers.values()][0].delay, 10_000);
  assert.equal(SHARED_LOCATION_POLL_MS, 10_000);
  await h.tick();
  assert.equal(h.requests.length, 1);
  h.requests[0].resolve(snapshot());
  await settle();
  await h.tick();
  assert.equal(h.requests.length, 2);
  assert.deepEqual(h.requests[1].args, ["shared_location_status"]);
  h.controller.setActive(false);
  assert.equal(h.timers.size, 0);
  h.requests[1].resolve(manual());
  await settle();
  assert.equal(h.controller.getSnapshot().snapshot.mode, "auto");
});

test("重新激活后的新状态不会被旧激活周期回复覆盖", async () => {
  const h = harness();
  h.controller.setActive(true);
  h.controller.setActive(false);
  h.controller.setActive(true);
  h.requests[1].resolve(manual());
  await settle();
  h.requests[0].resolve(snapshot());
  await settle();
  assert.equal(h.controller.getSnapshot().snapshot.mode, "manual");
  assert.equal(h.controller.getSnapshot().loading, false);
});

test("初次读取与后续轮询均保留用户正在编辑的草稿", async () => {
  const h = harness();
  h.controller.setActive(true);
  h.controller.edit(draft({ city: "草稿城" }));
  h.requests[0].resolve(manual());
  await settle();
  assert.equal(h.controller.getSnapshot().draft.city, "草稿城");
  await h.tick();
  h.requests[1].resolve(snapshot({ mode: "off", status: "off" }));
  await settle();
  assert.equal(h.controller.getSnapshot().draft.city, "草稿城");
  assert.equal(h.controller.getSnapshot().snapshot.mode, "off");
  assert.equal(h.requests.every(({ args }) => args[0] === "shared_location_status"), true);
});

test("无效草稿只返回字段错误，不发送写命令", async () => {
  const h = harness();
  await h.activate();
  h.controller.edit(draft({ city: "", lat: "", lon: "" }));
  assert.equal(await h.controller.save(), false);
  assert.equal(h.requests.length, 1);
  assert.deepEqual(Object.keys(h.controller.getSnapshot().validation), ["city"]);
});

test("保存调用精确契约；保存前的慢读取不能覆盖新设置", async () => {
  const h = harness();
  h.controller.setActive(true);
  h.controller.edit(draft({ city: " 新测试城 " }));
  const saving = h.controller.save();
  assert.deepEqual(h.requests[1].args, ["shared_location_configure", {
    input: { mode: "manual", manual: { city: "新测试城", lat: 0, lon: 0 } },
  }]);
  h.requests[1].resolve(manual({ city: "新测试城" }));
  assert.equal(await saving, true);
  h.requests[0].resolve(snapshot());
  await settle();
  assert.equal(h.controller.getSnapshot().snapshot.city, "新测试城");
  assert.equal(h.controller.getSnapshot().draft.city, "新测试城");
  assert.equal(h.controller.getSnapshot().dirty, false);
});

test("保存时禁止重复写入、编辑和轮询，旧查询失败也不显示为新错误", async () => {
  const h = harness();
  h.controller.setActive(true);
  h.controller.edit(draft());
  const saving = h.controller.save();
  assert.equal(await h.controller.save(), false);
  h.controller.edit({ mode: "off" });
  h.controller.resetDraft();
  await h.tick();
  assert.equal(h.requests.length, 2);
  assert.equal(h.controller.getSnapshot().draft.mode, "manual");
  h.requests[0].reject(new Error("旧查询错误"));
  await settle();
  assert.equal(h.controller.getSnapshot().readError, "");
  h.requests[1].resolve(manual());
  await saving;
  assert.equal(h.controller.getSnapshot().saving, false);
});

test("保存失败诚实报告结果未确认，保留草稿且只轮询、不重试写", async () => {
  const h = harness();
  await h.activate();
  h.controller.edit(draft({ city: "保留草稿城" }));
  const saving = h.controller.save();
  h.requests[1].reject("模拟传输断开");
  assert.equal(await saving, false);
  assert.equal(h.controller.getSnapshot().draft.city, "保留草稿城");
  assert.equal(h.controller.getSnapshot().dirty, true);
  assert.match(h.controller.getSnapshot().saveError, /结果未确认.*模拟传输断开.*草稿已保留/);
  await h.tick();
  h.requests[2].resolve(snapshot());
  await settle();
  assert.equal(h.controller.getSnapshot().draft.city, "保留草稿城");
  assert.equal(h.requests.filter(({ args }) => args[0] === "shared_location_configure").length, 1);
});

test("读取失败保留最近确认状态及草稿，后续成功查询才清除读取错误", async () => {
  const h = harness();
  await h.activate(manual());
  h.controller.edit({ city: "新草稿城" });
  const reading = h.controller.refresh();
  h.requests[1].reject(new Error("模拟只读失败"));
  await reading;
  assert.equal(h.controller.getSnapshot().snapshot.city, "测试城");
  assert.equal(h.controller.getSnapshot().draft.city, "新草稿城");
  assert.match(h.controller.getSnapshot().readError, /模拟只读失败/);
  const retry = h.controller.refresh();
  h.requests[2].resolve(manual());
  await retry;
  assert.equal(h.controller.getSnapshot().readError, "");
  assert.equal(h.controller.getSnapshot().draft.city, "新草稿城");
});

test("关闭或 auto 保存明确传 manual:null，错误快照不被当作保存成功", async () => {
  for (const mode of ["off", "auto"]) {
    const h = harness();
    await h.activate(manual());
    h.controller.edit({ mode });
    const saving = h.controller.save();
    assert.deepEqual(h.requests[1].args, ["shared_location_configure", { input: { mode, manual: null } }]);
    h.requests[1].resolve({ mode, status: "invalid" });
    assert.equal(await saving, false);
    assert.equal(h.controller.getSnapshot().snapshot.mode, "manual");
    assert.equal(h.controller.getSnapshot().draft.mode, mode);
    assert.equal(h.controller.getSnapshot().notice, "");
  }
});

test("已发出的保存可在离开面板后确认，离开期间不再查询", async () => {
  const h = harness();
  await h.activate();
  h.controller.edit(draft());
  const saving = h.controller.save();
  h.controller.setActive(false);
  assert.equal(h.timers.size, 0);
  await h.controller.refresh();
  assert.equal(h.requests.length, 2);
  h.requests[1].resolve(manual());
  await saving;
  assert.equal(h.controller.getSnapshot().snapshot.mode, "manual");
  assert.equal(h.controller.getSnapshot().saving, false);
});

test("恢复草稿必须由用户调用，只恢复最近快照且不写入", async () => {
  const h = harness();
  await h.activate(manual());
  h.controller.edit({ city: "放弃的草稿" });
  h.controller.resetDraft();
  assert.deepEqual(h.controller.getSnapshot().draft, draft());
  assert.equal(h.controller.getSnapshot().dirty, false);
  assert.equal(h.requests.length, 1);
});

// Synthetic candidates only. Native IPC is deferred by the harness, never called.
const candidate = (patch = {}) => ({
  id: 101, label: "样例区,样例市,样例省", city: "样例区", region: "样例市 · 样例省", country: "测试国",
  lat: 12.34, lon: 56.78, ...patch,
});
const editCity = (h, city) => h.controller.edit({ mode: "manual", city, lat: "", lon: "" });
const suggestCalls = (h) => h.calls("shared_location_suggest");

async function showCandidates(h, rows = [candidate()]) {
  await h.activate();
  h.controller.setSuggestionFocus(true);
  editCity(h, "样例区");
  await h.advance(SHARED_LOCATION_SUGGEST_MS);
  suggestCalls(h).at(-1).resolve(rows);
  await settle();
}

test("city-only 显式保存不发送坐标，并等待后端确认才清除草稿", async () => {
  const h = harness();
  await h.activate();
  editCity(h, " 样例省样例市样例县 ");
  const saving = h.controller.save();
  assert.deepEqual(h.calls("shared_location_configure")[0].args, ["shared_location_configure", {
    input: { mode: "manual", manual: { city: "样例省样例市样例县" } },
  }]);
  assert.equal(h.controller.getSnapshot().dirty, true);
  assert.equal(h.controller.getSnapshot().notice, "");
  h.calls("shared_location_configure")[0].resolve(manual({ city: "样例省样例市样例县", lat: 12.34, lon: 56.78 }));
  assert.equal(await saving, true);
  assert.equal(h.controller.getSnapshot().dirty, false);
  assert.equal(h.controller.getSnapshot().draft.lat, "12.34");
  assert.equal(h.controller.getSnapshot().draft.lon, "56.78");
});

test("city-only 的错误或不匹配确认不能伪装成保存成功", async () => {
  for (const returned of [manual({ city: "其他区" }), manual({ city: "样例区", status: "unknown" }), manual({ city: "样例区", lat: null })]) {
    const h = harness();
    await h.activate();
    editCity(h, "样例区");
    const saving = h.controller.save();
    h.calls("shared_location_configure")[0].resolve(returned);
    assert.equal(await saving, false);
    assert.equal(h.controller.getSnapshot().dirty, true);
    assert.equal(h.controller.getSnapshot().notice, "");
    assert.equal(h.controller.getSnapshot().draft.city, "样例区");
    assert.equal(h.calls("shared_location_configure").length, 1);
  }
});

test("候选仅接受有界安全ID、完整地区label和有限坐标，且去重", () => {
  assert.deepEqual(parseLocationSuggestions([candidate()]), [candidate()]);
  assert.throws(() => parseLocationSuggestions({ data: [candidate()] }));
  assert.throws(() => parseLocationSuggestions(Array.from({ length: 9 }, (_, id) => candidate({ id }))));
  const invalid = [
    null, candidate({ id: -1 }), candidate({ id: Number.MAX_SAFE_INTEGER + 1 }), candidate({ id: "101" }),
    candidate({ label: "汉".repeat(21) }), candidate({ label: "https://example.test" }), candidate({ label: "样例\n区" }),
    candidate({ lat: Infinity }), candidate({ lon: NaN }), candidate({ lat: 91 }), candidate({ lon: -181 }), candidate({ lat: "12.34" }),
  ];
  assert.deepEqual(parseLocationSuggestions(invalid.slice(0, 8)), []);
  assert.deepEqual(parseLocationSuggestions(invalid.slice(8)), []);
  assert.deepEqual(parseLocationSuggestions([candidate(), candidate({ label: "不应替换首个候选" })]), [candidate()]);
});

test("联想只在focused/active且至少2字符时350ms防抖查询，不写配置", async () => {
  const h = harness();
  h.controller.setSuggestionFocus(true);
  editCity(h, "未启用区");
  await h.advance(1000);
  assert.equal(h.requests.length, 0);
  await h.activate();
  h.controller.setSuggestionFocus(false);
  editCity(h, "未聚焦区");
  await h.advance(1000);
  assert.equal(suggestCalls(h).length, 0);
  h.controller.setSuggestionFocus(true);
  editCity(h, "区");
  await h.advance(1000);
  assert.equal(suggestCalls(h).length, 0);
  editCity(h, "样例");
  assert.equal(SHARED_LOCATION_SUGGEST_MS, 350);
  await h.advance(349);
  assert.equal(suggestCalls(h).length, 0);
  editCity(h, "样例区");
  await h.advance(349);
  assert.equal(suggestCalls(h).length, 0);
  await h.advance(1);
  assert.deepEqual(suggestCalls(h)[0].args, ["shared_location_suggest", { query: "样例区" }]);
  suggestCalls(h)[0].resolve([candidate()]);
  await settle();
  assert.equal(h.controller.getSnapshot().suggestions.length, 1);
  assert.equal(h.calls("shared_location_configure").length, 0);
});

test("慢联想期间只保留最新已防抖query，旧结果不展示且最多1个in-flight", async () => {
  const h = harness();
  const displayed = [];
  h.controller.subscribe(() => displayed.push(h.controller.getSnapshot().suggestions.map(item => item.label)));
  await h.activate();
  h.controller.setSuggestionFocus(true);
  editCity(h, "旧样例区");
  await h.advance(350);
  editCity(h, "中间样例区");
  await h.advance(350);
  editCity(h, "最新样例区");
  await h.advance(350);
  assert.equal(suggestCalls(h).length, 1);
  assert.equal(h.inFlight, 1);
  suggestCalls(h)[0].resolve([candidate({ label: "旧候选" })]);
  await settle();
  assert.equal(suggestCalls(h).length, 2);
  assert.deepEqual(suggestCalls(h)[1].args, ["shared_location_suggest", { query: "最新样例区" }]);
  assert.deepEqual(h.controller.getSnapshot().suggestions, []);
  suggestCalls(h)[1].resolve([candidate({ id: 202, label: "最新候选" })]);
  await settle();
  assert.equal(h.controller.getSnapshot().suggestions[0].label, "最新候选");
  assert.equal(displayed.some(labels => labels.includes("旧候选")), false);
  assert.equal(h.controller.getSnapshot().draft.city, "最新样例区");
  assert.equal(h.maximumInFlight, 1);
  assert.equal(h.inFlight, 0);
  assert.equal(h.calls("shared_location_configure").length, 0);
});

test("旧请求先结束时，新query尚未防抖完不能提前请求；旧错误不污染新输入", async () => {
  const h = harness();
  await h.activate();
  h.controller.setSuggestionFocus(true);
  editCity(h, "旧样例区");
  await h.advance(350);
  editCity(h, "新样例区");
  await h.advance(100);
  suggestCalls(h)[0].reject(new Error("旧请求后端详情"));
  await settle();
  assert.equal(h.controller.getSnapshot().suggestionError, "");
  assert.equal(suggestCalls(h).length, 1);
  await h.advance(249);
  assert.equal(suggestCalls(h).length, 1);
  await h.advance(1);
  assert.equal(suggestCalls(h).length, 2);
  suggestCalls(h)[1].resolve([candidate({ label: "新候选" })]);
  await settle();
  assert.equal(h.controller.getSnapshot().suggestions[0].label, "新候选");
  assert.equal(h.maximumInFlight, 1);
});

test("IME开始取消待查，组合输入不请求，结束后重新350ms防抖", async () => {
  const h = harness();
  await h.activate();
  h.controller.setSuggestionFocus(true);
  editCity(h, "拼音草稿");
  await h.advance(200);
  h.controller.setComposing(true);
  editCity(h, "组合中区");
  await h.advance(2000);
  assert.equal(h.timeouts.size, 0);
  assert.equal(suggestCalls(h).length, 0);
  editCity(h, "最终样例区");
  h.controller.setComposing(false);
  await h.advance(349);
  assert.equal(suggestCalls(h).length, 0);
  await h.advance(1);
  assert.deepEqual(suggestCalls(h)[0].args, ["shared_location_suggest", { query: "最终样例区" }]);
  h.controller.setComposing(true);
  suggestCalls(h)[0].resolve([candidate()]);
  await settle();
  assert.deepEqual(h.controller.getSnapshot().suggestions, []);
  assert.equal(h.controller.chooseSuggestion(0), false);
});

test("失焦、隐藏/卸载清理和Escape都取消待查并丢弃迟到响应", async () => {
  const clear = {
    blur: h => h.controller.setSuggestionFocus(false),
    hiddenOrUnmounted: h => h.controller.setActive(false),
    escape: h => h.controller.clearSuggestions(),
  };
  for (const cancel of Object.values(clear)) {
    const pending = harness();
    await pending.activate();
    pending.controller.setSuggestionFocus(true);
    editCity(pending, "待查样例区");
    cancel(pending);
    await pending.advance(1000);
    assert.equal(pending.timeouts.size, 0);
    assert.equal(suggestCalls(pending).length, 0);

    const h = harness();
    await h.activate();
    h.controller.setSuggestionFocus(true);
    editCity(h, "在途样例区");
    await h.advance(350);
    editCity(h, "排队样例区");
    await h.advance(350);
    cancel(h);
    suggestCalls(h)[0].resolve([candidate()]);
    await settle();
    await h.advance(1000);
    assert.equal(suggestCalls(h).length, 1);
    assert.deepEqual(h.controller.getSnapshot().suggestions, []);
    assert.equal(h.controller.getSnapshot().suggesting, false);
    assert.equal(h.controller.getSnapshot().suggestionError, "");
  }
});

test("离开后再进入且query相同，上一周期结果也不能展示或释放并发限制", async () => {
  const h = harness();
  await h.activate();
  h.controller.setSuggestionFocus(true);
  editCity(h, "相同样例区");
  await h.advance(350);
  h.controller.setActive(false);
  assert.equal(h.timers.size, 0);
  await h.activate();
  h.controller.setSuggestionFocus(true);
  await h.advance(350);
  assert.equal(suggestCalls(h).length, 1);
  suggestCalls(h)[0].resolve([candidate({ label: "上一周期候选" })]);
  await settle();
  assert.deepEqual(h.controller.getSnapshot().suggestions, []);
  assert.equal(suggestCalls(h).length, 2);
  suggestCalls(h)[1].resolve([candidate({ label: "本周期候选" })]);
  await settle();
  assert.equal(h.controller.getSnapshot().suggestions[0].label, "本周期候选");
  assert.equal(h.maximumInFlight, 1);
});

test("保存清除候选和pending，saving期间的聚焦/输入/IME不产生新联想", async () => {
  const h = harness();
  await h.activate();
  h.controller.setSuggestionFocus(true);
  editCity(h, "在途样例区");
  await h.advance(350);
  editCity(h, "待保存样例区");
  await h.advance(350);
  const saving = h.controller.save();
  assert.equal(h.timeouts.size, 0);
  assert.deepEqual(h.controller.getSnapshot().suggestions, []);
  h.controller.setSuggestionFocus(true);
  h.controller.setComposing(false);
  editCity(h, "不应覆盖区");
  await h.advance(1000);
  assert.equal(suggestCalls(h).length, 1);
  assert.equal(h.controller.getSnapshot().draft.city, "待保存样例区");
  suggestCalls(h)[0].resolve([candidate()]);
  await settle();
  assert.deepEqual(h.controller.getSnapshot().suggestions, []);
  h.calls("shared_location_configure")[0].resolve(manual({ city: "待保存样例区", lat: 12.34, lon: 56.78 }));
  assert.equal(await saving, true);
  await h.advance(1000);
  assert.equal(suggestCalls(h).length, 1);
  assert.equal(h.calls("shared_location_configure").length, 1);
});

test("选择候选不自动保存，显式Save保留完整label和所选坐标", async () => {
  const h = harness();
  const item = candidate();
  await showCandidates(h, [item]);
  assert.equal(h.controller.chooseSuggestion(0), true);
  assert.equal(h.calls("shared_location_configure").length, 0);
  assert.equal(h.controller.getSnapshot().snapshot.mode, "auto");
  assert.deepEqual(h.controller.getSnapshot().draft, draft({ city: item.label, lat: String(item.lat), lon: String(item.lon) }));
  assert.equal(h.controller.getSnapshot().dirty, true);
  assert.deepEqual(h.controller.getSnapshot().suggestions, []);
  // Same mode-only edit as SharedLocationPanel.saveManual: never discard coordinates.
  h.controller.edit({ mode: "manual" });
  const saving = h.controller.save();
  assert.equal(await h.controller.save(), false);
  assert.deepEqual(h.calls("shared_location_configure")[0].args, ["shared_location_configure", {
    input: { mode: "manual", manual: { city: item.label, lat: item.lat, lon: item.lon } },
  }]);
  assert.equal(h.timeouts.size, 0);
  h.calls("shared_location_configure")[0].resolve(manual({ city: item.label, lat: item.lat, lon: item.lon }));
  assert.equal(await saving, true);
  assert.equal(h.controller.getSnapshot().dirty, false);
  assert.equal(h.controller.getSnapshot().draft.lat, String(item.lat));
  assert.equal(h.controller.getSnapshot().draft.lon, String(item.lon));
  // Continuing to type follows the UI contract and removes stale candidate coordinates.
  editCity(h, "继续编辑区");
  assert.deepEqual(buildLocationInput(h.controller.getSnapshot().draft), { mode: "manual", manual: { city: "继续编辑区" } });
});

test("候选上下键索引循环且仅显式选择改草稿；无候选/离开不能选择", async () => {
  const h = harness();
  const rows = [candidate(), candidate({ id: 102, label: "另一样例区" })];
  await showCandidates(h, rows);
  const original = { ...h.controller.getSnapshot().draft };
  assert.equal(h.controller.getSnapshot().suggestionIndex, -1);
  h.controller.moveSuggestion(-1);
  assert.equal(h.controller.getSnapshot().suggestionIndex, 1);
  h.controller.moveSuggestion(1);
  assert.equal(h.controller.getSnapshot().suggestionIndex, 0);
  h.controller.moveSuggestion(1);
  assert.equal(h.controller.getSnapshot().suggestionIndex, 1);
  assert.deepEqual(h.controller.getSnapshot().draft, original);
  assert.equal(h.controller.chooseSuggestion(99), false);
  assert.equal(h.controller.chooseSuggestion(h.controller.getSnapshot().suggestionIndex), true);
  assert.equal(h.controller.getSnapshot().draft.city, rows[1].label);
  assert.equal(h.calls("shared_location_configure").length, 0);
  h.controller.setActive(false);
  h.controller.moveSuggestion(1);
  assert.equal(h.controller.chooseSuggestion(0), false);
});

test("联想失败只给短提示且不自动重试，新输入后才可重新查；空结果不禁止city-only保存", async () => {
  const h = harness();
  await h.activate();
  h.controller.setSuggestionFocus(true);
  editCity(h, "断网样例区");
  await h.advance(350);
  suggestCalls(h)[0].reject(new Error("不应显示的后端详情"));
  await settle();
  assert.match(h.controller.getSnapshot().suggestionError, /暂不可用/);
  assert.doesNotMatch(h.controller.getSnapshot().suggestionError, /后端详情/);
  await h.advance(10_000);
  assert.equal(suggestCalls(h).length, 1);
  editCity(h, "新样例区");
  await h.advance(350);
  assert.equal(suggestCalls(h).length, 2);
  suggestCalls(h)[1].resolve([]);
  await settle();
  assert.match(h.controller.getSnapshot().suggestionError, /未找到地区/);
  assert.deepEqual(h.controller.getSnapshot().validation, {});
  const saving = h.controller.save();
  assert.deepEqual(h.calls("shared_location_configure")[0].args[1].input, { mode: "manual", manual: { city: "新样例区" } });
  h.calls("shared_location_configure")[0].resolve(manual({ city: "新样例区", lat: 12.34, lon: 56.78 }));
  assert.equal(await saving, true);
});
