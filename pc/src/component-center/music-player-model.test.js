import test from "node:test";
import assert from "node:assert/strict";
import { musicTime, musicProgress, EMPTY_MUSIC, MUSIC_STATES } from "./music-player-model.js";
test("music progress is actual bounded elapsed time, never fake playback", () => {
  assert.equal(musicProgress(EMPTY_MUSIC), 0);
  assert.equal(musicProgress({ current: { durationMs: 2000 }, positionMs: 500 }), 25);
  assert.equal(musicProgress({ current: { durationMs: 2000 }, positionMs: 4000 }), 100);
  assert.equal(musicProgress({ current: { durationMs: 2000 }, positionMs: -1 }), 0);
  assert.equal(musicTime(125999), "2:05");
  for (const value of [NaN, Infinity, -1]) assert.equal(musicTime(value), "--:--");
  for (const state of ["queued", "buffering", "paused", "interrupted", "ended", "error"]) assert.notEqual(MUSIC_STATES[state], MUSIC_STATES.playing);
});
