#include "ota.h"
#include "main.h"

#include "uart_drv.h"//调试用

OTA_FirInfoType ota_info = {
    .magic  = 0XFFFFFFFF,
    .status = OTA_STATE_IDLE,
    .error  = OTA_ERR_NONE,
};

typedef void (*pFunction)(void);

/* ============================================================================
 * CRC32计算函数实现
 * ============================================================================ */
static uint32_t OTA_CalcAppCrc32(uint32_t addr, uint32_t len){
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t *p = (const uint8_t *)addr;

    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x00000001) {
                crc >>= 1;
                crc ^= 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

/* ============================================================================
 * 内部函数实现
 * ============================================================================ */
static void Flash_Read(uint32_t addr, uint8_t *buf, uint32_t len){
    const uint8_t *p = (const uint8_t *)addr;
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = p[i];
    }
}

static uint8_t Flash_Erase(uint32_t addr, uint32_t len){
    FLASH_EraseInitTypeDef eraseInit;
    uint32_t pageError = 0;
    uint8_t pageCount = (len + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;
    eraseInit.TypeErase = FLASH_TYPEERASE_PAGES;
    eraseInit.PageAddress = addr;
    eraseInit.NbPages = pageCount;

    if (HAL_FLASHEx_Erase(&eraseInit, &pageError) != HAL_OK) {
        return 1;
    }

    return 0;
}

static OTA_ErrorFlag Flash_Program(uint32_t addr, const uint8_t *buf, uint32_t len){
    for(uint32_t i = 0; i < len; i += 2){
        uint16_t halfword = buf[i];
        if((i + 1) < len){
            halfword |= ((uint16_t)buf[i + 1] << 8);
        }else{
            halfword |= 0xFF00;
        }

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr + i, halfword) != HAL_OK) {
            return OTA_ERR_FLASH_WRITE;
        }
    }
    return OTA_ERR_NONE;
}

static OTA_ErrorFlag Flash_WriteWithErase(uint32_t addr, const uint8_t *buf, uint32_t len){
    OTA_ErrorFlag err;

    HAL_FLASH_Unlock();
    if(Flash_Erase(addr, len)){
        HAL_FLASH_Lock();
        return OTA_ERR_FLASH_ERASE;
    }
    err = Flash_Program(addr, buf, len);
    HAL_FLASH_Lock();

    return err;
}

 /* ============================================================================
 * OTA信息读写函数
 * ============================================================================ */

void OTA_ReadInfo(OTA_FirInfoType *info){
    Flash_Read(OTAINFO_ADDR, (uint8_t *)info, sizeof(OTA_FirInfoType));
}
/**
 * @brief  OTA 写info
 * @param  info: 待写入的 OTA_FirInfoType 结构体指针
 * @note   TODO: 由Modbus响应中汇报错误信息，由上位机决策
 */
OTA_ErrorFlag OTA_WriteInfo(OTA_FirInfoType *info){
    OTA_ErrorFlag err = Flash_WriteWithErase(OTAINFO_ADDR, (const uint8_t *)info, sizeof(OTA_FirInfoType));
    #if DEBUG
    if (err != OTA_ERR_NONE) {
        UART1_Printf("OTA_WriteInfo error: %d\n", err);
    }
    #endif
    return err;
}

OTA_ErrorFlag OTA_EraseApp(void){
    HAL_FLASH_Unlock();
    if(Flash_Erase(APP_ADDR, APP_SIZE)){
        HAL_FLASH_Lock();
        return OTA_ERR_FLASH_ERASE;
    }
    HAL_FLASH_Lock();

    return OTA_ERR_NONE;
}

/**
 * @brief  写入 APP 固件数据
 * @note   APP 区由 OTA_Start 预先擦除，本函数只做半字编程。
 *         掉电恢复时重新调用 OTA_Start 会完整擦除 APP 区。
 */
OTA_ErrorFlag OTA_WriteAppData(uint32_t offset, const uint8_t *buf, uint32_t len){
    OTA_ErrorFlag err;

    if(buf == 0 || len == 0){
        return OTA_ERR_NONE;
    }
    if(offset >= APP_SIZE || len > (APP_SIZE - offset)){
        return OTA_ERR_OFFSET;
    }

    HAL_FLASH_Unlock();
    err = Flash_Program(APP_ADDR + offset, buf, len);
    HAL_FLASH_Lock();

    return err;
}


/* ============================================================================
 * OTA动作函数(Bootloader)
 * ============================================================================ */
/**
 * @brief  OTA 跳转应用 
 * @note   ota_info.magic 不为OTA_INFO_MAGIC 且 ota_info.status 为 OTA_STATE_IDLE 时，跳转应用
 * @note   ota_info.status 为 OTA_STATE_READY 时，跳转应用
 */
void AppJump(void){
    uint32_t app_sp = *(volatile uint32_t *)APP_ADDR;
    uint32_t app_reset = *(volatile uint32_t *)(APP_ADDR + 4);
    pFunction app = (pFunction)app_reset;

    __disable_irq();
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL = 0;

    CLEAR_BIT(huart1.Instance->CR3, USART_CR3_DMAT | USART_CR3_DMAR);
    CLEAR_BIT(huart2.Instance->CR3, USART_CR3_DMAT | USART_CR3_DMAR);
    if(huart1.hdmatx) __HAL_DMA_DISABLE(huart1.hdmatx);
    if(huart2.hdmarx) __HAL_DMA_DISABLE(huart2.hdmarx);
    if(huart2.hdmatx) __HAL_DMA_DISABLE(huart2.hdmatx);
    DMA1->IFCR = 0xFFFFFFFF;
    HAL_UART_DeInit(&huart1);
    HAL_UART_DeInit(&huart2);
    HAL_DeInit();

    for(uint32_t i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFF;
        NVIC->ICPR[i] = 0xFFFFFFFF;
    }
    SCB->VTOR = APP_ADDR;
    __set_MSP(app_sp);

    app();
}


/**
 * @brief  检查应用程序是否有效
 * @return 1: 有效, 0: 无效
 * @note   检查应用程序的栈指针和复位向量
 */
uint8_t OTA_AppIsValid(void){
    uint32_t app_sp = *(volatile uint32_t *)APP_ADDR;
    uint32_t app_reset = *(volatile uint32_t *)(APP_ADDR + 4);

    if(app_sp == 0xFFFFFFFF || app_reset == 0xFFFFFFFF){
        return 0;
    }

    if(app_sp < SRAM_BASE_ADDR || app_sp > SRAM_END_ADDR){
        return 0;
    }

    if((app_reset & 0x1) == 0){
        return 0;
    }

    app_reset &= ~0x1U;
    if(app_reset < APP_ADDR || app_reset >= OTAINFO_ADDR){
        return 0;
    }

    return 1;
}


/**
 * @brief  检查OTA状态并决定是否跳转应用
 * @note   ota_info.magic 不为OTA_INFO_MAGIC 且 ota_info.status 为 OTA_STATE_IDLE 时，跳转应用
 * @note   ota_info.status 为 OTA_STATE_READY 时，跳转应用

 */
void OTA_BootCheck(void){
    OTA_FirInfoType info;
    OTA_ReadInfo(&info);
    ota_info = info;
    if(info.magic != OTA_INFO_MAGIC){
        if(OTA_AppIsValid()){
            AppJump();
        }
        return;
    }

    switch(info.status){
        case OTA_STATE_REQUEST:
        case OTA_STATE_DOWNLOADING:
        case OTA_STATE_FAILED:
            return;   

        case OTA_STATE_READY:
            if(OTA_AppIsValid()){
                info.magic = 0xFFFFFFFF;
                info.status = OTA_STATE_IDLE;
                info.error = OTA_ERR_NONE;
                OTA_WriteInfo(&info);
                AppJump();
            }
            break;

        default:
            info.status = OTA_STATE_FAILED;
            info.error = OTA_ERR_STATE;
            OTA_WriteInfo(&info);
            break;
    }
}

/**
 * @brief  OTA 开始
 * @param  version: 新固件版本号
 * @param  size:    新固件大小
 * @param  crc:     新固件完整 CRC32
 * @note   进入升级流程前先记录 OTAInfo，随后擦除 App 区并切换到下载状态
 */
OTA_ErrorFlag OTA_Start(uint32_t version, uint32_t size, uint32_t crc){
    OTA_FirInfoType info;
    OTA_ErrorFlag err;

    if (size == 0 || size > APP_SIZE) {
        return OTA_ERR_SIZE;
    }

    OTA_ReadInfo(&info);
    info.magic   = OTA_INFO_MAGIC;
    info.status  = OTA_STATE_REQUEST;
    info.version = version;
    info.size    = size;
    info.offset  = 0;
    info.error   = OTA_ERR_NONE;
    info.crc     = crc;

    err = OTA_WriteInfo(&info);
    if (err != OTA_ERR_NONE) {
        return err;
    }

    err = OTA_EraseApp();
    if (err != OTA_ERR_NONE) {
        info.status = OTA_STATE_FAILED;
        info.error  = err;
        OTA_WriteInfo(&info);
        return err;
    }
    info.status = OTA_STATE_DOWNLOADING;
    err = OTA_WriteInfo(&info);
    if (err != OTA_ERR_NONE) {
        return err;
    }
    ota_info = info;
    return OTA_ERR_NONE;
}

OTA_ErrorFlag OTA_WriteData(uint32_t offset, const uint8_t *buf, uint16_t len){
    OTA_ErrorFlag err;
    if(ota_info.status != OTA_STATE_DOWNLOADING){
        return OTA_ERR_STATE;
    }
    if(offset != ota_info.offset){
        return OTA_ERR_OFFSET;
    }
    if(offset + len > ota_info.size){
        return OTA_ERR_SIZE;
    }
    err = OTA_WriteAppData(offset, buf, len);
    if(err != OTA_ERR_NONE){
        ota_info.status = OTA_STATE_FAILED;
        ota_info.error = err;
        OTA_WriteInfo(&ota_info);
        return err;
    }
    ota_info.offset += len;
    //不做断点续传不需要，做断点续传也应该是1kb或者固定N包才写一次
   // err = OTA_WriteInfo(&ota_info);
    return OTA_ERR_NONE;
}

/**
 * @brief  OTA 结束
 * @note   校验已接收数据是否完整，并计算 App 全量 CRC32
 * @note   校验通过后将状态置为 OTA_STATE_READY，等待跳转应用
 */
OTA_ErrorFlag OTA_Finish(void){
    uint32_t crc;

    if (ota_info.status != OTA_STATE_DOWNLOADING) {
        return OTA_ERR_STATE;
    }
    if (ota_info.offset != ota_info.size) {
        ota_info.status = OTA_STATE_FAILED;
        ota_info.error = OTA_ERR_SIZE;
        OTA_WriteInfo(&ota_info);
        return OTA_ERR_SIZE;
    }
    ota_info.status = OTA_STATE_VERIFYING;
    OTA_WriteInfo(&ota_info);

    crc = OTA_CalcAppCrc32(APP_ADDR, ota_info.size);   
    if (crc != ota_info.crc) {
        ota_info.status = OTA_STATE_FAILED;
        ota_info.error = OTA_ERR_CRC;
        OTA_WriteInfo(&ota_info);
        return OTA_ERR_CRC;
    }
    if (!OTA_AppIsValid()) {
        ota_info.status = OTA_STATE_FAILED;
        ota_info.error = OTA_ERR_APP_INVALID;
        OTA_WriteInfo(&ota_info);
        return OTA_ERR_APP_INVALID;
    }

    ota_info.status = OTA_STATE_READY;
    ota_info.error = OTA_ERR_NONE;
    OTA_WriteInfo(&ota_info);

    return OTA_ERR_NONE;
}

/**
 * @brief  OTA 中止
 * @note   主动终止本次升级流程，将状态标记为失败
 * @note   用于上位机取消升级或接收过程中发生严重错误
 */
OTA_ErrorFlag OTA_Abort(void){
    ota_info.magic = OTA_INFO_MAGIC;
    ota_info.status = OTA_STATE_FAILED;
    ota_info.error = OTA_ERR_ABORT;

    return OTA_WriteInfo(&ota_info);
}

