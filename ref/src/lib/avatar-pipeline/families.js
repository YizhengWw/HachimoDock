/**
 * [Input] family definitions for the avatar generation pipeline.
 * [Output] FAMILIES array with distinct first/last-frame motion grammar, subject-adaptive action cues, and marker props consumed by the prompt builder.
 * [Pos] lib node in ref/src/lib/avatar-pipeline
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
export const FAMILIES = [
  {
    family: "welcome",
    label: "warm greeting",
    playback: "one_shot_entry",
    motion_brief:
      "A one-shot greeting: start from the uploaded pose, do a small welcoming step/peek/slide or paw lift near the center, then return to the exact uploaded pose.",
    spatial_path:
      "Start at the uploaded center pose, make a small left/right or forward greeting motion inside the frame, then settle back to the same center pose.",
    pose_signature:
      "Visible greeting-step-peek-slide-settle silhouette: one paw or head leads a small welcome beat, then the full body returns to the reference pose.",
    prop_policy:
      "Required small marker prop: one tiny flower, sparkle sign, or welcome mark must appear near the entrance path; keep it secondary and do not cover the face.",
    avoid_motion:
      "Do not make welcome an off-screen entrance, a seated stationary idle, a tiny blink, or the same centered pose for the whole clip.",
    variation_hint: "Vary the greeting direction: small left step, right step, forward peek, or quick paw-lift, but always return to the reference pose.",
  },
  {
    family: "idle.playing",
    label: "playful idle",
    playback: "loop_state",
    motion_brief:
      "A compact playful loop: start from the uploaded pose, dash after a tiny play target across the screen, bat once with a paw, then return to the exact uploaded pose.",
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
      "A slow wandering loop: start from the uploaded pose, follow a tiny environmental marker across the screen with one or two exploratory side steps, then return to the exact pose.",
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
      "A focused in-progress work loop: compact desk-work motion such as tapping a tiny keyboard, nudging a small mouse, flipping one page of a small open book, or visible typing, then return to the same pose.",
    spatial_path:
      "No travel; keep the body planted with a clear centered work beat and a loop-safe return.",
    pose_signature:
      "Working silhouette: intent gaze, slight forward focus, one readable keyboard, mouse, page-flip, or typing action, and a rhythmic reset back to the uploaded pose.",
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
      "A clear large waiting-for-user cue: start from the uploaded pose, lean forward, raise a subject-appropriate hand, arm, paw, wing, or whole-body gesture toward a tiny option cue, do one obvious beckon or attentive side look, then return to the exact uploaded pose.",
    spatial_path:
      "Visible forward lean plus side-to-side attention shift inside the frame; the subject may travel about 15-30% toward screen-left, screen-right, or toward the viewer in the middle, then return to the original pose.",
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
      "A short completion reward: proud expression, tiny celebratory lift, ring a tiny finish bell or tap a finish marker once, and gracefully return to the same pose.",
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
      "A cute soft-error cycle: small startled recoil, confused look, apologetic recovery, and return to the same pose.",
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
      "Fast forward approach toward the viewer, face close to the front glass for a beat, then backward return to the original pose.",
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
      "A calm default idle cycle with subtle breathing or blink, no directional travel, and return to the exact pose.",
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
      "A short anticipation-and-hop idle: small crouch, lift, peak, descent, settle back to the exact pose.",
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
