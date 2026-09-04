/**
 * [Input] Checked/disabled state, change callback, optional visible label, and native input props.
 * [Output] Shared accessible switch control with one track, thumb, focus, and motion contract.
 * [Pos] Cross-page control primitive in pc/src/shell.
 * [Sync] If this file changes, update `pc/design.md`, `pc/src/styles.css`, and `pc/src/shell/.folder.md`.
 */

import React from "react";

export default function Switch({
  checked = false,
  disabled = false,
  label,
  ariaLabel,
  className = "",
  onChange,
  onCheckedChange,
  ...inputProps
}) {
  return (
    <label className={["switch", disabled ? "is-disabled" : "", className].filter(Boolean).join(" ")}>
      <input
        {...inputProps}
        type="checkbox"
        role="switch"
        checked={Boolean(checked)}
        disabled={disabled}
        aria-label={ariaLabel || (typeof label === "string" ? label : undefined)}
        onChange={(event) => {
          onChange?.(event);
          onCheckedChange?.(event.target.checked);
        }}
      />
      <span className="switch__track" aria-hidden="true">
        <span className="switch__thumb" />
      </span>
      {label && <span className="switch__label">{label}</span>}
    </label>
  );
}
