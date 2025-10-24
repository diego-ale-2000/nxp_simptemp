#!/usr/bin/env bash
set -e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KO_FILE="$ROOT_DIR/kernel/nxp_simtemp.ko"
DEVICE="/dev/simtemp"
SYSFS_DIR="/sys/class/misc/simtemp"
CLI="$ROOT_DIR/user/cli/main.py"

echo "🚀 Inserting module..."
sudo insmod "$KO_FILE" || { echo "❌ insmod failed"; exit 1; }
sudo chmod 666 /dev/simtemp

sleep 1  # give time for probe()

if [ ! -e "$DEVICE" ]; then
    echo "❌ Device node not found: $DEVICE"
    sudo rmmod nxp_simtemp || true
    exit 1
fi

echo "⚙️  Configuring parameters..."
echo 500 | sudo tee "$SYSFS_DIR/sampling_ms" >/dev/null
echo 40500 | sudo tee "$SYSFS_DIR/threshold_mC" >/dev/null
sudo python3 "$ROOT_DIR/user/cli/main.py" --mode normal >/dev/null

echo "📋 Current configuration:"
echo "  sampling_ms:   $(cat "$SYSFS_DIR/sampling_ms")"
echo "  threshold_mC:  $(cat "$SYSFS_DIR/threshold_mC")"
echo "  mode:          $(cat "$SYSFS_DIR/mode")"

echo
echo "▶️  Running CLI test (10 seconds)..."
timeout 10 python3 "$CLI" || echo "(CLI exited)"

echo "📊 Current stats:"
cat "$SYSFS_DIR/stats" || true

echo "🧹 Removing module..."
sudo rmmod nxp_simtemp || { echo "⚠️ Failed to remove module"; exit 1; }

echo "✅ Demo completed successfully!"
