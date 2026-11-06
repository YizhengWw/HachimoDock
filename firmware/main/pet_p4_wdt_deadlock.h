/* [Input] task watchdog as deadlock detector for bounded-loop P4 tasks.
 * [Output] panic + reboot when a tracked task cannot feed within the deadline.
 * [Pos] ESP32-P4 main firmware component; wedge self-recovery via A/B boot.
 * [Sync] If this file changes, update `firmware/.folder.md`.
 *
 * A task that blocks indefinitely (e.g. waiting on the state mutex whose
 * holder is stuck in I/O) cannot feed the watchdog. The Task WDT then fires:
 * the panic records the stuck task name and backtrace, and the A/B partition
 * reboots into the last valid image, so a silent wedge becomes a visible,
 * self-healing fault reset instead of a frozen board.
 */
#ifndef PET_P4_WDT_DEADLOCK_H
#define PET_P4_WDT_DEADLOCK_H
#include <stdbool.h>

bool pet_p4_wdt_deadlock_init(int feed_timeout_sec);
bool pet_p4_wdt_deadlock_track(void);
void pet_p4_wdt_deadlock_feed(void);

#endif
