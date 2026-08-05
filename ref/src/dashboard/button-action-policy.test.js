/**
 * [Input] Pure button action policy helpers.
 * [Output] Regression coverage for unique custom actions, repeatable confirm navigation, and duplicate ownership.
 * [Pos] Test node in ref/src/dashboard.
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  enforceUniqueButtonActions,
  findButtonActionOwner,
} from "./button-action-policy.js";

const rows = [
  {
    id: "sw1_short",
    label: "SW1 短按",
    defaultAction: "disabled",
    actionOptions: ["agent_prompt", "page_enter", "disabled"],
  },
  {
    id: "sw2_short",
    label: "SW2 短按",
    defaultAction: "disabled",
    actionOptions: ["agent_prompt", "page_enter", "disabled"],
  },
  {
    id: "encoder_short",
    label: "旋钮短按",
    defaultAction: "page_enter",
    actionOptions: ["agent_prompt", "page_enter", "disabled"],
  },
];

test("non-disabled actions have one owner while disabled remains repeatable", () => {
  assert.deepEqual(
    enforceUniqueButtonActions(rows, {
      sw1_short: "agent_prompt",
      sw2_short: "agent_prompt",
      encoder_short: "page_enter",
    }),
    {
      sw1_short: "agent_prompt",
      sw2_short: "disabled",
      encoder_short: "page_enter",
    },
  );
});

test("confirm navigation may be shared by SW1 and joystick center short press", () => {
  assert.deepEqual(
    enforceUniqueButtonActions(rows, {
      sw1_short: "page_enter",
      sw2_short: "disabled",
      encoder_short: "page_enter",
    }),
    {
      sw1_short: "page_enter",
      sw2_short: "disabled",
      encoder_short: "page_enter",
    },
  );
  assert.equal(
    findButtonActionOwner(
      rows,
      { sw1_short: "page_enter", encoder_short: "page_enter" },
      "sw1_short",
      "page_enter",
    ),
    null,
  );
});

test("duplicate ownership identifies the gesture that already uses an action", () => {
  const owner = findButtonActionOwner(
    rows,
    {
      sw1_short: "agent_prompt",
      sw2_short: "disabled",
      encoder_short: "page_enter",
    },
    "sw2_short",
    "agent_prompt",
  );
  assert.equal(owner?.label, "SW1 短按");
  assert.equal(findButtonActionOwner(rows, {}, "sw2_short", "disabled"), null);
});
