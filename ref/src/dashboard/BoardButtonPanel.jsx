/**
 * [Input] voiceConfig + buttonActions + voiceConfigDirty + voiceConfigOtaState + usbConnected + selectedTrigger + onVoiceConfigChange + onApplyVoiceConfig.
 * [Output] Region 3 of the device dashboard: larger non-overlapping SVG callouts + compact button editor + USB OTA dispatch button. Hovering an SVG button highlights its row below.
 * [Pos] component node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import React, { useState } from "react";
import { ChevronDown, Loader, UploadCloud, Mic } from "lucide-react";
import {
  BOARD_BUTTON_CONTROL_ROWS,
  BUTTON_FUNCTION_OPTIONS,
  DEFAULT_BUTTON_ACTIONS,
  DEFAULT_VOICE_CONFIG,
  actionOptionById,
} from "../DeviceDashboard.jsx";

// Each callout has an (x,y) anchor in the enlarged SVG viewport where the label is drawn.
const CALLOUT_ANCHORS = {
  encoder_button_short: { x: 476, y: 58,  align: "start" },
  encoder_button:       { x: 476, y: 110, align: "start" },
  encoder_rotate:       { x: 476, y: 162, align: "start" },
};

// Callout path d-attributes — endpoint coordinates match the visual button center.
const CALLOUT_PATHS = {
  encoder_button_short: "M370 117 L468 62",
  encoder_button:       "M370 117 L468 114",
  encoder_rotate:       "M370 117 L468 166",
};

function renderCalloutLabel(row, action, anchor, isHovered) {
  return (
    <text
      key={row.id}
      className={`board-button-panel__callout-label${isHovered ? " is-hovered" : ""}`}
      x={anchor.x}
      y={anchor.y}
      textAnchor={anchor.align}
      data-button-id={row.id}
      aria-label={`${row.label}: ${action.label}`}
    >
      <tspan className="board-button-panel__callout-label-name" x={anchor.x}>
        {row.label}
      </tspan>
      <tspan className="board-button-panel__callout-label-action" x={anchor.x} dy="14">
        {action.label}
      </tspan>
    </text>
  );
}

export default function BoardButtonPanel({
  voiceConfig,
  buttonActions = DEFAULT_BUTTON_ACTIONS,
  voiceConfigDirty,
  voiceConfigOtaState,
  usbConnected,
  selectedTrigger,
  onVoiceConfigChange,
  onApplyVoiceConfig,
}) {
  const [hoveredButtonId, setHoveredButton] = useState("");
  const triggerId = selectedTrigger?.id || DEFAULT_VOICE_CONFIG.trigger;

  const onButtonActionChange = (row, actionId) => {
    const nextActions = { ...(buttonActions || DEFAULT_BUTTON_ACTIONS), [row.id]: actionId };
    const patch = { buttonActions: nextActions };
    if (actionId === "voice_ptt" && row.voiceTriggerId) {
      BOARD_BUTTON_CONTROL_ROWS.forEach((item) => {
        if (item.voiceTriggerId && item.id !== row.id && nextActions[item.id] === "voice_ptt") {
          nextActions[item.id] = item.defaultAction;
        }
      });
      patch.trigger = row.voiceTriggerId;
    } else if (row.voiceTriggerId && triggerId === row.voiceTriggerId) {
      const fallbackVoiceRow = BOARD_BUTTON_CONTROL_ROWS.find(
        (item) => item.voiceTriggerId && nextActions[item.id] === "voice_ptt",
      );
      patch.trigger = fallbackVoiceRow?.voiceTriggerId || DEFAULT_VOICE_CONFIG.trigger;
    }
    onVoiceConfigChange(patch);
  };

  return (
    <div className="board-button-panel" data-testid="board-button-config-card">
      <div className="board-button-panel__left">
        <svg
          className="board-button-map__device board-button-panel__svg"
          viewBox="0 0 560 320"
          role="img"
          aria-label="板端外观和按钮位置示意"
        >
          <rect className="board-button-map__body" x="72" y="58" width="374" height="200" rx="32" />
          <rect className="board-button-map__screen-bezel" x="100" y="80" width="198" height="130" rx="18" />
          <rect className="board-button-map__screen" x="119" y="95" width="160" height="100" rx="8" />

          {/* Hit-areas for hardware controls. onMouseEnter sets hoveredButtonId so the row below highlights. */}
          <circle
            className={`board-button-map__encoder${buttonActions.encoder_button === "voice_ptt" && voiceConfig.enabled ? " is-active" : ""}`}
            cx="370" cy="117" r="48"
            onMouseEnter={() => setHoveredButton("encoder_button")}
            onMouseLeave={() => setHoveredButton("")}
            data-button-id="encoder_button"
          />
          {/* Callout paths from each button to its label anchor. */}
          {Object.entries(CALLOUT_PATHS).map(([id, d]) => (
            <path key={id} className={`board-button-map__callout${hoveredButtonId === id ? " is-hovered" : ""}`} d={d} />
          ))}

          {/* Action labels next to each callout — the spec upgrade. */}
          {BOARD_BUTTON_CONTROL_ROWS.map((row) => {
            const anchor = CALLOUT_ANCHORS[row.id];
            if (!anchor) return null;
            const action = actionOptionById(buttonActions[row.id] || row.defaultAction);
            return renderCalloutLabel(row, action, anchor, hoveredButtonId === row.id);
          })}
        </svg>
        {voiceConfigOtaState?.message && (
          <div className={`board-button-panel__hint board-button-panel__hint--${voiceConfigOtaState.tone || "info"}`}>
            {voiceConfigOtaState.message}
          </div>
        )}
      </div>

      <div className="board-button-panel__right">
        <div className="voice-button-action-list">
          <div className="voice-config-field__head">
            <span>按钮功能</span>
            <small>需 USB OTA 生效</small>
          </div>
          {BOARD_BUTTON_CONTROL_ROWS.map((row) => {
            const currentActionId = buttonActions?.[row.id] || row.defaultAction;
            const allowedOptions = BUTTON_FUNCTION_OPTIONS.filter((option) =>
              row.actionOptions.includes(option.id),
            );
            const isVoicePttRow = currentActionId === "voice_ptt";
            const rowClass = `voice-button-action-row${hoveredButtonId === row.id ? " is-hovered" : ""}`;
            return (
              <label
                className={rowClass}
                key={row.id}
                onMouseEnter={() => setHoveredButton(row.id)}
                onMouseLeave={() => setHoveredButton("")}
              >
                <span className="voice-button-action-row__head">
                  <strong className="voice-button-action-row__title">{row.label}</strong>
                  {isVoicePttRow && (
                    <span className={`board-button-panel__voice-chip${voiceConfig.enabled ? " is-on" : ""}`}>
                      <Mic size={12} />
                      {voiceConfig.enabled ? "语音助手已开启" : "未开启"}
                    </span>
                  )}
                  <span className="voice-button-action-row__event" title={row.event}>{row.event.split(" / ")[0]}</span>
                </span>
                {allowedOptions.length <= 1 ? (
                  <span className="voice-button-action-fixed">
                    {allowedOptions[0]?.label || actionOptionById(row.defaultAction).label}
                  </span>
                ) : (
                  <span className="voice-button-action-select-shell">
                    <select
                      className="voice-button-action-select"
                      value={currentActionId}
                      onChange={(event) => onButtonActionChange(row, event.target.value)}
                    >
                      {allowedOptions.map((option) => (
                        <option key={option.id} value={option.id}>{option.label}</option>
                      ))}
                    </select>
                    <ChevronDown
                      className="voice-button-action-select-shell__chevron"
                      size={14}
                      aria-hidden="true"
                    />
                  </span>
                )}
              </label>
            );
          })}
        </div>

        <div className="voice-config-footer">
          <button
            type="button"
            className={`btn-primary btn-sm voice-config-apply-btn${voiceConfigDirty ? " is-dirty" : ""}`}
            onClick={onApplyVoiceConfig}
            disabled={voiceConfigOtaState?.pending || !usbConnected}
            title={usbConnected ? "" : "需要 USB 连接设备后下发"}
          >
            {voiceConfigOtaState?.pending ? (
              <Loader size={14} className="spin" />
            ) : (
              <UploadCloud size={14} />
            )}
            通过 USB OTA 下发按钮配置
          </button>
          <span className={`voice-config-ota-note${voiceConfigDirty ? " is-dirty" : ""}`}>
            {voiceConfigDirty ? "有未下发配置" : "按钮配置已保存"}
          </span>
        </div>
      </div>
    </div>
  );
}
