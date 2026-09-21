import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createDoneCue, DONE_CUE } from "../../scripts/generate-done-cue.mjs";

test("completion audio has exactly four triple-beep groups and matches bundled PCM", () => {
  const wav = createDoneCue();
  assert.deepEqual(readFileSync(new URL("../../public/terrier-clips/done.wav", import.meta.url)), wav);
  const factory = JSON.parse(readFileSync(new URL("../../../firmware/factory-config.json", import.meta.url)));
  const manifest = JSON.parse(readFileSync(new URL(`../../public/terrier-clips/p4-ready/${factory.appearance.readyProfile}/p4/manifest.json`, import.meta.url)));
  assert.equal(manifest.systemCues, true);
  assert.ok(manifest.families.every((family) => !family.audioPath));
  const cmake = readFileSync(new URL("../../../firmware/main/CMakeLists.txt", import.meta.url), "utf8");
  assert.ok(cmake.includes("foreach(cue done error waiting_user)"));
  assert.ok(cmake.includes("pc/public/terrier-clips/${cue}.wav"));
  assert.equal(wav.readUInt32LE(24), 16000);
  assert.equal(wav.readUInt16LE(22), 1);
  assert.equal(wav.readUInt16LE(34), 16);
  assert.equal(wav.readUInt32LE(40) / 32000, 3.8);
  // Detect actual tone bursts in 10 ms windows, not merely generator constants.
  const bursts = []; let playing = false;
  for (let offset = 44, ms = 0; offset < wav.length; offset += 320, ms += 10) {
    let energy = 0;
    for (let i = offset; i < Math.min(offset + 320, wav.length); i += 2) energy += Math.abs(wav.readInt16LE(i));
    const audible = energy > 10000;
    if (audible && !playing) bursts.push({ start: ms });
    if (!audible && playing) bursts.at(-1).end = ms;
    playing = audible;
  }
  if (playing) bursts.at(-1).end = 3800;
  assert.equal(bursts.length, 12);
  for (let group = 0; group < 4; group++) {
    const three = bursts.slice(group * 3, group * 3 + 3);
    assert.equal(three[2].end - three[0].start, 650);
    assert.ok(three[2].end - three[0].start <= 1000);
    if (group) assert.equal(three[0].start - bursts[group * 3 - 1].end, DONE_CUE.betweenGroupsMs);
  }
});
