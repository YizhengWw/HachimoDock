/**
 * [Input] Component-library records with `createdAtMs` or ISO `createdAt`.
 * [Output] Stable newest-first user ordering followed by the default builtin sequence.
 * [Pos] component-center library node in pc/src/component-center
 * [Sync] If this file changes, update `pc/src/component-center/.folder.md`.
 */

export function componentCreatedAtMs(component) {
  const numeric = Number(component?.createdAtMs);
  if (Number.isFinite(numeric) && numeric > 0) return numeric;

  const parsed = Date.parse(String(component?.createdAt || ""));
  return Number.isFinite(parsed) ? parsed : 0;
}

export function sortComponentsByCreatedAt(components) {
  return [...(components || [])]
    .map((component, index) => ({ component, index }))
    .sort((left, right) => (
      componentCreatedAtMs(right.component) - componentCreatedAtMs(left.component)
      || left.index - right.index
    ))
    .map(({ component }) => component);
}

export function mergeComponentCatalog(publishedItems, builtins) {
  const publishedIds = new Set(publishedItems.map((item) => item.id));
  return [
    ...publishedItems,
    ...builtins.filter((item) => !publishedIds.has(item.id)),
  ];
}
