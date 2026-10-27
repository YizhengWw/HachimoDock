// Single-flight fallback reads must never overwrite a newer push/start/stop event.
export function createStatusReconciler() {
  let revision = 0;
  let pending = false;
  let disposed = false;
  return {
    invalidate() { revision += 1; },
    dispose() { disposed = true; revision += 1; },
    async poll(read, apply) {
      if (pending || disposed) return;
      pending = true;
      const startedAt = revision;
      try {
        const status = await read();
        if (!disposed && startedAt === revision && status) apply(status);
      } finally { pending = false; }
    },
  };
}

export function mergeStatus(current, status) {
  const next = { ...current, ...status };
  return JSON.stringify(next) === JSON.stringify(current) ? current : next;
}
