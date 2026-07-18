# DAQ 项目分析日志 —— FreeRTOS 移植到 STM32F103C8T6

> 生成日期: 2026-06-29
> 目标平台: STM32F103C8T6 (Cortex-M3, 72MHz, 64KB Flash / 20KB SRAM)
> 工具链: Keil MDK-ARM (ARMCC)
> 当前阶段: FreeRTOS 移植准备

---

## 一、项目目录结构总览

```
DAQ/
├── DAQ.ioc                         # STM32CubeMX 项目配置文件
├── .mxproject                      # CubeMX 项目元数据
│
├── Core/                           # ★ 用户应用层代码 (CubeMX 生成 + 用户自定义)
│   ├── Inc/                        # 头文件
│   │   ├── main.h                  # 主程序头文件 (引脚宏定义等)
│   │   ├── stm32f1xx_hal_conf.h   # HAL 库模块配置
│   │   ├── stm32f1xx_it.h         # 中断服务函数声明
│   │   ├── stm32_assert.h         # 断言配置
│   │   ├── gpio.h / dma.h / spi.h / tim.h / usart.h  # 各外设初始化头文件
│   └── Src/                        # 源文件
│       ├── main.c                  # ★ 主程序入口 (当前为裸机 while(1) 循环)
│       ├── stm32f1xx_it.c          # ★ 中断服务函数 (SVC/PendSV/SysTick 均为空壳)
│       ├── stm32f1xx_hal_msp.c     # HAL 外设 MSP 初始化
│       ├── system_stm32f1xx.c      # 系统初始化
│       ├── gpio.c / dma.c / spi.c / tim.c / usart.c  # 各外设初始化
│
├── Drivers/                        # ★ STM32 官方驱动层
│   ├── CMSIS/                      # ARM Cortex-M 核心支持
│   │   ├── Include/                # CMSIS-Core 头文件 (core_cm3.h 等)
│   │   └── Device/ST/STM32F1xx/
│   │       ├── Include/            # 器件头文件 (stm32f103xb.h, stm32f1xx.h)
│   │       └── Source/Templates/   # 启动文件模板 (gcc/iar/arm)
│   └── STM32F1xx_HAL_Driver/      # STM32F1 HAL 库
│       ├── Inc/                    # HAL 头文件 + LL 头文件
│       │   └── Legacy/             # 旧版兼容头文件
│       └── Src/                    # HAL 源文件 + LL 源文件
│
├── FreeRTOS/                       # ★ FreeRTOS 内核 (V11.1.0) — 已放入但未集成
│   ├── Inc/                        # 内核头文件
│   │   ├── FreeRTOS.h              # 内核主头文件
│   │   ├── FreeRTOSConfig.h        # ★ 内核配置文件 (版本不匹配，详见下文)
│   │   ├── task.h / queue.h / semphr.h / list.h
│   │   ├── atomic.h / portable.h / projdefs.h / stack_macros.h
│   ├── Src/                        # 内核源文件
│   │   ├── tasks.c                 # 任务调度核心
│   │   ├── queue.c                 # 队列管理
│   │   └── list.c                  # 链表数据结构
│   └── ARM_CM3/                    # Cortex-M3 移植层
│       ├── port.c                  # ★ 移植层实现 (SVC/PendSV/SysTick 处理)
│       ├── portmacro.h             # 移植层宏定义 (临界区/上下文切换)
│       └── heap_4.c               # 内存管理 (最佳匹配算法)
│
├── MDK-ARM/                        # ★ Keil MDK 工程目录
│   ├── DAQ.uvprojx                 # Keil 工程文件
│   ├── DAQ.uvoptx                  # Keil 工程选项
│   └── startup_stm32f103xb.s      # ★ 汇编启动文件 (向量表 + Reset_Handler)
│
└── 备用资料/                        # 参考资料
    └── FreeRTOS-202212.00/         # FreeRTOS 官方发布包 (2022年12月版)
        ├── FreeRTOS/               # 内核源码 + Demo
        └── FreeRTOS-Plus/          # 附加组件 (TCP/UDP/FAT等)
```

---

## 二、逻辑框架分析

### 2.1 硬件抽象层 (HAL) 架构

```
┌─────────────────────────────────────┐
│         用户应用程序 (main.c)         │  ← 业务逻辑
├─────────────────────────────────────┤
│         HAL 外设驱动 API             │  ← stm32f1xx_hal_*.c
├─────────────────────────────────────┤
│     CMSIS-Core (Cortex-M3 接口)      │  ← core_cm3.h, NVIC, SysTick
├─────────────────────────────────────┤
│     启动文件 + 向量表 (startup.s)     │  ← 汇编级硬件入口
└─────────────────────────────────────┘
```

### 2.2 当前外设资源配置 (来自 DAQ.ioc / main.c)

| 外设    | 功能      | DMA 通道      | 中断     |
|---------|-----------|---------------|----------|
| SPI1    | SPI 通信  | -             | -        |
| USART1  | 串口1     | TX: DMA1_CH4  | 已使能   |
| USART2  | 串口2     | RX: DMA1_CH6, TX: DMA1_CH7 | 已使能 |
| TIM2    | 定时器2   | -             | 已使能   |
| GPIO    | CVA(PA0), CS(PA1) | -      | -        |

### 2.3 系统时钟配置

- **HSE**: 外部高速晶振 (8MHz typ.) → PLL ×9 → SYSCLK = **72MHz**
- **AHB**: SYSCLK /1 = 72MHz
- **APB1**: HCLK /2 = 36MHz (TIM2 等定时器时钟 = 72MHz 因倍频)
- **APB2**: HCLK /1 = 72MHz
- **Flash Wait State**: 2 (FLASH_LATENCY_2)

### 2.4 FreeRTOS 目标架构

移植完成后的系统架构:

```
┌──────────────────────────────────────────┐
│  FreeRTOS 任务层                          │
│  ├── 默认任务 (TASK1)                     │
│  ├── 数据采集任务 (TASK2)                  │
│  └── ...                                 │
├──────────────────────────────────────────┤
│  FreeRTOS 内核                            │
│  ├── 任务调度器 (tasks.c)                  │
│  ├── 队列通信 (queue.c)                    │
│  ├── 内存管理 (heap_4.c)                   │
│  └── Cortex-M3 移植层 (port.c)            │
│      ├── SVC_Handler   → 启动第一个任务    │
│      ├── PendSV_Handler → 上下文切换       │
│      └── SysTick_Handler → 系统滴答时钟    │
├──────────────────────────────────────────┤
│  HAL 外设驱动层 + CMSIS                   │
└──────────────────────────────────────────┘
```

---

## 三、FreeRTOS 移植现状诊断

### 3.1 ✅ 已完成的工作

| 项目                    | 状态 | 说明                                  |
|-------------------------|------|---------------------------------------|
| FreeRTOS 内核源文件     | ✅   | tasks.c, queue.c, list.c 已放入 Src/  |
| FreeRTOS 头文件         | ✅   | 已放入 Inc/                           |
| Cortex-M3 移植层        | ✅   | port.c, portmacro.h 已放入 ARM_CM3/  |
| 内存管理                | ✅   | heap_4.c 已放入 ARM_CM3/            |
| FreeRTOSConfig.h        | ✅   | 配置文件已创建 (但存在问题，见下文)     |

### 3.2 ❌ 必须解决的六个问题

#### 问题 1: 版本不兼容 —— FreeRTOSConfig.h 与内核版本不匹配

| 项目            | 版本          |
|-----------------|---------------|
| FreeRTOS 内核   | **V11.1.0**   |
| FreeRTOSConfig.h| 来自 **202212.00** 版 (旧版本) |

**具体不兼容项**:
- `FreeRTOSConfig.h` 使用 `configUSE_16_BIT_TICKS` (旧宏)，V11.1.0 期望 `configTICK_TYPE_WIDTH_IN_BITS`
- 缺少 V11.1.0 新增的配置项 (如 `configTICK_TYPE_WIDTH_IN_BITS`)

从 `portmacro.h:62-67` 可以看到 V11.1.0 的期望写法:
```c
#if ( configTICK_TYPE_WIDTH_IN_BITS == TICK_TYPE_WIDTH_16_BITS )
    typedef uint16_t TickType_t;
#elif ( configTICK_TYPE_WIDTH_IN_BITS == TICK_TYPE_WIDTH_32_BITS )
    typedef uint32_t TickType_t;
```

#### 问题 2: 中断服务函数冲突 (最关键的阻塞问题)

`Core/Src/stm32f1xx_it.c` 中定义了以下 ISR，与 FreeRTOS `port.c` 产生冲突:

| ISR 名称           | stm32f1xx_it.c 状态 | FreeRTOS port.c 状态  |
|--------------------|---------------------|-----------------------|
| `SVC_Handler`      | 空壳 (无实质逻辑)    | `vPortSVCHandler` 实现 |
| `PendSV_Handler`   | 空壳 (无实质逻辑)    | `xPortPendSVHandler` 实现 |
| `SysTick_Handler`  | 调用 HAL_IncTick()  | `xPortSysTickHandler` 实现 |

**冲突机制**: Keil 链接器会使用 `stm32f1xx_it.c` 中的强定义，而忽略 `port.c` 中的定义，导致 FreeRTOS 调度器完全无法工作。

#### 问题 3: main.c 尚未集成 FreeRTOS

当前 `main.c` 仍然是裸机结构:
```c
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    // ... 外设初始化 ...
    while (1) { }    // ← 裸机死循环
}
```

需要改为 FreeRTOS 多任务模式。

#### 问题 4: Keil 工程可能未添加 FreeRTOS 源文件

需要确认 `DAQ.uvprojx` 工程中是否已添加:
- `FreeRTOS/Src/tasks.c`
- `FreeRTOS/Src/queue.c`
- `FreeRTOS/Src/list.c`
- `FreeRTOS/ARM_CM3/port.c`
- `FreeRTOS/ARM_CM3/heap_4.c`

以及 Include Paths 是否包含:
- `FreeRTOS/Inc`
- `FreeRTOS/ARM_CM3`

#### 问题 5: 堆栈大小配置

`startup_stm32f103xb.s` 中的当前配置:
- Stack Size: **0x400** (1KB)
- Heap Size: **0x200** (512B)

⚠️ **警告**: `configTOTAL_HEAP_SIZE = 17 * 1024 = 17408` 字节，但 STM32F103C8T6 仅有 20KB SRAM。17KB 堆 + 1KB 系统栈 = 18KB，剩余仅 2KB 给静态数据和任务 TCB。**很可能导致内存溢出**。建议减小到 **10KB ~ 12KB**。

#### 问题 6: FreeRTOSConfig.h 中断优先级配置

```c
#define configKERNEL_INTERRUPT_PRIORITY          255    // 原始 NVIC 值 (最低优先级)
#define configMAX_SYSCALL_INTERRUPT_PRIORITY     191    // 0xb0, 即优先级 11
#define configLIBRARY_KERNEL_INTERRUPT_PRIORITY  15     // ST 库优先级 (0-15)
```

这要求 `NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4)` (即 4 位全用于抢占优先级)。需要在 HAL_Init() 之后确认优先级分组设置。

---

## 四、六个问题的详细解决方案

---

### 问题 1: FreeRTOSConfig.h 版本不兼容 —— 完整修正方案

#### 根因分析

`FreeRTOS/Inc/FreeRTOSConfig.h` 第 2 行声明版本为 `V202212.00`（旧命名），而内核文件 `FreeRTOS.h`、`portmacro.h` 均为 `V11.1.0`。关键证据链：

**① 旧版 config.h (第52行)**:
```c
//#define configUSE_16_BIT_TICKS     0    // 已被注释掉，但是旧版宏
```
此宏在 V11.1.0 中**已被移除**，不再有效。

**② portmacro.h (第62-74行) 使用新版宏来判断 TickType_t 宽度**:
```c
#if ( configTICK_TYPE_WIDTH_IN_BITS == TICK_TYPE_WIDTH_16_BITS )
    typedef uint16_t     TickType_t;
#elif ( configTICK_TYPE_WIDTH_IN_BITS == TICK_TYPE_WIDTH_32_BITS )
    typedef uint32_t     TickType_t;
#else
    #error configTICK_TYPE_WIDTH_IN_BITS set to unsupported tick type width.
#endif
```
**如果 `configTICK_TYPE_WIDTH_IN_BITS` 未定义，编译将报错 `#error`。**

**③ port.c (第100行) 引用 `configCPU_CLOCK_HZ`**:
```c
#define configSYSTICK_CLOCK_HZ    ( configCPU_CLOCK_HZ )
```
当前 `FreeRTOSConfig.h` 第45行的宏名是 `configCPU_CLOCK_HZ` — 拼写为 C-L-O-C-K，这是**正确拼写**。此项无需修改。

**④ V11.1.0 新增了部分可选配置项的默认值依赖**:
- `configUSE_TICKLESS_IDLE` — 未定义则 `port.c` 中 tickless 代码段被 `#if` 排除（安全）
- `configASSERT` — 未定义则 assert 相关代码不编译（安全）

#### 修正操作: 完整替换 FreeRTOSConfig.h

将 `FreeRTOS/Inc/FreeRTOSConfig.h` 替换为以下 V11.1.0 兼容版本。核心变更:
1. 用 `configTICK_TYPE_WIDTH_IN_BITS` 替代已注释的 `configUSE_16_BIT_TICKS`
2. 添加 `configUSE_TICKLESS_IDLE` (明确禁用，省 RAM)
3. 添加 `configCHECK_FOR_STACK_OVERFLOW` (调试用)
4. 添加 `configASSERT` 定义 (映射到 HAL 的 assert)
5. 添加 `configUSE_MUTEXES`、`configUSE_COUNTING_SEMAPHORES` 等常用功能
6. 设置 `configTOTAL_HEAP_SIZE` 为 10KB (适配 20KB SRAM)
7. 内存估算注释

**替换后的完整文件内容**:

```c
/*
 * FreeRTOS Kernel V11.1.0
 * 配置文件 — 适配 STM32F103C8T6 (Cortex-M3, 72MHz, 20KB SRAM)
 * 生成日期: 2026-06-29
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/*-----------------------------------------------------------
 * 基础配置
 *----------------------------------------------------------*/
#define configUSE_PREEMPTION                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1  /* Cortex-M3 有 CLZ 指令, 启用硬件优化 */
#define configUSE_TICKLESS_IDLE                 0  /* 禁用 tickless, 节省 RAM, 简化调试 */
#define configCPU_CLOCK_HZ                      ( ( unsigned long ) 72000000 )
#define configTICK_RATE_HZ                      ( ( TickType_t ) 1000 )
#define configMAX_PRIORITIES                    ( 5 )
#define configMINIMAL_STACK_SIZE                ( ( unsigned short ) 128 )
#define configMAX_TASK_NAME_LEN                 ( 16 )
#define configUSE_16_BIT_TICKS                  0  /* 保留兼容旧工具, 实际由下一行决定 */
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS  /* ★ V11.1.0 新宏 */
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TASK_NOTIFICATIONS            1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               8
#define configUSE_QUEUE_SETS                    0

/*-----------------------------------------------------------
 * 内存分配
 *
 * STM32F103C8T6 仅有 20KB SRAM. 内存预算:
 *   ├── 系统栈 (startup.s)            : 1KB  (0x400)
 *   ├── 标准库堆 (startup.s, 可选)     : 0.5KB (0x200)
 *   ├── FreeRTOS 内核堆 (heap_4.c)    : 10KB (configTOTAL_HEAP_SIZE)
 *   │   ├── TCB × N tasks              : ~0.1KB × N
 *   │   ├── 任务栈 × N tasks           : configMINIMAL_STACK_SIZE × N
 *   │   └── 队列/信号量/软件定时器      : 按需分配
 *   ├── 静态数据 (.data + .bss)        : ~2-3KB (由链接器决定)
 *   └── 剩余余量                       : ~4-6KB
 *
 * 如果任务较多或栈需求大, 可将此处减小到 8192 (8KB).
 *----------------------------------------------------------*/
#define configTOTAL_HEAP_SIZE                    ( ( size_t ) ( 10 * 1024 ) )

/*-----------------------------------------------------------
 * 调试与跟踪
 *----------------------------------------------------------*/
#define configUSE_TRACE_FACILITY                0
#define configUSE_STATS_FORMATTING_FUNCTIONS    0
#define configCHECK_FOR_STACK_OVERFLOW          2  /* 方法2: 检查栈顶水印标记 */
#define configASSERT_DEFINED                    1  /* 启用断言 */

/*-----------------------------------------------------------
 * 软件定时器
 *----------------------------------------------------------*/
#define configUSE_TIMERS                        0  /* 暂不使用, 按需开启 */
#define configTIMER_TASK_PRIORITY               ( 2 )
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            ( configMINIMAL_STACK_SIZE * 2 )

/*-----------------------------------------------------------
 * 可选 API 函数包含
 *----------------------------------------------------------*/
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskCleanUpResources           0
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1  /* 方案B需要此函数 */
#define INCLUDE_xTimerPendFunctionCall          0
#define INCLUDE_eTaskGetState                   0

/*-----------------------------------------------------------
 * Cortex-M3 中断优先级配置 (FreeRTOS 移植层核心)
 *
 * STM32F1xx 使用 4 位优先级 (16 级, 0=最高, 15=最低).
 * FreeRTOS 要求所有优先级位用作抢占优先级 (即 NVIC_PriorityGroup_4).
 *
 * NVIC 原始值 = ST库值 << 4
 *   例: ST库优先级 15 → NVIC 值 240 (0xF0)
 *   例: ST库优先级 11 → NVIC 值 176 (0xB0)
 *
 * configKERNEL_INTERRUPT_PRIORITY (255):
 *   设置 PendSV 和 SysTick 为最低优先级, 确保它们不会抢占其他中断.
 *
 * configMAX_SYSCALL_INTERRUPT_PRIORITY (191 = 0xBF):
 *   等于 ST 库优先级 11. 只有优先级 >=11 (数值上) 的中断才能调用
 *   FreeRTOS 的 FromISR API. 优先级 0~10 的中断不能调用 FreeRTOS API.
 *
 * configLIBRARY_KERNEL_INTERRUPT_PRIORITY (15):
 *   等于 ST 库最低优先级. 此宏并非 FreeRTOS 官方宏, 仅 HAL 库使用.
 *   必须与 configKERNEL_INTERRUPT_PRIORITY 的 ST 库等价值一致.
 *----------------------------------------------------------*/
#define configKERNEL_INTERRUPT_PRIORITY          255  /* NVIC 原始值, 最低优先级 */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY     191  /* 0xBF, ST库优先级11 */
#define configLIBRARY_KERNEL_INTERRUPT_PRIORITY  15   /* ST库最低优先级 */

/*-----------------------------------------------------------
 * 断言映射 — 将 FreeRTOS assert 映射到 HAL 或自定义处理
 *
 * 当 configASSERT_DEFINED=1 时, FreeRTOS 会在关键位置调用 configASSERT(x).
 * 此处将其映射到死循环 (调试时可改为 BKPT 指令触发调试器断点).
 *----------------------------------------------------------*/
#include "stm32_assert.h"  /* 复用 STM32 的 assert_param 机制 */
#define configASSERT( x )    \
    if( ( x ) == 0 )         \
    {                        \
        taskDISABLE_INTERRUPTS(); \
        for( ;; );           \
    }

/*-----------------------------------------------------------
 * FreeRTOS 钩子函数 (Hook Functions)
 *----------------------------------------------------------*/
#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      0
#define configUSE_MALLOC_FAILED_HOOK             0

/*-----------------------------------------------------------
 * Run-time stats (运行时统计, 需要额外定时器)
 *----------------------------------------------------------*/
#define configGENERATE_RUN_TIME_STATS            0

/*-----------------------------------------------------------
 * 协程 (Co-routines) — 已弃用, 不启用
 *----------------------------------------------------------*/
#define configUSE_CO_ROUTINES                    0
#define configMAX_CO_ROUTINE_PRIORITIES          ( 2 )

#endif /* FREERTOS_CONFIG_H */
```

---

### 问题 2: ISR 冲突 —— 三种方案详解

#### 根因分析

启动文件 `startup_stm32f103xb.s` (第164-180行) 中 SVC_Handler、PendSV_Handler、SysTick_Handler 均声明为 `[WEAK]`:

```asm
SVC_Handler     PROC
                EXPORT  SVC_Handler                [WEAK]
                B       .
                ENDP
PendSV_Handler  PROC
                EXPORT  PendSV_Handler             [WEAK]
                B       .
                ENDP
SysTick_Handler PROC
                EXPORT  SysTick_Handler            [WEAK]
                B       .
                ENDP
```

`[WEAK]` 意味着: **如果其他 .c 文件定义了同名的强符号函数, 则链接器使用强符号, 丢弃此弱符号**。

但 `stm32f1xx_it.c` 中定义了**非 WEAK 的强符号**版本:
- `void SVC_Handler(void)` (第148行, 空壳)
- `void PendSV_Handler(void)` (第174行, 空壳)
- `void SysTick_Handler(void)` (第187行, 调用 HAL_IncTick())

FreeRTOS `port.c` 中定义了:
- `__asm void vPortSVCHandler(void)` — **函数名不同**, 不是 `SVC_Handler`
- `__asm void xPortPendSVHandler(void)` — **函数名不同**, 不是 `PendSV_Handler`
- `void xPortSysTickHandler(void)` — **函数名不同**, 不是 `SysTick_Handler`

**关键发现**: FreeRTOS ARM_CM3 port.c 使用的是自定义函数名 (vPortSVCHandler / xPortPendSVHandler / xPortSysTickHandler), 而不是 CMSIS 标准 ISR 名称。这意味着 FreeRTOS 期望用户在 ISR 中**手动转发**到 port 层函数。

**当前问题**:
- `SVC_Handler` 在 `stm32f1xx_it.c` 中是空壳 → FreeRTOS 的 `prvStartFirstTask()` 通过 `svc 0` 指令触发 SVC, 但 SVC_Handler 是空函数, 直接返回 → **第一个任务永远无法启动**
- `PendSV_Handler` 是空壳 → 上下文切换永远无法执行 → **任务切换失败**
- `SysTick_Handler` 仅调用 `HAL_IncTick()`, 未调用 `xPortSysTickHandler()` → **FreeRTOS 内核 tick 永远不会递增** → 调度器永远不会切换任务

#### 解决方案对比

| 方案 | 修改文件 | 优点 | 缺点 |
|------|---------|------|------|
| **A (推荐)** | stm32f1xx_it.c + stm32f1xx_it.h | 干净分离, FreeRTOS 完全接管 | 需同时修改 .c 和 .h |
| **B** | stm32f1xx_it.c | 保留 CubeMX 代码结构 | SysTick_Handler 仍需手动修改 |
| **C** | stm32f1xx_it.c + FreeRTOSConfig.h | 利用 port.c 的 [WEAK] 机制 | 依赖编译器行为, 可移植性差 |

#### 方案 A (推荐): 在 stm32f1xx_it.c 中转发 ISR 到 FreeRTOS port 函数

**步骤 1**: 在 `stm32f1xx_it.c` 顶部添加 FreeRTOS 头文件:

```c
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "FreeRTOS.h"
#include "task.h"
/* USER CODE END Includes */
```

**步骤 2**: 修改 `stm32f1xx_it.c` 中的 SysTick_Handler (第187-196行), 将:

```c
void SysTick_Handler(void)
{
  /* USER CODE BEGIN SysTick_IRQn 0 */

  /* USER CODE END SysTick_IRQn 0 */
  HAL_IncTick();
  /* USER CODE BEGIN SysTick_IRQn 1 */

  /* USER CODE END SysTick_IRQn 1 */
}
```

替换为:

```c
void SysTick_Handler(void)
{
  /* USER CODE BEGIN SysTick_IRQn 0 */

  /* USER CODE END SysTick_IRQn 0 */
  HAL_IncTick();
#if (INCLUDE_xTaskGetSchedulerState == 1)
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
  {
    xPortSysTickHandler();
  }
#endif
  /* USER CODE BEGIN SysTick_IRQn 1 */

  /* USER CODE END SysTick_IRQn 1 */
}
```

**步骤 3**: 修改 `stm32f1xx_it.c` 中的 PendSV_Handler (第174-182行), 将空壳替换为:

```c
void PendSV_Handler(void)
{
  /* USER CODE BEGIN PendSV_IRQn 0 */
  xPortPendSVHandler();
  /* USER CODE END PendSV_IRQn 0 */
  /* USER CODE BEGIN PendSV_IRQn 1 */

  /* USER CODE END PendSV_IRQn 1 */
}
```

**步骤 4**: 修改 `stm32f1xx_it.c` 中的 SVC_Handler (第148-156行), 将空壳替换为:

```c
void SVC_Handler(void)
{
  /* USER CODE BEGIN SVCall_IRQn 0 */
  vPortSVCHandler();
  /* USER CODE END SVCall_IRQn 0 */
  /* USER CODE BEGIN SVCall_IRQn 1 */

  /* USER CODE END SVCall_IRQn 1 */
}
```

**步骤 5**: 在 `stm32f1xx_it.h` 顶部添加 FreeRTOS 头文件引用:

```c
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "FreeRTOS.h"
#include "task.h"
/* USER CODE END Includes */
```

**重要**: 所有修改都在 `USER CODE` 区域内, 确保 CubeMX 重新生成代码时不会丢失修改。

#### 方案 B: 仅修改 stm32f1xx_it.c (最小改动)

与方案 A 相同, 但 CubeMX 重新生成时可能覆盖 USER CODE 区域。此方案适合不打算再次使用 CubeMX 生成代码的情况。

#### 方案 C: 利用 port.c 中函数的 [WEAK] 属性

查看 port.c: `vPortSetupTimerInterrupt` 使用了 `__weak` 属性 (第701行), 但 `vPortSVCHandler`、`xPortPendSVHandler`、`xPortSysTickHandler` **没有** `__weak` 属性。因此此方案不可行。必须使用方案 A 或 B。

**最终推荐: 方案 A**, 这是 ST 官方 FreeRTOS 例程中的标准做法。

---

### 问题 3: main.c 集成 FreeRTOS —— 完整代码方案

#### 改造前 (当前 main.c):
```c
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init(); MX_DMA_Init();
    MX_SPI1_Init(); MX_USART1_UART_Init();
    MX_USART2_UART_Init(); MX_TIM2_Init();
    while (1) { }   // ← 裸机死循环
}
```

#### 改造后 (FreeRTOS 版本):

```c
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define TASK1_STACK_SIZE    (configMINIMAL_STACK_SIZE * 2)
#define TASK2_STACK_SIZE    (configMINIMAL_STACK_SIZE * 2)
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
TaskHandle_t  xTask1Handle = NULL;
TaskHandle_t  xTask2Handle = NULL;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static void vTask1_Func(void *pvParameters);
static void vTask2_Func(void *pvParameters);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/**
  * @brief  任务1: 示例 — LED 闪烁 / 传感器轮询
  * @param  pvParameters: 任务参数 (未使用)
  * @retval None
  */
static void vTask1_Func(void *pvParameters)
{
    /* 移除编译器警告 */
    (void)pvParameters;

    for (;;)
    {
        /* TODO: 在此添加实际功能代码 */
        /* 例如: HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin); */
        vTaskDelay(pdMS_TO_TICKS(500));  /* 延时 500ms */
    }
}

/**
  * @brief  任务2: 示例 — 数据处理 / 通信
  * @param  pvParameters: 任务参数 (未使用)
  * @retval None
  */
static void vTask2_Func(void *pvParameters)
{
    (void)pvParameters;

    for (;;)
    {
        /* TODO: 在此添加实际功能代码 */
        /* 例如: 通过 USART 发送数据, 读取 ADC 等 */
        vTaskDelay(pdMS_TO_TICKS(1000));  /* 延时 1000ms */
    }
}

/* USER CODE END 0 */

/**
  * @brief  应用程序入口
  * @retval int
  */
int main(void)
{
    /* USER CODE BEGIN 1 */
    /* 可选: 设置中断优先级分组 (必须在 HAL_Init 之前或之后立即执行) */
    /* NVIC_SetPriorityGrouping(0);  // 0 = 4位全用于抢占优先级 */
    /* USER CODE END 1 */

    /* MCU Configuration--------------------------------------------------------*/
    HAL_Init();

    /* USER CODE BEGIN Init */

    /* USER CODE END Init */

    /* Configure the system clock */
    SystemClock_Config();

    /* USER CODE BEGIN SysInit */

    /* USER CODE END SysInit */

    /* Initialize all configured peripherals */
    MX_GPIO_Init();
    MX_DMA_Init();
    MX_SPI1_Init();
    MX_USART1_UART_Init();
    MX_USART2_UART_Init();
    MX_TIM2_Init();

    /* USER CODE BEGIN 2 */
    /* 创建 FreeRTOS 任务 */
    BaseType_t xReturned = pdPASS;

    xReturned = xTaskCreate(
        vTask1_Func,                /* 任务函数 */
        "Task1",                    /* 任务名称 (调试用) */
        TASK1_STACK_SIZE,           /* 栈大小 (words, 非 bytes) */
        NULL,                       /* 任务参数 */
        1,                          /* 优先级 (0=最低) */
        &xTask1Handle               /* 任务句柄 (可选) */
    );
    if (xReturned != pdPASS)
    {
        /* 任务创建失败 — 进入错误处理 */
        Error_Handler();
    }

    xReturned = xTaskCreate(
        vTask2_Func,
        "Task2",
        TASK2_STACK_SIZE,
        NULL,
        2,                          /* 优先级高于 Task1 */
        &xTask2Handle
    );
    if (xReturned != pdPASS)
    {
        Error_Handler();
    }

    /* 启动 FreeRTOS 调度器 — 此函数永不返回 */
    vTaskStartScheduler();
    /* USER CODE END 2 */

    /* 理论上永远不会执行到这里. 如果执行到了, 说明堆内存不足导致空闲任务创建失败. */
    /* Infinite loop */
    /* USER CODE BEGIN WHILE */
    while (1)
    {
        /* USER CODE END WHILE */
        /* USER CODE BEGIN 3 */
    }
    /* USER CODE END 3 */
}
```

**关键说明**:
1. 所有 FreeRTOS 代码写在 `USER CODE` 区域内, **CubeMX 重新生成时不会丢失**
2. `vTaskStartScheduler()` 成功后永不返回, 其后的 while(1) 仅为安全兜底
3. 任务栈大小单位是 **words** (4字节), 不是 bytes. `configMINIMAL_STACK_SIZE=128` 即每个任务最少 512 bytes 栈
4. `vTaskDelay(pdMS_TO_TICKS(500))` 是 FreeRTOS 延时, 使任务进入阻塞态, 释放 CPU 给其他任务

---

### 问题 4: Keil MDK 工程配置

#### 操作步骤 (在 Keil uVision5 中手动操作)

**Step 4.1: 添加 FreeRTOS 源文件到工程组**

1. 打开 `MDK-ARM/DAQ.uvprojx`
2. 在 Project 窗口中右键 `Target 1` → `Add Group...`
3. 创建两个新 Group:
   - `FreeRTOS/Kernel` — 内核文件
   - `FreeRTOS/Port` — 移植层文件
4. 向 `FreeRTOS/Kernel` 添加:
   - `..\FreeRTOS\Src\tasks.c`
   - `..\FreeRTOS\Src\queue.c`
   - `..\FreeRTOS\Src\list.c`
5. 向 `FreeRTOS/Port` 添加:
   - `..\FreeRTOS\ARM_CM3\port.c`
   - `..\FreeRTOS\ARM_CM3\heap_4.c`

**Step 4.2: 添加 Include Paths**

菜单 `Project` → `Options for Target 'Target 1'` → `C/C++` 选项卡 → `Include Paths` 添加:

```
..\FreeRTOS\Inc
..\FreeRTOS\ARM_CM3
```

**Step 4.3: 确保 ARM Compiler 设置支持 FreeRTOS**

在 `C/C++` 选项卡:
- `Optimization`: `-O1` 或 `-O0` (调试阶段)
- **取消勾选** `Use MicroLIB` — 如果使用了 MicroLIB, `heap_4.c` 完全独立实现内存管理, 不调用 malloc, 因此 MicroLIB 也是兼容的。可以保持勾选状态。

**Step 4.4: 验证编译顺序**

编译时应按以下顺序 (由 Keil 自动管理依赖):
1. HAL 库 + CMSIS
2. FreeRTOS 内核 (tasks.c → queue.c → list.c)
3. FreeRTOS 移植层 (port.c → heap_4.c)
4. 用户代码 (main.c, stm32f1xx_it.c 等)

如遇到 `undefined symbol` 错误, 检查 FreeRTOS 源文件是否全部加入工程。

---

### 问题 5: 内存配置 —— 详细计算

#### 5.1 STM32F103C8T6 内存布局

```
SRAM 总容量: 20KB (0x20000000 ~ 0x20005000)

┌──────────────────────────────┐ 0x20005000 (SRAM 顶部)
│    系统栈 (MSP, startup.s)    │ Stack_Size = 0x400 (1KB)
│    - main() 函数调用           │
│    - 中断嵌套的寄存器保存       │ ← FreeRTOS 下中断使用 MSP
│    - HAL 库内部使用的栈        │
├──────────────────────────────┤
│    标准库堆 (startup.s)       │ Heap_Size = 0x200 (0.5KB)
│    - malloc() 使用             │ ← FreeRTOS heap_4.c 不使用此区域!
├──────────────────────────────┤
│    FreeRTOS 内核堆 (heap_4)   │ configTOTAL_HEAP_SIZE = 10KB
│    - 任务 TCB (约 80B/任务)   │ ← 这是一个大数组: ucHeap[10*1024]
│    - 任务栈 (128~512 words)  │
│    - 队列控制块 + 存储区       │
│    - 信号量/互斥量             │
│    - 软件定时器                │
├──────────────────────────────┤
│    .data + .bss (全局/静态)   │ ~2-3KB
│    - 外设句柄 (HAL)           │ ← 由链接器从 Flash 加载到 RAM
│    - FreeRTOS 内部静态变量     │
│    - 用户全局变量              │
├──────────────────────────────┤
│    保留给中断嵌套              │ ~1KB (估算)
└──────────────────────────────┘ 0x20000000 (SRAM 底部)
```

#### 5.2 内存预算验证

| 项目              | 大小      | 累计    |
|-------------------|-----------|---------|
| 系统栈            | 1.0 KB    | 1.0 KB  |
| 标准库堆          | 0.5 KB    | 1.5 KB  |
| FreeRTOS 堆       | 10.0 KB   | 11.5 KB |
| 静态数据 (.data + .bss) | ~2.5 KB | 14.0 KB |
| 余量              | ~6.0 KB   | 20.0 KB |

**6KB 余量** 足够容纳额外的队列、信号量及未来的功能扩展。

#### 5.3 修正操作

修改 `FreeRTOS/Inc/FreeRTOSConfig.h` 第49行:

```c
// 旧值:
#define configTOTAL_HEAP_SIZE    ( ( size_t ) ( 17 * 1024 ) )   // 17KB — 危险!

// 新值:
#define configTOTAL_HEAP_SIZE    ( ( size_t ) ( 10 * 1024 ) )   // 10KB — 安全
```

同时修改 `MDK-ARM/startup_stm32f103xb.s` 中:

```asm
Stack_Size      EQU     0x600    ; 从 0x400 → 0x600 (1KB→1.5KB)
                                  ; 每个中断嵌套约需 64B (16 regs × 4B)
                                  ; 如果使用多层中断嵌套, 增大此值
Heap_Size       EQU     0x200    ; 保持 0.5KB (对 heap_4.c 无影响)
```

**注意**: 如果你的 HAL 外设 ISR (如 USART1_IRQHandler) 内部调用 `HAL_UART_IRQHandler()` → 回调函数 → FreeRTOS 的 `xQueueSendFromISR()`, 确保这些外设中断的优先级 >= configMAX_SYSCALL_INTERRUPT_PRIORITY (即 ST 库优先级 >=11). 否则在中断中调用 FromISR API 会触发 assert 失败。

---

### 问题 6: 中断优先级配置 —— 验证与修正

#### 6.1 当前配置解析

```c
#define configKERNEL_INTERRUPT_PRIORITY          255
#define configMAX_SYSCALL_INTERRUPT_PRIORITY     191   // 0xBF
#define configLIBRARY_KERNEL_INTERRUPT_PRIORITY  15
```

**Cortex-M3 优先级寄存器**: 8 位宽, 但 STM32F1 仅实现了高 4 位 (bit7~bit4), 低 4 位始终为 0.

| ST 库值 (0-15) | 对应 NVIC 寄存器值 | 二进制 (实现的4位) |
|----------------|--------------------|--------------------|
| 0 (最高)       | 0x00               | 0000               |
| 1              | 0x10               | 0001               |
| ...            | ...                | ...                |
| 11             | 0xB0               | 1011               |
| 15 (最低)      | 0xF0               | 1111               |

- `configMAX_SYSCALL_INTERRUPT_PRIORITY = 191 = 0xBF`: 对应 ST 库优先级 **11** (0xB0 ≤ 0xBF < 0xC0). 优先级 11~15 的中断可以调用 FreeRTOS FromISR API.
- `configKERNEL_INTERRUPT_PRIORITY = 255 = 0xFF`: 对应 ST 库优先级 15 (最低). PendSV 和 SysTick 被设置为最低优先级.
- `configLIBRARY_KERNEL_INTERRUPT_PRIORITY = 15`: 非 FreeRTOS 宏, 部分 ST 代码用.

#### 6.2 必须执行的验证操作

在 `main.c` 的 `HAL_Init()` 之后添加优先级分组设置:

```c
int main(void)
{
    HAL_Init();

    /* USER CODE BEGIN Init */
    /* 设置优先级分组: 4 位全用于抢占优先级 (0 位亚优先级).
     * 这是 FreeRTOS Cortex-M3 移植的要求.
     * 必须在任何 NVIC_SetPriority() 调用之前执行. */
    NVIC_SetPriorityGrouping(0);
    /* USER CODE END Init */
    ...
}
```

**为什么需要这行代码**:
- HAL_Init() 默认可能设置优先级分组为非 0 值
- 如果存在亚优先级 (sub-priority), FreeRTOS 的 BASEPRI 屏蔽机制会失效
- `port.c` 第790行有运行期检查: `configASSERT( ( portAIRCR_REG & portPRIORITY_GROUP_MASK ) <= ulMaxPRIGROUPValue )`

#### 6.3 外设中断优先级检查清单

当前启用的外设中断及其默认优先级 (CubeMX 默认全部为 0, 即最高):

| 中断                   | 默认优先级 | 是否调用 FreeRTOS API | 需要调整?                     |
|------------------------|-----------|-----------------------|-------------------------------|
| DMA1_Channel2          | 0 (最高)  | 否                    | ⚠️ 建议降到 >=11, 避免干扰 OS  |
| DMA1_Channel4 (USART1) | 0         | 否 (HAL DMA 回调)     | ⚠️ 同上                       |
| DMA1_Channel6 (USART2) | 0         | 否                    | ⚠️ 同上                       |
| DMA1_Channel7 (USART2) | 0         | 否                    | ⚠️ 同上                       |
| TIM2                   | 0         | 取决于用途             | ⚠️ 如果调用 FromISR API 则 >=11 |
| USART1                 | 0         | 取决于用途             | ⚠️ 同上                       |
| USART2                 | 0         | 取决于用途             | ⚠️ 同上                       |

**修正方法** (在 MX_xxx_Init 或 main 中):
```c
/* 将 USART1 中断优先级设为 12 (允许调用 FromISR API) */
HAL_NVIC_SetPriority(USART1_IRQn, 12, 0);

/* 将 DMA 中断优先级设为 13 (不调用 FromISR API, 但避免抢占内核) */
HAL_NVIC_SetPriority(DMA1_Channel4_IRQn, 13, 0);
```

**核心规则**: 数值上优先级 >= 11 的中断才能安全调用 `xQueueSendFromISR()`、`xSemaphoreGiveFromISR()` 等 API。

---

## 五、项目文件清单 (按职责分类)

### 应用层 (用户可修改)
```
Core/Src/main.c                  ← 主程序: 任务创建 + 启动调度器
Core/Src/stm32f1xx_it.c          ← 外设 ISR (不含 SVC/PendSV/SysTick)
Core/Inc/main.h                  ← 引脚宏定义 + 公共声明
Core/Inc/stm32f1xx_hal_conf.h    ← HAL 模块开关
FreeRTOS/Inc/FreeRTOSConfig.h    ← FreeRTOS 参数配置
```

### 驱动层 (只读/自动生成)
```
Drivers/CMSIS/...                ← ARM + ST 官方 CMSIS
Drivers/STM32F1xx_HAL_Driver/... ← STM32 HAL + LL 库
Core/Src/gpio.c ~ usart.c        ← CubeMX 生成的外设初始化
Core/Src/stm32f1xx_hal_msp.c     ← CubeMX 生成的 MSP 配置
```

### 内核层 (只读)
```
FreeRTOS/Src/tasks.c             ← 内核: 任务调度
FreeRTOS/Src/queue.c             ← 内核: 队列
FreeRTOS/Src/list.c              ← 内核: 链表
FreeRTOS/ARM_CM3/port.c          ← 移植: Cortex-M3 上下文切换
FreeRTOS/ARM_CM3/heap_4.c        ← 内存: heap_4 算法
FreeRTOS/ARM_CM3/portmacro.h     ← 移植: 临界区/任务切换宏
FreeRTOS/Inc/*.h                 ← 内核头文件
```

### 启动层 (只读)
```
MDK-ARM/startup_stm32f103xb.s    ← 向量表 + Reset_Handler
Core/Src/system_stm32f1xx.c      ← SystemInit()
```

---

## 六、当前优先级划分建议

| 优先级 | 任务                    | 说明                              |
|--------|------------------------|-----------------------------------|
| **P0** | 解决 ISR 冲突           | 阻塞项: SVC/PendSV/SysTick 未接管 |
| **P0** | 更新 FreeRTOSConfig.h   | 阻塞项: 版本不兼容导致编译失败    |
| **P1** | 修改 main.c 创建任务   | FreeRTOS 运行的前提条件           |
| **P1** | 配置 Keil 工程         | 添加源文件和头文件路径            |
| **P1** | 调整 configTOTAL_HEAP_SIZE | 20KB SRAM 容量约束             |
| **P2** | HAL_IncTick 时间基准    | HAL 延时函数依赖此计数器          |
| **P2** | 验证编译零错误零警告    | 质量检查                          |

---

## 七、附录

### A. STM32F103C8T6 资源约束

| 资源     | 容量          |
|----------|---------------|
| Flash    | 64 KB         |
| SRAM     | 20 KB         |
| 主频     | 72 MHz        |
| 内核     | Cortex-M3     |

### B. FreeRTOS V11.1.0 可用模块

| 模块           | 文件            | 当前状态 |
|----------------|-----------------|----------|
| 任务调度       | tasks.c         | ✅ 已添加 |
| 队列           | queue.c         | ✅ 已添加 |
| 链表           | list.c          | ✅ 已添加 |
| 软件定时器     | timers.c        | ❌ 未添加 |
| 事件组         | event_groups.c  | ❌ 未添加 |
| 协程           | croutine.c      | ❌ 未添加 |
| 内存管理       | heap_4.c        | ✅ 已添加 |

### C. 关键文件版本对照

| 文件                    | 实际版本   | 期望版本   | 匹配 |
|-------------------------|-----------|-----------|------|
| FreeRTOS.h              | V11.1.0   | V11.1.0   | ✅   |
| tasks.c / queue.c       | V11.1.0   | V11.1.0   | ✅   |
| port.c / portmacro.h    | V11.1.0   | V11.1.0   | ✅   |
| FreeRTOSConfig.h        | 202212.00 | V11.1.0   | ❌   |

---

*日志结束。下一步执行: 按优先级顺序逐步解决上述问题。*
