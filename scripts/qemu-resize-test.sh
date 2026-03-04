#!/bin/bash
# Quick QEMU test for partition auto-resize
set -e
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

IMG=/root/llamaste-build/output/images/llamaste.img
KERNEL=/root/llamaste-build/output/images/bzImage
TEST_DISK=/tmp/llamaste-test-16g.img

# Create 16GB test disk from raw image
cp "$IMG" "$TEST_DISK"
truncate -s 16G "$TEST_DISK"
echo "Test disk: $(ls -lh $TEST_DISK | awk '{print $5}')"

# Verify partitions before boot
echo "=== PARTITION TABLE BEFORE BOOT ==="
fdisk -l "$TEST_DISK" 2>/dev/null | grep -E "Disk |Device|/dev" || echo "(fdisk not available)"

# Boot with direct kernel, virtio disk (matching working qemu-boot-test.sh)
qemu-system-x86_64 \
  -m 4G -smp 2 \
  -nographic \
  -no-reboot \
  -drive file="$TEST_DISK",format=raw,if=virtio \
  -append 'console=ttyS0 root=/dev/vda3 rootfstype=squashfs ro init=/opt/llamaste/llamaste ip=dhcp' \
  -kernel "$KERNEL" \
  -net nic,model=e1000 -net user,hostfwd=tcp::8088-:80 \
  </dev/null >/tmp/qemu-resize-boot.log 2>&1 &

QEMU_PID=$!
echo "QEMU started (PID $QEMU_PID)"

# Wait for HTTP server
echo "Waiting for HTTP server..."
READY=0
for i in $(seq 1 45); do
    CODE=$(curl -s -o /dev/null -w '%{http_code}' http://localhost:8088/health 2>/dev/null || echo "000")
    if [ "$CODE" = "200" ]; then
        echo "Server ready after ${i}s"
        READY=1
        break
    fi
    sleep 1
done

if [ "$READY" = "0" ]; then
    echo "TIMEOUT: Server did not start in 45s"
    echo "--- QEMU log (last 40 lines) ---"
    tail -40 /tmp/qemu-resize-boot.log
    kill $QEMU_PID 2>/dev/null
    exit 1
fi

echo ""
echo "=== RESIZE LOG ==="
curl -s http://localhost:8088/llamaste/debug/resize-log 2>&1

echo ""
echo "=== SYSTEM INFO ==="
curl -s http://localhost:8088/llamaste/system 2>&1 | python3 -m json.tool 2>/dev/null

echo ""
echo "=== QEMU LOG (init lines) ==="
grep -E '\[init\]|\[main\]|Grew|resize' /tmp/qemu-resize-boot.log 2>/dev/null || echo "(none)"

# Cleanup
kill $QEMU_PID 2>/dev/null
wait $QEMU_PID 2>/dev/null || true
rm -f "$TEST_DISK"
echo ""
echo "Done."
