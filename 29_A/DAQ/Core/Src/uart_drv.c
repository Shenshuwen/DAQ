#include "uart_drv.h"

/* ============================================================================
 * 全局变量定义
 * ============================================================================ */

/* ---------- UART1 (调试口 PA9/PA10) ---------- */
UART_PrintDrvType uart1 = {
    .huart   = &huart1,
    .hdma_tx = &hdma_usart1_tx,
    .tx_busy = 0,
};

/* ---------- UART2 (数据口 PA2/PA3) ---------- */
UART_DataDrvType uart2 = {
    .huart   = &huart2,
    .hdma_rx = &hdma_usart2_rx,
    .hdma_tx = &hdma_usart2_tx,
    .tx_busy = 0,
};

/* ============================================================================
 * 内部辅助: DMA 通道使能状态
 * ============================================================================ */
static inline void DMA_WaitDisable(DMA_HandleTypeDef *hdma){
    while (hdma->Instance->CCR & DMA_CCR_EN);
}

/* ============================================================================
 * 协议模型初始化核心
 * ============================================================================ */

/**
 * @brief  零拷贝 UART 驱动初始化 (数据口)
 * @param  drv: 数据口驱动结构体
 */
static void UART_DataDrv_InitHW(UART_DataDrvType *drv){
    /* ---- RX 描述符队列初始化 ---- */
    drv->rx_ctrl.In  = &drv->rx_ctrl.Desc[0];
    drv->rx_ctrl.Out = &drv->rx_ctrl.Desc[0];
    drv->rx_ctrl.End = &drv->rx_ctrl.Desc[CBBUFFER_SIZE - 1];
    drv->rx_ctrl.In->Start = drv->rx_buf;     /* DMA 从缓冲首地址开始接收     */
    drv->rx_ctrl.In->End   = drv->rx_buf;     /* 尚未收到数据, 占位           */

    /* ---- 使能 UART IDLE及 DMA TC 中断 ---- */
    __HAL_UART_ENABLE_IT(drv->huart, UART_IT_IDLE);
    __HAL_DMA_ENABLE_IT(drv->hdma_rx, DMA_IT_TC);
    __HAL_DMA_ENABLE_IT(drv->hdma_tx, DMA_IT_TC);
    /* ---- 配置 DMA RX ---- */
    __HAL_DMA_DISABLE(drv->hdma_rx);
    DMA_WaitDisable(drv->hdma_rx);
    /* 清除该通道所有挂起的中断标志 */
    DMA1->IFCR = (0x0FUL << drv->hdma_rx->ChannelIndex);
    /* 设置传输长度、外设地址和内存地址 */
    drv->hdma_rx->Instance->CPAR  = (uint32_t)&drv->huart->Instance->DR;
    drv->hdma_rx->Instance->CNDTR = UART2_RX_MAX + 1U;
    drv->hdma_rx->Instance->CMAR  = (uint32_t)drv->rx_buf;
    /* 启动 DMA 接收 (TX 的 DMA 在 UART_Print_RingBuffer_Send 或 UART_Data_TX_ZeroCopy 中按需启动) */
    __HAL_DMA_ENABLE(drv->hdma_rx);

    /* ---- 使能 USART DMA 接收请求 ---- */
    SET_BIT(drv->huart->Instance->CR3, USART_CR3_DMAR);
    SET_BIT(drv->huart->Instance->CR3, USART_CR3_DMAT);
}

/**
 * @brief  环形缓冲 UART 驱动初始化 (打印口)
 * @param  drv: 打印口驱动结构体
 */
static void UART_PrintDrv_InitHW(UART_PrintDrvType *drv){
    /* ---- TX 环形缓冲初始化 ---- */
    drv->ring.Head = 0;
    drv->ring.Tail = 0;

    /* ---- 使能 DMA TC 中断 ---- */
    __HAL_DMA_ENABLE_IT(drv->hdma_tx, DMA_IT_TC);
    /* ---- 使能 USART DMA 发送请求 ---- */
    SET_BIT(drv->huart->Instance->CR3, USART_CR3_DMAT);
}

/* ============================================================================
 * 初始化 API
 * ============================================================================ */

void UART1_DrvInit(void){
    UART_PrintDrv_InitHW(&uart1);
    /* 创建 TX 信号量 (缓冲有空间通知) */
    if (uart1.tx_sem == NULL) {
        uart1.tx_sem = xSemaphoreCreateBinary();
    }
}

void UART2_DrvInit(void){
    UART_DataDrv_InitHW(&uart2);
    /* 创建 RX 信号量 (帧到达通知) */
    if (uart2.rx_sem == NULL) {
        uart2.rx_sem = xSemaphoreCreateBinary();
    }
    /* 创建 TX 信号量 (DMA 完成通知, 服务于Modbus发送函数) */
    if(uart2.tx_sem == NULL) {
        uart2.tx_sem = xSemaphoreCreateBinary();
        xSemaphoreGive(uart2.tx_sem); 
    }
}

/* ============================================================================
 * RX: 零拷贝接收
 * ============================================================================ */

 /**
 * @brief  零拷贝接收
 * @param  ppdata: 接收数据缓冲区指针的指针（返回数据的地址）
 * @param  plen: 接收数据长度指针（返回数据的长度）
 * @return 1:Success, 0:Error
 * 
 * @note  调用此函数后，数据仍在DMA缓冲区中有效，用户应尽快处理数据或调用释放函数
 */
uint8_t UART_RX_ZeroCopy_Get(UART_RxCtrlType *ctrl, uint8_t **ppdata, uint16_t *plen){
    if (ctrl->In == ctrl->Out) {
        /* 队列空 */
        *ppdata = NULL;
        *plen   = 0;
        return 0;
    }

    /* 直接返回 DMA 缓冲区指针, 零拷贝 */
    *ppdata = ctrl->Out->Start;
    /* 闭区间长度: End - Start + 1 */
    *plen   = (uint16_t)(ctrl->Out->End - ctrl->Out->Start + 1U);
    return 1;
}

/**
 * @brief  释放当前零拷贝帧
 * @note   配合UART_RX_ZeroCopy_Get函数使用，用于释放当前已处理完的数据帧
 *          
 * @warning 调用此函数后，之前 Get 到的指针将不再安全
 * 
 */
void UART_RX_ZeroCopy_Release(UART_RxCtrlType *ctrl){
    if (ctrl->Out == ctrl->In) {
        return;  /* 队列空, 安全检查 */
    }

    /* 推进消费者指针 (描述符队列回绕) */
    UART_RxDescType *next = ctrl->Out + 1;
    if (next > ctrl->End) {
        next = &ctrl->Desc[0];
    }
    ctrl->Out = next;
}

// /**
//  * @brief  零拷贝接收轮询处理函数 （裸机适配）
//  * @param  ctrl: 描述符队列控制块
//  * @param  handler: 处理函数指针 
//  * @note 具体的业务逻辑处理函数实现于调用该函数的文件！
//  *          
//  */
// void UART_RX_Poll(UART_RxCtrlType *ctrl, UART_RxHandler handler){
//     uint8_t  *pdata;
//     uint16_t  len;

//     if (UART_RX_ZeroCopy_Get(ctrl, &pdata, &len)) {
//         if (handler != NULL) {
//             handler(pdata, len);
//         }
//         UART_RX_ZeroCopy_Release(ctrl);
//     }
// }

/**
 * @brief  阻塞式接收 —— 等待帧到达后处理 (RTOS 版本)
 * @param  ctrl:    描述符队列控制块
 * @param  handler: 用户回调
 * @param  timeout: 等待超时 (tick, portMAX_DELAY = 永久等待)
 * @return pdTRUE: 处理了至少一帧, pdFALSE: 超时
 * @note   内部会一次性消费所有积压帧, 然后再次阻塞等待
 */
BaseType_t UART_RX_Wait(UART_DataDrvType *drv, UART_RxHandler handler, TickType_t timeout){
    if (drv->rx_sem == NULL) {
        return pdFALSE;
    }
    /* 阻塞等待 ISR 发来的帧到达通知 */
    if (xSemaphoreTake(drv->rx_sem, timeout) != pdTRUE) {
        return pdFALSE;
    }
    /* 一次性消费所有积压帧 (避免信号量累积) */
    uint8_t  *pdata;
    uint16_t  len;
    BaseType_t processed = pdFALSE;
    while (UART_RX_ZeroCopy_Get(&drv->rx_ctrl, &pdata, &len)) {
        if (handler != NULL) {
            handler(pdata, len);
        }
        UART_RX_ZeroCopy_Release(&drv->rx_ctrl);
        processed = pdTRUE;
    }
    return processed;
}

/* ============================================================================
 * TX: 零拷贝单帧发送
 * ============================================================================ */
/**
 * @brief  零拷贝单帧发送 (可用于UART2)
 * @param  drv:   数据口驱动结构体
 * @param  pdata: 待发送数据指针 (const, 零拷贝, 必须保持有效直至 DMA 完成)
 * @param  len:   待发送数据长度 (字节)
 * @return 1:成功, 0:忙 (DMA 正在发送中，需重试)
 *
 * @warning DMA 完成前 pdata 指向的缓冲区必须保持有效，否则发送错误数据
 */
uint8_t UART_Data_TX_ZeroCopy(UART_DataDrvType *drv, const uint8_t *pdata, uint16_t len){
    if (drv->tx_busy) {
        #ifdef DEBUG
        UART1_Printf("UART_Data_TX_ZeroCopy: DMA busy, cannot send data\n");
        #endif
        return 0;  /* DMA 正在发送中, 忙 */
    }

    /* 停止  TX DMA */
    __HAL_DMA_DISABLE(drv->hdma_tx);
    DMA_WaitDisable(drv->hdma_tx);

    /* 零拷贝 — 直接使用用户缓冲区地址 */
    drv->hdma_tx->Instance->CPAR  = (uint32_t)&drv->huart->Instance->DR;
    drv->hdma_tx->Instance->CMAR  = (uint32_t)pdata;
    drv->hdma_tx->Instance->CNDTR = len;

    drv->tx_last_len = len; // 记录本次发送长度, 供 ISR 使用 ，其实可有可无，调试用
    drv->tx_busy = 1;

    /* 启动 DMA 发送 */
    __HAL_DMA_ENABLE(drv->hdma_tx);
    return 1;
}

/* ============================================================================
 * TX: 环形缓冲发送
 * ============================================================================ */
/**
 * @brief  环形缓冲发送 (可用于UART1 )
 * @note
 *         - Tail 指针不在此函数中更新
 *         - Tail 仅在 DMA TC ISR 中推进 (避免覆盖正在 DMA 搬运中的数据)
 *         - 本函数仅记录本次发送长度 (tx_last_len) 供 ISR 使用
 *         - 回绕时只发送后半段 [Tail, SIZE); ISR 链式调用发前半段 [0, Head)
 */
static void UART_Print_RingBuffer_Send(UART_PrintDrvType *drv){
    if (drv->ring.Head == drv->ring.Tail) {
        /* 无待发送数据 */
        drv->tx_busy = 0;
        return;
    }

    /* 停止当前 DMA (如果有) */
    __HAL_DMA_DISABLE(drv->hdma_tx);
    DMA_WaitDisable(drv->hdma_tx);

    /* 计算待发送长度 */
    uint16_t length;
    if (drv->ring.Head > drv->ring.Tail) {
        /* 未回绕: 连续区间 [Tail, Head) */
        length = drv->ring.Head - drv->ring.Tail;
    } else {
        /* 已回绕: 先发后半段 [Tail, SIZE), ISR 中再发前半段 [0, Head)
         * Buffer: [ D | D | ... | D | D ]
         *          0  Head    Tail  SIZE-1
         */
        length = UART1_TX_SIZE - drv->ring.Tail;
    }

    /* 记录本次发送长度 (ISR 推进 Tail 用) */
    drv->tx_last_len = length;

    /* 配置 DMA: 外设 = USART->DR, 源 = TX_Buffer[Tail], 长度 = length */
    drv->hdma_tx->Instance->CPAR  = (uint32_t)&drv->huart->Instance->DR;
    drv->hdma_tx->Instance->CMAR  = (uint32_t)&drv->ring.Buffer[drv->ring.Tail];
    drv->hdma_tx->Instance->CNDTR = length;

    drv->tx_busy = 1;
    __HAL_DMA_ENABLE(drv->hdma_tx);
}

/**
 * @brief  printf 实现 (RTOS 版本)
 * @note   满等待时阻塞在信号量上, 不烧 CPU
 *         - TX TC ISR 推进 Tail 后 Give 信号量唤醒本任务
 *         - taskENTER_CRITICAL 替代裸 __disable_irq, FreeRTOS 感知临界区
 */
static void UART_Printf_Internal(UART_PrintDrvType *drv, char *fmt, va_list args){
    char buffer[128];/* 栈上临时格式化缓冲 */
        /* 格式化 */
    uint16_t len = (uint16_t)vsnprintf(buffer, sizeof(buffer), fmt, args);

    /* 逐字节写入环形缓冲 */
    for (uint16_t i = 0U; i < len; i++) {
        uint16_t next_head = (drv->ring.Head + 1U) % UART1_TX_SIZE;

        /* 满则等待 (阻塞而非自旋) */
        while (next_head == drv->ring.Tail) {
            if (drv->tx_busy == 0) {
                taskENTER_CRITICAL();
                if (drv->tx_busy == 0) {
                    drv->tx_busy = 1;
                    UART_Print_RingBuffer_Send(drv);
                }
                taskEXIT_CRITICAL();
            }
            /* 阻塞等待 ISR 推进 Tail 并 Take 信号量 (超时 100ms 防死等) */
            if (drv->tx_sem != NULL) {
                xSemaphoreTake(drv->tx_sem, pdMS_TO_TICKS(100));
            }
        }

        drv->ring.Buffer[drv->ring.Head] = (uint8_t)buffer[i];
        drv->ring.Head = next_head;
    }
    taskENTER_CRITICAL();
    if (drv->tx_busy == 0) {
        drv->tx_busy = 1;
        UART_Print_RingBuffer_Send(drv);
    }
    taskEXIT_CRITICAL();
}

void UART1_Printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    UART_Printf_Internal(&uart1, (char *)fmt, args);
    va_end(args);
}

/* ============================================================================
 * ISR: 中断统一处理
 * ============================================================================ */

/**
 * @brief  IDLE 中断处理
 * @note   完整流程 (与 OTA_Demo USART1_IRQHandler 等价):
 *         1. 清除 IDLE 标志 (读 SR → 读 DR)
 *         2. 停止 DMA (Normal 模式, 手动重启)
 *         3. 计算帧长 = (RX_MAX+1) - DMA_CNDTR
 *         4. 填写当前描述符 (In->End)
 *         5. 描述符队列回绕检查 (next_Cb_In > End → 回到 Desc[0])
 *         6. 数据缓冲区回绕检查 (next_start + MAX > BufEnd → 回到 Buf[0])
 *         7. 碰撞检测 (追上消费者 → 丢弃)
 *         8. 推进生产者 (In = next_Cb_In), 预设下一帧 Start
 *         9. 重启 DMA (新地址 + 新计数)
 */
void UART_IDLE_ISR(UART_DataDrvType *drv){
    uint16_t rx_max = UART2_RX_MAX + 1U;

    /* ① 清除 IDLE 标志 (先读 SR 后读 DR, 硬件序列要求) */
    __HAL_UART_CLEAR_IDLEFLAG(drv->huart);

    /* ② 停止 DMA */
    __HAL_DMA_DISABLE(drv->hdma_rx);
    DMA_WaitDisable(drv->hdma_rx);

    /* 清除该 DMA 通道所有挂起的中断标志 */
    DMA1->IFCR = (0x0FUL << drv->hdma_rx->ChannelIndex);

    /* ③ 计算帧长: 初始 CNDTR=rx_max, 剩余 = GET_COUNTER, 差值 = 收到字节数 */
    uint16_t current_len = rx_max - (uint16_t)__HAL_DMA_GET_COUNTER(drv->hdma_rx);

    if (current_len > 0) {
        uint8_t buffer_wrapped = 0;

        uint8_t *current_start_ptr = drv->rx_ctrl.In->Start;
        uint8_t *current_end_ptr   = current_start_ptr + current_len - 1;
        uint8_t *next_start_ptr    = current_end_ptr + 1;

        /* ④ 描述符队列回绕检查 */
        UART_RxDescType *next_Cb_In = drv->rx_ctrl.In + 1;
        if (next_Cb_In > drv->rx_ctrl.End) {
            next_Cb_In = &drv->rx_ctrl.Desc[0];          /* 回到队首 */
        }
        if (next_Cb_In == drv->rx_ctrl.Out) {
            buffer_wrapped = 1;                    /* 描述符满，丢帧 */
        }

        /* ⑤ 数据缓冲区回绕检查 */
        uint8_t *buffer_end_ptr = drv->rx_buf + UART2_RX_SIZE - 1;
        if ((next_start_ptr + rx_max - 1) > buffer_end_ptr) {
            /* 剩余空间不足一个 DMA burst → 回绕到缓冲区头部 */
            next_start_ptr = drv->rx_buf;

            /* 碰撞检测: 回绕后是否追上消费者 */
            uint8_t *collision_check = next_start_ptr + rx_max - 1;
            if ((collision_check >= drv->rx_ctrl.Out->Start) &&
                (next_start_ptr  <= drv->rx_ctrl.Out->End)) {
                buffer_wrapped = 1;                /* 数据缓冲碰撞，丢帧 */
            }
        }

        /* ⑥ 提交帧描述符 */
        if (buffer_wrapped == 0) {
            drv->rx_ctrl.In->End   = current_end_ptr;     /* 封帧闭区间 */
            drv->rx_ctrl.In        = next_Cb_In;          /* 推进生产者 */
            drv->rx_ctrl.In->Start = next_start_ptr;      /* 预设下一帧起点 */

            /* 通知等待任务: 有新帧到达 */
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            if (drv->rx_sem != NULL) {
                xSemaphoreGiveFromISR(drv->rx_sem, &xHigherPriorityTaskWoken);
            }
            portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        }
    }

    /* ⑦ 重启 DMA */
    drv->hdma_rx->Instance->CNDTR = rx_max;
    drv->hdma_rx->Instance->CMAR  = (uint32_t)drv->rx_ctrl.In->Start;
    __HAL_DMA_ENABLE(drv->hdma_rx);
}

void UART_Print_TX_TC_ISR(UART_PrintDrvType *drv){
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    drv->ring.Tail = (drv->ring.Tail + drv->tx_last_len) % UART1_TX_SIZE;
    if (drv->ring.Head != drv->ring.Tail) {
        /* 仍有待发送数据, 链式启动下一段 DMA */
        UART_Print_RingBuffer_Send(drv);
    } else {
        /* 全部发送完毕 */
        drv->tx_busy = 0;
    }

    /* 通知等待任务: 缓冲有空间了 */
    if (drv->tx_sem != NULL) {
        xSemaphoreGiveFromISR(drv->tx_sem, &xHigherPriorityTaskWoken);
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void UART_Data_TX_TC_ISR(UART_DataDrvType *drv){
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    drv->tx_busy = 0;  /* DMA 发送完成, 清忙 */
    if(drv->tx_sem != NULL) {
        xSemaphoreGiveFromISR(drv->tx_sem, &xHigherPriorityTaskWoken);  /* 通知等待任务: DMA 发送完成 */
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
