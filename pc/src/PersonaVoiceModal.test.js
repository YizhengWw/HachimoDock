/**
 * [Input] PersonaVoiceModal source and shared stylesheet.
 * [Output] Static coverage for the 人设与声音 modal: persona fields with templates, Doubao-only
 *          voice picker with cloned-speaker override, validation, required (post-creation) mode,
 *          factory reset for the built-in Terrier, and the preview gate.
 * [Pos] test node in ref/src
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const srcDir = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(srcDir, "PersonaVoiceModal.jsx"), "utf8");
const css = readFileSync(join(srcDir, "styles.css"), "utf8");

test("modal edits persona fields and applies templates", () => {
  for (const key of ["display_name", "intro", "style", "address", "language", "forbidden", "greeting"]) {
    assert.match(source, new RegExp(`updatePersona\\("${key}"`), `expected persona field ${key}`);
  }
  assert.match(source, /PERSONA_TEMPLATES\.map/);
  assert.match(source, /applyTemplate\(template\)/);
});

test("voice section is Doubao Seed TTS 2.0 only with cloned speaker override, speed and volume", () => {
  assert.match(source, /豆包 Seed TTS 2\.0/);
  assert.doesNotMatch(source, /provider.*<select/s, "no provider chooser in the first release");
  assert.match(source, /DOUBAO_SPEAKERS/);
  assert.match(source, /updateVoice\("clone_speaker_id"/);
  assert.match(source, /updateVoice\("speed"/);
  assert.match(source, /updateVoice\("volume"/);
});

test("required mode blocks closing until saved and validation gates the save", () => {
  assert.match(source, /const closeAllowed = !required && !saving/);
  assert.match(source, /validatePersonaVoice\(form\)/);
  assert.match(source, /savePersonaVoice\(appearance\.id, form\)/);
  assert.match(source, /保存并继续/);
});

test("built-in Terrier can restore its factory persona and preview stays gated on the voice runtime", () => {
  assert.match(source, /BUILTIN_TERRIER_PERSONA_VOICE/);
  assert.match(source, /恢复出厂人设/);
  assert.match(source, /disabled=\{!previewAvailable \|\| previewing \|\| saving\}/);
});

test("stylesheet defines the modal grid and the card persona line", () => {
  assert.match(css, /\.persona-voice-modal__grid\s*\{/);
  assert.match(css, /\.appearance-card__persona--missing\s*\{/);
});
