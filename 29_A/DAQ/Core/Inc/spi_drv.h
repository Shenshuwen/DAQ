/**
 ******************************************************************************
 * @file    spi_drv.h
 * @brief   SPI DMA 驱动 
 * @note    SPI1: 
 *          - RX DMA (Ch2): 读取外部 SPI 从设备 (16-bit 单工 RX)
 *          - TX DMA (Ch3): 仅产生 SCK 时钟 (发哑字), 无中断
 ******************************************************************************
 */

#ifndef __SPI_DRV_H
#define __SPI_DRV_H

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "FreeRTOS.h"
#include "semphr.h"

/* ============================================================================
 * 缓冲区尺寸定义
 * ============================================================================ */

#define SPI1_RX_MAX         8      /* 单次 DMA 最大字数                   */

/* ============================================================================
 * 驱动句柄 
 * ============================================================================ */

 /**
 * @brief  SPI 驱动句柄
 * @note   hdma_rx: DMA1_Ch2 (SPI1_RX)  — TC 中断
 *         hdma_tx: DMA1_Ch3 (SPI1_TX)  — 只发哑字, 无中断
 */
typedef struct {
    SPI_TypeDef          *Instance;         /* SPI1                          */
    DMA_HandleTypeDef     hdma_rx;          /* HAL DMA 句柄 (RX)             */
    DMA_HandleTypeDef     hdma_tx;          /* HAL DMA 句柄 (TX/SCK 时钟)    */
    volatile uint8_t      busy;             /* 0:空闲, 1:DMA 传输中          */
    uint16_t             *rx_buf;           /* 用户缓冲区指针                 */
    uint16_t              rx_len;           /* 当前传输字数                   */
    SemaphoreHandle_t     rx_sem;           /* RX 完成信号量 (RTOS)           */
    DMA_Channel_TypeDef *rx_dma;
    DMA_Channel_TypeDef *tx_dma;
    IRQn_Type            tx_irq;
    IRQn_Type            rx_irq;
} SPI_DrvTypeDef;

/* ============================================================================
 * 全局变量声明
 * ============================================================================ */

 extern SPI_DrvTypeDef spi1_drv;

 /* ============================================================================
 * API 声明
 * ============================================================================ */

void SPI_DrvInit(SPI_DrvTypeDef *drv);
uint8_t SPI_ReadStart(SPI_DrvTypeDef *drv, uint16_t *buf, uint16_t count);
BaseType_t SPI_ReadWait(SPI_DrvTypeDef *drv,uint16_t **ppdata, uint16_t *plen, TickType_t timeout);
void SPI_DMARX_ISR(SPI_DrvTypeDef *drv);

#endif

