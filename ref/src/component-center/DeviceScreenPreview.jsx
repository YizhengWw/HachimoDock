/**
 * [Input] component { dashboard: {...} } from BUILTIN_COMPONENT_CENTER.components.
 * [Output] Renders the .component-device-screen DOM tree with fixed header, metric, and bottom slots for stable widget previews.
 * [Pos] component node in ref/src/component-center
 * [Sync] If this file changes, update `ref/src/component-center/.folder.md`.
 */

import React from "react";

function normalizeProgress(progress) {
  if (!progress || typeof progress !== "object") return null;
  const raw = Number(progress.value);
  if (!Number.isFinite(raw)) return null;
  return { value: Math.max(0, Math.min(100, raw)), label: progress.label };
}

export default function DeviceScreenPreview({ component, className = "" }) {
  if (!component) return null;
  const dashboard = component.dashboard || {};
  const progress = normalizeProgress(dashboard.progress);
  return (
    <div
      className={`component-device-screen ${className}`.trim()}
      data-widget={component.id}
      aria-label={`${component.name || ""} 设备屏预览`}
    >
      <div className="cds-row-top">
        {dashboard.title && <div className="cds-title-badge">{dashboard.title}</div>}
        <div className="cds-top-status">
          {dashboard.headline && <div className="cds-headline">{dashboard.headline}</div>}
          {dashboard.badge && <div className="cds-badge-circle">{dashboard.badge}</div>}
        </div>
      </div>
      {dashboard.eyebrow && <div className="cds-eyebrow">{dashboard.eyebrow}</div>}
      {(dashboard.metricLabel || dashboard.metricValue) && (
        <div className="cds-metric-panel">
          {dashboard.metricLabel && <div className="cds-metric-label">{dashboard.metricLabel}</div>}
          <div className="cds-metric-row">
            {dashboard.metricValue && <span className="cds-metric-value">{dashboard.metricValue}</span>}
            {dashboard.metricUnit && <span className="cds-metric-unit">{dashboard.metricUnit}</span>}
          </div>
          {dashboard.note && <div className="cds-note">{dashboard.note}</div>}
        </div>
      )}
      {progress && (
        <div className="cds-progress" aria-label={progress.label || "进度"}>
          <div className="cds-progress__meta">
            <span>{progress.label || "进度"}</span>
            <span>{Math.round(progress.value)}%</span>
          </div>
          <div className="cds-progress__bar"><span style={{ width: `${progress.value}%` }} /></div>
        </div>
      )}
      {!progress && dashboard.footer && <div className="cds-footer">{dashboard.footer}</div>}
    </div>
  );
}
