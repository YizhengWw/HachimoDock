#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct {int dev_addr_length;unsigned device_address;unsigned scl_speed_hz;} i2c_device_config_t;
#define I2C_ADDR_BIT_LEN_7 7
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,const i2c_device_config_t *cfg,i2c_master_dev_handle_t *out);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,const uint8_t *tx,size_t txlen,uint8_t *rx,size_t rxlen,int ms);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,const uint8_t *tx,size_t txlen,int ms);
