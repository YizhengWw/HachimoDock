/**
 * [Input] component (Component | null), buttonBindings, deviceConnected, onChangeRequest.
 * [Output] Top "现在装着什么" hero card: bound state shows device-screen preview + button mappings
 *          + 更换 button; empty state is passive because the library is already below.
 * [Pos] component node in ref/src/component-center
 * [Sync] If this file changes, update `ref/src/component-center/.folder.md`.
 */

import React from "react";
import Card from "../shell/Card";
import DeviceScreenPreview from "./DeviceScreenPreview";
import { formatBindingControl } from "./binding-labels";

export default function NowShowingHero({
  component,
  buttonBindings,
  deviceConnected,
  onChangeRequest,
}) {
  if (!component) {
    return (
      <Card>
        <div className="now-showing-hero now-showing-hero--empty">
          <span className="now-showing-hero__empty-icon" aria-hidden>📦</span>
          <div className="now-showing-hero__empty-copy">
            <strong>设备还没装组件</strong>
            <span>从下方直接选择组件，或右上「创建组件」生成。</span>
          </div>
        </div>
      </Card>
    );
  }

  const isBuiltin = component.category === "内置案例";

  return (
    <Card>
      <div className="now-showing-hero">
        <div className="now-showing-hero__preview">
          <DeviceScreenPreview component={component} className="now-showing-hero__device" />
        </div>

        <div className="now-showing-hero__body">
          <header className="now-showing-hero__header">
            <h2>{component.name}</h2>
            <div className="now-showing-hero__chips">
              <span className="now-showing-hero__chip">
                {isBuiltin ? "内置" : "自定义"}
              </span>
              <span className="now-showing-hero__chip now-showing-hero__chip--active">
                运行中
              </span>
            </div>
          </header>

          {buttonBindings && buttonBindings.length > 0 && (
            <section className="now-showing-hero__bindings">
              <h3>按钮映射</h3>
              <ul>
                {buttonBindings.map((b, i) => (
                  <li key={b.action ?? i}>
                    <strong>{b.label}</strong>
                    <span className="now-showing-hero__binding-control">{formatBindingControl(b)}</span>
                  </li>
                ))}
              </ul>
            </section>
          )}

          <footer className="now-showing-hero__footer">
            <button
              className="btn-secondary"
              type="button"
              onClick={onChangeRequest}
              disabled={!deviceConnected}
            >
              更换组件
            </button>
            {!deviceConnected && (
              <span className="now-showing-hero__hint">设备离线，更换暂不可用</span>
            )}
          </footer>
        </div>
      </div>
    </Card>
  );
}
