/* StopWatch v1.0 BSP using the same ESP-IDF esp_lcd/I2C interfaces as P4.
 * Register/power sequencing references: m5stack/M5StopWatch-UserDemo,
 * M5IOE1 and M5PM1 (MIT; see THIRD_PARTY_NOTICES.md). No Arduino/M5GFX runtime.
 */
#include "board_stopwatch.h"
#include <string.h>
#include <stdatomic.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "stopwatch-board";
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t pm1, ioe, touch;
static esp_lcd_panel_handle_t panel;
static esp_lcd_panel_io_handle_t io;
static SemaphoreHandle_t dma_done;
static uint8_t *stripe;
static bool display_failed;
static uint32_t display_frames;
static int display_error;
static atomic_bool motor_on;
static atomic_int motor_error;
static uint8_t ioe_address, touch_chip, touch_firmware;
enum { STRIPE_LINES = 32 };

static esp_err_t read_reg(i2c_master_dev_handle_t dev, uint8_t reg, void *out, size_t n) {
  esp_err_t err = ESP_FAIL;
  for (unsigned i = 0; i < 3; ++i) {
    err = i2c_master_transmit_receive(dev, &reg, 1, out, n, 100);
    if (err == ESP_OK) break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return err;
}
static esp_err_t write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value) {
  uint8_t data[] = {reg, value};
  esp_err_t err = ESP_FAIL;
  for (unsigned i = 0; i < 3; ++i) {
    err = i2c_master_transmit(dev, data, sizeof(data), 100);
    if (err == ESP_OK) break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return err;
}
static esp_err_t update_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t old;
  esp_err_t err = read_reg(dev, reg, &old, 1);
  return err == ESP_OK ? write_reg(dev, reg, (old & ~mask) | (value & mask)) : err;
}
static esp_err_t add_device(uint8_t addr, i2c_master_dev_handle_t *dev) {
  i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address = addr, .scl_speed_hz = 100000};
  return i2c_master_bus_add_device(bus, &cfg, dev);
}
static esp_err_t ioe_output(unsigned pin, bool value) {
  unsigned bit = pin - 1;
  uint8_t mask = 1u << (bit % 8);
  ESP_RETURN_ON_ERROR(update_reg(ioe, 0x09 + bit / 8, mask, 0), TAG, "IOE pullup");
  ESP_RETURN_ON_ERROR(update_reg(ioe, 0x0b + bit / 8, mask, 0), TAG, "IOE pulldown");
  ESP_RETURN_ON_ERROR(update_reg(ioe, 0x13 + bit / 8, mask, 0), TAG, "IOE push-pull");
  ESP_RETURN_ON_ERROR(update_reg(ioe, 0x03 + bit / 8, mask, mask), TAG, "IOE direction");
  return update_reg(ioe, 0x05 + bit / 8, mask, value ? mask : 0);
}
static const co5300_lcd_init_cmd_t panel_init[] = {
  {0x11, NULL, 0, 150}, {0xc4, (uint8_t[]){0x80}, 1, 0},
  {0x35, (uint8_t[]){0x80}, 1, 0}, {0x44, (uint8_t[]){0x01,0xd2}, 2, 0},
  {0x53, (uint8_t[]){0x20}, 1, 0}, {0x20, NULL, 0, 0},
  {0x36, (uint8_t[]){0}, 1, 0}, {0x3a, (uint8_t[]){0x55}, 1, 0},
  {0x51, (uint8_t[]){128}, 1, 0}, {0x29, NULL, 0, 20}
};
static bool color_done(esp_lcd_panel_io_handle_t handle, esp_lcd_panel_io_event_data_t *event, void *ctx) {
  (void)handle; (void)event;
  BaseType_t wake = pdFALSE;
  xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &wake);
  return wake == pdTRUE;
}

esp_err_t stopwatch_board_init(void) {
  i2c_master_bus_config_t cfg = {.i2c_port = I2C_NUM_0, .sda_io_num = 47,
    .scl_io_num = 48, .clk_source = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true};
  ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &bus), TAG, "I2C");
  ESP_RETURN_ON_ERROR(add_device(0x6e, &pm1), TAG, "PM1");
  ESP_RETURN_ON_ERROR(write_reg(pm1, 0x09, 0), TAG, "PM1 wake");
  ESP_RETURN_ON_ERROR(write_reg(pm1, 0x0a, 0), TAG, "PM1 watchdog");
  // Preserve charging/button/download configuration; only enable peripheral rails.
  ESP_RETURN_ON_ERROR(update_reg(pm1, 0x06, 0x06, 0x06), TAG, "power rails");
  uint8_t ioe_addr = 0;
  for (unsigned attempt = 0; attempt < 3 && !ioe_addr; ++attempt) {
    if (i2c_master_probe(bus, 0x4f, 100) == ESP_OK) ioe_addr = 0x4f;
    else if (i2c_master_probe(bus, 0x6f, 100) == ESP_OK) ioe_addr = 0x6f;
    if (!ioe_addr) vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (!ioe_addr) return ESP_ERR_NOT_FOUND;
  ioe_address = ioe_addr;
  ESP_RETURN_ON_ERROR(add_device(ioe_addr, &ioe), TAG, "IOE");
  ESP_RETURN_ON_ERROR(write_reg(ioe, 0x23, 0), TAG, "IOE wake");
  // IO9/PWM1 may have been left enabled by another application.
  ESP_RETURN_ON_ERROR(update_reg(ioe, 0x1c, 0x80, 0), TAG, "motor PWM off");
  ESP_RETURN_ON_ERROR(ioe_output(10, false), TAG, "speaker off");
  ESP_RETURN_ON_ERROR(ioe_output(3, false), TAG, "audio off");
  ESP_RETURN_ON_ERROR(ioe_output(9, false), TAG, "motor off");
  ESP_RETURN_ON_ERROR(ioe_output(8, true), TAG, "OLED rail");
  ESP_RETURN_ON_ERROR(ioe_output(4, false), TAG, "touch reset");
  ESP_RETURN_ON_ERROR(ioe_output(5, false), TAG, "OLED reset");
  vTaskDelay(pdMS_TO_TICKS(20));
  ESP_RETURN_ON_ERROR(ioe_output(4, true), TAG, "touch release");
  ESP_RETURN_ON_ERROR(ioe_output(5, true), TAG, "OLED release");
  vTaskDelay(pdMS_TO_TICKS(150));
  gpio_config_t keys = {.pin_bit_mask = (1ULL << 1) | (1ULL << 2),
    .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
  ESP_RETURN_ON_ERROR(gpio_config(&keys), TAG, "keys");
  dma_done = xSemaphoreCreateBinary();
  stripe = heap_caps_malloc(STOPWATCH_WIDTH * STRIPE_LINES * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!dma_done || !stripe) return ESP_ERR_NO_MEM;
  spi_bus_config_t spi = CO5300_PANEL_BUS_QSPI_CONFIG(40, 41, 42, 46, 45,
    STOPWATCH_WIDTH * STRIPE_LINES * 2);
  ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &spi, SPI_DMA_CH_AUTO), TAG, "QSPI");
  esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(39, color_done, dma_done);
  // Start conservatively. Raise only after v1.0 signal-integrity/HIL tests.
  io_cfg.pclk_hz = 40000000;
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io), TAG, "panel IO");
  co5300_vendor_config_t vendor = {.init_cmds = panel_init,
    .init_cmds_size = sizeof(panel_init) / sizeof(panel_init[0]), .flags.use_qspi_interface = 1};
  esp_lcd_panel_dev_config_t panel_cfg = {.reset_gpio_num = -1,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16,
    .vendor_config = &vendor};
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(io, &panel_cfg, &panel), TAG, "CO5300");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(panel, 6, 0), TAG, "panel gap");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "panel on");
  // Reset CST820B after the long panel initialization, as the official HAL does.
  // Register reads wake/identify it; a single address probe can miss standby.
  ESP_RETURN_ON_ERROR(add_device(0x15, &touch), TAG, "touch handle");
  bool identified = false;
  for (unsigned attempt = 0; attempt < 3 && !identified; ++attempt) {
    ESP_RETURN_ON_ERROR(ioe_output(4, false), TAG, "touch reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(ioe_output(4, true), TAG, "touch release");
    vTaskDelay(pdMS_TO_TICKS(50));
    identified = read_reg(touch, 0xa7, &touch_chip, 1) == ESP_OK &&
      read_reg(touch, 0xa9, &touch_firmware, 1) == ESP_OK &&
      touch_chip != 0 && touch_chip != 0xff;
  }
  if (!identified) {
    ESP_ERROR_CHECK(i2c_master_bus_rm_device(touch));
    touch = NULL;
  }
  return ESP_OK;
}

esp_err_t stopwatch_board_present(const uint16_t *pixels) {
  if (!panel || !pixels || display_failed) return ESP_ERR_INVALID_STATE;
  for (int y = 0; y < STOPWATCH_HEIGHT; y += STRIPE_LINES) {
    int lines = STOPWATCH_HEIGHT - y;
    if (lines > STRIPE_LINES) lines = STRIPE_LINES;
    for (unsigned i = 0; i < STOPWATCH_WIDTH * lines; ++i) {
      uint16_t p = pixels[y * STOPWATCH_WIDTH + i];
      stripe[2 * i] = p >> 8;
      stripe[2 * i + 1] = p;
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, y, STOPWATCH_WIDTH, y + lines, stripe);
    if (err != ESP_OK) { display_failed = true; display_error = err; return err; }
    if (xSemaphoreTake(dma_done, pdMS_TO_TICKS(500)) != pdTRUE) {
      // Never overwrite a DMA buffer whose ownership is now uncertain.
      display_failed = true;
      display_error = ESP_ERR_TIMEOUT;
      return ESP_ERR_TIMEOUT;
    }
  }
  ++display_frames;
  return ESP_OK;
}
esp_err_t stopwatch_board_brightness(uint8_t value) {
  return esp_lcd_panel_io_tx_param(io, (0x02 << 24) | (0x51 << 8), &value, 1);
}
bool stopwatch_board_button(unsigned index) { return gpio_get_level(index ? 1 : 2) == 0; }
bool stopwatch_board_touch(int *x, int *y) {
  uint8_t data[7];
  if (!touch || read_reg(touch, 0, data, sizeof(data)) != ESP_OK ||
      !(data[2] & 0x0f) || ((data[3] >> 6) & 3) == 1) return false;
  *x = ((data[3] & 15) << 8) | data[4];
  *y = ((data[5] & 15) << 8) | data[6];
  return *x < STOPWATCH_WIDTH && *y < STOPWATCH_HEIGHT;
}
bool stopwatch_board_touch_ready(void) { return touch != NULL; }
esp_err_t stopwatch_board_vibrate(bool enabled) {
  esp_err_t err = ioe_output(9, enabled);
  atomic_store(&motor_error, err);
  if (err == ESP_OK) atomic_store(&motor_on, enabled);
  return err;
}
int stopwatch_board_battery_mv(void) {
  uint8_t data[2];
  return read_reg(pm1, 0x22, data, 2) == ESP_OK ? data[0] | ((data[1] & 15) << 8) : -1;
}
esp_err_t stopwatch_board_health(stopwatch_board_health_t *h) {
  if (!h) return ESP_ERR_INVALID_ARG;
  memset(h, 0, sizeof(*h));
  h->display_frames = display_frames; h->display_error = display_error;
  h->motor_on = atomic_load(&motor_on); h->motor_error = atomic_load(&motor_error);
  h->ioe_address = ioe_address; h->touch_chip = touch_chip;
  h->touch_firmware = touch_firmware; h->touch_ready = touch != NULL;
  ESP_RETURN_ON_ERROR(read_reg(pm1, 0x06, &h->power_config, 1), TAG, "power snapshot");
  uint8_t bytes[2];
  ESP_RETURN_ON_ERROR(read_reg(ioe, 0x03, bytes, 2), TAG, "mode snapshot");
  h->ioe_mode = bytes[0] | (bytes[1] << 8);
  ESP_RETURN_ON_ERROR(read_reg(ioe, 0x05, bytes, 2), TAG, "output snapshot");
  h->ioe_output = bytes[0] | (bytes[1] << 8);
  ESP_RETURN_ON_ERROR(read_reg(ioe, 0x07, bytes, 2), TAG, "input snapshot");
  h->ioe_input = bytes[0] | (bytes[1] << 8);
  ESP_RETURN_ON_ERROR(read_reg(ioe, 0x13, bytes, 2), TAG, "drive snapshot");
  h->ioe_drive = bytes[0] | (bytes[1] << 8);
  return ESP_OK;
}
