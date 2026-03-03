#!/bin/bash
# Test the installer flow in QEMU: boot ISO, install to target disk, verify PMBR
set -e

ISO="${1:-/root/llamaste-build/output/images/llamaste.iso}"
TARGET="/tmp/target-test.img"
PORT=9090

# Create target disk
echo "Creating 4 GB target disk..."
qemu-img create -f raw "$TARGET" 4G

# Start QEMU with ISO + target disk
echo "Starting QEMU with ISO + target disk..."
qemu-system-x86_64 \
    -cdrom "$ISO" \
    -drive file="$TARGET",format=raw,if=virtio \
    -m 512M -nographic \
    -net nic,model=e1000 -net user,hostfwd=tcp::${PORT}-:80 \
    -no-reboot &
QEMU_PID=$!

trap "kill $QEMU_PID 2>/dev/null; wait $QEMU_PID 2>/dev/null" EXIT

# Wait for HTTP server
echo "Waiting for HTTP server..."
for i in $(seq 1 60); do
    if curl -s http://localhost:${PORT}/health >/dev/null 2>&1; then
        echo "  HTTP server up after $i seconds"
        break
    fi
    sleep 1
done

# Health check
echo ""
echo "=== Health check ==="
curl -s http://localhost:${PORT}/health | python3 -m json.tool

# Detect disks
echo ""
echo "=== Detect disks ==="
curl -s http://localhost:${PORT}/install/disks | python3 -m json.tool

# Start installation
echo ""
echo "=== Starting installation ==="
curl -s -X POST http://localhost:${PORT}/install/start \
    -H "Content-Type: application/json" \
    -d '{"device":"/dev/vda"}' | python3 -m json.tool

# Poll progress
echo ""
echo "=== Installation progress ==="
for i in $(seq 1 120); do
    sleep 2
    PROGRESS=$(curl -s http://localhost:${PORT}/install/progress 2>/dev/null)
    PCT=$(echo "$PROGRESS" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('percent',0))" 2>/dev/null || echo "?")
    STATUS=$(echo "$PROGRESS" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('status',''))" 2>/dev/null || echo "?")
    FINISHED=$(echo "$PROGRESS" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('finished',False))" 2>/dev/null || echo "?")
    echo "  [$i] ${PCT}% - ${STATUS} (finished=${FINISHED})"

    if [ "$FINISHED" = "True" ]; then
        echo ""
        echo "=== Final result ==="
        echo "$PROGRESS" | python3 -m json.tool
        break
    fi
done

# Kill QEMU
kill $QEMU_PID 2>/dev/null
wait $QEMU_PID 2>/dev/null
trap - EXIT

# Verify the target disk
echo ""
echo "=== Target disk partition table ==="
fdisk -l "$TARGET" 2>&1 | head -20

echo ""
echo "=== PMBR hex dump ==="
hexdump -C -s 446 -n 16 "$TARGET"

echo ""
echo "=== PMBR size check ==="
python3 -c "
import struct
with open('$TARGET', 'rb') as f:
    f.seek(458)
    pmbr_size = struct.unpack('<I', f.read(4))[0]
    disk_size = 4 * 1024 * 1024 * 1024  # 4 GB
    disk_sectors = disk_size // 512
    print(f'PMBR size: {pmbr_size} sectors')
    print(f'Disk size: {disk_sectors} sectors')
    print(f'Match: {pmbr_size == disk_sectors - 1}')
    if pmbr_size == disk_sectors - 1:
        print('PMBR FIX VERIFIED!')
    else:
        print(f'MISMATCH: PMBR says {pmbr_size}, expected {disk_sectors - 1}')
"

# Now try booting from the installed target disk (EFI)
echo ""
echo "=== Attempting to boot installed system ==="
echo "Booting target disk..."

qemu-system-x86_64 \
    -drive file="$TARGET",format=raw,if=virtio \
    -m 512M -nographic \
    -net nic,model=e1000 -net user,hostfwd=tcp::${PORT}-:80 \
    -no-reboot &
QEMU_PID=$!
trap "kill $QEMU_PID 2>/dev/null; wait $QEMU_PID 2>/dev/null" EXIT

echo "Waiting for installed system to boot..."
BOOT_OK=false
for i in $(seq 1 60); do
    if curl -s http://localhost:${PORT}/health >/dev/null 2>&1; then
        echo "  Installed system booted after $i seconds!"
        BOOT_OK=true
        break
    fi
    sleep 1
    echo "  [$i] waiting..."
done

if $BOOT_OK; then
    echo ""
    echo "=== Installed system health ==="
    curl -s http://localhost:${PORT}/health | python3 -m json.tool
    echo ""
    echo "BOOT FROM INSTALLED DISK: PASS"
else
    echo ""
    echo "BOOT FROM INSTALLED DISK: FAIL (timed out after 60 seconds)"
fi

# Cleanup
kill $QEMU_PID 2>/dev/null
wait $QEMU_PID 2>/dev/null
trap - EXIT
rm -f "$TARGET"

echo ""
echo "Test complete."
