/**
 * [Input] title/subtitle/actions/children props plus an opt-in Collapsible with summary.
 * [Output] Unified section card replacing panel-card / component-store-section / component-tool-card; Collapsible powers the device dashboard's "voice assistant" section.
 * [Pos] component node in ref/src/shell
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import React, { useState } from "react";
import { ChevronDown } from "lucide-react";

export default function Card({ title, subtitle, actions, children }) {
  const showHeader = title || subtitle || actions;
  return (
    <section className="card">
      {showHeader && (
        <header className="card__header">
          <div className="card__title-block">
            {title && <h2 className="card__title">{title}</h2>}
            {subtitle && <p className="card__subtitle">{subtitle}</p>}
          </div>
          {actions && <div className="card__actions">{actions}</div>}
        </header>
      )}
      <div className="card__body">{children}</div>
    </section>
  );
}

function CardCollapsible({ title, summary, defaultOpen = false, children }) {
  const [open, setOpen] = useState(defaultOpen);
  return (
    <section
      className={open ? "card card--collapsible is-open" : "card card--collapsible"}
    >
      <button
        type="button"
        className="card__header card__header--toggle"
        onClick={() => setOpen((prev) => !prev)}
        aria-expanded={open}
      >
        <div className="card__title-block">
          <h2 className="card__title">{title}</h2>
          {!open && summary && <p className="card__subtitle">{summary}</p>}
        </div>
        <ChevronDown
          size={16}
          className={open ? "card__chevron is-open" : "card__chevron"}
          aria-hidden="true"
        />
      </button>
      {open && <div className="card__body">{children}</div>}
    </section>
  );
}

Card.Collapsible = CardCollapsible;
