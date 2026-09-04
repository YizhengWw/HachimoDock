"use strict";
/* [Input] Local HTTP request metadata. [Output] Loopback/Host/browser-origin validation.
 * [Pos] Shared bridge HTTP boundary; native clients share the OS user's trust boundary. */
const APP_ORIGINS = new Set(["tauri://localhost", "http://tauri.localhost", "https://tauri.localhost"]);
const DEV_ORIGINS = new Set(["http://localhost:4173", "http://127.0.0.1:4173"]);
function isLoopback(value) {
  return value === "127.0.0.1" || value === "::1" || value === "::ffff:127.0.0.1";
}
function validateLocalRequest(req, { allowAppOrigin = false } = {}) {
  if (!isLoopback(req.socket?.remoteAddress)) return "non-loopback peer";
  const host = req.headers.host;
  if (typeof host !== "string" || /[\s/@#?\\]/.test(host)) return "invalid host";
  let authority;
  try { authority = new URL(`http://${host}`); } catch { return "invalid host"; }
  if (!["localhost", "127.0.0.1", "[::1]"].includes(authority.hostname)) return "untrusted host";
  if (Number(authority.port || 80) !== req.socket.localPort) return "unexpected port";
  // Reject absolute-form request targets, malformed URLs and protocol-relative targets.
  if (typeof req.url !== "string" || !req.url.startsWith("/") || req.url.startsWith("//")) return "invalid target";
  const origin = req.headers.origin;
  const trustedOrigin = allowAppOrigin && (APP_ORIGINS.has(origin)
    || (process.env.PET_MANAGER_ALLOW_DEV_ORIGIN === "1" && DEV_ORIGINS.has(origin)));
  if (origin && !trustedOrigin) return "untrusted browser origin";
  // Browsers may omit Origin on GET; native hooks and Rust do not send Fetch Metadata.
  if (!trustedOrigin && req.headers["sec-fetch-site"] && req.headers["sec-fetch-site"] !== "none") return "browser request denied";
  if (req.headers["sec-fetch-mode"] === "navigate") return "navigation denied";
  return null;
}
module.exports = { validateLocalRequest, isLoopback };
