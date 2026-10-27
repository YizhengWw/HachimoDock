import { useEffect, useId, useState } from "react";
import { invoke } from "@tauri-apps/api/core";

// Keep IPC ordering stable when a completed task is immediately replaced by a new one.
const reasons = new Map();
const listeners = new Set();
let visibleReasons = [];
let queue = Promise.resolve();
export function setExitReason(id, reason) {
  if (reason) reasons.set(id, reason); else reasons.delete(id);
  const snapshot = [...new Set(reasons.values())];
  if (JSON.stringify(snapshot) === JSON.stringify(visibleReasons)) return;
  visibleReasons = snapshot;
  for (const listener of listeners) listener(snapshot);
  if (typeof window !== "undefined" && window.__TAURI_INTERNALS__) {
    queue = queue.catch(() => {}).then(() => invoke("set_exit_reasons", { reasons: snapshot })).catch(() => {});
  }
}
export function useExitReasons() {
  const [value, setValue] = useState(() => visibleReasons);
  useEffect(() => { listeners.add(setValue); setValue(visibleReasons); return () => listeners.delete(setValue); }, []);
  return value;
}
export function useExitProtection(reason) {
  const id = useId();
  useEffect(() => { setExitReason(id, reason); return () => setExitReason(id, ""); }, [id, reason]);
}
export function installBrowserExitProtection() {
  const handler = event => {
    if (!reasons.size) return;
    event.preventDefault(); event.returnValue = "";
  };
  window.addEventListener("beforeunload", handler);
  return () => window.removeEventListener("beforeunload", handler);
}
