#ifndef _UART_DRV_H_
#define _UART_DRV_H_

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
*                               INCLUDES
*******************************************************************************/
#include <stdio.h>
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"


/*******************************************************************************
*                               DEFINES
*******************************************************************************/
static const int RX_BUF_SIZE = 256;


/*******************************************************************************
*                     EXTERNAL VARIABLE DECLARATIONS
*******************************************************************************/

/*******************************************************************************
*                     EXTERNAL FUNCTION DECLARATIONS
*******************************************************************************/


/*******************************************************************************
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
*******************************************************************************/
void uart2_init(void);
void uart0_init(void);

int uart_sendData(const char* logName, const char* data);
int uart_sendBytes(char* bytes, int len);












#ifdef __cplusplus
}
#endif

#endif

/******************************** End of file *********************************/
