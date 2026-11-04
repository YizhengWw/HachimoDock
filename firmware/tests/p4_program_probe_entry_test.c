/* Compile the exact main.c entry function, with cJSON and read-only gate stubs. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "pet_p4_program_probe.h"
typedef pet_program_probe_send_fn pet_p4_send_line_fn;
#define portMAX_DELAY 0xffffffffU
static int g_state_mutex = 1, locks, takes, probe_calls, replies;
static struct { bool asset_transfer_active; unsigned sentinel; } g_state = {false, 0x12345678};
static bool ota, reboot;
static int route_a, route_b;
static void *expected_route;
static const char *expected_error;
static bool expected_publication;
static unsigned publication_calls;
static int xSemaphoreTake(int mutex, unsigned wait) {
  assert(mutex == 1 && wait == portMAX_DELAY && locks == 0); locks++; takes++; return 1;
}
static void xSemaphoreGive(int mutex) { assert(mutex == 1 && locks == 1); locks--; }
static bool pet_p4_ota_transfer_active(void) { assert(locks == 1); return ota; }
static bool pet_p4_diagnostics_reboot_pending(void) { assert(locks == 1); return reboot; }
static void receive(const char *line, void *context) {
  assert(locks == 0 && context == expected_route);
  cJSON *root = cJSON_Parse(line); assert(root);
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(root, "topic")->valuestring, expected_publication?"debug/program_publication_result":"debug/program_probe_result"));
  const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload"); assert(cJSON_IsObject(payload));
  if (expected_error) {
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(payload, "ok")));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(payload, "error")->valuestring, expected_error));
  } else assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload, "ok")));
  cJSON_Delete(root); replies++;
}
void pet_p4_program_probe(pet_program_probe_send_fn send, void *context) {
  assert(!locks && !g_state.asset_transfer_active && !ota && !reboot);
  assert(send == receive && context == expected_route);
  probe_calls++;
  send("{\"topic\":\"debug/program_probe_result\",\"payload\":{\"ok\":true}}", context);
}
void pet_p4_program_publication_probe(pet_program_probe_send_fn send,void *context){
  assert(!locks&&!g_state.asset_transfer_active&&!ota&&!reboot&&expected_publication);publication_calls++;
  send("{\"topic\":\"debug/program_publication_result\",\"payload\":{\"ok\":true}}",context);
}
#include "program_probe_entry.inc"
int main(void) {
  const char *valid = "{\"topic\":\"debug/program_probe\",\"payload\":{}}";
  expected_route = &route_a;
  assert(!maybe_handle_program_probe_line(NULL, receive, expected_route));
  assert(!maybe_handle_program_probe_line("{\"topic\":\"other\",\"payload\":{}}", receive, expected_route));
  assert(!maybe_handle_program_probe_line("{\"topic\":\"debug/program_probe_suffix\",\"payload\":{}}", receive, expected_route));
  assert(maybe_handle_program_probe_line(valid, receive, expected_route));
  expected_route = &route_b;
  assert(maybe_handle_program_probe_line(valid, receive, expected_route));
  assert(probe_calls == 2 && replies == 2 && takes == 2);
  const char *invalid[] = {
    "{\"topic\":\"debug/program_probe\"}",
    "{\"topic\":\"debug/program_probe\",\"payload\":null}",
    "{\"topic\":\"debug/program_probe\",\"payload\":[]}",
    "{\"topic\":\"debug/program_probe\",\"payload\":{\"case\":0}}",
    "{\"topic\":\"debug/program_probe\",\"payload\":{\"bytecode\":[0]}}",
    "{\"topic\":\"debug/program_probe\",\"payload\":{\"owner\":1}}",
    "{\"topic\":\"debug/program_probe\",\"payload\":{\"grants\":[]}}",
    "{\"topic\":\"debug/program_probe\",\"payload\":{\"state\":{}}}",
    "{\"topic\":\"debug/program_probe\",\"payload\":{},\"extra\":1}",
    "{\"topic\":\"debug/program_probe\",\"payload\":{},\"payload\":{}}",
    "{\"topic\":\"debug/program_probe\",\"topic\":\"debug/program_probe\",\"payload\":{}}",
  };
  expected_error = "invalid_request";
  for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    int before_replies = replies;
    assert(maybe_handle_program_probe_line(invalid[i], receive, expected_route));
    assert(replies == before_replies + 1 && probe_calls == 2 && takes == 2);
  }
  /* Malformed complete-input parsing may delegate to the normal parser, but
   * it must never call the probe or acknowledge successful execution. */
  (void)maybe_handle_program_probe_line("{\"topic\":\"debug/program_probe\",\"payload\":{}}junk", receive, expected_route);
  (void)maybe_handle_program_probe_line("{\"topic\":\"debug/program_probe\",\"payload\":{}}{}", receive, expected_route);
  assert(probe_calls == 2 && takes == 2);
  expected_error = "transfer_busy";
  for (unsigned i = 0; i < 3; i++) {
    g_state.asset_transfer_active = i == 0; ota = i == 1; reboot = i == 2;
    assert(maybe_handle_program_probe_line(valid, receive, expected_route));
    assert(probe_calls == 2 && locks == 0 && g_state.sentinel == 0x12345678);
  }
  assert(takes == 5);
  g_state.asset_transfer_active=ota=reboot=false;expected_error=NULL;expected_publication=true;
  const char *publication="{\"topic\":\"debug/program_publication_probe\",\"payload\":{}}";
  assert(maybe_handle_program_probe_line(publication,receive,expected_route)&&publication_calls==1&&takes==6);
  expected_error="invalid_request";
  assert(maybe_handle_program_probe_line("{\"topic\":\"debug/program_publication_probe\",\"payload\":{\"program\":[]}}",receive,expected_route));
  assert(maybe_handle_program_probe_line("{\"topic\":\"debug/program_publication_probe\",\"payload\":{},\"payload\":{}}",receive,expected_route));
  assert(publication_calls==1&&takes==6);ota=true;expected_error="transfer_busy";
  assert(maybe_handle_program_probe_line(publication,receive,expected_route)&&publication_calls==1&&takes==7);
  puts("PASS exact diagnostic entry, no bytecode/owner/grants/state input, caller route, unlocked read-only gates");
  return 0;
}
