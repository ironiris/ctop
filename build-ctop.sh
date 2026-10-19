#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ARCH=$(uname -m)
TRIPLE="$ARCH-linux"
CHECK_ONLY=0
BACKEND=auto
NVML_INCLUDE_DIR=${NVML_INCLUDE_DIR:-}
VULKAN_INCLUDE_DIR=${VULKAN_INCLUDE_DIR:-}
VULKAN_LIBRARY_DIR=${VULKAN_LIBRARY_DIR:-}

usage() {
    cat <<'EOF'
Usage: ./build-ctop.sh [--backend auto|nvidia|vulkan|both] [--check] [-h|--help]

Builds ctop with the web dashboard assets embedded.

  --backend NAME   choose GPU backend; auto selects all available backends
  --check          verify compiler and selected backend dependencies, then exit
  -h, --help       show this help

Environment:
  CUDA_ROOT          CUDA installation to search for nvml.h
  NVML_INCLUDE_DIR   directory containing nvml.h
  VULKAN_INCLUDE_DIR  directory containing vulkan/vulkan.h
  VULKAN_LIBRARY_DIR directory containing libvulkan.so (optional)
EOF
}

while [ $# -gt 0 ]; do
    case $1 in
        --check|--check-deps) CHECK_ONLY=1 ;;
        --backend)
            [ $# -ge 2 ] || { echo "--backend requires a value" >&2; usage >&2; exit 2; }
            BACKEND=$2
            shift
            ;;
        --backend=*) BACKEND=${1#*=} ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'unknown option: %s\n\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

case $BACKEND in
    auto|nvidia|vulkan|both) ;;
    *) printf 'unknown backend: %s\n\n' "$BACKEND" >&2; usage >&2; exit 2 ;;
esac

if ! command -v gcc >/dev/null 2>&1; then
    echo "gcc was not found, so the build cannot continue." >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ctop-build.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

try_nvml_include() {
    [ -f "$1/nvml.h" ] || return 1
    NVML_INCLUDE_DIR=$1
    return 0
}

try_nvml_base() {
    [ -f "$1/nvml.h" ] && { NVML_INCLUDE_DIR=$1; return 0; }
    for include in "$1/targets/$TRIPLE/include" "$1/include"; do
        try_nvml_include "$include" && return 0
    done
    return 1
}

find_nvml_header() {
    if [ -n "$NVML_INCLUDE_DIR" ] && try_nvml_include "$NVML_INCLUDE_DIR"; then
        return 0
    fi
    if [ -n "${CUDA_ROOT:-}" ] && try_nvml_base "$CUDA_ROOT"; then
        return 0
    fi
    for base in /usr/local/cuda-* /usr/local/cuda /opt/cuda-* /opt/cuda; do
        [ -d "$base" ] || continue
        try_nvml_base "$base" && return 0
    done
    for include in /usr/include /usr/local/include; do
        try_nvml_include "$include" && return 0
    done
    return 1
}

try_vulkan_include() {
    [ -f "$1/vulkan/vulkan.h" ] || return 1
    VULKAN_INCLUDE_DIR=$1
    return 0
}

find_vulkan_header() {
    if [ -n "$VULKAN_INCLUDE_DIR" ] && try_vulkan_include "$VULKAN_INCLUDE_DIR"; then
        return 0
    fi
    for include in /usr/include /usr/local/include /opt/vulkan/include; do
        try_vulkan_include "$include" && return 0
    done
    return 1
}

if find_nvml_header; then NVML_FOUND=1; else NVML_FOUND=0; fi
if find_vulkan_header; then
    if [ -n "$VULKAN_LIBRARY_DIR" ]; then
        if printf 'int main(void) { return 0; }\n' | \
            gcc -x c - -o "$TMP/vulkan-link-check" -L"$VULKAN_LIBRARY_DIR" -lvulkan >/dev/null 2>&1; then
            VULKAN_FOUND=1
        else
            VULKAN_FOUND=0
        fi
    elif printf 'int main(void) { return 0; }\n' | \
        gcc -x c - -o "$TMP/vulkan-link-check" -lvulkan >/dev/null 2>&1; then
        VULKAN_FOUND=1
    else
        VULKAN_FOUND=0
    fi
else
    VULKAN_FOUND=0
fi

case $BACKEND in
    auto)
        if [ "$NVML_FOUND" -eq 1 ] && [ "$VULKAN_FOUND" -eq 1 ]; then
            SELECTED_BACKEND=both
        elif [ "$NVML_FOUND" -eq 1 ]; then
            SELECTED_BACKEND=nvidia
        elif [ "$VULKAN_FOUND" -eq 1 ]; then
            SELECTED_BACKEND=vulkan
        else
            echo "No GPU backend development files found (need nvml.h or Vulkan headers and loader)." >&2
            exit 1
        fi
        ;;
    nvidia)
        if [ "$NVML_FOUND" -ne 1 ]; then
            echo "nvml.h was not found. Set NVML_INCLUDE_DIR or CUDA_ROOT." >&2
            exit 1
        fi
        SELECTED_BACKEND=nvidia
        ;;
    vulkan)
        if [ "$VULKAN_FOUND" -ne 1 ]; then
            echo "Vulkan headers or the Vulkan loader development library were not found." >&2
            echo "Install Vulkan development files or set VULKAN_INCLUDE_DIR." >&2
            exit 1
        fi
        SELECTED_BACKEND=vulkan
        ;;
    both)
        if [ "$NVML_FOUND" -ne 1 ] || [ "$VULKAN_FOUND" -ne 1 ]; then
            echo "The both backend requires nvml.h, Vulkan headers, and the Vulkan loader development library." >&2
            echo "Set NVML_INCLUDE_DIR/CUDA_ROOT, VULKAN_INCLUDE_DIR, and VULKAN_LIBRARY_DIR as needed." >&2
            exit 1
        fi
        SELECTED_BACKEND=both
        ;;
esac

if [ "$CHECK_ONLY" = 1 ]; then
    printf 'compiler:       %s\n' "$(gcc -dumpversion)"
    printf 'arch:           %s\n' "$ARCH"
    printf 'GPU backend:    %s\n' "$SELECTED_BACKEND"
    case $SELECTED_BACKEND in
        nvidia|both) printf 'NVML header:    %s/nvml.h\n' "$NVML_INCLUDE_DIR" ;;
    esac
    case $SELECTED_BACKEND in
        vulkan|both)
            printf 'Vulkan header:  %s/vulkan/vulkan.h\n' "$VULKAN_INCLUDE_DIR"
            printf 'Vulkan loader:  available (-lvulkan)\n'
            printf 'ROCm/AMD SMI:   optional, loaded at runtime from /opt/rocm (no build dependency)\n'
            ;;
    esac
    echo "all selected backend dependencies are present"
    exit 0
fi

for asset in index.html style.css app.js html2canvas.min.js favicon.svg; do
    if [ ! -f "$ROOT/web/$asset" ]; then
        echo "web asset not found: $ROOT/web/$asset" >&2
        exit 1
    fi
done

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
    emit_asset CTOP_WEB_HTML2CANVAS "$ROOT/web/html2canvas.min.js"
    emit_asset CTOP_WEB_FAVICON "$ROOT/web/favicon.svg"
    printf '#endif\n'
} > "$TMP/web_assets.h"

set -- -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -I"$TMP"
case $SELECTED_BACKEND in
    nvidia|both) set -- "$@" -DCTOP_ENABLE_NVML=1 -I"$NVML_INCLUDE_DIR" ;;
esac
case $SELECTED_BACKEND in
    vulkan|both) set -- "$@" -DCTOP_ENABLE_VULKAN=1 -I"$VULKAN_INCLUDE_DIR" ;;
esac

printf 'building GPU backend: %s\n' "$SELECTED_BACKEND"
case $SELECTED_BACKEND in
    vulkan|both)
        if [ -n "$VULKAN_LIBRARY_DIR" ]; then
            gcc "$@" "$ROOT/ctop.c" -ldl -lm -L"$VULKAN_LIBRARY_DIR" -lvulkan -o "$ROOT/ctop"
        else
            gcc "$@" "$ROOT/ctop.c" -ldl -lm -lvulkan -o "$ROOT/ctop"
        fi
        ;;
    nvidia)
        gcc "$@" "$ROOT/ctop.c" -ldl -lm -o "$ROOT/ctop"
        ;;
esac
printf 'built %s\n' "$ROOT/ctop"
