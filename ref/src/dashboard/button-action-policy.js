/**
 * [Input] Runtime-specific button rows and their selected action map.
 * [Output] Duplicate-action ownership lookup and deterministic unique-action normalization.
 * [Pos] Pure button-policy helper shared by DeviceDashboard and BoardButtonPanel.
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

const REPEATABLE_ACTIONS = new Set(["disabled"]);

function selectedActionForRow(row, buttonActions = {}) {
  const selected = buttonActions[row.id];
  return row.actionOptions.includes(selected) ? selected : row.defaultAction;
}

export function findButtonActionOwner(
  controlRows = [],
  buttonActions = {},
  rowId = "",
  actionId = "",
) {
  if (!actionId || REPEATABLE_ACTIONS.has(actionId)) return null;
  return controlRows.find((row) => (
    row.id !== rowId
    && selectedActionForRow(row, buttonActions) === actionId
  )) || null;
}

export function enforceUniqueButtonActions(controlRows = [], buttonActions = {}) {
  const next = { ...buttonActions };
  const ownerByAction = new Map();

  controlRows.forEach((row) => {
    const selectedAction = selectedActionForRow(row, next);
    if (!selectedAction || REPEATABLE_ACTIONS.has(selectedAction)) {
      next[row.id] = selectedAction || "disabled";
      return;
    }
    if (!ownerByAction.has(selectedAction)) {
      next[row.id] = selectedAction;
      ownerByAction.set(selectedAction, row.id);
      return;
    }

    const fallback = row.defaultAction;
    if (
      fallback
      && !REPEATABLE_ACTIONS.has(fallback)
      && row.actionOptions.includes(fallback)
      && !ownerByAction.has(fallback)
    ) {
      next[row.id] = fallback;
      ownerByAction.set(fallback, row.id);
      return;
    }
    next[row.id] = "disabled";
  });

  return next;
}
