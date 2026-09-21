#pragma once
#include "esp_lcd_touch.h"

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create new touchscreen
 *
 * If you want to free resources allocated by this function, you can use esp_lcd_touch API, ie.:
 *
 * \code{.c}
 * esp_lcd_touch_del(tp);
 * \endcode
 *
 * @param[in]  config    touch configuration
 * @param[out] ret_touch esp_lcd_touch touchscreen handle
 * @return
 *      - ESP_OK         On success
 *      - Else           esp_lcd_touch failure
 */
esp_err_t bsp_touch_new(const bsp_display_cfg_t *cfg, esp_lcd_touch_handle_t *ret_touch);

#ifdef __cplusplus
}
#endif

#endif
