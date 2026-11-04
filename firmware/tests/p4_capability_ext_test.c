/* Host harness: runs the production Track C parser (pet_p4_component_ext.c) over a
 * runtime/widget.json read from stdin exactly the way the miniapp hooks do
 * (sections, surface media, transition/tick ops, event subscriptions). Prints
 * {"ok":..,"error":".."}. No device, files or audio. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "pet_p4_component_ext.h"
static const cJSON *g_root;
static int find_int_var(void *ctx, const char *name) {
  (void) ctx;
  const cJSON *vars = cJSON_GetObjectItemCaseSensitive(g_root, "vars"), *v = cJSON_GetObjectItemCaseSensitive(vars, name);
  if (!cJSON_IsObject(v)) return -1;
  const cJSON *type = cJSON_GetObjectItemCaseSensitive(v, "type");
  if (!cJSON_IsString(type) || strcmp(type->valuestring, "int")) return -1;
  int index = 0; const cJSON *it;
  cJSON_ArrayForEach(it, vars) { if (it == v) return index; index++; }
  return -1;
}
static bool has_action(void *ctx, const char *action) {
  (void) ctx; const cJSON *t;
  cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(g_root, "transitions")) {
    const cJSON *on = cJSON_GetObjectItemCaseSensitive(t, "on");
    if (cJSON_IsString(on) && !strcmp(on->valuestring, action)) return true;
  }
  return false;
}
static const char *str(const cJSON *o, const char *k) { const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k); return cJSON_IsString(v) ? v->valuestring : ""; }
int main(void) {
  static char buffer[1 << 16]; size_t n = fread(buffer, 1, sizeof(buffer) - 1, stdin); buffer[n] = 0;
  cJSON *document = cJSON_Parse(buffer); if (!document) { puts("{\"ok\":false,\"error\":\"json\"}"); return 1; }
  cJSON *root = cJSON_GetObjectItemCaseSensitive(document,"runtime");
  if (!root) root = document;
  g_root = root;
  static pet_cext_runtime_t ext; char error[160] = {0}; bool ok = true;
  const cJSON *surface = cJSON_GetObjectItemCaseSensitive(root, "surface");
  pet_cext_parse_env_t env = {
    .runtime_v4 = !strcmp(str(root, "engine"), "p4-bounded-runtime-v4"),
    .exclusive_audio_owner = cJSON_HasObjectItem(root, "instrument") || cJSON_HasObjectItem(root, "media") || cJSON_HasObjectItem(root, "local") || cJSON_HasObjectItem(root, "program"),
    .persist = cJSON_HasObjectItem(root, "persist"),
    .surface_app = !strcmp(str(surface, "engine"), "p4-app-surface-v1"),
    .ctx = NULL, .find_int_var = find_int_var, .has_action = has_action,
  };
  ok = pet_cext_parse(root, &ext, &env, error, sizeof(error));
  if (ok && surface) ok = pet_cext_parse_surface_media(surface, &ext, error, sizeof(error));
  if (ok && (ext.clip || ext.big) && !env.surface_app) { ok = false; snprintf(error, sizeof(error), "clip and big image nodes need the app surface"); }
  const cJSON *t;
  cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(root, "transitions")) {
    pet_cext_ops_t ops;
    if (!ok) break;
    ok = pet_cext_parse_ops(&ext, t, &ops, error, sizeof(error));
    if (ok && !pet_cext_add_subscription(&ext, str(t, "on"))) { ok = false; snprintf(error, sizeof(error), "event subscription requires events p4-events-v1 and at most 8 safe names"); }
    if (ok) pet_cext_fire(&ext, &ops);
  }
  cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(root, "tick")) {
    pet_cext_ops_t ops; const cJSON *then = cJSON_GetObjectItemCaseSensitive(t, "then");
    if (!ok) break;
    if (then) ok = pet_cext_parse_ops(&ext, then, &ops, error, sizeof(error));
  }
  bool parsed_ok = ok;
  const cJSON *buttons = cJSON_GetObjectItemCaseSensitive(document, "buttons");
  if (ok && buttons) ok = pet_cext_binding_coverage(&ext, root, buttons, error, sizeof(error));
  unsigned internal_mask = 0, query = 0; const cJSON *queries = cJSON_GetObjectItemCaseSensitive(document, "queries"), *q;
  cJSON_ArrayForEach(q, queries) { if (query < 32 && cJSON_IsString(q) && parsed_ok && pet_cext_internal_action(&ext, q->valuestring)) internal_mask |= 1u << query; query++; }
  printf("{\"internal_mask\":%u,\"ok\":%s,\"error\":\"%s\",\"sequence\":%s,\"samples\":%u,\"emits\":%u,\"subs\":%u,\"alarms\":%u,\"clips\":%u,\"bigs\":%u,\"emit_pending\":%u}\n",
    internal_mask, ok ? "true" : "false", error, ext.sequence ? "true" : "false", ext.sample_count, ext.emit_count, ext.sub_count, ext.alarms.count, ext.clip_count, ext.big_count, ext.emit_pending);
  cJSON_Delete(document);
  return 0;
}
