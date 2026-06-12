#include "runtime_pairing.h"

#include <string.h>

static bool br_pairing_transition(br_pairing_machine *machine, br_pairing_state next_state, long long now_ms) {
  if (!machine || machine->state == next_state) {
    return false;
  }
  machine->state = next_state;
  machine->entered_ms = now_ms;
  return true;
}

void br_pairing_init(
  br_pairing_machine *machine,
  bool has_valid_network_config,
  int discovery_timeout_ms,
  long long now_ms
) {
  if (!machine) {
    return;
  }
  memset(machine, 0, sizeof(*machine));
  // Non-positive timeout means "never auto-fallback to AP" - the pairing
  // machine stays in LAN discovery until an explicit transition is
  // requested (e.g. via /pairing/ap-mode).
  machine->discovery_timeout_ms = discovery_timeout_ms;
  machine->network_config_valid = has_valid_network_config;
  machine->entered_ms = now_ms;
  machine->last_discovery_ms = now_ms;
  machine->state = has_valid_network_config ? BR_PAIRING_STA_READY : BR_PAIRING_WAITING_CONFIG;
}

bool br_pairing_mark_discovered(br_pairing_machine *machine, long long now_ms) {
  if (!machine) {
    return false;
  }
  machine->last_discovery_ms = now_ms;
  machine->discovered_once = true;
  if (machine->state == BR_PAIRING_WAITING_CONFIG) {
    return br_pairing_transition(machine, BR_PAIRING_LAN_DISCOVERY, now_ms);
  }
  return false;
}

bool br_pairing_start_discovery(br_pairing_machine *machine, long long now_ms) {
  if (!machine) {
    return false;
  }
  machine->last_discovery_ms = now_ms;
  if (machine->state == BR_PAIRING_WAITING_CONFIG) {
    return br_pairing_transition(machine, BR_PAIRING_LAN_DISCOVERY, now_ms);
  }
  return false;
}

bool br_pairing_apply_config(br_pairing_machine *machine, long long now_ms) {
  if (!machine) {
    return false;
  }
  machine->network_config_valid = true;
  return br_pairing_transition(machine, BR_PAIRING_STA_READY, now_ms);
}

bool br_pairing_reset_to_waiting(br_pairing_machine *machine, long long now_ms) {
  if (!machine) {
    return false;
  }
  machine->network_config_valid = false;
  machine->discovered_once = false;
  machine->last_discovery_ms = now_ms;
  return br_pairing_transition(machine, BR_PAIRING_WAITING_CONFIG, now_ms);
}

bool br_pairing_tick(br_pairing_machine *machine, long long now_ms) {
  if (!machine) {
    return false;
  }
  if (machine->state == BR_PAIRING_WAITING_CONFIG) {
    if (!machine->network_config_valid) {
      // No network config at all — go straight to AP mode, no point waiting.
      return br_pairing_transition(machine, BR_PAIRING_AP_FALLBACK, now_ms);
    }
    return br_pairing_start_discovery(machine, now_ms);
  }
  if (machine->state == BR_PAIRING_LAN_DISCOVERY) {
    if (machine->discovery_timeout_ms <= 0) {
      return false;
    }
    if (now_ms - machine->entered_ms >= machine->discovery_timeout_ms) {
      return br_pairing_transition(machine, BR_PAIRING_AP_FALLBACK, now_ms);
    }
  }
  return false;
}

const char *br_pairing_state_name(br_pairing_state state) {
  switch (state) {
    case BR_PAIRING_BOOT: return "boot";
    case BR_PAIRING_WAITING_CONFIG: return "waiting_config";
    case BR_PAIRING_LAN_DISCOVERY: return "lan_discovery";
    case BR_PAIRING_AP_FALLBACK: return "ap_fallback";
    case BR_PAIRING_STA_READY: return "sta_ready";
  }
  return "unknown";
}

const char *br_pairing_mode_name(br_pairing_state state) {
  switch (state) {
    case BR_PAIRING_AP_FALLBACK: return "ap";
    case BR_PAIRING_STA_READY: return "sta";
    default: return "pairing";
  }
}

bool br_pairing_is_waiting(br_pairing_state state) {
  return state == BR_PAIRING_WAITING_CONFIG ||
         state == BR_PAIRING_LAN_DISCOVERY ||
         state == BR_PAIRING_AP_FALLBACK;
}
