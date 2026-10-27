/**
 * [Input] thinking-model JSON response and FAMILIES from this folder.
 * [Output] normalized prompt manifest plus subject-adaptive first/last-frame motion grammar used by video task submission.
 * [Pos] lib node in pc/src/lib/avatar-pipeline
 * [Sync] If this file changes, update this header.
 */

import { FAMILIES } from "./families.js";
import { parseStrictModelJson } from './model-json.js';
import { KEEP_CHIBI_STYLE_EN } from './character-style.js';

export function parseModelJson(raw) {
  return parseStrictModelJson(raw);
}

function normalizeFamilyId(value) {
  const family = String(value || "").trim();
  if (!family) throw new Error("family id is required");
  if (!/^[a-z0-9_.-]+$/i.test(family)) {
    throw new Error(`invalid family id: ${family}`);
  }
  return family;
}

export function normalizeFamilies(rawFamilies) {
  if (!Array.isArray(rawFamilies) || rawFamilies.length === 0) {
    throw new Error("families must be a non-empty array");
  }
  const seen = new Set();
  return rawFamilies.map((item) => {
    const family = normalizeFamilyId(typeof item === "string" ? item : item.family);
    if (seen.has(family)) throw new Error(`duplicate family definition: ${family}`);
    seen.add(family);
    return {
      family,
      label: String(item.label || item.state || family),
      playback: String(item.playback || "loop_state").trim(),
      motion_brief: String(item.motion_brief || item.notes || "").trim(),
      spatial_path: String(item.spatial_path || "").trim(),
      pose_signature: String(item.pose_signature || "").trim(),
      prop_policy: String(item.prop_policy || "").trim(),
      ...(item.preserve_reference_accessories === true ? {preserve_reference_accessories:true} : {}),
      avoid_motion: String(item.avoid_motion || "").trim(),
      variation_hint: String(item.variation_hint || "").trim(),
    };
  });
}

function promptTextFromItem(item) {
  return String(item.prompt || item.video_prompt || "").trim();
}

export function normalizePromptResponse({ response, families }) {
  const normalizedFamilies = normalizeFamilies(families);
  const prompts = response && Array.isArray(response.prompts) ? response.prompts : [];
  const expectedFamilies = new Set(normalizedFamilies.map((item) => item.family));
  const promptByFamily = new Map();

  for (const item of prompts) {
    const family = normalizeFamilyId(item.family);
    if (!expectedFamilies.has(family)) {
      throw new Error(`unknown prompt family: ${family}`);
    }
    if (promptByFamily.has(family)) {
      throw new Error(`duplicate prompt for family: ${family}`);
    }
    const prompt = promptTextFromItem(item);
    if (!prompt) throw new Error(`empty prompt for family: ${family}`);
    promptByFamily.set(family, {
      family,
      prompt,
      variation_notes: String(item.variation_notes || "").trim(),
    });
  }

  const missing = normalizedFamilies
    .map((item) => item.family)
    .filter((family) => !promptByFamily.has(family));
  if (missing.length > 0) {
    throw new Error(`missing prompts for family: ${missing.join(", ")}`);
  }

  const entries = normalizedFamilies.map((familyDef) => {
    const promptItem = promptByFamily.get(familyDef.family);
    return {
      family: familyDef.family,
      label: familyDef.label,
      playback: familyDef.playback,
      motion_brief: familyDef.motion_brief,
      spatial_path: familyDef.spatial_path,
      pose_signature: familyDef.pose_signature,
      prop_policy: familyDef.prop_policy,
      avoid_motion: familyDef.avoid_motion,
      variation_hint: familyDef.variation_hint,
      prompt: promptItem.prompt,
      variation_notes: promptItem.variation_notes,
    };
  });

  return {
    manifest_version: 1,
    mode: "single_family_video",
    persona: response.persona || {},
    entries,
  };
}

export function buildDryRunResponse({ families, sourceImageName = "uploaded image" }) {
  const normalizedFamilies = normalizeFamilies(families);
  return {
    persona: {
      identity_summary: `Character identity inferred from ${sourceImageName}.`,
      visual_constraints: [
        "same character identity",
        "same camera framing",
        "same background style",
        "no text or watermark",
      ],
    },
    prompts: normalizedFamilies.map((family) => ({
      family: family.family,
      prompt: buildDryRunPrompt(family, sourceImageName),
      variation_notes: family.variation_hint || "Keep variation in the middle motion only.",
    })),
  };
}

function buildDryRunPrompt(family, sourceImageName) {
  const stateLabel = family.label || family.family;
  const shared = [
    KEEP_CHIBI_STYLE_EN,
    `Keep the uploaded subject identity, markings, proportions, fixed camera, composition, background style, and lighting from ${sourceImageName}.`,
    "Use subject-adaptive anatomy: human or humanoid subjects use hands, arms, shoulders, gaze, and steps; cats or dogs use paws, tail, ears, crouch, and body lean; birds use wings, head, and feet; toys, plush, icons, or objects use tilt, bounce, rotation, or movable parts.",
    "Do not write paws for a human, hands for a cat, wings for a cat, or pet-only body language when the uploaded subject is not an animal.",
    family.family === "idle.default" ? (family.preserve_reference_accessories ? "Only subtle breathing and blinking; retain original character-associated accessories, no large movement or new props." : "Only subtle breathing and blinking; no large movement or props.")
      : "Use subject-appropriate medium-to-large visible middle motion without inventing limbs; let the action begin and finish naturally in state-specific poses.",
    family.spatial_path ? `Spatial path: ${family.spatial_path}` : "",
    family.pose_signature ? `Silhouette signature: ${family.pose_signature}` : "",
    family.prop_policy ? `${family.preserve_reference_accessories ? 'Original character accessory policy' : 'Optional small marker prop policy (idle.default forbids props)'}: ${family.prop_policy}` : "",
    family.avoid_motion ? `Avoid motion collapse: ${family.avoid_motion}` : "",
    family.preserve_reference_accessories ? "Every state retains original character-associated accessories already held or worn, including a tennis racket or instrument. Never add unrelated props, markers, text, watermarks or a second character; no identity drift."
      : "Idle.default has no props; other states may include at most one small marker prop if useful. No text, no watermark, no second main character, no oversized props, no identity drift.",
  ].filter(Boolean);

  if (family.playback === "one_shot_entry") {
    return [
      `Use the uploaded image ${sourceImageName} as the character identity and appearance reference.`,
      `Create one concise one-shot greeting for the ${stateLabel} state.`,
      family.motion_brief || `The action must clearly express ${family.label}.`,
      "Perform a readable greeting beat with a distinct silhouette and a relaxed natural finish; do not reset to a shared reference pose or freeze at the end.",
      ...shared,
    ].join(" ");
  }

  return [
    `Use the uploaded image ${sourceImageName} as the character identity and appearance reference.`,
    `Create one concise ${stateLabel} loop_state animation.`,
    family.motion_brief || `The middle motion must clearly express ${family.label}.`,
    "Create fluid state-appropriate motion (subtle only for idle.default). For repeated playback seek a gentle rhythmic transition, not an identical reference-pose reset or static ending.",
    ...shared,
  ].join(" ");
}

export function buildUserPrompt({ families = FAMILIES, appearanceName = "", personality = "" } = {}) {
  const lines = [
    KEEP_CHIBI_STYLE_EN,
    "Analyze the uploaded image first, then generate exactly one image-to-video prompt for each family below.",
    "Identify the uploaded image subject_type before writing prompts: human/humanoid, cat/dog/animal, bird, plush/toy/object, or other character.",
    "Use subject-appropriate anatomy in every prompt: human/humanoid uses hands, arms, shoulders, gaze, and steps; cat/dog/animal uses paws, tail, ears, crouch, and body lean; bird uses wings, head, and feet; plush/toy/object uses tilt, bounce, rotation, or movable parts.",
    "Do not write paws for a human, hands for a cat, wings for a cat, or pet-only body language when the uploaded subject is not an animal.",
    "Except idle.default (subtle breathing/blinking only), use subject-appropriate medium-to-large visible middle motion where helpful. Never invent limbs on a robot or object.",
    "All final video prompts must be in English.",
    "For playback=loop_state, describe continuous state-specific movement with a gentle repeat rhythm; do not require the same first/last pose or a shared reference-image reset. Idle.default stays calm.",
    "For playback=one_shot_entry, describe a readable greeting and natural relaxed finish. Different clips may start and end in different poses while retaining the same character.",
    "Do not collapse different families into the same sitting or standing pose. Every family must use its spatial_path, pose_signature, and avoid_motion fields.",
    "Custom generation should output a single working family clip. Built-in appearances may provide working.thinking, working.typing, and working.browsing for richer variation.",
    "The runtime treats both working and working.* clips as the same working state, and randomly chooses only when more than one working clip is present.",
    "Do not output thinking or tool_running families; they are legacy input states that map to working.",
    "waiting_user is the separate decide/user-choice state; do not mix it into random working variants.",
    families.some(f=>f.preserve_reference_accessories) ? "Every state, including idle.default, must preserve original character-associated accessories already held or worn, including a tennis racket, instrument, glasses or hat. Do not invent new props or markers. Keep the subject and accessories fully within frame."
      : "Idle.default has no props. Other states may use at most one small marker prop from prop_policy when needed; never cover the subject or become the subject.",
    "Treat touch.lick and touch.what as touch-screen feedback variants, not literal-name actions or symbolic reactions: the subject should move its face or front side close to the foreground and gently touch the screen glass.",
    "Do not copy legacy family ids into the final video prompt text; write the visible state behavior instead.",
    "Do not create enter / loop / exit prompts. Do not omit any family. Do not add extra families.",
  ];
  if (appearanceName.trim()) {
    lines.push(`The user calls this subject "${appearanceName.trim()}".`);
  }
  if (personality.trim()) {
    lines.push(`User-provided personality direction: ${personality.trim()}`);
  }
  lines.push("", "Families:", JSON.stringify(families, null, 2));
  return lines.join("\n");
}
