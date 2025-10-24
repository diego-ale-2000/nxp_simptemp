#!/usr/bin/env python3
import os
import sys
import select
import struct
import time
import argparse
from datetime import datetime
from zoneinfo import ZoneInfo

DEVICE = "/dev/simtemp"
SYSFS_BASE = "/sys/class/misc/simtemp"

record_fmt = "Qii"  # timestamp_ns, temp_mC, flags
record_size = struct.calcsize(record_fmt)

GDL_TZ = ZoneInfo("America/Mexico_City")  # Guadalajara timezone

def read_sysfs(path):
    try:
        with open(path, "r") as f:
            return f.read().strip()
    except Exception as e:
        print(f"Error reading {path}: {e}")
        return None


def write_sysfs(path, value):
    try:
        with open(path, "w") as f:
            f.write(str(value))
        return True
    except Exception as e:
        print(f"Error writing {path}: {e}")
        return False


def print_stats():
    stats_path = os.path.join(SYSFS_BASE, "stats")
    stats = read_sysfs(stats_path)
    if stats:
        print(f"\n--- Device Stats ---\n{stats}\n")
    else:
        print("Could not read stats.")


def set_mode(mode):
    mode_path = os.path.join(SYSFS_BASE, "mode")
    print(f"Setting mode to '{mode}'...")
    if write_sysfs(mode_path, mode):
        print(f"Mode set to: {read_sysfs(mode_path)}")
    else:
        print("Failed to set mode.")


def live_poll():
    fd = os.open(DEVICE, os.O_RDONLY | os.O_NONBLOCK)
    poller = select.poll()
    poller.register(fd, select.POLLIN | select.POLLPRI)

    print(f"Polling {DEVICE} for new temperature samples...\n")

    try:
        while True:
            events = poller.poll(1000)
            for fd_, flag in events:
                if flag & (select.POLLIN | select.POLLPRI):
                    data = os.read(fd, record_size)
                    if len(data) != record_size:
                        continue
                    ts_ns, temp, flags = struct.unpack(record_fmt, data)
                    alert = "YES" if flags & 0x2 else "NO"
                    
                    now = datetime.now(GDL_TZ)
                    print(f"{now.strftime('%Y-%m-%d %H:%M:%S')} | {temp/1000:.2f} °C | Threshold crossed? {alert}")
    except KeyboardInterrupt:
        print("\nExiting...")
    finally:
        os.close(fd)

def set_threshold(value):
    path = os.path.join(SYSFS_BASE, "threshold_mC")
    print(f"Setting threshold to {value} m°C...")
    if write_sysfs(path, value):
        print(f"Threshold set to: {read_sysfs(path)}")
    else:
        print("Failed to set threshold.")


def set_sampling(value):
    path = os.path.join(SYSFS_BASE, "sampling_ms")
    print(f"Setting sampling interval to {value} ms...")
    if write_sysfs(path, value):
        print(f"Sampling interval set to: {read_sysfs(path)} ms")
    else:
        print("Failed to set sampling interval.")

def run_test():
    print("🚀 Running device test mode...")
    
    test_mode = "noisy"
    test_threshold = 41000 
    test_sampling = 500   
    
    write_sysfs(os.path.join(SYSFS_BASE, "mode"), test_mode)
    write_sysfs(os.path.join(SYSFS_BASE, "threshold_mC"), test_threshold)
    write_sysfs(os.path.join(SYSFS_BASE, "sampling_ms"), test_sampling)
    
    print(f"Mode={test_mode}, Threshold={test_threshold}, Sampling={test_sampling} ms")
    print("Waiting for a sample to cross threshold...")
    
    fd = os.open(DEVICE, os.O_RDONLY | os.O_NONBLOCK)
    poller = select.poll()
    poller.register(fd, select.POLLIN | select.POLLPRI)

    timeout = 10 
    start = time.time()
    success = False
    
    while time.time() - start < timeout:
        events = poller.poll(1000)
        for fd_, flag in events:
            if flag & (select.POLLIN | select.POLLPRI):
                data = os.read(fd, record_size)
                if len(data) != record_size:
                    continue
                _, temp, flags = struct.unpack(record_fmt, data)
                if flags & 0x2:
                    print(f"PASS: Sample crossed threshold! Temp={temp/1000:.2f} °C")
                    success = True
                    break
        if success:
            break

    if not success:
        print("FAIL: No sample crossed threshold in time.")
    
    os.close(fd)



def main():
    parser = argparse.ArgumentParser(description="CLI for nxp_simtemp device")
    parser.add_argument("--mode", help="Set device mode (normal, noisy, ramp)")
    parser.add_argument("--stats", action="store_true", help="Show stats and exit")
    parser.add_argument("--threshold", type=int, help="Set threshold in m°C")
    parser.add_argument("--sampling", type=int, help="Set sampling interval in ms")
    parser.add_argument("--test", action="store_true", help="Run automated device test")

    args = parser.parse_args()

    # Apply settings
    if args.mode:
        set_mode(args.mode)
    if args.threshold is not None:
        set_threshold(args.threshold)
    if args.sampling is not None:
        set_sampling(args.sampling)

    if args.stats:
        print_stats()
        return
    
    if args.test:
        run_test()
        return
    
    # Default action: live poll
    live_poll()


if __name__ == "__main__":
    main()
