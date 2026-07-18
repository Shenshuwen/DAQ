#include "tim_drv.h"
#include "main.h"
TIM_DrvType tim2_drv = {
    .Instance = TIM2,
    .CHX      = LL_TIM_CHANNEL_CH1
};

/* ============================================================================
 * TIM 使能
 * ============================================================================ */
/**
 * @brief  TIM 使能
 * @note  使能CHx输出，使能更新中断，启动计数器
 */
void TIM_Enable(TIM_DrvType *drv){
    LL_TIM_CC_EnableChannel(drv->Instance, drv->CHX);  // 开 PWM 输出
    LL_TIM_EnableIT_UPDATE(drv->Instance);             // 开更新中断
    LL_TIM_EnableCounter(drv->Instance);               // 启动
 }

/* ============================================================================
 * ISR 入口 
 * ============================================================================ */

/**
 * @brief  TIM2 采样完成 ISR — UEV 中断通知 Sensor_Task 读数据
 * @param  task: 目标任务句柄
 */
void TIM_Sample_ISR(TaskHandle_t task){
    if (LL_TIM_IsActiveFlag_UPDATE(TIM2)) {
        LL_TIM_ClearFlag_UPDATE(TIM2);

        if (task != NULL) {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            vTaskNotifyGiveFromISR(task, &xHigherPriorityTaskWoken);
            portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        }
    }
}
