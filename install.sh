#!/usr/bin/env bash
# Install diskdoc: verify/install smartctl and libcurl, compile the project,
# and copy the binary to /usr/local/bin (already in the default PATH).
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

install_libcurl() {
    if printf '#include <curl/curl.h>\nint main(void){return 0;}\n' \
        | gcc -xc - -o /dev/null -lcurl &>/dev/null; then
        echo "libcurl already installed"
        return
    fi

    echo "libcurl not found, installing curl development package..."
    if command -v apt-get &>/dev/null; then
        as_root apt-get update
        as_root apt-get install -y libcurl4-openssl-dev
    elif command -v dnf &>/dev/null; then
        as_root dnf install -y libcurl-devel
    elif command -v pacman &>/dev/null; then
        as_root pacman -Sy --noconfirm curl
    elif command -v zypper &>/dev/null; then
        as_root zypper install -y libcurl-devel
    elif command -v apk &>/dev/null; then
        as_root apk add curl-dev
    else
        echo "Package manager not known: manually install the libcurl development package (e.g. libcurl4-openssl-dev)." >&2
        exit 1
    fi
}

LEGACY_ENV_PATH="/etc/diskdoc/.env"

warn_legacy_env() {
    if [ -f "$LEGACY_ENV_PATH" ]; then
        echo
        echo "WARNING: $LEGACY_ENV_PATH still holds an API key in a system-wide file."
        echo "Move it to your own account with 'diskdoc --set-key <provider>', then run"
        echo "./uninstall.sh or delete the file, and rotate the key: it was reachable by"
        echo "anyone able to run commands as root on this machine."
    fi
}

install_smartctl
install_libcurl

echo "Compiling diskdoc..."
make

echo "Installing  in /usr/local/bin (sudo required)..."
as_root make install

warn_legacy_env

echo
echo "Installation completed."
echo
echo "To use the AI analysis, store your API key with:"
echo "    diskdoc --set-key <openai|anthropic|gemini>"
echo "It is written to ~/.config/diskdoc/credentials with mode 0600."
