#!/bin/bash
# Test desktop mode boots and HTTP server responds
set -e
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

IMG=/root/llamaste-build/output/images/llamaste.img
KERNEL=/root/llamaste-build/output/images/bzImage
LOG=/tmp/qemu-desktop.log

qemu-system-x86_64 -m 512 -display none -device virtio-gpu-pci \
    -drive file="$IMG",format=raw \
    -serial file:"$LOG" \
    -append "console=ttyS0 llamaste.mode=desktop ip=dhcp root=/dev/sda3 rootfstype=squashfs init=/opt/llamaste/llamaste" \
    -kernel "$KERNEL" \
    -net nic -net user,hostfwd=tcp::8080-:80 &
QEMU_PID=$!
echo "QEMU PID: $QEMU_PID"

for i in $(seq 1 15); do
    result=$(curl -sf http://localhost:8080/health 2>/dev/null || true)
    if [ -n "$result" ]; then
        echo "HTTP ready after ${i}s"
        echo "$result"
        kill $QEMU_PID 2>/dev/null || true
        wait $QEMU_PID 2>/dev/null || true
        echo ""
        echo "=== Desktop mode serial output ==="
        grep -E '\[(child|supervisor|init|main|auth)\]' "$LOG" || true
        exit 0
    fi
    sleep 1
done

echo "TIMEOUT - HTTP server not ready after 15s"
tail -30 "$LOG"
kill $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true
exit 1
