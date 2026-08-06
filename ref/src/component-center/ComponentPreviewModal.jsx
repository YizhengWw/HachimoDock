/**
 * [Input] component metadata, exact-source current state, resolved gameplay bindings,
 *         board-authoritative global-exit control, component-scope/conflict state, live-device state, and actions.
 * [Output] Structured preview/sync confirmation with a device-preview overview,
 *          dedicated button-mapping workspace, duplicate-input guard, consolidated
 *          sync-impact panel without ambiguous current-component replacement copy,
 *          dynamic global-return guidance, and context-aware confirmation footer.
 * [Pos] component node in ref/src/component-center
 * [Sync] If this file changes, update `ref/src/component-center/.folder.md`.
 */

import React, { useEffect, useRef } from "react";
import { AlertTriangle, Download, RotateCcw, Save, Settings2, Trash2, Unplug, X } from "lucide-react";
import Button from "../shell/Button";
import { componentKindLabel, resolveComponentKind } from "./CandidateCard";
import DeviceScreenPreview from "./DeviceScreenPreview";

export default function ComponentPreviewModal({
  component,
  kind,
  isLocal,
  isInstalled = false,
  currentComponent,
  isCurrent,
  singleSlotReplacement = false,
  deviceConnected,
  installing,
  bindings = [],
  bindingConflict = "",
  globalExitControl = "SW1",
  getControlOptions,
  onBindingChange,
  onResetBindings,
  componentButtonsWillApply = false,
  installBlockedReason = "",
  onInstall,
  onRemove,
  onDelete,
  onClose,
}) {
  const isDeviceOnly = component.isDeviceOnly === true;
  const isUpdatingCurrent = !isDeviceOnly && (
    typeof isCurrent === "boolean"
      ? isCurrent
      : Boolean(currentComponent?.id && currentComponent.id === component.id)
  );
  const switchControls = ["SW1", "SW2", "SW3"];
  const resolvedKind = resolveComponentKind(kind, component.gameType);
  const componentKind = componentKindLabel(resolvedKind);
  const dialogRef = useRef(null);
  const returnFocusRef = useRef(null);
  const onCloseRef = useRef(onClose);
  const installingRef = useRef(installing);
  onCloseRef.current = onClose;
  installingRef.current = installing;

  useEffect(() => {
    if (!returnFocusRef.current) returnFocusRef.current = document.activeElement;
    const dialogElement = dialogRef.current;
    dialogRef.current?.querySelector("button")?.focus();
    const handleKeyDown = (event) => {
      if (event.key === "Escape" && !installingRef.current) {
        event.preventDefault();
        onCloseRef.current?.();
        return;
      }
      if (event.key !== "Tab") return;
      const focusable = Array.from(dialogRef.current?.querySelectorAll(
        'button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [href], [tabindex]:not([tabindex="-1"])',
      ) || []);
      if (focusable.length === 0) return;
      const first = focusable[0];
      const last = focusable[focusable.length - 1];
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault();
        last.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault();
        first.focus();
      }
    };
    document.addEventListener("keydown", handleKeyDown);
    return () => {
      document.removeEventListener("keydown", handleKeyDown);
      window.setTimeout(() => {
        if (dialogElement?.isConnected) return;
        const previous = returnFocusRef.current;
        if (previous?.isConnected && typeof previous.focus === "function") previous.focus();
      }, 0);
    };
  }, []);

  return (
    <div
      className="modal-backdrop"
      onClick={() => {
        if (!installing) onClose();
      }}
    >
      <div
        ref={dialogRef}
        className="modal-card modal-card--preview"
        role="dialog"
        aria-modal="true"
        aria-labelledby="component-preview-modal-title"
        onClick={(e) => e.stopPropagation()}
      >
        <div className="modal-header">
          <div>
            <h3 className="modal-title" id="component-preview-modal-title">
              {isUpdatingCurrent
                  ? (resolvedKind === "game" ? "调整小游戏按钮" : "调整工具组件")
                : isDeviceOnly
                  ? "设备包详情"
                : isInstalled
                  ? "已同步组件详情"
                  : "装到设备"}
            </h3>
            <div className="modal-subtitle">{component.name}</div>
          </div>
          <Button
            variant="icon"
            size="small"
            onClick={onClose}
            aria-label="关闭组件详情"
            disabled={installing}
          >
            <X size={16} />
          </Button>
        </div>
        <div className="modal-body">
          <div className="component-preview-modal__layout">
            <aside className="component-preview-modal__overview" aria-label="组件预览与说明">
              <div className="component-preview-modal__screen">
                <DeviceScreenPreview component={component} className="component-preview-modal__device" />
              </div>
              <div className="component-preview-modal__summary">
                <div className="component-preview-modal__summary-tags">
                  <span className={`candidate-card__badge candidate-card__badge--${isLocal ? "custom" : "builtin"}`}>
                    {component.isDeviceOnly ? "设备包" : isLocal ? "正式本地" : "内置"}
                  </span>
                  <span className="component-preview-modal__kind">{componentKind}</span>
                </div>
                {component.goal && <p>{component.goal}</p>}
              </div>
            </aside>
            <div className="component-preview-modal__workspace">
              {bindings.length > 0 && (
                <section className="component-preview-modal__bindings">
                  <header>
                    <div>
                      <h4><Settings2 size={13} /> 这个组件的按钮</h4>
                      <p>动作逻辑不变，只调整由哪个实体按键触发。</p>
                    </div>
                    <Button
                      variant="ghost"
                      size="small"
                      onClick={onResetBindings}
                      disabled={installing}
                    >
                      <RotateCcw size={14} /> 恢复默认
                    </Button>
                  </header>
                  <div className="component-preview-modal__switch-strip" aria-label="SW1、SW2、SW3 当前映射">
                    {switchControls.map((control) => {
                      const assigned = bindings.filter((binding) => binding.control === control);
                      return (
                        <div
                          className={`component-preview-modal__switch${assigned.length ? " is-assigned" : ""}`}
                          key={control}
                        >
                          <span>{control}</span>
                          <strong>
                            {assigned.length
                              ? assigned.map((binding) => binding.label).join(" / ")
                              : control === globalExitControl ? "全局退出" : "可分配"}
                          </strong>
                        </div>
                      );
                    })}
                  </div>
                  <ul>
                    {bindings.map((b, i) => (
                      <li key={b.action || i}>
                        <div>
                          <strong>{b.label}</strong>
                          <small>{b.controlHelp}</small>
                        </div>
                        <select
                          className="component-preview-modal__binding-select"
                          aria-label={`${b.label} 的按钮`}
                          value={b.controlLabel}
                          onChange={(event) => onBindingChange?.(b, event.target.value)}
                          disabled={installing}
                        >
                          {(getControlOptions?.(b) || []).map((option) => (
                            <option
                              key={option.event}
                              value={option.label}
                              disabled={option.disabled}
                            >
                              {option.label}{option.disabled ? `（${option.disabledReason || "已占用"}）` : ""}
                            </option>
                          ))}
                        </select>
                      </li>
                    ))}
                  </ul>
                  {bindingConflict && (
                    <p className="component-preview-modal__binding-error">
                      按钮冲突：{bindingConflict}
                    </p>
                  )}
                </section>
              )}
            </div>
          </div>
          {(componentButtonsWillApply
            || installBlockedReason
            || !deviceConnected
            || isDeviceOnly
            || singleSlotReplacement) && (
            <div className="component-preview-modal__impact" role="note">
              <div className="component-preview-modal__impact-title">
                <AlertTriangle size={15} aria-hidden="true" />
                <strong>{isDeviceOnly ? "设备包信息" : "同步影响"}</strong>
              </div>
              <div>
                {singleSlotReplacement && currentComponent && (
                  <p>
                    <b>当前设备是单槽模式。</b>
                    确认后会用这个组件替换 <b>{currentComponent.name}</b>；
                    旧组件不会保留在板端，但仍保留在本机组件库。
                  </p>
                )}
                {isInstalled && !isUpdatingCurrent && !isDeviceOnly && !singleSlotReplacement && (
                  <p>这个组件已同步到设备；确认后会重新同步配置和按钮。</p>
                )}
                {componentButtonsWillApply && (
                  <p>
                    <b>组件按钮仅包含游戏或工具动作。</b>
                    退出始终跟随设备全局设置；当前退出键是 {globalExitControl}，不会写入组件包。
                  </p>
                )}
                {installBlockedReason && (
                  <p><b>暂时无法安装。</b>{installBlockedReason}</p>
                )}
                {!deviceConnected && (
                  <p>设备离线，需要 USB 直连或上线后才能同步</p>
                )}
              </div>
            </div>
          )}
        </div>
        <div className="modal-footer component-preview-modal__footer">
          <p className="component-preview-modal__footer-copy">
            {isDeviceOnly
              ? "这是设备回报的组件包；本机没有对应安装源。"
              : isUpdatingCurrent
              ? "保存后会立即同步组件与按钮映射。"
              : singleSlotReplacement
                ? "确认后会替换板端唯一组件，并立即启用。"
              : isInstalled
                ? "确认后会重新同步组件配置和按钮。"
              : "确认后会通过当前设备连接下发组件与按钮映射。"}
          </p>
          <div className="component-preview-modal__footer-actions">
            {isLocal && typeof onDelete === "function" && (
              <Button
                variant="danger"
                className="component-preview-modal__action component-preview-modal__action--delete"
                onClick={onDelete}
                disabled={installing}
              >
                <Trash2 size={15} />
                {isInstalled ? "从电脑和设备删除" : "从电脑删除"}
              </Button>
            )}
            {(isInstalled || isDeviceOnly) && onRemove && (
              <Button
                variant="danger"
                className="component-preview-modal__action component-preview-modal__action--remove"
                onClick={onRemove}
                disabled={installing}
              >
                <Unplug size={15} />
                从设备移除
              </Button>
            )}
            <Button
              variant="secondary"
              className="component-preview-modal__action"
              onClick={onClose}
              disabled={installing}
            >
              {isDeviceOnly ? "关闭" : "取消"}
            </Button>
            {!isDeviceOnly && typeof onInstall === "function" && (
              <Button
                variant="primary"
                className="component-preview-modal__action"
                onClick={onInstall}
                loading={installing}
                loadingLabel="同步中…"
                disabled={
                  !deviceConnected
                  || Boolean(bindingConflict)
                  || Boolean(installBlockedReason)
                }
              >
                {isUpdatingCurrent || isInstalled ? (
                  <><Save size={15} /> 保存并同步</>
                ) : singleSlotReplacement ? (
                  <><Download size={15} /> 替换并启用</>
                ) : (
                  <><Download size={15} /> 同步到设备</>
                )}
              </Button>
            )}
          </div>
        </div>
      </div>
    </div>
  );
}
