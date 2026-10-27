"""Execute production pointer/gesture and swept slicing logic on the host."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def test_touch_and_slice_behavior(tmp_path):
    binary = tmp_path / 'touch-slice'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(ROOT/'main'),
                    str(ROOT/'main/pet_p4_touch_core.c'),
                    str(ROOT/'main/pet_p4_slice_core.c'),
                    str(ROOT/'tests/p4_touch_slice_test.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

def test_touch_controller_identity_gate(tmp_path):
    source = (ROOT/'main/pet_p4_touch.c').read_text()
    identify = source.split('static esp_err_t identify_register_touch(void) {', 1)[1]
    identify = 'static esp_err_t identify_register_touch(void) {' + identify.split('\nesp_err_t pet_p4_touch_init', 1)[0]
    harness = r'''
#include <assert.h>
#include <string.h>
#include "pet_p4_touch_core.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NOT_SUPPORTED 1
#define ESP_ERR_INVALID_RESPONSE 2
#define ESP_LOGW(...) ((void)0)
static bool g_cst826, g_ft6336;
static uint8_t g_chip_id, g_vendor_id;
static void *g_touch_io;
static uint8_t cst_id, ft_id, count;
static int fail_register = -1;
static int esp_lcd_panel_io_rx_param(void *io, int reg, void *out, size_t n) {
  (void)io;
  if (reg == fail_register) return 3;
  memset(out, 0, n);
  *(uint8_t *)out = reg == 0xaa ? cst_id : reg == 0xa3 ? ft_id : reg == 2 ? count : 0;
  return ESP_OK;
}
'''
    harness += identify + r'''
static void reset(void) {g_cst826=g_ft6336=false; cst_id=ft_id=count=0; fail_register=-1;}
int main(void) {
  reset(); assert(identify_register_touch()==ESP_ERR_NOT_SUPPORTED);
  assert(!g_cst826 && !g_ft6336);
  reset(); cst_id=0x11; assert(identify_register_touch()==ESP_OK);
  assert(g_cst826 && !g_ft6336 && g_chip_id==0x11);
  reset(); ft_id=0x64; assert(identify_register_touch()==ESP_OK);
  assert(g_ft6336 && !g_cst826 && g_chip_id==0x64);
  reset(); cst_id=0x11; fail_register=0xaa;
  assert(identify_register_touch()!=ESP_OK && !g_cst826);
  reset(); cst_id=0x11; count=3;
  assert(identify_register_touch()==ESP_ERR_INVALID_RESPONSE && !g_cst826);
  reset(); cst_id=0x11; fail_register=2;
  assert(identify_register_touch()!=ESP_OK && !g_cst826);
}
'''
    path = tmp_path/'identity.c'
    path.write_text(harness)
    binary = tmp_path/'identity'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(ROOT/'main'),
                    str(ROOT/'main/pet_p4_touch_core.c'), str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
