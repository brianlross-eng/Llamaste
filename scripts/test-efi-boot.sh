#!/bin/bash
# Test EFI boot with QEMU + OVMF
set -e
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

IMG="${1:-/root/llamaste-build/output/images/llamaste.img}"
PORT=9191

# Copy OVMF vars to a writable location
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/ovmf_vars.fd

echo "=== Testing EFI boot with OVMF ==="
echo "Image: $IMG"

qemu-system-x86_64 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=/tmp/ovmf_vars.fd \
    -drive file="$IMG",format=raw \
    -m 512M -nographic \
    -net nic,model=e1000 -net user,hostfwd=tcp::${PORT}-:80 \
    -no-reboot &
PID=$!
trap "kill $PID 2>/dev/null; wait $PID 2>/dev/null" EXIT

echo "Waiting for boot (QEMU PID=$PID)..."
for i in $(seq 1 60); do
    if curl -s http://localhost:${PORT}/health >/dev/null 2>&1; then
        echo ""
        echo "*** EFI BOOT SUCCESS after $i seconds! ***"
        curl -s http://localhost:${PORT}/health | python3 -m json.tool
        exit 0
    fi
    sleep 1
    if [ $((i % 10)) -eq 0 ]; then
        echo "  [$i] still waiting..."
    fi
done

echo ""
echo "*** EFI BOOT FAILED (timeout 60s) ***"
echo "QEMU is probably stuck at EFI shell or GRUB"
exit 1
