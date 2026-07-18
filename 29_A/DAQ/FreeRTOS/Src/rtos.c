#include "rtos.h"
#include "main.h"
#include "uart_drv.h"
#include "modbus.h"
#include "sys_state.h"
#include "app_config.h"
#include "tim_drv.h"

TaskHandle_t  xSensorTaskHandle = NULL;
TaskHandle_t  xCommTaskHandle = NULL;
TaskHandle_t  xMonitorTaskHandle = NULL;

volatile SysErrorFlag sys_error_flag ;
volatile uint32_t sys_sensor_heartbeat ;
volatile uint32_t sys_comm_heartbeat ;

extern IWDG_HandleTypeDef hiwdg;

/* 暂定 ------------------------------------------------------------*/
#define SENSOR_STACK_SIZE    configMINIMAL_STACK_SIZE *2
#define COMM_STACK_SIZE      configMINIMAL_STACK_SIZE *2
#define MONITOR_STACK_SIZE   configMINIMAL_STACK_SIZE *2

/* ----------------------------------------------------------------*/

void Sensor_Task(void *pvParameters){
    (void)pvParameters;
    TIM_Enable(&tim2_drv);
    for (;;){
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        Modbus_Updata();
        sys_sensor_heartbeat++;
    }
}

void Comm_Task(void *pvParameters){
    (void)pvParameters;
    for (;;){
        UART_RX_Wait(&uart2, Modbus_RxHandler, pdMS_TO_TICKS(1000));
        sys_comm_heartbeat++;
    }
}

void Monitor_Task(void *pvParameters){
    (void)pvParameters;
    for (;;){
        static uint32_t last_sensor_heartbeat = 0;
        static uint32_t last_comm_heartbeat = 0;
        #if DEBUG
        uint32_t print_drv = 0;
        #endif

        vTaskDelay(pdMS_TO_TICKS(1000));
        if(sys_sensor_heartbeat == last_sensor_heartbeat){
            sys_error_flag |= SYS_ERROR_SensorTask;
        }else{
            sys_error_flag &= ~SYS_ERROR_SensorTask;
        }
        
        if(sys_comm_heartbeat == last_comm_heartbeat){
            sys_error_flag |= SYS_ERROR_CommTask;
        }else{
            sys_error_flag &= ~SYS_ERROR_CommTask;
        }

        last_sensor_heartbeat = sys_sensor_heartbeat;
        last_comm_heartbeat = sys_comm_heartbeat;

        if(sys_error_flag == SYS_ERROR_NONE){
            HAL_IWDG_Refresh(&hiwdg);
        }else{
            #if DEBUG
            UART1_Printf("[Monitor] Error: %u\r\n", (unsigned)sys_error_flag);
            #endif
        }

        #if DEBUG
        if(++print_drv > 5){
            print_drv = 0;
            /* 栈高水位 */
            UART1_Printf("[Monitor] Sensor:%u Comm:%u Monitor:%u\r\n",
            (unsigned)uxTaskGetStackHighWaterMark(xSensorTaskHandle),
            (unsigned)uxTaskGetStackHighWaterMark(xCommTaskHandle),
            (unsigned)uxTaskGetStackHighWaterMark(xMonitorTaskHandle));

            /* 最新 ADC 原始值 */
            const uint16_t *buf = (const uint16_t *)Modbus_GetInputData();
            UART1_Printf("[Monitor] ADC: %04X %04X %04X %04X %04X %04X %04X %04X\r\n",
                buf[0], buf[1], buf[2], buf[3],
                buf[4], buf[5], buf[6], buf[7]);

            /* FreeRTOS 堆剩余 */
            UART1_Printf("[Monitor] FreeHeap:%u\r\n",(unsigned)xPortGetFreeHeapSize());
        }
        #endif

    }
}



void RTOS_Init(void){
    BaseType_t xReturned;
    xReturned = xTaskCreate(Sensor_Task, "Sensor", SENSOR_STACK_SIZE, NULL, configMAX_PRIORITIES - 1, &xSensorTaskHandle);
    if(xReturned != pdPASS)
        Error_Handler();

    xReturned = xTaskCreate(Comm_Task, "Comm", COMM_STACK_SIZE, NULL, configMAX_PRIORITIES - 2, &xCommTaskHandle);
    if(xReturned != pdPASS)
        Error_Handler();

    xReturned = xTaskCreate(Monitor_Task, "Monitor", MONITOR_STACK_SIZE, NULL, configMAX_PRIORITIES - 3, &xMonitorTaskHandle);
    if(xReturned != pdPASS)
        Error_Handler();

}
