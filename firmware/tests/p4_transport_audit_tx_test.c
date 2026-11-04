/* Reuse the exact asynchronous bounded-queue driver fixture. Existing baseline
 * assertions remain in its separately executed test; no physical UART is used. */
#define main baseline_transport_main
#include "p4_transport_stress_test.c"
#undef main
#include "cJSON.h"
typedef void (*pet_p4_send_line_fn)(const char *, void *);
#define EXT_RAM_BSS_ATTR
#include "production_audit_handler.inc"

static unsigned fingerprint_calls;
static char *mutate_frame;
static unsigned mutation_call;
bool real_audit_fingerprint(const char *, size_t, uint32_t *);
bool pet_transport_audit_fingerprint(const char *body, size_t bytes, uint32_t *out) {
  fingerprint_calls++;
  if(mutate_frame&&fingerprint_calls==mutation_call){mutate_frame[15]^=3;mutate_frame=NULL;}
  return real_audit_fingerprint(body, bytes, out);
}
static void arm(void) {
  assert(!pthread_mutex_lock(&transport_mutex));
  uint32_t id = pet_transport_audit_prepare(&g_transport_audit);
  assert(id && pet_transport_audit_arm(&g_transport_audit, id));
  assert(!pthread_mutex_unlock(&transport_mutex));
}
static pet_transport_audit_snapshot_t close_capture(void) {
  pet_transport_audit_snapshot_t snapshot;
  assert(!pthread_mutex_lock(&transport_mutex));
  pet_transport_audit_close(&g_transport_audit, &snapshot);
  assert(!pthread_mutex_unlock(&transport_mutex));
  return snapshot;
}
static void reset_wire(void) { reset_driver(&uart_driver); reset_driver(&usb_driver); }
static void check_success_record(const pet_transport_audit_record_t *r, unsigned route) {
  assert(r->route == (pet_transport_audit_route_t)route);
  assert(r->body_bytes == strlen(diagnostic) && r->fingerprint_available);
  assert(r->fingerprint_after_available&&r->fingerprint_after==r->fingerprint);
  if (route == PET_TRANSPORT_AUDIT_NATIVE_USB) {
    assert(!r->driver_observed && r->body_drain == PET_TRANSPORT_AUDIT_NOT_OBSERVED);
    assert(r->newline_drain == PET_TRANSPORT_AUDIT_NOT_OBSERVED);
    assert(r->body_last_return == PET_TRANSPORT_AUDIT_NOT_OBSERVED && !r->body_accepted);
  } else {
    assert(r->driver_observed && r->body_accepted == strlen(diagnostic) && r->newline_accepted == 1);
    assert(r->body_drain == ESP_OK && r->newline_drain == ESP_OK && !r->invalid_return);
  }
}
static void disabled_and_no_drain(void) {
  pet_p4_transport_route_t route = PET_P4_TRANSPORT_UART;
  transport_send_line(diagnostic, &route);
  assert(fingerprint_calls == 0 && g_transport_audit.seen == 0 && uart_driver.drains == 2);
  reset_wire(); arm();
  unsigned before = fingerprint_calls;
  transport_send_line(audio_ack, NULL);
  transport_send_line("{\"topic\":\"media/status\",\"payload\":{}}", NULL);
  transport_send_audio_line(diagnostic, NULL);
  log_line("I metadata-free log\n");
  assert(!g_transport_audit.seen && fingerprint_calls == before);
  assert(!uart_driver.drains && !usb_driver.drains);
  assert(!pet_transport_audit_active(&g_transport_audit));
  arm(); /* Audio/media cancellation must not automatically resume. */
  transport_send_line("{\"topic\":\"debug/transport_audit_result\",\"payload\":{}}", &route);
  assert(!g_transport_audit.seen && fingerprint_calls == before);
  reset_wire();
  transport_send_line(diagnostic, NULL);
  pet_transport_audit_snapshot_t snapshot = close_capture();
  assert(snapshot.count == 3 && snapshot.seen == 3 && fingerprint_calls == before + 4);
  for (unsigned i = 0; i < 3; i++) check_success_record(&snapshot.records[i], i + 1);
  assert(snapshot.records[0].fingerprint == snapshot.records[1].fingerprint);
  assert(uart_driver.drains == 2 && usb_driver.drains == 2);
}
static void fault_capture(void) {
  for (unsigned r = 0; r < 2; r++) {
    pet_p4_transport_route_t route = r ? PET_P4_TRANSPORT_USB_SERIAL_JTAG : PET_P4_TRANSPORT_UART;
    driver_t *d = r ? &usb_driver : &uart_driver;
    for (int returned = -1; returned <= 0; returned++) {
      reset_wire(); arm(); d->fail_after = 23; d->fail_return = returned;
      transport_send_line(diagnostic, &route);
      pet_transport_audit_snapshot_t snapshot = close_capture();
      assert(snapshot.count == 1);
      const pet_transport_audit_record_t *record = &snapshot.records[0];
      assert(record->body_accepted == 23 && !record->newline_accepted && !record->invalid_return);
      assert(record->body_last_return == returned && record->newline_last_return == returned);
      assert(record->body_drain == ESP_OK && record->newline_drain == ESP_OK && d->drains == 2);
    }
    reset_wire(); arm(); d->fail_after = (long)strlen(diagnostic); d->fail_return = 0;
    transport_send_line(diagnostic, &route);
    pet_transport_audit_snapshot_t snapshot = close_capture();
    assert(snapshot.records[0].body_accepted == strlen(diagnostic) && !snapshot.records[0].newline_accepted);
    reset_wire(); arm(); d->drain_error = true;
    transport_send_line(diagnostic, &route); snapshot = close_capture();
    assert(snapshot.records[0].body_drain == -1 && snapshot.records[0].newline_drain == -1);
    assert(snapshot.records[0].body_accepted == strlen(diagnostic) && snapshot.records[0].newline_accepted == 1);
    reset_wire(); arm(); d->fail_after = 0; d->fail_return = INT32_MAX;
    transport_send_line(diagnostic, &route); snapshot = close_capture();
    assert(snapshot.records[0].invalid_return && !snapshot.records[0].body_accepted);
    assert(!snapshot.records[0].newline_accepted && snapshot.records[0].body_last_return == INT32_MAX);
    assert(d->writes == 2 && d->drains == 2); /* No extra retry or wait. */
  }
}
static void concurrent_capture(void) {
  reset_wire();
  successful_concurrency(); /* Retains all prior interleaving/audio assertions. */
  reset_wire(); arm(); unsigned before = fingerprint_calls;
  pthread_t producers[3];
  void *(*functions[])(void *) = {long_frames, screenshots, logs};
  for (unsigned i = 0; i < 3; i++) assert(!pthread_create(&producers[i], NULL, functions[i], NULL));
  for (unsigned i = 0; i < 3; i++) assert(!pthread_join(producers[i], NULL));
  flush_driver(&uart_driver); flush_driver(&usb_driver);
  pet_transport_audit_snapshot_t snapshot = close_capture();
  assert(snapshot.seen == 64 && snapshot.count == 16 && snapshot.overwritten == 48);
  assert(snapshot.records[0].sequence == 49 && snapshot.records[15].sequence == 64);
  assert(fingerprint_calls == before + 22 + 64); /* pre/message, post/retained route */
  for (unsigned i = 0; i < 16; i++) {
    const pet_transport_audit_record_t *r = &snapshot.records[i];
    assert(r->body_bytes == strlen(diagnostic) || r->body_bytes == strlen(hello));
    assert(r->fingerprint_available && !r->invalid_return);
  }
}
static char handler_reply[PET_TRANSPORT_AUDIT_REPLY_BYTES];
static unsigned handler_sends;
static void *expected_context;
static bool inject_during_reply;
static void handler_send(const char *line, void *ctx) {
  /* Would fail rather than hang if the handler encoded/sent under TX lock. */
  assert(!pthread_mutex_trylock(&transport_mutex));
  assert(!pthread_mutex_unlock(&transport_mutex));
  assert(ctx == expected_context && strlen(line) < sizeof(handler_reply));
  strcpy(handler_reply, line); handler_sends++;
  transport_send_line(line, ctx);
  if (inject_during_reply) transport_send_line(diagnostic, ctx);
}
static void self_reply_failure_is_not_delivery(void){
  reset_wire();pet_p4_transport_route_t route=PET_P4_TRANSPORT_UART;
  expected_context=&route;inject_during_reply=false;
  const char *read="{\"topic\":\"debug/transport_audit_read\",\"payload\":{}}";
  const char *last="{\"topic\":\"debug/transport_audit_reply_read\",\"payload\":{}}";
  uart_driver.fail_after=23;uart_driver.fail_return=0;
  assert(maybe_handle_transport_audit(read,handler_send,&route)&&g_transport_reply_observed);
  reset_wire();
  assert(maybe_handle_transport_audit(last,handler_send,&route));
  cJSON *root=cJSON_Parse(handler_reply);assert(root);
  const cJSON *records=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root,"payload"),"records");
  assert(cJSON_GetArraySize(records)==1);
  const cJSON *record=cJSON_GetArrayItem(records,0);
  assert(cJSON_GetObjectItemCaseSensitive(record,"bodyAccepted")->valueint==23);
  assert(cJSON_GetObjectItemCaseSensitive(record,"bodyBytes")->valueint>23);
  assert(cJSON_GetObjectItemCaseSensitive(record,"newlineAccepted")->valueint==0);
  cJSON_Delete(root);
  reset_wire();mutate_frame=g_transport_audit_reply;mutation_call=fingerprint_calls+2;
  assert(maybe_handle_transport_audit(read,handler_send,&route)&&!mutate_frame);
  assert(maybe_handle_transport_audit(last,handler_send,&route));
  root=cJSON_Parse(handler_reply);assert(root);
  records=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root,"payload"),"records");
  record=cJSON_GetArrayItem(records,0);
  assert(strcmp(cJSON_GetObjectItemCaseSensitive(record,"fnv")->valuestring,cJSON_GetObjectItemCaseSensitive(record,"fnvAfter")->valuestring));
  cJSON_Delete(root);
}
static void handler_contract(void) {
  reset_wire();
  pet_p4_transport_route_t route = PET_P4_TRANSPORT_UART;
  expected_context = &route;
  const char *start = "{\"topic\":\"debug/transport_audit_start\",\"payload\":{}}";
  const char *read = "{\"topic\":\"debug/transport_audit_read\",\"payload\":{}}";
  unsigned before = fingerprint_calls;
  inject_during_reply = true;
  assert(maybe_handle_transport_audit(start, handler_send, &route));
  assert(strstr(handler_reply, "transport_audit_started") && pet_transport_audit_active(&g_transport_audit));
  assert(!g_transport_audit.seen && fingerprint_calls == before);
  inject_during_reply = false;
  transport_send_line(diagnostic, &route);
  assert(g_transport_audit.seen == 1);
  const char *invalid[] = {
    "{\"topic\":\"debug/transport_audit_start\",\"payload\":{\"budget\":999}}",
    "{\"topic\":\"debug/transport_audit_read\",\"payload\":[]}",
    "{\"topic\":\"debug/transport_audit_start\"}",
    "{\"topic\":\"debug/transport_audit_read\",\"payload\":{},\"extra\":1}",
    "{\"topic\":\"debug/transport_audit_start\",\"payload\":{},\"payload\":{}}"
  };
  for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    assert(maybe_handle_transport_audit(invalid[i], handler_send, &route));
    assert(strstr(handler_reply, "invalid_request") && g_transport_audit.seen == 1);
    assert(pet_transport_audit_active(&g_transport_audit));
  }
  unsigned sends = handler_sends;
  assert(!maybe_handle_transport_audit("{\"topic\":\"other\",\"payload\":\"debug/transport_audit_start\"}", handler_send, &route));
  assert(!maybe_handle_transport_audit("{\"topic\":\"debug/transport_audit_start\",\"payload\":{}} junk", handler_send, &route));
  assert(handler_sends == sends && g_transport_audit.seen == 1);
  uint32_t captured_session = g_transport_audit.session;
  atomic_store(&audio_busy, true);
  assert(maybe_handle_transport_audit(start, handler_send, &route));
  assert(strstr(handler_reply, "audio_busy") && !pet_transport_audit_active(&g_transport_audit));
  assert(g_transport_audit.session == captured_session && g_transport_audit.seen == 1);
  assert(maybe_handle_transport_audit(read, handler_send, &route));
  assert(strstr(handler_reply, "audio_busy") && g_transport_audit.seen == 1);
  atomic_store(&audio_busy, false);
  inject_during_reply = true;
  assert(maybe_handle_transport_audit(read, handler_send, &route));
  assert(!pet_transport_audit_active(&g_transport_audit) && g_transport_audit.seen == 1);
  assert(strstr(handler_reply, "\"seen\":1") && fingerprint_calls == before + 4);
  cJSON *reply = cJSON_Parse(handler_reply); assert(reply); cJSON_Delete(reply);
  static char captured_reply[PET_TRANSPORT_AUDIT_REPLY_BYTES],metadata[PET_TRANSPORT_AUDIT_REPLY_BYTES];
  strcpy(captured_reply,handler_reply);unsigned observed_hashes=fingerprint_calls;
  assert(g_transport_reply_observed&&!pet_transport_audit_active(&g_transport_reply_audit));
  inject_during_reply=false;
  const char *reply_read="{\"topic\":\"debug/transport_audit_reply_read\",\"payload\":{}}";
  assert(maybe_handle_transport_audit(reply_read,handler_send,&route));
  strcpy(metadata,handler_reply);
  assert(strstr(metadata,"debug/transport_audit_reply_result")&&fingerprint_calls==observed_hashes);
  reply=cJSON_Parse(metadata);assert(reply);
  const cJSON *records=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(reply,"payload"),"records");
  assert(cJSON_GetArraySize(records)==1);
  const cJSON *record=cJSON_GetArrayItem(records,0);
  assert(cJSON_GetObjectItemCaseSensitive(record,"bodyBytes")->valueint==(int)strlen(captured_reply));
  assert(cJSON_GetObjectItemCaseSensitive(record,"bodyAccepted")->valueint==(int)strlen(captured_reply));
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(record,"fnv")->valuestring,cJSON_GetObjectItemCaseSensitive(record,"fnvAfter")->valuestring));
  cJSON_Delete(reply);
  assert(maybe_handle_transport_audit(reply_read,handler_send,&route));
  assert(!strcmp(metadata,handler_reply)&&fingerprint_calls==observed_hashes&&g_transport_audit.seen==1);
  puts(captured_reply);
}
static void source_mutation_is_distinct_from_driver_delivery(void){
  reset_wire();arm();pet_p4_transport_route_t route=PET_P4_TRANSPORT_UART;
  char changed[96]="{\"topic\":\"test/unchanged\",\"payload\":{}}";
  mutate_frame=changed;mutation_call=fingerprint_calls+2;
  transport_send_line(changed,&route);
  pet_transport_audit_snapshot_t snapshot=close_capture();
  assert(snapshot.count==1&&!mutate_frame);
  const pet_transport_audit_record_t *r=&snapshot.records[0];
  assert(r->driver_observed&&r->body_accepted==strlen(changed)&&r->newline_accepted==1);
  assert(r->body_drain==ESP_OK&&r->newline_drain==ESP_OK);
  assert(r->fingerprint_available&&r->fingerprint_after_available&&r->fingerprint!=r->fingerprint_after);
}
int main(void) {
  fill_json(diagnostic, sizeof(diagnostic), "diagnostics/status", 2048, 'D');
  fill_json(hello, sizeof(hello), "device/hello", 5120, 'H');
  memset(screenshot_body, 'A', 4096);
  int n = snprintf(screenshot_frame, sizeof(screenshot_frame), "%s%s\"}}", shot_header, screenshot_body);
  assert(n > 0 && (size_t)n < sizeof(screenshot_frame));
  start_driver(&uart_driver); start_driver(&usb_driver);
  disabled_and_no_drain(); fault_capture(); concurrent_capture(); source_mutation_is_distinct_from_driver_delivery();
  self_reply_failure_is_not_delivery();handler_contract();
  driver_t *drivers[] = {&uart_driver, &usb_driver};
  for (unsigned i = 0; i < 2; i++) {
    driver_t *d = drivers[i]; flush_driver(d);
    assert(!pthread_mutex_lock(&d->lock)); d->stop = true;
    assert(!pthread_cond_signal(&d->changed)); assert(!pthread_mutex_unlock(&d->lock));
    assert(!pthread_join(d->thread, NULL));
    assert(!pthread_cond_destroy(&d->changed)); assert(!pthread_mutex_destroy(&d->lock));
  }
  return 0;
}
