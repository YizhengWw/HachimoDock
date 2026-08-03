/**
 * [Input] component, explicit kind, local/installed/enabled state, and select/device/delete actions.
 * [Output] Compact card used in the component library grid: adaptive complete device-screen mini preview,
 *          source + game/tool metadata, full goal blurb without preview overlays,
 *          one two-state device action, and formal-local dual-delete access.
 * [Pos] component node in ref/src/component-center
 * [Sync] If this file changes, update `ref/src/component-center/.folder.md`.
 */

import React from "react";
import { Download, PackageCheck, Trash2 } from "lucide-react";
import Button from "../shell/Button";
import DeviceScreenPreview from "./DeviceScreenPreview";

export function resolveComponentKind(kind, gameType) {
  const explicitKind = String(kind || "").trim().toLowerCase();
  if (["game", "mini-game", "minigame", "小游戏"].includes(explicitKind)) return "game";
  if (["tool", "utility", "widget", "工具", "工具组件"].includes(explicitKind)) return "tool";
  return gameType ? "game" : "tool";
}

export function componentKindLabel(kind) {
  return kind === "game" ? "小游戏" : "工具组件";
}

export default function CandidateCard({
  component,
  kind,
  isLocal,
  isInstalled,
  isEnabled,
  onClick,
  onDeviceAction,
  onDelete,
}) {
  const resolvedKind = resolveComponentKind(kind, component.gameType);
  const sourceKind = component.isDeviceOnly ? "device" : isLocal ? "custom" : "builtin";
  const sourceLabel = component.isDeviceOnly ? "仅设备" : isLocal ? "正式本地" : "内置";
  const cardClass = [
    "candidate-card",
    `candidate-card--${component.isDeviceOnly ? "device" : isLocal ? "local" : "builtin"}`,
    isInstalled ? "candidate-card--installed" : "",
    isEnabled ? "candidate-card--enabled" : "",
  ].filter(Boolean).join(" ");

  return (
    <article className={cardClass} aria-current={isEnabled ? "true" : undefined}>
      <button className="candidate-card__select" onClick={onClick} type="button">
        <div className="candidate-card__preview">
          <DeviceScreenPreview component={component} className="candidate-card__screen" />
        </div>
        <div className="candidate-card__body">
          <header className="candidate-card__head">
            <strong className="candidate-card__name">{component.name}</strong>
            <div className="candidate-card__tags">
              <span className={`candidate-card__badge candidate-card__badge--kind-${resolvedKind}`}>
                {componentKindLabel(resolvedKind)}
              </span>
              <span className={`candidate-card__badge candidate-card__badge--${sourceKind}`}>
                {sourceLabel}
              </span>
            </div>
          </header>
          {component.goal && <p className="candidate-card__goal">{component.goal}</p>}
        </div>
      </button>
      <footer className="candidate-card__actions">
        {onDeviceAction && (
          <Button
            variant={isInstalled ? "secondary" : "primary"}
            size="small"
            className="candidate-card__install"
            aria-label={
              isInstalled
                ? `${component.name} 已同步到设备，点击从设备删除`
                : `同步 ${component.name} 到设备`
            }
            onClick={(event) => {
              event.stopPropagation();
              onDeviceAction();
            }}
          >
            {isInstalled ? <PackageCheck size={14} /> : <Download size={14} />}
            {isInstalled ? "已同步到设备（点击从设备删除）" : "同步到设备"}
          </Button>
        )}
        {isLocal && onDelete && (
          <Button
            variant="danger"
            size="small"
            className="candidate-card__delete"
            aria-label={`从电脑和设备删除 ${component.name}`}
            title="从电脑和设备删除"
            onClick={(event) => {
              event.stopPropagation();
              onDelete();
            }}
          >
            <Trash2 size={14} />
          </Button>
        )}
      </footer>
    </article>
  );
}
