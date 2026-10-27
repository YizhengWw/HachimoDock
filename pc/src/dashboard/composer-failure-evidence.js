// Local native failure snapshot; no second AX scan, persistence or network.
const block = /\[COMPOSER_EVIDENCE\]([\s\S]*?)\[\/COMPOSER_EVIDENCE\]/g;
export function withoutComposerEvidence(value) {
  return typeof value === "string" ? value.replace(block, "").trim() : "";
}
export function composerFailureEvidence(...messages) {
  for (const message of messages) {
    if (typeof message !== "string" || message.length > 131072) continue;
    for (const match of message.matchAll(block)) {
      try {
        const result = JSON.parse(match[1]);
        if (result?.schema === 1 && result.stage === "firstAmbiguousSelection"
          && Number.isInteger(result.candidateCount) && result.candidateCount > 1
          && Array.isArray(result.structure?.controls) && result.structure.controls.length <= 8) return result;
      } catch { /* Keep the normal error message if a transport truncated evidence. */ }
    }
  }
  return null;
}
