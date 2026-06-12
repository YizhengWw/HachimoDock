/**
 * [Input] agentAppearanceMap, enabledAgents, appearances, agentOptions.
 * [Output] Pure helper exported separately so unit tests can import it without pulling in @tauri-apps/api transitively.
 * [Pos] lib node in ref/src/shell
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import {
  activeDesktopAssignment,
  appearanceById,
  channelLabelForId,
} from "../lib/agent-appearance-config.js";

export function deriveCurrentDisplay(agentAppearanceMap, enabledAgents, appearances, agentOptions) {
  const active = activeDesktopAssignment(agentAppearanceMap, enabledAgents);
  return {
    agentId: active.agentId,
    appearance: appearanceById(appearances, active.appearanceId),
    channelLabel: active.agentId ? channelLabelForId(agentOptions, active.agentId) : "",
  };
}
