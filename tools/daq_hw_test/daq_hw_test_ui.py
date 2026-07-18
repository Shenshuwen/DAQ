#!/usr/bin/env python3
"""Tkinter UI for the DAQ 29_A/29_B half-hardware test tool."""

from __future__ import annotations

import os
import queue
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from daq_hw_test_core import (
    DEFAULT_BAUD,
    DebugReader,
    ProtocolError,
    get_serial_ports,
    inspect_app_firmware,
    open_data_port,
    read_adc_once,
    request_app_to_bootloader,
    send_abort,
    send_jump_app,
    send_ota_firmware,
    test_bad_crc_is_silent,
)


class DaqHardwareTestApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("DAQ半实物测试工具")
        self.geometry("1120x760")
        self.minsize(980, 660)

        self.log_queue: queue.Queue[tuple[str, object]] = queue.Queue()
        self.worker_lock = threading.Lock()
        self.worker: threading.Thread | None = None
        self.cancel_event = threading.Event()
        self.loop_stop_event = threading.Event()
        self.debug_stop_event = threading.Event()
        self.debug_reader: DebugReader | None = None
        self.ser = None
        self.port_map: dict[str, str] = {}

        self.data_port_var = tk.StringVar()
        self.debug_port_var = tk.StringVar()
        self.baud_var = tk.StringVar(value=str(DEFAULT_BAUD))
        self.connected_var = tk.StringVar(value="未连接")
        self.debug_var = tk.StringVar(value="调试监听未启动")
        self.interval_var = tk.StringVar(value="1.0")
        self.firmware_var = tk.StringVar()
        self.version_var = tk.StringVar(value="1")
        self.skip_app_request_var = tk.BooleanVar(value=False)
        self.firmware_info_var = tk.StringVar(value="未选择固件")
        self.ota_stage_var = tk.StringVar(value="空闲")
        self.progress_var = tk.DoubleVar(value=0.0)
        self.ch_vars = [tk.StringVar(value="--") for _ in range(8)]

        self._configure_style()
        self._build_ui()
        self.refresh_ports()
        self.after(80, self._drain_queue)
        self.protocol("WM_DELETE_WINDOW", self.on_close)

    def _configure_style(self) -> None:
        style = ttk.Style(self)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure("TButton", padding=(10, 5))
        style.configure("Danger.TButton", foreground="#9b1c1c")
        style.configure("Accent.TButton", foreground="#0f4c81")
        style.configure("Status.TLabel", foreground="#314155")
        style.configure("Value.TLabel", font=("Consolas", 12, "bold"))

    def _build_ui(self) -> None:
        root = ttk.Frame(self, padding=12)
        root.pack(fill=tk.BOTH, expand=True)
        root.columnconfigure(0, weight=0)
        root.columnconfigure(1, weight=1)
        root.rowconfigure(0, weight=1)

        left = ttk.Frame(root)
        left.grid(row=0, column=0, sticky="nsw", padx=(0, 12))

        right = ttk.Frame(root)
        right.grid(row=0, column=1, sticky="nsew")
        right.rowconfigure(1, weight=1)
        right.columnconfigure(0, weight=1)

        self._build_serial_frame(left)
        self._build_adc_frame(left)
        self._build_ota_frame(left)
        self._build_log_frame(right)

    def _build_serial_frame(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="串口配置", padding=10)
        frame.pack(fill=tk.X, pady=(0, 10))
        frame.columnconfigure(1, weight=1)

        ttk.Label(frame, text="数据口 USART2").grid(row=0, column=0, sticky="w", pady=3)
        self.data_port_combo = ttk.Combobox(frame, textvariable=self.data_port_var, width=28, state="readonly")
        self.data_port_combo.grid(row=0, column=1, sticky="ew", pady=3)

        ttk.Label(frame, text="调试口 USART1").grid(row=1, column=0, sticky="w", pady=3)
        self.debug_port_combo = ttk.Combobox(frame, textvariable=self.debug_port_var, width=28, state="readonly")
        self.debug_port_combo.grid(row=1, column=1, sticky="ew", pady=3)

        ttk.Label(frame, text="波特率").grid(row=2, column=0, sticky="w", pady=3)
        ttk.Entry(frame, textvariable=self.baud_var, width=12).grid(row=2, column=1, sticky="w", pady=3)

        buttons = ttk.Frame(frame)
        buttons.grid(row=3, column=0, columnspan=2, sticky="ew", pady=(8, 3))
        ttk.Button(buttons, text="刷新串口", command=self.refresh_ports).pack(side=tk.LEFT, padx=(0, 6))
        self.connect_button = ttk.Button(buttons, text="连接数据口", command=self.toggle_data_port, style="Accent.TButton")
        self.connect_button.pack(side=tk.LEFT, padx=(0, 6))
        self.debug_button = ttk.Button(buttons, text="启动调试监听", command=self.toggle_debug_reader)
        self.debug_button.pack(side=tk.LEFT)

        ttk.Label(frame, textvariable=self.connected_var, style="Status.TLabel").grid(row=4, column=0, columnspan=2, sticky="w", pady=(5, 0))
        ttk.Label(frame, textvariable=self.debug_var, style="Status.TLabel").grid(row=5, column=0, columnspan=2, sticky="w")

    def _build_adc_frame(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="APP / Modbus 测试", padding=10)
        frame.pack(fill=tk.X, pady=(0, 10))

        grid = ttk.Frame(frame)
        grid.pack(fill=tk.X, pady=(0, 8))
        for i, var in enumerate(self.ch_vars):
            ttk.Label(grid, text=f"CH{i}").grid(row=i // 4 * 2, column=i % 4, sticky="w", padx=(0, 18))
            ttk.Label(grid, textvariable=var, style="Value.TLabel", width=8).grid(row=i // 4 * 2 + 1, column=i % 4, sticky="w", padx=(0, 18), pady=(0, 5))

        buttons = ttk.Frame(frame)
        buttons.pack(fill=tk.X)
        ttk.Button(buttons, text="读取ADC一次", command=self.read_adc_once_ui).pack(side=tk.LEFT, padx=(0, 6))
        self.loop_button = ttk.Button(buttons, text="循环读取", command=self.toggle_adc_loop)
        self.loop_button.pack(side=tk.LEFT, padx=(0, 6))
        ttk.Label(buttons, text="间隔(s)").pack(side=tk.LEFT)
        ttk.Entry(buttons, textvariable=self.interval_var, width=6).pack(side=tk.LEFT, padx=(4, 8))
        ttk.Button(buttons, text="坏CRC测试", command=self.bad_crc_ui).pack(side=tk.LEFT)

    def _build_ota_frame(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="OTA / Bootloader", padding=10)
        frame.pack(fill=tk.BOTH, expand=False)
        frame.columnconfigure(1, weight=1)

        ttk.Label(frame, text="APP固件").grid(row=0, column=0, sticky="w", pady=3)
        ttk.Entry(frame, textvariable=self.firmware_var, width=34).grid(row=0, column=1, sticky="ew", pady=3)
        ttk.Button(frame, text="选择", command=self.select_firmware).grid(row=0, column=2, padx=(6, 0), pady=3)

        ttk.Label(frame, text="版本号").grid(row=1, column=0, sticky="w", pady=3)
        ttk.Entry(frame, textvariable=self.version_var, width=14).grid(row=1, column=1, sticky="w", pady=3)

        ttk.Checkbutton(frame, text="设备已在Bootloader，直接OTA", variable=self.skip_app_request_var).grid(row=2, column=0, columnspan=3, sticky="w", pady=3)
        ttk.Label(frame, textvariable=self.firmware_info_var, style="Status.TLabel", wraplength=420).grid(row=3, column=0, columnspan=3, sticky="w", pady=(4, 8))

        buttons = ttk.Frame(frame)
        buttons.grid(row=4, column=0, columnspan=3, sticky="ew", pady=(0, 8))
        ttk.Button(buttons, text="APP进入Bootloader", command=self.app_ota_request_ui).pack(side=tk.LEFT, padx=(0, 6))
        ttk.Button(buttons, text="完整OTA", command=self.ota_ui, style="Accent.TButton").pack(side=tk.LEFT, padx=(0, 6))
        ttk.Button(buttons, text="ABORT", command=self.abort_ui, style="Danger.TButton").pack(side=tk.LEFT, padx=(0, 6))
        ttk.Button(buttons, text="JUMP APP", command=self.jump_app_ui).pack(side=tk.LEFT)

        self.progress = ttk.Progressbar(frame, variable=self.progress_var, maximum=100)
        self.progress.grid(row=5, column=0, columnspan=3, sticky="ew", pady=(0, 4))
        ttk.Label(frame, textvariable=self.ota_stage_var, style="Status.TLabel").grid(row=6, column=0, columnspan=3, sticky="w")

    def _build_log_frame(self, parent: ttk.Frame) -> None:
        top = ttk.Frame(parent)
        top.grid(row=0, column=0, sticky="ew", pady=(0, 6))
        top.columnconfigure(0, weight=1)
        ttk.Label(top, text="运行日志").grid(row=0, column=0, sticky="w")
        ttk.Button(top, text="清空日志", command=self.clear_log).grid(row=0, column=1, sticky="e")

        log_frame = ttk.Frame(parent)
        log_frame.grid(row=1, column=0, sticky="nsew")
        log_frame.rowconfigure(0, weight=1)
        log_frame.columnconfigure(0, weight=1)
        self.log_text = tk.Text(log_frame, wrap="word", font=("Consolas", 10), height=28)
        self.log_text.grid(row=0, column=0, sticky="nsew")
        self.log_line_count = 0
        scroll = ttk.Scrollbar(log_frame, command=self.log_text.yview)
        scroll.grid(row=0, column=1, sticky="ns")
        self.log_text.configure(yscrollcommand=scroll.set)

    def log(self, message: str) -> None:
        self.log_queue.put(("log", message))

    def progress_update(self, stage: str, done: int, total: int) -> None:
        self.log_queue.put(("progress", (stage, done, total)))

    def _drain_queue(self) -> None:
        processed = 0
        # UART1 can print continuously. Limit one UI tick so Tkinter stays responsive.
        while processed < 120:
            try:
                kind, payload = self.log_queue.get_nowait()
            except queue.Empty:
                break
            processed += 1
            if kind == "log":
                self._append_log(str(payload))
            elif kind == "progress":
                stage, done, total = payload  # type: ignore[misc]
                percent = 0.0 if total == 0 else done * 100.0 / total
                self.progress_var.set(percent)
                self.ota_stage_var.set(f"{stage}: {done}/{total} ({percent:.1f}%)")
            elif kind == "adc":
                values = payload  # type: ignore[assignment]
                for i, value in enumerate(values):
                    self.ch_vars[i].set(str(value))
            elif kind == "loop_done":
                self.loop_button.configure(text="循环读取")
            elif kind == "worker_done":
                self._set_busy(False)
        self.after(80, self._drain_queue)

    def _append_log(self, message: str) -> None:
        stamp = time.strftime("%H:%M:%S")
        self.log_text.insert(tk.END, f"{stamp} {message}\n")
        self.log_line_count += 1
        if self.log_line_count > 2000:
            self.log_text.delete("1.0", "501.0")
            self.log_line_count -= 500
        self.log_text.see(tk.END)

    def clear_log(self) -> None:
        self.log_text.delete("1.0", tk.END)
        self.log_line_count = 0

    def refresh_ports(self) -> None:
        try:
            ports = get_serial_ports()
        except Exception as exc:
            messagebox.showerror("串口错误", str(exc))
            return
        self.port_map = {p.label: p.device for p in ports}
        labels = list(self.port_map.keys())
        self.data_port_combo["values"] = labels
        self.debug_port_combo["values"] = [""] + labels
        if labels and not self.data_port_var.get():
            self.data_port_var.set(labels[0])
        if not self.debug_port_var.get():
            self.debug_port_var.set("")
        self.log(f"[tool] found {len(labels)} serial port(s)")

    def _selected_data_port(self) -> str:
        label = self.data_port_var.get()
        return self.port_map.get(label, label)

    def _selected_debug_port(self) -> str:
        label = self.debug_port_var.get()
        return self.port_map.get(label, label)

    def _baud(self) -> int:
        try:
            value = int(self.baud_var.get(), 0)
        except ValueError as exc:
            raise ValueError("波特率必须是整数") from exc
        if value <= 0:
            raise ValueError("波特率必须大于0")
        return value

    def _validate_distinct_ports(self) -> None:
        data = self._selected_data_port()
        debug = self._selected_debug_port()
        if debug and data == debug:
            raise ValueError("数据口和调试口不能选择同一个COM口")

    def toggle_data_port(self) -> None:
        if self.ser and self.ser.is_open:
            self.ser.close()
            self.ser = None
            self.connected_var.set("未连接")
            self.connect_button.configure(text="连接数据口")
            self.log("[data] closed")
            return
        try:
            self._validate_distinct_ports()
            port = self._selected_data_port()
            if not port:
                raise ValueError("请选择数据口")
            self.ser = open_data_port(port, self._baud(), log_callback=self.log)
            self.connected_var.set(f"数据口已连接: {port}")
            self.connect_button.configure(text="断开数据口")
        except Exception as exc:
            messagebox.showerror("连接失败", str(exc))

    def toggle_debug_reader(self) -> None:
        if self.debug_reader:
            self.debug_stop_event.set()
            self.debug_reader.join(timeout=1.0)
            self.debug_reader = None
            self.debug_var.set("调试监听未启动")
            self.debug_button.configure(text="启动调试监听")
            self.log("[debug] closed")
            return
        try:
            self._validate_distinct_ports()
            port = self._selected_debug_port()
            if not port:
                raise ValueError("请选择调试口")
            self.debug_stop_event = threading.Event()
            self.debug_reader = DebugReader(port, self._baud(), self.debug_stop_event, log_callback=self.log)
            self.debug_reader.start()
            self.debug_var.set(f"调试监听中: {port}")
            self.debug_button.configure(text="停止调试监听")
        except Exception as exc:
            messagebox.showerror("调试口错误", str(exc))

    def ensure_data_port(self):
        if self.ser and self.ser.is_open:
            return self.ser
        port = self._selected_data_port()
        if not port:
            raise ValueError("请选择数据口")
        self.ser = open_data_port(port, self._baud(), log_callback=self.log)
        self.connected_var.set(f"数据口已连接: {port}")
        self.connect_button.configure(text="断开数据口")
        return self.ser

    def run_worker(self, title: str, target) -> None:
        if self.worker and self.worker.is_alive():
            messagebox.showwarning("操作进行中", "请等待当前操作完成，或先ABORT。")
            return
        self.cancel_event.clear()
        self._set_busy(True)

        def wrapper() -> None:
            try:
                with self.worker_lock:
                    self.log(f"[tool] start: {title}")
                    target()
                    self.log(f"[tool] done: {title}")
            except Exception as exc:
                self.log(f"[tool] ERROR: {exc}")
                message = str(exc)
                self.log_queue.put(("log", f"[tool] {message}"))
            finally:
                self.log_queue.put(("worker_done", None))

        self.worker = threading.Thread(target=wrapper, daemon=True)
        self.worker.start()

    def _set_busy(self, busy: bool) -> None:
        state = tk.DISABLED if busy else tk.NORMAL
        for widget in (self.connect_button, self.data_port_combo, self.debug_port_combo):
            widget.configure(state=state if widget is self.connect_button else ("disabled" if busy else "readonly"))

    def read_adc_once_ui(self) -> None:
        def task() -> None:
            values = read_adc_once(self.ensure_data_port(), log_callback=self.log)
            self.log_queue.put(("adc", values))
        self.run_worker("读取ADC一次", task)

    def toggle_adc_loop(self) -> None:
        if self.worker and self.worker.is_alive() and not self.loop_stop_event.is_set():
            self.loop_stop_event.set()
            self.loop_button.configure(text="循环读取")
            self.log("[loop] stop requested")
            return

        self.loop_stop_event.clear()
        self.loop_button.configure(text="停止循环")

        def task() -> None:
            ser = self.ensure_data_port()
            interval = float(self.interval_var.get())
            if interval <= 0:
                raise ValueError("循环间隔必须大于0")
            try:
                while not self.loop_stop_event.is_set():
                    try:
                        values = read_adc_once(ser, verbose=False, log_callback=self.log)
                        self.log_queue.put(("adc", values))
                    except Exception as exc:
                        self.log(f"[loop] read failed: {exc}")
                    self.loop_stop_event.wait(interval)
            finally:
                self.log_queue.put(("loop_done", None))

        self.run_worker("循环读取ADC", task)

    def bad_crc_ui(self) -> None:
        def task() -> None:
            test_bad_crc_is_silent(self.ensure_data_port(), log_callback=self.log)
        self.run_worker("坏CRC测试", task)

    def app_ota_request_ui(self) -> None:
        if not messagebox.askyesno("确认复位", "该操作会让APP写OTAInfo并复位进入Bootloader。继续？"):
            return

        def task() -> None:
            request_app_to_bootloader(self.ensure_data_port(), log_callback=self.log, cancel_event=self.cancel_event)
        self.run_worker("APP进入Bootloader", task)

    def select_firmware(self) -> None:
        path = filedialog.askopenfilename(title="选择APP固件", filetypes=[("固件文件", "*.bin *.hex"), ("所有文件", "*.*")])
        if not path:
            return
        self.firmware_var.set(path)
        self.inspect_firmware(path)

    def inspect_firmware(self, path: str) -> None:
        try:
            info = inspect_app_firmware(path)
            self.firmware_info_var.set(
                f"OK | {os.path.basename(path)} | size={info.size} bytes | crc32=0x{info.crc32:08X} | "
                f"SP=0x{info.initial_sp:08X} | Reset=0x{info.reset_vector:08X}"
            )
            self.log(f"[fw] {self.firmware_info_var.get()}")
        except Exception as exc:
            self.firmware_info_var.set(f"固件校验失败: {exc}")
            self.log(f"[fw] invalid: {exc}")

    def _version(self) -> int:
        try:
            return int(self.version_var.get(), 0)
        except ValueError as exc:
            raise ValueError("版本号必须是整数，例如 1 或 0x00000001") from exc

    def ota_ui(self) -> None:
        path = self.firmware_var.get().strip()
        if not path:
            messagebox.showwarning("缺少固件", "请先选择APP .bin固件。")
            return
        try:
            inspect_app_firmware(path)
        except Exception as exc:
            messagebox.showerror("固件校验失败", str(exc))
            return
        if not messagebox.askyesno("确认OTA", "OTA会擦写APP区。请确认已选择正确的0x08004000链接APP固件。继续？"):
            return

        def task() -> None:
            ser = self.ensure_data_port()
            send_ota_firmware(
                ser,
                path,
                self._version(),
                skip_app_request=self.skip_app_request_var.get(),
                log_callback=self.log,
                progress_callback=self.progress_update,
                cancel_event=self.cancel_event,
            )
            self.log("[verify] reading ADC after OTA")
            values = read_adc_once(ser, timeout=2.0, log_callback=self.log)
            self.log_queue.put(("adc", values))
        self.progress_var.set(0)
        self.ota_stage_var.set("开始OTA")
        self.run_worker("完整OTA", task)

    def abort_ui(self) -> None:
        self.cancel_event.set()

        def task() -> None:
            send_abort(self.ensure_data_port(), log_callback=self.log)
        if self.worker and self.worker.is_alive():
            self.log("[abort] cancel requested; ABORT will be attempted")
            try:
                send_abort(self.ensure_data_port(), log_callback=self.log)
            except Exception as exc:
                self.log(f"[abort] failed: {exc}")
        else:
            self.run_worker("ABORT", task)

    def jump_app_ui(self) -> None:
        def task() -> None:
            send_jump_app(self.ensure_data_port(), log_callback=self.log)
        self.run_worker("JUMP APP", task)

    def on_close(self) -> None:
        self.cancel_event.set()
        self.loop_stop_event.set()
        self.debug_stop_event.set()
        if self.debug_reader:
            self.debug_reader.join(timeout=1.0)
        if self.ser and self.ser.is_open:
            self.ser.close()
        self.destroy()


def main() -> None:
    app = DaqHardwareTestApp()
    app.mainloop()


if __name__ == "__main__":
    main()
