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

## 硬件平台

| 项目 | 规格 |
|---|---|
| MCU | STM32F103C8T6 (Cortex-M3, 64KB Flash, 20KB SRAM) |
| 时钟 | HSE 8MHz → PLL ×9 → 72MHz |
| IDE | Keil MDK-ARM 5.42 / ARMCC 5.06 |
| CubeMX | STM32CubeMX 6.17 + FW_F1 V1.8.7 |
| 调试串口 | USART1 PA9/PA10, 115200 8N1 |
| 数据串口 | USART2 PA2/PA3, 115200 8N1 |
| 传感器 | SPI1 外挂 ADC 模块 |
| RTOS | FreeRTOS (APP 侧) |

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

## OTA 升级流程

```
上位机 ──0x04──> APP (正常采集)
上位机 ──0x41──> APP (请求升级)
APP    返回 ACK, 写 OTAInfo, 复位
Bootloader 启动, 读 OTAInfo, 停留升级模式
上位机 ──START──> Bootloader (版本/大小/CRC32)
上位机 ──DATA──>  Bootloader (240B 分包, 顺序写入)
上位机 ──END───>  Bootloader (校验 CRC32 + 向量表)
Bootloader 写入 READY, 复位
Bootloader 重启, OTA_BootCheck → AppJump
APP 恢复运行
上位机 ──0x04──> APP (验证采集)
```


