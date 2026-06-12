/**
 * [Input] useDeviceContext for binding/usb/online/currentDisplay/currentComponent; navigation callbacks for the three rows + bind CTA.
 * [Output] Sidebar-bottom rail showing device chip + current appearance + current component, collapsing to a single bind CTA when no binding.
 * [Pos] component node in ref/src/shell
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import React from "react";
import {
  Monitor,
  Wifi,
  WifiOff,
  Usb,
  Image as ImageIcon,
  Blocks,
  Plus,
} from "lucide-react";
import { useDeviceContext } from "./DeviceContext.jsx";

export default function ContextRail({
  onOpenDevice,
  onOpenAppearance,
  onOpenComponent,
  onStartBinding,
}) {
  const { binding, usb, deviceOnline, currentDisplay, currentComponent } = useDeviceContext();

  if (!binding) {
    return (
      <div className="context-rail context-rail--empty">
        <button type="button" className="context-rail__cta" onClick={onStartBinding}>
          <Plus size={14} />
          <span>绑定设备</span>
        </button>
      </div>
    );
  }

  const connectionIcon = usb.connected ? <Usb size={12} /> : deviceOnline ? <Wifi size={12} /> : <WifiOff size={12} />;
  const connectionLabel = usb.connected ? "USB" : deviceOnline ? "在线" : "离线";

  return (
    <div className="context-rail">
      <button type="button" className="context-rail__row" onClick={onOpenDevice} title="打开设备页">
        <Monitor size={14} />
        <span className="context-rail__primary">{binding.boardDeviceId}</span>
        <span className={`context-rail__chip context-rail__chip--${usb.connected ? "ok" : deviceOnline ? "ok" : "warn"}`}>
          {connectionIcon}
          {connectionLabel}
        </span>
      </button>

      <button type="button" className="context-rail__row" onClick={onOpenAppearance} title="打开形象画廊">
        <ImageIcon size={14} />
        <span className="context-rail__primary">
          {currentDisplay.appearance?.name || "未配置形象"}
        </span>
        {currentDisplay.channelLabel && (
          <span className="context-rail__muted">{currentDisplay.channelLabel}</span>
        )}
      </button>

      <button type="button" className="context-rail__row" onClick={onOpenComponent} title="打开组件中心">
        <Blocks size={14} />
        <span className="context-rail__primary">
          {currentComponent?.name || "未选择组件"}
        </span>
      </button>
    </div>
  );
}
