# DAQ 风机性能测试数据采集系统

基于 STM32F103C8T6 的 Modbus RTU 数据采集与 OTA 升级固件。

## 工程结构

```
DAQ/
├── 29_A/DAQ/             # APP 程序 (FreeRTOS + Modbus + SPI 传感器采集)
├── 29_B/DAQ_B/           # Bootloader 程序 (Modbus OTA 升级协议)
├── tools/daq_hw_test/    # 半实物测试工具 (Python CLI + Tkinter GUI)
└── README.md
```

## 技术架构

### RTOS

| 项目 | 配置 |
|---|---|
| 调度器 | FreeRTOS, 抢占式, tick 1 kHz, 最大优先级 5 |
| 堆内存 | 10 KB (heap_4) |
| 任务通知 | 启用 (ISR → Task 轻量通信, 替代信号量) |
| 互斥量/计数信号量 | 启用 |
| NVIC 优先级 | 内核 `0xF0`, 系统调用 `0x50` (优先级 5) |

**任务划分：**

| 任务 | 优先级 | 职责 |
|---|---|---|
| SensorTask | `max-1` | TIM2 UEV 中断通知 → SPI1 DMA 读 AD7606 → 双缓冲交换 |
| CommTask | `max-2` | USART2 IDLE 中断等 Modbus 帧 → `0x04` 响应 / `0x41` OTA 请求 |
| MonitorTask | `max-3` | 检测 Sensor/Comm 心跳 → 喂 IWDG → 错误记录 |

**SysTick 防护：** 调度器启动前不调用 `xPortSysTickHandler`，避免访问未初始化内核数据。

**双缓冲无锁读写 (SPSC)：** Cortex-M3 上 `uint16_t*` 对齐读是原子的。Writer (SensorTask) 写 inactive buffer，完成后原子交换 `active_buf` 指针；Reader (CommTask) 直接快照读取，无需互斥锁。

### 硬件平台

| 项目 | 规格 |
|---|---|
| MCU | STM32F103C8T6 (Cortex-M3, LQFP48, 64KB Flash, 20KB SRAM) |
| 时钟 | HSE 8MHz → PLL ×9 → 72MHz, APB1 36MHz, APB2 72MHz |
| ADC | AD7606 (8 通道同步采样) |
| 触发 | TIM2 CH1 PWM → PA0 → AD7606 CONVST, 10kHz (100μs) |
| 调试串口 | USART1 PA9/PA10, 115200 8N1 |
| 数据/OTA 口 | USART2 PA2/PA3, 115200 8N1 |
| 传感器接口 | SPI1 (全双工主模式, DMA Normal) |
| 看门狗 | IWDG (硬件) + MonitorTask 软件心跳 |
| IDE | Keil MDK-ARM 5.42 / ARMCC 5.06 |
| CubeMX | STM32CubeMX 6.17 + FW_F1 V1.8.7 |

**UART 驱动：**

| 串口 | 用途 | RX | TX |
|---|---|---|---|
| USART1 | 调试打印 | 暂不启用 | 环形缓冲 + DMA TC ISR |
| USART2 | 通讯/OTA | 固定缓冲 (1024B) + 描述符队列 (8槽) + IDLE 中断 | 零拷贝单帧 + DMA TC ISR |

自编驱动直接操作寄存器 (CPAR/CMAR/CNDTR)，HAL 的 `HAL_DMA_Init` 只写 CCR 不写 CPAR。

**SPI 驱动：** SPI1 DMA Normal 模式，TX 发哑字触发 SCK 以采样 MISO。CNDTR 到 0 后硬件自动清 EN。

**TIM2 驱动：** `MX_TIM2_Init` 不自动启用。需额外调用 `LL_TIM_EnableIT_UPDATE` (UEV 中断通知任务) 和 `LL_TIM_EnableCounter` (启动计数)。

### 上位机

| 项目 | 技术栈 |
|---|---|
| 主系统 | C# / WPF / .NET Framework 4.8 / MVVM |
| 图表 | SciChart 6.x |
| 通信 | TouchSocket TCP + System.IO.Ports.SerialPort |
| Excel | ClosedXML / Spire.XLS |
| 数学 | MathNet.Numerics |

## Flash 分区 (64KB)

| 分区 | 起始地址 | 大小 | 说明 |
|---|---|---|---|
| Bootloader | `0x08000000` | 16 KB | 29_B, 页 0–15 |
| APP | `0x08004000` | 47 KB | 29_A, 页 16–62 |
| OTAInfo | `0x0800FC00` | 1 KB | 升级参数区, 页 63 |

APP 向量表重定位至 `0x08004000`，由 `system_stm32f1xx.c` 在 `SystemInit()` 中完成。

## 通信协议

### 物理层

- USART2, 115200 8N1, TTL 电平
- 可通过 CH340 / MAX485 转为 RS485

### 链路层

- Modbus RTU 风格帧, CRC16 (多项式 `0xA001`, 初值 `0xFFFF`, 低字节在前)
- 从站地址: `0x02`

### APP 支持的功能码

| 功能码 | 说明 |
|---|---|
| `0x04` | 读输入寄存器 (8 路 × uint16) |
| `0x41` | OTA 升级请求 (APP 写 OTAInfo 后复位, 进 Bootloader) |

#### 0x04 读输入寄存器

请求:

```
02 04 00 00 00 08 CRC_LO CRC_HI
```

响应 (21 字节):

```
02 04 10 [8×uint16 BE] CRC_LO CRC_HI
```

#### 0x41 APP 升级请求

请求 (4 字节):

```
02 41 CRC_LO CRC_HI
```

响应 (5 字节):

```
02 41 00 CRC_LO CRC_HI
```

APP 收到后写 OTAInfo、发送 ACK、等待 TC 后复位。

### Bootloader 支持的功能码

| 功能码 | 说明 |
|---|---|
| `0x41` | OTA 固件下载 (子命令 START / DATA / END / ABORT) |
| `0x42` | 跳转 APP (维护用) |

#### 0x41 START (17 字节)

```
02 41 01 [version u32 BE] [size u32 BE] [crc32 u32 BE] CRC_LO CRC_HI
```

#### 0x41 DATA (可变长, max 251 字节)

```
02 41 02 [offset u32 BE] [data_len u16 BE] [data...] CRC_LO CRC_HI
```

`data_len` 限制 1..240。

#### 0x41 END (5 字节)

```
02 41 03 CRC_LO CRC_HI
```

#### 0x41 ABORT (5 字节)

```
02 41 04 CRC_LO CRC_HI
```

#### OTA 响应帧 (11 字节)

```
02 41 [cmd] [status] [err] [offset u32 BE] CRC_LO CRC_HI
```

- `status = 0x00` 成功, `0x01` 失败
- `err` 见 `OTA_ErrorFlag` 枚举

#### OTA 异常响应 (5 字节)

```
02 C1 [err] CRC_LO CRC_HI
```

#### 0x42 跳转 APP (4 字节)

```
02 42 CRC_LO CRC_HI
```

成功时直接跳转, 通常无响应。

## OTA 升级

### Flash 物理特性

STM32F103C8T6 中容量产品：64 页 × 1KB。页擦除约 40ms，半字编程约 40μs。写入前目标区域必须已擦除 (全 `0xFF`)。编程以半字 (16-bit) 为单位。

### 状态机

```text
IDLE → REQUEST → ERASING → DOWNLOADING → VERIFYING → READY
  ↑                                                      │
  └──────────── FAILED ←─────────────────────────────────┘
```

### Bootloader 启动决策 (`OTA_BootCheck`)

| OTAInfo 状态 | APP 有效? | 动作 |
|---|---|---|
| magic ≠ `0xA5A5A5A5` | 是 | 直接 `AppJump` |
| magic ≠ `0xA5A5A5A5` | 否 | 停留 Bootloader |
| REQUEST / DOWNLOADING / FAILED | — | 停留 Bootloader 等升级 |
| READY | 是 | 清 OTAInfo → `AppJump` |
| 其他 | — | 标记 FAILED |

### APP 有效性检查

`OTA_AppIsValid()` 校验：
1. 初始 SP 在 `0x20000000–0x20005000`
2. Reset vector 不能是 `0xFFFFFFFF`
3. Reset vector 最低位 = 1 (Thumb)
4. Reset vector 地址在 `APP_ADDR–OTAINFO_ADDR` 区间

### AppJump 操作顺序

关全局中断 → 停 SysTick → 清 UART DMA 请求 → 停 DMA → `HAL_UART_DeInit` ×2 → `HAL_DeInit` → 清 NVIC → `SCB->VTOR = APP_ADDR` → `__set_MSP(app_sp)` → 跳转

### 升级流程

```
上位机 ──0x04──> APP (正常采集)
上位机 ──0x41──> APP (请求升级)
APP    返回 ACK, 写 OTAInfo REQUEST, 复位
Bootloader 启动, OTA_BootCheck → REQUEST → 停留
上位机 ──START──> Bootloader (version/size/CRC32)
                  Bootloader: 校验 size, 写 OTAInfo → 擦 APP 区 → DOWNLOADING
上位机 ──DATA──>  Bootloader (offset+240B 分包, 顺序写入, 不跳包)
上位机 ──END───>  Bootloader (校验 CRC32 + 向量表 → READY → 复位)
Bootloader 重启, OTA_BootCheck → READY + APP 有效 → AppJump
APP 恢复运行
上位机 ──0x04──> APP (验证采集)
```


