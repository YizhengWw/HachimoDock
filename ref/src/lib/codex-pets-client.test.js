/**
 * [Input] Tauri command invoker for Codex pet scan/install/import commands.
 * [Output] Node test coverage for cached Codex pet listing and install-triggered invalidation.
 * [Pos] test node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";

import { createCodexPetsClient } from "./codex-pets-client.js";

test("listCodexPets reuses cached scans until forced", async () => {
  const calls = [];
  const client = createCodexPetsClient({
    invoke: async (command) => {
      calls.push(command);
      return [{ id: `pet-${calls.length}` }];
    },
  });

  assert.deepEqual(await client.listCodexPets(), [{ id: "pet-1" }]);
  assert.deepEqual(await client.listCodexPets(), [{ id: "pet-1" }]);
  assert.deepEqual(await client.listCodexPets({ force: true }), [{ id: "pet-2" }]);
  assert.deepEqual(calls, ["list_codex_pets", "list_codex_pets"]);
});

test("installCodexCommunityPet invalidates the cached pet scan", async () => {
  const calls = [];
  const client = createCodexPetsClient({
    invoke: async (command, payload) => {
      calls.push({ command, payload });
      if (command === "list_codex_pets") return [{ id: `scan-${calls.length}` }];
      return { installed: true };
    },
  });

  assert.deepEqual(await client.listCodexPets(), [{ id: "scan-1" }]);
  assert.deepEqual(await client.installCodexCommunityPet("rx-93"), { installed: true });
  assert.deepEqual(await client.listCodexPets(), [{ id: "scan-3" }]);
  assert.deepEqual(calls.map((call) => call.command), [
    "list_codex_pets",
    "install_codex_community_pet",
    "list_codex_pets",
  ]);
  assert.deepEqual(calls[1].payload, { petId: "rx-93" });
});
