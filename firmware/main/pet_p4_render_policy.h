/* Private native raster candidates, not package grants or timing policies. */
#pragma once
/* Cached pet animation may coexist with initialized conversation audio. New
 * filesystem/cache work remains protected, as does audio initialization. */
#ifndef PET_P4_CONVERSATION_VIDEO
#define PET_P4_CONVERSATION_VIDEO 0
#endif
#if PET_P4_CONVERSATION_VIDEO && !(defined(PET_P4_H264_PREFETCH) && PET_P4_H264_PREFETCH)
#error Conversation animation requires the existing native prefetch worker
#endif
#ifndef PET_P4_APP_PANEL_MIRROR
#define PET_P4_APP_PANEL_MIRROR 0
#endif
#ifndef PET_P4_LOGICAL_COMPARE
#define PET_P4_LOGICAL_COMPARE 0
#endif
#ifndef PET_P4_APP_RETAIN_LOGICAL
#define PET_P4_APP_RETAIN_LOGICAL 0
#endif
#if PET_P4_LOGICAL_COMPARE && !PET_P4_APP_CACHE_PROGRESSIVE
#error Logical comparison requires the exact staged-key cache parent
#endif
#if PET_P4_APP_RETAIN_LOGICAL && (!PET_P4_LOGICAL_COMPARE || !PET_P4_APP_CACHE_PROGRESSIVE)
#error Final logical retention requires exact comparison and staged-key ownership
#endif
/* Private exact clear of only the area the first opaque layer does not
 * overwrite itself, for surfaces with fonts/images too. Same pixels. */
#ifndef PET_P4_APP_CLEAR_OPAQUE_CENTER
#define PET_P4_APP_CLEAR_OPAQUE_CENTER 0
#endif
#if PET_P4_APP_CLEAR_OPAQUE_CENTER && !PET_P4_APP_DIRTY_CACHE
#error Opaque-center clear requires the exact dirty clean-frame parent
#endif
/* Numeric frame/widget-operation overlap evidence; never a cause or CPU time. */
#ifndef PET_P4_FRAME_IO_OVERLAP
#define PET_P4_FRAME_IO_OVERLAP 0
#endif
/* Main-loop context retained for the boot-lifetime inter-frame gap peak:
 * state-mutex wait, mutex-held processing and component processing maxima
 * between two frames, plus the widget operation lanes at frame start. */
#ifndef PET_P4_GAP_PEAK_CONTEXT
#define PET_P4_GAP_PEAK_CONTEXT 0
#endif
#if PET_P4_GAP_PEAK_CONTEXT && !PET_P4_FRAME_IO_OVERLAP
#error Gap peak context requires the frame/IO overlap evidence parent
#endif
/* Attribution for the gap-peak wait: the state-mutex holder class sampled by
 * the main loop before its longest wait, the protocol worker's in-flight topic
 * hash, the wall clock of the peak frame, and the worker's longest locked span.
 * Correlation only; no scheduling or locking change. */
#ifndef PET_P4_GAP_PEAK_HOLDER
#define PET_P4_GAP_PEAK_HOLDER 0
#endif
#if PET_P4_GAP_PEAK_HOLDER && !PET_P4_GAP_PEAK_CONTEXT
#error Gap peak holder attribution requires the gap peak context parent
#endif
/* Cold APP frame fast paths, each pixel-exact to the loop it bypasses: a 1:1
 * unrotated untiled palette image is blitted per row; a radius-0 constant-
 * alpha panel is blended per span; the full clear is skipped when the first
 * drawn node is a full-screen opaque palette image. Counters only otherwise. */
#ifndef PET_P4_APP_COLD_FAST
#define PET_P4_APP_COLD_FAST 0
#endif
#if PET_P4_APP_COLD_FAST && !PET_P4_APP_CLEAR_OPAQUE_CENTER
#error Cold APP fast paths require the opaque-center clear parent
#endif
/* APP clean-frame copies (cache populate / prefix copy / cache-hit copy) through
 * the PPA SRM client as a 1:1 blocking bit copy; row-sampled self-check with a
 * permanent CPU fallback on any mismatch. Same bytes, DMA instead of CPU. */
#ifndef PET_P4_APP_PPA_COPY
#define PET_P4_APP_PPA_COPY 0
#endif
#if PET_P4_APP_PPA_COPY && !PET_P4_APP_COLD_FAST
#error PPA frame copy requires the cold APP fast-path parent
#endif
/* Main-page H.264 frames: the I420 -> packed-YUV420 rewrite the PPA needs
 * (17 ms of render-frame CPU/join) runs on the existing priority-1 prefetch
 * worker right after its decode, into one of two PSRAM back buffers; the render
 * frame only adopts the finished buffer and keeps its single PPA convert. Same
 * kernel, same bytes, same conversion; no new task, priority or clock. */
#ifndef PET_P4_H264_WORKER_PACK
#define PET_P4_H264_WORKER_PACK 0
#endif
#if PET_P4_H264_WORKER_PACK && !PET_P4_APP_PPA_COPY
#error Worker-side H264 pack requires the PPA frame copy parent
#endif
#if PET_P4_H264_WORKER_PACK && !(defined(PET_P4_H264_PREFETCH) && PET_P4_H264_PREFETCH)
#error Worker-side H264 pack requires the native prefetch worker
#endif
#if PET_P4_CONVERSATION_VIDEO && !PET_P4_H264_WORKER_PACK
#error Conversation animation requires frame-bound worker offers
#endif
/* Capability expansion (r150 capability parent). Each switch is a package-visible
 * capability advertised in hello only while on; default-off keeps every public
 * target byte-identical in format, budgets and hello. */
/* p4-graphics-v2: per-package graphics quota (8 scene sprites / 16384 pixels,
 * 6 surface images / 196608 palette pixels / 256 KiB compiled, 4 fonts of 98
 * chars) selected by a package that declares graphics:"p4-graphics-v2". */
#ifndef PET_P4_GRAPHICS_V2
#define PET_P4_GRAPHICS_V2 0
#endif
/* p4-canvas-v2: 64 commands, 16 bound names, 8192 spans, polyline/arc/ring
 * on the same integer rasterizer; v1 declarations keep v1 budgets. */
#ifndef PET_P4_CANVAS_V2
#define PET_P4_CANVAS_V2 0
#endif
/* Expanded canvas work is admitted only by the graphics v2 parent. The
 * interpreter also rejects an explicitly disabled canvas beneath a v2 build. */
#if PET_P4_CANVAS_V2 && !PET_P4_GRAPHICS_V2
#error Canvas v2 requires the graphics v2 quota parent
#endif
/* p4-particles-v1: up to two decorative fixed-step Q8 dot emitters per surface,
 * no collisions, bounded dots per frame, var guard. */
#ifndef PET_P4_PARTICLES
#define PET_P4_PARTICLES 0
#endif
#if PET_P4_PARTICLES && !PET_P4_GRAPHICS_V2
#error Particle emitters require the graphics v2 quota parent
#endif
/* p4-keyframe-bg-v1: the JPEG background may carry 2-4 P4J1 frames swapped at
 * most every 500 ms; the frame decode runs on the existing asset worker into a
 * PSRAM cache, never in the render frame. */
#ifndef PET_P4_KEYFRAME_BG
#define PET_P4_KEYFRAME_BG 0
#endif
#if PET_P4_KEYFRAME_BG && !PET_P4_GRAPHICS_V2
#error Keyframe backgrounds require the graphics v2 quota parent
#endif
#if PET_P4_KEYFRAME_BG && !(defined(PET_P4_H264_PREFETCH) && PET_P4_H264_PREFETCH)
#error Keyframe backgrounds require the native asset prefetch worker
#endif
/* p4-bodies-v1: up to 8 Q8 rigid bodies on the open grid scene with gravity,
 * elastic body/body and body/wall collisions, impulse op and rolling rotation. */
#ifndef PET_P4_BODIES
#define PET_P4_BODIES 0
#endif
#if PET_P4_BODIES && !PET_P4_GRAPHICS_V2
#error Bodies require the graphics v2 quota parent
#endif
