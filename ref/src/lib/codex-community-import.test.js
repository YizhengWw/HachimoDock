/**
 * [Input] Codex community import helper fixtures.
 * [Output] Node test coverage for snapshot diffing and newest-first ordering.
 * [Pos] test node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";

import {
  buildCodexPetSnapshot,
  findUpdatedCodexPets,
  parseCommunityPetImportInput,
} from "./codex-community-import.js";

test("findUpdatedCodexPets returns new and modified pets newest first", () => {
  const baseline = buildCodexPetSnapshot([
    { id: "same", modifiedAt: 1000 },
    { id: "updated", modifiedAt: 2000 },
  ]);

  const changed = findUpdatedCodexPets(baseline, [
    { id: "same", modifiedAt: 1000 },
    { id: "updated", modifiedAt: 9000 },
    { id: "newer", modifiedAt: 12000 },
  ]);

  assert.deepEqual(
    changed.map((pet) => pet.id),
    ["newer", "updated"],
  );
});

test("findUpdatedCodexPets falls back to id diff when modifiedAt is missing", () => {
  const baseline = buildCodexPetSnapshot([{ id: "already-installed" }]);

  const changed = findUpdatedCodexPets(baseline, [
    { id: "already-installed" },
    { id: "fresh-install" },
  ]);

  assert.deepEqual(
    changed.map((pet) => pet.id),
    ["fresh-install"],
  );
});

test("parseCommunityPetImportInput extracts pet ids from CLI commands", () => {
  assert.deepEqual(parseCommunityPetImportInput("$ npx codex-pets add sakura-jk"), {
    ok: true,
    petId: "sakura-jk",
    source: "cli",
  });
});

test("parseCommunityPetImportInput extracts pet ids from community URLs", () => {
  assert.deepEqual(
    parseCommunityPetImportInput("https://codex-pets.net/#/pets/sakura-jk?from=gallery"),
    {
      ok: true,
      petId: "sakura-jk",
      source: "url",
    },
  );
});

test("parseCommunityPetImportInput extracts pet ids from curl install commands", () => {
  assert.deepEqual(
    parseCommunityPetImportInput("curl -fsSL https://codex-pets.net/install/sakura-jk | sh"),
    {
      ok: true,
      petId: "sakura-jk",
      source: "curl",
    },
  );
});

test("parseCommunityPetImportInput extracts pet ids from curl script arguments", () => {
  assert.deepEqual(
    parseCommunityPetImportInput("curl -fsSL https://codex-pets.net/install.sh | sh -s sakura-jk"),
    {
      ok: true,
      petId: "sakura-jk",
      source: "curl",
    },
  );
});

test("parseCommunityPetImportInput rejects unsupported commands", () => {
  assert.deepEqual(parseCommunityPetImportInput("rm -rf ~/.codex/pets"), {
    ok: false,
    error: "请粘贴 codex-pets.net 地址、curl 安装命令，或 npx codex-pets add <id>。",
  });
});
