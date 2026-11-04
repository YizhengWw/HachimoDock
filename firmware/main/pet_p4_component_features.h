/* Development implementation and physical admission are separate gates.
 * Do not set REVIEWED merely to make installation tests pass. Ordinary builds
 * keep both off. The UI target compiles implementation only until measured. */
#pragma once
/* Private raster-only candidate; no parser/lease/execution grant. */
#ifndef PET_P4_APP_DIRTY_CACHE
#define PET_P4_APP_DIRTY_CACHE 0
#endif
#ifndef PET_COMPONENT_PROGRAM_ENABLED
#define PET_COMPONENT_PROGRAM_ENABLED 0
#endif
#ifndef PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED
#define PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED 0
#endif
#ifndef PET_COMPONENT_BACKGROUND_ENABLED
#define PET_COMPONENT_BACKGROUND_ENABLED 0
#endif
#ifndef PET_COMPONENT_PROGRAM_PARENT_UNIFIED
#define PET_COMPONENT_PROGRAM_PARENT_UNIFIED 0
#endif
#ifndef PET_COMPONENT_PROGRAM_RAM_RESUME
#define PET_COMPONENT_PROGRAM_RAM_RESUME 0
#endif
/* Public program candidate profile (normal-build admission, 2026-11-01 engineering
 * defaults): exactly ONE PROGRAM lease / 4096 fuel total, PERIPHERAL stays zero,
 * background still PC-started only, no autostart/alarm, audio reservation still
 * revokes. An explicit quota-table entry, never inferred from the private chain. */
#ifndef PET_P4_PUBLIC_PROGRAM_PROFILE
#define PET_P4_PUBLIC_PROGRAM_PROFILE 0
#endif
#if PET_P4_PUBLIC_PROGRAM_PROFILE && !(PET_COMPONENT_PROGRAM_ENABLED && PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED && PET_COMPONENT_BACKGROUND_ENABLED && PET_COMPONENT_PROGRAM_PARENT_UNIFIED)
#error "Public program profile requires the reviewed program, background and unified parent switches"
#endif
#if PET_P4_PUBLIC_PROGRAM_PROFILE && (PET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE || PET_VARIABLE_OWNERS_ENABLED || PET_COMPONENT_PROGRAM_RAM_RESUME)
#error "Public program profile excludes private capacity acceptance, variable owners and RAM resume"
#endif
#if PET_COMPONENT_PROGRAM_RAM_RESUME && !PET_COMPONENT_PROGRAM_PARENT_UNIFIED
#error "Explicit RAM resume requires the reviewed unified native parent"
#endif
#if PET_COMPONENT_PROGRAM_PARENT_UNIFIED && !PET_COMPONENT_BACKGROUND_ENABLED
#error "Unified program parent requires the reviewed installed background parent"
#endif
#if PET_COMPONENT_BACKGROUND_ENABLED && (!PET_COMPONENT_PROGRAM_ENABLED || !PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED)
#error "Background parent requires the complete reviewed private program backend"
#endif
#if PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED && !PET_COMPONENT_PROGRAM_ENABLED
#error "Program execution requires the complete component implementation"
#endif
/* Track A package capabilities (r150 capability parent). Each is a default-off
 * negotiated package grant: the hello only advertises the capability string
 * when its flag is on, and every PC validator refuses packages the device did
 * not advertise. Big new runtime state stays in PSRAM (EXT_RAM_BSS_ATTR). */
/* p4-cells-v1: a bounded 1..16 x 1..16 grid of 2/4-bit cells (<=256) owned by
 * the bounded runtime: transition/tick ops, conditions, count/run helpers, an
 * atlas-tiled surface node, and (with program v2) a VM cell mirror. */
#ifndef PET_P4_CELLS_V1
#define PET_P4_CELLS_V1 0
#endif
/* p4-texts-v1: a package text table (<=64 entries, <=120 B each, <=4096 B)
 * addressed by integer variables through {"text_at": "<var>"}. */
#ifndef PET_P4_TEXT_TABLE
#define PET_P4_TEXT_TABLE 0
#endif
/* p4-quota-v3: larger negotiated array ceilings (vars 32, states 16, pages 12,
 * transitions 48, ticks 16, choices 12, nodes 64, deck items 64, persist 32,
 * string vars 127 B, widget.json 12287 B). v1/v2 packages keep their numbers. */
#ifndef PET_P4_QUOTA_V3
#define PET_P4_QUOTA_V3 0
#endif
/* p4-component-program-v2: code <=1024, fuel <=16384 (same 64-per-turn slice),
 * VM cells <=256, TEXT/CELLGET/CELLSET opcodes. v1 images stay valid. */
#ifndef PET_P4_PROGRAM_V2
#define PET_P4_PROGRAM_V2 0
#endif
#if PET_P4_TEXT_TABLE && !PET_P4_APP_DIRTY_CACHE
#error "Text table bindings resolve under the exact dirty clean-frame parent"
#endif
#if PET_P4_QUOTA_V3 && !PET_P4_TEXT_TABLE
#error "Quota v3 string ceilings ride the bounded text-table parent"
#endif
#if PET_P4_PROGRAM_V2 && !PET_COMPONENT_PROGRAM_ENABLED
#error "Program v2 requires the complete component program implementation"
#endif
#if PET_P4_PROGRAM_V2 && !PET_P4_TEXT_TABLE
#error "Program v2 TEXT selection requires the package text table"
#endif
#if PET_P4_CELLS_V1 && PET_COMPONENT_PROGRAM_ENABLED && !PET_P4_PROGRAM_V2
#error "Cells with a component program require the program v2 cell mirror"
#endif
/* Track C capability expansion (2026-11-02, r150 capability parent). Each flag is
 * one negotiated package capability; ordinary builds keep every one off and the
 * hello omits the matching widgetXxx string, so PC validators refuse the package.
 * Portable cores (sfx sequencer/sampler, event bus, alarm set) always compile; a
 * flag only adds the parser keys, the install file kinds and the runtime hooks. */
/* widgetSequence = p4-sequence-v1: on-device note sequencer through the sfx mixer */
#ifndef PET_P4_SEQUENCE
#define PET_P4_SEQUENCE 0
#endif
/* widgetSamples = p4-samples-v1: <=4 PCM one-shots (<=64 KiB each, 256 KiB budget) in PSRAM */
#ifndef PET_P4_SAMPLES
#define PET_P4_SAMPLES 0
#endif
/* widgetClip = p4-clip-v1: one H.264 clip node decoded on the prefetch worker */
#ifndef PET_P4_CLIP
#define PET_P4_CLIP 0
#endif
/* widgetBigImage = p4-big-image-v1: tiled 240x240 P4I1 pannable image, 1 MiB budget */
#ifndef PET_P4_BIG_IMAGE
#define PET_P4_BIG_IMAGE 0
#endif
/* widgetEvents = p4-events-v1: bounded cross-component event bus and persistent alarms */
#ifndef PET_P4_EVENTS
#define PET_P4_EVENTS 0
#endif
#define PET_P4_CAPABILITY_EXT (PET_P4_SEQUENCE || PET_P4_SAMPLES || PET_P4_CLIP || PET_P4_BIG_IMAGE || PET_P4_EVENTS)
#if PET_P4_CLIP && !(defined(PET_P4_H264_WORKER_PACK) && PET_P4_H264_WORKER_PACK)
#error "Component clips reuse the worker-side H.264 decode/pack parent (PET_P4_H264_WORKER_PACK)"
#endif
/* Track D app-shell capabilities (same r150 capability parent). Each is a
 * default-off negotiated package grant advertised in hello only when on. */
/* p4-app-shell-v1: pages addressed 0..11 (quota v3 ceiling), a bottom
 * surface.tabs bar expanded into standard button nodes, reserved tab.next /
 * tab.prev actions and the existing 8-deep navigation stack. */
#ifndef PET_P4_APP_SHELL
#define PET_P4_APP_SHELL 0
#endif
/* p4-data-rows-v1: generic data_view layout "rows", data.filters (<=4 enum
 * filters bound to int vars), data.select_var, row_bind text nodes and the
 * reserved data.refresh action (PC re-poll with cooldown). */
#ifndef PET_P4_DATA_ROWS
#define PET_P4_DATA_ROWS 0
#endif
/* p4-longtext-v1: surface.texts (<=2 x 4096 B), longtext nodes paginated by
 * the deterministic pet_p4_text_flow.h layout, figure (image + caption). */
#ifndef PET_P4_LONGTEXT
#define PET_P4_LONGTEXT 0
#endif
/* p4-app-form-v3: up to 4 text fields with an on-device keyboard engine
 * (numeric keypad, Latin T9, pinyin T9 over the bounded MIT table). */
#ifndef PET_FORM_V3_ENABLED
#define PET_FORM_V3_ENABLED 0
#endif
/* p4-notify-v1: bounded widget/notify topic (PC toast + main-page bubble). */
#ifndef PET_P4_NOTIFY
#define PET_P4_NOTIFY 0
#endif
#if PET_P4_APP_SHELL && !PET_P4_QUOTA_V3
#error "The app shell addresses up to 12 pages and rides the quota v3 parent"
#endif
#if PET_FORM_V3_ENABLED && !PET_FORM_V2_ENABLED
#error "Form v3 extends the reviewed mixed text form engine"
#endif
