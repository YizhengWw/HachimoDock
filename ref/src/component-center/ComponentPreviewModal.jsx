/**
 * [Input] component, isDraft, currentComponent, deviceConnected, installing, onInstall, onClose.
 * [Output] Full-screen confirm modal: large device-screen preview, button-mappings list,
 *          replace-warning banner, offline warning, and Install / Cancel footer actions.
 * [Pos] component node in ref/src/component-center
 * [Sync] If this file changes, update `ref/src/component-center/.folder.md`.
 */

import React from "react";
import { X, Loader } from "lucide-react";
import DeviceScreenPreview from "./DeviceScreenPreview";
import { formatBindingControl, isRoutedWidgetBinding } from "./binding-labels";

export default function ComponentPreviewModal({
  component,
  isDraft,
  currentComponent,
  deviceConnected,
  installing,
  onInstall,
  onClose,
}) {
  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal-card modal-card--preview" onClick={(e) => e.stopPropagation()}>
        <div className="modal-header">
          <div>
            <h3 className="modal-title">装到设备</h3>
            <div className="modal-subtitle">{component.name}</div>
          </div>
          <button className="icon-btn" onClick={onClose} aria-label="关闭" disabled={installing}>
            <X size={16} />
          </button>
        </div>
        <div className="modal-body">
          <div className="component-preview-modal__layout">
            <div className="component-preview-modal__screen">
              <DeviceScreenPreview component={component} className="component-preview-modal__device" />
            </div>
            <div className="component-preview-modal__meta">
              <span className={`candidate-card__badge candidate-card__badge--${isDraft ? "custom" : "builtin"}`}>
                {isDraft ? "自定义" : "内置"}
              </span>
              {component.goal && <p>{component.goal}</p>}

              {component.defaultBindings && component.defaultBindings.filter(isRoutedWidgetBinding).length > 0 && (
                <section className="component-preview-modal__bindings">
                  <h4>按钮映射</h4>
                  <ul>
                    {component.defaultBindings.filter(isRoutedWidgetBinding).map((b, i) => (
                      <li key={b.action || i}>
                        <strong>{b.label}</strong>
                        <span className="component-preview-modal__binding-control">{formatBindingControl(b)}</span>
                      </li>
                    ))}
                  </ul>
                </section>
              )}
            </div>
          </div>
          {currentComponent && currentComponent.id !== component.id && (
            <div className="message-banner message-banner--warning">
              安装后将替换当前的 <strong>{currentComponent.name}</strong>
            </div>
          )}
          {!deviceConnected && (
            <div className="message-banner message-banner--warning">
              设备离线，需要 USB 直连或上线后才能安装
            </div>
          )}
        </div>
        <div className="modal-footer component-preview-modal__footer">
          <button className="btn-secondary" type="button" onClick={onClose} disabled={installing}>
            取消
          </button>
          <button
            className="btn-primary"
            type="button"
            onClick={onInstall}
            disabled={installing || !deviceConnected}
          >
            {installing ? (
              <>
                <Loader size={14} className="spin" /> 安装中…
              </>
            ) : (
              "安装到设备"
            )}
          </button>
        </div>
      </div>
    </div>
  );
}
