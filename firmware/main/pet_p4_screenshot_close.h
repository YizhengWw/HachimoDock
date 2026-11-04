/* Optional native diagnostic transaction boundary, not an execution grant.
 * Legacy {id} cleanup is unchanged; acknowledgement requires explicit opt-in.
 * This does not add retries or extend snapshot, driver or host deadlines. */
#pragma once
#ifndef PET_P4_SCREENSHOT_CLOSE_ACK
#define PET_P4_SCREENSHOT_CLOSE_ACK 0
#endif
#define PET_SCREENSHOT_CLOSE_ACK_ENGINE "p4-snapshot-close-v1"
#if PET_P4_SCREENSHOT_CLOSE_ACK
#include "pet_p4_control_integrity.h"
#endif
