# ESP32-P4 WLK2802 Display BSP

This local Waveshare BSP adaptation drives the 480x640 WLK2802/ST7701S panel
over one 800Mbps MIPI DSI data lane. The firmware renders a 640x480 logical
framebuffer and rotates it onto the portrait panel.

DSI PHY clock selection is left to the SDK (`phy_clk_src = 0`) so each
chip family uses its supported PLL reference. The old explicit PHY default
is invalid on P4 v3. Lane rate, rotation, RGB565 format and resolution are unchanged.

P4 v3 uses an exact 24MHz DPI clock (240MHz / 10). Requesting 25MHz with
IDF 5.5.4 produces 24MHz and a horizontal timing correction of -21 pixels,
which exceeds this panel's 10-pixel front porch and prevents normal scanout.
The v1 build retains its existing 25MHz request and SDK timing behavior.

The panel command return path is treated as write-only. Startup skips the
optional ST7701S ID read and disables command ACK requests before sending the
vendor initialization table. Waiting for either response can otherwise fill
the ESP32-P4 command FIFO and block startup before the backlight is enabled.

## Backlight

```c
bsp_display_brightness_init();
bsp_display_backlight_on();
bsp_display_backlight_off();
bsp_display_brightness_set(100);
```

## Dependencies

| Capability | Implementation |
|---|---|
| Display | Local `esp_lcd_st7701` 2.0.2 override |
| Touch | `espressif/esp_lcd_touch_gt911` |
| Audio | `espressif/esp_codec_dev` |
| SD card | ESP-IDF |
