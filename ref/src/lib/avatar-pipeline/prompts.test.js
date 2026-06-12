/**
 * [Input] Avatar pipeline family definitions and prompt builders.
 * [Output] Node regression coverage for distinct subject-adaptive motion grammar, first/last frame prompts, marker props, and touch-screen feedback prompts.
 * [Pos] test node in ref/src/lib/avatar-pipeline
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { FAMILIES } from "./families.js";
import { buildDryRunResponse, buildUserPrompt } from "./prompts.js";
import { SYSTEM_PROMPT_ZH } from "./system-prompt.js";

function family(id) {
  const item = FAMILIES.find((entry) => entry.family === id);
  assert.ok(item, `Expected family definition for ${id}`);
  return item;
}

test("welcome is authored as a single-clip greeting that still returns to the uploaded frame", () => {
  const welcome = family("welcome");

  assert.equal(welcome.playback, "one_shot_entry");
  assert.match(welcome.spatial_path, /edge|left|right|center/i);
  assert.match(welcome.pose_signature, /greeting|step|peek|slide|settle/i);
  assert.match(welcome.avoid_motion, /seated|stationary|idle/i);
  assert.match(welcome.prop_policy, /optional|flower|spark|sign/i);

  const dryRunPrompt = buildDryRunResponse({
    families: [welcome],
    sourceImageName: "cat-reference.png",
  }).prompts[0].prompt;

  assert.match(dryRunPrompt, /one-shot greeting/i);
  assert.match(dryRunPrompt, /first and last frame reference/i);
  assert.match(dryRunPrompt, /return to the exact uploaded pose/i);
  assert.match(dryRunPrompt, /required small marker prop/i);
  assert.match(dryRunPrompt, /Start from the exact uploaded pose/);
  assert.doesNotMatch(dryRunPrompt, /may begin off-screen|Start off-screen|from outside the frame/i);
  assert.match(SYSTEM_PROMPT_ZH, /first_frame 和 last_frame/);
});

test("core visible states carry different spatial paths and silhouette signatures", () => {
  const welcome = family("welcome");
  const playing = family("idle.playing");
  const wandering = family("idle.wandering");

  for (const item of [welcome, playing, wandering]) {
    assert.ok(item.spatial_path, `${item.family} needs a spatial path`);
    assert.ok(item.pose_signature, `${item.family} needs a silhouette signature`);
    assert.ok(item.avoid_motion, `${item.family} needs explicit anti-collapse guidance`);
  }

  assert.equal(new Set([welcome.pose_signature, playing.pose_signature, wandering.pose_signature]).size, 3);
  assert.match(playing.spatial_path, /dash|screen-left|screen-right|left-to-right/i);
  assert.match(wandering.spatial_path, /side|arc|wander|left|right/i);
  assert.match(playing.prop_policy, /butterfly|toy|leaf/i);
  assert.match(wandering.prop_policy, /bird|flower|leaf/i);
  assert.match(playing.avoid_motion, /welcome|wandering|same sitting/i);
  assert.match(wandering.avoid_motion, /welcome|playing|same sitting/i);
});

test("thinking prompt warns the model not to collapse different families into the same pose", () => {
  const userPrompt = buildUserPrompt({
    families: [family("welcome"), family("idle.playing"), family("idle.wandering")],
    appearanceName: "kitty",
  });

  assert.match(userPrompt, /Do not collapse different families into the same sitting or standing pose/);
  assert.match(userPrompt, /one_shot_entry/);
  assert.match(userPrompt, /spatial_path/);
  assert.match(userPrompt, /pose_signature/);
  assert.match(userPrompt, /prop_policy/);
  assert.match(userPrompt, /Each prompt must include exactly one small marker prop/);
  assert.match(userPrompt, /touch-screen feedback variants/);
  assert.match(SYSTEM_PROMPT_ZH, /不同 family/);
  assert.match(SYSTEM_PROMPT_ZH, /身体轮廓/);
  assert.match(SYSTEM_PROMPT_ZH, /道具/);
  assert.match(SYSTEM_PROMPT_ZH, /welcome/);
  assert.match(SYSTEM_PROMPT_ZH, /touch\.lick 和 touch\.what/);
});

test("each family requires a visible marker prop instead of relying on generic motion", () => {
  for (const item of FAMILIES) {
    assert.match(item.prop_policy, /required/i, `${item.family} should require a marker prop`);
    assert.doesNotMatch(item.prop_policy, /^No prop/i, `${item.family} should not forbid marker props`);
  }

  const dryRunPrompt = buildDryRunResponse({
    families: [family("working")],
    sourceImageName: "pet.png",
  }).prompts[0].prompt;

  assert.match(dryRunPrompt, /Required small marker prop policy/i);
  assert.match(dryRunPrompt, /keyboard|mouse|open book|page/i);
});

test("requested state prompts encode concrete props and stronger screen travel", () => {
  const playing = family("idle.playing");
  const wandering = family("idle.wandering");
  const working = family("working");
  const waiting = family("waiting_user");
  const done = family("done");

  assert.match(playing.spatial_path, /screen-left|screen-right|left-to-right/i);
  assert.match(wandering.spatial_path, /screen-left|screen-right|right-to-left/i);
  assert.match(playing.prop_policy, /butterfly|toy|ribbon/i);
  assert.match(wandering.prop_policy, /bird|flower|leaf/i);
  assert.match(working.motion_brief, /keyboard|mouse|page|typing/i);
  assert.match(working.prop_policy, /keyboard|mouse|open book|page/i);
  assert.doesNotMatch(working.prop_policy, /progress gear|focus sparkle|cursor block/i);
  assert.match(waiting.prop_policy, /choice|question|option/i);
  assert.match(waiting.motion_brief, /large|clear|raise|lean|beckon/i);
  assert.match(waiting.spatial_path, /side-to-side|forward|screen-left|screen-right|travel/i);
  assert.match(waiting.pose_signature, /subject-appropriate|arm|hand|paw|wing|body/i);
  assert.doesNotMatch(waiting.spatial_path, /No travel/i);
  assert.doesNotMatch(waiting.pose_signature, /still paws/i);
  assert.match(done.prop_policy, /finish|bell|check/i);
  assert.match(done.motion_brief, /ring|bell|finish/i);
});

test("custom generation families use one working clip plus separate decide state", () => {
  const ids = new Set(FAMILIES.map((item) => item.family));

  assert.ok(ids.has("working"));
  assert.ok(ids.has("waiting_user"));
  assert.equal(ids.has("working.thinking"), false);
  assert.equal(ids.has("working.typing"), false);
  assert.equal(ids.has("working.browsing"), false);
  assert.equal(ids.has("working.default"), false);
  assert.equal(ids.has("thinking"), false);
  assert.equal(ids.has("tool_running"), false);
});

test("prompt contract explains single custom working and built-in variant compatibility", () => {
  const userPrompt = buildUserPrompt({
    families: [family("working"), family("waiting_user")],
    appearanceName: "terrier",
  });

  assert.match(userPrompt, /Custom generation should output a single working family clip/i);
  assert.match(userPrompt, /Built-in appearances may provide working\.thinking, working\.typing, and working\.browsing/i);
  assert.match(userPrompt, /runtime treats both working and working\.\* clips as the same working state/i);
  assert.match(userPrompt, /randomly chooses only when more than one working clip is present/i);
  assert.match(userPrompt, /waiting_user is the separate decide\/user-choice state/i);
  assert.match(SYSTEM_PROMPT_ZH, /自定义生成只需要输出一个 working family/);
  assert.match(SYSTEM_PROMPT_ZH, /内置形象可以提供 working\.thinking、working\.typing、working\.browsing/);
  assert.match(SYSTEM_PROMPT_ZH, /只有存在多个 working clip 时才随机选择/);
  assert.match(SYSTEM_PROMPT_ZH, /waiting_user 是单独的 decide/);
});

test("thinking prompt requires subject-adaptive anatomy and stronger middle motion", () => {
  const userPrompt = buildUserPrompt({
    families: [family("waiting_user")],
    appearanceName: "reference subject",
  });

  assert.match(userPrompt, /Identify the uploaded image subject_type/i);
  assert.match(userPrompt, /human/i);
  assert.match(userPrompt, /cat/i);
  assert.match(userPrompt, /subject-appropriate anatomy/i);
  assert.match(userPrompt, /medium-to-large visible middle motion/i);
  assert.match(userPrompt, /Do not write paws for a human/i);
  assert.match(SYSTEM_PROMPT_ZH, /subject_type/);
  assert.match(SYSTEM_PROMPT_ZH, /人物|人类/);
  assert.match(SYSTEM_PROMPT_ZH, /猫|狗/);
  assert.match(SYSTEM_PROMPT_ZH, /中段动作幅度/);

  const dryRunPrompt = buildDryRunResponse({
    families: [family("waiting_user")],
    sourceImageName: "human-reference.png",
  }).prompts[0].prompt;

  assert.match(dryRunPrompt, /subject-adaptive anatomy/i);
  assert.match(dryRunPrompt, /medium-to-large visible middle motion/i);
});

test("touch lick and what are authored as screen-touch close-up reactions", () => {
  const lick = family("touch.lick");
  const what = family("touch.what");

  for (const item of [lick, what]) {
    const combined = [
      item.label,
      item.motion_brief,
      item.spatial_path,
      item.pose_signature,
      item.prop_policy,
      item.avoid_motion,
      item.variation_hint,
    ].join(" ");

    assert.match(combined, /touch|screen|viewer|close|foreground|face/i);
    assert.match(combined, /tap ripple|screen smudge|glass|fingerprint/i);
    assert.match(item.avoid_motion, /legacy family name|literal/i);
    assert.doesNotMatch(combined, /tongue|lick-like|question mark|speech bubble|saliva/i);
  }

  assert.match(lick.motion_brief, /close|foreground|screen/i);
  assert.match(what.motion_brief, /close|foreground|screen/i);
  assert.match(lick.avoid_motion, /screen-contact feedback/i);
  assert.match(what.avoid_motion, /screen-contact feedback/i);

  const dryRun = buildDryRunResponse({
    families: [lick, what],
    sourceImageName: "pet.png",
  });
  const promptText = dryRun.prompts.map((item) => item.prompt).join("\n");

  assert.match(promptText, /touch-screen|screen glass|foreground/i);
  assert.doesNotMatch(promptText, /touch\.lick|touch\.what|lick-like|question mark|speech bubble|saliva/i);
});
