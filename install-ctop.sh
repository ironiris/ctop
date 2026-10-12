#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PREFIX=${PREFIX:-/usr/local}
DESTDIR=${DESTDIR:-}

"$ROOT/build-ctop.sh"
install -D -m 755 "$ROOT/ctop" "$DESTDIR$PREFIX/bin/ctop"
printf 'Installed ctop to %s\n' "$DESTDIR$PREFIX/bin/ctop"
