#!/bin/bash
# Build a minimal initramfs for PXE booting Llamaste
# Output: /tmp/initramfs.cpio.gz (~1MB)
set -e

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
INITRD_DIR=$(mktemp -d)
trap "rm -rf $INITRD_DIR" EXIT

cd "$INITRD_DIR"

echo "=== Building PXE initramfs ==="

# Download static busybox if needed
if [ ! -f /tmp/busybox ]; then
    echo "[initramfs] Downloading static busybox..."
    wget -q "https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox" -O /tmp/busybox
    chmod +x /tmp/busybox
fi

# Create directory structure
mkdir -p bin dev proc sys tmp mnt/root run

# Install busybox and create symlinks
cp /tmp/busybox bin/busybox
chmod 755 bin/busybox
for cmd in sh mount umount mkdir mknod wget ip losetup switch_root sleep cat echo ls udhcpc awk; do
    ln -s busybox "bin/$cmd"
done

# Create essential device nodes
mknod dev/console c 5 1
mknod dev/null c 1 3
mknod dev/loop0 b 7 0

# Copy init script
cp "$SCRIPT_DIR/pxe-init.sh" init
chmod 755 init

# Build cpio archive
echo "[initramfs] Creating cpio archive..."
find . | cpio -o -H newc 2>/dev/null | gzip > /tmp/initramfs.cpio.gz

echo "[initramfs] Done:"
ls -lh /tmp/initramfs.cpio.gz
