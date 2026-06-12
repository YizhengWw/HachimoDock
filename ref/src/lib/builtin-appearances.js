/**
 * [Input] Static appearance assets bundled under `ref/public`.
 * [Output] Built-in Westie/Terrier clip records, including default state-specific WAV cues, that can be prepended to app-local records.
 * [Pos] lib node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

export const BUILTIN_TERRIER_APPEARANCE_ID = "builtin-terrier";

const TERRIER_FAMILIES = [
  "welcome",
  "idle.playing",
  "idle.wandering",
  "working.thinking",
  "working.typing",
  "working.browsing",
  "waiting_user",
  "done",
  "error",
  "touch.lick",
  "touch.what",
  "idle.begging",
  "idle.daydreaming",
  "idle.eating",
  "idle.reading",
  "idle.traveling",
];

export const DEFAULT_AUDIO_CUE_BY_FAMILY = Object.freeze({
  done: "done",
  error: "error",
  waiting_user: "waiting_user",
});

function terrierVideoSrc(family) {
  return `/terrier-clips/${family}.mp4`;
}

function terrierAudioCueSrc(cue) {
  return `/terrier-clips/${cue}.wav`;
}

export function defaultAudioCueSrcForFamily(family) {
  const cue = DEFAULT_AUDIO_CUE_BY_FAMILY[family];
  return cue ? terrierAudioCueSrc(cue) : "";
}

export function createBuiltinTerrierAppearance() {
  return {
    schema_version: 1,
    id: BUILTIN_TERRIER_APPEARANCE_ID,
    type: "builtin",
    name: "西高地小狗",
    description: "默认内置形象，用于展示经典桌宠动作素材。",
    provider: "builtin",
    model: "terrier-clips",
    base_url: "",
    thinking_model: "",
    persona: { source: "terrier-clips" },
    source_image: "",
    source_image_src: "/terrier-clips/thumbs/welcome.jpg",
    source_mime: "image/jpeg",
    families: TERRIER_FAMILIES.map((family) => {
      const audioSrc = defaultAudioCueSrcForFamily(family);
      const audioCue = audioSrc
        ? {
            audioPath: audioSrc,
            audioSrc,
            audioDefault: true,
          }
        : {};
      return {
        family,
        ok: true,
        prompt: "builtin terrier clip",
        videoPath: "",
        videoSrc: terrierVideoSrc(family),
        taskId: `builtin-terrier-${family}`,
        videoUrl: terrierVideoSrc(family),
        ...audioCue,
      };
    }),
    created_at: "2026-05-08T11:08:26.256Z",
    absolute_dir: "builtin://terrier-clips",
  };
}

export function listBuiltinAppearances() {
  return [createBuiltinTerrierAppearance()];
}

export function mergeBuiltinAppearances(records = []) {
  const builtin = listBuiltinAppearances();
  const builtinIds = new Set(builtin.map((record) => record.id));
  return [
    ...builtin,
    ...(records || []).filter((record) => !builtinIds.has(record?.id)),
  ];
}
