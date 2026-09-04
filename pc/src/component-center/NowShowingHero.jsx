/**
 * [Input] component, explicit kind, target-verification state, resolved button bindings, device state, and configure/change/remove actions.
 * [Output] Top "现在装着什么" hero card: bound state shows device-screen preview + button mappings
 *          + source/type/enabled state + in-component button editing + change/remove actions;
 *          empty state is passive because the library is already below.
 * [Pos] component node in pc/src/component-center
 * [Sync] If this file changes, update `pc/src/component-center/.folder.md`.
 */

import React from "react";
import { Settings2, Unplug } from "lucide-react";
import Button from "../shell/Button";
import Card from "../shell/Card";
import DeviceScreenPreview from "./DeviceScreenPreview";
import { formatBindingControl } from "./binding-labels";
import { componentKindLabel, resolveComponentKind } from "./CandidateCard";

export default function NowShowingHero({
  component,
  kind,
  buttonBindings,
  deviceConnected,
  isTargetVerified = true,
  onConfigureButtons,
  onChangeRequest,
  onRemove,
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

  const isBuiltin = typeof component.category === "string"
    && component.category.startsWith("内置");
  const resolvedKind = resolveComponentKind(kind, component.gameType);

  return (
    <Card>
      <div
        className="now-showing-hero"
        aria-current={isTargetVerified ? "true" : undefined}
      >
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
              <span className={`now-showing-hero__chip now-showing-hero__chip--kind-${resolvedKind}`}>
                {componentKindLabel(resolvedKind)}
              </span>
              <span
                className={`now-showing-hero__chip ${
                  isTargetVerified
                    ? "now-showing-hero__chip--active"
                    : "now-showing-hero__chip--unverified"
                }`}
              >
                {isTargetVerified ? "已启用" : "上次启用 · 未确认设备"}
              </span>
            </div>
          </header>

          {buttonBindings && buttonBindings.length > 0 && (
            <section className="now-showing-hero__bindings">
              <header>
                <h3>按钮映射</h3>
                <Button
                  variant="ghost"
                  size="small"
                  className="now-showing-hero__configure"
                  onClick={onConfigureButtons}
                >
                  <Settings2 size={13} />
                  修改按钮
                </Button>
              </header>
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
            {isTargetVerified && onRemove && (
              <Button
                variant="danger"
                onClick={onRemove}
              >
                <Unplug size={14} />
                从设备移除
              </Button>
            )}
            <Button
              variant="secondary"
              onClick={onChangeRequest}
              disabled={!deviceConnected}
            >
              更换组件
            </Button>
            {!deviceConnected && (
              <span className="now-showing-hero__hint">设备离线，更换暂不可用</span>
            )}
            {!isTargetVerified && (
              <span className="now-showing-hero__hint">
                旧记录没有设备身份；重新安装后才能安全移除
              </span>
            )}
          </footer>
        </div>
      </div>
    </Card>
  );
}
