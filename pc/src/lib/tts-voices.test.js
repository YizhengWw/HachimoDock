import test from "node:test";
import assert from "node:assert/strict";
import catalog from "./tts-voices.json" with { type: "json" };
import { DOUBAO_SPEAKERS, normalizePersonaVoice, speakerOptionsFor, speakerLabel } from "./persona-voice.js";

test("TTS 2.0 catalog matches the complete official 2026-09-15 snapshot", () => {
  assert.equal(DOUBAO_SPEAKERS.length, 445);
  assert.equal(new Set(DOUBAO_SPEAKERS.map(s => s.id)).size, 445);
  let hash = 2166136261;
  for (const c of DOUBAO_SPEAKERS.map(s => `${s.id}|${s.label}`).join("\n")) hash = Math.imul(hash ^ c.charCodeAt(0), 16777619) >>> 0;
  assert.equal(hash, 2848423747);
  assert.equal(DOUBAO_SPEAKERS.filter(s => s.transport === "http").length, 15);
  for (const speaker of DOUBAO_SPEAKERS) {
    assert.ok(!catalog.legacySpeakerIds.includes(speaker.id));
    assert.equal(normalizePersonaVoice({ voice: { speaker: speaker.id } }).voice.speaker, speaker.id);
  }
});

test("voice search supports names, language and IDs without discarding the current selection", () => {
  assert.equal(speakerOptionsFor().length, 445);
  assert.ok(speakerOptionsFor("", "儒雅逸辰").some(s => s.id === "zh_male_ruyayichen_uranus_bigtts"));
  assert.ok(speakerOptionsFor("", "日语").length > 0);
  assert.ok(speakerOptionsFor("", "EN_MALE_BILL_JONES").some(s => s.label === "Bill"));
  assert.deepEqual(speakerOptionsFor("", "no-such-voice"), []);
  const current = "zh_female_vv_uranus_bigtts";
  assert.equal(speakerOptionsFor(current, "no-such-voice")[0].id, current);
  for (const speaker of catalog.compatibilitySpeakers) {
    assert.equal(speakerOptionsFor(speaker.id)[0].id, speaker.id);
    assert.equal(speakerLabel({ speaker: speaker.id }), speaker.label);
  }
  assert.equal(speakerOptionsFor("custom-saved-id")[0].id, "custom-saved-id");
});
