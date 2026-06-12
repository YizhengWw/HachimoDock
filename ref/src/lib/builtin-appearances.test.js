/**
 * [Input] Built-in appearance descriptors.
 * [Output] Node test coverage for prepending the built-in Westie/Terrier clip set before user records and mapping default state-specific WAV cues.
 * [Pos] test node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";

import {
  BUILTIN_TERRIER_APPEARANCE_ID,
  listBuiltinAppearances,
  mergeBuiltinAppearances,
} from "./builtin-appearances.js";

test("listBuiltinAppearances exposes Westie clip set as a built-in appearance", () => {
  const [terrier] = listBuiltinAppearances();

  assert.equal(terrier.id, BUILTIN_TERRIER_APPEARANCE_ID);
  assert.equal(terrier.name, "西高地小狗");
  assert.equal(terrier.type, "builtin");
  assert.equal(terrier.families.length, 16);
  assert.equal(terrier.families[0].family, "welcome");
  assert.match(terrier.families[0].videoSrc, /\/terrier-clips\/welcome\.mp4$/);
  const done = terrier.families.find((family) => family.family === "done");
  const error = terrier.families.find((family) => family.family === "error");
  const waitingUser = terrier.families.find((family) => family.family === "waiting_user");
  assert.match(done.audioSrc, /\/terrier-clips\/done\.wav$/);
  assert.match(done.audioPath, /done\.wav$/);
  assert.equal(done.audioDefault, true);
  assert.match(error.audioSrc, /\/terrier-clips\/error\.wav$/);
  assert.match(error.audioPath, /error\.wav$/);
  assert.equal(error.audioDefault, true);
  assert.match(waitingUser.audioSrc, /\/terrier-clips\/waiting_user\.wav$/);
  assert.match(waitingUser.audioPath, /waiting_user\.wav$/);
  assert.equal(waitingUser.audioDefault, true);
  assert.equal(terrier.families.find((family) => family.family === "working.typing").audioSrc, undefined);
  assert.deepEqual(
    terrier.families
      .map((family) => family.family)
      .filter((family) => family.startsWith("working.")),
    ["working.thinking", "working.typing", "working.browsing"],
  );
  assert.equal(terrier.families.some((family) => family.family === "working.default"), false);
});

test("mergeBuiltinAppearances keeps the built-in Westie first and avoids duplicate ids", () => {
  const merged = mergeBuiltinAppearances([
    { id: "user-pet", created_at: "2026-05-13T00:00:00.000Z" },
    { id: BUILTIN_TERRIER_APPEARANCE_ID, created_at: "2026-05-14T00:00:00.000Z" },
  ]);

  assert.deepEqual(
    merged.map((record) => record.id),
    [BUILTIN_TERRIER_APPEARANCE_ID, "user-pet"],
  );
});
