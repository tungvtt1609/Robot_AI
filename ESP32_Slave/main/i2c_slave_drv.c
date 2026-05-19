/*******************************************************************************
**                               INCLUDES
*******************************************************************************/
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/i2c_slave.h"
#include "esp_check.h"
#include "esp_log.h"
#include "i2c_slave_drv.h"

/*******************************************************************************
**                       INTERNAL MACRO DEFINITIONS
*******************************************************************************/
#define I2C_SLAVE_TAG "i2c_slave_drv"
#define I2C_SLAVE_TASK_STACK_SIZE (3 * 1024)
#define I2C_SLAVE_TASK_PRIORITY   5

typedef enum {
    I2C_SLAVE_EVENT_RX,
    I2C_SLAVE_EVENT_TX_REQUEST,
} i2c_slave_event_t;

/*******************************************************************************
**                      INTERNAL VARIABLE DEFINITIONS
*******************************************************************************/
static i2c_slave_dev_handle_t s_i2c_slave_handle;
static QueueHandle_t s_i2c_event_queue;
static SemaphoreHandle_t s_i2c_data_lock;

static uint8_t s_rx_buf[I2C_SLAVE_DRV_RX_BUF_SIZE];
static size_t s_rx_len;
static uint8_t s_last_rx_buf[I2C_SLAVE_DRV_RX_BUF_SIZE];
static size_t s_last_rx_len;
static uint8_t s_tx_buf[I2C_SLAVE_DRV_TX_BUF_SIZE] = "ESP32_SLAVE_READY";
static size_t s_tx_len = 17;

/*******************************************************************************
**                      INTERNAL FUNCTION PROTOTYPES
*******************************************************************************/
static bool i2c_slave_request_cb(i2c_slave_dev_handle_t i2c_slave,
                                 const i2c_slave_request_event_data_t *evt_data,
                                 void *arg);
static bool i2c_slave_receive_cb(i2c_slave_dev_handle_t i2c_slave,
                                 const i2c_slave_rx_done_event_data_t *evt_data,
                                 void *arg);
static void i2c_slave_task(void *arg);
static void i2c_slave_prepare_ack_locked(const uint8_t *data, size_t len);
static esp_err_t i2c_slave_write_current_tx(TickType_t timeout_ticks);

/*******************************************************************************
**                           FUNCTION DEFINITIONS
*******************************************************************************/
esp_err_t i2c_slave_drv_init(void)
{
    if (s_i2c_slave_handle != NULL) {
        return ESP_OK;
    }

    s_i2c_event_queue = xQueueCreate(8, sizeof(i2c_slave_event_t));
    ESP_RETURN_ON_FALSE(s_i2c_event_queue != NULL, ESP_ERR_NO_MEM, I2C_SLAVE_TAG, "create event queue failed");

    s_i2c_data_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_i2c_data_lock != NULL, ESP_ERR_NO_MEM, I2C_SLAVE_TAG, "create data lock failed");

    const i2c_slave_config_t slave_config = {
        .i2c_port = I2C_SLAVE_DRV_PORT,
        .sda_io_num = I2C_SLAVE_DRV_SDA_IO,
        .scl_io_num = I2C_SLAVE_DRV_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .send_buf_depth = I2C_SLAVE_DRV_TX_BUF_SIZE,
        .receive_buf_depth = I2C_SLAVE_DRV_RX_BUF_SIZE,
        .slave_addr = I2C_SLAVE_DRV_ADDR,
        .addr_bit_len = I2C_ADDR_BIT_LEN_7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };

    ESP_RETURN_ON_ERROR(i2c_new_slave_device(&slave_config, &s_i2c_slave_handle),
                        I2C_SLAVE_TAG, "new slave device failed");

    const i2c_slave_event_callbacks_t callbacks = {
        .on_request = i2c_slave_request_cb,
        .on_receive = i2c_slave_receive_cb,
    };
    ESP_RETURN_ON_ERROR(i2c_slave_register_event_callbacks(s_i2c_slave_handle, &callbacks, NULL),
                        I2C_SLAVE_TAG, "register callbacks failed");

    ESP_RETURN_ON_FALSE(xTaskCreate(i2c_slave_task, "i2c_slave_task", I2C_SLAVE_TASK_STACK_SIZE,
                                    NULL, I2C_SLAVE_TASK_PRIORITY, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, I2C_SLAVE_TAG, "create task failed");

    ESP_RETURN_ON_ERROR(i2c_slave_write_current_tx(pdMS_TO_TICKS(100)),
                        I2C_SLAVE_TAG, "preload tx buffer failed");

    ESP_LOGI(I2C_SLAVE_TAG, "initialized: addr=0x%02X scl=%d sda=%d",
             I2C_SLAVE_DRV_ADDR, I2C_SLAVE_DRV_SCL_IO, I2C_SLAVE_DRV_SDA_IO);
    return ESP_OK;
}

esp_err_t i2c_slave_drv_send_bytes(const uint8_t *data, size_t len, TickType_t timeout_ticks)
{
    ESP_RETURN_ON_FALSE(s_i2c_slave_handle != NULL, ESP_ERR_INVALID_STATE,
                        I2C_SLAVE_TAG, "driver is not initialized");
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, I2C_SLAVE_TAG, "data is NULL");
    ESP_RETURN_ON_FALSE(len > 0 && len <= I2C_SLAVE_DRV_TX_BUF_SIZE, ESP_ERR_INVALID_SIZE,
                        I2C_SLAVE_TAG, "invalid tx length");

    xSemaphoreTake(s_i2c_data_lock, portMAX_DELAY);
    memcpy(s_tx_buf, data, len);
    s_tx_len = len;
    xSemaphoreGive(s_i2c_data_lock);

    return i2c_slave_write_current_tx(timeout_ticks);
}

size_t i2c_slave_drv_get_last_rx(uint8_t *out, size_t out_size)
{
    if (out == NULL || out_size == 0 || s_i2c_data_lock == NULL) {
        return 0;
    }

    xSemaphoreTake(s_i2c_data_lock, portMAX_DELAY);
    const size_t copy_len = s_last_rx_len < out_size ? s_last_rx_len : out_size;
    memcpy(out, s_last_rx_buf, copy_len);
    xSemaphoreGive(s_i2c_data_lock);
    return copy_len;
}

static bool i2c_slave_request_cb(i2c_slave_dev_handle_t i2c_slave,
                                 const i2c_slave_request_event_data_t *evt_data,
                                 void *arg)
{
    (void)i2c_slave;
    (void)evt_data;
    (void)arg;
    BaseType_t task_woken = pdFALSE;
    const i2c_slave_event_t event = I2C_SLAVE_EVENT_TX_REQUEST;
    xQueueSendFromISR(s_i2c_event_queue, &event, &task_woken);
    return task_woken == pdTRUE;
}

static bool i2c_slave_receive_cb(i2c_slave_dev_handle_t i2c_slave,
                                 const i2c_slave_rx_done_event_data_t *evt_data,
                                 void *arg)
{
    (void)i2c_slave;
    (void)arg;
    BaseType_t task_woken = pdFALSE;

    if (evt_data != NULL && evt_data->buffer != NULL) {
        const size_t copy_len = evt_data->length < I2C_SLAVE_DRV_RX_BUF_SIZE ?
                                evt_data->length : I2C_SLAVE_DRV_RX_BUF_SIZE;
        memcpy(s_rx_buf, evt_data->buffer, copy_len);
        s_rx_len = copy_len;

        const i2c_slave_event_t event = I2C_SLAVE_EVENT_RX;
        xQueueSendFromISR(s_i2c_event_queue, &event, &task_woken);
    }

    return task_woken == pdTRUE;
}

static void i2c_slave_task(void *arg)
{
    (void)arg;
    i2c_slave_event_t event;

    while (1) {
        if (xQueueReceive(s_i2c_event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (event == I2C_SLAVE_EVENT_RX) {
            xSemaphoreTake(s_i2c_data_lock, portMAX_DELAY);
            memcpy(s_last_rx_buf, s_rx_buf, s_rx_len);
            s_last_rx_len = s_rx_len;
            i2c_slave_prepare_ack_locked(s_rx_buf, s_rx_len);
            xSemaphoreGive(s_i2c_data_lock);

            ESP_LOG_BUFFER_HEX_LEVEL(I2C_SLAVE_TAG, s_last_rx_buf, s_last_rx_len, ESP_LOG_INFO);
            ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_slave_write_current_tx(pdMS_TO_TICKS(100)));
        } else if (event == I2C_SLAVE_EVENT_TX_REQUEST) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_slave_write_current_tx(0));
        }
    }
}

static void i2c_slave_prepare_ack_locked(const uint8_t *data, size_t len)
{
    int written = snprintf((char *)s_tx_buf, I2C_SLAVE_DRV_TX_BUF_SIZE, "ACK:");
    if (written < 0) {
        s_tx_len = 0;
        return;
    }

    size_t offset = (size_t)written;
    for (size_t i = 0; i < len && offset + 3 < I2C_SLAVE_DRV_TX_BUF_SIZE; i++) {
        written = snprintf((char *)&s_tx_buf[offset], I2C_SLAVE_DRV_TX_BUF_SIZE - offset,
                           "%02X", data[i]);
        if (written < 0) {
            break;
        }
        offset += (size_t)written;
    }
    s_tx_len = offset;
}

static esp_err_t i2c_slave_write_current_tx(TickType_t timeout_ticks)
{
    uint8_t tx_copy[I2C_SLAVE_DRV_TX_BUF_SIZE];
    size_t tx_len;

    xSemaphoreTake(s_i2c_data_lock, portMAX_DELAY);
    tx_len = s_tx_len;
    memcpy(tx_copy, s_tx_buf, tx_len);
    xSemaphoreGive(s_i2c_data_lock);

    uint32_t write_len = 0;
    return i2c_slave_write(s_i2c_slave_handle, tx_copy, tx_len, &write_len,
                           pdTICKS_TO_MS(timeout_ticks));
}

/******************************** End of file **********************************/
