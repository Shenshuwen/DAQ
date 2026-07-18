#include "ota.h"
#include "main.h"

#include "uart_drv.h"//调试用

OTA_FirInfoType ota_info = {
    .magic  = 0XFFFFFFFF,
    .status = OTA_STATE_IDLE,
    .error  = OTA_ERR_NONE,
};


/* ============================================================================
 * 内部函数声明
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

static OTA_ErrorFlag Flash_Write(uint32_t addr, uint8_t *buf, uint32_t len){
    HAL_FLASH_Unlock();
    if(Flash_Erase(addr, len)){
        HAL_FLASH_Lock();
        return OTA_ERR_FLASH_ERASE;
    }
    for(uint32_t i = 0; i < len; i += 2){
        uint16_t halfword = buf[i];
        if((i + 1) < len){
            halfword |= ((uint16_t)buf[i + 1] << 8);
        }else{
            halfword |= 0xFF00;
        }
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr + i, halfword) != HAL_OK) {
            HAL_FLASH_Lock();
            return OTA_ERR_FLASH_WRITE;
        }
    }
    HAL_FLASH_Lock();
    return OTA_ERR_NONE;
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
    OTA_ErrorFlag err = Flash_Write(OTAINFO_ADDR, (uint8_t *)info, sizeof(OTA_FirInfoType));
    #if DEBUG
    if (err != OTA_ERR_NONE) {
        UART1_Printf("OTA_WriteInfo error: %d\n", err);
    }
    #endif
    return err;
}

/* ============================================================================
 * OTA基础操作函数(Bootloader)
 * ============================================================================ */

/* ============================================================================
 * OTA动作函数(Bootloader)
 * ============================================================================ */
/**
 * @brief  OTA 跳转应用 
 * @note   ota_info.magic 不为OTA_INFO_MAGIC 且 ota_info.status 为 OTA_STATE_IDLE 时，跳转应用
 * @note   ota_info.status 为 OTA_STATE_READY 时，跳转应用
 */
// void OTA_JumpApp(void){

// }

// void OTA_Updata(void){

// }

// void OTA_BootProcess(void){
//     OTA_ReadInfo(&ota_info);

//     if((ota_info.magic != OTA_INFO_MAGIC) && (ota_info.status == OTA_STATE_IDLE)){
//         OTA_JumpApp();
//     }

//     switch (ota_info.status){
//         case OTA_STATE_REQUEST:
//         ota_info.status = OTA_STATE_ERASING;
//         OTA_WriteInfo(&ota_info);
//         OTA_Updata();
//         break;
//         case OTA_STATE_ERASING:
//         ota_info.status = OTA_STATE_DOWNLOADING;
//         break;
//         case OTA_STATE_DOWNLOADING:
//         break;
//         case OTA_STATE_VERIFYING:
//         break;
//         case OTA_STATE_READY:
//         ota_info.status = OTA_STATE_IDLE;
//         ota_info.magic = 0xFFFFFFFF;
//         OTA_WriteInfo(&ota_info);
//         //TODO: 复位
//         break;
//         case OTA_STATE_FAILED:
//         break;
//         default:
//         break;
//     }
// }

