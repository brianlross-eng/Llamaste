#!/bin/bash
# Quick boot test with KVM detection
set -e

PORT=8080
KERNEL=/root/llamaste-build/output/images/bzImage
IMAGE=/root/llamaste-build/output/images/llamaste.img
LOG=/tmp/qemu-kvm.log

# Kill any leftover QEMU
pkill -f qemu-system-x86_64 2>/dev/null || true
sleep 1

# Check KVM
CPU_ARGS=""
if [ -c /dev/kvm ]; then
    CPU_ARGS="-cpu host -enable-kvm"
    echo "Using KVM"
else
    echo "No KVM, using emulation (slow)"
fi

qemu-system-x86_64 \
    -m 4G -smp 4 \
    $CPU_ARGS \
    -kernel "$KERNEL" \
    -append "root=/dev/vda3 rootfstype=squashfs ro console=ttyS0 init=/opt/llamaste/llamaste llamaste.mode=server ip=dhcp" \
    -drive file="$IMAGE",format=raw,if=virtio \
    -netdev user,id=net0,hostfwd=tcp::${PORT}-:80 \
    -device virtio-net-pci,netdev=net0 \
    -nographic \
    >"$LOG" 2>&1 &
QEMU_PID=$!
echo "QEMU PID: $QEMU_PID"

for i in $(seq 1 120); do
    if curl -s --connect-timeout 2 --max-time 3 "http://localhost:${PORT}/health" 2>/dev/null | grep -q ok; then
        echo "UP after ${i}s"
        curl -s "http://localhost:${PORT}/health"
        echo ""
        kill $QEMU_PID 2>/dev/null
        wait $QEMU_PID 2>/dev/null || true
        exit 0
    fi
    if ! kill -0 $QEMU_PID 2>/dev/null; then
        echo "QEMU died"
        tail -20 "$LOG"
        exit 1
    fi
    sleep 1
done
echo "TIMEOUT after 120s"
echo "--- Log tail ---"
tail -30 "$LOG"
kill $QEMU_PID 2>/dev/null
wait $QEMU_PID 2>/dev/null || true
exit 1
