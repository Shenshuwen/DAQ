#include "spi_drv.h"

/* ============================================================================
 * 全局变量
 * ============================================================================ */

/* TX 哑字: 静态单字, DMA 重复发送以产生 SCK 时钟 */
static const uint16_t SPI_DummyWord = 0xFFFF;

SPI_DrvTypeDef spi1_drv = {
    .Instance = SPI1,
    .rx_dma = DMA1_Channel2,
    .tx_dma = DMA1_Channel3,
    .tx_irq  = DMA1_Channel3_IRQn,
    .rx_irq  = DMA1_Channel2_IRQn,
};

/* ============================================================================
 * 内部辅助: DMA 使能
 * ============================================================================ */

 static inline void DMA_WaitDisable(DMA_HandleTypeDef *hdma){
    while (hdma->Instance->CCR & DMA_CCR_EN);
}

/* ============================================================================
 * 初始化
 * ============================================================================ */

 /**
 * @brief  SPI 驱动初始化
 * @note   配置 DMA 通道, 使能 RX TC 中断, TX DMA 仅发哑字产生 SCK, 无中断
 * @note   仅初始化 DMA, SPI 外设由 CubeMX 初始化
 * @param  drv: 数据口驱动结构体
 * @param  hdma_rx: RX DMA 通道指针
 * @param  hdma_tx: TX DMA 通道指针
 */
 void SPI_DrvInit(SPI_DrvTypeDef *drv){
    /* ---- RX DMA (DMA1_Ch2) ---- */
    drv->hdma_rx.Instance                  = drv->rx_dma;
    drv->hdma_rx.Init.Direction            = DMA_PERIPH_TO_MEMORY;
    drv->hdma_rx.Init.PeriphInc            = DMA_PINC_DISABLE;
    drv->hdma_rx.Init.MemInc               = DMA_MINC_ENABLE;
    drv->hdma_rx.Init.PeriphDataAlignment  = DMA_PDATAALIGN_HALFWORD;
    drv->hdma_rx.Init.MemDataAlignment     = DMA_MDATAALIGN_HALFWORD;
    drv->hdma_rx.Init.Mode                 = DMA_NORMAL;
    drv->hdma_rx.Init.Priority             = DMA_PRIORITY_VERY_HIGH;
    HAL_DMA_Init(&drv->hdma_rx);

    /* ---- TX DMA (DMA1_Ch3, 只发哑字产生 SCK) ---- */
    drv->hdma_tx.Instance                  = drv->tx_dma;
    drv->hdma_tx.Init.Direction            = DMA_MEMORY_TO_PERIPH;
    drv->hdma_tx.Init.PeriphInc            = DMA_PINC_DISABLE;
    drv->hdma_tx.Init.MemInc               = DMA_MINC_DISABLE;  // 固定哑字
    drv->hdma_tx.Init.PeriphDataAlignment  = DMA_PDATAALIGN_HALFWORD;
    drv->hdma_tx.Init.MemDataAlignment     = DMA_MDATAALIGN_HALFWORD;
    drv->hdma_tx.Init.Mode                 = DMA_NORMAL;
    drv->hdma_tx.Init.Priority             = DMA_PRIORITY_HIGH;
    HAL_DMA_Init(&drv->hdma_tx);

    /* ---- RX DMA TC 中断使能 ---- */
    __HAL_DMA_ENABLE_IT(&drv->hdma_rx, DMA_IT_TC);
    /* TX DMA 不使能 TC 中断 — 仅 RX 完成即整体完成 */

    /* ---- NVIC 设置    ---- */
    HAL_NVIC_SetPriority(drv->rx_irq, 7, 0);
    /* ★ 必须在 NVIC 中启用, 否则中断永不触发 ★ */
    HAL_NVIC_EnableIRQ(drv->rx_irq);   
    HAL_NVIC_SetPriority(drv->tx_irq, 7, 0);
    /* TX 中断不需要使能 (仅发哑字, 不使能 DMA_IT_TC) */

    if(drv->rx_sem == NULL) {
        drv->rx_sem = xSemaphoreCreateBinary();
    }

    drv->busy = 0;
 }

 /* ============================================================================
 * 读取
 * ============================================================================ */

 /**
 * @brief  启动 SPI DMA 读取 
 * @param  drv: 数据口驱动结构体
 * @param  buf:   用户提供的 16-bit 缓冲区
 * @param  count: 要读取的字数 (≤ SPI1_RX_MAX)
 * @return 1:成功, 0:忙 (上次传输中)
 * @note   内部并行启动 TX DMA (发哑字) + RX DMA (收数据)
 *         TX 和 RX DMA 配置相同字数, 锁步完成
 *         完成时 RX DMA TC ISR 通知
 */
 uint8_t SPI_ReadStart(SPI_DrvTypeDef *drv, uint16_t *buf, uint16_t count){
    taskENTER_CRITICAL();
    if (drv->busy) {
        taskEXIT_CRITICAL();
        return 0;  /* DMA 正在传输中, 忙 */
    }
    drv->busy = 1;
    taskEXIT_CRITICAL();

    drv->rx_buf = buf;
    drv->rx_len = count;

    /* ---- 1. 停 RX DMA, 清除挂起标志 ---- */
    __HAL_DMA_DISABLE(&drv->hdma_rx);
    DMA_WaitDisable(&drv->hdma_rx);
    DMA1->IFCR = (0x0FUL << drv->hdma_rx.ChannelIndex);

    /* ---- 2. 配置 RX DMA: 外设=SPI_DR, 内存=用户缓冲, 字长=count ---- */
    drv->hdma_rx.Instance->CPAR  = (uint32_t)&drv->Instance->DR;
    drv->hdma_rx.Instance->CMAR  = (uint32_t)buf;
    drv->hdma_rx.Instance->CNDTR = count;

    /* ---- 3. 停 TX DMA, 清除挂起标志 ---- */
    __HAL_DMA_DISABLE(&drv->hdma_tx);
    DMA_WaitDisable(&drv->hdma_tx);
    DMA1->IFCR = (0x0FUL << drv->hdma_tx.ChannelIndex);

    /* ---- 4. 配置 TX DMA: 外设=SPI_DR, 内存=哑字(固定), 字长=count ---- */
    drv->hdma_tx.Instance->CPAR  = (uint32_t)&drv->Instance->DR;
    drv->hdma_tx.Instance->CMAR  = (uint32_t)&SPI_DummyWord;
    drv->hdma_tx.Instance->CNDTR = count;

    /* ---- 5. 使能 SPI DMA 请求 ---- */
    LL_SPI_EnableDMAReq_TX(drv->Instance);
    LL_SPI_EnableDMAReq_RX(drv->Instance);

    /* ---- 6. 使能 SPI ---- */
    LL_SPI_Enable(drv->Instance);

    /* ---- 7. 启动 DMA: TX 先响应 TXE 写 DR 产生 SCK; RX 后收数据 ---- */
    __HAL_DMA_ENABLE(&drv->hdma_tx);
    __HAL_DMA_ENABLE(&drv->hdma_rx);

    return 1;  /* 成功启动 DMA 传输 */
}

/* ============================================================================
 * 阻塞等待: 等 RX 完成后获取数据指针 (RTOS)
 * ============================================================================ */

/**
 * @brief  阻塞等待 SPI DMA 读取完成
 * @param  drv: 数据口驱动结构体
 * @param  ppdata: 指向用户缓冲区指针的指针(传出参数)
 * @param  plen: 指向读取字数的指针（传出参数）
 * @param  timeout: 超时时间
 * @return pdTRUE: 成功, pdFALSE: 超时
 */
BaseType_t SPI_ReadWait(SPI_DrvTypeDef *drv, uint16_t **ppdata, uint16_t *plen, TickType_t timeout){
    if(drv->rx_sem == NULL){
        return pdFALSE;
    }
    if(xSemaphoreTake(drv->rx_sem, timeout) != pdTRUE){
        return pdFALSE;  /* 超时 */
    }
    *ppdata = drv->rx_buf;
    *plen   = drv->rx_len;
    return pdTRUE;  /* 成功获取数据指针 */
}

/* ============================================================================
 * ISR: 中断统一处理
 * ============================================================================ */

void SPI_DMARX_ISR(SPI_DrvTypeDef *drv){
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    /* ---- 1. 停止 DMA ---- */
    LL_SPI_DisableDMAReq_TX(drv->Instance);
    LL_SPI_DisableDMAReq_RX(drv->Instance);

    /* ---- 2. 清除 DMA 挂起标志 ---- */
    DMA1->IFCR = ((0x0FUL << drv->hdma_rx.ChannelIndex)|
                  (0x0FUL << drv->hdma_tx.ChannelIndex));

    /* ---- 3. 停止 SPI 外设 (停止 SCK) ---- */
    /* ★ RXONLY 模式: 必须先关 SPE 停止连续时钟, 再等 BSY 清零 ★ */
    LL_SPI_Disable(drv->Instance);
    while(LL_SPI_IsActiveFlag_BSY(drv->Instance));  /* 等待当前字节完成 */
    drv->busy = 0;  /* 标记 DMA 传输完成 */

    /* ---- 4. 通知等待任务: 有新数据到达 ---- */
    if (drv->rx_sem != NULL) {
        xSemaphoreGiveFromISR(drv->rx_sem, &xHigherPriorityTaskWoken);
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);  
    
}

