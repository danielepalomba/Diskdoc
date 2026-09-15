#!/usr/bin/env bash
# Uninstall diskdoc: remove the installed binary and local build artifacts.
# Dependencies installed by install.sh (smartmontools, libcurl) are left untouched.
# Your API key is kept unless --purge is given.
set -euo pipefail

PURGE=0
for arg in "$@"; do
    case "$arg" in
        --purge) PURGE=1 ;;
        *) echo "Usage: $0 [--purge]" >&2; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
cd "$SCRIPT_DIR"

as_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    else
        sudo "$@"
    fi
}

# Resolves the config directory of the human running this, not of root: under
# sudo both HOME and XDG_CONFIG_HOME still belong to root.
config_dir() {
    if [ "$(id -u)" -eq 0 ] && [ -n "${SUDO_USER:-}" ]; then
        local home
        home="$(getent passwd "$SUDO_USER" | cut -d: -f6)" || home=""
        if [ -n "$home" ]; then
            echo "$home/.config/diskdoc"
            return
        fi
    fi

    echo "${XDG_CONFIG_HOME:-$HOME/.config}/diskdoc"
}

# Runs a command on a file, asking for root only when the file is not ours to
# write: deleting our own key must never require a password.
run_on_file() {
    local file="$1"
    shift

    if [ -w "$file" ]; then
        "$@"
    else
        as_root "$@"
    fi
}

# Overwrites a file before unlinking it, so the key does not survive in the
# blocks the filesystem hands to the next file.
shred_file() {
    if command -v shred &>/dev/null; then
        run_on_file "$1" shred -u "$1"
        return
    fi

    local size
    size="$(run_on_file "$1" stat -c %s "$1")"
    run_on_file "$1" dd if=/dev/zero of="$1" bs=1 count="$size" conv=notrunc status=none
    run_on_file "$1" rm -f "$1"
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

LEGACY_ENV_PATH="/etc/diskdoc/.env"
if [ -f "$LEGACY_ENV_PATH" ]; then
    echo "Removing $LEGACY_ENV_PATH (sudo required)..."
    shred_file "$LEGACY_ENV_PATH"
    as_root rmdir --ignore-fail-on-non-empty "$(dirname "$LEGACY_ENV_PATH")" 2>/dev/null || true
    echo "That key was stored system-wide: rotate it on the provider's console."
fi

CONFIG_DIR="$(config_dir)"
CREDENTIALS="$CONFIG_DIR/credentials"

if [ -f "$CREDENTIALS" ]; then
    if [ "$PURGE" -eq 1 ]; then
        echo "Removing $CREDENTIALS..."
        shred_file "$CREDENTIALS"
        rmdir "$CONFIG_DIR" 2>/dev/null || true
    else
        echo "Your API key is left untouched in $CREDENTIALS"
        echo "Run './uninstall.sh --purge' to delete it as well."
    fi
fi

echo
echo "Uninstall completed."
