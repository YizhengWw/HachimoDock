"""Execute the actual state-lock callbacks, including builds without assertions."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("assertions", [True, False])
def test_storage_lock_callbacks_do_not_depend_on_assert_side_effects(tmp_path, assertions):
    main = (ROOT / "main/pet_p4_main.c").read_text()
    callback_source = "\n".join(function(main,n) for n in ("storage_command_owner","storage_command_unlock","storage_command_lock"))
    assertion = "assert(condition)" if assertions else "((void)0)"
    source = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "pet_p4_storage_io_trace.h"
#include "pet_p4_storage_lane.h"
#include "pet_p4_task_policy.h"
typedef int BaseType_t;
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffU
static int g_state_mutex = 1;
static uintptr_t task = 7, holder = 7;
static uintptr_t g_protocol_task = 7;
static bool g_protocol_storage_background;
static _Atomic bool g_storage_query_window;
static unsigned takes, gives, suspended, priority = PET_P4_PROTOCOL_TASK_PRIORITY, priority_changes;
#define configASSERT(condition) ASSERTION
static uintptr_t xTaskGetCurrentTaskHandle(void) { return task; }
static __attribute__((unused)) uintptr_t xSemaphoreGetMutexHolder(int lock) {
  assert(lock == 1); return holder;
}
static BaseType_t xSemaphoreGive(int lock) {
  assert(lock == 1 && holder == task && suspended && atomic_load(&g_storage_query_window)); holder = 0; gives++; return pdTRUE;
}
static BaseType_t xSemaphoreTake(int lock, unsigned wait) {
  assert(lock == 1 && wait == portMAX_DELAY && !holder && !atomic_load(&g_storage_query_window));
  assert(priority == PET_P4_PROTOCOL_TASK_PRIORITY && !g_protocol_storage_background);
  holder = task; takes++; return pdTRUE;
}
static void vTaskPrioritySet(uintptr_t owner, unsigned next) {
  assert(owner == task && task == g_protocol_task && !holder);
  assert(next == PET_P4_STORAGE_IO_TASK_PRIORITY || next == PET_P4_PROTOCOL_TASK_PRIORITY);
  assert(g_protocol_storage_background == (next == PET_P4_STORAGE_IO_TASK_PRIORITY));
  priority = next; priority_changes++;
}
static void pet_p4_renderer_set_asset_preparation_suspended(bool value) {
  assert(holder == task && value); suspended++;
}
'''.replace("ASSERTION", assertion) + callback_source + r'''
int main(void) {
  pet_storage_lane_t lane = {0}; pet_storage_scope_t scope = {0};
  assert(pet_storage_lane_configure(&lane, storage_command_unlock,
    storage_command_lock, storage_command_owner, 0));
  /* Registration alone must not release a lock on boot/native fallback. */
  assert(pet_storage_scope_begin(&lane, &scope));
  assert(pet_storage_scope_unlock(&scope)); assert(holder == task && !gives);
  assert(priority_changes == 0 && priority == PET_P4_PROTOCOL_TASK_PRIORITY);
  assert(pet_storage_scope_finish(&scope));
  assert(pet_storage_lane_enter(&lane));
  assert(pet_storage_scope_begin(&lane, &scope));
  assert(pet_storage_scope_unlock(&scope)); assert(!holder && gives == 1 && suspended == 1);
  assert(priority == PET_P4_STORAGE_IO_TASK_PRIORITY && priority_changes == 1);
  /* Failure exit reacquires before returning to handle_line's caller. */
  assert(pet_storage_scope_finish(&scope)); assert(holder == task && takes == 1);
  assert(priority == PET_P4_PROTOCOL_TASK_PRIORITY && priority_changes == 2);
  assert(pet_storage_lane_leave(&lane));
  /* A non-protocol native fallback never alters any task's scheduling base. */
  task = holder = 8;
  assert(pet_storage_lane_enter(&lane));
  assert(pet_storage_scope_begin(&lane, &scope));
  assert(pet_storage_scope_unlock(&scope));
  assert(priority_changes == 2 && !g_protocol_storage_background);
  assert(pet_storage_scope_finish(&scope));
  assert(pet_storage_lane_leave(&lane));
  return 0;
}
'''
    path, exe = tmp_path / "integration.c", tmp_path / "integration"
    path.write_text(source)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                    str(path), str(ROOT / "main/pet_p4_storage_lane.c"),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def test_command_scope_and_suspension_integration_leave_render_cadence_unchanged():
    main = (ROOT / "main/pet_p4_main.c").read_text()
    worker = main.split("static void protocol_worker_task(", 1)[1].split("static bool consume_byte(", 1)[0]
    assert worker.index("xSemaphoreTake(g_state_mutex") < worker.index("pet_p4_miniapp_storage_lane_enter()")
    assert worker.index("pet_p4_miniapp_storage_lane_enter()") < worker.index("pet_p4_handle_line(")
    assert worker.index("pet_p4_handle_line(") < worker.index("pet_p4_miniapp_storage_lane_leave()")
    assert worker.index("pet_p4_miniapp_storage_lane_leave()") < worker.index("xSemaphoreGive(g_state_mutex)")
    loop = main[main.index('    pet_p4_asset_transfer_process(&g_state, now_ms);'):]
    compact = ''.join(loop.split())
    suspension = ('pet_p4_renderer_set_asset_preparation_suspended('
                  'transfer_active||pet_p4_miniapp_storage_busy()||pet_p4_appearance_active());')
    assert compact.index('booltransfer_active=g_state.asset_transfer_active||firmware_transfer_active;') < compact.index(suspension) < compact.index('xSemaphoreGive(g_state_mutex);')
    # All three causes stop new preparation, but only the original transfer
    # flag selects the 500ms UI. Actual borrow/relock assertions run above
    # with configASSERT both enabled/disabled; activation interleavings also
    # run in p4_phase2_transaction_test (no device timing claim).
    assert 'else if (lcd_err == ESP_OK && !transfer_active' in loop
    # Storage need not stop rendering or reduce it to the transfer UI's 500ms.
    assert "bool transfer_active = g_state.asset_transfer_active || firmware_transfer_active;" in main
    assert "&g_protocol_task" in main
