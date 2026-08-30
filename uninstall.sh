#!/usr/bin/env bash
# Uninstall diskdoc: remove the installed binary and local build artifacts.
# Dependencies installed by install.sh (smartmontools, libcurl) are left untouched.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
cd "$SCRIPT_DIR"

as_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    else
        sudo "$@"
    fi
}

PREFIX="${PREFIX:-/usr/local}"
BIN_PATH="$PREFIX/bin/diskdoc"

if [ -f "$BIN_PATH" ] || command -v diskdoc &>/dev/null; then
    echo "Removing $BIN_PATH (sudo required)..."
    as_root make uninstall PREFIX="$PREFIX"
else
    echo "diskdoc binary not found in $PREFIX/bin, skipping."
fi

if [ -d build ]; then
    echo "Removing local build directory..."
    as_root rm -rf build
fi

# if [ -f .env ]; then
#     echo "Removing local .env..."
#     rm -f .env
# fi

SYSTEM_ENV_PATH="/etc/diskdoc/.env"
if [ -f "$SYSTEM_ENV_PATH" ]; then
    echo "Removing $SYSTEM_ENV_PATH (sudo required)..."
    as_root rm -f "$SYSTEM_ENV_PATH"
    as_root rmdir --ignore-fail-on-non-empty "$(dirname "$SYSTEM_ENV_PATH")" 2>/dev/null || true
fi

echo
echo "Uninstall completed."
