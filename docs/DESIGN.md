1. Block Diagram

+-----------------+       +-------------------+       +----------------+
| Kernel Module   |       | Misc Device       |       | User-space CLI |
| nxp_simtemp.ko  | <---> | /dev/simtemp      | <---> | main.py / GUI  |
| Platform Driver |       | sysfs attributes  |       | polling / set  |
+-----------------+       +-------------------+       +----------------+
        |                         ^
        |                         |
        v                         |
   High-resolution timer          |
        |                         |
        v                         |
    Workqueue (sample generation) |
        |                         |
        +-------------------------+

- Kernel generates temperature samples via high-resolution timer → workqueue.
- Samples stored in ring buffer protected by spinlock.
- User-space reads via non-blocking read on /dev/simtemp.
- Mode, threshold, sampling can be controlled via sysfs attributes.


2. Module Interaction

    1. Initialization
        - platform_driver_register() + platform_device_register_simple().
        - probe() allocates ring buffer, initializes spinlock, waitqueue, misc device, and HRT timer.
        - Sysfs attributes (sampling_ms, threshold_mC, mode, stats) are created.

    2. Sample Generation
        - HRT timer triggers periodically (sampling_ms).
        - Timer callback schedules workqueue.
        - Workqueue generates a sample depending on mode (normal/noisy/ramp).
        - Flags set if threshold crossed; spinlock ensures atomic buffer update.
        - Waitqueue wakes up any blocking readers.

    3. User-Space Interaction
        - Reading samples: os.read() or select.poll() waits for available data in /dev/simtemp.
        - Polling: POLLIN indicates new sample; POLLPRI if threshold crossed.
        - Configuration: Writing to sysfs attributes updates sampling period, threshold, or mode.
        - Stats: Read-only sysfs file shows cumulative updates, alerts, and last error.


3. Locking Choices

    1. Spinlock (gdev->lock):
        - Used in simtemp_work_func() and simtemp_read() to protect ring buffer and stats.
        - Chosen because critical sections are short (<1 μs); suitable for workqueue context and timer callback.

    2. No mutexes used

4. API Trade-Offs

    1. Sysfs:
        Used for configuration (mode, threshold, sampling) and statistics.
        Pros: easy to read/write, human-readable, allows simple shell scripts.
        Cons: cannot perform complex operations or bulk data transfer.

    2. Character Device (/dev/simtemp):
        Used for streaming samples.
        Non-blocking read + poll allows efficient event-driven design.

    3. Ioctl not used:
        Sysfs + char device combination sufficient.
        Ioctl unnecessary for simple configuration; avoids extra complexity and kernel ABI dependencies.


5. Device Tree Mapping

    - Platform device created statically with platform_device_register_simple("nxp_simtemp").
    - compatible string not required in this design; if DT is present, probe() matches using of_match_table.


6. Scaling Considerations

    - Target: 10 kHz sampling
    - Potential limitations:
        - Ring buffer spinlock contention (workqueue + readers)
        - Workqueue scheduling latency
        - HRT timer resolution

    - Mitigation strategies:
        - Increase ring buffer size
        - Batch multiple samples per workqueue execution
        - Use lock-free queue or per-CPU buffers to reduce contention
        - Consider kernel FIFO (kfifo) instead of manual array
