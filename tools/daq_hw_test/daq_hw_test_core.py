#!/usr/bin/env python3
"""Core protocol and serial helpers for the DAQ half-hardware test tools."""

from __future__ import annotations

import struct
import threading
import time
from dataclasses import dataclass
from typing import Callable, Optional

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover
    serial = None
    list_ports = None


SLAVE_ADDR = 0x02
FUNC_READ_INPUT_REGS = 0x04
FUNC_OTA = 0x41
FUNC_JUMP_APP = 0x42

OTA_CMD_START = 0x01
OTA_CMD_DATA = 0x02
OTA_CMD_END = 0x03
OTA_CMD_ABORT = 0x04

OTA_PACKET_DATA_MAX = 240
APP_ADDR = 0x08004000
OTAINFO_ADDR = 0x0800FC00
APP_SIZE_MAX = 47 * 1024
SRAM_BASE = 0x20000000
SRAM_END = 0x20005000

DEFAULT_BAUD = 115200
LogCallback = Callable[[str], None]
ProgressCallback = Callable[[str, int, int], None]


class ProtocolError(RuntimeError):
    pass


@dataclass
class OtaResponse:
    command: int
    status: int
    error: int
    offset: int
    is_exception: bool = False


@dataclass
class FirmwareInfo:
    path: str
    size: int
    crc32: int
    initial_sp: int
    reset_vector: int


@dataclass
class SerialPortInfo:
    device: str
    description: str
    hwid: str

    @property
    def label(self) -> str:
        text = f"{self.device} - {self.description}"
        return text.strip()


def require_pyserial() -> None:
    if serial is None:
        raise RuntimeError("pyserial is required. Install it with: python -m pip install pyserial")


def log_to(callback: Optional[LogCallback], message: str) -> None:
    if callback:
        callback(message)
    else:
        print(message)


class DebugReader(threading.Thread):
    def __init__(self, port: str, baud: int, stop_event: threading.Event, log_callback: Optional[LogCallback] = None):
        super().__init__(daemon=True)
        self.port = port
        self.baud = baud
        self.stop_event = stop_event
        self.log_callback = log_callback
        self.ser: Optional[serial.Serial] = None

    def run(self) -> None:
        require_pyserial()
        try:
            self.ser = serial.Serial(self.port, self.baud, timeout=0.2)
            log_to(self.log_callback, f"[debug] opened {self.port} @ {self.baud}")
            buffer = bytearray()
            while not self.stop_event.is_set():
                chunk = self.ser.read(256)
                if not chunk:
                    continue
                buffer.extend(chunk)
                while b"\n" in buffer:
                    line, _, buffer = buffer.partition(b"\n")
                    text = line.rstrip(b"\r").decode("utf-8", errors="replace")
                    log_to(self.log_callback, f"[UART1] {text}")
            if buffer:
                text = buffer.decode("utf-8", errors="replace")
                log_to(self.log_callback, f"[UART1] {text}")
        except Exception as exc:
            log_to(self.log_callback, f"[debug] stopped: {exc}")
        finally:
            if self.ser and self.ser.is_open:
                self.ser.close()


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
            crc &= 0xFFFF
    return crc


def append_crc16(payload: bytes) -> bytes:
    crc = crc16_modbus(payload)
    return payload + bytes((crc & 0xFF, (crc >> 8) & 0xFF))


def check_crc16(frame: bytes) -> bool:
    if len(frame) < 4:
        return False
    expected = frame[-2] | (frame[-1] << 8)
    return expected == crc16_modbus(frame[:-2])


def crc32_bootloader(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x00000001:
                crc = (crc >> 1) ^ 0xEDB88320
            else:
                crc >>= 1
            crc &= 0xFFFFFFFF
    return (~crc) & 0xFFFFFFFF


def hexs(data: bytes) -> str:
    return " ".join(f"{b:02X}" for b in data)


def build_read_adc() -> bytes:
    return append_crc16(bytes((SLAVE_ADDR, FUNC_READ_INPUT_REGS, 0x00, 0x00, 0x00, 0x08)))


def build_app_ota_request() -> bytes:
    return append_crc16(bytes((SLAVE_ADDR, FUNC_OTA)))


def build_jump_app() -> bytes:
    return append_crc16(bytes((SLAVE_ADDR, FUNC_JUMP_APP)))


def build_ota_start(version: int, size: int, crc32: int) -> bytes:
    payload = bytes((SLAVE_ADDR, FUNC_OTA, OTA_CMD_START))
    payload += struct.pack(">III", version & 0xFFFFFFFF, size & 0xFFFFFFFF, crc32 & 0xFFFFFFFF)
    return append_crc16(payload)


def build_ota_data(offset: int, data: bytes) -> bytes:
    if not 1 <= len(data) <= OTA_PACKET_DATA_MAX:
        raise ValueError(f"DATA payload length must be 1..{OTA_PACKET_DATA_MAX}")
    payload = bytes((SLAVE_ADDR, FUNC_OTA, OTA_CMD_DATA))
    payload += struct.pack(">IH", offset & 0xFFFFFFFF, len(data))
    payload += data
    return append_crc16(payload)


def build_ota_end() -> bytes:
    return append_crc16(bytes((SLAVE_ADDR, FUNC_OTA, OTA_CMD_END)))


def build_ota_abort() -> bytes:
    return append_crc16(bytes((SLAVE_ADDR, FUNC_OTA, OTA_CMD_ABORT)))


def inspect_app_firmware(path: str) -> FirmwareInfo:
    with open(path, "rb") as f:
        data = f.read()
    validate_app_firmware(data)
    sp, reset = struct.unpack_from("<II", data, 0)
    return FirmwareInfo(path=path, size=len(data), crc32=crc32_bootloader(data), initial_sp=sp, reset_vector=reset)


def validate_app_firmware(data: bytes) -> None:
    if not data:
        raise ProtocolError("firmware is empty")
    if len(data) > APP_SIZE_MAX:
        raise ProtocolError(f"firmware too large: {len(data)} bytes > {APP_SIZE_MAX} bytes")
    if len(data) < 8:
        raise ProtocolError("firmware is too small to contain vector table")

    sp, reset = struct.unpack_from("<II", data, 0)
    reset_addr = reset & ~1
    if not (SRAM_BASE <= sp <= SRAM_END):
        raise ProtocolError(f"invalid initial SP: 0x{sp:08X}, expected 0x{SRAM_BASE:08X}..0x{SRAM_END:08X}")
    if (reset & 1) == 0:
        raise ProtocolError(f"reset vector is not a Thumb address: 0x{reset:08X}")
    if not (APP_ADDR <= reset_addr < OTAINFO_ADDR):
        raise ProtocolError(
            f"reset vector 0x{reset_addr:08X} is outside APP partition "
            f"0x{APP_ADDR:08X}..0x{OTAINFO_ADDR - 1:08X}; check linker address"
        )


def read_exact(ser: serial.Serial, size: int, timeout: float, cancel_event: Optional[threading.Event] = None) -> bytes:
    old_timeout = ser.timeout
    ser.timeout = 0.05
    deadline = time.monotonic() + timeout
    data = bytearray()
    try:
        while len(data) < size and time.monotonic() < deadline:
            if cancel_event and cancel_event.is_set():
                raise InterruptedError("operation cancelled")
            chunk = ser.read(size - len(data))
            if chunk:
                data.extend(chunk)
        if len(data) != size:
            raise TimeoutError(f"timeout waiting {size} bytes, got {len(data)}: {hexs(bytes(data))}")
        return bytes(data)
    finally:
        ser.timeout = old_timeout


def read_adc_once(ser: serial.Serial, timeout: float = 1.0, verbose: bool = True, log_callback: Optional[LogCallback] = None) -> list[int]:
    frame = build_read_adc()
    ser.reset_input_buffer()
    ser.write(frame)
    ser.flush()
    resp = read_exact(ser, 21, timeout)
    if verbose:
        log_to(log_callback, f"[adc] tx: {hexs(frame)}")
        log_to(log_callback, f"[adc] rx: {hexs(resp)}")
    if not check_crc16(resp):
        raise ProtocolError("ADC response CRC mismatch")
    if resp[0] != SLAVE_ADDR or resp[1] != FUNC_READ_INPUT_REGS or resp[2] != 16:
        raise ProtocolError(f"unexpected ADC response header: {hexs(resp[:3])}")
    values = [struct.unpack(">H", resp[3 + i * 2 : 5 + i * 2])[0] for i in range(8)]
    log_to(log_callback, "[adc] " + " ".join(f"CH{i}={v}" for i, v in enumerate(values)))
    return values


def test_bad_crc_is_silent(ser: serial.Serial, timeout: float = 0.5, log_callback: Optional[LogCallback] = None) -> None:
    frame = bytearray(build_read_adc())
    frame[-1] ^= 0xFF
    ser.reset_input_buffer()
    ser.write(frame)
    ser.flush()
    time.sleep(timeout)
    waiting = ser.in_waiting
    data = ser.read(waiting) if waiting else b""
    log_to(log_callback, f"[bad-crc] tx: {hexs(bytes(frame))}")
    if data:
        raise ProtocolError(f"bad CRC frame unexpectedly got response: {hexs(data)}")
    log_to(log_callback, "[bad-crc] no response, OK")


def parse_app_ota_ack(frame: bytes) -> None:
    if len(frame) != 5:
        raise ProtocolError(f"APP OTA ack length mismatch: {len(frame)}")
    if not check_crc16(frame):
        raise ProtocolError(f"APP OTA ack CRC mismatch: {hexs(frame)}")
    if frame[:3] != bytes((SLAVE_ADDR, FUNC_OTA, 0x00)):
        raise ProtocolError(f"unexpected APP OTA ack: {hexs(frame)}")


def read_ota_response(ser: serial.Serial, expected_cmd: int, timeout: float, cancel_event: Optional[threading.Event] = None) -> OtaResponse:
    head = read_exact(ser, 5, timeout, cancel_event=cancel_event)
    if head[0] != SLAVE_ADDR:
        raise ProtocolError(f"Bootloader response address mismatch: {hexs(head)}")
    if head[1] & 0x80:
        if not check_crc16(head):
            raise ProtocolError(f"Bootloader exception CRC mismatch: {hexs(head)}")
        return OtaResponse(command=expected_cmd, status=1, error=head[2], offset=0, is_exception=True)
    tail = read_exact(ser, 6, timeout, cancel_event=cancel_event)
    frame = head + tail
    if not check_crc16(frame):
        raise ProtocolError(f"Bootloader response CRC mismatch: {hexs(frame)}")
    if frame[1] != FUNC_OTA:
        raise ProtocolError(f"Bootloader response function mismatch: {hexs(frame)}")
    offset = struct.unpack(">I", frame[5:9])[0]
    return OtaResponse(command=frame[2], status=frame[3], error=frame[4], offset=offset)


def require_ota_ok(resp: OtaResponse, cmd: int, expected_offset: int) -> None:
    if resp.is_exception:
        raise ProtocolError(f"Bootloader exception response, err={resp.error}")
    if resp.command != cmd:
        raise ProtocolError(f"Bootloader command mismatch: got 0x{resp.command:02X}, expected 0x{cmd:02X}")
    if resp.status != 0 or resp.error != 0:
        raise ProtocolError(f"Bootloader OTA error: status={resp.status}, err={resp.error}, offset={resp.offset}")
    if resp.offset != expected_offset:
        raise ProtocolError(f"Bootloader offset mismatch: got {resp.offset}, expected {expected_offset}")


def request_app_to_bootloader(
    ser: serial.Serial,
    timeout: float = 1.5,
    allow_no_ack: bool = False,
    log_callback: Optional[LogCallback] = None,
    cancel_event: Optional[threading.Event] = None,
) -> bool:
    frame = build_app_ota_request()
    ser.reset_input_buffer()
    log_to(log_callback, f"[app-ota] tx: {hexs(frame)}")
    ser.write(frame)
    ser.flush()
    try:
        ack = read_exact(ser, 5, timeout, cancel_event=cancel_event)
        log_to(log_callback, f"[app-ota] rx: {hexs(ack)}")
        parse_app_ota_ack(ack)
        log_to(log_callback, "[app-ota] APP acknowledged, waiting reboot to Bootloader")
        time.sleep(1.2)
        ser.reset_input_buffer()
        return True
    except TimeoutError:
        if allow_no_ack:
            log_to(log_callback, "[app-ota] no APP ack; continue as device may already be in Bootloader")
            ser.reset_input_buffer()
            return False
        raise


def send_ota_firmware(
    ser: serial.Serial,
    firmware_path: str,
    version: int,
    skip_app_request: bool = False,
    log_callback: Optional[LogCallback] = None,
    progress_callback: Optional[ProgressCallback] = None,
    cancel_event: Optional[threading.Event] = None,
) -> None:
    with open(firmware_path, "rb") as f:
        firmware = f.read()
    validate_app_firmware(firmware)
    crc32 = crc32_bootloader(firmware)
    log_to(log_callback, f"[fw] file={firmware_path}")
    log_to(log_callback, f"[fw] size={len(firmware)} bytes, crc32=0x{crc32:08X}, version=0x{version & 0xFFFFFFFF:08X}")

    if progress_callback:
        progress_callback("准备", 0, len(firmware))

    if not skip_app_request:
        request_app_to_bootloader(ser, allow_no_ack=True, log_callback=log_callback, cancel_event=cancel_event)

    if cancel_event and cancel_event.is_set():
        raise InterruptedError("operation cancelled")

    start = build_ota_start(version, len(firmware), crc32)
    log_to(log_callback, f"[ota] START tx: {hexs(start)}")
    ser.write(start)
    ser.flush()
    resp = read_ota_response(ser, OTA_CMD_START, timeout=8.0, cancel_event=cancel_event)
    log_to(log_callback, f"[ota] START rx: cmd=0x{resp.command:02X} status={resp.status} err={resp.error} offset={resp.offset}")
    require_ota_ok(resp, OTA_CMD_START, 0)

    offset = 0
    last_report = time.monotonic()
    while offset < len(firmware):
        if cancel_event and cancel_event.is_set():
            raise InterruptedError("operation cancelled")
        chunk = firmware[offset : offset + OTA_PACKET_DATA_MAX]
        frame = build_ota_data(offset, chunk)
        ser.write(frame)
        ser.flush()
        resp = read_ota_response(ser, OTA_CMD_DATA, timeout=3.0, cancel_event=cancel_event)
        require_ota_ok(resp, OTA_CMD_DATA, offset + len(chunk))
        offset += len(chunk)
        now = time.monotonic()
        if now - last_report >= 0.2 or offset == len(firmware):
            pct = offset * 100.0 / len(firmware)
            log_to(log_callback, f"[ota] DATA {offset}/{len(firmware)} bytes ({pct:.1f}%)")
            if progress_callback:
                progress_callback("传输固件", offset, len(firmware))
            last_report = now

    end = build_ota_end()
    log_to(log_callback, f"[ota] END tx: {hexs(end)}")
    ser.write(end)
    ser.flush()
    resp = read_ota_response(ser, OTA_CMD_END, timeout=10.0, cancel_event=cancel_event)
    log_to(log_callback, f"[ota] END rx: cmd=0x{resp.command:02X} status={resp.status} err={resp.error} offset={resp.offset}")
    require_ota_ok(resp, OTA_CMD_END, len(firmware))
    if progress_callback:
        progress_callback("等待APP启动", len(firmware), len(firmware))
    log_to(log_callback, "[ota] END OK, waiting Bootloader reset and APP start")
    time.sleep(2.0)


def send_abort(ser: serial.Serial, log_callback: Optional[LogCallback] = None) -> None:
    frame = build_ota_abort()
    log_to(log_callback, f"[abort] tx: {hexs(frame)}")
    ser.write(frame)
    ser.flush()
    try:
        resp = read_ota_response(ser, OTA_CMD_ABORT, timeout=1.0)
        log_to(log_callback, f"[abort] rx: cmd=0x{resp.command:02X} status={resp.status} err={resp.error} offset={resp.offset}")
    except Exception as exc:
        log_to(log_callback, f"[abort] no valid response: {exc}")


def send_jump_app(ser: serial.Serial, log_callback: Optional[LogCallback] = None) -> None:
    frame = build_jump_app()
    log_to(log_callback, f"[jump-app] tx: {hexs(frame)}")
    ser.reset_input_buffer()
    ser.write(frame)
    ser.flush()
    time.sleep(1.0)
    log_to(log_callback, "[jump-app] command sent; valid APP jumps directly without normal response")


def get_serial_ports() -> list[SerialPortInfo]:
    require_pyserial()
    return [SerialPortInfo(p.device, p.description, p.hwid) for p in list_ports.comports()]


def list_serial_ports(log_callback: Optional[LogCallback] = None) -> None:
    ports = get_serial_ports()
    if not ports:
        log_to(log_callback, "No serial ports found")
        return
    for p in ports:
        log_to(log_callback, f"{p.device:10s} {p.description} {p.hwid}")


def open_data_port(port: str, baud: int, log_callback: Optional[LogCallback] = None) -> serial.Serial:
    require_pyserial()
    ser = serial.Serial(port, baudrate=baud, bytesize=8, parity="N", stopbits=1, timeout=0.05, write_timeout=2)
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    log_to(log_callback, f"[data] opened {port} @ {baud}")
    return ser
