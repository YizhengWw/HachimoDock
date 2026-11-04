# Bounded local selectors and form editing

The separately gated mixed text extension is described in [app-form-v2.md](app-form-v2.md).
It does not change the v1 contract below or imply hardware acceptance.

This is an opt-in `widgetAppForm=p4-app-form-v1` extension, not a keyboard,
dynamic list, arbitrary phone UI, host form, or general script interpreter.
It works without touch through the existing buttons → action route. Device,
desktop preview, Python model, and Rust installer share the contract vectors
in `contracts/app-form-v1.json`.
The PC/JavaScript tool preview now runs the existing device's
`pet_p4_app_form.h` state machine through the product-owned `form-v1-core.wasm`,
not a second JS editing algorithm. Its 64 KiB memory is fixed, with zero imports
and no growth; one instance is reused per current declaration and every call
reloads the complete integer model. Non-form components do not instantiate it.
The authored labels and decimal values are rendered using the native-selected
index/value. Schema validation remains separate, and this interpreter is not a
package-supplied module or a service grant. Existing saves, limits and physical
action bindings are unchanged. Python's contract model and installer validation
still use shared vectors; this is not a claim that all framework paths have
already converged on a single C implementation.

## Profile and fixed budgets

Use runtime `p4-bounded-runtime-v4`, surface `p4-app-surface-v1`, graphics
`p4-app-graphics-v1`, bindings `p4-variable-bindings-v1`, and input `p4-input-v1`.
Declare 1–4 pages; form.page names one. `host`, `local`, `scene`, `game`, `data`,
`media`, and `instrument` must be absent. Existing navigation may coexist.
Old profiles remain unchanged and must not advertise this feature without the
firmware capability. Existing 32 surface nodes, 8 vars, 24 input transitions,
and 8191 runtime JSON bytes remain unchanged; this does not enlarge them.

The form object has exactly **six required keys**:

```json
{"engine":"p4-app-form-v1","page":"edit","focus_var":"focus","active_var":"editing",
 "fields":[{"id":"level","var":"level","draft_var":"levelDraft","kind":"integer","min":0,"max":10,"step":1}],
 "actions":{"previous":"field.prev","next":"field.next","decrease":"value.down","increase":"value.up","accept":"form.accept","cancel":"form.cancel"}}
```

Fields number 1–3. Each has unique ASCII `[A-Za-z0-9_.-]{1,15}` id.
`integer` has exactly id/var/draft_var/kind/min/max/step. Bounds are integers
within ±1,000,000,000, min < max, step 1–1,000,000,000, and span divisible by
step. `choice` has exactly id/var/draft_var/kind/options; options number 2–8,
each exactly `{value,label}`. Values are unique bounded integers in declaration
order; labels are 1–63 UTF-8 bytes, with no C0 or DEL. C1 characters are not
newly banned. Form runtime strings/keys cannot contain NUL (cJSON parity).
Unknown keys at these extension levels are rejected, not ignored.

All committed, draft, focus and active names are distinct existing int vars
with safe names up to 31 ASCII bytes. A three-field form uses all eight vars.
Focus and active init/default are 0. Each committed init belongs to its choice
set or integer step grid, and its draft init equals it. If limits are declared,
they must contain the whole role range: field option min/max or integer bounds,
focus 0..fields−1, active 0..1. Do not silently reset old saved committed values
when adding the extension or narrowing a field constraint.

## Input, atomicity and feedback

Actions has exactly previous/next/decrease/increase/accept/cancel; six distinct
ASCII safe action ids up to 47 bytes. Each has exactly one transition
`{"from":"*","on":"action"}`, with no effects, state/page destination or
from_page. `host.*`, `input.*`, and existing system navigation actions are
reserved. These actions cannot use input.press, hold, confirm or cooldown.
Accept/cancel cannot repeat; the other four may use existing directional repeat.

All six actions need distinct physical paths through normal buttons.json.
Allowed physical events are SW1/SW2 short, encoder short/long, knob ccw/cw,
joystick up/down. SW3 stays global. Default example: up/down select the field,
left/right adjust, SW1 edit/accept, SW2 cancel. These are example mappings, not
runtime hardcoding: legal user remappings remain valid. Global voice/exit
ownership still wins, so deployments must retain six available paths after
their configured global overrides. Touch can supplement but cannot replace
these paths. Combined legacy knob strings do not satisfy this opt-in profile.

- Previous/next clamp focus; they may operate while closed. Decrease/increase
  only operate while editing, following choice order/integer step, without wrap.
- First accept copies committed values to drafts and opens editing. If an old
  committed value is outside a new constraint, only its draft starts at first
  choice/min. Closed choice display is `—` for unmapped old values.
- Second accept prevalidates every draft and atomically copies all of them to
  committed vars, then closes. It never also executes transition side effects.
- Cancel, hide, or an available non-form transition discards drafts. Invalid
  navigation (for example back at root) leaves drafts intact. Valid non-form
  transitions may then update committed vars using existing bounded effects.
- Non-form transitions cannot write focus/active/draft. Tick or tick.then cannot
  write any form-owned var. Focus remains after cancel; restoration starts at 0.
- Persistence may include committed vars, never focus/active/draft. Restoration
  initializes transient state from loaded committed values and leaves the save
  format, save version and unrelated data alone. No package data is erased.

Text/button nodes may use `field_bind:"field-id"`, mutually exclusive with
text/bind/var_bind/key_for. Meter and all other types reject field_bind. It
requires a known form field. Existing page semantics remain: omitted/page=4 is
shared; binding does not grant editing on another page. Field text shows draft
while editing, committed otherwise, with choice labels or decimal integers.
Use existing show/var_bind nodes for focus/active feedback and action-enabled
button styling. Form changes invalidate pending pointer clicks, not physical
repeat ownership. No additional renderer node type or per-frame allocation.

## Verification boundary

Offline tests cover identical action sequences, clamp/disabled actions, atomic
multi-field accept, cancellation, unavailable navigation, restoration with
out-of-range old values, strict invalid schemas and swapped legal physical
mappings. These are not hardware, screen, voice/AEC, or storage power-loss proof.
Do not claim dynamic lists, free-text input, touchscreen keyboards, or arbitrary
mobile UI as complete from this bounded vertical slice.
