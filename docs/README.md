# NXP Simulated Temperature Sensor

Simulated temperature sensor implemented as a Linux kernel platform driver with sysfs control and a user-space CLI + GUI.

---

## Author

Diego Alejandro Delgado González

## Features

- Simulates temperature in three modes: `normal`, `noisy`, `ramp`.
- Configurable sampling period (`sampling_ms`) and threshold (`threshold_mC`) via sysfs.
- Provides temperature samples through `/dev/simtemp`.
- Alerts when temperature crosses threshold.
- CLI tool for live monitoring, configuration and test mode.
- GUI app for live monitoring and configuration.

---

## Requirements

- Linux kernel >= 6.x
- Python 3
- `make`, `gcc`, `kernel-headers` installed

---

## Build & Install

```bash
# Build kernel module
cd kernel
make

# Insert module
sudo insmod nxp_simtemp.ko

# Adjust device permissions if needed
sudo chmod 666 /dev/simtemp
```
---

## Remove Module
```bash
sudo rmmod nxp_simtemp
```
---

## Sysfs Attributes

### Attribute	    Description	                            Read/Write
    sampling_ms    Sampling period in milliseconds	        RW
    threshold_mC	Threshold in milli-degrees Celsius	    RW
    mode	        Sensor mode (normal, noisy, ramp)	    RW
    stats	        Updates, alerts, last_error	            R
 
# Examples
```bash
cat /sys/class/misc/simtemp/stats
echo 500 | sudo tee /sys/class/misc/simtemp/sampling_ms
echo 40000 | sudo tee /sys/class/misc/simtemp/threshold_mC
echo -n normal | sudo tee /sys/class/misc/simtemp/mode
```
---

## User-space CLI

```bash
# Run CLI in live monitoring mode
sudo python3 user/cli/main.py

# Set sampling period in ms
sudo python3 user/cli/main.py --sampling 500

# Set mode from CLI
sudo python3 user/cli/main.py --mode normal

# Set threshold in mC
sudo python3 user/cli/main.py --threshold 40000

# Show stats and exit
sudo python3 user/cli/main.py --stats

# Run test mode
sudo python3 user/cli/main.py --test
```
---


## User-space GUI
```bash
# Run CLI in live monitoring mode
sudo python3 user/cli/app.py
```


## Scripts

Provided two Bash scripts to simplify building and demoing the `nxp_simtemp` kernel module.

### `build.sh`

Builds the kernel module and installs Python dependencies.

**Description:**

- Checks for kernel headers for the running kernel.
- Builds the `nxp_simtemp.ko` module.
- Installs required Python packages (`matplotlib` and `tk`).

### Usage

From the project root (`~/simtemp`):

```bash
./scripts/build.sh
```

### `run_demo.sh`

This script runs a full demo of the `nxp_simtemp` device:

1. Loads the kernel module.
2. Configures sampling, threshold, and mode.
3. Runs the CLI test for 10 seconds.
4. Prints current device stats.
5. Unloads the kernel module.

### Usage

From the project root (`~/simtemp`):

```bash
./scripts/demo.sh
```
---

