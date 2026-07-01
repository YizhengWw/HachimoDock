/**
 * [Input] Custom-draft records and `.clawpkg` on-disk paths from ComponentCenter.
 * [Output] Pure (no-JSX) helpers for draft summaries and matching a draft to a
 *          clawpkg path across OS path separators. Plain JS so the matching
 *          rules are unit-testable on bare node.
 * [Pos] helper node in ref/src/component-center
 * [Sync] If this file changes, update `ref/src/component-center/.folder.md`.
 */

export function buildDraftGoal(draft) {
  const description = typeof draft.description === "string" ? draft.description.trim() : "";
  return description || "自定义草稿 · 可预览后安装到负一屏。";
}

export function normalizeLocalPath(value) {
  return String(value || "").replaceAll("\\", "/");
}

export function pathContainsComponentId(value, componentId) {
  const id = String(componentId || "").trim();
  if (!id) return false;
  return normalizeLocalPath(value)
    .split("/")
    .some((segment) => segment === id || segment === `${id}.clawpkg` || segment === `${id}.zip`);
}

export function matchesDraftPath(draft, clawpkgPath) {
  if (!draft || !clawpkgPath) return false;
  return normalizeLocalPath(draft.path) === normalizeLocalPath(clawpkgPath)
    || pathContainsComponentId(clawpkgPath, draft.id);
}
