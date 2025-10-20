#!/usr/bin/env python3
import os
import struct
import select
import time
import threading
import tkinter as tk
from tkinter import ttk, messagebox
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure

DEVICE = "/dev/simtemp"
SYSFS_BASE = "/sys/class/misc/simtemp"
record_fmt = "Qii"  # timestamp_ns, temp_mC, flags
record_size = struct.calcsize(record_fmt)

MAX_POINTS = 50 


class SimTempGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("SimTemp Monitor")

        # Internal state
        self.temps = []
        self.alert_flags = []
        self.threshold_mC = self.read_sysfs("threshold_mC")
        self.mode = self.read_sysfs("mode")
        self.running = True

        # UI
        self.create_controls()
        self.create_plot()
        self.create_stats()

        # Lecture
        self.thread = threading.Thread(target=self.poll_device, daemon=True)
        self.thread.start()

        # Periodic stats update
        self.update_stats()

    # ---------- SYSFS HELPERS ----------
    def sysfs_path(self, name):
        return os.path.join(SYSFS_BASE, name)

    def read_sysfs(self, name):
        try:
            with open(self.sysfs_path(name), "r") as f:
                return f.read().strip()
        except Exception:
            return "N/A"

    def write_sysfs(self, name, value):
        try:
            with open(self.sysfs_path(name), "w") as f:
                f.write(str(value))
        except Exception as e:
            messagebox.showerror("Error", f"Failed to write {name}: {e}")

    # ---------- UI ----------
    def create_controls(self):
        frame = ttk.LabelFrame(self.root, text="Controls")
        frame.pack(fill="x", padx=10, pady=5)

        # Sampling
        ttk.Label(frame, text="Sampling (ms):").grid(row=0, column=0, sticky="w")
        self.sampling_var = tk.StringVar(value=self.read_sysfs("sampling_ms"))
        ttk.Entry(frame, textvariable=self.sampling_var, width=10).grid(row=0, column=1)
        ttk.Button(frame, text="Apply", command=self.apply_sampling).grid(row=0, column=2, padx=5)

        # Threshold
        ttk.Label(frame, text="Threshold (m°C):").grid(row=1, column=0, sticky="w")
        self.threshold_var = tk.StringVar(value=self.threshold_mC)
        ttk.Entry(frame, textvariable=self.threshold_var, width=10).grid(row=1, column=1)
        ttk.Button(frame, text="Apply", command=self.apply_threshold).grid(row=1, column=2, padx=5)

        # Mode
        ttk.Label(frame, text="Mode:").grid(row=2, column=0, sticky="w")
        self.mode_var = tk.StringVar(value=self.mode)
        ttk.Combobox(
            frame, textvariable=self.mode_var, values=["normal", "noisy", "ramp"], width=10
        ).grid(row=2, column=1)
        ttk.Button(frame, text="Apply", command=self.apply_mode).grid(row=2, column=2, padx=5)

    def create_plot(self):
        frame = ttk.LabelFrame(self.root, text="Temperature Plot")
        frame.pack(fill="both", expand=True, padx=10, pady=5)

        self.figure = Figure(figsize=(6, 3), dpi=100)
        self.ax = self.figure.add_subplot(111)
        self.ax.set_title("Live Temperature")
        self.ax.set_xlabel("Samples")
        self.ax.set_ylabel("Temperature (°C)")
        self.line, = self.ax.plot([], [], "-o")

        self.canvas = FigureCanvasTkAgg(self.figure, master=frame)
        self.canvas.get_tk_widget().pack(fill="both", expand=True)

    def create_stats(self):
        frame = ttk.LabelFrame(self.root, text="Stats")
        frame.pack(fill="x", padx=10, pady=5)

        self.stats_text = tk.StringVar(value="Updates: 0 | Alerts: 0 | Last error: 0")
        ttk.Label(frame, textvariable=self.stats_text).pack(anchor="w", padx=5, pady=5)

    # ---------- CALLBACKS ----------
    def apply_sampling(self):
        self.write_sysfs("sampling_ms", self.sampling_var.get())

    def apply_threshold(self):
        val = self.threshold_var.get()
        if val.isdigit():
            self.write_sysfs("threshold_mC", val)
        else:
            messagebox.showwarning("Invalid", "Threshold must be an integer (m°C).")

    def apply_mode(self):
        self.write_sysfs("mode", self.mode_var.get())

    # ---------- POLL THREAD ----------
    def poll_device(self):
        fd = os.open(DEVICE, os.O_RDONLY | os.O_NONBLOCK)
        poller = select.poll()
        poller.register(fd, select.POLLIN | select.POLLPRI)

        while self.running:
            events = poller.poll(500)
            for _, flag in events:
                if flag & (select.POLLIN | select.POLLPRI):
                    data = os.read(fd, record_size)
                    if len(data) == record_size:
                        _, temp_mC, flags = struct.unpack(record_fmt, data)
                        self.temps.append(temp_mC / 1000.0)
                        self.alert_flags.append(bool(flags & 0x2))

                        if len(self.temps) > MAX_POINTS:
                            self.temps = self.temps[-MAX_POINTS:]
                            self.alert_flags = self.alert_flags[-MAX_POINTS:]

                        self.root.after(0, self.update_plot)

        os.close(fd)

    # ---------- UI UPDATES ----------
    def update_plot(self):
        self.line.set_data(range(len(self.temps)), self.temps)
        self.ax.set_xlim(0, MAX_POINTS)
        if self.temps:
            self.ax.set_ylim(min(self.temps) - 1, max(self.temps) + 1)
        self.canvas.draw()

    def update_stats(self):
        stats_val = self.read_sysfs("stats")
        self.stats_text.set(stats_val)
        if self.running:
            self.root.after(1000, self.update_stats)  # refrescar cada 1 s

    # ---------- CLEANUP ----------
    def on_close(self):
        self.running = False
        self.root.destroy()


if __name__ == "__main__":
    root = tk.Tk()
    app = SimTempGUI(root)
    root.protocol("WM_DELETE_WINDOW", app.on_close)
    root.mainloop()
