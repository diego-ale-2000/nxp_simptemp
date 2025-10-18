#!/usr/bin/env python3
import os
import select
import struct
import time

DEVICE = "/dev/simtemp"

record_fmt = "Qii"  # timestamp_ns, temp_mC, flags
record_size = struct.calcsize(record_fmt)

def main():
    fd = os.open(DEVICE, os.O_RDONLY | os.O_NONBLOCK)
    poller = select.poll()
    poller.register(fd, select.POLLIN | select.POLLPRI)

    print(f"Polling {DEVICE} for new temperature samples...")

    try:
        while True:
            events = poller.poll(1000)  # 1s timeout
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

if __name__ == "__main__":
    main()
