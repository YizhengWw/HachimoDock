"use strict";

/**
 * AgentAdapter — interface every coding-agent integration must implement.
 *
 * The bus owns no per-session state of its own; it routes
 * "user said X to agent Y in session Z" to the right adapter and forwards
 * the agent's stream back. Each adapter is responsible for translating that
 * to whatever the agent's actual transport happens to be (CLI subprocess,
 * local npm import, RPC, …).
 *
 * `resolveActive()` has a default implementation that picks the newest
 * entry from `listSessions()` — this is the "永远续最近" rule from
 * docs/voice-architecture.md §4. Override only if the agent has a stronger
 * notion of "active" than just file mtime (e.g. an explicit
 * "currently-attached-IDE-session" pointer).
 *
 * @typedef {Object} SessionRef
 * @property {string} id
 * @property {number} lastModified  epoch ms
 * @property {string} [cwd]
 * @property {string} [name]
 * @property {string} [summary]
 *
 * @typedef {Object} InjectRequest
 * @property {string} sessionId         "auto" or an adapter-specific id
 * @property {string} text
 * @property {Record<string, unknown>} [metadata]
 * @property {AbortSignal} [signal]
 * @property {string} [cwd]             optional cwd hint for openNew()
 *
 * @typedef {{kind:"token", text:string} |
 *          {kind:"tool", name:string, phase:"start"|"end", input?:any, ok?:boolean} |
 *          {kind:"done", sessionId:string, tokens?:number, stopReason?:string} |
 *          {kind:"error", code:string, message:string}} AgentEvent
 */

class BaseAdapter {
  /**
   * @param {object} opts
   * @param {string} opts.agentId  must match the public agent id (e.g. "claude-code")
   * @param {(level: string, msg: string, details?: object) => void} [opts.log]
   */
  constructor({ agentId, log } = {}) {
    if (!agentId || typeof agentId !== "string") {
      throw new Error("BaseAdapter requires an agentId");
    }
    this.agentId = agentId;
    this.log = log || (() => {});
  }

  /**
   * Probe whether this agent is installed/usable on this host.
   * @returns {Promise<{ready: boolean, reason?: string}>}
   */
  async isAvailable() {
    return { ready: false, reason: "not implemented" };
  }

  /**
   * List sessions usable by `inject`, most-recent first.
   * Implementations should not throw on a missing-on-disk state — return [] instead.
   * @param {{limit?: number}} [_opts]
   * @returns {Promise<SessionRef[]>}
   */
  async listSessions(_opts) {
    return [];
  }

  /**
   * Pick the session for `auto` resolution. Default: newest in listSessions().
   * @returns {Promise<SessionRef|null>}
   */
  async resolveActive() {
    const sessions = await this.listSessions({ limit: 1 });
    return sessions.length > 0 ? sessions[0] : null;
  }

  /**
   * Open a brand-new session and return its ref. Some agents only mint a
   * session id on the first injected message — implementations may choose
   * to return a ref with `id: ""` and let `inject()` fill it in via the
   * `done` event's `sessionId` field.
   * @param {{cwd?: string}} [_opts]
   * @returns {Promise<SessionRef>}
   */
  async openNew(_opts) {
    throw new Error(`${this.agentId}.openNew not implemented`);
  }

  /**
   * Inject one user turn into a session and yield agent events.
   * @param {InjectRequest} _req
   * @returns {AsyncIterable<AgentEvent>}
   */
  // eslint-disable-next-line require-yield
  async *inject(_req) {
    throw new Error(`${this.agentId}.inject not implemented`);
  }
}

module.exports = {
  BaseAdapter,
};
