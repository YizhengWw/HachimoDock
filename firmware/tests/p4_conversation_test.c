#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pet_p4_conversation.h"
#include "pet_p4_view.h"

static pet_p4_runtime_state_t state;

static void status_hints_have_no_trailing_punctuation(void) {
  pet_p4_runtime_state_t hints = {0};
  pet_p4_view_model_t view;
  const char *phases[] = {"preparing", "listening", "thinking", "speaking"};
  const char *labels[] = {"准备中", "我在听", "想一想", "准备回答"};
  for (unsigned i = 0; i < sizeof(phases) / sizeof(phases[0]); i++) {
    assert(pet_p4_conversation_update(&hints, "hint-test", phases[i], "小西", "", false, false, i + 1));
    pet_p4_build_view_model(&hints, &view);
    assert(!strcmp(view.body, labels[i]));
    assert(!strchr(view.body, '?') && !strstr(view.body, "？") && !strstr(view.body, "…"));
  }
  /* Real questions are captions, not status labels: preserve their punctuation. */
  assert(pet_p4_conversation_update(&hints, "hint-test", "listening", "小西", "几点了？", true, true, 10));
  pet_p4_build_view_model(&hints, &view);
  assert(!strcmp(view.body, "几点了？"));
  assert(pet_p4_conversation_update(&hints, "hint-test", "speaking", "小西", "还想问什么？", false, true, 11));
  pet_p4_build_view_model(&hints, &view);
  assert(!strcmp(view.body, "还想问什么？"));
}

int main(void) {
  status_hints_have_no_trailing_punctuation();
  pet_p4_view_model_t view;
  strcpy(state.current_state, "working");
  strcpy(state.current_speech, "Agent latest");
  strcpy(state.active_agent, "test-agent");
  strcpy(state.screen_page, "app");
  state.session_queue_count = 1;
  assert(pet_p4_conversation_update(&state, "rtc-1", "preparing", "小西", "", false, false, 1));
  assert(!pet_p4_conversation_update(&state, "old", "ended", "", "", false, false, 2));
  assert(pet_p4_conversation_update(&state, "rtc-1", "listening", "小西", "用户的话", true, true, 3));
  pet_p4_build_view_model(&state, &view);
  assert(view.realtime_conversation && !view.compact_bubble && !view.show_voice_overlay);
  assert(!strcmp(view.page, "main") && !view.agent[0] && !view.stats_json[0]);
  assert(!strcmp(view.title, "你") && !strcmp(view.body, "用户的话"));

  assert(pet_p4_conversation_update(&state, "rtc-1", "speaking", "小西", "", false, false, 4));
  pet_p4_conversation_playback_begin(&state, "rtc-1-turn");
  assert(!pet_p4_conversation_queue_cue(&state, "old-turn", 0, "wrong"));
  assert(!pet_p4_conversation_queue_cue(&state, "rtc-1-turn", 1, "unaligned"));
  assert(pet_p4_conversation_queue_cue(&state, "rtc-1-turn", 0, "宠物第一句"));
  assert(pet_p4_conversation_queue_cue(&state, "rtc-1-turn", 32000, "宠物第二句"));
  pet_p4_conversation_advance(&state, 0, false, 5);
  assert(!state.conversation_text[0]); // prebuffering must not start subtitles
  pet_p4_conversation_advance(&state, 0, true, 6);
  pet_p4_build_view_model(&state, &view);
  assert(!strcmp(view.body, "宠物第一句") && !strcmp(view.title, "小西"));
  assert(!strstr(view.body, "用户的话")); // No retained user summary during replies
  pet_p4_conversation_advance(&state, 31998, true, 10000);
  assert(!strcmp(state.conversation_text, "宠物第一句")); // audio underrun, not wall time
  pet_p4_conversation_advance(&state, 32000, false, 10001);
  assert(!strcmp(state.conversation_text, "宠物第二句")); // short final clip may already finish
  pet_p4_conversation_move(&state, -1, 10002);
  pet_p4_build_view_model(&state, &view);
  assert(view.conversation_history && !strcmp(view.body, "宠物第一句"));
  pet_p4_conversation_move(&state, 1, 10003);
  pet_p4_conversation_move(&state, 1, 10004);
  assert(state.conversation_history_cursor == -1);

  for (int i = 0; i < 20; i++) {
    char text[32]; snprintf(text, sizeof(text), "caption-%d", i);
    assert(pet_p4_conversation_update(&state, "rtc-1", "listening", "小西", text, true, true, 20000 + i));
  }
  assert(state.conversation_history_count == PET_P4_CONVERSATION_HISTORY_MAX);
  assert(!strcmp(state.conversation_history[0].text, "caption-12"));
  char utf8[901];
  for (int i = 0; i < 300; i++) memcpy(utf8 + i * 3, "蛙", 3);
  utf8[900] = 0;
  assert(pet_p4_conversation_update(&state, "rtc-1", "listening", "小西", utf8, true, false, 30000));
  assert(strlen(state.conversation_text) == 510);
  pet_p4_conversation_playback_begin(&state, "rtc-1-turn2");
  for (int i = 0; i < PET_P4_CONVERSATION_CUES_MAX; i++)
    assert(pet_p4_conversation_queue_cue(&state, "rtc-1-turn2", i * 2, "cue"));
  assert(!pet_p4_conversation_queue_cue(&state, "rtc-1-turn2", 100, "overflow"));
  assert(pet_p4_conversation_update(&state, "rtc-1", "error", "小西", "连接失败", false, false, 40000));
  assert(!state.conversation_cue_count && state.conversation_error_until_ms == 48000);
  pet_p4_conversation_advance(&state, 100000, true, 40001);
  assert(!strcmp(state.conversation_text, "连接失败"));
  assert(pet_p4_conversation_update(&state, "rtc-1", "ended", "", "", false, false, 50000));
  assert(!pet_p4_conversation_active(&state) && !state.conversation_history_count);
  assert(!state.conversation_history[0].text[0] && !state.conversation_cues[0].text[0]);
  assert(!pet_p4_conversation_update(&state, "rtc-1", "speaking", "小西", "late", false, false, 50001));
  assert(!strcmp(state.current_speech, "Agent latest") && state.session_queue_count == 1);
  pet_p4_build_view_model(&state, &view);
  assert(!view.realtime_conversation && !strcmp(view.agent, "test-agent"));
  return 0;
}
