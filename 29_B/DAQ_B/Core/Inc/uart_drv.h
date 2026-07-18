/**
 ******************************************************************************
 * @file    uart_drv.h
 * @brief   UART  DMA 驱动 
 * @note    UART1：负责打印
 *          - RX: 暂无
 *          - TX: 环形缓冲+字节流
 *          UART2: 负责数据传输
 *          - RX: 固定缓冲区+描述符队列
 *          - TX: 零拷贝单帧
 ******************************************************************************
 */

#ifndef __UART_DRV_H
#define __UART_DRV_H

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>


/* ============================================================================
 * 缓冲区尺寸定义 
 * ============================================================================ */

/* UART1用于串口调试 */
#define UART1_TX_SIZE         255  /* 发送环形缓冲 */

/* UART2 用于数据传输 */
#define UART2_RX_SIZE       1024 /* DMA 接收缓冲 */
#define UART2_RX_MAX        255  /* 单次 DMA 长度 = 256 */
#define CBBUFFER_SIZE       8    /* 描述符槽位数量 */

/* ============================================================================
 * 数据结构
 * ============================================================================ */

 /**
 * @brief  RX 描述符 —— 指向 DMA 缓冲区内一帧数据的起止位置
 * @note   UARTx_RxBuf 内的指针区间
 */
typedef struct {
    uint8_t *Start;     /* 帧起始地址        */
    uint8_t *End;       /* 帧结束地址       */
} UART_RxDescType;

/**
 * @brief  RX 描述符环形队列控制块
 * @note   In  = 生产者 (IDLE ISR 写入, 推进)
 *         Out = 消费者 (应用层通过 ZeroCopy_Get/Release 推进)
 *         End = 哨兵, 指向 Desc 数组最后一个元素, 用于回绕判定
 */
typedef struct {
    UART_RxDescType Desc[CBBUFFER_SIZE];  /* 定长描述符数组          */
    UART_RxDescType *In;                  /* 生产者指针 (ISR 写入)   */
    UART_RxDescType *Out;                 /* 消费者指针 (应用层读取) */
    UART_RxDescType *End;                 /* 哨兵 = &Desc[SIZE-1]    */
} UART_RxCtrlType;

/**
 * @brief  TX 环形缓冲 
 * @note   Head = 生产者 (Printf 写入), 仅在任务上下文中修改
 *         Tail = 消费者 (DMA TC ISR 推进), 仅在 ISR 中修改
 *         两者独立且单向推进, 无数据竞争; 仅 TxBusy 有竞争需保护
 */
typedef struct {
    uint8_t  Buffer[UART1_TX_SIZE];       /* 环形缓冲本体 (用宏中较大的 size) */
    uint16_t Head;                        /* 生产者写入位置                   */
    uint16_t Tail;                        /* 消费者 (DMA) 已发送位置          */
} UART_TxRingType;

/**
 * @brief  UART 打印口驱动 (UART1: 环形缓冲 + DMA TX)
 */
typedef struct {
    UART_HandleTypeDef   *huart;
    DMA_HandleTypeDef    *hdma_tx;
    UART_TxRingType       ring;
    volatile uint8_t      tx_busy;//快速查询标志位，0 :dma空闲,1:dma忙
    volatile uint16_t     tx_last_len;
} UART_PrintDrvType;

/**
 * @brief  UART 数据口驱动 (UART2: RX 描述符 + TX 零拷贝)
 */
typedef struct {
    UART_HandleTypeDef   *huart;
    DMA_HandleTypeDef    *hdma_rx;
    DMA_HandleTypeDef    *hdma_tx;
    UART_RxCtrlType       rx_ctrl;
    uint8_t               rx_buf[UART2_RX_SIZE];
    volatile uint8_t      tx_busy;
    volatile uint16_t     tx_last_len;
    volatile uint8_t      rx_busy;
} UART_DataDrvType;

/* ============================================================================
 * 全局变量声明
 * ============================================================================ */

/* --- UART1 (调试口) --- */
extern UART_PrintDrvType    uart1;                    /* 打印: 环形缓冲+字节流       */

/* --- UART2 (数据口) --- */
extern UART_DataDrvType     uart2;                    /* 数据: 描述符队列+零拷贝     */

/* ============================================================================
 * RX 回调函数类型
 * ============================================================================ */

/**
 * @brief  RX 数据帧处理回调
 * @param  pdata: 帧数据指针 (指向 DMA 缓冲区, 零拷贝, Release 前有效)
 * @param  len:   帧长度 (字节)
 * @warning 必须在 Release 之前处理完毕或自行拷贝
 */
typedef void (*UART_RxHandler)(uint8_t *pdata, uint16_t len);

/* ============================================================================
 * API 函数声明
 * ============================================================================ */

void UART1_DrvInit(void);
void UART2_DrvInit(void);
uint8_t UART_RX_ZeroCopy_Get(UART_RxCtrlType *ctrl, uint8_t **ppdata, uint16_t *plen);
void UART_RX_ZeroCopy_Release(UART_RxCtrlType *ctrl);
void UART1_Printf(const char *fmt, ...);
uint8_t UART_Data_TX_ZeroCopy(UART_DataDrvType *drv, const uint8_t *pdata, uint16_t len);
void UART_RX_Poll(UART_RxCtrlType *ctrl, UART_RxHandler handler);

/* ---------- ISR 入口 (在 stm32f1xx_it.c 中调用) ---------- */

/**
 * @brief  IDLE 中断统一处理入口
 * @note   停 DMA → 计算帧长 → 填写描述符 → 回绕检查 → 推进 In → 重启 DMA
 */
void UART_IDLE_ISR(UART_DataDrvType *drv);

/**
 * @brief  DMA TX 传输完成中断统一处理入口
 * @note
 */
void UART_Print_TX_TC_ISR(UART_PrintDrvType *drv);

void UART_Data_TX_TC_ISR(UART_DataDrvType *drv);
/* ============================================================================
 * 外部 HAL 句柄 (供宏使用)
 * ============================================================================ */
extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;
extern DMA_HandleTypeDef  hdma_usart1_tx;
extern DMA_HandleTypeDef  hdma_usart2_rx;
extern DMA_HandleTypeDef  hdma_usart2_tx;

/* ============================================================================
 * 便利宏
 * ============================================================================ */

#define UART2_TX_ZeroCopy_Send(pdata, len)  \
    UART_Data_TX_ZeroCopy(&uart2, (pdata), (len))

#endif
