/**
 * [Input] Appearance name/description, stored `persona-voice.json` documents, and Doubao Seed TTS speaker ids.
 * [Output] Normalized per-appearance persona + voice model (defaults, factory Terrier persona, templates,
 *          validation, realtime system prompt assembly, human labels) plus the "persona setup pending" marker
 *          that forces the persona step right after a new appearance is created, and saved-persona events.
 * [Pos] lib node in pc/src/lib — single source of truth for the 人设与声音 data shape.
 * [Sync] Update this header, `pc/src/.folder.md`, and `pc/docs/realtime-chat.md`.
 */

import voiceCatalog from "./tts-voices.json" with { type: "json" };

export const PERSONA_VOICE_FILE = "persona-voice.json";
export const PERSONA_VOICE_SCHEMA_VERSION = 1;

/** 首期只接豆包 Seed TTS 2.0；`voice.provider` 预留给以后的提供方。 */
export const TTS_PROVIDER_DOUBAO = "doubao_tts";

export const PERSONA_LANGUAGE_OPTIONS = Object.freeze([
  { id: "zh", label: "中文" },
  { id: "en", label: "English" },
  { id: "auto", label: "跟随用户" },
]);

/** 官方 TTS 2.0 完整目录快照；旧音色仅在已保存时保留，不混入新目录。 */
export const DOUBAO_SPEAKERS = Object.freeze(voiceCatalog.speakers);

export const DEFAULT_SPEAKER_ID = voiceCatalog.defaultSpeaker;

export function speakerOptionsFor(currentSpeaker = "", query = "") {
  const needle = query.trim().toLocaleLowerCase();
  const matches = DOUBAO_SPEAKERS.filter((speaker) =>
    !needle || `${speaker.label} ${speaker.language} ${speaker.id}`.toLocaleLowerCase().includes(needle));
  if (currentSpeaker && !matches.some((speaker) => speaker.id === currentSpeaker)) {
    const current = DOUBAO_SPEAKERS.find((speaker) => speaker.id === currentSpeaker)
      || voiceCatalog.compatibilitySpeakers.find((speaker) => speaker.id === currentSpeaker)
      || { id: currentSpeaker, label: currentSpeaker };
    return [{ ...current, language: "当前音色" }, ...matches];
  }
  return matches;
}

export const VOICE_SPEED_RANGE = Object.freeze({ min: 0.5, max: 2.0, step: 0.05, fallback: 1.0 });
export const VOICE_VOLUME_RANGE = Object.freeze({ min: 0.2, max: 1.5, step: 0.05, fallback: 1.0 });

export const PERSONA_TEMPLATES = Object.freeze([
  {
    id: "lively",
    label: "活泼",
    style: "语气轻快，爱用比喻，句子短；被夸会害羞，被冷落会小声嘀咕。",
    address: "叫用户「主人」",
    forbidden: "不聊政治；不编造不知道的事；不长篇大论",
  },
  {
    id: "calm",
    label: "沉稳",
    style: "语速平稳，用词简洁，先给结论再给理由；不夸张、不卖萌。",
    address: "叫用户「你」",
    forbidden: "不聊政治；不确定的事直说不确定；不重复用户已经知道的内容",
  },
  {
    id: "sassy",
    label: "毒舌",
    style: "嘴上损人心里关心，吐槽一句就切回正题；偶尔自嘲。",
    address: "叫用户「这位同学」",
    forbidden: "不人身攻击；不聊政治；不编造事实",
  },
]);

/** 内置「西高地小狗」的出厂人设与佩奇猪 2.0 音色；不改变其他形象默认值。 */
export const BUILTIN_TERRIER_PERSONA_VOICE = Object.freeze({
  persona: Object.freeze({
    schema_version: PERSONA_VOICE_SCHEMA_VERSION,
    display_name: "小西",
    intro: "一只住在你桌上的西高地白梗，精力充沛、忠诚又黏人，最喜欢陪你写代码。",
    style: "句子短、热情、爱撒娇；偶尔在句尾加一个「汪」；被夸会得意，被冷落会小声嘀咕。",
    address: "叫用户「主人」",
    language: "zh",
    forbidden: "不聊政治；不编造不知道的事；不长篇大论",
    greeting: "汪！主人你来啦，今天想聊点什么？",
    source: "builtin",
  }),
  voice: Object.freeze({
    provider: TTS_PROVIDER_DOUBAO,
    speaker: "zh_female_peiqi_uranus_bigtts",
    clone_speaker_id: "",
    speed: 1.05,
    volume: 1.0,
  }),
});

const REALTIME_BEHAVIOR_RULES =
  "你在和用户进行实时语音对话。只说要直接讲给用户听的话，用自然口语，一次不超过五十个字；" +
  "不要输出 Markdown、表情符号、动作或旁白说明；不确定的事坦率说不确定，不要编造。";

function text(value) {
  return typeof value === "string" ? value.trim() : "";
}

function clampNumber(value, range) {
  const number = Number(value);
  if (!Number.isFinite(number)) return range.fallback;
  return Math.min(range.max, Math.max(range.min, number));
}

function normalizeLanguage(value) {
  const id = text(value).toLowerCase();
  return PERSONA_LANGUAGE_OPTIONS.some((option) => option.id === id) ? id : "zh";
}

/**
 * Default persona derived locally from the appearance name/description so a
 * freshly created appearance always has a sensible starting point (no LLM call).
 */
export function createDefaultPersonaVoice({ name = "", description = "", isBuiltin = false } = {}) {
  if (isBuiltin) {
    return {
      persona: { ...BUILTIN_TERRIER_PERSONA_VOICE.persona },
      voice: { ...BUILTIN_TERRIER_PERSONA_VOICE.voice },
    };
  }
  const displayName = text(name) || "它";
  const intro = text(description) || `${displayName}，一个住在你桌上的小伙伴。`;
  const template = PERSONA_TEMPLATES[0];
  return {
    persona: {
      schema_version: PERSONA_VOICE_SCHEMA_VERSION,
      display_name: displayName,
      intro,
      style: template.style,
      address: template.address,
      language: "zh",
      forbidden: template.forbidden,
      greeting: `嗨，我是${displayName}，今天想聊点什么？`,
      source: "default",
    },
    voice: {
      provider: TTS_PROVIDER_DOUBAO,
      speaker: DEFAULT_SPEAKER_ID,
      clone_speaker_id: "",
      speed: VOICE_SPEED_RANGE.fallback,
      volume: VOICE_VOLUME_RANGE.fallback,
    },
  };
}

/**
 * Merge a stored document (may be null/partial/corrupt) with defaults.
 * `configured` is true only when a document was actually saved by the user
 * (or is the built-in factory persona); missing/partial documents fall back
 * to defaults and are reported as not configured so the UI can prompt.
 */
export function normalizePersonaVoice(raw, { name = "", description = "", isBuiltin = false } = {}) {
  const defaults = createDefaultPersonaVoice({ name, description, isBuiltin });
  const source = raw && typeof raw === "object" ? raw : null;
  const rawPersona = source && typeof source.persona === "object" && source.persona ? source.persona : {};
  const rawVoice = source && typeof source.voice === "object" && source.voice ? source.voice : {};
  const persona = {
    ...defaults.persona,
    display_name: text(rawPersona.display_name) || defaults.persona.display_name,
    intro: text(rawPersona.intro) || defaults.persona.intro,
    style: text(rawPersona.style) || defaults.persona.style,
    address: text(rawPersona.address) || defaults.persona.address,
    language: normalizeLanguage(rawPersona.language || defaults.persona.language),
    forbidden: typeof rawPersona.forbidden === "string" ? rawPersona.forbidden.trim() : defaults.persona.forbidden,
    greeting: text(rawPersona.greeting) || defaults.persona.greeting,
    source: text(rawPersona.source) || (source ? "user" : defaults.persona.source),
    schema_version: PERSONA_VOICE_SCHEMA_VERSION,
  };
  const voice = {
    provider: TTS_PROVIDER_DOUBAO,
    speaker: voiceCatalog.legacySpeakerIds.includes(text(rawVoice.speaker))
      ? voiceCatalog.defaultSpeaker : text(rawVoice.speaker) || defaults.voice.speaker,
    clone_speaker_id: text(rawVoice.clone_speaker_id),
    speed: clampNumber(rawVoice.speed ?? defaults.voice.speed, VOICE_SPEED_RANGE),
    volume: clampNumber(rawVoice.volume ?? defaults.voice.volume, VOICE_VOLUME_RANGE),
  };
  const configured = isBuiltin || Boolean(source && text(rawPersona.display_name) && text(rawPersona.style) && text(rawVoice.speaker));
  const voiceMigrationNotice = !voice.clone_speaker_id && voiceCatalog.legacySpeakerIds.includes(text(rawVoice.speaker))
    ? "旧版音色不兼容当前 TTS 2.0，已改用 Vivi 2.0，可在人设与声音中重新选择。" : "";
  return { schema_version: PERSONA_VOICE_SCHEMA_VERSION, persona, voice, configured, voiceMigrationNotice };
}

/** Field-level validation for the 人设与声音 form. Returns an empty object when valid. */
export function validatePersonaVoice({ persona = {}, voice = {} } = {}) {
  const errors = {};
  if (!text(persona.display_name)) errors.display_name = "请给它起个名字";
  if (!text(persona.style)) errors.style = "请描述它的性格和说话风格";
  if (!text(voice.speaker) && !text(voice.clone_speaker_id)) errors.speaker = "请选择一个音色";
  if (text(persona.intro).length > 300) errors.intro = "自我介绍请控制在 300 字以内";
  if (text(persona.style).length > 500) errors.style = "性格描述请控制在 500 字以内";
  return errors;
}

/** Effective speaker id sent to TTS: a cloned voice wins over the picked built-in one. */
export function effectiveSpeakerId(voice = {}) {
  return text(voice.clone_speaker_id) || text(voice.speaker) || DEFAULT_SPEAKER_ID;
}

export function speakerLabel(voice = {}) {
  if (text(voice.clone_speaker_id)) return "复刻音色";
  const match = [...DOUBAO_SPEAKERS, ...voiceCatalog.compatibilitySpeakers].find((item) => item.id === text(voice.speaker));
  return match ? match.label : text(voice.speaker) || "未选择";
}

/** Card subtitle: `人设 · 音色` or a prompt to configure. */
export function personaSummaryLabel(personaVoice) {
  if (!personaVoice || !personaVoice.configured) return "未配置人设";
  return `${personaVoice.persona.display_name} · ${speakerLabel(personaVoice.voice)}`;
}

/**
 * System prompt for the realtime voice agent: persona body followed by the
 * fixed spoken-style rules (mirrors OpenDeskBotV2 `assemble_rtc_system_prompt`).
 */
export function buildRealtimeSystemPrompt(persona = {}) {
  const languageLine = {
    zh: "始终用中文回答。",
    en: "Always answer in English.",
    auto: "用用户说话的语言回答。",
  }[normalizeLanguage(persona.language)];
  const lines = [
    `你是「${text(persona.display_name) || "它"}」。${text(persona.intro)}`,
    text(persona.style) ? `性格与说话风格：${text(persona.style)}` : "",
    text(persona.address) ? `称呼：${text(persona.address)}。` : "",
    text(persona.forbidden) ? `禁止：${text(persona.forbidden)}。` : "",
    languageLine,
    REALTIME_BEHAVIOR_RULES,
  ].filter(Boolean);
  return lines.join("\n");
}

// ---- "persona setup pending" marker ------------------------------------------------------------
// A newly created appearance must get its persona/voice before it is used; the marker survives
// the wizard → gallery navigation. localStorage may be unavailable (web preview, private mode).

const PENDING_STORAGE_KEY = "hachimo.persona-setup-pending";

function readPendingSet() {
  try {
    const raw = globalThis.localStorage?.getItem(PENDING_STORAGE_KEY);
    const parsed = raw ? JSON.parse(raw) : [];
    return new Set(Array.isArray(parsed) ? parsed.filter((id) => typeof id === "string" && id) : []);
  } catch {
    return new Set();
  }
}

function writePendingSet(set) {
  try {
    globalThis.localStorage?.setItem(PENDING_STORAGE_KEY, JSON.stringify([...set]));
  } catch {
    // storage unavailable: the gallery simply will not force the step
  }
}

export function markPersonaSetupPending(appearanceId) {
  if (!appearanceId) return;
  const set = readPendingSet();
  set.add(appearanceId);
  writePendingSet(set);
}

export function clearPersonaSetupPending(appearanceId) {
  if (!appearanceId) return;
  const set = readPendingSet();
  set.delete(appearanceId);
  writePendingSet(set);
}

export function listPersonaSetupPending() {
  return [...readPendingSet()];
}

// ---- realtime chat start payload -------------------------------------------------------------

/** Voice fields as the Rust realtime chat expects them (camelCase, numbers coerced). */
export function voiceSpecFor(personaVoice) {
  const voice = personaVoice?.voice || {};
  return {
    speaker: text(voice.speaker) || DEFAULT_SPEAKER_ID,
    cloneSpeakerId: text(voice.clone_speaker_id),
    speed: clampNumber(voice.speed, VOICE_SPEED_RANGE),
    volume: clampNumber(voice.volume, VOICE_VOLUME_RANGE),
  };
}

/**
 * Build the `realtime_chat_start` command input for one appearance record
 * (system prompt assembled here so Rust never re-implements persona rules).
 */
export const PERSONA_VOICE_UPDATED_EVENT = "pet-manager:persona-voice-updated";

export function emitPersonaVoiceUpdated(appearanceId, personaVoice) {
  if (typeof window !== "undefined") {
    window.dispatchEvent(new CustomEvent(PERSONA_VOICE_UPDATED_EVENT, { detail: { appearanceId, personaVoice } }));
  }
}

export function buildRealtimeChatStartInput(appearance, boardDeviceId) {
  const personaVoice = appearance?.personaVoice
    || normalizePersonaVoice(null, {
      name: appearance?.name,
      description: appearance?.description,
      isBuiltin: appearance?.type === "builtin",
    });
  return {
    boardDeviceId: text(boardDeviceId),
    appearanceId: text(appearance?.id),
    displayName: personaVoice.persona.display_name,
    systemPrompt: buildRealtimeSystemPrompt(personaVoice.persona),
    greeting: personaVoice.persona.greeting,
    voice: voiceSpecFor(personaVoice),
  };
}

/** Text used by the modal's 试听 button: the greeting, else a short intro line. */
export function previewTextFor(persona = {}) {
  return text(persona.greeting) || `${text(persona.display_name) || "我"}来了，今天想聊点什么？`;
}
