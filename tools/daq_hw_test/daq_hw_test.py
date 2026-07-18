#!/usr/bin/env python3
"""Command-line entry for the DAQ 29_A/29_B half-hardware tester."""

from __future__ import annotations

import argparse
import sys
import threading
import time

from daq_hw_test_core import (
    DEFAULT_BAUD,
    DebugReader,
    list_serial_ports,
    open_data_port,
    read_adc_once,
    request_app_to_bootloader,
    send_abort,
    send_jump_app,
    send_ota_firmware,
    test_bad_crc_is_silent,
)


def parse_int(text: str) -> int:
    return int(text, 0)


def main() -> int:
    parser = argparse.ArgumentParser(description="29_A APP / 29_B Bootloader hardware workflow tester over CH340 TTL UART")
    parser.add_argument("--list-ports", action="store_true", help="list available COM ports and exit")
    parser.add_argument("--data-port", help="CH340 connected to STM32 USART2 PA2/PA3")
    parser.add_argument("--debug-port", help="optional CH340 connected to STM32 USART1 PA9/PA10")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="serial baud rate, default 115200")
    parser.add_argument("--loop", action="store_true", help="read ADC repeatedly")
    parser.add_argument("--interval", type=float, default=1.0, help="ADC loop interval seconds")
    parser.add_argument("--read-adc", action="store_true", help="read ADC once")
    parser.add_argument("--bad-crc", action="store_true", help="send a bad CRC read frame and expect no response")
    parser.add_argument("--app-ota-request", action="store_true", help="request APP to reboot into Bootloader; this resets the board")
    parser.add_argument("--ota", metavar="APP_BIN", help="download APP binary with Bootloader OTA protocol")
    parser.add_argument("--version", type=parse_int, default=1, help="firmware version for START frame, decimal or 0x... default 1")
    parser.add_argument("--skip-app-request", action="store_true", help="OTA directly, for device already in Bootloader")
    parser.add_argument("--abort", action="store_true", help="send Bootloader OTA ABORT")
    parser.add_argument("--jump-app", action="store_true", help="send Bootloader 0x42 jump app command")
    args = parser.parse_args()

    if args.list_ports:
        list_serial_ports()
        return 0

    if not args.data_port:
        parser.error("--data-port is required unless --list-ports is used")

    stop_event = threading.Event()
    debug_reader = None
    if args.debug_port:
        debug_reader = DebugReader(args.debug_port, args.baud, stop_event)
        debug_reader.start()
        time.sleep(0.2)

    ser = None
    try:
        ser = open_data_port(args.data_port, args.baud)

        did_action = False
        if args.read_adc:
            read_adc_once(ser)
            did_action = True
        if args.bad_crc:
            test_bad_crc_is_silent(ser)
            did_action = True
        if args.loop:
            did_action = True
            print("[loop] press Ctrl+C to stop")
            while True:
                try:
                    read_adc_once(ser, verbose=False)
                except Exception as exc:
                    print(f"[loop] read failed: {exc}")
                time.sleep(args.interval)
        if args.app_ota_request:
            request_app_to_bootloader(ser)
            did_action = True
        if args.abort:
            send_abort(ser)
            did_action = True
        if args.jump_app:
            send_jump_app(ser)
            did_action = True
        if args.ota:
            try:
                send_ota_firmware(ser, args.ota, args.version, skip_app_request=args.skip_app_request)
                print("[verify] reading ADC after OTA")
                read_adc_once(ser, timeout=2.0)
            except Exception:
                print("[ota] failed; trying ABORT if Bootloader is still responsive")
                send_abort(ser)
                raise
            did_action = True

        if not did_action:
            print("No action selected. Try --read-adc, --loop, --bad-crc, --app-ota-request, --ota APP_BIN, --abort, or --jump-app.")
            return 2
        return 0
    except KeyboardInterrupt:
        print("\nInterrupted")
        return 130
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    finally:
        stop_event.set()
        if ser and ser.is_open:
            ser.close()
        if debug_reader:
            debug_reader.join(timeout=1.0)


if __name__ == "__main__":
    raise SystemExit(main())
