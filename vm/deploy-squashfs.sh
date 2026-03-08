#!/bin/bash
set -e

VDI=/mnt/d/Llamaste/vm/Llamaste2/llamaste-disk.vdi
SQUASHFS=/root/llamaste-build/output/images/rootfs.squashfs
RAW=/tmp/llamaste-deploy.raw

echo "[deploy] Squashfs: $(du -h $SQUASHFS | cut -f1)"

echo "[deploy] Step 1: Converting VDI -> RAW..."
qemu-img convert -f vdi -O raw "$VDI" "$RAW"
echo "[deploy] RAW: $(du -h $RAW | cut -f1)"

echo "[deploy] Step 2: Mounting via losetup..."
LOOP=$(losetup -f)
losetup --partscan "$LOOP" "$RAW"
sleep 1
ls ${LOOP}p*

echo "[deploy] Step 3: Writing squashfs to partition 3..."
dd if="$SQUASHFS" of="${LOOP}p3" bs=4M conv=fsync 2>&1
echo "[deploy] SYS-A updated"

echo "[deploy] Step 4: Cleanup..."
losetup -d "$LOOP"
sync

echo "[deploy] Step 5: Converting RAW -> VDI..."
rm -f "${VDI}.new"
qemu-img convert -f raw -O vdi "$RAW" "${VDI}.new"
rm -f "$RAW"
echo "[deploy] New VDI ready"

echo "[deploy] Step 6: Replace original..."
rm -f "$VDI"
mv "${VDI}.new" "$VDI"
echo "[deploy] DONE"
