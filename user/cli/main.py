import os
import sys
import select
import struct
import time
import argparse

DEVICE = "/dev/simtemp"
SYSFS_BASE = "/sys/class/misc/simtemp"

record_fmt = "Qii"  # timestamp_ns, temp_mC, flags
record_size = struct.calcsize(record_fmt)


def read_sysfs(path):
    """Reads a sysfs attribute as string."""
    try:
        with open(path, "r") as f:
            return f.read().strip()
    except Exception as e:
        print(f"Error reading {path}: {e}")
        return None


def write_sysfs(path, value):
    """Writes a string to a sysfs attribute."""
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
                    ts, temp, flags = struct.unpack(record_fmt, data)
                    alert = "YES" if flags & 0x2 else "NO"
                    print(f"{time.strftime('%H:%M:%S')} | {temp/1000:.2f} °C | Threshold crossed? {alert}")
    except KeyboardInterrupt:
        print("\nExiting...")
    finally:
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description="CLI for nxp_simtemp device")
    parser.add_argument("--mode", help="Set device mode (normal, noisy, ramp)")
    parser.add_argument("--stats", action="store_true", help="Show stats and exit")
    args = parser.parse_args()

    if args.mode:
        set_mode(args.mode)
        return

    if args.stats:
        print_stats()
        return

    # Default action: live poll
    live_poll()


if __name__ == "__main__":
    main()
