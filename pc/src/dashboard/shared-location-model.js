// Pure state and command boundary. No location, storage, or network access here.
export const SHARED_LOCATION_POLL_MS = 10_000;
export const SHARED_LOCATION_SUGGEST_MS = 350;
export const LOCATION_MODES = Object.freeze({ auto: "IP 估算", manual: "手动设置", off: "关闭" });
export const CITY_PRESETS = Object.freeze([
  { city: "北京", lat: 39.9042, lon: 116.4074 },
  { city: "上海", lat: 31.2304, lon: 121.4737 },
  { city: "广州", lat: 23.1291, lon: 113.2644 },
  { city: "深圳", lat: 22.5431, lon: 114.0579 },
  { city: "杭州", lat: 30.2741, lon: 120.1551 },
  { city: "成都", lat: 30.5728, lon: 104.0668 },
  { city: "香港", lat: 22.3193, lon: 114.1694 },
  { city: "新加坡", lat: 1.3521, lon: 103.8198 },
  { city: "纽约", lat: 40.7128, lon: -74.006 },
].map(Object.freeze));

const MODES = Object.keys(LOCATION_MODES);
const STATUSES = ["ready", "locating", "unknown", "off", "error"];
const validCoordinate = (value, limit) => typeof value === "number" && Number.isFinite(value) && Math.abs(value) <= limit;
const optionalNumber = (value) => value === null || (typeof value === "number" && Number.isFinite(value) && value >= 0);
const stringOrEmpty = (value) => typeof value === "string" ? value : "";

export function parseLocationSnapshot(value) {
  if (!value || !MODES.includes(value.mode) || !STATUSES.includes(value.status)
    || !["ip", "manual", null].includes(value.source)
    || !(value.lat === null || validCoordinate(value.lat, 90))
    || !(value.lon === null || validCoordinate(value.lon, 180))
    || !optionalNumber(value.updatedAt) || !optionalNumber(value.expiresAt)) {
    throw new Error("位置服务返回了无效状态，无法确认当前设置。");
  }
  return {
    mode: value.mode, status: value.status, source: value.source,
    city: stringOrEmpty(value.city), region: stringOrEmpty(value.region), country: stringOrEmpty(value.country),
    lat: value.lat, lon: value.lon, updatedAt: value.updatedAt, expiresAt: value.expiresAt,
    error: stringOrEmpty(value.error),
  };
}

export function draftFromSnapshot(snapshot) {
  return {
    mode: snapshot?.mode ?? "auto",
    city: snapshot?.mode === "manual" ? snapshot.city : "",
    lat: snapshot?.mode === "manual" && snapshot.lat !== null ? String(snapshot.lat) : "",
    lon: snapshot?.mode === "manual" && snapshot.lon !== null ? String(snapshot.lon) : "",
  };
}

function parseCoordinate(value) {
  // Empty input must never turn into the real coordinate 0.
  const text = String(value ?? "").trim();
  return /^[+-]?(?:\d+\.?\d*|\.\d+)(?:e[+-]?\d+)?$/i.test(text) ? Number(text) : NaN;
}

export function validateLocationDraft(draft) {
  const errors = {};
  if (!MODES.includes(draft.mode)) errors.mode = "请选择位置模式。";
  if (draft.mode !== "manual") return errors;
  const rawCity = String(draft.city ?? "");
  const city = rawCity.trim();
  if (!city || new TextEncoder().encode(city).length > 60 || /[\p{Cc}\p{Cf}]/u.test(rawCity)
    || !/^[\p{L}\p{N} .\-'·（）(),，]+$/u.test(city)) errors.city = "请填写有效地区名称（最多 60 字节），不要输入网址或控制字符。";
  const hasCoordinates = String(draft.lat ?? "").trim() !== "" || String(draft.lon ?? "").trim() !== "";
  if (hasCoordinates && !validCoordinate(parseCoordinate(draft.lat), 90)) errors.lat = "纬度须为 -90 至 90 的数字。";
  if (hasCoordinates && !validCoordinate(parseCoordinate(draft.lon), 180)) errors.lon = "经度须为 -180 至 180 的数字。";
  return errors;
}

export function buildLocationInput(draft) {
  if (Object.keys(validateLocationDraft(draft)).length) throw new Error("请检查手动位置后再保存。");
  return {
    mode: draft.mode,
    manual: draft.mode === "manual"
      ? { city: draft.city.trim(), ...(String(draft.lat ?? "").trim() !== "" || String(draft.lon ?? "").trim() !== ""
          ? { lat: parseCoordinate(draft.lat), lon: parseCoordinate(draft.lon) } : {}) }
      : null,
  };
}

// Accept Unix seconds and milliseconds without assuming a city or device timezone.
export function locationEpochMs(value) {
  return value === null ? null : value < 1e12 ? value * 1000 : value;
}

export function describeSharedLocation(snapshot, now = Date.now()) {
  if (!snapshot) return { available: false, title: "尚未读取位置状态" };
  if (snapshot.mode === "off" || snapshot.status === "off") return { available: false, title: "共享位置已关闭" };
  if (snapshot.status === "error") return { available: false, title: "位置服务出错" };
  if (snapshot.status === "locating") return { available: false, title: "正在估算位置…" };
  if (snapshot.status !== "ready") return { available: false, title: "暂未获得位置" };
  if (snapshot.expiresAt !== null && locationEpochMs(snapshot.expiresAt) <= now) {
    return { available: false, title: "位置已过期，等待位置服务更新" };
  }
  if (!snapshot.city.trim() || !validCoordinate(snapshot.lat, 90) || !validCoordinate(snapshot.lon, 180)
    || snapshot.source !== (snapshot.mode === "manual" ? "manual" : "ip")) {
    return { available: false, title: "位置数据不完整，暂不可用" };
  }
  return { available: true, title: snapshot.city };
}

// Suggestions only become draft coordinates after an explicit user selection.
export function parseLocationSuggestions(value) {
  if (!Array.isArray(value) || value.length > 8) throw new Error("地区联想返回了无效结果。");
  const ids = new Set();
  return value.filter((item) => {
    if (!item || !Number.isSafeInteger(item.id) || item.id < 0 || ids.has(item.id)
      || typeof item.label !== "string" || !item.label.trim()
      || validateLocationDraft({ mode: "manual", city: item.label, lat: "", lon: "" }).city
      || !validCoordinate(item.lat, 90) || !validCoordinate(item.lon, 180)) return false;
    ids.add(item.id);
    return true;
  }).map((item) => ({
    id: item.id, label: item.label.trim(), city: stringOrEmpty(item.city),
    region: stringOrEmpty(item.region), country: stringOrEmpty(item.country), lat: item.lat, lon: item.lon,
  }));
}

function errorMessage(error) {
  if (typeof error === "string" && error.trim()) return error;
  if (typeof error?.message === "string" && error.message.trim()) return error.message;
  return "位置服务请求失败，未能确认结果。";
}

/** Inject commands/timers for offline tests; only explicit save() writes configuration. */
export function createSharedLocationController({
  invoke, setInterval: schedule = globalThis.setInterval, clearInterval: cancel = globalThis.clearInterval,
  setTimeout: defer = globalThis.setTimeout, clearTimeout: cancelDeferred = globalThis.clearTimeout,
}) {
  let state = {
    snapshot: null, draft: draftFromSnapshot(null), dirty: false,
    loading: false, saving: false, readError: "", saveError: "", notice: "", validation: {},
    suggestions: [], suggesting: false, suggestionError: "", suggestionIndex: -1,
  };
  let active = false;
  let interval = null;
  let readVersion = 0;
  let suggestionVersion = 0;
  let suggestionTimer = null;
  let suggestionFocused = false;
  let composing = false;
  let suggestionInFlight = false;
  let pendingSuggestion = null;
  const listeners = new Set();
  const update = (patch) => {
    state = { ...state, ...patch };
    listeners.forEach((listener) => listener());
  };

  function clearSuggestions() {
    ++suggestionVersion;
    if (suggestionTimer !== null) cancelDeferred(suggestionTimer);
    suggestionTimer = null;
    pendingSuggestion = null;
    update({ suggestions: [], suggesting: false, suggestionError: "", suggestionIndex: -1 });
  }

  function canSuggest(request) {
    return active && suggestionFocused && !composing && !state.saving
      && state.draft.mode === "manual" && request.version === suggestionVersion
      && state.draft.city.trim() === request.query;
  }

  async function queryPendingSuggestion() {
    // The in-flight slot is held until native settles, even after invalidation.
    // Debounced newer input waits in a single replaceable pending slot.
    if (suggestionInFlight || !pendingSuggestion?.ready || !canSuggest(pendingSuggestion)) return;
    const request = pendingSuggestion;
    pendingSuggestion = null;
    suggestionInFlight = true;
    update({ suggesting: true, suggestionError: "" });
    try {
      const result = await invoke("shared_location_suggest", { query: request.query });
      if (!canSuggest(request)) return;
      const suggestions = parseLocationSuggestions(result);
      update({ suggestions, suggestionIndex: -1, suggestionError: suggestions.length ? "" : "未找到地区，请补充省市" });
    } catch {
      if (canSuggest(request)) update({ suggestions: [], suggestionIndex: -1, suggestionError: "地区联想暂不可用，可继续输入" });
    } finally {
      suggestionInFlight = false;
      if (canSuggest(request)) update({ suggesting: false });
      // Only a newer, already-debounced query may run. Never retry this request.
      if (pendingSuggestion?.ready) void queryPendingSuggestion();
    }
  }

  function scheduleSuggestions() {
    clearSuggestions();
    const query = state.draft.city.trim();
    const request = { query, version: suggestionVersion, ready: false };
    if (!canSuggest(request) || [...query].length < 2
      || validateLocationDraft({ mode: "manual", city: state.draft.city, lat: "", lon: "" }).city) return;
    pendingSuggestion = request;
    suggestionTimer = defer(() => {
      suggestionTimer = null;
      if (pendingSuggestion !== request || !canSuggest(request)) return;
      request.ready = true;
      void queryPendingSuggestion();
    }, SHARED_LOCATION_SUGGEST_MS);
  }

  async function refresh() {
    if (!active || state.loading || state.saving) return;
    const version = ++readVersion;
    update({ loading: true });
    try {
      const snapshot = parseLocationSnapshot(await invoke("shared_location_status"));
      if (!active || version !== readVersion) return;
      update({ snapshot, readError: "", ...(!state.dirty ? { draft: draftFromSnapshot(snapshot) } : {}) });
    } catch (error) {
      if (active && version === readVersion) update({ readError: errorMessage(error) });
    } finally {
      if (version === readVersion) update({ loading: false });
    }
  }

  return {
    getSnapshot: () => state,
    subscribe(listener) { listeners.add(listener); return () => listeners.delete(listener); },
    setActive(next) {
      next = next === true;
      if (next === active) return;
      active = next;
      if (active) {
        void refresh();
        interval = schedule(() => { void refresh(); }, SHARED_LOCATION_POLL_MS);
      } else {
        suggestionFocused = false;
        composing = false;
        clearSuggestions();
        cancel(interval);
        interval = null;
        ++readVersion;
        update({ loading: false });
      }
    },
    refresh,
    clearSuggestions,
    setSuggestionFocus(next) {
      suggestionFocused = next === true;
      if (suggestionFocused) scheduleSuggestions(); else { composing = false; clearSuggestions(); }
    },
    setComposing(next) {
      composing = next === true;
      if (composing) clearSuggestions(); else scheduleSuggestions();
    },
    moveSuggestion(direction) {
      const count = state.suggestions.length;
      if (!active || state.saving || !suggestionFocused || composing || !count) return;
      const index = state.suggestionIndex;
      update({ suggestionIndex: index < 0 ? direction < 0 ? count - 1 : 0 : (index + direction + count) % count });
    },
    chooseSuggestion(index) {
      if (!active || state.saving || !suggestionFocused || composing) return false;
      const item = state.suggestions[index];
      if (!item) return false;
      clearSuggestions();
      update({ draft: { ...state.draft, mode: "manual", city: item.label, lat: String(item.lat), lon: String(item.lon) }, dirty: true, validation: {}, saveError: "", notice: "" });
      return true;
    },
    edit(patch) {
      if (state.saving) return;
      const allowed = Object.fromEntries(Object.entries(patch).filter(([key]) => ["mode", "city", "lat", "lon"].includes(key)));
      update({ draft: { ...state.draft, ...allowed }, dirty: true, validation: {}, saveError: "", notice: "" });
      scheduleSuggestions();
    },
    resetDraft() {
      if (state.saving || !state.snapshot) return;
      clearSuggestions();
      update({ draft: draftFromSnapshot(state.snapshot), dirty: false, validation: {}, saveError: "", notice: "" });
    },
    async save() {
      if (!active || state.saving) return false;
      clearSuggestions();
      const validation = validateLocationDraft(state.draft);
      if (Object.keys(validation).length) { update({ validation, notice: "" }); return false; }
      const input = buildLocationInput(state.draft);
      // Invalidate every status read begun before this write. Writes are single-flight.
      ++readVersion;
      update({ saving: true, loading: false, saveError: "", notice: "", validation: {} });
      try {
        const snapshot = parseLocationSnapshot(await invoke("shared_location_configure", { input }));
        if(snapshot.mode!==input.mode || (input.manual && (!describeSharedLocation(snapshot).available || snapshot.source!=="manual" || snapshot.city!==input.manual.city || (input.manual.lat !== undefined && Math.abs(snapshot.lat-input.manual.lat)>0.005000001) || (input.manual.lon !== undefined && Math.abs(snapshot.lon-input.manual.lon)>0.005000001)))) throw new Error("位置保存确认不匹配，请先查询状态。");
        update({ snapshot, draft: draftFromSnapshot(snapshot), dirty: false, readError: "", notice: "设置已保存。" });
        return true;
      } catch (error) {
        // A failed transport may still have applied the write: never claim rollback or retry it.
        update({ dirty: true, saveError: `保存结果未确认：${errorMessage(error)} 草稿已保留，可查询状态后再决定是否保存。` });
        return false;
      } finally {
        update({ saving: false });
      }
    },
  };
}
