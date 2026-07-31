#!/bin/bash
set -e
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

IMG=/root/llamaste-build/output/images/llamaste.img
KERNEL=/root/llamaste-build/output/images/bzImage
TEST_DISK=/tmp/llamaste-test-16g.img

cp "$IMG" "$TEST_DISK"
truncate -s 16G "$TEST_DISK"

boot_and_check() {
    local LABEL=$1
    echo "=== $LABEL ==="
    qemu-system-x86_64 -m 4G -smp 2 -nographic -no-reboot \
      -drive file="$TEST_DISK",format=raw,if=virtio \
      -append 'console=ttyS0 root=/dev/vda3 rootfstype=squashfs ro init=/opt/llamaste/llamaste ip=dhcp' \
      -kernel "$KERNEL" \
      -net nic,model=e1000 -net user,hostfwd=tcp::8088-:80 \
      </dev/null >/tmp/qemu-resize-boot.log 2>&1 &
    local QPID=$!

    # Wait for HTTP
    for i in $(seq 1 20); do
        if curl -s -o /dev/null -w '%{http_code}' http://localhost:8088/health 2>/dev/null | grep -q 200; then
            break
        fi
        sleep 1
    done

    echo "Resize log:"
    curl -s http://localhost:8088/llamaste/debug/resize-log 2>&1 || echo "(no response)"
    echo ""
    echo "Disk size:"
    curl -s http://localhost:8088/llamaste/system 2>&1 | grep disk_total || echo "(no response)"

    kill $QPID 2>/dev/null
    wait $QPID 2>/dev/null || true
    sleep 2
}

boot_and_check "FIRST BOOT (should resize)"
echo ""
boot_and_check "SECOND BOOT (should skip resize)"

rm -f "$TEST_DISK"
echo ""
echo "Done."
