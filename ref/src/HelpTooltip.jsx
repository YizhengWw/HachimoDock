/**
 * [Input] Small help copy that should be shown on hover/focus/click.
 * [Output] Unified rounded-rectangle help tooltip trigger for compact UI hints.
 * [Pos] component node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import React, { useEffect, useRef, useState } from "react";
import { HelpCircle } from "lucide-react";

export default function HelpTooltip({
  content,
  label = "说明",
  className = "",
  onClick,
}) {
  const [open, setOpen] = useState(false);
  const rootRef = useRef(null);

  useEffect(() => {
    function handlePointerDown(event) {
      if (!rootRef.current?.contains(event.target)) {
        setOpen(false);
      }
    }
    document.addEventListener("pointerdown", handlePointerDown);
    return () => document.removeEventListener("pointerdown", handlePointerDown);
  }, []);

  return (
    <span
      ref={rootRef}
      className={`help-tooltip ${className}`.trim()}
      onMouseEnter={() => setOpen(true)}
      onMouseLeave={() => setOpen(false)}
    >
      <button
        type="button"
        className="help-tooltip__trigger"
        aria-label={label}
        title={label}
        onFocus={() => setOpen(true)}
        onBlur={() => setOpen(false)}
        onClick={(event) => {
          setOpen((value) => !value);
          onClick?.(event);
        }}
      >
        <HelpCircle size={15} />
      </button>
      {open && (
        <span className="help-tooltip__bubble" role="tooltip">
          {content}
        </span>
      )}
    </span>
  );
}
