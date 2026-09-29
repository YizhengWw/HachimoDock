import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { LOCAL_SOURCES, deadlineIso, localDateTime, todoMutation, todoDateLabel, todoHeaderDate } from "./local-widget-model.js";

test("todo display has only month/day/time while editor retains the full date", () => {
  assert.equal(todoDateLabel(deadlineIso("2026-09-25T14:30")), "09-25 14:30");
  assert.equal(todoDateLabel(null), "未设置截止时间");
  assert.equal(todoDateLabel("invalid"), "未设置截止时间");
  assert.equal(todoHeaderDate("2026-09-25"), "09-25");
  assert.equal(todoHeaderDate(null), "等待数据");
});

test("local widget dates preserve local time and explicit clearing", () => {
  const source = "2026-09-25T14:30";
  assert.equal(localDateTime(deadlineIso(source)), source);
  assert.equal(deadlineIso(""), "");
  assert.throws(() => deadlineIso("invalid"));
  assert.equal(localDateTime("invalid"), "");
});
test("todo mutations use stable identity and revision rather than title matching", () => {
  assert.deepEqual(todoMutation("delete", { id: "abc", revision: 4, title: "重复" }), { operation: "delete", id: "abc", revision: 4 });
  assert.deepEqual(todoMutation("add", null, { title: "买牛奶" }), { operation: "add", title: "买牛奶" });
  assert.deepEqual([...LOCAL_SOURCES], ["computer.status", "todos.upcoming"]);
});
test("both UI and realtime route through the same typed local tools", () => {
  const read = path => readFileSync(new URL(path, import.meta.url), "utf8");
  assert.match(read("./LocalDataPanel.jsx"), /local_tool_execute/);
  assert.match(read("../../src-tauri/src/persona_llm.rs"), /local_turn\.execute_async\(tool_name,input,read_only_batch\)\.await/);
  assert.match(read("../../src-tauri/src/lib.rs"), /local_tools::local_tool_execute/);
  assert.match(read("./LocalDataPanel.jsx"), /截止时间用于排序与展示，暂不提供到点提醒/);
  assert.doesNotMatch(read("./LocalDataPanel.jsx"), /最近待办/);
});
