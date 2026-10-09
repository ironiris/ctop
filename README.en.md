# ctop

A lightweight terminal dashboard for NVIDIA GPU, system memory, process, and network monitoring.

`ctop` is a single C11 program with no curses dependency. It reads GPU data through NVML, system data from Linux `/proc` interfaces, and link speeds from `/sys`.

## Features

- GPU temperature, power, VRAM, utilization, and GPU process information
- RAM and SWAP usage with history graphs
- VRAM and GPU utilization history graphs
- Network throughput in bit/s with per-interface link-speed utilization
- RX/TX indicators and per-interface network rows
- Docker-related interfaces (`docker*`, `br-*`, and `veth*`) excluded from host network totals
- Mouse-enabled section collapse/expand
- Optional alternate-screen mode

## Requirements

### Build time

- Linux
- GCC with C11 support
- CUDA/NVML development files:
  - `nvml.h`
  - the NVML stub library, normally `libnvidia-ml.so` under CUDA's `targets/x86_64-linux/lib/stubs`
- Standard glibc, `libdl`, and `libm` libraries

The build script defaults to CUDA 12.8 at `/usr/local/cuda-12.8`. A different CUDA installation can be selected with `CUDA_ROOT`.

### Runtime

- A supported NVIDIA GPU
- NVIDIA driver with NVML available (`nvidia-smi` should work)
- An interactive Linux terminal
- A terminal at least 76 columns by 13 rows

The program also reads `/proc/meminfo`, `/proc/net/dev`, `/proc/stat`, `/proc/uptime`, and `/sys/class/net/<interface>/speed`.

## Installing prerequisites

Install the compiler and basic build tools:

```bash
sudo apt update
sudo apt install build-essential
```

Install the NVIDIA driver appropriate for your GPU and verify it:

```bash
nvidia-smi
```

Install the CUDA Toolkit from the official NVIDIA CUDA installation guide. The toolkit must provide `nvml.h` and the NVML stub library. If CUDA is installed somewhere other than `/usr/local/cuda-12.8`, pass its path through `CUDA_ROOT`.

## Build

From the project directory:

```bash
./build-ctop.sh
```

Or with a custom CUDA installation:

```bash
CUDA_ROOT=/usr/local/cuda ./build-ctop.sh
```

This creates the `ctop` executable in the project directory.

## Run

```bash
./ctop
```

Useful options:

```text
-i, --interval N          refresh interval in seconds
--no-color                disable ANSI colors
--debug-log FILE          append diagnostics to a file
-a, --alternate-screen    restore the previous screen on exit
-r, --restore-screen      alias for --alternate-screen
-v, --version             show version
```

## Controls

```text
q                         quit
h                         show/hide help
g                         collapse/expand GPU history
m                         collapse/expand the system section
Space                     pause/resume sampling
+ / -                     change refresh interval
r                         clear graph history
j                         select the next GPU process
k / K                     send SIGTERM confirmation to the selected process
```

The top-bar actions can also be clicked when the terminal supports SGR mouse input.

## Notes

Network rates are calculated from interface byte counters and converted to bits per second. The displayed utilization uses each interface's link speed from `/sys/class/net/<interface>/speed`; interfaces without a readable speed show `N/A` utilization. Traffic from Docker containers leaving the host is still visible through the host's physical interface; the dashboard does not provide per-container accounting.
