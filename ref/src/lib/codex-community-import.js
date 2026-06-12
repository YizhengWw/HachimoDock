/**
 * [Input] Codex pet summaries returned by the Tauri `list_codex_pets` command.
 * [Output] Pure snapshot/diff and paste-input parsing helpers for the community import flow.
 * [Pos] lib node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

export function normalizeCodexPetModifiedAt(value) {
  const n = Number(value);
  return Number.isFinite(n) && n > 0 ? n : 0;
}

export function buildCodexPetSnapshot(pets = []) {
  const snapshot = new Map();
  for (const pet of pets || []) {
    if (!pet?.id) continue;
    snapshot.set(pet.id, normalizeCodexPetModifiedAt(pet.modifiedAt));
  }
  return snapshot;
}

export function sortCodexPetsByModifiedAt(pets = []) {
  return [...(pets || [])].sort((a, b) => {
    const modifiedDelta =
      normalizeCodexPetModifiedAt(b?.modifiedAt) - normalizeCodexPetModifiedAt(a?.modifiedAt);
    if (modifiedDelta !== 0) return modifiedDelta;
    return String(a?.displayName || a?.id || "").localeCompare(
      String(b?.displayName || b?.id || ""),
      "zh-Hans-CN",
    );
  });
}

export function findUpdatedCodexPets(snapshot, pets = []) {
  const baseline = snapshot instanceof Map ? snapshot : new Map();
  return sortCodexPetsByModifiedAt(
    (pets || []).filter((pet) => {
      if (!pet?.id) return false;
      if (!baseline.has(pet.id)) return true;
      const previous = baseline.get(pet.id) || 0;
      const current = normalizeCodexPetModifiedAt(pet.modifiedAt);
      return previous > 0 && current > previous;
    }),
  );
}

export function formatCodexPetModifiedAt(value) {
  const modifiedAt = normalizeCodexPetModifiedAt(value);
  if (!modifiedAt) return "";
  return new Date(modifiedAt).toLocaleString();
}

const COMMUNITY_IMPORT_ERROR =
  "请粘贴 codex-pets.net 地址、curl 安装命令，或 npx codex-pets add <id>。";
const PET_ID_RE = /^[a-zA-Z0-9][a-zA-Z0-9._-]{0,127}$/;
const CODEX_PETS_HOST_RE = /(^|\.)codex-pets\.net$/i;

function normalizePastedCommand(input) {
  return String(input || "")
    .trim()
    .replace(/^\$\s*/, "")
    .replace(/^>\s*/, "");
}

function validatePetId(value) {
  const petId = decodeURIComponent(String(value || "").trim()).replace(/^@/, "");
  return PET_ID_RE.test(petId) ? petId : "";
}

function isRouteMarker(value) {
  return ["pet", "pets", "add", "install"].includes(String(value || "").toLowerCase());
}

function isInstallerScriptSegment(value) {
  return /\.(?:sh|bash|zsh|ps1|cmd|bat)$/i.test(String(value || ""));
}

function petIdFromUrl(candidate) {
  let url;
  try {
    url = new URL(candidate);
  } catch (_) {
    return "";
  }
  if (!CODEX_PETS_HOST_RE.test(url.hostname)) return "";

  const queryId =
    validatePetId(url.searchParams.get("pet")) || validatePetId(url.searchParams.get("id"));
  if (queryId) return queryId;

  const route = `${url.pathname}/${url.hash.replace(/^#\/?/, "")}`;
  const segments = route
    .split(/[/?#&=]+/)
    .map((part) => part.trim())
    .filter(Boolean);
  const markerIndex = segments.findIndex((part) =>
    isRouteMarker(part),
  );
  if (markerIndex >= 0) {
    const markedId = validatePetId(segments[markerIndex + 1]);
    if (markedId) return markedId;
  }

  for (let i = segments.length - 1; i >= 0; i -= 1) {
    const petId = validatePetId(segments[i]);
    if (petId && !isRouteMarker(petId) && !isInstallerScriptSegment(petId)) {
      return petId;
    }
  }
  return "";
}

function petIdFromCurlShellArgs(input) {
  const match = input.match(
    /\|\s*(?:sh|bash|zsh)\b(?:\s+-s)?(?:\s+--)?\s+([a-zA-Z0-9][a-zA-Z0-9._-]{0,127})(?:\s|$)/i,
  );
  return validatePetId(match?.[1]);
}

function firstCommunityUrl(input) {
  const urls = input.match(/https?:\/\/[^\s'"`<>）)]+/gi) || [];
  return urls.find((url) => petIdFromUrl(url)) || "";
}

export function parseCommunityPetImportInput(input) {
  const value = normalizePastedCommand(input);
  if (!value) {
    return { ok: false, error: COMMUNITY_IMPORT_ERROR };
  }

  const commandMatch = value.match(
    /(?:^|\s)(?:npx\s+(?:--yes\s+|-y\s+)?(?:--\s+)?)?codex-pets\s+add\s+([a-zA-Z0-9][a-zA-Z0-9._-]{0,127})(?:\s|$)/i,
  );
  if (commandMatch) {
    return { ok: true, petId: commandMatch[1], source: "cli" };
  }

  if (/\bcurl\b/i.test(value)) {
    const curlArgId = petIdFromCurlShellArgs(value);
    if (curlArgId) {
      return { ok: true, petId: curlArgId, source: "curl" };
    }
  }

  const url = firstCommunityUrl(value);
  if (url) {
    return {
      ok: true,
      petId: petIdFromUrl(url),
      source: /\bcurl\b/i.test(value) ? "curl" : "url",
    };
  }

  const rawId = validatePetId(value);
  if (rawId) {
    return { ok: true, petId: rawId, source: "id" };
  }

  return { ok: false, error: COMMUNITY_IMPORT_ERROR };
}
