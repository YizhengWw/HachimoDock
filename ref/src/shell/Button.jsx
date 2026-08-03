/**
 * [Input] Semantic variant/size, native button props, optional loading label, and button content.
 * [Output] Shared Pet Manager button primitive with consistent hierarchy, loading state,
 *          focus behavior, icon sizing, and disabled semantics.
 * [Pos] Cross-page control primitive in ref/src/shell.
 * [Sync] If this file changes, update `ref/design.md`, `ref/src/styles.css`, and `ref/src/shell/.folder.md`.
 */

import React from "react";

const BUTTON_VARIANTS = new Set(["primary", "secondary", "ghost", "danger", "icon"]);
const BUTTON_SIZES = new Set(["small", "medium", "large"]);

export default function Button({
  variant = "secondary",
  size = "medium",
  loading = false,
  loadingLabel = "处理中…",
  disabled = false,
  className = "",
  type = "button",
  children,
  ...buttonProps
}) {
  const resolvedVariant = BUTTON_VARIANTS.has(variant) ? variant : "secondary";
  const resolvedSize = BUTTON_SIZES.has(size) ? size : "medium";
  const classes = [
    "button",
    `button--${resolvedVariant}`,
    `button--${resolvedSize}`,
    loading ? "is-loading" : "",
    className,
  ].filter(Boolean).join(" ");

  return (
    <button
      {...buttonProps}
      type={type}
      className={classes}
      disabled={disabled || loading}
      aria-busy={loading || undefined}
    >
      <span className="button__content" aria-hidden={loading || undefined}>
        {children}
      </span>
      {loading && (
        <span className="button__loading">
          <span className="button__spinner" aria-hidden="true" />
          {loadingLabel}
        </span>
      )}
    </button>
  );
}
