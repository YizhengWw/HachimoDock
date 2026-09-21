"use strict";

/*
 * [Input] Codex rollout metadata, user prompts, and display summaries.
 * [Output] Conservative classification of internal approval/auto-review runs.
 * [Pos] Shared source filter for Codex discovery, monitoring, and HTTP sessions.
 */

const INTERNAL_MODEL_MARKERS = new Set([
  "codex-auto-review",
  "codex_auto_review",
  "approvals-reviewer",
  "approvals_reviewer",
]);

function normalize(value) {
  return typeof value === "string"
    ? value.replace(/\s+/g, " ").trim().toLowerCase()
    : "";
}

function isInternalCodexReviewText(value) {
  const text = normalize(value);
  if (!text) return false;
  const schemaMarkers = [
    "risk_level",
    "user_authorization",
    "outcome",
    "rationale",
  ].filter((marker) => text.includes(marker)).length;
  return schemaMarkers >= 3
    && (
      text.startsWith("the following is the code")
      || text.includes("approval")
      || text.includes("authorization")
    );
}

function isInternalCodexSession(session) {
  if (!session || typeof session !== "object") return false;
  for (const value of [
    session.model,
    session.originator,
    session.threadSource,
    session.thread_source,
  ]) {
    const marker = normalize(value).replace(/ /g, "-");
    if (INTERNAL_MODEL_MARKERS.has(marker)) return true;
    if (marker.includes("auto-review") || marker.includes("approval-review")) return true;
  }

  const textValues = [
    session.name,
    session.summary,
    session.displayTitle,
    session.displayContent,
    session.sessionTitle,
    session.firstUserMessage,
    session.lastUserMessage,
    session.lastAgentMessage,
  ];
  if (textValues.some(isInternalCodexReviewText)) return true;
  return isInternalCodexReviewText(textValues.filter(Boolean).join(" "));
}

module.exports = {
  isInternalCodexReviewText,
  isInternalCodexSession,
};
