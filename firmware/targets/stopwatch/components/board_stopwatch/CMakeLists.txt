idf_component_register(SRCS "board_stopwatch.c" INCLUDE_DIRS "include"
  REQUIRES esp_lcd esp_driver_spi esp_driver_i2c esp_driver_gpio esp_timer)
