#ifndef _I2C_SLAVE_DRV_H_
#define _I2C_SLAVE_DRV_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "hal/gpio_types.h"

#define I2C_SLAVE_DRV_PORT             (-1)
#define I2C_SLAVE_DRV_SCL_IO          GPIO_NUM_25
#define I2C_SLAVE_DRV_SDA_IO          GPIO_NUM_32
#define I2C_SLAVE_DRV_ADDR            0x28
#define I2C_SLAVE_DRV_RX_BUF_SIZE     256
#define I2C_SLAVE_DRV_TX_BUF_SIZE     256

esp_err_t i2c_slave_drv_init(void);
esp_err_t i2c_slave_drv_send_bytes(const uint8_t *data, size_t len, TickType_t timeout_ticks);
size_t i2c_slave_drv_get_last_rx(uint8_t *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif

/******************************** End of file *********************************/
