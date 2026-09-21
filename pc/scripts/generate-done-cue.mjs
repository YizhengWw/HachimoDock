/** Deterministic P4 completion cue: four groups of three beeps; PCM16 mono 16 kHz. */
import { writeFileSync } from "node:fs";
import { fileURLToPath, pathToFileURL } from "node:url";

export const DONE_CUE = Object.freeze({
  sampleRate: 16000, frequency: 1200, groups: 4, beepsPerGroup: 3,
  beepMs: 150, betweenBeepsMs: 100, betweenGroupsMs: 400, fadeMs: 8,
});

export function createDoneCue() {
  const c = DONE_CUE;
  const groupMs = c.beepsPerGroup * c.beepMs + (c.beepsPerGroup - 1) * c.betweenBeepsMs;
  const durationMs = c.groups * groupMs + (c.groups - 1) * c.betweenGroupsMs;
  const samples = c.sampleRate * durationMs / 1000;
  const wav = Buffer.alloc(44 + samples * 2);
  wav.write("RIFF", 0); wav.writeUInt32LE(wav.length - 8, 4); wav.write("WAVEfmt ", 8);
  wav.writeUInt32LE(16, 16); wav.writeUInt16LE(1, 20); wav.writeUInt16LE(1, 22);
  wav.writeUInt32LE(c.sampleRate, 24); wav.writeUInt32LE(c.sampleRate * 2, 28);
  wav.writeUInt16LE(2, 32); wav.writeUInt16LE(16, 34); wav.write("data", 36);
  wav.writeUInt32LE(samples * 2, 40);
  const beepSamples = c.sampleRate * c.beepMs / 1000;
  const fadeSamples = c.sampleRate * c.fadeMs / 1000;
  for (let group = 0; group < c.groups; group++) {
    for (let beep = 0; beep < c.beepsPerGroup; beep++) {
      const start = c.sampleRate * (group * (groupMs + c.betweenGroupsMs)
        + beep * (c.beepMs + c.betweenBeepsMs)) / 1000;
      for (let i = 0; i < beepSamples; i++) {
        const envelope = Math.min(1, i / fadeSamples, (beepSamples - 1 - i) / fadeSamples);
        const sample = Math.round(14000 * envelope * Math.sin(2 * Math.PI * c.frequency * i / c.sampleRate));
        wav.writeInt16LE(sample, 44 + (start + i) * 2);
      }
    }
  }
  return wav;
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const output = fileURLToPath(new URL("../public/terrier-clips/done.wav", import.meta.url));
  writeFileSync(output, createDoneCue());
  console.log("Generated completion cue: 4 × 3 beeps, 650 ms per group, 400 ms group pause, 3.8 s total.");
}
