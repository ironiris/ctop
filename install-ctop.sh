#!/bin/sh
set -eu

REPOSITORY=https://github.com/ironiris/ctop
PREFIX=${PREFIX:-}
PREFIX_EXPLICIT=0
[ -n "$PREFIX" ] && PREFIX_EXPLICIT=1
BINDIR=${CTOP_BINDIR:-}
BINDIR_EXPLICIT=0
[ -n "$BINDIR" ] && BINDIR_EXPLICIT=1
BACKEND=${CTOP_BACKEND:-auto}
DESTDIR=${DESTDIR:-}

usage() {
    cat <<'EOF'
Usage: install-ctop.sh [--prefix DIR|--bindir DIR] [--backend auto|nvidia|vulkan|both]

Always checks GitHub for the latest ctop release, downloads its source,
builds it, and updates the installed executable.

Options:
  --prefix DIR       install as DIR/bin/ctop (default: existing /usr/bin/ctop, else /usr/local/bin)
  --bindir DIR       install as DIR/ctop (for a custom service executable path)
  --backend NAME     choose GPU backend (default: auto)
  -h, --help         show this help

Environment:
  PREFIX             same as --prefix
  CTOP_BINDIR        same as --bindir
  CTOP_BACKEND       same as --backend
  DESTDIR            staging root for packaging
EOF
}

while [ $# -gt 0 ]; do
    case $1 in
        --prefix)
            [ $# -ge 2 ] || { echo "--prefix requires a directory" >&2; exit 2; }
            PREFIX=$2
            PREFIX_EXPLICIT=1
            shift
            ;;
        --prefix=*) PREFIX=${1#*=}; PREFIX_EXPLICIT=1 ;;
        --bindir)
            [ $# -ge 2 ] || { echo "--bindir requires a directory" >&2; exit 2; }
            BINDIR=$2
            BINDIR_EXPLICIT=1
            shift
            ;;
        --bindir=*) BINDIR=${1#*=}; BINDIR_EXPLICIT=1 ;;
        --backend)
            [ $# -ge 2 ] || { echo "--backend requires a name" >&2; exit 2; }
            BACKEND=$2
            shift
            ;;
        --backend=*) BACKEND=${1#*=} ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'Unknown option: %s\n\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

case $BACKEND in
    auto|nvidia|vulkan|both) ;;
    *) printf 'Invalid backend: %s\n' "$BACKEND" >&2; exit 2 ;;
esac

if [ -z "$PREFIX" ]; then
    if [ -x /usr/bin/ctop ]; then PREFIX=/usr; else PREFIX=/usr/local; fi
fi

command -v curl >/dev/null 2>&1 || { echo "curl is required to download ctop." >&2; exit 1; }
command -v tar >/dev/null 2>&1 || { echo "tar is required to unpack ctop source." >&2; exit 1; }
command -v awk >/dev/null 2>&1 || { echo "awk is required to read release metadata." >&2; exit 1; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ctop-install.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

printf 'Checking the latest ctop release...\n'
curl -fsSL "https://api.github.com/repos/ironiris/ctop/releases/latest" -o "$TMP/release.json"
RELEASE=$(awk -F '"' '/"tag_name"[[:space:]]*:/ { print $4; exit }' "$TMP/release.json")
case $RELEASE in
    v[0-9]*) ;;
    *) printf 'Could not determine a valid latest release tag: %s\n' "$RELEASE" >&2; exit 1 ;;
esac

echo "Downloading ctop $RELEASE source."
curl -fsSL "$REPOSITORY/archive/refs/tags/$RELEASE.tar.gz" -o "$TMP/ctop.tar.gz"
mkdir "$TMP/source"
tar -xzf "$TMP/ctop.tar.gz" -C "$TMP/source"
BUILD_SCRIPT=$(find "$TMP/source" -type f -name build-ctop.sh -print -quit)
[ -n "$BUILD_SCRIPT" ] || { echo "Could not find build-ctop.sh in the source archive." >&2; exit 1; }
ROOT=$(dirname -- "$BUILD_SCRIPT")

"$ROOT/build-ctop.sh" --backend "$BACKEND"
BINARY="$ROOT/ctop"
if [ "$PREFIX_EXPLICIT" -eq 1 ] && [ "$BINDIR_EXPLICIT" -eq 1 ]; then
    echo "Choose either --prefix or --bindir, not both." >&2
    exit 2
fi
if [ -n "$BINDIR" ]; then
    TARGET="$DESTDIR$BINDIR/ctop"
else
    TARGET="$DESTDIR$PREFIX/bin/ctop"
fi

install_binary() {
    staged="$TARGET.new.$$"
    if [ -n "$DESTDIR" ]; then
        install -D -m 755 "$BINARY" "$staged"
        mv -f "$staged" "$TARGET"
    elif install -D -m 755 "$BINARY" "$staged" 2>/dev/null; then
        mv -f "$staged" "$TARGET"
    elif command -v sudo >/dev/null 2>&1; then
        echo "Elevated permission is needed to update $TARGET."
        sudo install -D -m 755 "$BINARY" "$staged" && sudo mv -f "$staged" "$TARGET"
    else
        return 1
    fi
}

if ! install_binary; then
    if [ "$BINDIR_EXPLICIT" -eq 0 ] && [ "$PREFIX_EXPLICIT" -eq 0 ] && [ "$PREFIX" = /usr ]; then
        echo "Cannot update /usr/bin; falling back to /usr/local/bin."
        PREFIX=/usr/local
        TARGET="$DESTDIR$PREFIX/bin/ctop"
        install_binary || { echo "Installation to $TARGET failed." >&2; exit 1; }
    else
        printf 'Cannot update %s; choose a writable --prefix or configure sudo.\n' "$TARGET" >&2
        exit 1
    fi
fi

printf 'Installed %s to %s\n' "$($BINARY --version)" "$TARGET"
echo "Restart any running ctop process to use the updated executable."
