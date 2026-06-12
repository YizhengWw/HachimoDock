/**
 * [Input] Children tree consuming useToast; toast push payloads.
 * [Output] App-level toast queue with ToastProvider/useToast hook and a bottom-anchored ToastStack rendering tone/title/message/action items with auto-dismiss; replaces App.jsx inline ToastStack and AppearanceGallery inline-style sync-notice.
 * [Pos] component node in ref/src/shell
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import React, {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
} from "react";
import { CheckCircle, AlertCircle, Info, X } from "lucide-react";

const ToastContext = createContext(null);

const DEFAULT_TTL = 4000;
const ICONS = {
  success: CheckCircle,
  error: AlertCircle,
  warning: AlertCircle,
  info: Info,
};

let nextToastId = 1;

export function ToastProvider({ children }) {
  const [items, setItems] = useState([]);
  const timersRef = useRef(new Map());

  const dismiss = useCallback((id) => {
    setItems((prev) => prev.filter((item) => item.id !== id));
    const timer = timersRef.current.get(id);
    if (timer) {
      clearTimeout(timer);
      timersRef.current.delete(id);
    }
  }, []);

  const push = useCallback(
    (toast) => {
      const id = nextToastId++;
      const ttl = typeof toast.ttl === "number" ? toast.ttl : DEFAULT_TTL;
      setItems((prev) => [...prev, { ...toast, id }]);
      if (ttl > 0) {
        const timer = setTimeout(() => dismiss(id), ttl);
        timersRef.current.set(id, timer);
      }
      return id;
    },
    [dismiss],
  );

  useEffect(() => {
    const timers = timersRef.current;
    return () => {
      for (const timer of timers.values()) clearTimeout(timer);
      timers.clear();
    };
  }, []);

  const value = useMemo(() => ({ push, dismiss, items }), [push, dismiss, items]);
  return <ToastContext.Provider value={value}>{children}</ToastContext.Provider>;
}

export function useToast() {
  const ctx = useContext(ToastContext);
  if (!ctx) throw new Error("useToast must be used inside <ToastProvider>");
  return ctx;
}

export default function ToastStack() {
  const { items, dismiss } = useToast();
  if (items.length === 0) return null;
  return (
    <div className="toast-stack">
      {items.map(({ id, tone = "info", title, message, action }) => {
        const Icon = ICONS[tone] || Info;
        return (
          <div key={id} className={`toast toast--${tone}`} role="status" aria-live="polite">
            <div className="toast__head">
              <Icon size={16} />
              <span className="toast__title">{title}</span>
              <button
                type="button"
                className="icon-btn"
                onClick={() => dismiss(id)}
                aria-label="关闭通知"
              >
                <X size={14} />
              </button>
            </div>
            {message && (
              <div className="muted small" style={{ whiteSpace: "pre-wrap" }}>
                {message}
              </div>
            )}
            {action && (
              <div className="toast__actions">
                <button
                  className="btn-primary btn-sm"
                  type="button"
                  onClick={() => {
                    action.onClick?.();
                    dismiss(id);
                  }}
                >
                  {action.label}
                </button>
              </div>
            )}
          </div>
        );
      })}
    </div>
  );
}
