/* Host regression: recording feedback is independent of Session availability. */
#include <assert.h>
#include <string.h>
#include "pet_p4_view.h"

static pet_p4_runtime_state_t state;

int main(void) {
  pet_p4_view_model_t view;
  strcpy(state.screen_page, "main");
  strcpy(state.current_state, "idle");
  strcpy(state.current_speech, "休息中");
  pet_p4_build_view_model(&state, &view);
  assert(view.show_bubble && !view.show_voice_overlay);

  state.session_voice_active = true;
  pet_p4_build_view_model(&state, &view);
  assert(view.show_voice_overlay && !view.show_bubble);
  assert(state.session_queue_count == 0);

  // Refresh timestamps and stale lifecycle metadata cannot hide recording.
  state.session_snapshot_last_seen_ms = 1234;
  strcpy(state.current_state, "working");
  pet_p4_build_view_model(&state, &view);
  assert(view.show_voice_overlay && !view.show_bubble);

  // A real Session appearing mid-hold uses its existing card, not two overlays.
  state.session_queue_count = 1;
  pet_p4_build_view_model(&state, &view);
  assert(!view.show_voice_overlay && state.session_voice_active);
  state.session_queue_count = 0;
  pet_p4_build_view_model(&state, &view);
  assert(view.show_voice_overlay);

  // Existing component/catalog UI is not covered by the main-page fallback.
  strcpy(state.screen_page, "components");
  pet_p4_build_view_model(&state, &view);
  assert(!view.show_voice_overlay);
  strcpy(state.screen_page, "app");
  pet_p4_build_view_model(&state, &view);
  assert(!view.show_voice_overlay);

  // Default main page and release both work before any session snapshot.
  state.screen_page[0] = '\0';
  pet_p4_build_view_model(&state, &view);
  assert(view.show_voice_overlay);
  state.session_voice_active = false;
  pet_p4_build_view_model(&state, &view);
  assert(!view.show_voice_overlay && view.show_bubble && view.compact_bubble);
  assert(strcmp(view.body, "休息中") == 0);
  pet_p4_build_view_model(NULL, &view);
  assert(!view.show_voice_overlay);
  return 0;
}
