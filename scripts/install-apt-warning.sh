#!/bin/bash
# Install the APT reminder about the out-of-tree HP OMEN keyboard driver.

set -e

SRC_DIR=$(cd "$(dirname "$0")" && pwd)

if [ "$EUID" -ne 0 ]; then
    echo "Please run as root (use sudo)"
    exit 1
fi

if [ "${1:-}" = "--remove" ]; then
    rm -f /usr/local/sbin/omen-driver-warning
    rm -f /etc/apt/apt.conf.d/99omen-driver-warning
    echo "APT reminder removed."
    exit 0
fi

install -Dm755 "$SRC_DIR/omen-driver-warning.sh" \
    /usr/local/sbin/omen-driver-warning
install -Dm644 "$SRC_DIR/99omen-driver-warning" \
    /etc/apt/apt.conf.d/99omen-driver-warning

echo "APT reminder installed."
echo "Test it with: /usr/local/sbin/omen-driver-warning"
echo "Remove it with: sudo $0 --remove"
