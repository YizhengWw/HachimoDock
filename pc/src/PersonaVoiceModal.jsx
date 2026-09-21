/**
 * [Input] One appearance record (with its normalized `personaVoice`), an optional
 *         `required` flag for the post-creation step, and the persona/voice helpers in
 *         `lib/persona-voice.js`.
 * [Output] The 人设与声音 modal: persona form (name, intro, style, address, language,
 *          forbidden, greeting) with one-tap templates, Doubao Seed TTS 2.0 voice picker
 *          (complete official catalog with language groups/search, or a cloned speaker id,
 *          speed, volume), validation, save via
 *          `savePersonaVoice`, a "restore factory persona" action for the built-in Terrier,
 *          current-key chat instructions, API navigation without discarding drafts,
 *          and a preview button that refreshes after speech configuration changes.
 * [Pos] component node in ref/src, opened from AppearanceGallery cards and creation flows.
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import React, { useCallback, useEffect, useMemo, useState } from "react";
import { AlertCircle, Loader, Save, Volume2, WandSparkles, X } from "lucide-react";
import { invoke } from "@tauri-apps/api/core";
import { savePersonaVoice } from "./lib/appearance-store.js";
import UsageHelp from "./UsageHelp.jsx";
import { API_CONFIGURATION_UPDATED_EVENT } from "./lib/api-configuration.js";
import {
  BUILTIN_TERRIER_PERSONA_VOICE,
  DOUBAO_SPEAKERS,
  PERSONA_LANGUAGE_OPTIONS,
  PERSONA_TEMPLATES,
  VOICE_SPEED_RANGE,
  VOICE_VOLUME_RANGE,
  createDefaultPersonaVoice,
  normalizePersonaVoice,
  previewTextFor,
  speakerOptionsFor,
  validatePersonaVoice,
  voiceSpecFor,
} from "./lib/persona-voice.js";

const PREVIEW_UNAVAILABLE_HINT = "请先在「API 配置 → 语音识别与合成」配置语音服务，再来试听。";

async function loadPreviewAvailability() {
  try {
    const status = await invoke("load_voice_chat_settings");
    return status?.ttsConfigured === true;
  } catch {
    return false;
  }
}

async function playPersonaPreview(form) {
  await invoke("persona_voice_preview", {
    input: { text: previewTextFor(form.persona), voice: voiceSpecFor(form) },
  });
}

function initialFormState(appearance) {
  const normalized = appearance?.personaVoice
    || normalizePersonaVoice(null, {
      name: appearance?.name,
      description: appearance?.description,
      isBuiltin: appearance?.type === "builtin",
    });
  return { persona: { ...normalized.persona }, voice: { ...normalized.voice } };
}

export default function PersonaVoiceModal({ appearance, required = false, onClose, onSaved, onOpenApiSettings, previewAvailable: previewAvailableProp, onPreview = playPersonaPreview }) {
  const isBuiltin = appearance?.type === "builtin";
  const [form, setForm] = useState(() => initialFormState(appearance));
  const [previewReady, setPreviewReady] = useState(Boolean(previewAvailableProp));
  useEffect(() => {
    if (previewAvailableProp !== undefined) {
      setPreviewReady(Boolean(previewAvailableProp));
      return undefined;
    }
    let cancelled = false;
    let sequence = 0;
    const refresh = () => {
      const request = ++sequence;
      loadPreviewAvailability().then((ready) => { if (!cancelled && request === sequence) setPreviewReady(ready); });
    };
    refresh();
    window.addEventListener(API_CONFIGURATION_UPDATED_EVENT, refresh);
    window.addEventListener("focus", refresh);
    return () => { cancelled = true; window.removeEventListener(API_CONFIGURATION_UPDATED_EVENT, refresh); window.removeEventListener("focus", refresh); };
  }, [previewAvailableProp]);
  const previewAvailable = previewReady;
  const [errors, setErrors] = useState({});
  const [saving, setSaving] = useState(false);
  const [saveError, setSaveError] = useState("");
  const [previewing, setPreviewing] = useState(false);

  const [speakerQuery, setSpeakerQuery] = useState("");
  const speakerOptions = useMemo(() => speakerOptionsFor(form.voice.speaker, speakerQuery), [form.voice.speaker, speakerQuery]);
  const speakerGroups = useMemo(() => {
    const groups = new Map();
    for (const speaker of speakerOptions) {
      const language = speaker.language || "其他";
      if (!groups.has(language)) groups.set(language, []);
      groups.get(language).push(speaker);
    }
    return [...groups];
  }, [speakerOptions]);

  const updatePersona = useCallback((key, value) => {
    setForm((current) => ({ ...current, persona: { ...current.persona, [key]: value } }));
    setErrors((current) => (current[key] ? { ...current, [key]: undefined } : current));
  }, []);

  const updateVoice = useCallback((key, value) => {
    setForm((current) => ({ ...current, voice: { ...current.voice, [key]: value } }));
    setErrors((current) => (current.speaker ? { ...current, speaker: undefined } : current));
  }, []);

  const applyTemplate = useCallback((template) => {
    setForm((current) => ({
      ...current,
      persona: {
        ...current.persona,
        style: template.style,
        address: template.address,
        forbidden: template.forbidden,
      },
    }));
  }, []);

  const restoreFactory = useCallback(() => {
    const defaults = isBuiltin
      ? { persona: { ...BUILTIN_TERRIER_PERSONA_VOICE.persona }, voice: { ...BUILTIN_TERRIER_PERSONA_VOICE.voice } }
      : createDefaultPersonaVoice({ name: appearance?.name, description: appearance?.description });
    setForm(defaults);
    setErrors({});
  }, [appearance?.description, appearance?.name, isBuiltin]);

  const handleSave = useCallback(async () => {
    if (saving) return;
    const nextErrors = validatePersonaVoice(form);
    if (Object.keys(nextErrors).length > 0) {
      setErrors(nextErrors);
      return;
    }
    setSaving(true);
    setSaveError("");
    try {
      const saved = await savePersonaVoice(appearance.id, form);
      await onSaved?.(saved);
    } catch (err) {
      console.error(err);
      setSaveError(err?.message || String(err));
    } finally {
      setSaving(false);
    }
  }, [appearance?.id, form, onSaved, saving]);

  const handlePreview = useCallback(async () => {
    if (!previewAvailable || previewing || !onPreview) return;
    setPreviewing(true);
    try {
      await onPreview(form);
    } catch (err) {
      console.error(err);
      setSaveError(err?.message || String(err));
    } finally {
      setPreviewing(false);
    }
  }, [form, onPreview, previewAvailable, previewing]);

  const closeAllowed = !required && !saving;

  return (
    <div className="modal-backdrop" onClick={closeAllowed ? onClose : undefined}>
      <div className="modal-card modal-card--wide persona-voice-modal" onClick={(event) => event.stopPropagation()}>
        <div className="modal-header">
          <div>
            <h3 className="modal-title">人设与声音 · {appearance?.name}</h3>
            <div className="modal-subtitle">
              {required
                ? "先为新形象设置人设与声音，让它在「实时对话」中按你喜欢的方式聊天。"
                : "设置它的名字、性格和声音，让它在「实时对话」中按你喜欢的方式聊天。"}
            </div>
          </div>
          {!required && (
            <button className="icon-btn" type="button" onClick={onClose} disabled={saving} aria-label="关闭">
              <X size={16} />
            </button>
          )}
        </div>
        <div className="modal-body">
          <UsageHelp onOpenApiSettings={onOpenApiSettings} />
          {!previewAvailable && onOpenApiSettings && <button type="button" className="btn-ghost btn-sm" onClick={onOpenApiSettings}>配置语音服务</button>}
          <div className="persona-voice-modal__grid">
            <section className="persona-voice-modal__column">
              <div className="persona-voice-modal__section-head">
                <strong>人设</strong>
                <div className="persona-voice-modal__templates" aria-label="人设模板">
                  {PERSONA_TEMPLATES.map((template) => (
                    <button
                      key={template.id}
                      type="button"
                      className="btn-ghost btn-sm"
                      onClick={() => applyTemplate(template)}
                      disabled={saving}
                    >
                      <WandSparkles size={12} /> {template.label}
                    </button>
                  ))}
                </div>
              </div>
              <label className="ui-field">
                <span className="ui-field__label">名字</span>
                <input
                  className="field-input"
                  value={form.persona.display_name}
                  onChange={(event) => updatePersona("display_name", event.target.value)}
                  placeholder="它怎么称呼自己"
                  disabled={saving}
                />
                {errors.display_name && <span className="ui-field__error">{errors.display_name}</span>}
              </label>
              <label className="ui-field">
                <span className="ui-field__label">一句话自我介绍</span>
                <textarea
                  className="field-input"
                  rows={2}
                  value={form.persona.intro}
                  onChange={(event) => updatePersona("intro", event.target.value)}
                  placeholder="它是谁、住在哪、最喜欢做什么"
                  disabled={saving}
                />
                {errors.intro && <span className="ui-field__error">{errors.intro}</span>}
              </label>
              <label className="ui-field">
                <span className="ui-field__label">性格与说话风格</span>
                <textarea
                  className="field-input"
                  rows={3}
                  value={form.persona.style}
                  onChange={(event) => updatePersona("style", event.target.value)}
                  placeholder="语气、口头禅、被夸/被冷落时的反应"
                  disabled={saving}
                />
                {errors.style && <span className="ui-field__error">{errors.style}</span>}
              </label>
              <div className="persona-voice-modal__row">
                <label className="ui-field">
                  <span className="ui-field__label">怎么称呼用户</span>
                  <input
                    className="field-input"
                    value={form.persona.address}
                    onChange={(event) => updatePersona("address", event.target.value)}
                    placeholder="例如：叫用户「主人」"
                    disabled={saving}
                  />
                </label>
                <label className="ui-field">
                  <span className="ui-field__label">语言</span>
                  <select
                    className="field-input"
                    value={form.persona.language}
                    onChange={(event) => updatePersona("language", event.target.value)}
                    disabled={saving}
                  >
                    {PERSONA_LANGUAGE_OPTIONS.map((option) => (
                      <option key={option.id} value={option.id}>{option.label}</option>
                    ))}
                  </select>
                </label>
              </div>
              <label className="ui-field">
                <span className="ui-field__label">禁止事项</span>
                <input
                  className="field-input"
                  value={form.persona.forbidden}
                  onChange={(event) => updatePersona("forbidden", event.target.value)}
                  placeholder="用分号隔开"
                  disabled={saving}
                />
              </label>
              <label className="ui-field">
                <span className="ui-field__label">开场白</span>
                <input
                  className="field-input"
                  value={form.persona.greeting}
                  onChange={(event) => updatePersona("greeting", event.target.value)}
                  placeholder="进入实时对话时它先说的一句话"
                  disabled={saving}
                />
              </label>
            </section>

            <section className="persona-voice-modal__column">
              <div className="persona-voice-modal__section-head">
                <strong>声音</strong>
                <span className="muted small">豆包 Seed TTS 2.0</span>
              </div>
              <label className="ui-field">
                <span className="ui-field__label">搜索音色 · 共 {DOUBAO_SPEAKERS.length} 种</span>
                <input className="field-input" type="search" value={speakerQuery}
                  onChange={(event) => setSpeakerQuery(event.target.value)}
                  placeholder="搜索名称、语言或音色 ID" disabled={saving} />
              </label>
              <label className="ui-field">
                <span className="ui-field__label">音色</span>
                <select
                  className="field-input"
                  value={form.voice.speaker}
                  onChange={(event) => updateVoice("speaker", event.target.value)}
                  disabled={saving}
                >
                  {speakerGroups.map(([language, speakers]) => (
                    <optgroup key={language} label={language}>
                      {speakers.map((speaker) => <option key={speaker.id} value={speaker.id}>{speaker.label}</option>)}
                    </optgroup>
                  ))}
                </select>
                {speakerQuery.trim() && speakerOptions.every((speaker) => speaker.language === "当前音色") && (
                  <span className="muted small">未找到匹配音色，已保留当前选择。</span>
                )}
                {errors.speaker && <span className="ui-field__error">{errors.speaker}</span>}
              </label>
              <label className="ui-field">
                <span className="ui-field__label">复刻音色 ID（可选，填了优先）</span>
                <input
                  className="field-input"
                  value={form.voice.clone_speaker_id}
                  onChange={(event) => updateVoice("clone_speaker_id", event.target.value)}
                  placeholder="火山控制台声音复刻页申请所得，形如 S_xxxx"
                  disabled={saving}
                />
              </label>
              <div className="persona-voice-modal__row">
                <label className="ui-field">
                  <span className="ui-field__label">语速 <em className="muted">{Number(form.voice.speed).toFixed(2)}</em></span>
                  <input
                    className="persona-voice-modal__range"
                    type="range"
                    min={VOICE_SPEED_RANGE.min}
                    max={VOICE_SPEED_RANGE.max}
                    step={VOICE_SPEED_RANGE.step}
                    value={form.voice.speed}
                    onChange={(event) => updateVoice("speed", Number(event.target.value))}
                    disabled={saving}
                  />
                </label>
                <label className="ui-field">
                  <span className="ui-field__label">音量 <em className="muted">{Number(form.voice.volume).toFixed(2)}</em></span>
                  <input
                    className="persona-voice-modal__range"
                    type="range"
                    min={VOICE_VOLUME_RANGE.min}
                    max={VOICE_VOLUME_RANGE.max}
                    step={VOICE_VOLUME_RANGE.step}
                    value={form.voice.volume}
                    onChange={(event) => updateVoice("volume", Number(event.target.value))}
                    disabled={saving}
                  />
                </label>
              </div>
              <div className="persona-voice-modal__preview">
                <button
                  type="button"
                  className="btn-secondary btn-sm"
                  onClick={handlePreview}
                  disabled={!previewAvailable || previewing || saving}
                  title={previewAvailable ? "用当前人设和音色生成一句问候并播放" : PREVIEW_UNAVAILABLE_HINT}
                >
                  {previewing ? <Loader size={14} className="spin" /> : <Volume2 size={14} />} 试听
                </button>
                <span className="muted small">
                  {previewAvailable ? "点击试听听听效果。外语音色建议使用对应语言的开场白。" : PREVIEW_UNAVAILABLE_HINT}
                </span>
              </div>
            </section>
          </div>

          {saveError && (
            <div className="message-banner message-banner--error">
              <AlertCircle size={14} /> {saveError}
            </div>
          )}
        </div>
        <div className="modal-footer persona-voice-modal__footer">
          <button type="button" className="btn-ghost btn-sm" onClick={restoreFactory} disabled={saving}>
            {isBuiltin ? "恢复出厂人设" : "恢复默认"}
          </button>
          <div className="persona-voice-modal__footer-actions">
            {!required && (
              <button type="button" className="btn-ghost" onClick={onClose} disabled={saving}>
                取消
              </button>
            )}
            <button type="button" className="btn-primary" onClick={handleSave} disabled={saving}>
              {saving ? <Loader size={14} className="spin" /> : <Save size={14} />} {required ? "保存并继续" : "保存"}
            </button>
          </div>
        </div>
      </div>
    </div>
  );
}
