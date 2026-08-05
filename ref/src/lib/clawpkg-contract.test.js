/**
 * [Input] Frontend .clawpkg structure, dashboard-slot, and per-component button constraints.
 * [Output] Node coverage for required package files, optional safe slots, binding count,
 *          UTF-8 label size, vars object shape, allowed events, and overlapping input rejection.
 * [Pos] contract test node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  CLAWPKG_FILES,
  COMPONENT_BUTTON_EVENTS,
  COMPONENT_BUTTON_LABEL_MAX_BYTES,
  COMPONENT_BUTTON_MAX_BINDINGS,
  COMPONENT_DASHBOARD_V1_SLOTS,
  COMPONENT_GAME_TYPES,
  COMPONENT_ID_PATTERN,
  COMPONENT_KINDS,
  COMPONENT_RUNTIME_ENGINES,
  COMPONENT_SCENE_ENGINE,
  COMPONENT_SCENE_SHAPES,
  COMPONENT_VISUAL_PRESETS,
  P4_BUTTONS_JSON_MAX_BYTES,
  P4_WIDGET_MAX_EFFECTS,
  P4_WIDGET_JSON_MAX_BYTES,
  validateClawpkgManifest,
} from "./clawpkg-contract.js";

test("clawpkg 六件套结构齐全", () => {
  assert.deepEqual(
    CLAWPKG_FILES.map((f) => f.name),
    ["component.json", "negative-screen.json", "buttons.json", "runtime/", "assets/", "share.json"],
  );
});

test("COMPONENT_DASHBOARD_V1 暴露 10 个内容槽位和 4 个安全视觉槽位", () => {
  const ids = COMPONENT_DASHBOARD_V1_SLOTS.map((s) => s.id);
  assert.deepEqual(ids, [
    "title", "eyebrow", "headline", "metricLabel", "metricValue", "metricUnit",
    "badge", "note", "footer", "progress",
    "visualStyle", "visualPalette", "visualLayout", "visualSprite",
  ]);
  assert.ok(COMPONENT_DASHBOARD_V1_SLOTS.every((s) => Number.isInteger(s.maxBytes) && s.maxBytes > 0));
  assert.deepEqual(COMPONENT_VISUAL_PRESETS.visualStyle, ["classic", "pixel", "clean"]);
  assert.ok(COMPONENT_VISUAL_PRESETS.visualPalette.includes("candy"));
  assert.ok(COMPONENT_VISUAL_PRESETS.visualPalette.includes("ocean"));
  assert.ok(COMPONENT_VISUAL_PRESETS.visualPalette.includes("mono"));
  assert.ok(COMPONENT_VISUAL_PRESETS.visualLayout.includes("tool"));
  assert.ok(COMPONENT_VISUAL_PRESETS.visualSprite.includes("coffee"));
  assert.ok(COMPONENT_VISUAL_PRESETS.visualSprite.includes("timer"));
  assert.ok(COMPONENT_VISUAL_PRESETS.visualSprite.includes("droplet"));
  assert.ok(COMPONENT_VISUAL_PRESETS.visualSprite.includes("gauge"));
  assert.ok(COMPONENT_VISUAL_PRESETS.visualSprite.includes("blocks"));
  assert.ok(COMPONENT_VISUAL_PRESETS.visualSprite.includes("snake"));
  assert.ok(COMPONENT_VISUAL_PRESETS.visualSprite.includes("flappy"));
  assert.ok(COMPONENT_VISUAL_PRESETS.visualSprite.includes("mole-ready"));
  assert.ok(COMPONENT_VISUAL_PRESETS.visualSprite.includes("mole-center"));
  assert.ok(COMPONENT_VISUAL_PRESETS.visualSprite.includes("mole-right"));
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

  const pixelGame = validateClawpkgManifest({
    "component.json": { id: "pixel-game", name: "像素游戏", version: "1.0.0" },
    "negative-screen.json": {
      dashboard: {
        title: "点击挑战",
        visualStyle: "pixel",
        visualPalette: "candy",
        visualLayout: "arcade",
        visualSprite: "target",
      },
    },
    "buttons.json": [], "runtime/": {}, "assets/": {}, "share.json": { title: "像素游戏" },
  });
  assert.equal(pixelGame.valid, true);

  const unsafeVisual = validateClawpkgManifest({
    "component.json": { id: "unsafe-game", name: "不安全游戏", version: "1.0.0" },
    "negative-screen.json": { dashboard: { visualStyle: "custom-css" } },
    "buttons.json": [], "runtime/": {}, "assets/": {}, "share.json": { title: "不安全游戏" },
  });
  assert.equal(unsafeVisual.valid, false);
  assert.match(unsafeVisual.errors.join(" "), /未知视觉预置/);
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

test("component.json 可显式声明 game/tool，旧包不声明 kind 仍兼容", () => {
  assert.deepEqual(COMPONENT_KINDS, ["game", "tool"]);
  const base = {
    "negative-screen.json": { dashboard: { title: "组件" } },
    "buttons.json": [],
    "runtime/": {},
    "assets/": {},
    "share.json": {},
  };
  for (const kind of [...COMPONENT_KINDS, undefined]) {
    const result = validateClawpkgManifest({
      ...base,
      "component.json": {
        id: `kind-${kind || "legacy"}`,
        name: "组件",
        version: "1.0.0",
        ...(kind ? { kind } : {}),
      },
    });
    assert.equal(result.valid, true);
  }
  const invalid = validateClawpkgManifest({
    ...base,
    "component.json": {
      id: "kind-invalid",
      name: "组件",
      version: "1.0.0",
      kind: "dashboard",
    },
  });
  assert.equal(invalid.valid, false);
  assert.match(invalid.errors.join(" "), /kind 只支持 game\/tool/);
});

test("component.json id 与 Linux/P4 安装删除合约一致", () => {
  assert.equal(COMPONENT_ID_PATTERN.test("token-usage"), true);
  assert.equal(COMPONENT_ID_PATTERN.test("Token.Usage"), false);
  const base = {
    "negative-screen.json": { dashboard: { title: "组件" } },
    "buttons.json": [],
    "runtime/": {},
    "assets/": {},
    "share.json": {},
  };
  for (const id of ["Uppercase", "has.dot", "1-leading", "-leading", `a${"b".repeat(47)}`]) {
    const result = validateClawpkgManifest({
      ...base,
      "component.json": { id, name: "组件", version: "1.0.0" },
    });
    assert.equal(result.valid, false);
    assert.match(result.errors.join(" "), /1-47 位小写 ASCII 标识/);
  }
});

test("P4 JSON 使用压缩后字节数校验固定接收缓冲区", () => {
  assert.equal(P4_WIDGET_JSON_MAX_BYTES, 4095);
  assert.equal(P4_BUTTONS_JSON_MAX_BYTES, 2047);
  const result = validateClawpkgManifest({
    "component.json": { id: "oversized", name: "Oversized", version: "1.0.0" },
    "negative-screen.json": { dashboard: { title: "Oversized" } },
    "buttons.json": [],
    "runtime/": {},
    "runtime/widget.json": {
      schema_version: 1,
      oversized: "x".repeat(P4_WIDGET_JSON_MAX_BYTES),
    },
    "assets/": {},
    "share.json": { title: "Oversized" },
  });

  assert.equal(result.valid, false);
  assert.match(result.errors.join(" "), /runtime\/widget\.json 压缩后超过 P4 4095 字节上限/);
});

test("runtime vars 必须始终是对象，无变量时使用空对象", () => {
  const base = {
    "component.json": { id: "vars-shape", name: "Vars Shape", version: "1.0.0" },
    "negative-screen.json": { dashboard: { title: "Vars Shape" } },
    "buttons.json": [],
    "runtime/": {},
    "assets/": {},
    "share.json": { title: "Vars Shape" },
  };

  assert.equal(validateClawpkgManifest({
    ...base,
    "runtime/widget.json": { schema_version: 1, vars: {} },
  }).valid, true);
  for (const vars of [undefined, null, [], ["count"]]) {
    const runtime = { schema_version: 1 };
    if (vars !== undefined) runtime.vars = vars;
    const result = validateClawpkgManifest({
      ...base,
      "runtime/widget.json": runtime,
    });
    assert.equal(result.valid, false);
    assert.match(result.errors.join(" "), /vars 必须是以变量名为键的 JSON 对象/);
  }
});

test("P4 每条 transition、tick 与 tick.then 最多允许 4 个 effect", () => {
  assert.equal(P4_WIDGET_MAX_EFFECTS, 4);
  const base = {
    "component.json": { id: "effects", name: "Effects", version: "1.0.0" },
    "negative-screen.json": { dashboard: { title: "Effects" } },
    "buttons.json": [],
    "runtime/": {},
    "assets/": {},
    "share.json": { title: "Effects" },
  };
  const result = validateClawpkgManifest({
    ...base,
    "runtime/widget.json": {
      schema_version: 1,
      transitions: [{
        from: "*",
        on: "game.restart",
        set: { a: 0, b: 0, c: 0 },
        inc: { d: 1, e: 1 },
      }],
      tick: [{
        every_ms: 1000,
        set: { a: 0, b: 0, c: 0, d: 0, e: 0 },
        then: { set: { a: 0, b: 0, c: 0 }, inc: { d: 1, e: 1 } },
      }],
    },
  });

  assert.equal(result.valid, false);
  assert.match(result.errors.join(" "), /transitions\[0\].*5 项.*P4 4 项上限/);
  assert.match(result.errors.join(" "), /tick\[0\].*5 项.*P4 4 项上限/);
  assert.match(result.errors.join(" "), /tick\.then\[0\].*5 项.*P4 4 项上限/);
});

test("组件按钮契约限制 8 个动作、唯一事件/action 与 30 字节标签", () => {
  assert.equal(COMPONENT_BUTTON_MAX_BINDINGS, 8);
  assert.equal(COMPONENT_BUTTON_LABEL_MAX_BYTES, 30);
  assert.ok(COMPONENT_BUTTON_EVENTS.includes("screen.region.tap"));
  assert.ok(COMPONENT_BUTTON_EVENTS.includes("button.sw3.short_press"));
  assert.equal(COMPONENT_BUTTON_EVENTS.includes("button.sw3.long_press"), false);
  assert.ok(COMPONENT_BUTTON_EVENTS.includes("button.encoder.long_press"));
  assert.ok(COMPONENT_BUTTON_EVENTS.includes("knob.rotate_ccw"));
  assert.ok(COMPONENT_BUTTON_EVENTS.includes("joystick.up"));
  assert.ok(COMPONENT_BUTTON_EVENTS.includes("joystick.down"));

  const base = {
    "component.json": { id: "game", name: "小游戏", version: "1.0.0" },
    "negative-screen.json": { dashboard: { title: "小游戏" } },
    "runtime/": {},
    "assets/": {},
    "share.json": { title: "小游戏" },
  };
  const duplicate = validateClawpkgManifest({
    ...base,
    "buttons.json": [
      { action: "game.start", control: "SW1", event: "button.sw1.short_press", label: "开始" },
      { action: "game.retry", control: "SW1", event: "button.sw1.short_press", label: "重试" },
    ],
  });
  assert.equal(duplicate.valid, false);
  assert.match(duplicate.errors.join(" "), /与已有绑定冲突/);

  const duplicateAction = validateClawpkgManifest({
    ...base,
    "buttons.json": [
      { action: "game.score", control: "SW1", event: "button.sw1.short_press", label: "左键得分" },
      { action: "game.score", control: "SW2", event: "button.sw2.short_press", label: "中键得分" },
    ],
  });
  assert.equal(duplicateAction.valid, false);
  assert.match(duplicateAction.errors.join(" "), /动作 game\.score 重复/);

  const overlappingRotation = validateClawpkgManifest({
    ...base,
    "buttons.json": [
      { action: "game.adjust", control: "前方旋钮", event: "knob.rotate_cw / knob.rotate_ccw", label: "调整" },
      { action: "game.next", control: "前方旋钮", event: "knob.rotate_cw", label: "下一个" },
    ],
  });
  assert.equal(overlappingRotation.valid, false);
  assert.match(overlappingRotation.errors.join(" "), /与已有绑定冲突/);

  const crowded = validateClawpkgManifest({
    ...base,
    "buttons.json": [
      {
        action: "game.start",
        control: "SW1",
        event: "button.sw1.short_press",
        label: "这是一个明显超过十个汉字的按钮提示",
      },
    ],
  });
  assert.equal(crowded.valid, false);
  assert.match(crowded.errors.join(" "), /30 字节/);

  const tooMany = validateClawpkgManifest({
    ...base,
    "buttons.json": Array.from({ length: 9 }, (_, index) => ({
      action: `game.action_${index}`,
      control: "SW1",
      event: index === 8 ? "button.sw1.short_press" : COMPONENT_BUTTON_EVENTS[index],
      label: `动作${index}`,
    })),
  });
  assert.equal(tooMany.valid, false);
  assert.match(tooMany.errors.join(" "), /最多允许 8/);
});

test("P4 动态游戏只接受有界 blocks/snake/flappy 配置并校验动作对齐", () => {
  assert.deepEqual(COMPONENT_GAME_TYPES, ["blocks", "snake", "flappy"]);
  const buttons = [
    { action: "snake.start", control: "前方旋钮", event: "button.encoder.short_press", label: "开始" },
    { action: "snake.left", control: "前方旋钮", event: "knob.rotate_ccw", label: "左转" },
    { action: "snake.right", control: "前方旋钮", event: "knob.rotate_cw", label: "右转" },
  ];
  const widget = {
    schema_version: 1,
    vars: { score: { type: "int", init: 0 } },
    states: ["ready", "playing", "result"],
    initial_state: "ready",
    transitions: [
      { from: "ready", on: "snake.start", to: "playing" },
      { from: "playing", on: "snake.left" },
      { from: "playing", on: "snake.right" },
    ],
    game: {
      type: "snake",
      tick_ms: 220,
      playing_state: "playing",
      result_state: "result",
      score_var: "score",
      actions: { start: "snake.start", left: "snake.left", right: "snake.right" },
    },
    dashboard: {},
  };
  const base = {
    "component.json": { id: "snake", name: "Snake", version: "1.0.0" },
    "negative-screen.json": { dashboard: { title: "Snake" } },
    "buttons.json": buttons,
    "runtime/": {},
    "runtime/widget.json": widget,
    "assets/": {},
    "share.json": { title: "Snake" },
  };
  assert.equal(validateClawpkgManifest(base).valid, true);

  const flappy = validateClawpkgManifest({
    ...base,
    "component.json": { id: "flappy", name: "Flappy", version: "1.0.0", kind: "game" },
    "buttons.json": [
      { action: "flappy.flap", control: "SW1", event: "button.sw1.short_press", label: "拍翅" },
    ],
    "runtime/widget.json": {
      schema_version: 1,
      vars: { score: { type: "int", init: 0 } },
      states: ["ready", "playing", "result"],
      initial_state: "ready",
      transitions: [{ from: "*", on: "flappy.flap", to: "playing" }],
      game: {
        type: "flappy",
        tick_ms: 100,
        playing_state: "playing",
        result_state: "result",
        score_var: "score",
        actions: { flap: "flappy.flap" },
      },
      dashboard: {},
    },
  });
  assert.equal(flappy.valid, true, flappy.errors.join(" "));

  const invalid = validateClawpkgManifest({
    ...base,
    "runtime/widget.json": {
      ...widget,
      game: {
        ...widget.game,
        tick_ms: 20,
        actions: { ...widget.game.actions, right: "snake.missing", teleport: "snake.teleport" },
        script: "not-allowed",
      },
    },
  });
  assert.equal(invalid.valid, false);
  assert.match(invalid.errors.join(" "), /tick_ms/);
  assert.match(invalid.errors.join(" "), /actions\.right/);
  assert.match(invalid.errors.join(" "), /未知字段/);
  assert.match(invalid.errors.join(" "), /未知动作/);
});

test("runtime vars 在传输前拒绝固件不支持的声明字段和越界初值", () => {
  const base = {
    "component.json": { id: "strict-vars", name: "Strict Vars", version: "1.0.0" },
    "negative-screen.json": { dashboard: { title: "Strict Vars" } },
    "buttons.json": [],
    "runtime/": {},
    "assets/": {},
    "share.json": { title: "Strict Vars" },
  };
  const validateVars = (vars) => validateClawpkgManifest({
    ...base,
    "runtime/widget.json": { schema_version: 1, vars },
  });

  assert.equal(validateVars({ score: { type: "int", init: 0 } }).valid, true);
  assert.equal(validateVars({ title: { type: "string", init: "ready" } }).valid, true);
  const unsupported = validateVars({ score: { type: "int", init: 0, min: 0 } });
  assert.equal(unsupported.valid, false);
  assert.match(unsupported.errors.join(" "), /vars\.score.*min.*type.*init/);
  assert.equal(validateVars({ score: { type: "int", init: 0.5 } }).valid, false);
  assert.equal(validateVars({ title: { type: "string", init: "中".repeat(22) } }).valid, false);
});

test("统一运行时接受不依赖旧预设的通用 scene 游戏", () => {
  assert.deepEqual(COMPONENT_RUNTIME_ENGINES, ["p4-bounded-runtime-v3"]);
  assert.equal(COMPONENT_SCENE_ENGINE, "p4-grid-scene-v1");
  const buttons = [
    { action: "catch.start", control: "旋钮", event: "button.encoder.short_press", label: "开始" },
    { action: "catch.left", control: "旋钮", event: "knob.rotate_ccw", label: "左移" },
    { action: "catch.right", control: "旋钮", event: "knob.rotate_cw", label: "右移" },
  ];
  const widget = {
    schema_version: 1,
    engine: "p4-bounded-runtime-v3",
    vars: { score: { type: "int", init: 0 } },
    states: ["ready", "playing", "result"],
    initial_state: "ready",
    transitions: [
      { from: "*", on: "catch.start", to: "playing" },
      { from: "playing", on: "catch.left" },
      { from: "playing", on: "catch.right" },
    ],
    tick: [],
    scene: {
      tick_ms: 140,
      active_state: "playing",
      result_state: "result",
      score_var: "score",
      grid: { width: 12, height: 8 },
      entities: [
        { id: "player", x: 5, y: 7, width: 2, tone: 3, shape: "paddle" },
        { id: "star", x: 5, y: 0, tone: 2, vy: 1, bounds: "hide", shape: "star" },
      ],
      rules: [
        { on: "catch.start", do: [{ op: "restart" }] },
        { on: "catch.left", do: [{ op: "move", entity: "player", dx: -1, dy: 0 }] },
        { on: "catch.right", do: [{ op: "move", entity: "player", dx: 1, dy: 0 }] },
        {
          on: "collision",
          entity: "player",
          with: "star",
          do: [{ op: "score", add: 1 }, { op: "place", entity: "star", x: [0, 11], y: 0 }],
        },
      ],
    },
    dashboard: {},
  };
  const base = {
    "component.json": { id: "falling-catch", name: "Catch", version: "1.0.0", kind: "game" },
    "negative-screen.json": { dashboard: { title: "Catch" } },
    "buttons.json": buttons,
    "runtime/": {},
    "runtime/widget.json": widget,
    "assets/": {},
    "share.json": { title: "Catch" },
  };
  const valid = validateClawpkgManifest(base);
  assert.equal(valid.valid, true, valid.errors.join(" "));
  assert.ok(COMPONENT_SCENE_SHAPES.includes("player-ship"));
  const invalid = validateClawpkgManifest({
    ...base,
    "runtime/widget.json": {
      ...widget,
      scene: {
        ...widget.scene,
        rules: [{ on: "catch.left", do: [{ op: "teleport", entity: "player" }] }],
      },
    },
  });
  assert.equal(invalid.valid, false);
  assert.match(invalid.errors.join(" "), /teleport/);
  const invalidShape = validateClawpkgManifest({
    ...base,
    "runtime/widget.json": {
      ...widget,
      scene: {
        ...widget.scene,
        entities: [{ ...widget.scene.entities[0], shape: "copied-game-sprite" }],
      },
    },
  });
  assert.equal(invalidShape.valid, false);
  assert.match(invalidShape.errors.join(" "), /shape/);
});
