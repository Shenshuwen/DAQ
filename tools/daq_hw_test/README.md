# DAQ 半实物测试工具

用于一块 STM32 最小系统板 + 两个 USB-CH340 模块条件下，验证 29_A APP / 29_B Bootloader 工作流。

## 硬件连接

### 数据口 USART2

用于 Python 工具发送 Modbus / OTA 帧。

```text
STM32 PA2 / USART2_TX -> CH340 数据口 RXD
STM32 PA3 / USART2_RX -> CH340 数据口 TXD
STM32 GND             -> CH340 数据口 GND
```

### 调试口 USART1

用于实时查看 29_A APP / 29_B Bootloader 调试打印。

```text
STM32 PA9  / USART1_TX -> CH340 调试口 RXD
STM32 PA10 / USART1_RX -> CH340 调试口 TXD，可选
STM32 GND              -> CH340 调试口 GND
```

串口参数默认：`115200, 8N1, 无校验, 无流控`。

注意：CH340 的 TXD 建议使用 3.3V 电平。数据口和调试口不能选择同一个 COM。

## 安装依赖

在本目录或工程根目录执行：

```bash
python -m pip install -r tools/daq_hw_test/requirements.txt
```

## 图形界面

启动 UI：

```bash
python tools/daq_hw_test/daq_hw_test_ui.py
```

推荐操作顺序：

1. 点击“刷新串口”。
2. 选择数据口 USART2 对应 COM。
3. 选择调试口 USART1 对应 COM，可选。
4. 点击“连接数据口”。
5. 点击“启动调试监听”。
6. 用“读取ADC一次”验证 29_A APP 的 `0x04` 输入寄存器响应。
7. 用“坏CRC测试”验证 APP 会丢弃 CRC 错误帧。
8. 选择 APP `.bin` 固件，确认 size / CRC32 / SP / Reset 校验通过。
9. 点击“完整OTA”执行 APP -> Bootloader -> START/DATA/END -> APP 恢复验证。

如果设备已经停留在 Bootloader，勾选“设备已在Bootloader，直接OTA”。

## 命令行入口

保留 CLI 入口，便于脚本化测试。

列出串口：

```bash
python tools/daq_hw_test/daq_hw_test.py --list-ports
```

读取一次 ADC：

```bash
python tools/daq_hw_test/daq_hw_test.py --data-port COM8 --debug-port COM7 --read-adc
```

循环读取 ADC：

```bash
python tools/daq_hw_test/daq_hw_test.py --data-port COM8 --debug-port COM7 --loop --interval 1
```

坏 CRC 测试：

```bash
python tools/daq_hw_test/daq_hw_test.py --data-port COM8 --bad-crc
```

请求 APP 进入 Bootloader：

```bash
python tools/daq_hw_test/daq_hw_test.py --data-port COM8 --debug-port COM7 --app-ota-request
```

完整 OTA：

```bash
python tools/daq_hw_test/daq_hw_test.py --data-port COM8 --debug-port COM7 --ota path/to/DAQ.bin --version 1
```

设备已在 Bootloader 时直接 OTA：

```bash
python tools/daq_hw_test/daq_hw_test.py --data-port COM8 --debug-port COM7 --ota path/to/DAQ.bin --skip-app-request
```

发送 ABORT：

```bash
python tools/daq_hw_test/daq_hw_test.py --data-port COM8 --abort
```

发送 JUMP APP：

```bash
python tools/daq_hw_test/daq_hw_test.py --data-port COM8 --jump-app
```

## 打包为 exe

安装 PyInstaller：

```bash
python -m pip install pyinstaller
```

执行：

```bash
tools/daq_hw_test/build_ui.bat
```

产物在：

```text
dist/DAQ半实物测试工具.exe
```

## 常见问题

### UI 能打开，但读取 ADC 超时

检查：

- 数据口是否接 USART2：PA2/PA3。
- TX/RX 是否交叉。
- GND 是否共地。
- APP 是否已经烧录并运行，而不是停留在 Bootloader。
- 波特率是否为 115200。

### UART1 没有日志

检查：

- 调试口是否接 USART1：PA9/PA10。
- 如果只看日志，至少需要 PA9 -> CH340 RXD 和 GND。
- 29_A 是否启用了 UART1 打印。

### OTA 提示固件向量表非法

说明选择的 `.bin` 不是链接到 `0x08004000` 的 29_A APP 镜像，或选错了 `.hex` / 其他文件。当前工具按 APP bin 的前 8 字节检查 SP 和 Reset vector。

### 完整 OTA 中断后设备无法进入 APP

这符合 Bootloader 设计：START 后 APP 区会被擦除，失败后设备可能停留 Bootloader。重新执行 OTA，并勾选“设备已在Bootloader，直接OTA”。
