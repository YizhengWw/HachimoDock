/**
 * [Input] audio-cue board WAV helper.
 * [Output] Node coverage for board-safe WAV compression constants and PCM encoding.
 * [Pos] test node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  BOARD_AUDIO_CUE_CHANNELS,
  BOARD_AUDIO_CUE_SAMPLE_RATE,
  downmixAudioBufferToMonoSamples,
  encodeMonoPcm16Wav,
} from "./audio-cue.js";

function ascii(bytes, start, length) {
  return String.fromCharCode(...bytes.slice(start, start + length));
}

test("audio cue encoder writes board-safe 16 kHz mono PCM WAV bytes", () => {
  const samples = new Float32Array([0, 0.5, -0.5, 1, -1]);
  const wav = encodeMonoPcm16Wav(samples, BOARD_AUDIO_CUE_SAMPLE_RATE);
  const view = new DataView(wav.buffer, wav.byteOffset, wav.byteLength);

  assert.equal(BOARD_AUDIO_CUE_SAMPLE_RATE, 16000);
  assert.equal(BOARD_AUDIO_CUE_CHANNELS, 1);
  assert.equal(ascii(wav, 0, 4), "RIFF");
  assert.equal(ascii(wav, 8, 4), "WAVE");
  assert.equal(ascii(wav, 12, 4), "fmt ");
  assert.equal(view.getUint16(20, true), 1);
  assert.equal(view.getUint16(22, true), BOARD_AUDIO_CUE_CHANNELS);
  assert.equal(view.getUint32(24, true), BOARD_AUDIO_CUE_SAMPLE_RATE);
  assert.equal(view.getUint16(34, true), 16);
  assert.equal(ascii(wav, 36, 4), "data");
  assert.equal(view.getUint32(40, true), samples.length * 2);
});

test("audio cue helper downmixes multi-channel source buffers before encoding", () => {
  const left = Float32Array.from([1, 0.5, 0, -0.5]);
  const right = Float32Array.from([0, -0.5, -1, -0.5]);
  const mono = downmixAudioBufferToMonoSamples(
    {
      sampleRate: 4,
      length: 4,
      numberOfChannels: 2,
      getChannelData(channel) {
        return channel === 0 ? left : right;
      },
    },
    2,
  );

  assert.equal(mono.length, 2);
  assert.deepEqual(Array.from(mono).map((value) => Number(value.toFixed(3))), [0.5, -0.5]);
});
