#ifndef BOARD_RUNTIME_PAIRING_H
#define BOARD_RUNTIME_PAIRING_H

#include <stdbool.h>

typedef enum {
  BR_PAIRING_BOOT = 0,
  BR_PAIRING_WAITING_CONFIG = 1,
  BR_PAIRING_LAN_DISCOVERY = 2,
  BR_PAIRING_AP_FALLBACK = 3,
  BR_PAIRING_STA_READY = 4
} br_pairing_state;

typedef struct {
  br_pairing_state state;
  long long entered_ms;
  long long last_discovery_ms;
  int discovery_timeout_ms;
  bool network_config_valid;
  bool discovered_once;
} br_pairing_machine;

void br_pairing_init(
  br_pairing_machine *machine,
  bool has_valid_network_config,
  int discovery_timeout_ms,
  long long now_ms
);

bool br_pairing_mark_discovered(br_pairing_machine *machine, long long now_ms);
bool br_pairing_start_discovery(br_pairing_machine *machine, long long now_ms);
bool br_pairing_apply_config(br_pairing_machine *machine, long long now_ms);
bool br_pairing_reset_to_waiting(br_pairing_machine *machine, long long now_ms);
bool br_pairing_tick(br_pairing_machine *machine, long long now_ms);
const char *br_pairing_state_name(br_pairing_state state);
const char *br_pairing_mode_name(br_pairing_state state);
bool br_pairing_is_waiting(br_pairing_state state);

#endif
