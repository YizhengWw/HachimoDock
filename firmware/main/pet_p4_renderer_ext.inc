/* Track C renderer fragment: p4-big-image-v1 tile drawing and p4-clip-v1 playback.
 * Included by pet_p4_renderer.c after its codec helpers, the H.264 prefetch
 * fragment and the surface-image statics; never compiled as a separate owner.
 * Clips reuse the main-page decoder role on the existing priority-1 asset
 * worker (one job at a time), so a visible clip is exclusive with the pet
 * animation: the pet page is hidden while a component is open, and the first
 * main-page frame afterwards restarts its own stream (path mismatch -> reset). */
#if PET_P4_BIG_IMAGE || PET_P4_CLIP
#include "pet_p4_component_ext.h"
#include "pet_p4_component_media_clients.h"
static uint32_t g_ext_snapshot_generation;
#if PET_P4_BIG_IMAGE
static bool g_big_read_attempted;
#endif
/* Render-thread only: exactly one handoff before drawing this frame's nodes. */
void renderer_ext_media_snapshot(uint32_t generation) {
  g_ext_snapshot_generation = generation;
#if PET_P4_BIG_IMAGE
  g_big_read_attempted = false;
#endif
}
#endif
#if PET_P4_BIG_IMAGE
/* Fixed PSRAM slots cover both legal sources in full. Direct indexing prevents
 * pan/multiple-node eviction; generation remains part of every cache key.
 * One file-read attempt per frame, including failures, shared by ALL nodes. */
#define RENDERER_BIG_SLOTS (PET_CEXT_BIG_MAX * PET_CEXT_BIG_MAX_TILES)
typedef struct { bool valid; uint8_t big, tile; uint16_t w, h; uint32_t generation; uint8_t palette[256][4]; } renderer_big_slot_t;
static EXT_RAM_BSS_ATTR renderer_big_slot_t g_big_slots[RENDERER_BIG_SLOTS];
static EXT_RAM_BSS_ATTR uint8_t g_big_indices[RENDERER_BIG_SLOTS][PET_CEXT_BIG_TILE * PET_CEXT_BIG_TILE];
static EXT_RAM_BSS_ATTR uint8_t g_big_file[PET_CEXT_BIG_TILE_MAX_BYTES];
static renderer_big_slot_t *renderer_big_slot(unsigned big, unsigned tile, uint32_t generation) {
  if (big >= PET_CEXT_BIG_MAX || tile >= PET_CEXT_BIG_MAX_TILES || !generation) return NULL;
  unsigned slot = big * PET_CEXT_BIG_MAX_TILES + tile;
  renderer_big_slot_t *victim = &g_big_slots[slot];
  if (victim->valid && victim->generation == generation && victim->big == big && victim->tile == tile) return victim;
  if (g_big_read_attempted) return NULL;
  g_big_read_attempted = true; /* consume BEFORE IO/decode, even when either fails */
  size_t bytes = 0;
  if (!pet_cmedia_big_tile_try_at(big, tile, generation, g_big_file, sizeof(g_big_file), &bytes) || bytes < PET_IMAGE_HEADER) return NULL;
  victim->valid = false; /* failed decode must not expose a partially overwritten old tile */
  if (!pet_image_decode(g_big_file, bytes, g_big_indices[slot], sizeof(g_big_indices[slot]))) return NULL;
  const uint8_t *palette = g_big_file + 12;
  /* Retain the source RGBA bytes: quantizing to RGB565 before blending can
   * change translucent pixels. This uses 256 more PSRAM bytes per fixed slot. */
  memcpy(victim->palette, palette, sizeof(victim->palette));
  victim->w = g_big_file[4]; victim->h = g_big_file[5]; victim->big = (uint8_t) big; victim->tile = (uint8_t) tile;
  victim->generation = generation; victim->valid = true;
  return victim;
}
/* Draws only the tiles intersecting the node window; missing tiles leave the background. */
static void renderer_ext_draw_big(const pet_surface_node_t *n) {
  pet_cext_big_t meta;
  uint32_t generation = g_ext_snapshot_generation;
  if (!generation || n->big < 0 || !g_framebuffer || !pet_cmedia_big_meta_try_at((unsigned) n->big, generation, &meta)) return;
  int32_t ox = n->big_x, oy = n->big_y, max_x = (int32_t) meta.w - n->w, max_y = (int32_t) meta.h - n->h;
  if (ox < 0 || max_x <= 0) ox = 0; else if (ox > max_x) ox = max_x;
  if (oy < 0 || max_y <= 0) oy = 0; else if (oy > max_y) oy = max_y;
  uint8_t tiles[PET_CEXT_BIG_MAX_TILES];
  unsigned count = pet_cext_big_visible_tiles(&meta, ox, oy, n->w, n->h, tiles);
  for (unsigned t = 0; t < count; t++) {
    renderer_big_slot_t *s = renderer_big_slot((unsigned) n->big, tiles[t], generation);
    if (!s) continue;
    int tx = (tiles[t] % meta.cols) * meta.tile_w, ty = (tiles[t] / meta.cols) * meta.tile_h;
    int x0 = ox > tx ? ox : tx, y0 = oy > ty ? oy : ty;
    int x1 = ox + n->w < tx + s->w ? ox + n->w : tx + s->w, y1 = oy + n->h < ty + s->h ? oy + n->h : ty + s->h;
    const uint8_t *indices = g_big_indices[s - g_big_slots];
    for (int y = y0; y < y1; y++) {
      int dy = n->y + (y - oy); if (dy < 0 || dy >= PET_P4_UI_HEIGHT) continue;
      uint16_t *row = g_framebuffer + dy * PET_P4_UI_WIDTH; const uint8_t *src = indices + (y - ty) * s->w;
      for (int x = x0; x < x1; x++) {
        int dx = n->x + (x - ox); if (dx < 0 || dx >= PET_P4_UI_WIDTH || !app_clip_contains(dx, dy)) continue;
        const uint8_t *p = s->palette[src[x - tx]];unsigned a = p[3];
        if (!a) continue;
        if (a == 255) { row[dx] = rgb565(p[0], p[1], p[2]); continue; }
        row[dx] = blend_rgb565(row[dx], p[0], p[1], p[2], (uint8_t) a);
      }
    }
  }
}
#endif
#if PET_P4_CLIP
static struct { pet_cext_clip_state_t state; uint32_t generation; int presented; char path[80]; } g_clip;
static EXT_RAM_BSS_ATTR pet_p4_asset_cache_entry_t g_clip_entry;
static pet_cmedia_clip_lease_t g_clip_lease;
/* Main render thread only; caller invokes on every hidden/closed frame too.
 * Reset returning false means worker/native decoder cleanup still owns the source.
 * Do not mutate the descriptor or unpin until reset has physically finished. */
bool renderer_ext_clip_retire(void) {
  if (!g_clip_lease.token) return true;
  if (!h264_prefetch_reset()) return false;
  portENTER_CRITICAL(&g_asset_prepare_mux);
  bool idle = !g_clip_entry.decoder_readers && g_h264_prefetch_lifetime.phase == PET_PREFETCH_EMPTY;
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  if (!idle) return false;
  if (!pet_cmedia_unpin_deferred(g_clip_lease.token)) return false; /* retain token and retry next frame */
  if (g_clip_entry.owner.generation) {
    pet_resources_device_revoke(g_clip_entry.owner);
    (void)pet_resources_device_finish(g_clip_entry.buffer_lease);
  }
  memset(&g_clip_entry, 0, sizeof(g_clip_entry));
  memset(&g_clip_lease, 0, sizeof(g_clip_lease));
  memset(&g_clip, 0, sizeof(g_clip));
  return true;
}
static bool renderer_clip_entry(pet_cmedia_clip_lease_t *lease, const char *path) {
  if (!lease->token || g_clip_lease.token) return false;
  if (pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_ASSET_BUFFER), &g_clip_entry.owner) != PET_RESOURCE_OK) return false;
  if (pet_resources_device_acquire(g_clip_entry.owner, PET_RESOURCE_ASSET_BUFFER, (uint32_t)lease->bytes, &g_clip_entry.buffer_lease) != PET_RESOURCE_OK) {
    pet_resources_device_revoke(g_clip_entry.owner); memset(&g_clip_entry, 0, sizeof(g_clip_entry)); return false;
  }
  g_clip_lease = *lease; memset(lease, 0, sizeof(*lease)); /* ownership transfer */
  g_clip_entry.bytes = (uint8_t *)g_clip_lease.stream; g_clip_entry.size = g_clip_lease.bytes;
  g_clip_entry.finished = true; g_clip_entry.h264 = true; g_clip_entry.single_slice_valid = true;
  snprintf(g_clip_entry.fs_path, sizeof(g_clip_entry.fs_path), "%s", path);
  return true;
}
/* Set from the view snapshot, captured under the runtime lock by the owner.
 * Untagged draw calls are rejected rather than mapping an old node onto new media. */
static void renderer_clip_fill(const pet_surface_node_t *n, uint16_t color) { fill_rect(n->x, n->y, n->w, n->h, color); }
static void renderer_ext_draw_clip(const pet_surface_node_t *n, unsigned long long now_ms) {
  uint32_t generation = g_ext_snapshot_generation;
  if (!generation || !pet_cmedia_generation_try(generation) || !g_framebuffer) {
    (void)renderer_ext_clip_retire(); renderer_clip_fill(n, n->color); return;
  }
  if (g_clip_lease.token && (g_clip_lease.generation != generation || !pet_resources_device_current(g_clip_entry.buffer_lease))) {
    if (!renderer_ext_clip_retire()) { renderer_clip_fill(n, n->color); return; }
  }
  if (!g_clip_lease.token) {
    /* A prior main-page job/decoder must also fully retire before changing sources. */
    if (!h264_prefetch_reset()) { renderer_clip_fill(n, n->color); return; }
    pet_cmedia_clip_lease_t lease = {0};
    if (!pet_cmedia_clip_pin_try_at(0, generation, &lease)) { renderer_clip_fill(n, n->color); return; }
    snprintf(g_clip.path, sizeof(g_clip.path), "clip:%s", lease.meta.id);
    if (!renderer_clip_entry(&lease, g_clip.path)) {
      /* Failed admission still owns its source token until retirement accepts it. */
      g_clip_lease = lease;
      (void)renderer_ext_clip_retire(); renderer_clip_fill(n, n->color); return;
    }
    g_clip.generation = generation; g_clip.presented = -1;
    snprintf(g_h264_presented_path, sizeof(g_h264_presented_path), "%s", g_clip.path);
    g_h264_packed_current = (pet_h264_packed_view_t){0};
  }
  pet_cext_clip_t meta = g_clip_lease.meta;
  int target = pet_cext_clip_target(&g_clip.state, &meta, true, now_ms);
  if (target < 0 || meta.w != n->w || meta.h != n->h) { (void)renderer_ext_clip_retire(); renderer_clip_fill(n, n->color); return; }
  pet_h264_prefetch_job_t ready = {0}; bool taken = false, discarded = false;
  portENTER_CRITICAL(&g_asset_prepare_mux);
  pet_prefetch_phase_t phase = g_h264_prefetch_lifetime.phase;
  if (phase == PET_PREFETCH_DISCARDED) discarded = true;
  else if (phase == PET_PREFETCH_READY && !strcmp(g_h264_prefetch_job.path, g_clip.path)) { ready = g_h264_prefetch_job; taken = pet_prefetch_take(ready.ticket, ready.cancel_epoch + 1); }
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  if (discarded) { (void)renderer_ext_clip_retire(); renderer_clip_fill(n, n->color); return; }
  if (taken) {
    int stride = 0, storage_h = 0;
    bool layout = resolve_h264_i420_layout(meta.w, meta.h, ready.output_size, &stride, &storage_h);
    bool packed_ok = layout && h264_worker_pack_adopt(&ready, meta.w, meta.h, stride, storage_h);
    if (layout && !packed_ok) {
      const uint8_t *packed = NULL;
      packed_ok = pack_h264_i420_for_ppa(ready.output, meta.w, meta.h, stride, storage_h, &packed)
        && pet_h264_packed_view_init(&g_h264_packed_current, packed, g_h264_ppa_input_capacity, (size_t) meta.w * 3 / 2, meta.w, meta.h, storage_h);
    }
    portENTER_CRITICAL(&g_asset_prepare_mux);
    bool current = !g_h264_prefetch_lifetime.cancelled;
    (void) pet_prefetch_release(ready.ticket);
    portEXIT_CRITICAL(&g_asset_prepare_mux);
    /* Packing may already have replaced packed-current. A cancellation during
     * that work revokes delivery, including when an older frame was presented. */
    if (!current) {
      (void)renderer_ext_clip_retire(); renderer_clip_fill(n, n->color); return;
    }
    if (packed_ok) g_clip.presented = ready.frame;
  }
  /* Draw the newest decoded frame 1:1 (no rotation, no scaling): <= 320x240 pixels of
   * packed-YUV reads on the render task; decode itself stayed on the worker. */
  if (g_clip.presented >= 0 && g_h264_packed_current.data) {
    for (int y = 0; y < n->h; y++) {
      int dy = n->y + y; if (dy < 0 || dy >= PET_P4_UI_HEIGHT) continue;
      uint16_t *row = g_framebuffer + dy * PET_P4_UI_WIDTH;
      for (int x = 0; x < n->w; x++) { int dx = n->x + x; if (dx >= 0 && dx < PET_P4_UI_WIDTH && app_clip_contains(dx, dy)) row[dx] = pet_h264_packed_pixel(&g_h264_packed_current, x, y); }
    }
  } else renderer_clip_fill(n, n->color);
  /* H.264 frames depend on their predecessors: always decode the next one in stream
   * order, restarting at the loop boundary. Under load the clip simply advances slower. */
  int next = g_clip.presented < 0 ? 0 : (g_clip.presented + 1) % meta.frames;
  if (g_clip.presented != target) (void) h264_prefetch_offer(&g_clip_entry, g_clip.path, meta.w, meta.h, next == 0, next);
}
#endif
