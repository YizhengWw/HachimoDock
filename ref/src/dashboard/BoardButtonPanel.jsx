/**
 * [Input] Connected runtime plus persisted button actions/values, OTA state, and update/apply callbacks.
 * [Output] Runtime-aware hardware-control workspace with the device map above
 *          physical-order SVG control cards, inline button/joystick gesture editors,
 *          repeatable actions across gestures, per-gesture Code Agent prompts,
 *          multi-trigger voice-input row hints, and USB sync feedback.
 * [Pos] component node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import React, { useState } from "react";
import { ChevronDown, UploadCloud, Mic, Usb } from "lucide-react";
import {
  BUTTON_FUNCTION_OPTIONS,
  DEFAULT_BUTTON_ACTIONS,
  DEFAULT_VOICE_CONFIG,
  actionOptionById,
  buttonControlRowsForRuntime,
} from "../DeviceDashboard.jsx";
import Button from "../shell/Button";

const CONTROL_GROUP_META = {
  p4_sw1: { label: "SW1", detail: "左侧按键" },
  p4_sw2: { label: "SW2", detail: "中间按键" },
  p4_sw3: { label: "SW3", detail: "右侧按键" },
  p4_joystick: { label: "摇杆", detail: "四向与中按" },
  legacy_encoder: { label: "前方旋钮", detail: "旋转与按压" },
};

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

function groupControlRows(controlRows, isP4Runtime) {
  const groups = [];
  const byId = new Map();
  controlRows.forEach((row) => {
    const groupId = isP4Runtime ? row.controlId || row.id : "legacy_encoder";
    if (!byId.has(groupId)) {
      const meta = CONTROL_GROUP_META[groupId] || {
        label: row.label,
        detail: "设备按键",
      };
      const group = { id: groupId, ...meta, rows: [] };
      groups.push(group);
      byId.set(groupId, group);
    }
    byId.get(groupId).rows.push(row);
  });
  return groups;
}

function gestureLabel(row, group) {
  return row.label
    .replace(new RegExp(`^${group.label}\\s*`), "")
    .replace(/^前方旋钮\s*/, "")
    .replace(/^摇杆\s*/, "")
    || row.label;
}

function ControlGlyph({ controlId, label, active = false }) {
  const isJoystick = controlId === "p4_joystick";
  const isEncoder = controlId === "legacy_encoder";
  return (
    <svg
      className={`board-button-control-glyph${isEncoder || isJoystick ? " is-encoder" : " is-key"}${isJoystick ? " is-joystick" : ""}${active ? " is-active" : ""}`}
      viewBox="0 0 120 92"
      role="img"
      aria-label={`${label} 实体控件示意`}
    >
      <rect className="board-button-control-glyph__deck" x="5" y="5" width="110" height="82" rx="22" />
      {isJoystick ? (
        <>
          <circle className="board-button-control-glyph__encoder-shadow" cx="60" cy="49" r="27" />
          <circle className="board-button-control-glyph__encoder" cx="60" cy="45" r="25" />
          <circle className="board-button-control-glyph__encoder-cap" cx="60" cy="45" r="15" />
          <path className="board-button-control-glyph__arrow" d="M60 12 V25 M55 18 L60 12 L65 18" />
          <path className="board-button-control-glyph__arrow" d="M60 78 V65 M55 72 L60 78 L65 72" />
          <path className="board-button-control-glyph__arrow" d="M22 45 H36 M28 40 L22 45 L28 50" />
          <path className="board-button-control-glyph__arrow" d="M98 45 H84 M92 40 L98 45 L92 50" />
        </>
      ) : isEncoder ? (
        <>
          <path className="board-button-control-glyph__rotation" d="M28 53 A34 34 0 0 1 92 53" />
          <path className="board-button-control-glyph__arrow" d="M28 53 L25 43 M28 53 L38 50" />
          <path className="board-button-control-glyph__arrow" d="M92 53 L95 43 M92 53 L82 50" />
          <circle className="board-button-control-glyph__encoder-shadow" cx="60" cy="47" r="29" />
          <circle className="board-button-control-glyph__encoder" cx="60" cy="43" r="27" />
          <circle className="board-button-control-glyph__encoder-cap" cx="60" cy="43" r="18" />
          <path className="board-button-control-glyph__encoder-mark" d="M60 19 V27" />
        </>
      ) : (
        <>
          <rect className="board-button-control-glyph__key-shadow" x="32" y="29" width="56" height="46" rx="15" />
          <rect className="board-button-control-glyph__key" x="32" y="23" width="56" height="46" rx="15" />
          <path className="board-button-control-glyph__press-mark" d="M49 14 H71 M54 9 L49 14 L54 19 M66 9 L71 14 L66 19" />
        </>
      )}
      <text className="board-button-control-glyph__label" x="60" y={isEncoder || isJoystick ? "49" : "52"} textAnchor="middle">
        {label}
      </text>
    </svg>
  );
}

export default function BoardButtonPanel({
  voiceConfig,
  buttonActions = DEFAULT_BUTTON_ACTIONS,
  buttonValues = {},
  buttonLabels = {},
  runtime = "",
  voiceConfigDirty,
  voiceConfigOtaState,
  usbConnected,
  selectedTrigger,
  onVoiceConfigChange,
  onApplyVoiceConfig,
}) {
  const [hoveredButtonId, setHoveredButton] = useState("");
  const triggerId = selectedTrigger?.id || DEFAULT_VOICE_CONFIG.trigger;
  const controlRows = buttonControlRowsForRuntime(runtime);
  const isP4Runtime = String(runtime || "").toLowerCase() === "esp-p4";
  const controlGroups = groupControlRows(controlRows, isP4Runtime);

  if (controlRows.length === 0) {
    return (
      <div className="board-button-panel__unverified" role="status">
        <Usb size={28} aria-hidden="true" />
        <strong>等待设备协议握手</strong>
        <span>{usbConnected ? "正在确认设备型号" : "尚未连接受支持的设备"}</span>
      </div>
    );
  }

  const onButtonActionChange = (row, actionId) => {
    const nextActions = { ...(buttonActions || DEFAULT_BUTTON_ACTIONS), [row.id]: actionId };
    const nextValues = { ...buttonValues };
    const nextLabels = { ...buttonLabels };
    const patch = { buttonActions: nextActions, buttonValues: nextValues, buttonLabels: nextLabels };
    if (actionId !== "miniapp_action") {
      delete nextLabels[row.id];
    }
    if (actionId === "agent_prompt" && !nextValues[row.id]) {
      nextValues[row.id] = row.defaultValue || "继续当前任务。";
    }
    if (actionId === "voice_ptt" && row.voiceTriggerId) {
      patch.trigger = row.voiceTriggerId;
    } else if (row.voiceTriggerId && triggerId === row.voiceTriggerId) {
      const fallbackVoiceRow = controlRows.find(
        (item) => item.voiceTriggerId && nextActions[item.id] === "voice_ptt",
      );
      patch.trigger = fallbackVoiceRow?.voiceTriggerId || DEFAULT_VOICE_CONFIG.trigger;
    }
    onVoiceConfigChange(patch);
  };

  const onButtonValueChange = (row, value) => {
    onVoiceConfigChange({ buttonValues: { ...buttonValues, [row.id]: value } });
  };

  const isControlHovered = (controlId) => hoveredButtonId === controlId
    || controlRows.some((row) => row.controlId === controlId && row.id === hoveredButtonId);

  return (
    <div className="board-button-panel" data-testid="board-button-config-card">
      <div className="board-button-panel__toolbar">
        <div className="board-button-panel__sync-state">
          <h2>按钮配置</h2>
          <strong>{controlGroups.length} 个控件 <i /> {controlRows.length} 个手势</strong>
          <span className={`board-button-panel__sync-label${voiceConfigDirty ? " is-dirty" : ""}`}>
            {voiceConfigDirty ? "待同步" : "已同步"}
          </span>
        </div>
        <div className="board-button-panel__toolbar-actions">
          <Button
            variant="primary"
            size="medium"
            className={`voice-config-apply-btn${voiceConfigDirty ? " is-dirty" : ""}`}
            onClick={onApplyVoiceConfig}
            loading={voiceConfigOtaState?.pending}
            loadingLabel="正在同步…"
            disabled={voiceConfigOtaState?.pending || !usbConnected}
            title={usbConnected ? "通过 USB 将当前映射同步到设备" : "需要 USB 连接设备后同步"}
          >
            <UploadCloud size={14} />
            同步到设备
          </Button>
        </div>
      </div>

      {voiceConfigOtaState?.message && (
        <div
          className={`board-button-panel__hint board-button-panel__hint--${voiceConfigOtaState.tone || "info"}`}
          role="status"
          aria-live="polite"
        >
          {voiceConfigOtaState.message}
        </div>
      )}

      <div className={`board-button-panel__workspace${isP4Runtime ? " board-button-panel__workspace--p4" : ""}`}>
        <section className="board-button-panel__left" aria-label="设备控件导航">
          <div className="board-button-panel__device-stage">
            <svg
              className="board-button-map__device board-button-panel__svg"
              viewBox={isP4Runtime ? "0 0 456 320" : "0 0 560 320"}
              role="img"
              aria-label="板端外观和按钮位置示意"
            >
              {/* Hit-areas mirror the physical deck and link to the focused editor. */}
              {isP4Runtime ? (
                <>
                  <rect className="board-button-map__body board-button-map__body--p4" x="48" y="20" width="360" height="278" rx="32" />
                  <rect className="board-button-map__screen-bezel board-button-map__screen-bezel--p4" x="70" y="40" width="316" height="154" rx="20" />
                  <rect className="board-button-map__screen board-button-map__screen--p4" x="84" y="54" width="288" height="126" rx="10" />
                  <circle className="board-button-map__screen-status" cx="108" cy="82" r="4" />
                  <text className="board-button-map__screen-kicker" x="120" y="86">HARDWARE INPUT</text>
                  <text className="board-button-map__screen-title" x="108" y="128">
                    12 GESTURES
                  </text>
                  <text className="board-button-map__screen-copy" x="108" y="153">
                    SW1 · SW2 · SW3 · JOYSTICK
                  </text>
                  <rect className="board-button-map__control-deck" x="66" y="206" width="324" height="76" rx="22" />
                  {[
                    ["p4_sw1", "p4_sw1_short", 84, "SW1"],
                    ["p4_sw2", "p4_sw2_short", 150, "SW2"],
                    ["p4_sw3", "p4_sw3_short", 216, "SW3"],
                  ].map(([controlId, activeActionId, x, label]) => (
                    <g
                      key={controlId}
                      onMouseEnter={() => setHoveredButton(controlId)}
                      onMouseLeave={() => setHoveredButton("")}
                      data-button-id={controlId}
                    >
                      <rect
                        className={[
                          "board-button-map__hardware-key",
                          controlId === "p4_sw1"
                            && buttonActions.p4_sw1_long === "voice_ptt"
                            && voiceConfig.enabled
                            ? "is-active"
                            : "",
                          isControlHovered(controlId) ? "is-hovered" : "",
                        ].filter(Boolean).join(" ")}
                        x={x} y="218" width="48" height="48" rx="12"
                      />
                      <text className="board-button-map__hardware-key-label" x={x + 24} y="247" textAnchor="middle">
                        {label}
                      </text>
                    </g>
                  ))}
                  <circle
                    className={[
                      "board-button-map__encoder",
                      "board-button-map__encoder--p4",
                    ].filter(Boolean).join(" ")}
                    cx="328" cy="242" r="32"
                    onMouseEnter={() => setHoveredButton("p4_joystick")}
                    onMouseLeave={() => setHoveredButton("")}
                    data-button-id="p4_joystick"
                  />
                  <circle className="board-button-map__encoder-cap" cx="328" cy="242" r="24" />
                  <text className="board-button-map__encoder-label" x="328" y="246" textAnchor="middle">
                    摇杆
                  </text>
                </>
              ) : (
                <>
                  <rect className="board-button-map__body" x="72" y="58" width="374" height="200" rx="32" />
                  <rect className="board-button-map__screen-bezel" x="100" y="80" width="198" height="130" rx="18" />
                  <rect className="board-button-map__screen" x="119" y="95" width="160" height="100" rx="8" />
                  <circle
                    className={`board-button-map__encoder${buttonActions.encoder_button === "voice_ptt" && voiceConfig.enabled ? " is-active" : ""}`}
                    cx="370" cy="117" r="48"
                    onMouseEnter={() => setHoveredButton("encoder_button")}
                    onMouseLeave={() => setHoveredButton("")}
                    data-button-id="encoder_button"
                  />
                </>
              )}
              {!isP4Runtime && controlRows.map((row) => [row.id, CALLOUT_PATHS[row.id]]).filter(([, d]) => d).map(([id, d]) => (
                <path key={id} className={`board-button-map__callout${hoveredButtonId === id ? " is-hovered" : ""}`} d={d} />
              ))}
              {!isP4Runtime && controlRows.map((row) => {
                const anchor = CALLOUT_ANCHORS[row.id];
                if (!anchor) return null;
                const action = actionOptionById(buttonActions[row.id] || row.defaultAction);
                return renderCalloutLabel(row, action, anchor, hoveredButtonId === row.id);
              })}
            </svg>
          </div>
        </section>

        <section className="board-button-panel__right" aria-label="实体控件与手势配置">
          <div className="board-button-control-groups">
            {controlGroups.map((group, groupIndex) => (
              <fieldset
                className={`board-button-control-group board-button-control-group--${group.rows.length}`}
                key={group.id}
                onMouseEnter={() => setHoveredButton(group.id)}
                onMouseLeave={() => setHoveredButton("")}
              >
                <legend>
                  <span>{String(groupIndex + 1).padStart(2, "0")}</span>
                  <small>{group.label} · {group.detail}</small>
                </legend>
                <div className="board-button-control-group__visual">
                  <ControlGlyph
                    controlId={group.id}
                    label={group.label}
                    active={group.rows.some(
                      (row) => buttonActions?.[row.id] === "voice_ptt" && voiceConfig.enabled,
                    )}
                  />
                  <span>
                    <strong>{group.label}</strong>
                    <small>
                      {group.id === "p4_joystick"
                        ? "上、下、左、右与中按分别配置"
                        : "短按、长按分别配置"}
                    </small>
                  </span>
                </div>
                <div className={`board-button-control-group__rows board-button-control-group__rows--${group.rows.length}`}>
                  {group.rows.map((row) => {
                    const currentActionId = buttonActions?.[row.id] || row.defaultAction;
                    const isShortPress = isP4Runtime && row.event.endsWith(".short_press");
                    const allowedOptions = BUTTON_FUNCTION_OPTIONS.filter((option) =>
                      row.actionOptions.includes(option.id)
                        || (isShortPress && option.id === "voice_ptt"),
                    );
                    const isVoicePttRow = currentActionId === "voice_ptt";
                    const rowHovered = hoveredButtonId === row.id || (row.controlId && hoveredButtonId === row.controlId);
                    const fieldId = `button-action-${row.id}`;
                    const promptId = `button-prompt-${row.id}`;
                    return (
                      <div
                        className={`board-button-control-field${rowHovered ? " is-hovered" : ""}${currentActionId === "agent_prompt" ? " has-prompt" : ""}`}
                        key={row.id}
                        data-event={row.event}
                        onMouseEnter={() => setHoveredButton(row.id)}
                        onMouseLeave={() => setHoveredButton(group.id)}
                      >
                        <span className="board-button-control-field__head">
                          <label htmlFor={fieldId}>{gestureLabel(row, group)}</label>
                          {isVoicePttRow && (
                            <span className={`board-button-panel__voice-chip${voiceConfig.enabled ? " is-on" : ""}`}>
                              <Mic size={11} />
                              {voiceConfig.enabled ? "语音输入已开启" : "语音输入未开启"}
                            </span>
                          )}
                        </span>
                        {allowedOptions.length <= 1 ? (
                          <span className="voice-button-action-fixed" id={fieldId}>
                            {allowedOptions[0]?.label || actionOptionById(row.defaultAction).label}
                          </span>
                        ) : (
                          <span className="voice-button-action-select-shell">
                            <select
                              id={fieldId}
                              className="voice-button-action-select"
                              value={currentActionId}
                              aria-label={`${group.label}${gestureLabel(row, group)}功能`}
                              onChange={(event) => onButtonActionChange(row, event.target.value)}
                            >
                              {allowedOptions.map((option) => {
                                const requiresLongPress = option.id === "voice_ptt" && isShortPress;
                                const disabledReason = requiresLongPress
                                  ? "需要长按"
                                  : "";
                                return (
                                  <option
                                    key={option.id}
                                    value={option.id}
                                    disabled={Boolean(disabledReason)}
                                    title={disabledReason || undefined}
                                  >
                                    {option.label}{disabledReason ? `（${disabledReason}）` : ""}
                                  </option>
                                );
                              })}
                            </select>
                            <ChevronDown
                              className="voice-button-action-select-shell__chevron"
                              size={14}
                              aria-hidden="true"
                            />
                          </span>
                        )}
                        {currentActionId === "agent_prompt" && (
                          <label className="board-button-control-field__prompt" htmlFor={promptId}>
                            <span>
                              <strong>自定义指令</strong>
                              <small>按键后直接发送给 Code Agent</small>
                            </span>
                            <textarea
                              id={promptId}
                              className="voice-button-action-value"
                              value={buttonValues[row.id] || row.defaultValue || ""}
                              onChange={(event) => onButtonValueChange(row, event.target.value)}
                              maxLength={120}
                              rows={3}
                              placeholder="例如：总结当前进度，检查报错并继续实现。"
                              aria-label={`${row.label} 发送给 Code Agent 的自定义指令`}
                            />
                            <small className="board-button-control-field__prompt-count">
                              {String(buttonValues[row.id] || row.defaultValue || "").length}/120
                            </small>
                          </label>
                        )}
                      </div>
                    );
                  })}
                </div>
              </fieldset>
            ))}
          </div>
        </section>
      </div>
    </div>
  );
}
