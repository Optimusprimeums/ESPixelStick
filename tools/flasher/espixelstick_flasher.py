#!/usr/bin/env python3
"""ESPixelStick ESP32-S3 N16R8 GUI firmware flasher."""

from __future__ import annotations

import contextlib
import io
import os
import queue
import shutil
import tempfile
import threading
import tkinter as tk
import zipfile
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

import esptool
from serial.tools import list_ports

APP_TITLE = "ESPixelStick ESP32-S3 N16R8 Flasher"
TARGET = "esp32s3_devkitc"
FLASH_LAYOUT = (
    (0x0000, "bootloader.bin"),
    (0x8000, "partitions.bin"),
    (0xE000, "boot_app0.bin"),
    (0x10000, "app.bin"),
)


class QueueWriter(io.TextIOBase):
    def __init__(self, output_queue: queue.Queue):
        self.output_queue = output_queue

    def write(self, text):
        if text:
            self.output_queue.put(("log", text))
        return len(text)

    def flush(self):
        return None


class FlasherApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP_TITLE)
        self.geometry("850x650")
        self.minsize(760, 560)
        self.output_queue = queue.Queue()
        self.temp_dir = None
        self.images = {}
        self.busy = False

        self.port_var = tk.StringVar()
        self.firmware_var = tk.StringVar()
        self.erase_var = tk.BooleanVar(value=False)
        self.status_var = tk.StringVar(value="Select a firmware ZIP and serial port.")
        self.progress_var = tk.DoubleVar(value=0)

        self._build_ui()
        self.refresh_ports()
        self.after(100, self._drain_queue)

    def _build_ui(self):
        root = ttk.Frame(self, padding=12)
        root.pack(fill="both", expand=True)
        root.columnconfigure(1, weight=1)
        root.rowconfigure(6, weight=1)

        ttk.Label(root, text=APP_TITLE, font=("Segoe UI", 16, "bold")).grid(
            row=0, column=0, columnspan=3, sticky="w", pady=(0, 12)
        )

        ttk.Label(root, text="Serial port").grid(row=1, column=0, sticky="w")
        self.port_combo = ttk.Combobox(root, textvariable=self.port_var, state="readonly")
        self.port_combo.grid(row=1, column=1, sticky="ew", padx=8)
        ttk.Button(root, text="Refresh", command=self.refresh_ports).grid(row=1, column=2)

        ttk.Label(root, text="Firmware ZIP").grid(row=2, column=0, sticky="w", pady=(8, 0))
        ttk.Entry(root, textvariable=self.firmware_var, state="readonly").grid(
            row=2, column=1, sticky="ew", padx=8, pady=(8, 0)
        )
        ttk.Button(root, text="Browse...", command=self.choose_firmware).grid(
            row=2, column=2, pady=(8, 0)
        )

        options = ttk.Frame(root)
        options.grid(row=3, column=0, columnspan=3, sticky="ew", pady=10)
        ttk.Checkbutton(
            options,
            text="Erase entire flash first (removes saved configuration)",
            variable=self.erase_var,
        ).pack(side="left")

        buttons = ttk.Frame(root)
        buttons.grid(row=4, column=0, columnspan=3, sticky="ew")
        self.identify_button = ttk.Button(buttons, text="Identify ESP32-S3", command=self.identify)
        self.identify_button.pack(side="left")
        self.flash_button = ttk.Button(buttons, text="Flash & Verify", command=self.flash)
        self.flash_button.pack(side="left", padx=8)

        ttk.Progressbar(root, variable=self.progress_var, maximum=100).grid(
            row=5, column=0, columnspan=3, sticky="ew", pady=(12, 4)
        )
        ttk.Label(root, textvariable=self.status_var).grid(
            row=6, column=0, columnspan=3, sticky="nw", pady=(0, 4)
        )

        log_frame = ttk.LabelFrame(root, text="Flasher log", padding=4)
        log_frame.grid(row=7, column=0, columnspan=3, sticky="nsew")
        root.rowconfigure(7, weight=1)
        log_frame.rowconfigure(0, weight=1)
        log_frame.columnconfigure(0, weight=1)
        self.log = tk.Text(log_frame, wrap="word", height=18, state="disabled")
        self.log.grid(row=0, column=0, sticky="nsew")
        scroll = ttk.Scrollbar(log_frame, command=self.log.yview)
        scroll.grid(row=0, column=1, sticky="ns")
        self.log.configure(yscrollcommand=scroll.set)

        ttk.Label(
            root,
            text="Target: ESP32-S3 DevKitC N16R8 • bootloader 0x0000 • partitions 0x8000 • boot_app0 0xE000 • app 0x10000",
        ).grid(row=8, column=0, columnspan=3, sticky="w", pady=(8, 0))

    def refresh_ports(self):
        ports = sorted(list_ports.comports(), key=lambda p: p.device)
        labels = [p.device for p in ports]
        self.port_combo["values"] = labels
        if self.port_var.get() not in labels:
            self.port_var.set(labels[0] if labels else "")
        self.status_var.set(
            f"Found {len(labels)} serial port(s)." if labels else "No serial ports found."
        )

    def choose_firmware(self):
        filename = filedialog.askopenfilename(
            title="Select ESPixelStick firmware artifact",
            filetypes=[("ZIP archives", "*.zip"), ("All files", "*.*")],
        )
        if not filename:
            return
        try:
            self._load_firmware(Path(filename))
        except Exception as exc:
            messagebox.showerror("Invalid firmware", str(exc))
            self.status_var.set("Firmware validation failed.")
            return
        self.firmware_var.set(filename)
        self.status_var.set("Firmware validated for ESP32-S3 N16R8.")
        self._append_log("Firmware package validated:\n")
        for address, key in FLASH_LAYOUT:
            self._append_log(f"  0x{address:05X}  {self.images[key].name}\n")

    def _load_firmware(self, archive: Path):
        if not zipfile.is_zipfile(archive):
            raise ValueError("The selected file is not a valid ZIP archive.")
        if self.temp_dir:
            shutil.rmtree(self.temp_dir, ignore_errors=True)
        self.temp_dir = tempfile.mkdtemp(prefix="espixelstick-flasher-")
        with zipfile.ZipFile(archive) as zf:
            members = [m for m in zf.infolist() if not m.is_dir()]
            unsafe = [m.filename for m in members if ".." in Path(m.filename).parts]
            if unsafe:
                raise ValueError("Firmware ZIP contains an unsafe path.")
            zf.extractall(self.temp_dir)

        files = list(Path(self.temp_dir).rglob("*"))
        files = [p for p in files if p.is_file()]
        self.images = {
            "bootloader.bin": self._find_unique(files, f"{TARGET}-bootloader.bin"),
            "partitions.bin": self._find_unique(files, f"{TARGET}-partitions.bin"),
            "boot_app0.bin": self._find_unique(files, "boot_app0.bin"),
            "app.bin": self._find_unique(files, f"{TARGET}-app.bin"),
        }
        for path in self.images.values():
            if path.stat().st_size == 0:
                raise ValueError(f"Firmware image is empty: {path.name}")

    @staticmethod
    def _find_unique(files, filename):
        matches = [p for p in files if p.name == filename]
        if len(matches) != 1:
            raise ValueError(
                f"Expected exactly one {filename}; found {len(matches)}. "
                "Select the firmware-binary-esp32s3_devkitc artifact."
            )
        return matches[0]

    def identify(self):
        if not self._validate_port():
            return
        self._run_worker(self._identify_worker, "Identifying ESP32-S3...")

    def flash(self):
        if not self._validate_port():
            return
        if not self.images:
            messagebox.showwarning("Firmware required", "Select and validate a firmware ZIP first.")
            return
        if self.erase_var.get():
            if not messagebox.askyesno(
                "Erase flash?",
                "Erase entire flash before programming? This removes saved ESPixelStick configuration.",
            ):
                return
        self._run_worker(self._flash_worker, "Preparing to flash...")

    def _validate_port(self):
        if not self.port_var.get():
            messagebox.showwarning("Serial port required", "Select an ESP32-S3 serial port.")
            return False
        return True

    def _run_worker(self, target, status):
        if self.busy:
            return
        self.busy = True
        self.progress_var.set(0)
        self.status_var.set(status)
        self.identify_button.configure(state="disabled")
        self.flash_button.configure(state="disabled")
        threading.Thread(target=target, daemon=True).start()

    def _esptool(self, args):
        writer = QueueWriter(self.output_queue)
        with contextlib.redirect_stdout(writer), contextlib.redirect_stderr(writer):
            esptool.main(args)

    def _identify_worker(self):
        try:
            self._esptool(["--chip", "esp32s3", "--port", self.port_var.get(), "chip-id"])
            self.output_queue.put(("done", "ESP32-S3 identified successfully."))
        except BaseException as exc:
            self.output_queue.put(("error", f"Identification failed: {exc}"))

    def _flash_worker(self):
        port = self.port_var.get()
        try:
            self.output_queue.put(("status", "Verifying connected chip..."))
            self._esptool(["--chip", "esp32s3", "--port", port, "chip-id"])
            self.output_queue.put(("progress", 10))

            if self.erase_var.get():
                self.output_queue.put(("status", "Erasing flash..."))
                self._esptool(["--chip", "esp32s3", "--port", port, "erase-flash"])
                self.output_queue.put(("progress", 25))

            args = [
                "--chip", "esp32s3",
                "--port", port,
                "--baud", "921600",
                "--after", "hard-reset",
                "write-flash",
                "--flash-mode", "qio",
                "--flash-freq", "80m",
                "--flash-size", "16MB",
            ]
            for address, key in FLASH_LAYOUT:
                args.extend([hex(address), str(self.images[key])])

            self.output_queue.put(("status", "Flashing and verifying firmware..."))
            self.output_queue.put(("progress", 30))
            self._esptool(args)
            self.output_queue.put(("progress", 100))
            self.output_queue.put(("done", "Flash and verification completed successfully."))
        except BaseException as exc:
            self.output_queue.put(("error", f"Flash failed: {exc}"))

    def _append_log(self, text):
        self.log.configure(state="normal")
        self.log.insert("end", text)
        self.log.see("end")
        self.log.configure(state="disabled")

    def _drain_queue(self):
        try:
            while True:
                kind, value = self.output_queue.get_nowait()
                if kind == "log":
                    self._append_log(value)
                elif kind == "status":
                    self.status_var.set(value)
                elif kind == "progress":
                    self.progress_var.set(value)
                elif kind in ("done", "error"):
                    self.busy = False
                    self.identify_button.configure(state="normal")
                    self.flash_button.configure(state="normal")
                    self.status_var.set(value)
                    self._append_log("\n" + value + "\n")
                    if kind == "error":
                        messagebox.showerror("ESPixelStick Flasher", value)
        except queue.Empty:
            pass
        self.after(100, self._drain_queue)


if __name__ == "__main__":
    app = FlasherApp()
    app.mainloop()
