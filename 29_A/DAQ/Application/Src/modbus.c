#include "modbus.h"
#include "uart_drv.h"
#include "spi_drv.h"
#include "app_config.h"
#include "ota.h"
/* ============================================================================
 * 内部缓冲
 * ============================================================================
 * 双缓冲设计 (Single-Producer Single-Consumer, 无锁):
 *   Reader (Comm_Task):  直接读 active_buf, 不加锁
 *   Writer (Sensor_Task): SPI DMA → inactive_buf → 完成后原子交换 active_buf
 *
 * Cortex-M3 上 uint16_t* 的对齐读写是原子的, 无需额外同步原语.
 * Writer 始终写入 Reader 不使用的那个缓冲区, 不会冲突.
 * ============================================================================ */
static uint8_t  modbus_tx_buf[256];                         /* Modbus 帧组装缓冲区      */
static uint16_t InputData_A[InputDataNum];                  /* 双缓冲 A                 */
static uint16_t InputData_B[InputDataNum];                  /* 双缓冲 B                 */
static volatile uint16_t *active_buf = InputData_A;         /* 当前可读缓冲区指针       */

static uint8_t Modbus_Send(UART_DataDrvType *drv, uint8_t *data, uint16_t len);
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
 * @brief  Modbus 通用发送 
 * @param  drv:  UART 数据口驱动指针
 * @param  data: 待发送帧缓冲区指针 
 * @param  len:  帧总长度 (含 CRC, 单位: 字节)
 * @note   内部阻塞等待上次 DMA 发送完成 (tx_sem), 保证 modbus_tx_buf 不会竞争
 */
static uint8_t Modbus_Send(UART_DataDrvType *drv, uint8_t *data, uint16_t len){
    if(xSemaphoreTake(drv->tx_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
        #if DEBUG
        UART1_Printf("Modbus_Send: Timeout waiting for tx_sem\n");
        #endif
        return 0;
    }
    if(!UART_Data_TX_ZeroCopy(drv, data, len)){
        xSemaphoreGive(drv->tx_sem);
        #if DEBUG
        UART1_Printf("Modbus_Send: UART_Data_TX_ZeroCopy failed\n");
        #endif
        return 0;
    }
    return 1;
}

// /**
//  * @brief  Modbus 轮询接收 （裸机适配）
//  * @param  ctrl:  UART 数据口描述符队列控制块指针
//  * @param  ppdata: 待发送帧缓冲区指针的指针
//  * @param  plen:  接收数据长度指针 (含 CRC, 单位: 字节)
//  * @note   
//  */
// void Modbus_Poll(UART_RxCtrlType *ctrl, uint8_t **ppdata, uint16_t *plen){
//     if(UART_RX_ZeroCopy_Get(ctrl, ppdata, plen)){
//         Modbus_Response_ReadInputRegisters(*ppdata);
//         UART_RX_ZeroCopy_Release(ctrl);
//     }
// }

const uint16_t *Modbus_GetInputData(void){
    return (const uint16_t *)active_buf;
}


/* ============================================================================
 * 协议基础API定义
 * ============================================================================ */

/**
 * @brief  Modbus 模块初始化
 * @note   双缓冲模式下仅确保 active_buf 指向有效缓冲区, 无动态资源分配
 */
void Modbus_Init(void){
    active_buf = InputData_A;  /* 初始可读缓冲区 */
}

 /**
 * @brief  04 读输入寄存器 —— 组装响应帧并调用通用发送
 * @param  rxbuf: Modbus 请求帧缓冲区指针
 * @note   从 active_buf 快照读取 (无锁), 保证一次响应内数据一致
 */
static void Modbus_Response_ReadInputRegisters(uint8_t *rxbuf){
    uint16_t start_addr = (rxbuf[2] << 8) | rxbuf[3];
    uint16_t quantity   = (rxbuf[4] << 8) | rxbuf[5];

    if(quantity < 1 || (quantity + start_addr) > InputDataNum){
        Modbus_ExceptionResponse(MODBUS_READ_INPUT_REGISTERS,
                                 MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS);
        #if DEBUG
        UART1_Printf("Modbus: Invalid quantity %d\n", quantity);
        #endif
        return;
    }

    /* 快照 active_buf 指针: 一次读取, 保证整帧数据来自同一份缓冲区 */
    const uint16_t *buf = (const uint16_t *)active_buf;

    /* 组装响应帧 */
    uint8_t byte_count = quantity * 2;
    modbus_tx_buf[0] = MODBUS_SLAVE_ADDR;
    modbus_tx_buf[1] = MODBUS_READ_INPUT_REGISTERS;
    modbus_tx_buf[2] = byte_count;

    for(uint16_t i = 0; i < quantity; i++){
        uint16_t reg_value = buf[start_addr + i];
        modbus_tx_buf[3 + i * 2] = (reg_value >> 8) & 0xFF;
        modbus_tx_buf[4 + i * 2] = reg_value & 0xFF;
    }

    uint16_t resp_len = 3 + byte_count;
    uint16_t crc = Modbus_Crc16(modbus_tx_buf, resp_len);
    modbus_tx_buf[resp_len] = crc & 0xFF;
    modbus_tx_buf[resp_len + 1] = (crc >> 8);

    /* 发送响应帧 */
    if( Modbus_Send(&uart2, modbus_tx_buf, resp_len + 2) == 0){
        #if DEBUG
        UART1_Printf("Modbus: Failed to send response\n");
        #endif
    }
}


/* ============================================================================
 * User适配API定义
 * ============================================================================ */

 static void Modbus_OTAInfo_Updata(void);

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
        case MODBUS_READ_INPUT_REGISTERS:
            Modbus_Response_ReadInputRegisters((uint8_t *)rxbuf);
            break;
        case OTA_UPDATA:
            if(len == 4){
                Modbus_OTAInfo_Updata();
            }else{
                Modbus_ExceptionResponse(rxbuf[1], OTA_ERR_SIZE);
            }
            break;
        default:
            Modbus_ExceptionResponse(rxbuf[1], MODBUS_EXCEPTION_ILLEGAL_FUNCTION);
            #if DEBUG
            UART1_Printf("Modbus: Unsupported function code 0x%02X\n", rxbuf[1]);
            #endif
            break;
    }
}

/**
 * @brief  更新输入寄存器数据 (Sensor_Task 周期性调用)
 * @note   双缓冲策略:
 *          1. SPI DMA 写入非活跃缓冲区 (Reader 当前不读的那个)
 *          2. 等待 DMA 完成后, 原子交换 active_buf 指针
 *          3. Reader 始终无锁读取 active_buf
 * @note   500ms 超时对应上位机轮询周期, 确保在上位机请求前数据已刷新
 */
void Modbus_Updata(void){
    uint16_t *pdata;
    uint16_t  len;

    /* 选择非活跃缓冲区 — 不会被 Reader 同时访问 */
    uint16_t *inactive = (active_buf == InputData_A) ? InputData_B : InputData_A;

    if(!SPI_ReadStart(&spi1_drv, inactive, InputDataNum)){
        #if DEBUG
        UART1_Printf("Modbus_Updata: SPI busy, cannot start read\n");
        #endif
        return;
    }
    if(SPI_ReadWait(&spi1_drv, &pdata, &len, pdMS_TO_TICKS(500)) != pdTRUE){
        #if DEBUG
        UART1_Printf("Modbus_Updata: SPI read timeout\n");
        #endif
        return;
    }

    /* 原子交换: 新数据立即可读, 无需解锁 */
    active_buf = inactive;
} 


static void Modbus_OTAInfo_Updata(void){
    OTA_FirInfoType info = {0};
    info.magic  = OTA_INFO_MAGIC;
    info.status = OTA_STATE_REQUEST;
    info.error  = OTA_ERR_NONE;
    OTA_ErrorFlag err = OTA_WriteInfo(&info);
    if(err == OTA_ERR_NONE){
        modbus_tx_buf[0] = MODBUS_SLAVE_ADDR;
        modbus_tx_buf[1] = OTA_UPDATA;
        modbus_tx_buf[2] = 0x00;  /* 0=成功, 其他=错误 */
        uint16_t crc = Modbus_Crc16(modbus_tx_buf, 3);
        modbus_tx_buf[3] = crc & 0xFF;
        modbus_tx_buf[4] = (crc >> 8);
        if(Modbus_Send(&uart2, modbus_tx_buf, 5)){
            xSemaphoreTake(uart2.tx_sem, pdMS_TO_TICKS(200));
            while(!(USART2->SR & USART_SR_TC));
        }
        NVIC_SystemReset();
    }else{
        Modbus_ExceptionResponse(OTA_UPDATA, err);
    }
};

/**
 * @brief  Modbus 接收回调
 * @param  pdata: 接收到的帧数据指针
 * @param  len: 接收到的帧长度
 * @note   该函数作为 UART_RX_Wait 的回调，处理接收到的 Modbus 请求帧
 */
void Modbus_RxHandler(uint8_t *pdata, uint16_t len){
    Modbus_Dispatch(pdata, len);
}
