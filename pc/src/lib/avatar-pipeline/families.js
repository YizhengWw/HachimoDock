/**
 * [Input] family definitions for the avatar generation pipeline.
 * [Output] FAMILIES array for all known/imported states plus CUSTOM_GENERATION_FAMILIES for user-created avatar generation.
 * [Pos] lib node in pc/src/lib/avatar-pipeline
 * [Sync] If this file changes, update this header.
 */

/**
 * @typedef {{
 *   family: string,
 *   label: string,
 *   playback: 'one_shot_entry' | 'loop_state',
 *   motion_brief: string,
 *   spatial_path: string,
 *   pose_signature: string,
 *   prop_policy: string,
 *   avoid_motion: string,
 *   variation_hint: string
 * }} FamilyDef
 */

/** @type {FamilyDef[]} */
const FAMILY_DEFINITIONS = [
  {
    family: "welcome",
    label: "warm greeting",
    playback: "one_shot_entry",
    motion_brief:
      "A one-shot greeting: begin in a state-appropriate pose, do a small welcoming step/peek/slide or paw lift near the center, then settle naturally into a relaxed state-specific pose.",
    spatial_path:
      "Start at the uploaded center pose, make a small left/right or forward greeting motion inside the frame, then settle back to the same center pose.",
    pose_signature:
      "Visible greeting-step-peek-slide-settle silhouette: one paw or head leads a small welcome beat, then the full body settles naturally into a relaxed greeting pose.",
    prop_policy:
      "Required small marker prop: one tiny flower, sparkle sign, or welcome mark must appear near the entrance path; keep it secondary and do not cover the face.",
    avoid_motion:
      "Do not make welcome an off-screen entrance, a seated stationary idle, a tiny blink, or the same centered pose for the whole clip.",
    variation_hint: "Vary the greeting direction: small left step, right step, forward peek, or quick paw-lift, but always finish naturally without a shared pose reset.",
  },
  {
    family: "idle.playing",
    label: "playful idle",
    playback: "loop_state",
    motion_brief:
      "A compact playful loop: begin in a state-appropriate pose, dash after a tiny play target across the screen, bat once with a paw, then settle naturally into a relaxed state-specific pose.",
    spatial_path:
      "Clear left-to-right or screen-left to screen-right dash across the frame, with a visible paw reach near the far side and quick recoil back to the original pose; do not keep the pet centered for the whole clip.",
    pose_signature:
      "Play silhouette: raised front paw, lowered shoulders, alert ears, curved body line, and a quick spring-back.",
    prop_policy:
      "Required small marker prop: one tiny butterfly, toy ball, or ribbon must cross from screen-left to screen-right as the target of the paw bat.",
    avoid_motion:
      "Do not reuse welcome entry, wandering side-walk, or the same sitting pose with only blinking.",
    variation_hint: "Vary the target prop or paw side; the pet should visibly travel toward one side before returning.",
  },
  {
    family: "idle.wandering",
    label: "wandering idle",
    playback: "loop_state",
    motion_brief:
      "A slow wandering loop: begin in a state-appropriate pose, follow a tiny environmental marker across the screen with one or two exploratory side steps, then settle into a natural comfortable pose.",
    spatial_path:
      "Visible side travel from screen-left toward screen-right or from screen-right toward screen-left, using a shallow wandering arc inside the frame; the pet may briefly occupy a side position instead of staying centered.",
    pose_signature:
      "Wander silhouette: head turns first, body follows with a small side step, tail or hips lag behind, then everything recenters.",
    prop_policy:
      "Required small marker prop: one tiny bird silhouette, small flower, or drifting leaf must guide the side-to-side wander; keep it background-level and secondary.",
    avoid_motion:
      "Do not reuse welcome entry, playing pounce, or the same sitting pose with only blinking.",
    variation_hint: "Vary whether the wander goes left-to-right or right-to-left, but keep it slow and loop-safe.",
  },
  {
    family: "working",
    label: "working",
    playback: "loop_state",
    motion_brief:
      "A focused in-progress work loop: compact desk-work motion such as tapping a tiny keyboard, nudging a small mouse, flipping one page of a small open book, or visible typing, then finish naturally.",
    spatial_path:
      "No travel; keep the body planted with a clear centered work beat and a loop-safe return.",
    pose_signature:
      "Working silhouette: intent gaze, slight forward focus, one readable keyboard, mouse, page-flip, or typing action, and a rhythmic settle into a natural focused pose.",
    prop_policy:
      "Required small marker prop: one tiny keyboard, small mouse, or small open book/page near the working action; no readable UI text, no abstract progress/status effects, no cursor graphics.",
    avoid_motion:
      "Do not make it idle.default breathing only, wandering steps, done celebration, waiting_user beckoning, or error recoil.",
    variation_hint:
      "Choose one readable work style that fits the subject; custom generation only needs this single working clip.",
  },
  {
    family: "waiting_user",
    label: "waiting for user",
    playback: "loop_state",
    motion_brief:
      "A clear large waiting-for-user cue: begin in a state-appropriate pose, lean forward, raise a subject-appropriate hand, arm, paw, wing, or whole-body gesture toward a tiny option cue, do one obvious beckon or attentive side look, then settle naturally into a relaxed state-specific pose.",
    spatial_path:
      "Visible forward lean plus side-to-side attention shift inside the frame; the subject may travel about 15-30% toward screen-left, screen-right, or toward the viewer in the middle, then settle naturally.",
    pose_signature:
      "Waiting silhouette: subject-appropriate raised arm/hand, lifted paw, wing cue, torso lean, head-and-body beckon, or whole-body bounce that reads as asking the user to choose.",
    prop_policy:
      "Required small marker prop: one tiny question mark or two miniature option cards represented by dots or simple shapes; no readable text, no speech bubble.",
    avoid_motion:
      "Do not reduce it to a slow blink, tiny head tilt, still paws, error recoil, active typing, or playful batting.",
    variation_hint:
      "Adapt the gesture to the uploaded subject: a human uses hand/arm/shoulder movement, a cat or dog uses paw/ear/tail/body lean, a bird uses wing/head movement, and a toy/object uses tilt, bounce, or part movement; keep the option cue tiny and unreadable.",
  },
  {
    family: "done",
    label: "task done",
    playback: "loop_state",
    motion_brief:
      "A short completion reward: proud expression, tiny celebratory lift, ring a tiny finish bell or tap a finish marker once, and gracefully finish naturally.",
    spatial_path:
      "Small upward lift or proud chest rise in place, then a clean settle back to center.",
    pose_signature:
      "Done silhouette: proud raised chest, one small paw lift or nod, bright expression, then calm reset.",
    prop_policy:
      "Required small marker prop: one tiny finish bell, check sparkle, or finish flag icon must pop briefly; avoid readable badges or text.",
    avoid_motion:
      "Do not reuse welcome entry, typing taps, or error recoil.",
    variation_hint: "Keep celebration small; the bell or finish marker is the main semantic cue, with no confetti, text, or large symbols.",
  },
  {
    family: "error",
    label: "soft error",
    playback: "loop_state",
    motion_brief:
      "A cute soft-error cycle: small startled recoil, confused look, apologetic recovery, and finish naturally.",
    spatial_path:
      "Tiny backward recoil and recovery in place, with no side travel.",
    pose_signature:
      "Error silhouette: quick flinch, ears or head dip, confused glance, then apologetic reset.",
    prop_policy:
      "Required small marker prop: one tiny question mark, wobble mark, or soft warning sparkle; no harsh warning sign or text.",
    avoid_motion:
      "Do not make it a playful pounce, done celebration, or calm waiting blink.",
    variation_hint: "No warning text or harsh icon; emotion should be readable but gentle.",
  },
  {
    family: "touch.lick",
    label: "affectionate screen touch close-up",
    playback: "loop_state",
    motion_brief:
      "A touch-screen affectionate response: after the user taps, the pet suddenly leans close so the face grows large in the foreground, softly presses nose or cheek against the screen glass, then quietly returns to the pose.",
    spatial_path:
      "Fast forward approach toward the viewer, face close to the front glass for a beat, then backward settle naturally.",
    pose_signature:
      "Close-up touch silhouette: enlarged face near the glass, soft eyes, nose or cheek leading the contact, body still readable behind the foreground head.",
    prop_policy:
      "Required small marker prop: one tiny tap ripple, screen smudge, glass shine, or fingerprint sparkle at the contact point; no heart overlay.",
    avoid_motion:
      "Do not interpret the legacy family name literally; keep it as affectionate screen-contact feedback, not side touch, confused touch, or a full-body jump.",
    variation_hint:
      "Vary the contact as nose press, cheek nudge, or paw-against-glass while keeping the face close-up friendly and undistorted.",
  },
  {
    family: "touch.what",
    label: "curious screen touch close-up",
    playback: "loop_state",
    motion_brief:
      "A curious touch-screen response: after the user taps, the pet pops its face close to the foreground as if inspecting the screen, quietly bumps or sniffs the glass, then returns to the same pose.",
    spatial_path:
      "Quick forward pop toward the viewer, face briefly enlarged near the screen glass, then retreat to the original centered pose.",
    pose_signature:
      "Curious close-up silhouette: enlarged face near the glass, slight head tilt, nose or paw near the touched spot, attentive eyes, reset.",
    prop_policy:
      "Required small marker prop: one tiny tap ripple, screen smudge, glass shine, or fingerprint sparkle near the touched spot.",
    avoid_motion:
      "Do not interpret the legacy family name literally; keep it as curious screen-contact feedback, not symbolic confusion, readable text, error recoil, or waiting stillness.",
    variation_hint:
      "Make it a silent screen-inspection touch response with a closer face and a slightly puzzled expression.",
  },
  // --- Families below are primarily produced by the codex-pet importer
  // (M13.1); the self-research pipeline will also render them once prompts
  // are authored. Kept as additive entries so existing records stay valid.
  {
    family: "idle.default",
    label: "default idle",
    playback: "loop_state",
    motion_brief:
      "A calm default idle cycle with subtle breathing or blink, no directional travel, and settle into a natural comfortable pose.",
    spatial_path:
      "No travel; only breathing, tiny head buoyancy, and blink timing.",
    pose_signature:
      "Default idle silhouette: nearly unchanged body outline with soft breathing and blink.",
    prop_policy:
      "Required small marker prop: one tiny breathing sparkle or soft idle dot near the body; keep this as the clean neutral baseline.",
    avoid_motion:
      "Do not add entrance, pounce, wandering, or task-specific gestures.",
    variation_hint: "Keep motion minimal and loop-safe.",
  },
  {
    family: "idle.jumping",
    label: "jumping idle",
    playback: "loop_state",
    motion_brief:
      "A short anticipation-and-hop idle: small crouch, lift, peak, descent, settle naturally after landing.",
    spatial_path:
      "Vertical crouch-hop-settle path, centered and low enough to stay in frame.",
    pose_signature:
      "Jump silhouette: compressed crouch, lifted body, extended paws, soft landing.",
    prop_policy:
      "Required small marker prop: one tiny dust puff or star sparkle at landing; no extra character.",
    avoid_motion:
      "Do not turn it into side wandering, welcome entrance, or typing.",
    variation_hint: "Hop height stays small; avoid leaving frame.",
  },
  {
    family: "touch.right",
    label: "touch reaction right",
    playback: "loop_state",
    motion_brief:
      "A directional touch reaction toward the right: small lean or step right, acknowledge the touch, return to pose.",
    spatial_path:
      "Clear rightward lean or half-step, then return to center.",
    pose_signature:
      "Right-touch silhouette: body bends toward screen-right, right-side paw or ear reacts first.",
    prop_policy:
      "Required small marker prop: one tiny tap ripple on the right side only.",
    avoid_motion:
      "Do not mirror left, move forward for lick, or use centered idle-only motion.",
    variation_hint: "Direction should read clearly without large displacement.",
  },
  {
    family: "touch.left",
    label: "touch reaction left",
    playback: "loop_state",
    motion_brief:
      "Mirror of touch.right: directional touch reaction toward the left, acknowledge, return.",
    spatial_path:
      "Clear leftward lean or half-step, then return to center.",
    pose_signature:
      "Left-touch silhouette: body bends toward screen-left, left-side paw or ear reacts first.",
    prop_policy:
      "Required small marker prop: one tiny tap ripple on the left side only.",
    avoid_motion:
      "Do not mirror right accidentally, move forward for lick, or use centered idle-only motion.",
    variation_hint: "Do not reuse right-facing frames; redraw mirrored.",
  },
];

const CUSTOM_GENERATION_EXCLUDED_FAMILY_IDS = new Set(["touch.right", "touch.left"]);
export const FAMILIES = FAMILY_DEFINITIONS.map(item => ({ ...item,
  prop_policy: item.family === "idle.default" ? "No props, sparkles, dots or decorations. Only natural breathing and blinking."
    : item.prop_policy.replace(/Required/g, "Optional").replace(/must/g, "may"),
}));

const CLEAN_MOTIONS = {
  welcome:'A small greeting lean and subject-appropriate limb or head wave, then settle naturally.',
  'idle.playing':'Playfully crouch, make a short side hop, then settle naturally; keep any original character-associated accessory, never introduce an object.',
  'idle.wandering':'Take two slow exploratory side steps and look around, then settle naturally.',
  working:'Focus the gaze, lean attentively and make two small rhythmic head or own-limb movements, then settle naturally.',
  waiting_user:'Look toward the viewer, tilt the head and beckon using the subject’s own anatomy, then settle naturally.',
  done:'Raise the head, make one cheerful body bounce and settle back.',
  error:'Pause, make a small surprised recoil and concerned head tilt, then settle naturally.',
  'touch.lick':'Lean toward the viewer with a friendly facial expression and gently retreat; stay entirely in frame.',
  'touch.what':'Make a curious forward lean and head tilt, then settle naturally; stay entirely in frame.',
  'idle.default':'Subtle natural breathing and blinking only, with the complete body staying still.',
  'idle.jumping':'One low crouch-hop-soft-landing cycle, entire body in frame, then settle naturally.',
};
export const CUSTOM_GENERATION_FAMILIES = FAMILIES.filter(
  (item) => !CUSTOM_GENERATION_EXCLUDED_FAMILY_IDS.has(item.family),
).map(item=>({...item,preserve_reference_accessories:true,motion_brief:CLEAN_MOTIONS[item.family],spatial_path:'Keep the complete subject within the frame with safe margins; use state-specific starting and ending poses without a shared pose reset.',
  pose_signature:CLEAN_MOTIONS[item.family],prop_policy:'Preserve original character-associated accessories already held or worn in the reference, such as a tennis racket, musical instrument, glasses or hat. No newly invented props, hands from other people, unrelated furniture, symbols, effects or decorations. Preserve the subject\'s own hands, arms and legs. Keep the complete subject and retained accessories within frame on pure black.',
  variation_hint:'Vary only the subject’s own pose, gaze and rhythm. Never add objects.'}));
