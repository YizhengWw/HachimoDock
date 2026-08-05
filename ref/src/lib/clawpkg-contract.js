/**
 * [Input] 设备端 fb_speech_overlay.c 的 STATS_DASHBOARD_V1 版式与 br_stats_dashboard_model 字节上限。
 * [Output] .clawpkg 包结构常量、game/tool 类型、可选 COMPONENT_DASHBOARD_V1 内容与视觉槽位、
 *          每组件按钮数量/事件/标签尺寸（含四向摇杆和旧旋钮别名）、P4 vars 对象与单条规则 effect 约束，以及清单校验函数。
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
  { id: "visualStyle", maxBytes: 16, role: "安全视觉样式:classic/pixel/clean" },
  { id: "visualPalette", maxBytes: 16, role: "预置配色:candy/sunset/mint/arcade/ocean/forest/ember/mono" },
  { id: "visualLayout", maxBytes: 16, role: "预置布局:arcade/scoreboard/tool" },
  { id: "visualSprite", maxBytes: 16, role: "预置像素精灵:target/trophy/star/bolt/coffee/timer/droplet/gauge/blocks/snake/flappy/mole-ready/mole-left/mole-center/mole-right" },
];

export const COMPONENT_VISUAL_PRESETS = {
  visualStyle: ["classic", "pixel", "clean"],
  visualPalette: ["candy", "sunset", "mint", "arcade", "ocean", "forest", "ember", "mono"],
  visualLayout: ["arcade", "scoreboard", "tool"],
  visualSprite: [
    "target", "trophy", "star", "bolt", "coffee", "timer", "droplet", "gauge",
    "blocks", "snake", "flappy",
    "mole-ready", "mole-left", "mole-center", "mole-right",
  ],
};

export const COMPONENT_RUNTIME_ENGINES = ["p4-bounded-runtime-v3"];
export const COMPONENT_SCENE_ENGINE = "p4-grid-scene-v1";
export const COMPONENT_GAME_PRESETS = ["blocks", "snake", "flappy"];
// Backward-compatible export for callers that still label native presets as game types.
export const COMPONENT_GAME_TYPES = COMPONENT_GAME_PRESETS;
export const COMPONENT_KINDS = ["game", "tool"];
export const COMPONENT_ID_PATTERN = /^[a-z][a-z0-9_-]{0,46}$/;
export const COMPONENT_GAME_TICK_MIN_MS = 100;
export const COMPONENT_GAME_TICK_MAX_MS = 2000;
export const COMPONENT_SCENE_MAX_ENTITIES = 12;
export const COMPONENT_SCENE_MAX_RULES = 20;
export const COMPONENT_SCENE_MAX_OPS = 4;
export const COMPONENT_SCENE_SHAPES = [
  "rect", "player-ship", "enemy-ship", "bullet", "star", "paddle", "ball",
];
export const P4_WIDGET_MAX_VARS = 8;
export const P4_WIDGET_VAR_NAME_MAX_BYTES = 31;
export const P4_WIDGET_STRING_VAR_MAX_BYTES = 63;
export const P4_WIDGET_INT_MIN = -1000000000;
export const P4_WIDGET_INT_MAX = 1000000000;

// P4 stores each decoded JSON document in a fixed receive buffer. The trailing
// NUL occupies one byte, so authored content is capped at capacity - 1.
export const P4_WIDGET_JSON_MAX_BYTES = 4095;
export const P4_BUTTONS_JSON_MAX_BYTES = 2047;
export const P4_WIDGET_MAX_EFFECTS = 4;
export const COMPONENT_BUTTON_MAX_BINDINGS = 8;
export const COMPONENT_BUTTON_LABEL_MAX_BYTES = 30;
export const COMPONENT_BUTTON_EVENTS = [
  "screen.region.tap",
  "screen.region.long_press",
  "button.sw1.short_press",
  "button.sw2.short_press",
  "button.sw3.short_press",
  "button.encoder.short_press",
  "button.encoder.long_press",
  "knob.rotate_cw",
  "knob.rotate_ccw",
  "joystick.up",
  "joystick.down",
  "knob.rotate_cw / knob.rotate_ccw",
];

const SLOT_BY_ID = new Map(COMPONENT_DASHBOARD_V1_SLOTS.map((s) => [s.id, s]));
const utf8Bytes = (text) => new TextEncoder().encode(String(text ?? "")).length;
const compactJsonBytes = (value) => utf8Bytes(JSON.stringify(value));

function validateComponentButtons(buttons, errors) {
  if (!Array.isArray(buttons)) {
    errors.push("buttons.json 必须是数组");
    return;
  }
  if (buttons.length > COMPONENT_BUTTON_MAX_BINDINGS) {
    errors.push(`buttons.json 最多允许 ${COMPONENT_BUTTON_MAX_BINDINGS} 个按钮动作`);
  }
  const usedEvents = new Set();
  const usedActions = new Set();
  buttons.forEach((binding, index) => {
    if (!binding || typeof binding !== "object" || Array.isArray(binding)) {
      errors.push(`buttons.json 第 ${index + 1} 项必须是对象`);
      return;
    }
    for (const field of ["action", "control", "event", "label"]) {
      if (typeof binding[field] !== "string" || !binding[field].trim()) {
        errors.push(`buttons.json 第 ${index + 1} 项缺少非空字段 ${field}`);
      }
    }
    const event = typeof binding.event === "string" ? binding.event.trim() : "";
    const action = typeof binding.action === "string" ? binding.action.trim() : "";
    if (action && usedActions.has(action)) {
      errors.push(`buttons.json 动作 ${action} 重复，无法独立换键`);
    }
    if (action) usedActions.add(action);
    if (event && !COMPONENT_BUTTON_EVENTS.includes(event)) {
      errors.push(`buttons.json 第 ${index + 1} 项含未知事件 ${event}`);
    }
    const eventSlots = event === "knob.rotate_cw / knob.rotate_ccw"
      ? ["knob.rotate_cw", "knob.rotate_ccw"]
      : [event];
    if (event && eventSlots.some((slot) => usedEvents.has(slot))) {
      errors.push(`buttons.json 事件 ${event} 与已有绑定冲突`);
    }
    if (event) eventSlots.forEach((slot) => usedEvents.add(slot));
    if (typeof binding.label === "string" && utf8Bytes(binding.label) > COMPONENT_BUTTON_LABEL_MAX_BYTES) {
      errors.push(`buttons.json 第 ${index + 1} 项标签超出 ${COMPONENT_BUTTON_LABEL_MAX_BYTES} 字节上限`);
    }
  });
}

const isObject = (value) => Boolean(value) && typeof value === "object" && !Array.isArray(value);
const isBoundedInt = (value, minimum, maximum) => (
  Number.isInteger(value) && value >= minimum && value <= maximum
);
const hasOnlyKeys = (value, keys) => (
  isObject(value) && Object.keys(value).every((key) => keys.includes(key))
);

function validateLegacyGame(widget, buttons, errors) {
  const game = widget?.game;
  if (game === undefined) return;
  if (!isObject(game)) {
    errors.push("runtime/widget.json game 必须是对象");
    return;
  }
  if (!COMPONENT_GAME_PRESETS.includes(game.type)) {
    errors.push(`runtime/widget.json game.type 不支持 ${String(game.type || "")}`);
    return;
  }
  const allowedGameKeys = [
    "type", "tick_ms", "playing_state", "result_state", "score_var", "actions",
  ];
  if (!hasOnlyKeys(game, allowedGameKeys)) errors.push("runtime/widget.json game 含未知字段");
  if (!isBoundedInt(game.tick_ms, COMPONENT_GAME_TICK_MIN_MS, COMPONENT_GAME_TICK_MAX_MS)) {
    errors.push(`runtime/widget.json game.tick_ms 必须在 ${COMPONENT_GAME_TICK_MIN_MS}-${COMPONENT_GAME_TICK_MAX_MS}`);
  }
  const stateIds = new Set(Array.isArray(widget.states) ? widget.states : []);
  if (!stateIds.has(game.playing_state) || !stateIds.has(game.result_state)) {
    errors.push("runtime/widget.json game 引用了未知 playing/result 状态");
  }
  if (widget?.vars?.[game.score_var]?.type !== "int") {
    errors.push("runtime/widget.json game.score_var 必须引用 int 变量");
  }
  const requiredActions = game.type === "blocks"
    ? ["start", "left", "right", "rotate", "drop"]
    : game.type === "snake" ? ["start", "left", "right"] : ["flap"];
  const actionKeys = isObject(game.actions) ? Object.keys(game.actions) : [];
  if (actionKeys.length !== requiredActions.length
      || actionKeys.some((key) => !requiredActions.includes(key))) {
    errors.push(`runtime/widget.json ${game.type} game.actions 字段不完整或含未知动作`);
  }
  const transitionActions = new Set(
    Array.isArray(widget.transitions) ? widget.transitions.map((item) => item?.on) : [],
  );
  const buttonActions = new Set(Array.isArray(buttons) ? buttons.map((item) => item?.action) : []);
  requiredActions.forEach((key) => {
    const action = game.actions?.[key];
    if (typeof action !== "string" || !action
        || !transitionActions.has(action) || !buttonActions.has(action)) {
      errors.push(`runtime/widget.json game.actions.${key} 必须同时匹配 transition 与 button`);
    }
  });
}

function validSceneCoordinate(value) {
  if (isBoundedInt(value, 0, 15)) return true;
  return Array.isArray(value) && value.length === 2
    && isBoundedInt(value[0], 0, 15)
    && isBoundedInt(value[1], value[0], 15);
}

function validateSceneOp(op, entityIds, errors, label) {
  if (!isObject(op) || typeof op.op !== "string") {
    errors.push(`${label} 必须是含 op 的对象`);
    return;
  }
  const requireEntity = () => {
    if (!entityIds.has(op.entity)) {
      errors.push(`${label}.entity 引用了未知实体`);
      return false;
    }
    return true;
  };
  const vectorsValid = (x, y) => isBoundedInt(op[x], -4, 4) && isBoundedInt(op[y], -4, 4);
  if (op.op === "move") {
    if (!hasOnlyKeys(op, ["op", "entity", "dx", "dy"]) || !requireEntity()
        || !vectorsValid("dx", "dy") || (op.dx === 0 && op.dy === 0)) {
      errors.push(`${label} move 需要实体及 -4..4 的非零 dx/dy`);
    }
  } else if (["velocity", "accelerate"].includes(op.op)) {
    if (!hasOnlyKeys(op, ["op", "entity", "vx", "vy"]) || !requireEntity()
        || !vectorsValid("vx", "vy")) {
      errors.push(`${label} ${op.op} 需要实体及 -4..4 的 vx/vy`);
    }
  } else if (op.op === "place") {
    const keysValid = hasOnlyKeys(op, ["op", "entity", "source", "x", "y", "dx", "dy"]);
    const sourceValid = op.source === undefined || entityIds.has(op.source);
    const offsetsValid = (op.dx === undefined || isBoundedInt(op.dx, -4, 4))
      && (op.dy === undefined || isBoundedInt(op.dy, -4, 4));
    const coordsValid = (op.x === undefined || validSceneCoordinate(op.x))
      && (op.y === undefined || validSceneCoordinate(op.y));
    if (!keysValid || !requireEntity() || !sourceValid || !offsetsValid || !coordsValid
        || (op.source === undefined && op.x === undefined && op.y === undefined)) {
      errors.push(`${label} place 需要合法实体、坐标范围或 source`);
    }
  } else if (["show", "hide"].includes(op.op)) {
    if (!hasOnlyKeys(op, ["op", "entity"]) || !requireEntity()) {
      errors.push(`${label} ${op.op} 需要一个已声明实体`);
    }
  } else if (op.op === "score") {
    const hasAdd = Object.hasOwn(op, "add");
    const hasSet = Object.hasOwn(op, "set");
    const value = hasAdd ? op.add : op.set;
    if (!hasOnlyKeys(op, ["op", "add", "set"]) || hasAdd === hasSet
        || !isBoundedInt(value, -10000, 10000)) {
      errors.push(`${label} score 必须且只能含一个 -10000..10000 的 add/set`);
    }
  } else if (["run", "stop", "restart"].includes(op.op)) {
    if (!hasOnlyKeys(op, ["op"])) errors.push(`${label} ${op.op} 不接受其他字段`);
  } else if (op.op === "bounce") {
    if (!hasOnlyKeys(op, ["op", "entity", "axis"]) || !requireEntity()
        || !["x", "y", "both"].includes(op.axis)) {
      errors.push(`${label} bounce 需要实体及 x/y/both 轴`);
    }
  } else if (op.op === "tone") {
    if (!hasOnlyKeys(op, ["op", "entity", "tone"]) || !requireEntity()
        || !isBoundedInt(op.tone, 1, 4)) {
      errors.push(`${label} tone 需要实体及 1..4 色阶`);
    }
  } else {
    errors.push(`${label}.op 不支持 ${op.op}`);
  }
}

function validateBoundedScene(widget, buttons, errors) {
  const scene = widget?.scene;
  if (scene === undefined) return;
  if (widget.engine !== COMPONENT_RUNTIME_ENGINES[0]) {
    errors.push(`runtime/widget.json scene 需要 engine=${COMPONENT_RUNTIME_ENGINES[0]}`);
  }
  if (!hasOnlyKeys(scene, [
    "tick_ms", "active_state", "result_state", "score_var", "auto_start",
    "grid", "entities", "rules",
  ])) {
    errors.push("runtime/widget.json scene 含未知字段或不是对象");
    return;
  }
  if (!isBoundedInt(scene.tick_ms, COMPONENT_GAME_TICK_MIN_MS, COMPONENT_GAME_TICK_MAX_MS)) {
    errors.push(`runtime/widget.json scene.tick_ms 必须在 ${COMPONENT_GAME_TICK_MIN_MS}-${COMPONENT_GAME_TICK_MAX_MS}`);
  }
  const stateIds = new Set(Array.isArray(widget.states) ? widget.states : []);
  if (!stateIds.has(scene.active_state)
      || (scene.result_state !== undefined && !stateIds.has(scene.result_state))) {
    errors.push("runtime/widget.json scene 引用了未知 active/result 状态");
  }
  if (scene.score_var !== undefined && widget?.vars?.[scene.score_var]?.type !== "int") {
    errors.push("runtime/widget.json scene.score_var 必须引用 int 变量");
  }
  if (scene.auto_start !== undefined && typeof scene.auto_start !== "boolean") {
    errors.push("runtime/widget.json scene.auto_start 必须是布尔值");
  }
  if (scene.auto_start === true && widget.initial_state !== scene.active_state) {
    errors.push("runtime/widget.json scene.auto_start 要求 initial_state 等于 active_state");
  }
  const grid = scene.grid;
  if (!hasOnlyKeys(grid, ["width", "height", "rows", "solid_tones"])
      || !isBoundedInt(grid?.width, 4, 16) || !isBoundedInt(grid?.height, 4, 16)) {
    errors.push("runtime/widget.json scene.grid 需要 4..16 的 width/height");
  } else {
    if (grid.rows !== undefined && (!Array.isArray(grid.rows)
        || grid.rows.length !== grid.height
        || grid.rows.some((row) => typeof row !== "string"
          || row.length !== grid.width || !/^[0-4]+$/.test(row)))) {
      errors.push("runtime/widget.json scene.grid.rows 必须按高度/宽度使用 0..4 色阶");
    }
    if (grid.solid_tones !== undefined && (!Array.isArray(grid.solid_tones)
        || grid.solid_tones.length > 4
        || new Set(grid.solid_tones).size !== grid.solid_tones.length
        || grid.solid_tones.some((tone) => !isBoundedInt(tone, 1, 4)))) {
      errors.push("runtime/widget.json scene.grid.solid_tones 必须是唯一的 1..4 色阶");
    }
  }
  const entities = Array.isArray(scene.entities) ? scene.entities : [];
  if (entities.length < 1 || entities.length > COMPONENT_SCENE_MAX_ENTITIES) {
    errors.push(`runtime/widget.json scene.entities 必须为 1-${COMPONENT_SCENE_MAX_ENTITIES} 项`);
  }
  const entityIds = new Set();
  entities.forEach((entity, index) => {
    const label = `runtime/widget.json scene.entities[${index}]`;
    if (!hasOnlyKeys(entity, [
      "id", "x", "y", "width", "height", "tone", "vx", "vy",
      "bounds", "shape", "active", "collidable",
    ])) {
      errors.push(`${label} 含未知字段或不是对象`);
      return;
    }
    if (typeof entity.id !== "string" || !/^[A-Za-z0-9_.-]{1,15}$/.test(entity.id)
        || entityIds.has(entity.id)) {
      errors.push(`${label}.id 无效或重复`);
    } else entityIds.add(entity.id);
    const width = entity.width ?? 1;
    const height = entity.height ?? 1;
    if (!isBoundedInt(entity.x, 0, 15) || !isBoundedInt(entity.y, 0, 15)
        || !isBoundedInt(width, 1, 8) || !isBoundedInt(height, 1, 8)
        || (grid?.width && entity.x + width > grid.width)
        || (grid?.height && entity.y + height > grid.height)
        || (entity.tone !== undefined && !isBoundedInt(entity.tone, 1, 4))
        || (entity.vx !== undefined && !isBoundedInt(entity.vx, -4, 4))
        || (entity.vy !== undefined && !isBoundedInt(entity.vy, -4, 4))) {
      errors.push(`${label} 的位置、尺寸、色阶或速度越界`);
    }
    if (entity.bounds !== undefined
        && !["clamp", "wrap", "bounce", "hide", "stop"].includes(entity.bounds)) {
      errors.push(`${label}.bounds 不受支持`);
    }
    if (entity.shape !== undefined && !COMPONENT_SCENE_SHAPES.includes(entity.shape)) {
      errors.push(`${label}.shape 不受支持`);
    }
    if ((entity.active !== undefined && typeof entity.active !== "boolean")
        || (entity.collidable !== undefined && typeof entity.collidable !== "boolean")) {
      errors.push(`${label} active/collidable 必须是布尔值`);
    }
  });
  const transitionActions = new Set(
    Array.isArray(widget.transitions) ? widget.transitions.map((item) => item?.on) : [],
  );
  const buttonActions = new Set(Array.isArray(buttons) ? buttons.map((item) => item?.action) : []);
  const rules = Array.isArray(scene.rules) ? scene.rules : [];
  if (rules.length < 1 || rules.length > COMPONENT_SCENE_MAX_RULES) {
    errors.push(`runtime/widget.json scene.rules 必须为 1-${COMPONENT_SCENE_MAX_RULES} 项`);
  }
  rules.forEach((rule, index) => {
    const label = `runtime/widget.json scene.rules[${index}]`;
    if (!hasOnlyKeys(rule, ["on", "entity", "with", "edge", "do"])) {
      errors.push(`${label} 含未知字段或不是对象`);
      return;
    }
    if (rule.on === "collision") {
      if (!entityIds.has(rule.entity) || !entityIds.has(rule.with) || rule.entity === rule.with) {
        errors.push(`${label} collision 需要两个不同的已声明实体`);
      }
    } else if (["edge", "blocked"].includes(rule.on)) {
      if (!entityIds.has(rule.entity)) errors.push(`${label} 需要已声明实体`);
      if (rule.on === "blocked" && rule.edge !== undefined) {
        errors.push(`${label} blocked 不接受 edge`);
      } else if (rule.edge !== undefined
          && !["any", "left", "right", "top", "bottom"].includes(rule.edge)) {
        errors.push(`${label}.edge 不受支持`);
      }
    } else if (rule.on !== "tick") {
      if (typeof rule.on !== "string" || !/^[A-Za-z0-9_.-]{1,47}$/.test(rule.on)
          || !transitionActions.has(rule.on) || !buttonActions.has(rule.on)) {
        errors.push(`${label}.on 必须同时匹配 transition 与 button 动作`);
      }
    }
    if (!Array.isArray(rule.do) || rule.do.length < 1
        || rule.do.length > COMPONENT_SCENE_MAX_OPS) {
      errors.push(`${label}.do 必须为 1-${COMPONENT_SCENE_MAX_OPS} 项`);
    } else {
      rule.do.forEach((op, opIndex) => validateSceneOp(op, entityIds, errors, `${label}.do[${opIndex}]`));
    }
  });
}

function validateComponentGame(widget, buttons, errors) {
  if (widget?.engine !== undefined && !COMPONENT_RUNTIME_ENGINES.includes(widget.engine)) {
    errors.push(`runtime/widget.json engine 不支持 ${String(widget.engine || "")}`);
  }
  if (widget?.scene !== undefined && widget?.game !== undefined) {
    errors.push("runtime/widget.json scene 与旧版 game 不能同时存在");
  }
  validateBoundedScene(widget, buttons, errors);
  validateLegacyGame(widget, buttons, errors);
}

function validateWidgetVars(widget, errors) {
  if (!widget || typeof widget !== "object" || Array.isArray(widget)) {
    errors.push("runtime/widget.json 必须是 JSON 对象");
    return;
  }
  if (
    !widget.vars
    || typeof widget.vars !== "object"
    || Array.isArray(widget.vars)
  ) {
    errors.push("runtime/widget.json 的 vars 必须是以变量名为键的 JSON 对象；无变量时请使用 {}");
    return;
  }
  const entries = Object.entries(widget.vars);
  if (entries.length > P4_WIDGET_MAX_VARS) {
    errors.push(`runtime/widget.json.vars 最多允许 ${P4_WIDGET_MAX_VARS} 个变量`);
  }
  entries.forEach(([name, declaration]) => {
    const label = `runtime/widget.json.vars.${name}`;
    if (!/^[A-Za-z0-9_.-]{1,31}$/.test(name)
        || utf8Bytes(name) > P4_WIDGET_VAR_NAME_MAX_BYTES) {
      errors.push(`${label} 的变量名无效`);
      return;
    }
    if (!isObject(declaration)) {
      errors.push(`${label} 必须是对象`);
      return;
    }
    const unsupported = Object.keys(declaration).find((key) => !["type", "init"].includes(key));
    if (unsupported) {
      errors.push(`${label} 含固件不支持的字段 ${unsupported}；只允许 type 和 init`);
      return;
    }
    if (!["int", "string"].includes(declaration.type)) {
      errors.push(`${label}.type 只能是 int 或 string`);
      return;
    }
    if (!Object.hasOwn(declaration, "init")) return;
    if (declaration.type === "int") {
      if (!Number.isInteger(declaration.init)
          || declaration.init < P4_WIDGET_INT_MIN
          || declaration.init > P4_WIDGET_INT_MAX) {
        errors.push(`${label}.init 必须是 ${P4_WIDGET_INT_MIN}..${P4_WIDGET_INT_MAX} 的整数`);
      }
    } else if (typeof declaration.init !== "string"
        || utf8Bytes(declaration.init) > P4_WIDGET_STRING_VAR_MAX_BYTES) {
      errors.push(`${label}.init 必须是最多 ${P4_WIDGET_STRING_VAR_MAX_BYTES} 个 UTF-8 字节的字符串`);
    }
  });
}

function widgetEffectCount(rule) {
  if (!rule || typeof rule !== "object" || Array.isArray(rule)) return 0;
  const setCount = rule.set && typeof rule.set === "object" && !Array.isArray(rule.set)
    ? Object.keys(rule.set).length
    : 0;
  const incCount = rule.inc && typeof rule.inc === "object" && !Array.isArray(rule.inc)
    ? Object.keys(rule.inc).length
    : 0;
  return setCount + incCount;
}

function validateWidgetEffectBounds(widget, errors) {
  if (!widget || typeof widget !== "object" || Array.isArray(widget)) return;
  const checkRules = (rules, label, nested = false) => {
    if (!Array.isArray(rules)) return;
    rules.forEach((rule, index) => {
      const target = nested ? rule?.then : rule;
      const count = widgetEffectCount(target);
      if (count > P4_WIDGET_MAX_EFFECTS) {
        errors.push(
          `runtime/widget.json ${label}[${index}] 的 set+inc 共 ${count} 项，超过 P4 ${P4_WIDGET_MAX_EFFECTS} 项上限`,
        );
      }
    });
  };
  checkRules(widget.transitions, "transitions");
  checkRules(widget.tick, "tick");
  checkRules(widget.tick, "tick.then", true);
}

export function validateClawpkgManifest(manifest) {
  const errors = [];
  for (const file of CLAWPKG_FILES) {
    if (!(file.name in (manifest || {}))) errors.push(`缺少 ${file.name}`);
  }
  const meta = manifest?.["component.json"];
  if ("component.json" in (manifest || {}) && !(meta?.id && meta?.name && meta?.version)) {
    errors.push("component.json 必须含 id、name、version");
  }
  if (meta?.id && !COMPONENT_ID_PATTERN.test(meta.id)) {
    errors.push("component.json id 必须为 1-47 位小写 ASCII 标识（字母开头，仅 a-z、0-9、-、_）");
  }
  if (
    meta?.kind !== undefined
    && !COMPONENT_KINDS.includes(meta.kind)
  ) {
    errors.push(`component.json kind 只支持 ${COMPONENT_KINDS.join("/")}`);
  }
  if ("buttons.json" in (manifest || {})) {
    if (compactJsonBytes(manifest["buttons.json"]) > P4_BUTTONS_JSON_MAX_BYTES) {
      errors.push(`buttons.json 压缩后超过 P4 ${P4_BUTTONS_JSON_MAX_BYTES} 字节上限`);
    }
    validateComponentButtons(manifest["buttons.json"], errors);
  }
  if ("runtime/widget.json" in (manifest || {})) {
    if (compactJsonBytes(manifest["runtime/widget.json"]) > P4_WIDGET_JSON_MAX_BYTES) {
      errors.push(`runtime/widget.json 压缩后超过 P4 ${P4_WIDGET_JSON_MAX_BYTES} 字节上限`);
    }
    validateWidgetVars(manifest["runtime/widget.json"], errors);
    validateComponentGame(
      manifest["runtime/widget.json"],
      manifest["buttons.json"],
      errors,
    );
    validateWidgetEffectBounds(manifest["runtime/widget.json"], errors);
  }
  const dashboard = manifest?.["negative-screen.json"]?.dashboard;
  if (dashboard) {
    for (const [slot, value] of Object.entries(dashboard)) {
      const def = SLOT_BY_ID.get(slot);
      if (!def) {
        errors.push(`negative-screen.json 含未知槽位 ${slot}`);
      } else if (utf8Bytes(value) > def.maxBytes) {
        errors.push(`槽位 ${slot} 超出 ${def.maxBytes} 字节上限`);
      } else if (
        slot in COMPONENT_VISUAL_PRESETS
        && !COMPONENT_VISUAL_PRESETS[slot].includes(value)
      ) {
        errors.push(`槽位 ${slot} 含未知视觉预置 ${value}`);
      }
    }
  }
  return { valid: errors.length === 0, errors };
}
