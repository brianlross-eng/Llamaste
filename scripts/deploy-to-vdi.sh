#!/bin/bash
# deploy-to-vdi.sh — deploy new squashfs (SYS-A partition) to llamaste-disk.vdi
# Usage: bash /mnt/d/Llamaste/scripts/deploy-to-vdi.sh
#
# Writes rootfs.squashfs directly into VDI partition 3 (SYS-A).
# Also updates GRUB grubenv on ESP (partition 2) if post_image.sh artifacts exist.
# After this script, run finish-deploy.sh on Windows for file copy + UUID fix + VM start.
set -e

SQUASHFS=/root/llamaste-build/output/images/rootfs.squashfs
VDI=/mnt/d/Llamaste/vm/Llamaste2/llamaste-disk.vdi
VDI_TMP=/mnt/d/Llamaste/vm/Llamaste2/llamaste-disk-new.vdi
RAW=/tmp/llamaste-deploy.raw

if [ ! -f "$SQUASHFS" ]; then
    echo "[deploy] ERROR: $SQUASHFS not found. Run 'make llamaste-dirclean && make llamaste && make rootfs-squashfs' first."
    exit 1
fi

echo "[deploy] Squashfs: $(du -h "$SQUASHFS" | cut -f1)"

echo "[deploy] Step 1: Converting VDI -> RAW..."
qemu-img convert -p -f vdi -O raw "$VDI" "$RAW"
echo "[deploy] VDI converted: $(du -h "$RAW" | cut -f1)"

echo "[deploy] Step 2: Mounting raw via losetup..."
VDI_LOOP=$(losetup -f)
losetup --partscan "$VDI_LOOP" "$RAW"
sleep 1
echo "[deploy] Loop: $VDI_LOOP"
echo "[deploy] Partitions: $(ls ${VDI_LOOP}p* 2>/dev/null | tr '\n' ' ')"

echo "[deploy] Step 3: Writing squashfs to partition 3 (SYS-A)..."
dd if="$SQUASHFS" of="${VDI_LOOP}p3" bs=4M status=progress conv=fsync
echo "[deploy] SYS-A updated"

echo "[deploy] Step 4: Detaching loop device..."
losetup -d "$VDI_LOOP"
sync

echo "[deploy] Step 5: Converting RAW -> VDI (temp)..."
rm -f "$VDI_TMP"
qemu-img convert -p -f raw -O vdi "$RAW" "$VDI_TMP"
rm -f "$RAW"
echo "[deploy] New VDI: $(du -h "$VDI_TMP" | cut -f1)"

echo "[deploy] WSL2 side complete."
echo "[deploy] Now run: powershell.exe D:/Llamaste/scripts/finish-deploy.ps1"
