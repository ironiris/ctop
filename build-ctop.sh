#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ARCH=$(uname -m)
TRIPLE="$ARCH-linux"
CHECK_ONLY=0

usage() {
    cat <<'EOF'
Usage: ./build-ctop.sh [--check] [-h|--help]

Builds ctop with the web dashboard assets embedded.

  --check, --check-deps   verify build dependencies and exit
  -h, --help              show this help

Environment:
  CUDA_ROOT   CUDA installation containing nvml.h and the NVML stub library.
              When unset, common locations are searched automatically.
EOF
}

while [ $# -gt 0 ]; do
    case $1 in
        --check|--check-deps) CHECK_ONLY=1 ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'unknown option: %s\n\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

stop_without_compiler() {
    printf 'gcc was not found, so the build cannot continue.\n' >&2
    exit 1
}

# Resolve the directory holding nvml.h and the directory holding the NVML stub
# library. Both must exist: the driver provides the runtime library only.
try_nvml_base() {
    for include in "$1/targets/$TRIPLE/include" "$1/include"; do
        [ -f "$include/nvml.h" ] || continue
        for stubs in "$1/targets/$TRIPLE/lib/stubs" "$1/lib64/stubs" "$1/lib/stubs"; do
            if [ -f "$stubs/libnvidia-ml.so" ] || [ -f "$stubs/libnvidia-ml.a" ]; then
                NVML_INCLUDE_DIR=$include
                NVML_STUB_DIR=$stubs
                return 0
            fi
        done
    done
    return 1
}

find_nvml_development() {
    if [ -n "${CUDA_ROOT:-}" ] && try_nvml_base "$CUDA_ROOT"; then
        return 0
    fi

    for base in $(for path in /usr/local/cuda-* /usr/local/cuda /opt/cuda-* /opt/cuda; do
        [ -d "$path" ] && printf '%s\n' "$path"
    done | sort -rV); do
        try_nvml_base "$base" && return 0
    done

    for include in /usr/include /usr/local/include; do
        [ -f "$include/nvml.h" ] || continue
        for stubs in /usr/lib/$TRIPLE/stubs /usr/lib64/stubs /usr/local/lib/stubs; do
            if [ -f "$stubs/libnvidia-ml.so" ] || [ -f "$stubs/libnvidia-ml.a" ]; then
                NVML_INCLUDE_DIR=$include
                NVML_STUB_DIR=$stubs
                return 0
            fi
        done
    done
    return 1
}

stop_without_nvml() {
    printf 'nvml.h or the NVML stub library was not found under:\n'
    printf '  CUDA_ROOT (if set), /usr/local/cuda*, /opt/cuda*, /usr\n\n'
    printf 'ctop links against the NVML stub at build time; the NVIDIA driver only supplies\n'
    printf 'the runtime library. Stopping here. If the NVML development files live elsewhere:\n\n'
    printf '  CUDA_ROOT=/path/to/cuda ./build-ctop.sh\n'
}

if ! command -v gcc >/dev/null 2>&1; then
    stop_without_compiler
fi

if ! find_nvml_development; then
    stop_without_nvml >&2
    exit 1
fi

if [ "$CHECK_ONLY" = 1 ]; then
    printf 'compiler:   %s\n' "$(gcc -dumpversion)"
    printf 'arch:       %s\n' "$ARCH"
    printf 'nvml.h:     %s\n' "$NVML_INCLUDE_DIR/nvml.h"
    printf 'NVML stub:  %s\n' "$NVML_STUB_DIR"
    printf 'all build dependencies are present\n'
    exit 0
fi

for asset in index.html style.css app.js; do
    if [ ! -f "$ROOT/web/$asset" ]; then
        echo "web asset not found: $ROOT/web/$asset" >&2
        exit 1
    fi
done

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ctop-build.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

emit_asset() {
    symbol=$1
    file=$2
    printf 'static const unsigned char %s[] = {\n' "$symbol"
    od -An -v -tu1 "$file" | awk '{
        for (i = 1; i <= NF; i++) {
            printf "%s,", $i
            n++
            if (n % 16 == 0) printf "\n"
        }
    } END { printf "0\n};\n" }'
    printf '#define %s_SIZE (sizeof(%s) - 1)\n\n' "$symbol" "$symbol"
}

{
    printf '#ifndef CTOP_WEB_ASSETS_H\n#define CTOP_WEB_ASSETS_H\n\n'
    emit_asset CTOP_WEB_INDEX "$ROOT/web/index.html"
    emit_asset CTOP_WEB_CSS "$ROOT/web/style.css"
    emit_asset CTOP_WEB_JS "$ROOT/web/app.js"
    printf '#endif\n'
} > "$TMP/web_assets.h"

printf 'using nvml.h from %s\n' "$NVML_INCLUDE_DIR"
gcc -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter \
    -I"$NVML_INCLUDE_DIR" -I"$TMP" "$ROOT/ctop.c" \
    -L"$NVML_STUB_DIR" -Wl,-rpath,/lib/$TRIPLE \
    -lnvidia-ml -ldl -lm -o "$ROOT/ctop"
printf 'built %s\n' "$ROOT/ctop"
