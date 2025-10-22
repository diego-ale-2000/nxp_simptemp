#!/usr/bin/env bash
set -e  # fail fast
set -u  # undefined var = error

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL_DIR="/lib/modules/$(uname -r)/build"
MODULE_DIR="$ROOT_DIR/kernel"
USER_DIR="$ROOT_DIR/user"

echo "🔍 Checking kernel headers..."
if [ ! -d "$KERNEL_DIR" ]; then
    echo "❌ Kernel headers not found at $KERNEL_DIR"
    echo "   Try: sudo apt install linux-headers-$(uname -r)"
    exit 1
fi

echo "🛠️  Building kernel module..."
make -C "$KERNEL_DIR" M="$MODULE_DIR" modules

KO_FILE="$MODULE_DIR/nxp_simtemp.ko"
if [ ! -f "$KO_FILE" ]; then
    echo "❌ Build failed: module not found at $KO_FILE"
    exit 1
fi

echo "✅ Kernel module built: $KO_FILE"

echo "🐍 Checking Python dependencies..."
sudo apt install python3-matplotlib
sudo apt install python3-tk

echo "✅ Build complete!"
