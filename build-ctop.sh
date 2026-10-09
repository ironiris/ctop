#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CUDA_ROOT=${CUDA_ROOT:-/usr/local/cuda-12.8}
INCLUDE="$CUDA_ROOT/targets/x86_64-linux/include"
STUBS="$CUDA_ROOT/targets/x86_64-linux/lib/stubs"

if [ ! -f "$INCLUDE/nvml.h" ]; then
    echo "nvml.h not found under $INCLUDE" >&2
    echo "Set CUDA_ROOT to a CUDA installation containing the NVML development headers." >&2
    exit 1
fi

exec gcc -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter \
    -I"$INCLUDE" "$ROOT/ctop.c" \
    -L"$STUBS" -Wl,-rpath,/lib/x86_64-linux-gnu \
    -lnvidia-ml -ldl -lm -o "$ROOT/ctop"
