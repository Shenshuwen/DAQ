#include "modbus.h"
#include "uart_drv.h"
#include "app_config.h"
#include "ota.h"

static uint8_t  modbus_tx_buf[256];                         /* Modbus 帧组装缓冲区      */

static uint8_t Modbus_Send(UART_DataDrvType *drv, uint8_t *data, uint16_t len);

/* ============================================================================
 * OTA相关函数定义
 * ============================================================================ */
#define OTA_RESP_OK          0x00
#define OTA_RESP_ERR         0x01
#define OTA_PACKET_DATA_MAX  240U

/* ============================================================================
 * CRC 校验函数
 * ============================================================================ */
static uint16_t Modbus_Crc16(uint8_t *data, uint16_t len){
    uint16_t crc = 0xFFFF;

    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static uint8_t Modbus_CheckCrc16(uint8_t *frame, uint16_t len){
    if(len < 4) {
        return 0; // 帧长度不足
    }
    uint16_t received_crc = (frame[len - 1] << 8) | frame[len - 2];
    uint16_t calculated_crc = Modbus_Crc16(frame, len - 2);
    return (received_crc == calculated_crc);
}

static uint16_t Modbus_GetU16BE(const uint8_t *p){
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t Modbus_GetU32BE(const uint8_t *p){
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |
           p[3];
}

static void Modbus_PutU32BE(uint8_t *p, uint32_t v){
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}


static void Modbus_ExceptionResponse(uint8_t func,uint8_t code){
   modbus_tx_buf[0] = MODBUS_SLAVE_ADDR;
   modbus_tx_buf[1] = func | 0x80;  /* 异常响应功能码 */
   modbus_tx_buf[2] = code;          /* 异常代码 */
   uint16_t crc = Modbus_Crc16(modbus_tx_buf, 3);
   modbus_tx_buf[3] = crc & 0xFF;
   modbus_tx_buf[4] = (crc >> 8) ;
   Modbus_Send(&uart2, modbus_tx_buf, 5);
}
/* ============================================================================
 * Port适配API定义
 * ============================================================================ */

/**
 * @brief  Modbus 通用发送 — 裸机版本
 * @note   Bootloader 无 RTOS，使用轮询等待 tx_busy 清除。
 *         超时后强制复位 tx_busy 恢复后续发送能力。
 */
static uint8_t Modbus_Send(UART_DataDrvType *drv, uint8_t *data, uint16_t len){
    /*  轮询等待上一次 DMA 发送完成 (tx_busy 由 DMA TC ISR 清除) */
    uint32_t timeout = 100000;  /* 粗略超时, 防止死循环 */
    while (drv->tx_busy && --timeout);
    if (timeout == 0) {
        drv->tx_busy = 0; /* 超时强制恢复，避免永久阻塞后续发送 */
        #if DEBUG
        UART1_Printf("Modbus_Send: Timeout waiting for tx_busy\n");
        #endif
        return 0;
    }
    return UART_Data_TX_ZeroCopy(drv, data, len);
}

/**
 * @brief  Modbus 轮询接收 （裸机适配）
 * @param  ctrl:  UART 数据口描述符队列控制块指针
 * @param  ppdata: 待发送帧缓冲区指针的指针
 * @param  plen:  接收数据长度指针 (含 CRC, 单位: 字节)
 * @note   
 */
void Modbus_Poll(UART_RxCtrlType *ctrl, uint8_t **ppdata, uint16_t *plen){
    if(UART_RX_ZeroCopy_Get(ctrl, ppdata, plen)){
        Modbus_RxHandler(*ppdata, *plen);
        UART_RX_ZeroCopy_Release(ctrl);
    }
}

static void Modbus_OTA_Response(uint8_t cmd, OTA_ErrorFlag err){
    uint16_t crc;

    modbus_tx_buf[0] = MODBUS_SLAVE_ADDR;
    modbus_tx_buf[1] = OTA_UPDATA;
    modbus_tx_buf[2] = cmd;
    modbus_tx_buf[3] = (err == OTA_ERR_NONE) ? OTA_RESP_OK : OTA_RESP_ERR;
    modbus_tx_buf[4] = (uint8_t)err;

    Modbus_PutU32BE(&modbus_tx_buf[5], ota_info.offset);

    crc = Modbus_Crc16(modbus_tx_buf, 9);
    modbus_tx_buf[9] = crc & 0xFF;
    modbus_tx_buf[10] = crc >> 8;

    Modbus_Send(&uart2, modbus_tx_buf, 11);
}


/* ============================================================================
 * User适配API定义
 * ============================================================================ */
static void Modbus_OTA_Start(const uint8_t *rxbuf, uint16_t len){
    uint32_t version;
    uint32_t size;
    uint32_t crc;
    OTA_ErrorFlag err;

    if(len != 17){
        Modbus_OTA_Response(OTA_CMD_START, OTA_ERR_SIZE);
        return;
    }

    version = Modbus_GetU32BE(&rxbuf[3]);
    size    = Modbus_GetU32BE(&rxbuf[7]);
    crc     = Modbus_GetU32BE(&rxbuf[11]);

    err = OTA_Start(version, size, crc);
    Modbus_OTA_Response(OTA_CMD_START, err);
}

static void Modbus_OTA_Data(const uint8_t *rxbuf, uint16_t len){
    uint32_t offset;
    uint16_t data_len;
    OTA_ErrorFlag err;

    if(len < 11){
        Modbus_OTA_Response(OTA_CMD_DATA, OTA_ERR_SIZE);
        return;
    }

    offset   = Modbus_GetU32BE(&rxbuf[3]);
    data_len = Modbus_GetU16BE(&rxbuf[7]);

    if(data_len == 0 || data_len > OTA_PACKET_DATA_MAX){
        Modbus_OTA_Response(OTA_CMD_DATA, OTA_ERR_SIZE);
        return;
    }

    if(len != (uint16_t)(9 + data_len + 2)){
        Modbus_OTA_Response(OTA_CMD_DATA, OTA_ERR_SIZE);
        return;
    }

    err = OTA_WriteData(offset, &rxbuf[9], data_len);
    Modbus_OTA_Response(OTA_CMD_DATA, err);
}


static void Modbus_WaitTxDone(UART_DataDrvType *drv){
    uint32_t timeout = 1000000;
    while(drv->tx_busy && --timeout);
}

static void Modbus_OTA_End(const uint8_t *rxbuf, uint16_t len){
    OTA_ErrorFlag err;

    if(len != 5){
        Modbus_OTA_Response(OTA_CMD_END, OTA_ERR_SIZE);
        return;
    }

    err = OTA_Finish();
    Modbus_OTA_Response(OTA_CMD_END, err);

    if(err == OTA_ERR_NONE){
        Modbus_WaitTxDone(&uart2);
        NVIC_SystemReset();
    }
}

static void Modbus_OTA_Abort(const uint8_t *rxbuf, uint16_t len){
    OTA_ErrorFlag err;

    if(len != 5){
        Modbus_OTA_Response(OTA_CMD_ABORT, OTA_ERR_SIZE);
        return;
    }
    (void)rxbuf; /* 帧校验已通过，无需再解析 */
    err = OTA_Abort();
    Modbus_OTA_Response(OTA_CMD_ABORT, err);
}



 static void Modbus_OTA_Updata(const uint8_t *rxbuf, uint16_t len){
    if(len < 5){
        Modbus_ExceptionResponse(OTA_UPDATA, OTA_ERR_SIZE);  
        return;
    }

    switch(rxbuf[2]){
        case OTA_CMD_START:
            Modbus_OTA_Start(rxbuf, len);
            break;
        case OTA_CMD_DATA:
            Modbus_OTA_Data(rxbuf, len);
            break;
        case OTA_CMD_END:
            Modbus_OTA_End(rxbuf, len);
            break;
        case OTA_CMD_ABORT:
            Modbus_OTA_Abort(rxbuf, len);
            break;
        default:
            Modbus_ExceptionResponse(OTA_UPDATA, OTA_ERR_STATE);  
            break; 
    }
}



 /**
 * @brief  分发Modbus请求
 * @param  rxbuf: 接收到的Modbus请求帧
 * @param  len: 请求帧长度
 * @note   根据功能码调用对应的处理函数
 *
 */
static void Modbus_Dispatch(const uint8_t *rxbuf, uint16_t len){
    if(rxbuf[0] != MODBUS_SLAVE_ADDR){
        #if DEBUG
        UART1_Printf("Modbus: Address mismatch. Received: 0x%02X, Expected: 0x%02X\n", rxbuf[0], MODBUS_SLAVE_ADDR);
        #endif
        return;
    }
    if(!Modbus_CheckCrc16((uint8_t *)rxbuf, len)){
        #if DEBUG
        UART1_Printf("Modbus: CRC check failed\n");
        #endif
        return;
    }

    switch(rxbuf[1]){
        case OTA_UPDATA:
            Modbus_OTA_Updata(rxbuf, len);
            break;  
        case OTA_JUMP_APP:
            if(OTA_AppIsValid()){
                AppJump();
            }else{
                Modbus_ExceptionResponse(rxbuf[1], OTA_ERR_APP_INVALID);  
            }
            break;
        default:
            Modbus_ExceptionResponse(rxbuf[1], OTA_ERR_STATE);  
            break;
    }
}


/**
 * @brief  Modbus 接收回调
 * @param  pdata: 接收到的帧数据指针
 * @param  len: 接收到的帧长度
 * @note   该函数作为 UART_RX_Wait 的回调，处理接收到的 Modbus 请求帧
 */
void Modbus_RxHandler(uint8_t *pdata, uint16_t len){
    Modbus_Dispatch(pdata, len);
}
