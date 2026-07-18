#ifndef __OTA_H
#define __OTA_H

#include <stdint.h>

/* ============================================================================
 * 分区宏定义
 * 103C8T6 
 *  -RAM: 20KB
 *  -Flash: 64KB,1kB/页，64页
 *   -Bootloader: 16KB,页0~15
 *   -App: 47KB,页16~62
 *   -OTAInfo: 1KB,页63
 * ============================================================================ */
#define FLASH_BASE_ADDR   0x08000000

#define BOOTLOADER_ADDR   FLASH_BASE_ADDR
#define BOOTLOADER_SIZE   (16UL * FLASH_PAGE_SIZE)

#define APP_ADDR           (BOOTLOADER_ADDR + BOOTLOADER_SIZE)
#define APP_SIZE           (47UL * FLASH_PAGE_SIZE)

#define OTAINFO_ADDR       (APP_ADDR + APP_SIZE)
#define OTAINFO_SIZE       (1UL * FLASH_PAGE_SIZE)

#define FLASH_END_ADDR      (OTAINFO_ADDR + OTAINFO_SIZE)

#define OTA_INFO_MAGIC     0xA5A5A5A5


#define SRAM_BASE_ADDR   0x20000000U
#define SRAM_SIZE        (20L * 1024L)
#define SRAM_END_ADDR    (SRAM_BASE_ADDR + SRAM_SIZE)

/* ============================================================================
 * 数据结构定义
 * ============================================================================ */
typedef struct {
    uint32_t magic;
    uint32_t status;
    uint32_t version;
    uint32_t size;
    uint32_t offset;
    uint32_t error;
    uint32_t crc;
    //uint32_t info_crc;
} OTA_FirInfoType;

typedef enum {
    OTA_STATE_IDLE = 0,          /* 正常状态，可跳转 App */
    OTA_STATE_REQUEST,           /* App 请求升级，Bootloader 进入升级模式 */
    OTA_STATE_ERASING,           /* Bootloader 正在擦除 App 区 */
    OTA_STATE_DOWNLOADING,       /* Bootloader 正在接收并写入 App */
    OTA_STATE_VERIFYING,         /* Bootloader 正在校验 App */
    OTA_STATE_READY,             /* 新 App 有效，可以启动 */
    OTA_STATE_FAILED             /* 升级失败，停留 Bootloader */
} OTA_StateFlag;

typedef enum {
    OTA_ERR_NONE = 0,
    OTA_ERR_STATE ,
    OTA_ERR_SIZE ,
    OTA_ERR_OFFSET ,
    OTA_ERR_FLASH_ERASE ,
    OTA_ERR_FLASH_WRITE ,
    OTA_ERR_VERIFY ,
    OTA_ERR_CRC ,
    OTA_ERR_APP_INVALID ,
    OTA_ERR_ABORT,
} OTA_ErrorFlag;


/* ============================================================================
 * 全局变量声明
 * ============================================================================ */
extern OTA_FirInfoType ota_info;

/* ============================================================================
 * 函数声明
 * ============================================================================ */
void OTA_ReadInfo(OTA_FirInfoType *info);
OTA_ErrorFlag OTA_WriteInfo(OTA_FirInfoType *info);
OTA_ErrorFlag OTA_EraseApp(void);
OTA_ErrorFlag OTA_WriteAppData(uint32_t offset, const uint8_t *buf, uint32_t len);

uint8_t OTA_AppIsValid(void);
void OTA_BootCheck(void);
void AppJump(void);

OTA_ErrorFlag OTA_Start(uint32_t version, uint32_t size,uint32_t crc);
OTA_ErrorFlag OTA_WriteData(uint32_t offset, const uint8_t *buf, uint16_t len);
OTA_ErrorFlag OTA_Finish(void);
OTA_ErrorFlag OTA_Abort(void);
#endif /* __OTA_H */
