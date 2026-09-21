"use strict";

class AdapterRegistry {
  /**
   * @param {object} opts
   * @param {Array<import("./adapters/base").BaseAdapter>} [opts.adapters]
   * @param {(level: string, msg: string, details?: object) => void} [opts.log]
   */
  constructor({ adapters = [], log } = {}) {
    this._byId = new Map();
    this._statusCache = new Map();
    this.log = log || (() => {});
    for (const a of adapters) this.register(a);
  }

  register(adapter) {
    if (!adapter || typeof adapter !== "object") {
      throw new Error("AdapterRegistry.register expects an adapter instance");
    }
    if (typeof adapter.agentId !== "string" || !adapter.agentId) {
      throw new Error("adapter.agentId must be a non-empty string");
    }
    if (this._byId.has(adapter.agentId)) {
      throw new Error(`adapter ${adapter.agentId} already registered`);
    }
    this._byId.set(adapter.agentId, adapter);
    this._statusCache.delete(adapter.agentId);
    this.log("info", "adapter registered", { agentId: adapter.agentId });
  }

  ids() {
    return Array.from(this._byId.keys());
  }

  has(agentId) {
    return this._byId.has(agentId);
  }

  /** @returns {import("./adapters/base").BaseAdapter|null} */
  get(agentId) {
    return this._byId.get(agentId) || null;
  }

  /**
   * Cached availability snapshot. Pass `{fresh:true}` to bypass the cache.
   * @param {{fresh?: boolean, ttlMs?: number}} [opts]
   */
  async statusAll({ fresh = false, ttlMs = 5000 } = {}) {
    const out = [];
    for (const [agentId, adapter] of this._byId.entries()) {
      out.push(await this._statusOne(agentId, adapter, { fresh, ttlMs }));
    }
    return out;
  }

  async statusOne(agentId, opts = {}) {
    const adapter = this._byId.get(agentId);
    if (!adapter) return { agentId, ready: false, reason: "adapter not registered" };
    return this._statusOne(agentId, adapter, opts);
  }

  async _statusOne(agentId, adapter, { fresh = false, ttlMs = 5000 } = {}) {
    const now = Date.now();
    const cached = this._statusCache.get(agentId);
    if (!fresh && cached && now - cached.checkedAt < ttlMs) {
      return cached.value;
    }
    let value;
    try {
      const probe = await adapter.isAvailable();
      value = {
        agentId,
        ready: Boolean(probe && probe.ready),
        reason: probe && typeof probe.reason === "string" ? probe.reason : null,
      };
    } catch (error) {
      value = {
        agentId,
        ready: false,
        reason: `isAvailable() threw: ${error && error.message ? error.message : String(error)}`,
      };
    }
    this._statusCache.set(agentId, { checkedAt: now, value });
    return value;
  }
}

module.exports = {
  AdapterRegistry,
};
