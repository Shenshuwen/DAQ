#ifndef __MODBUS_H
#define __MODBUS_H

#include <stdint.h>
#include "uart_drv.h"


/* ============================================================================
 * 地址及功能码定义
 * ============================================================================ */
/* ---------- 从站地址 ---------- */
#define MODBUS_SLAVE_ADDR  0x02  


/* ---------- Start包定义 ---------- */
#define OTA_CMD_START 0x01
#define OTA_CMD_DATA  0x02
#define OTA_CMD_END   0x03
#define OTA_CMD_ABORT 0x04


/* ---------- OTA 升级功能码 ---------- */
#define OTA_UPDATA             0x41 /* OTA升级请求 */
#define OTA_JUMP_APP           0x42 /*  跳转到应用程序 */


void Modbus_RxHandler(uint8_t *pdata, uint16_t len);  
void Modbus_Poll(UART_RxCtrlType *ctrl, uint8_t **ppdata, uint16_t *plen);  

#endif
