#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/*-----------------------------------------------------------
 * 基础配置
 *----------------------------------------------------------*/
#define configUSE_PREEMPTION                    1
#define configCPU_CLOCK_HZ			( ( unsigned long ) 72000000 )
#define configTICK_RATE_HZ			( ( TickType_t ) 1000 )
#define configMAX_PRIORITIES		( 5 )
#define configMINIMAL_STACK_SIZE	( ( unsigned short ) 128 )
#define configMAX_TASK_NAME_LEN		( 16 )
#define configTICK_TYPE_WIDTH_IN_BITS   TICK_TYPE_WIDTH_32_BITS
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1

/*-----------------------------------------------------------
 * 功能配置
 *----------------------------------------------------------*/
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TASK_NOTIFICATIONS            1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           1
#define configENABLE_BACKWARD_COMPATIBILITY			0
#define configENABLE_FPU												0
#define INCLUDE_uxTaskGetStackHighWaterMark     1

/*-----------------------------------------------------------
 * 内存管理配置
 * 10kb
 *----------------------------------------------------------*/
#define configTOTAL_HEAP_SIZE       ( ( size_t ) ( 10 * 1024 ) )

/*-----------------------------------------------------------
 * 调试配置
 *----------------------------------------------------------*/
#define configUSE_TRACE_FACILITY	0
#define configUSE_QUEUE_SETS                    0
#define configQUEUE_REGISTRY_SIZE               8
#define configASSERT_DEFINED                    1
#define configUSE_STATS_FORMATTING_FUNCTIONS    0
#define INCLUDE_uxTaskGetStackHighWaterMark     1

/*-----------------------------------------------------------
 * 定时器配置
 *----------------------------------------------------------*/
#define configUSE_TIMERS                        0
#define configGENERATE_RUN_TIME_STATS            0

 /*-----------------------------------------------------------
 * API函数配置
 *----------------------------------------------------------*/
#define INCLUDE_vTaskPrioritySet                0
#define INCLUDE_uxTaskPriorityGet               0
#define INCLUDE_vTaskDelete                     0
#define INCLUDE_vTaskCleanUpResources           0
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTimerPendFunctionCall          0
#define INCLUDE_eTaskGetState                   0

 /*-----------------------------------------------------------
 * 中断配置
 *----------------------------------------------------------*/
#define configKERNEL_INTERRUPT_PRIORITY          240  /* 0xF0, 最低优先级 (仅高4位有效) */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY     80  /* 0x50, ST库优先级11 (5<<4=80) */

 /*-----------------------------------------------------------
 * 断言映射
 *----------------------------------------------------------*/
#include "stm32_assert.h"
#define configASSERT( x )    \
    if( ( x ) == 0 )         \
    {                        \
        taskDISABLE_INTERRUPTS(); \
        for( ;; );           \
    }

 /*-----------------------------------------------------------
 * 钩子函数
 *----------------------------------------------------------*/
#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      0
#define configUSE_MALLOC_FAILED_HOOK             0

#endif /* FREERTOS_CONFIG_H */
