#!/bin/bash
# rmpp-stabilizer uninstall script

set -e

DEVICE_IP="${1:-10.11.99.1}"

echo "=== rmpp-stabilizer uninstaller ==="

ssh "root@$DEVICE_IP" bash -s <<'EOF'
mount -o remount,rw /
rm -f /etc/systemd/system/xochitl.service.d/stabilizer.conf
rmdir --ignore-fail-on-non-empty /etc/systemd/system/xochitl.service.d/ 2>/dev/null
mount -o remount,ro /
rm -f /home/root/libstabilizer.so
rm -f /home/root/.stabilizer.conf
systemctl daemon-reload
systemctl restart xochitl
EOF

echo "✓ rmpp-stabilizer removed."
