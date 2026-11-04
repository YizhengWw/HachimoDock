import React, { useEffect, useId, useMemo, useRef, useSyncExternalStore } from "react";
import { invoke } from "@tauri-apps/api/core";
import { createSharedLocationController, describeSharedLocation } from "./shared-location-model.js";
import "./shared-location.css";
import { useExitProtection } from "../lib/exit-protection.js";

export default function SharedLocationPanel({ active = false, invokeCommand = invoke, controller: suppliedController }) {
  const id = useId();
  const composing = useRef(false);
  const suggestionList = useRef(null);
  const controller = useMemo(() => suppliedController || createSharedLocationController({ invoke: invokeCommand }), [suppliedController, invokeCommand]);
  const state = useSyncExternalStore(controller.subscribe, controller.getSnapshot, controller.getSnapshot);
  useEffect(() => {
    controller.setActive(active);
    return () => {
      composing.current = false;
      controller.setActive(false);
    };
  }, [active, controller]);
  const { draft, snapshot, saving, validation } = state;
  const suggestions = state.suggestions || [];
  const suggestionIndex = state.suggestionIndex ?? -1;
  const suggestionsOpen = active === true && !saving && suggestions.length > 0;
  useEffect(() => {
    if (suggestionsOpen && suggestionIndex >= 0) {
      suggestionList.current?.children[suggestionIndex]?.scrollIntoView({ block: "nearest" });
    }
  }, [suggestionsOpen, suggestionIndex]);
  useExitProtection(suppliedController ? "" : saving ? "位置设置正在保存" : state.dirty ? "位置设置有未保存的修改" : "");
  const location = describeSharedLocation(snapshot);
  const disabled = saving || active !== true;
  const saveFeedback = state.saveError?.includes("同名") ? "地区存在同名，请补充省市"
    : state.saveError?.includes("未找到") ? "未找到地区，请补充省市"
    : "保存未确认，请刷新核对";
  const feedback = validation.city ? validation.city
    : state.saveError ? saveFeedback
    : state.readError ? "暂时无法读取地区"
    : saving ? "正在保存…" : state.notice ? "已保存"
    : state.suggestionError || (state.suggesting ? "正在查找地区…" : "");
  const saveManual = () => {
    controller.edit({ mode: "manual" });
    void controller.save();
  };
  return (
    <section className="shared-location" aria-label="地区位置">
      <div className="shared-location__row shared-location__current" aria-live="polite">
        <strong className="shared-location__label">所在地</strong>
        <span className="shared-location__city">{location.available ? snapshot.city : snapshot?.status === "off" ? "未开启" : snapshot?.status === "locating" ? "识别中…" : "暂未识别"}</span>
        {location.available && <span className="shared-location__mode">{snapshot.mode === "manual" ? "手动" : "自动"}</span>}
        <button type="button" disabled={disabled} onClick={() => {
          controller.edit({ mode: "auto" });
          void controller.save();
        }}>自动识别</button>
      </div>
      <form className="shared-location__row" aria-label="手动调整地区" onSubmit={(event) => {
        event.preventDefault();
        if (!composing.current) saveManual();
      }}>
        <label className="shared-location__label" htmlFor={`${id}-city`}>手动调整</label>
        <div className="shared-location__suggest-field">
          <input id={`${id}-city`} name="city" type="text" role="combobox" autoComplete="off" maxLength={60}
            aria-autocomplete="list" aria-haspopup="listbox" aria-controls={suggestionsOpen ? `${id}-suggestions` : undefined}
            aria-expanded={suggestionsOpen} aria-activedescendant={suggestionsOpen && suggestionIndex >= 0 ? `${id}-suggestion-${suggestionIndex}` : undefined}
            aria-busy={Boolean(state.suggesting)} placeholder="输入地区名联想，或直接保存" value={draft.city}
            disabled={disabled} aria-invalid={Boolean(validation.city)}
            onFocus={() => controller.setSuggestionFocus(true)}
            onBlur={() => { composing.current = false; controller.setSuggestionFocus(false); }}
            onCompositionStart={() => { composing.current = true; controller.setComposing(true); }}
            onCompositionEnd={(event) => {
              controller.edit({ mode: "manual", city: event.currentTarget.value, lat: "", lon: "" });
              composing.current = false;
              controller.setComposing(false);
            }}
            onChange={(event) => controller.edit({ mode: "manual", city: event.target.value, lat: "", lon: "" })}
            onKeyDown={(event) => {
              if (composing.current || event.nativeEvent.isComposing || event.keyCode === 229) return;
              if ((event.key === "ArrowDown" || event.key === "ArrowUp") && suggestionsOpen) {
                event.preventDefault();
                controller.moveSuggestion(event.key === "ArrowDown" ? 1 : -1);
              } else if (event.key === "Enter" && suggestionsOpen) {
                event.preventDefault();
                controller.chooseSuggestion(suggestionIndex >= 0 ? suggestionIndex : 0);
              } else if (event.key === "Escape") {
                event.preventDefault();
                controller.clearSuggestions();
              }
            }} />
          {suggestionsOpen && <ul ref={suggestionList} id={`${id}-suggestions`} className="shared-location__suggestions" role="listbox" aria-label="地区候选">
            {suggestions.map((item, index) => <li key={item.id} id={`${id}-suggestion-${index}`}
              className={`shared-location__suggestion${index === suggestionIndex ? " shared-location__suggestion--active" : ""}`}
              role="option" aria-selected={index === suggestionIndex}
              onPointerDown={(event) => event.preventDefault()}
              onMouseDown={(event) => event.preventDefault()}
              onClick={() => controller.chooseSuggestion(index)}>{item.label}</li>)}
          </ul>}
        </div>
        <button className="shared-location__save" type="submit" disabled={disabled || !draft.city.trim()} aria-busy={saving}>保存</button>
      </form>
      {feedback && <div className="shared-location__row shared-location__feedback" role={state.saveError || state.readError || validation.city ? "alert" : "status"}>
        <span>{feedback}</span>
        {(state.saveError || state.readError) && <button type="button" disabled={disabled || state.loading} onClick={() => { void controller.refresh(); }}>刷新</button>}
      </div>}
    </section>
  );
}
