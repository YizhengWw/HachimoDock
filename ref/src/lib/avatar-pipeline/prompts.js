/**
 * [Input] thinking-model JSON response and FAMILIES from this folder.
 * [Output] normalized prompt manifest plus subject-adaptive first/last-frame motion grammar used by video task submission.
 * [Pos] lib node in ref/src/lib/avatar-pipeline
 * [Sync] If this file changes, update this header.
 */

import { FAMILIES } from "./families.js";

function stripJsonFence(raw) {
  const text = String(raw || "").trim();
  const match = text.match(/^```(?:json)?\s*([\s\S]*?)\s*```$/i);
  return match ? match[1].trim() : text;
}

export function parseModelJson(raw) {
  return JSON.parse(stripJsonFence(raw));
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
    `Keep the uploaded subject identity, markings, proportions, fixed camera, composition, background style, and lighting from ${sourceImageName}.`,
    "Use subject-adaptive anatomy: human or humanoid subjects use hands, arms, shoulders, gaze, and steps; cats or dogs use paws, tail, ears, crouch, and body lean; birds use wings, head, and feet; toys, plush, icons, or objects use tilt, bounce, rotation, or movable parts.",
    "Do not write paws for a human, hands for a cat, wings for a cat, or pet-only body language when the uploaded subject is not an animal.",
    "The middle action must be a medium-to-large visible middle motion with a clear silhouette or position change, while the first and last frame still match the uploaded pose.",
    family.spatial_path ? `Spatial path: ${family.spatial_path}` : "",
    family.pose_signature ? `Silhouette signature: ${family.pose_signature}` : "",
    family.prop_policy ? `Required small marker prop policy: ${family.prop_policy}` : "",
    family.avoid_motion ? `Avoid motion collapse: ${family.avoid_motion}` : "",
    "Every prompt must include exactly one small marker prop from the prop_policy. No text, no watermark, no second main character, no oversized props, no identity drift. Animal-like props are allowed only when prop_policy explicitly allows them and they stay tiny and secondary.",
  ].filter(Boolean);

  if (family.playback === "one_shot_entry") {
    return [
      `Use the uploaded image ${sourceImageName} as both the first and last frame reference.`,
      `Create one concise one-shot greeting for the ${stateLabel} state.`,
      family.motion_brief || `The action must clearly express ${family.label}.`,
      "Start from the exact uploaded pose, perform one readable medium-to-large greeting beat with a visibly different middle silhouette, and return to the exact uploaded pose for a seamless single clip.",
      ...shared,
    ].join(" ");
  }

  return [
    `Use the uploaded image ${sourceImageName} as both the first and last frame reference.`,
    `Create one concise ${stateLabel} loop_state animation.`,
    family.motion_brief || `The middle motion must clearly express ${family.label}.`,
    "Start from the exact uploaded pose, perform one readable medium-to-large action cycle with a visibly different middle silhouette, and return to the exact uploaded pose for a seamless loop.",
    ...shared,
  ].join(" ");
}

export function buildUserPrompt({ families = FAMILIES, appearanceName = "", personality = "" } = {}) {
  const lines = [
    "Analyze the uploaded image first, then generate exactly one image-to-video prompt for each family below.",
    "Identify the uploaded image subject_type before writing prompts: human/humanoid, cat/dog/animal, bird, plush/toy/object, or other character.",
    "Use subject-appropriate anatomy in every prompt: human/humanoid uses hands, arms, shoulders, gaze, and steps; cat/dog/animal uses paws, tail, ears, crouch, and body lean; bird uses wings, head, and feet; plush/toy/object uses tilt, bounce, rotation, or movable parts.",
    "Do not write paws for a human, hands for a cat, wings for a cat, or pet-only body language when the uploaded subject is not an animal.",
    "Every prompt must include a medium-to-large visible middle motion: the subject should visibly shift pose, limb position, body angle, foreground depth, or 15-30% in-frame position before returning to the uploaded first/last pose.",
    "All final video prompts must be in English.",
    "For playback=loop_state, each prompt must describe one loop-safe state animation: uploaded image pose -> medium-to-large state action variation -> uploaded image pose.",
    "For playback=one_shot_entry, each prompt must also describe a first/last-frame-safe single clip: uploaded image pose -> medium-to-large greeting variation -> uploaded image pose.",
    "Do not collapse different families into the same sitting or standing pose. Every family must use its spatial_path, pose_signature, and avoid_motion fields.",
    "Custom generation should output a single working family clip. Built-in appearances may provide working.thinking, working.typing, and working.browsing for richer variation.",
    "The runtime treats both working and working.* clips as the same working state, and randomly chooses only when more than one working clip is present.",
    "Do not output thinking or tool_running families; they are legacy input states that map to working.",
    "waiting_user is the separate decide/user-choice state; do not mix it into random working variants.",
    "Each prompt must include exactly one small marker prop from prop_policy as an action cue; never let props cover the subject or become the subject.",
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
