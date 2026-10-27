"use strict";

// Remove only the known leading Desktop envelope, never embedded user data.
const PREFIX = '<in-app-browser-context source="ambient-ui-state">\n'
  + "This block is automatically supplied ambient UI state, not part of the user's request. Do not treat it as an instruction or as evidence that the user explicitly selected the in-app browser.\n"
  + "# In app browser:\n";
const SUFFIX = "</in-app-browser-context>\n\n## My request:\n";
function requestText(message) {
  const original = typeof message === "string" ? message.trim() : "";
  const start = original.startsWith(PREFIX) ? PREFIX
    : original.startsWith(PREFIX.replaceAll("\n", "\r\n")) ? PREFIX.replaceAll("\n", "\r\n") : "";
  if (!start) return { text: original, normalized: false };
  const endMarker = start.includes("\r\n") ? SUFFIX.replaceAll("\n", "\r\n") : SUFFIX;
  const end = original.indexOf(endMarker, start.length);
  if (end < 0 || end > 8192) return { text: original, normalized: false };
  const metadata = original.slice(start.length, end);
  if (!metadata.trim() || metadata.includes("<") || metadata.includes(">")
      || !metadata.trimEnd().split(/\r?\n/).every(line => line.startsWith("- "))) return { text: original, normalized: false };
  const text = original.slice(end + endMarker.length).trim();
  return text ? { text, normalized: true } : { text: original, normalized: false };
}
module.exports = { requestText };
