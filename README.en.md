# ctop

A lightweight terminal dashboard for AMD/NVIDIA GPUs, system memory, processes, network, and disk I/O.

`ctop` is a single C11 program with no curses dependency. NVIDIA data comes from NVML; AMD devices are enumerated with Vulkan and monitored through Linux `amdgpu` interfaces.

![ctop terminal UI](docs/images/ctop.png)

![ctop web dashboard](docs/images/ctop_web.png)

## Features

- GPU temperature, power, memory, and utilization
- GPU process table with `GPU-MEM`, `GPU-LOAD`, `CPU-AVG`, and per-process host RAM percentage (`MEM`)
- RAM and SWAP usage with history graphs
- Live disk I/O graph with a Y-axis scaled to the maximum peak measured over the last 400 seconds
- GPU memory and utilization history graphs
- Network throughput in bit/s with per-interface link-speed utilization
- RX/TX indicators and per-interface network rows
- Docker-related interfaces (`docker*`, `br-*`, and `veth*`) excluded from host network totals
- Mouse-enabled section collapse/expand
- Optional alternate-screen mode

## Requirements

### Build time

- Linux and GCC with C11 support
- NVIDIA backend: `nvml.h` (the NVML stub library and full CUDA Toolkit are not required)
- Vulkan backend: Vulkan headers and the loader development library
- Standard glibc, `libdl`, and `libm` libraries

The default `auto` build selects NVIDIA, Vulkan, or both backends based on the installed development files. Select one explicitly with `--backend nvidia|vulkan|both`. Set `NVML_INCLUDE_DIR`/`CUDA_ROOT` or `VULKAN_INCLUDE_DIR` to specify header locations; `VULKAN_LIBRARY_DIR` can specify the Vulkan loader library directory.

Check dependencies without compiling:

```bash
./build-ctop.sh --check
./build-ctop.sh --check --backend both
```

### Runtime

- NVIDIA monitoring requires an NVIDIA driver that provides NVML (`nvidia-smi` should work).
- AMD monitoring requires the Vulkan loader, the `amdgpu` kernel driver, and accessible DRM/sysfs data.
- An interactive Linux terminal at least 76 columns by 13 rows for TUI mode
- No terminal is needed when running web-only with `--no-tui`

The program also reads `/proc/meminfo`, `/proc/net/dev`, `/proc/stat`, `/proc/uptime`, `/proc/diskstats`, and `/sys/class/net/<interface>/speed`.

## Installing prerequisites

Install the compiler and basic build tools:

```bash
sudo apt update
sudo apt install build-essential
```

Install the appropriate AMD or NVIDIA kernel driver. Verify an NVIDIA installation with:

```bash
nvidia-smi
```

The AMD backend needs Vulkan development files (Ubuntu/Debian: `libvulkan-dev`). The NVIDIA backend needs `nvml.h` from an NVML development package; the NVML stub library is not needed. Find the header with:

```bash
find /usr/local -name nvml.h 2>/dev/null
```

## Build

From the project directory:

```bash
./build-ctop.sh
```

Backends are selected automatically, or can be chosen explicitly:

```bash
./build-ctop.sh --backend auto
./build-ctop.sh --backend nvidia
./build-ctop.sh --backend vulkan
./build-ctop.sh --backend both
```

If the NVML header is outside standard paths, set `NVML_INCLUDE_DIR=/path/to/include` or `CUDA_ROOT=/path/to/cuda`.

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
--monitor ADDRESS         add a remote HTTP monitor shared by TUI and web (repeatable)
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
./ctop --monitor 192.168.0.177:8079
./ctop --no-tui --port 8079
```

## Web dashboard and API

Use `-p 8079` to run the web dashboard alongside the existing terminal UI. Open `http://<server-address>:8079/` in a browser. When running with the TUI, the detected web monitor address (for example, `http://192.168.0.177:8079`) appears in the TUI's upper-right corner. With `--localhost`, it displays `http://127.0.0.1:8079`. Add `--no-tui` to run the web dashboard without the terminal UI. Use the camera icon in the dashboard header to copy a screenshot to the clipboard. If clipboard copying is unavailable, the image is saved as a file.

Add another ctop address under `ADDITIONAL MONITORS` to register the same monitor in both the TUI and web dashboard. Monitors added from the TUI also appear in the web dashboard. A duplicate is identified only when host/address, port, and token all match; a different token is treated as a separate monitor. Prefixes (`A`, `B`, and so on) are assigned automatically. If a remote connection drops and is detected again, the web page reloads to update its connection state. TUI remote monitor metrics are included in the web API. Without `--token`, other network users can view them and change the monitor list, so set `--token` when exposing the server to a network. If enabled, sign in to the current dashboard first. The form's `TOKEN` authenticates the remote ctop and is separate from the dashboard token. In `GPU PROCESSES`, `GPU-MEM` is the process's GPU memory allocation, while `MEM` is that process's RSS as a percentage of the monitored host's total RAM. `SYSTEM` RAM usage is calculated for the whole host using `MemAvailable`, so it differs from `MEM` and will not equal the sum of per-process `MEM` values. Remote addresses must be reachable from the ctop server. TUI remote connections use HTTP only; do not send tokens over untrusted networks. Section order and collapse state are configured separately in the web browser and TUI.

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

## TUI remote monitors and controls

The remote monitor list is shared with the web dashboard; adding or removing a monitor in either UI updates the other. Enter the server address in the bottom `REMOTE` field and, if required, its token in the adjacent `TOKEN` field; then click `ADD` to connect. Entering an address alone does not connect or create a monitor section. Click either field or press `a` to focus the address. Tab/Enter moves focus through TOKEN and ADD; press Enter on the focused ADD button to connect. A URL such as `http://host:port` is also accepted; `/api/v1/metrics` is appended automatically. Remote GPUs are combined in GPU LIST, while GPU graphs, SYSTEM, and GPU PROCESSES appear in separate sections per monitor. You can also pass `--monitor ADDRESS` at startup (repeatable). TUI remote connections currently support HTTP only; tokens are sent unencrypted, so do not use them over untrusted networks.

```text
q / Esc                   quit
h                         show/hide help
a                         focus the bottom REMOTE address field
Tab / Enter               move focus through TOKEN and ADD
[ / ]                     select a section
u / d                     move the selected section up/down
x                         remove the selected remote monitor
c                         collapse/expand the selected section
g / m                     collapse/expand local GPU / SYSTEM sections
Space                     pause/resume sampling
+ / -                     change refresh interval
r                         clear graph history
j                         select the next GPU process
k / K                     confirm SIGTERM for a selected local process
```

Section order lasts for the current TUI session. When the terminal supports SGR mouse input, click `▲`/`▼` before a title to move that section up or down, and the right triangle to collapse or expand it.

## Disk display

The disk information in the SYSTEM section uses this format:

```text
DISK(XX%) XXX.XGB/YYY.YGB R:<read rate> W:<write rate>
```

`XXX.XGB` is the current used capacity and `YYY.YGB` is the total capacity. `R:` and `W:` display read and write throughput, respectively (for example, `W:271K`). The I/O graph shows combined read and write throughput for block devices, excluding partitions and `loop*`, `ram*`, and `zram*` devices. Its Y-axis is scaled to the maximum peak measured during the most recent 400 seconds rather than a 0–100% range. Capacity is normally measured for the root filesystem (`/`). If `/` is a virtual filesystem such as `composefs` or `overlay`, ctop selects the largest non-virtual filesystem visible in the current mount namespace (usually `/var` on Bazzite). Set `CTOP_DISK_PATH=/path` to select a filesystem explicitly. If no non-virtual filesystem is visible, capacity is reported as unavailable.

## Notes

Network rates are calculated from interface byte counters and converted to bits per second. Compact interface rows abbreviate total throughput as `Tot:`; arrows indicate receive and transmit rates. The displayed utilization uses each interface's link speed from `/sys/class/net/<interface>/speed`; interfaces without a readable speed show `N/A` utilization. Traffic from Docker containers leaving the host is still visible through the host's physical interface; the dashboard does not provide per-container accounting.
