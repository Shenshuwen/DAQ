# 29-DAQ UART DMA 驱动架构

## 架构概览

```
┌─────────────────────────────────────────────────────────────┐
│                        任务层                                │
│  Sensor_Task              Comm_Task                         │
│  (1s 周期 printf)         (阻塞等待 RX 帧 → 回显)            │
│      │                        │                             │
│      ▼                        ▼                             │
│  UART1_Printf            UART_RX_Wait                       │
│  (环形缓冲 + DMA TX)     (信号量阻塞 + 零拷贝 RX)            │
│      │                        │                             │
├──────┼────────────────────────┼─────────────────────────────┤
│      ▼                        ▼                             │
│  UART1_TxRing             UART2_RxCtrl                      │
│  Buffer[256]              Desc[8] ← 指向 UART2_RxBuf[1024]  │
│  Head / Tail              In / Out / End                    │
│      │                        │                             │
├──────┼────────────────────────┼─────────────────────────────┤
│      ▼                        ▼                             │
│  DMA1_Ch4 TX              DMA1_Ch6 RX + DMA1_Ch7 TX         │
│  USART1 (PA9/PA10)        USART2 (PA2/PA3)                  │
└─────────────────────────────────────────────────────────────┘
```

## UART 角色分工

| | UART1 (调试口) | UART2 (数据口) |
|---|---|---|
| 引脚 | PA9 (TX), PA10 (RX) | PA2 (TX), PA3 (RX) |
| 波特率 | 115200 8N1 | 115200 8N1 |
| TX 模式 | 环形缓冲 + 字节流 | 零拷贝单帧 |
| RX 模式 | 无 | 固定缓冲 + 描述符队列 |
| DMA TX | DMA1 Channel4, 优先级 LOW | DMA1 Channel7, 优先级 MEDIUM |
| DMA RX | 无 | DMA1 Channel6, 优先级 HIGH |
| TX ISR | `UART_RingBufferTX_TC_ISR` | `UART_ZeroCopyTX_TC_ISR` |
| RX ISR | 无 | `UART_IDLE_ISR` + DMA TC 双路径 |
| RTOS 同步 | `UART1_TxSemaphore` (缓冲有空间) | `UART2_RxSemaphore` (帧到达) |

## 数据结构

### RX 描述符队列 (`UART_RxCtrlType`)

```
UART2_RxBuf[1024]                          Desc[8]
┌──────────────────────┐    ┌──────┬──────┬──────┬─────┬──────┐
│ Frame0 │ Frame1 │ ... │    │[0]   │[1]   │[2]   │ ... │[7]   │
│  ┌─┐    │  ┌─┐   │    │  ┌─┤Start │      │      │     │      │
│  └─┘    │  └─┘   │    │  │ ┤End   │      │      │     │      │
└──────────────────────┘    │ ├──┐   │      │      │     │      │
        ▲          ▲        │ │In│  Out     │      │     │ End  │
        └──────────┼────────┘ └──┘   │      │      │     │(哨兵)│
                   └───────────────┘      │      │      │     │
                  Start/End 指针指向 DMA 缓冲区内部
```

- `In`  = 生产者 (IDLE ISR 写入)
- `Out` = 消费者 (应用层 `Get`/`Release` 推进)
- `End` = `&Desc[7]` 哨兵, 回绕判定
- **8 个槽位, 零拷贝** — 应用获取的是 DMA 缓冲区的直接指针

### TX 环形缓冲 (`UART_TxRingType`)

```
Buffer[256]
┌──────────────────────────────────────┐
│ D  D  D  D  D  .  .  .  .  .  .  .  │
└──────────────────────────────────────┘
   ▲           ▲
   Tail        Head
   (DMA已发)   (printf已写)

Head == Tail → 缓冲空
(Head + 1) % 256 == Tail → 缓冲满 (留1字节防混淆)
```

- Head = 生产者 (printf 写入, **仅任务上下文**)
- Tail = 消费者 (DMA TC ISR 推进, **仅 ISR 上下文**)
- 单向推进, 无锁设计; 仅 `TxBusy` 需临界区保护

## RTOS 同步机制

### TX 路径：信号量通知缓冲空间

```
Sensor_Task                    DMA1_Ch4 ISR
    │                               │
    │ UART1_Printf("...")           │
    │ 逐字节写入环形缓冲              │
    │ 缓冲满 → xSemaphoreTake()     │
    │ 任务挂起 ⏸                    │
    │                               │ DMA 传输完成
    │                               │ 推进 Tail
    │                               │ xSemaphoreGiveFromISR()
    │ 任务唤醒 ▶                     │
    │ 继续写入剩余字节                │
    │                               │
```

- 信号量: `UART1_TxSemaphore` (二值, 初始无信号)
- 创建: `UART1_DrvInit()` 中 `xSemaphoreCreateBinary()`
- Take: `UART1_Printf` 缓冲满时, `xSemaphoreTake(信号量, 100ms)`
- Give: `UART_RingBufferTX_TC_ISR` 推进 Tail 后, `xSemaphoreGiveFromISR()`

### RX 路径：信号量通知帧到达

```
Comm_Task                       USART2 ISR / DMA Ch6 ISR
    │                               │
    │ UART_RX_Wait(portMAX_DELAY)   │
    │ xSemaphoreTake() → 挂起 ⏸     │
    │                               │ IDLE 中断 / DMA TC
    │                               │ 帧入描述符队列
    │                               │ xSemaphoreGiveFromISR()
    │ 任务唤醒 ▶                     │
    │ while(Get) 消费所有积压帧      │
    │ 再次阻塞等待下一帧              │
    │                               │
```

- 信号量: `UART2_RxSemaphore` (二值, 初始无信号)
- API: `UART_RX_Wait(ctrl, handler, timeout)` — 封装阻塞 + 消费逻辑
- 关键：while 循环一次性消费**所有**积压帧, 避免信号量累积

## 临界区保护

| 原 (裸机) | 改后 (RTOS) | 原因 |
|---|---|---|
| `__disable_irq()` | `taskENTER_CRITICAL()` | FreeRTOS 感知嵌套, 不影响内核状态 |
| `__enable_irq()` | `taskEXIT_CRITICAL()` | 同上 |

`taskENTER_CRITICAL` 设置 BASEPRI 掩蔽所有优先级 ≥ 5 的中断（包含本驱动的 DMA/USART 中断 7-12）, 但不过度关闭最高优先级中断。

## ISR 中断链路

```
中断源                  ISR 函数                          处理逻辑
─────────────────────  ────────────────────────────────  ──────────────────────
USART2 (IDLE)          USART2_IRQHandler                 主路径: 帧边界检测
                         └─ UART_IDLE_ISR                 停 DMA → 计算帧长
                                                            → 填写描述符
                                                            → Give 信号量
                                                            → 重启 DMA

DMA1_Ch6 (RX TC)       DMA1_Channel6_IRQHandler          备路径: 满帧无 IDLE
                         └─ UART_IDLE_ISR                 同上 (双保险)

DMA1_Ch4 (UART1 TX TC) DMA1_Channel4_IRQHandler          UART1 环形缓冲完成
                         └─ UART_RingBufferTX_TC_ISR      推进 Tail → 链式 Kick
                                                            → Give 信号量

DMA1_Ch7 (UART2 TX TC) DMA1_Channel7_IRQHandler          UART2 零拷贝完成
                         └─ UART_ZeroCopyTX_TC_ISR        清 busy

USART1                 USART1_IRQHandler                 仅 HAL 处理
                         └─ HAL_UART_IRQHandler           (无自定义逻辑)
```

## 中断优先级

| IRQ | 优先级 | 用途 | FromISR API |
|---|---|---|---|
| USART2 | 7 | RX IDLE 帧检测 | ✅ |
| DMA1_Ch6 | 8 | RX TC 满帧检测 | ✅ |
| DMA1_Ch7 | 8 | TX 零拷贝完成 | ✅ |
| USART1 | 11 | (未使用 RX) | — |
| DMA1_Ch4 | 12 | TX 环形缓冲完成 | ✅ |
| SysTick | 15 (最低) | FreeRTOS 时基 | — |

所有调用 `xSemaphoreGiveFromISR` 的中断优先级均 ≥ 7, 高于 `configMAX_SYSCALL_INTERRUPT_PRIORITY`(5), 满足 FreeRTOS 要求。

## 修复过的 Bug

| # | Bug | 症状 | 修复 |
|---|---|---|---|
| 1 | `ChannelIndex * 4U` 双重移位 | IFCR 写 0 → TC 标志清不掉 → ISR storm → CPU 假死 | 去掉 `* 4U`, HAL 的 `ChannelIndex` 已是 bit 偏移 |
| 2 | DMA CPAR 从未设置 | 数据搬到地址 0 → USART 无输出 | 3 处补 `CPAR = &huart->Instance->DR` |
| 3 | `UART2_RxBuf` 头文件缺 `extern` | 多文件包含重复定义 | 补 `extern` |
| 4 | `UART_TX_ZeroCopy_Send` 声明与实现不匹配 | 链接失败 | 统一签名, 删 `is_zc` |
| 5 | `UART2_TX_ZeroCopy_Send` 宏缺 `last_len` 参数 | 编译失败 | 补 `&UART2_LastTxLen` |
| 6 | printf 缓冲满时自旋等待 | CPU 空烧 | RTOS 改造: 信号量阻塞 |
| 7 | Comm_Task 10ms 轮询 | 无效 CPU 唤醒 | RTOS 改造: `UART_RX_Wait` 阻塞 |
| 8 | 裸 `__disable_irq` | FreeRTOS 内核状态不感知 | `taskENTER_CRITICAL` |

## TX API 对照

| API | 模式 | 阻塞? | 适用场景 |
|---|---|---|---|
| `UART1_Printf(fmt, ...)` | 环形缓冲 | 仅满时阻塞 (有超时) | 调试打印 |
| `UART_TX_ZeroCopy_Send(huart, hdma, busy, pdata, last_len, len)` | 零拷贝 | 忙时立即返回 0 | 协议应答 |
| `UART2_TX_ZeroCopy_Send(pdata, len)` | 零拷贝 (UART2 便利宏) | 同上 | Modbus 应答 |

## RX API 对照

| API | 阻塞? | 适用场景 |
|---|---|---|
| `UART_RX_ZeroCopy_Get(ctrl, &pdata, &len)` | 否, 立即返回 | 底层, 获取单帧指针 |
| `UART_RX_ZeroCopy_Release(ctrl)` | 否 | 释放槽位 |
| `UART_RX_Poll(ctrl, handler)` | 否, 空返回 | 非 RTOS / 轮询 |
| `UART_RX_Wait(ctrl, handler, timeout)` | **是, RTOS 阻塞** | 任务中首选 |

## 文件清单

| 文件 | 职责 |
|---|---|
| `DAQ/Core/Inc/uart_drv.h` | 数据结构、API 声明、HAL 句柄 extern、宏 |
| `DAQ/Core/Src/uart_drv.c` | 全部驱动实现: 初始化、RX、TX、printf、ISR、RTOS 同步 |
| `DAQ/Core/Src/stm32f1xx_it.c` | ISR 入口: 6 路中断分发到驱动 ISR |
| `DAQ/Core/Src/usart.c` | CubeMX 生成: HAL UART/DMA 初始化 + MspInit |
| `DAQ/Core/Src/dma.c` | CubeMX 生成: DMA NVIC 配置 |
| `DAQ/Core/Src/main.c` | 初始化顺序: HAL → DMA → USART → `UARTx_DrvInit` → RTOS |
| `DAQ/FreeRTOS/Src/rtos.c` | 测试任务: Sensor (printf) + Comm (回显) |

## 测试方法

1. **编译**: 确认 `uart_drv.c` 已加入 Keil 工程
2. **烧录**: STM32F103C8T6
3. **UART1** (PA9/PA10, 115200 8N1): 应每秒输出 `Hello from UART1: tick=N`
4. **UART2** (PA2/PA3, 115200 8N1): 发送任意字节, 原样返回 (回显)
5. **LED**: PC13 每秒翻转, 配合 printf 验证任务未卡死
