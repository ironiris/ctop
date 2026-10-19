# Building ctop with both NVIDIA and AMD/Vulkan support

Tested against `ironiris/ctop` main at base commit `393d3ff` (dual-backend changes in this working tree are uncommitted).

This guide records the dual-backend implementation and the exact build setup used on an AMD BC-250. The goal is **one ctop executable** that preserves the existing NVIDIA/NVML telemetry and can also identify and monitor AMD GPUs on Vulkan/RADV systems.

## Backend design

- NVIDIA telemetry continues to use the existing NVML calls and data model. NVML is loaded at runtime with `dlopen()` instead of being linked against the development stub. Thus an AMD-only host can run the same dual-backend executable without `libnvidia-ml.so.1`; on a host with the NVIDIA driver, NVML is activated automatically.
- Vulkan is used to enumerate the AMD physical device and read its name/device ID. Vulkan does not expose general-purpose temperature, power, or utilization telemetry. Those live measurements therefore come from the Linux `amdgpu` DRM/sysfs interface (`hwmon`, `mem_info_*`, `gpu_busy_percent`) and per-process DRM `fdinfo`.
- If the kernel does not implement `gpu_busy_percent`, aggregate and per-process GPU busy percentages are calculated from DRM `drm-engine-gfx` time deltas. This requires readable `/proc/<pid>/fdinfo` entries.
- On BC-250 unified memory, ctop prefers the GTT memory counters (`mem_info_gtt_used/total`); other supported AMD devices fall back to VRAM counters if GTT counters are unavailable.
- The NVIDIA and Vulkan backends can coexist in the same process. `--backend both` builds that executable; `auto` selects both when both development headers are installed, otherwise it selects the available backend.

## Dependencies

On Fedora/Bazzite, the Vulkan build tools used here were:

```bash
sudo dnf install gcc vulkan-headers vulkan-loader-devel vulkan-loader
```

The NVIDIA driver is a runtime requirement for monitoring NVIDIA hardware; it is **not** required to compile or run the AMD path. For a dual-backend build, the compiler also needs NVIDIA's `nvml.h`, supplied by NVIDIA's `cuda-nvml-devel` package. The full CUDA Toolkit and NVML link stub are not required.

If the CUDA repository is enabled:

```bash
sudo dnf install cuda-nvml-devel
```

To extract only the header without installing the package (example for Fedora 43; use the current RPM listed in NVIDIA's repository for other releases):

```bash
mkdir -p /tmp/nvml-header .deps/nvml
cd /tmp/nvml-header
curl -fLO https://developer.download.nvidia.com/compute/cuda/repos/fedora43/x86_64/cuda-nvml-devel-13-2-13.2.86-1.x86_64.rpm
rpm2cpio cuda-nvml-devel-13-2-13.2.86-1.x86_64.rpm | cpio -idmv --no-absolute-filenames
cp usr/local/cuda-13.2/targets/x86_64-linux/include/nvml.h /path/to/ctop/.deps/nvml/
cd /path/to/ctop
```

`.deps/` is ignored by Git. Alternatively set `NVML_INCLUDE_DIR` to a directory that contains `nvml.h`, or install the NVML development package normally.

On Debian/Ubuntu, install GCC and Vulkan development files (`build-essential libvulkan-dev`); install NVIDIA's NVML development package/header separately if a dual-backend executable is required.

## Build

Check available backend development dependencies and build both backends:

```bash
./build-ctop.sh --check --backend both
./build-ctop.sh --backend both
```

The default build is automatic:

```bash
./build-ctop.sh --check
./build-ctop.sh
```

To install into the current user's PATH without root access:

```bash
PREFIX="$HOME/.local" ./install-ctop.sh
```

Backend-specific builds are also available:

```bash
./build-ctop.sh --backend nvidia
./build-ctop.sh --backend vulkan
```

The resulting executable is `./ctop`. The NVIDIA-only build does not require Vulkan; the Vulkan-only build does not require NVML headers. The dual build links to the Vulkan loader but loads NVML dynamically, so the dual binary has no hard `libnvidia-ml.so.1` dependency.

## Verification

Check linked runtime libraries:

```bash
ldd ./ctop | grep -E 'vulkan|nvidia|not found'
```

A dual build should link to `libvulkan.so.1` and should not list `libnvidia-ml.so.1` as a mandatory dependency. On a machine with Vulkan installed, start the web monitor without requiring a terminal:

```bash
./ctop --no-tui --port 8079 --localhost
curl http://127.0.0.1:8079/api/v1/metrics
```

## BC-250 build and runtime check

Test machine and toolchain:

- AMD BC-250, PCI ID `1002:13fe`, AMDGPU kernel driver
- Vulkan 1.4 loader/headers 1.4.341
- Mesa RADV 26.0.4; device reported as `AMD BC-250 (RADV GFX1013)` by `vulkaninfo`
- GCC 15.2.1, x86_64
- NVIDIA NVML header extracted from NVIDIA CUDA NVML development RPM 13.2.86; no NVIDIA driver/runtime installed

Commands used:

```bash
./build-ctop.sh --check --backend both
./build-ctop.sh --backend both
ldd ./ctop
./ctop --no-tui --port 18081 --localhost
curl http://127.0.0.1:18081/api/v1/metrics
```

The dual-backend build succeeded and ran without an NVIDIA runtime. The metrics API identified the Vulkan device as `AMD BC-250 (RADV GFX1013)` and returned live temperature, power, and GTT memory values. The host's `gpu_busy_percent` sysfs node returned `EOPNOTSUPP`, so DRM `fdinfo` engine-time deltas were used; under a Kanana inference request the reported GPU utilization reached approximately 96%.

## ROCm (AMD SMI) optional runtime backend

AMD telemetry prefers the ROCm system-management API when the ROCm runtime is
installed. This is a runtime-only backend: `ctop` dlopens `libamd_smi.so`
(ROCm 6.4+, preferred) then `librocm_smi64.so.1` from `/opt/rocm/lib`, uses the
`rsmi_*` symbol list, and needs no headers, link libraries, or build flags.
The build works unchanged on hosts without ROCm; verified on Ubuntu 24.04 with
`rocm-smi-lib` and `amd-smi-lib` from the ROCm 7.2 apt repository: both
libraries dlopen cleanly, `libamd_smi.so.26` exports the full legacy `rsmi_*`
shim (all 15 candidate symbols resolve in both), and on an AMD-GPU-less host
`rsmi_init()` fails gracefully (`RSMI_STATUS_INIT_ERROR`), which keeps the
existing Vulkan/sysfs path in charge.

- Scope: per-AMD-GPU temperature (edge, then junction), power draw/cap
  (microwatts), VRAM/GTT used+total (bytes, honoring `prefer_gtt`), and busy
  percent. Device matching uses the ROCm BDFID encoding versus the
  `domain:bus:device.function` slot from the Vulkan/sysfs enumeration.
- Per-process data stays on DRM `fdinfo`; ROCm's process API is compute-only
  and less stable across versions.
- Every ROCm call is optional and per-field; any failure falls back to the
  sysfs value. The data source is labeled per GPU (`nvml` / `rocm` / `sysfs`)
  in the TUI GPU list, the web GPU table, and the `/api/v1/metrics` JSON
  (`"source"` field, also parsed for remote monitors).

Not yet verified end-to-end on a host with a live AMD GPU through ROCm
(temperature/power/memory values, BDFID matching, multi-partition MI-series
caveats); the BC-250 path remains the sysfs fallback until then.

## Notes for contributors

- Keep the NVML collection path intact when making telemetry/UI changes; it remains the NVIDIA implementation.
- Vulkan provides device enumeration, not the AMD thermal/power telemetry. Do not describe the sysfs sampling code as Vulkan telemetry.
- GPU memory has historically been called `VRAM` in some UI/API fields. Display labels are generalized to “GPU MEM”; the `/api/v1/metrics` JSON field names remain unchanged for compatibility.
- The local extracted header and built executable are not source changes. Commit the source, build script, README updates, and this document; do not commit `.deps/`.
