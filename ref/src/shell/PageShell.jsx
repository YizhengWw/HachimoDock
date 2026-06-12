/**
 * [Input] Page-level title/subtitle/actions/help props plus children.
 * [Output] Unified page header + content wrapper used by every top-level page; replaces page-hero / page-toolbar / component-store-hero patterns.
 * [Pos] component node in ref/src/shell
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import React from "react";
import { HelpCircle } from "lucide-react";

export default function PageShell({ title, subtitle, actions, help, children }) {
  return (
    <div className="page-shell">
      <header className="page-shell__header">
        <div className="page-shell__title-block">
          <h1 className="page-shell__title">{title}</h1>
          {subtitle && <p className="page-shell__subtitle">{subtitle}</p>}
        </div>
        <div className="page-shell__trailing">
          {actions && <div className="page-shell__actions">{actions}</div>}
          {help && (
            <button
              type="button"
              className="icon-btn page-shell__help"
              onClick={help}
              aria-label="查看页面使用指南"
              title="查看页面使用指南"
            >
              <HelpCircle size={16} />
            </button>
          )}
        </div>
      </header>
      <div className="page-shell__body">{children}</div>
    </div>
  );
}
