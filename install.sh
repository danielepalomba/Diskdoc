#!/usr/bin/env bash
# Install diskdoc: verify/install smartctl, compile the project, and
# copy the binary to /usr/local/bin (already in the default PATH).
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

install_smartctl() {
    if command -v smartctl &>/dev/null; then
        echo "smartctl already installed: $(command -v smartctl)"
        return
    fi

    echo "smartctl not found, installing smartmontools..."
    if command -v apt-get &>/dev/null; then
        as_root apt-get update
        as_root apt-get install -y smartmontools
    elif command -v dnf &>/dev/null; then
        as_root dnf install -y smartmontools
    elif command -v pacman &>/dev/null; then
        as_root pacman -Sy --noconfirm smartmontools
    elif command -v zypper &>/dev/null; then
        as_root zypper install -y smartmontools
    elif command -v apk &>/dev/null; then
        as_root apk add smartmontools
    else
        echo "Package manager not known: manually install smartmontools." >&2
        exit 1
    fi
}

install_smartctl

echo "Compiling diskdoc..."
make

echo "Installing  in /usr/local/bin (sudo required)..."
as_root make install

echo
echo "Installation completed."
