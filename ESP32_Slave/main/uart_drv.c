/*******************************************************************************
**                               INCLUDES
*******************************************************************************/
#include <stdio.h>
#include "math.h"
#include "string.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "uart_drv.h"
#include "cmn.h"
#include "gif_player.h"
/*******************************************************************************
**                       INTERNAL MACRO DEFINITIONS
*******************************************************************************/
#define UART_TAG "uart_drv"

#define TXD_PIN (GPIO_NUM_17)
#define RXD_PIN (GPIO_NUM_16)

#define UART0_TXD_PIN GPIO_NUM_1
#define UART0_RXD_PIN GPIO_NUM_3
/*******************************************************************************
**                     EXTERNAL VARIABLE DECLARATIONS
*******************************************************************************/


/*******************************************************************************
**                      COMMON VARIABLE DEFINITIONS
*******************************************************************************/

/*******************************************************************************
**                      INTERNAL VARIABLE DEFINITIONS
*******************************************************************************/

/*******************************************************************************
**                      INTERNAL FUNCTION PROTOTYPES
*******************************************************************************/
static void uart0_rx_task(void *arg);
static void uart2_rx_task(void *arg);

volatile uint8_t play_id = 0;
volatile bool change_play_id = false;
/*******************************************************************************
**                           FUNCTION DEFINITIONS
*******************************************************************************/


/******************************************************************************
* @Function:     name function
*
* @Description:
*               to be define
*
* @Parameters:
*               to be define
*               [in]
*               [out]
*
* @Return:
*               to be define
******************************************************************************/
void uart2_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // We won't use a buffer for sending data.
    uart_driver_install(UART_NUM_1, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_1, &uart_config);
    uart_set_pin(UART_NUM_1, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ESP_LOGI(UART_TAG, "uart2_init: Initialized!");

    // Create a task to continuously read data from UART1
    xTaskCreate(uart2_rx_task, "uart2_rx_task", 1024 * 2, NULL, configMAX_PRIORITIES - 1, NULL);
}

/******************************************************************************
* @Function:     name function
*
* @Description:
*               to be define
*
* @Parameters:
*               to be define
*               [in]
*               [out]
*
* @Return:
*               to be define
******************************************************************************/
void uart0_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // We won't use a buffer for sending data.
    uart_driver_install(UART_NUM_0, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_0, &uart_config);
    uart_set_pin(UART_NUM_0, UART0_TXD_PIN, UART0_RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ESP_LOGI(UART_TAG, "uart0_init: Initialized!");

    // Create a task to continuously read data from UART0
    xTaskCreate(uart0_rx_task, "uart0_rx_task", 1024 * 2, NULL, configMAX_PRIORITIES - 1, NULL);
}

/******************************************************************************
* @Function:    sendData
*
* @Description:
*               to be define
*
* @Parameters:
*               to be define
*               [in]
*               [out]
*
* @Return:
*               to be define
******************************************************************************/
int uart_sendData(const char* logName, const char* data)
{
    const int len = strlen(data);
    const int txBytes = uart_write_bytes(UART_NUM_1, data, len);
    ESP_LOGI(logName, "Wrote %d bytes", txBytes);
    return txBytes;
}


/******************************************************************************
* @Function:    uart_sendBytes
*
* @Description:
*               to be define
*
* @Parameters:
*               [in] bytes: byte array
*               [in] len: length of byte array
*
* @Return:
*               number of bytes sent
******************************************************************************/
int uart_sendBytes(char* bytes, int len)
{
    const int txBytes = uart_write_bytes(UART_NUM_1, bytes, len);
    return txBytes;
}

/*******************************************************************************
* @Function:     uart0_rx_task
*
* @Description:
*               to be define
*
* @Parameters:
*               to be define
*               [in]
*               [out]
*
* @Return:
*               to be define
*******************************************************************************/
static void uart0_rx_task(void *arg)
{
    static const char *RX0_TASK_TAG = "RX0_TASK";
    esp_log_level_set(RX0_TASK_TAG, ESP_LOG_INFO);
    uint8_t* data = (uint8_t*) malloc(RX_BUF_SIZE + 1);
    while (1)
    {
        const int rxBytes = uart_read_bytes(UART_NUM_0, data, RX_BUF_SIZE, pdMS_TO_TICKS(1000));
        if (rxBytes > 0)
        {
            data[rxBytes] = 0;
            ESP_LOGI(RX0_TASK_TAG, "Read %d bytes: '%s'", rxBytes, data);

            play_id = atoi((char*)data);
            ESP_LOGI(RX0_TASK_TAG, "Updated play_id to %d", play_id);
            gif_player_stop();

        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    free(data);
}

static void uart2_rx_task(void *arg)
{
    static const char *RX2_TASK_TAG = "RX2_TASK";
    esp_log_level_set(RX2_TASK_TAG, ESP_LOG_INFO);
    uint8_t* data = (uint8_t*) malloc(RX_BUF_SIZE + 1);
    while (1)
    {
        const int rxBytes = uart_read_bytes(UART_NUM_1, data, RX_BUF_SIZE, pdMS_TO_TICKS(10));
        if (rxBytes > 0)
        {
            data[rxBytes] = 0;
            ESP_LOGI(RX2_TASK_TAG, "Read %d bytes: '%s'", rxBytes, data);

            play_id = atoi((char*)data);
            ESP_LOGI(RX2_TASK_TAG, "Updated play_id to %d", play_id);
            gif_player_stop();
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    free(data);
}
/******************************** End of file **********************************/
