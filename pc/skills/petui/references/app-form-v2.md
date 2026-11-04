# Mixed text forms — development capability, default off

This extends the generic form, not a component-specific editor. Normal firmware
keeps `PET_FORM_V2_ENABLED=0` and advertises only v1. The independent UI candidate
target enables `widgetAppFormV2=p4-app-form-v2`; import validation is not a grant
to install it on a device without that exact capability. P4 timing, real input,
screen glyph coverage and voice/AEC coexistence remain hardware acceptance gates.
The separately gated [variable owner profile](variable-owners.md) may combine
disjoint host fields with native forms; legacy host packages remain excluded.

## Contract and ownership

Keep the v1 runtime/surface/graphics/bindings/input profiles, pages, legacy data
and budgets. Change form.engine to `p4-app-form-v2`, add a required `keyboard`,
and add `append` and `erase` to the six existing actions. Integer and choice
fields retain their v1 schema. A text field has exactly:

```json
{"id":"name","var":"title","draft_var":"titleDraft","kind":"text","max_bytes":63}
```

Committed and draft vars must be distinct declared `string` vars with equal
initial values. The maximum is 1–63 **UTF-8 bytes**, not characters. Strings are
complete Unicode scalars without C0/C1 controls, surrogate values or NUL. There
is no normalization or truncation. Multiple scalars can form a visible grapheme;
erase removes one scalar, not a whole grapheme. This is a bounded single-line
editor, not rich text or a device OS input method.

`keyboard` has exactly index_var, label_var, characters, and optional space_label.
The index is a distinct int var initialized to 0. The label is a distinct string
var initialized to the displayed first character. `characters` contains 1–96
unique single scalars (each at most four bytes). An actual space displays the
1–16-byte nonempty space_label (default `Space`), but append inserts a space,
not that label. Limits cannot be attached to string roles; integer limits must
cover the entire role range. All roles use the **existing eight var slots**:
one text plus one integer/choice consumes eight. This does not increase field,
node, JSON, input or resource quotas; two fields is the effective mixed maximum.

Eight distinct physical paths are required: SW1/SW2 short, encoder short/long,
knob ccw/cw, joystick up/down. Existing global voice/exit overrides take
precedence. Every logical operation has one wildcard no-op transition. Accept,
cancel, append and erase cannot repeat or use hold/press/confirmation/cooldown.
Touch may supplement these operations; it does not remove reachability checks.
Neither ordinary transitions nor ticks may write editor-owned vars. This
separates editor state from external effects, rather than guessing ownership.

## Editing, restoration and lifecycle

First accept opens drafts; second validates all fields before atomically changing
any committed value. Previous/next chooses a field. Decrease/increase adjusts
an integer/choice or the selected keyboard scalar. Append and erase apply only
to the active text draft. Bounds disable operations rather than wrap/truncate.
Cancel/hide/available navigation discards drafts; unavailable root-back does not.
Pending pointer identity includes editor epoch and keyboard selection.

Only committed text/int fields may be persisted. V2 explicitly opts text into
the **existing versioned JSON save envelope**, not a new auto-migration/reset.
Transient drafts, focus, active and keyboard vars never persist. Valid old text
above a newly narrowed field maximum remains intact in committed and draft;
the user may erase or replace it before accepting. Malformed/renamed/duplicate
saved fields, invalid state/page or schema conflicts reject restoration, keeping
the old file. Failed cold restore cannot continue with defaults and overwrite it.

PC previews provide a trusted optional text/IME input while a text draft is
active. Composition changes only the local draft after completion; stale
completion is rejected after field/page/context change. Enter is not accept.
The replace-draft API is not wire authority and does not commit. Device editing
uses the published alphabet, not a hidden PC dependency. Character acceptance
does not prove resident font coverage: provide the needed glyph atlas and verify
the actual screen; an arbitrary Unicode keyboard is not claimed.

## Shared implementation and verification

Device and PC use the exact `pet_p4_form_v2.c` logic. The preview instantiates
only product-owned fixed-memory (262144 bytes), zero-import WASM when v2 is used;
components cannot supply executable modules. Python verification delegates to
this core rather than adding another editor model. Rust's validation-only v1
projection checks shared invariants; projected numbers never run or save data.
`contracts/app-form-v2.json` drives native, WASM, JS, Python and Rust rejection
checks. Rebuild/check mirrors with `node pc/scripts/build-form-runtime.mjs --check`.

Sanitizers, bounded mutation runs, mixed atomic accept, scalar deletion, drafts,
IME events, typed restoration and strict schemas are offline evidence only.
No component is published or installed, and no device/audio/cloud is used by
these tests. This slice does not complete dynamic collections, arbitrary
phone UI, capability coexistence, program deployment or native peripheral APIs.
