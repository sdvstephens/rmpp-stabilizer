#!/bin/bash
# rmpp-stabilizer install script
# Copies library to device and configures xochitl to use it

set -e

DEVICE_IP="${1:-10.11.99.1}"
LIB_NAME="libstabilizer.so"
REMOTE_PATH="/home/root/$LIB_NAME"
SERVICE_OVERRIDE="/etc/systemd/system/xochitl.service.d/stabilizer.conf"

echo "=== rmpp-stabilizer installer ==="
echo "Device: $DEVICE_IP"
echo ""

# Check library exists
if [ ! -f "$LIB_NAME" ]; then
    echo "Error: $LIB_NAME not found. Run 'make' first."
    exit 1
fi

echo "[1/4] Copying library to device..."
scp "$LIB_NAME" "root@$DEVICE_IP:$REMOTE_PATH"

echo "[2/4] Creating systemd override..."
ssh "root@$DEVICE_IP" bash -s <<'EOF'
mount -o remount,rw /
mkdir -p /etc/systemd/system/xochitl.service.d
cat > /etc/systemd/system/xochitl.service.d/stabilizer.conf <<'UNIT'
[Service]
Environment=LD_PRELOAD=/home/root/libstabilizer.so
UNIT
mount -o remount,ro /
EOF

echo "[3/4] Creating default config..."
ssh "root@$DEVICE_IP" bash -s <<'EOF'
cat > /home/root/.stabilizer.conf <<'CONF'
algorithm=string_pull
strength=0.5
pressure_smoothing=false
tilt_smoothing=false
CONF
EOF

echo "[4/4] Restarting xochitl..."
ssh "root@$DEVICE_IP" "systemctl daemon-reload && systemctl restart xochitl"

echo ""
echo "✓ rmpp-stabilizer installed!"
echo "  Config: ssh root@$DEVICE_IP 'cat ~/.stabilizer.conf'"
echo "  To uninstall: ./scripts/uninstall.sh"
