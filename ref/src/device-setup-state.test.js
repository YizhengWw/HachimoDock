/**
 * [Input] The pure onboarding state machine exported by `./device-setup-state.js`.
 * [Output] Behavior coverage (bare node) for stepIndex phase→progress mapping and
 *          the reducer transitions the wizard depends on.
 * [Pos] test node in ref/src
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { INITIAL_STATE, reducer, stepIndex } from "./device-setup-state.js";

test("stepIndex maps each wizard phase to the right progress step", () => {
  assert.equal(stepIndex("idle"), 0);
  assert.equal(stepIndex("wait_user_input"), 1);
  assert.equal(stepIndex("ethernet_binding"), 2);
  assert.equal(stepIndex("choose_agent_appearance"), 3);
  assert.equal(stepIndex("completed"), 3);
  assert.equal(stepIndex("error"), 0);
  assert.equal(stepIndex("something-new"), 0);
});

test("reducer is pure and leaves the previous state untouched", () => {
  const next = reducer(INITIAL_STATE, { type: "set_password", value: "secret" });
  assert.equal(next.password, "secret");
  assert.equal(INITIAL_STATE.password, "");
  assert.notEqual(next, INITIAL_STATE);
});

test("set_result advances to the appearance-choice phase and records the attempt", () => {
  const next = reducer(
    { ...INITIAL_STATE, phase: "polling_result", error: "boom" },
    { type: "set_result", ip: "192.168.1.5", attempt: { id: 1 }, connectionMode: "ethernet" },
  );
  assert.equal(next.phase, "choose_agent_appearance");
  assert.equal(next.resultIp, "192.168.1.5");
  assert.equal(next.connectionMode, "ethernet");
  assert.equal(next.error, null);
});

test("set_completed finishes the wizard and clears the saving flag", () => {
  const next = reducer(
    { ...INITIAL_STATE, phase: "choose_agent_appearance", savingAgentAppearance: true },
    { type: "set_completed" },
  );
  assert.equal(next.phase, "completed");
  assert.equal(next.savingAgentAppearance, false);
});

test("set_error routes to error, and reset returns a fresh INITIAL_STATE", () => {
  const errored = reducer(INITIAL_STATE, { type: "set_error", error: "no-route", message: "配网失败" });
  assert.equal(errored.phase, "error");
  const reset = reducer(errored, { type: "reset" });
  assert.deepEqual(reset, INITIAL_STATE);
  assert.notEqual(reset, INITIAL_STATE);
});

test("unknown actions return the same state reference", () => {
  const state = { ...INITIAL_STATE, phase: "wait_user_input" };
  assert.equal(reducer(state, { type: "totally-unknown" }), state);
});
