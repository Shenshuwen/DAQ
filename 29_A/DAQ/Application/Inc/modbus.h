#ifndef __MODBUS_H
#define __MODBUS_H

#include <stdint.h>



/* ============================================================================
 * 地址及功能码定义
 * ============================================================================ */
#define InputDataNum 8  /* 输入寄存器数量 */
/* ---------- 从站地址 ---------- */
#define MODBUS_SLAVE_ADDR  0x02  

/* ---------- 错误码定义 ---------- */
#define MODBUS_EXCEPTION_ILLEGAL_FUNCTION     0x01
#define MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS 0x02
#define MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE   0x03

/* ---------- 寄存器操作功能码 ---------- */
#define MODBUS_READ_INPUT_REGISTERS   0x04 /* 读输入寄存器 */
/* ---------- OTA 升级功能码 (与29_B Bootloader一致) ---------- */
#define OTA_UPDATA             0x41 /* OTA升级请求 */


void Modbus_Updata(void);
void Modbus_Init(void);
void Modbus_RxHandler(uint8_t *pdata, uint16_t len);  
// void Modbus_Poll(UART_RxCtrlType *ctrl, uint8_t **ppdata, uint16_t *plen);  /* 保留兼容 */
const uint16_t *Modbus_GetInputData(void);

#endif
