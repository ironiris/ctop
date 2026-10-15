# ctop

A lightweight terminal dashboard for NVIDIA GPU, system memory, process, network, and disk I/O monitoring.

`ctop` is a single C11 program with no curses dependency. It reads GPU data through NVML, system data from Linux `/proc` interfaces, and link speeds from `/sys`.

![ctop terminal UI](docs/images/ctop.png)

![ctop web dashboard](docs/images/ctop_web.png)

## Features

- GPU temperature, power, VRAM, and utilization
- GPU process table with `GPU-LOAD`, `CPU-AVG`, and `MEM` percentage columns
- RAM and SWAP usage with history graphs
- Live disk I/O graph with a Y-axis scaled to the maximum peak measured over the last 400 seconds
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

The build script looks for the NVML development files under `CUDA_ROOT` (when set), then `/usr/local/cuda*`, `/opt/cuda*`, and `/usr`. If none are found it stops and tells you to set `CUDA_ROOT`. To check dependencies without compiling, run `./build-ctop.sh --check`.

### Runtime

- A supported NVIDIA GPU
- NVIDIA driver with NVML available (`nvidia-smi` should work)
- An interactive Linux terminal at least 76 columns by 13 rows for TUI mode
- No terminal is needed when running web-only with `--no-tui`

The program also reads `/proc/meminfo`, `/proc/net/dev`, `/proc/stat`, `/proc/uptime`, `/proc/diskstats`, and `/sys/class/net/<interface>/speed`.

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

Install the CUDA Toolkit from the official NVIDIA CUDA installation guide. The toolkit must provide `nvml.h` and the NVML stub library. The build script detects installations under `/usr/local/cuda*` and `/opt/cuda*`; for any other location, pass its path through `CUDA_ROOT`.

## Build

From the project directory:

```bash
./build-ctop.sh
```

Or with a custom CUDA installation:

```bash
CUDA_ROOT=/usr/local/cuda ./build-ctop.sh
```

This creates the `ctop` executable in the project directory. The web dashboard's HTML, CSS, and JavaScript resources are embedded into the executable during this build.

## Install

Build and install the executable to `/usr/local/bin/ctop`:

```bash
sudo ./install-ctop.sh
```

## Run

```bash
./ctop
```

Useful options:

```text
-i, --interval N          refresh interval in seconds
-p, --port PORT           serve the web dashboard alongside the TUI
--no-tui                  run only the web dashboard (requires -p)
--localhost               restrict web access to 127.0.0.1 (requires -p)
-t, --token TOKEN         require an access token for the dashboard and API
                          (requires -p; CTOP_TOKEN is also accepted)
--no-color                disable ANSI colors
--debug-log FILE          append diagnostics to a file
-a, --alternate-screen    restore the previous screen on exit
-r, --restore-screen      alias for --alternate-screen
-v, --version             show version
```

## Examples

```bash
./ctop -i 1
./ctop --no-color
./ctop --alternate-screen
./ctop -p 8079
./ctop -p 8079 --localhost
./ctop -p 8079 --token "$(openssl rand -hex 16)" --localhost
./ctop --no-tui --port 8079
```

## Web dashboard and API

Use `-p 8079` to run the web dashboard alongside the existing terminal UI. Open `http://<server-address>:8079/` in a browser. When running with the TUI, the detected web monitor address (for example, `http://192.168.0.177:8079`) appears in the TUI's upper-right corner. With `--localhost`, it displays `http://127.0.0.1:8079`. Add `--no-tui` to run the web dashboard without the terminal UI. The camera icon at the top right copies the dashboard image with 10px of horizontal padding, excluding the version footer. If the browser blocks clipboard access, the image is downloaded instead. Clipboard copying requires HTTPS or localhost.

Add another ctop server address under `ADDITIONAL MONITORS` at the bottom of the dashboard to combine GPUs in GPU LIST and show separate GPU, SYSTEM, and GPU PROCESSES sections per monitor. Enter only a host and port (for example, `192.168.0.177:8079`) or a protocol-relative address (`//192.168.0.177:8079`). The dashboard's current protocol (`http` or `https`) is used, and `/api/v1/metrics` is appended automatically. With one monitor, prefixes and the prefix field are hidden. With multiple monitors, the current server defaults to prefix `A` (editable next to GPU LIST); new monitors get the next unused prefix (`B`, `C`, and so on), producing IDs such as `A0` and `B0`. Each monitor's `GPU(A)`, `SYSTEM(A)`, and `GPU PROCESSES(A)` sections can be moved independently with the triangle buttons to the left of their titles. GPU, SYSTEM, and GPU PROCESSES sections each have a collapse button on the right. GPU LIST can be moved the same way; the order is saved in the browser. Addresses, prefixes, and tokens entered in the adjacent `Token` field are saved in browser localStorage. Remote addresses must be reachable from the browser, and remote servers must allow cross-origin browser requests. Use the `Token` field for token-protected remote APIs; the `?token=...` URL form is also supported.

Current metrics and graph history are also available as JSON:

```bash
curl http://localhost:8079/api/v1/metrics
```

With `--token`, every path except `/login` and the icon files (`/favicon.svg`, `/favicon.ico`) is protected. A browser gets a token prompt on first visit and then stays signed in through a seven-day cookie. Once that cookie expires or is cleared, re-enter the token in the prompt inside the dashboard. Only tokens entered there are kept in localStorage, so from then on a lost cookie is redeemed automatically; a token typed into the sign-in page itself is not stored. From scripts use either form:

```bash
curl -H "Authorization: Bearer TOKEN" http://localhost:8079/api/v1/metrics
curl "http://localhost:8079/api/v1/metrics?token=TOKEN"
```

The response is JSON (`application/json`) and includes current GPU, process, RAM/SWAP, disk, and network metrics along with graph history. `/api/metrics` returns the same payload. Each history sample carries its age in seconds (`age_seconds`) and its measured value (`value`); NET history values are throughput in bit/s, and `sample_interval` reports the sampling interval in seconds. When more than 500 samples fall inside the 400-second window they are bucketed, but each bucket keeps its minimum and maximum value. GPU averages are computed over the devices that returned valid readings. By default, the server listens on all network interfaces (`0.0.0.0`). Add `--localhost` to restrict it to `127.0.0.1`. Without `--token`, the dashboard and API are open without authentication. The metrics API allows cross-origin browser requests for multi-monitoring, so use `--token` on instances exposed to a network.

## Controls

```text
q / Esc                   quit
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

## Disk display

The disk information in the SYSTEM section uses this format:

```text
DISK(XX%) XXX.XGB/YYY.YGB R:<read rate> W:<write rate>
```

`XXX.XGB` is the current used capacity and `YYY.YGB` is the total capacity. `R:` and `W:` display read and write throughput, respectively (for example, `W:271K`). The I/O graph shows combined read and write throughput for block devices, excluding partitions and `loop*`, `ram*`, and `zram*` devices. Its Y-axis is scaled to the maximum peak measured during the most recent 400 seconds rather than a 0–100% range. Capacity is measured for the root filesystem (`/`).

## Notes

Network rates are calculated from interface byte counters and converted to bits per second. Compact interface rows abbreviate total throughput as `Tot:`; arrows indicate receive and transmit rates. The displayed utilization uses each interface's link speed from `/sys/class/net/<interface>/speed`; interfaces without a readable speed show `N/A` utilization. Traffic from Docker containers leaving the host is still visible through the host's physical interface; the dashboard does not provide per-container accounting.
