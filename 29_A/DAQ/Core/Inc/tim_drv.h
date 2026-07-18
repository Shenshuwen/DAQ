/**
 ******************************************************************************
 * @file    tim_drv.h
 * @brief   TIM  驱动 
 * @note    TIM2
 *          - CH1: PWM Generation,
 *            负责向 AD7606 发出 CONVST（转换开始）的硬件触发脉冲
 *          TIM2: PSC = 71, ARR = 99
 *          f_timer_clk = 72MHz / (71 + 1) = 1MHz      (1 tick = 1μs)
 *          f_sample    = 1MHz  / (99 + 1) = 10kHz     (周期 = 100μs)
 ******************************************************************************
 */

#ifndef __TIM_DRV_H
#define __TIM_DRV_H

#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
/* ============================================================================
 * 数据结构
 * ============================================================================ */
typedef struct {
    TIM_TypeDef *Instance;      
    uint16_t    CHX;
} TIM_DrvType;


void TIM_Enable(TIM_DrvType *drv);

extern TIM_DrvType tim2_drv;
/* ============================================================================
 * ISR 入口 
 * ============================================================================ */

void TIM_Sample_ISR(TaskHandle_t task);

#endif
