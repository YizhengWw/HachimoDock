/**
 * [Input] component (Component), isDraft (boolean), onClick (() => void), onDelete (() => void).
 * [Output] Compact card used in the component library grid: adaptive complete device-screen mini preview,
 *          source modifier, name, builtin/custom badge, full goal blurb, and a draft-only delete action.
 *          Clicking the main card opens ComponentPreviewModal.
 * [Pos] component node in ref/src/component-center
 * [Sync] If this file changes, update `ref/src/component-center/.folder.md`.
 */

import React from "react";
import { Trash2 } from "lucide-react";
import DeviceScreenPreview from "./DeviceScreenPreview";

export default function CandidateCard({ component, isDraft, onClick, onDelete }) {
  const cardClass = `candidate-card candidate-card--${isDraft ? "draft" : "builtin"}`;
  return (
    <article className={cardClass}>
      <button className="candidate-card__select" onClick={onClick} type="button">
        <div className="candidate-card__preview">
          <DeviceScreenPreview component={component} className="candidate-card__screen" />
        </div>
        <div className="candidate-card__body">
          <header className="candidate-card__head">
            <strong className="candidate-card__name">{component.name}</strong>
            <span className={`candidate-card__badge candidate-card__badge--${isDraft ? "custom" : "builtin"}`}>
              {isDraft ? "自定义" : "内置"}
            </span>
          </header>
          {component.goal && <p className="candidate-card__goal">{component.goal}</p>}
        </div>
      </button>
      {isDraft && onDelete && (
        <button
          type="button"
          className="candidate-card__delete"
          aria-label={`删除 ${component.name}`}
          onClick={(event) => {
            event.stopPropagation();
            onDelete();
          }}
        >
          <Trash2 size={14} />
          删除
        </button>
      )}
    </article>
  );
}
