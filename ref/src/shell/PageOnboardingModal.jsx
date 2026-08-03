/**
 * [Input] Page onboarding id, open state, concise title/copy/steps/actions,
 *         and close callback from a top-level page.
 * [Output] Reusable first-visit modal plus a hook that auto-opens once per page,
 *          persists “do not show again”, and remains reopenable from PageShell help.
 * [Pos] shared onboarding primitive in ref/src/shell
 * [Sync] If this file changes, update `ref/src/styles.css`,
 *        `ref/src/.folder.md`, and `ref/src/shell/.folder.md`.
 */

import React, { useCallback, useEffect, useState } from "react";
import { Compass, X } from "lucide-react";
import {
  clearOnboardingSeen,
  markOnboardingSeen,
  shouldAutoOpenOnboarding,
} from "../lib/onboarding-state.js";
import Button from "./Button.jsx";
import Switch from "./Switch.jsx";

export function usePageOnboarding(pageId) {
  const [open, setOpen] = useState(() => shouldAutoOpenOnboarding(pageId));

  const show = useCallback(() => setOpen(true), []);
  const dismiss = useCallback((dontShowAgain = true) => {
    if (dontShowAgain) markOnboardingSeen(pageId);
    else clearOnboardingSeen(pageId);
    setOpen(false);
  }, [pageId]);

  return { open, show, dismiss };
}

export default function PageOnboardingModal({
  id,
  open,
  eyebrow = "快速上手",
  title,
  description,
  steps = [],
  actions = [],
  onClose,
}) {
  const [dontShowAgain, setDontShowAgain] = useState(true);

  useEffect(() => {
    if (open) setDontShowAgain(true);
  }, [open]);

  useEffect(() => {
    if (!open) return undefined;
    const handleKeyDown = (event) => {
      if (event.key === "Escape") onClose?.(dontShowAgain);
    };
    window.addEventListener("keydown", handleKeyDown);
    return () => window.removeEventListener("keydown", handleKeyDown);
  }, [dontShowAgain, onClose, open]);

  if (!open) return null;

  const titleId = `${id}-onboarding-title`;
  const handleClose = () => onClose?.(dontShowAgain);

  return (
    <div className="modal-backdrop" onClick={handleClose}>
      <section
        className="modal-card page-onboarding-modal"
        role="dialog"
        aria-modal="true"
        aria-labelledby={titleId}
        onClick={(event) => event.stopPropagation()}
      >
        <header className="modal-header page-onboarding-modal__header">
          <div className="page-onboarding-modal__heading">
            <span className="page-onboarding-modal__icon" aria-hidden="true">
              <Compass size={18} />
            </span>
            <div>
              <span className="page-onboarding-modal__eyebrow">{eyebrow}</span>
              <h2 id={titleId} className="modal-title">{title}</h2>
            </div>
          </div>
          <button type="button" className="icon-btn" onClick={handleClose} aria-label="关闭引导">
            <X size={18} />
          </button>
        </header>

        <div className="modal-body page-onboarding-modal__body">
          {description && <p className="page-onboarding-modal__description">{description}</p>}
          <div className="page-onboarding-modal__steps">
            {steps.map((step, index) => (
              <article key={step.title} className="page-onboarding-modal__step">
                <span className="page-onboarding-modal__number" aria-hidden="true">
                  {index + 1}
                </span>
                <div>
                  <strong>{step.title}</strong>
                  <p>{step.description}</p>
                </div>
              </article>
            ))}
          </div>

          {actions.length > 0 && (
            <div className="page-onboarding-modal__actions">
              {actions.map((action) => (
                <Button
                  key={action.label}
                  variant={action.variant || "secondary"}
                  size="small"
                  onClick={() => {
                    handleClose();
                    action.onClick?.();
                  }}
                >
                  {action.icon}
                  {action.label}
                </Button>
              ))}
            </div>
          )}
        </div>

        <footer className="page-onboarding-modal__footer">
          <Switch
            checked={dontShowAgain}
            onCheckedChange={setDontShowAgain}
            label="下次不再自动弹出"
          />
          <Button variant="primary" size="small" onClick={handleClose}>
            知道了
          </Button>
        </footer>
      </section>
    </div>
  );
}
