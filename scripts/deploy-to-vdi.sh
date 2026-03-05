#!/bin/bash
# deploy-to-vdi.sh — deploy new squashfs (SYS-A partition) from llamaste.img to llamaste-disk.vdi
set -e

IMG=/root/llamaste-build/output/images/llamaste.img
VDI=/mnt/d/Llamaste/vm/Llamaste/llamaste-disk.vdi
VDI_TMP=/mnt/d/Llamaste/vm/Llamaste/llamaste-disk-new.vdi
RAW=/tmp/llamaste-deploy.raw

echo "[deploy] Converting VDI -> RAW..."
qemu-img convert -p -f vdi -O raw "$VDI" "$RAW"
SIZE=$(du -h "$RAW" | cut -f1)
echo "[deploy] VDI converted: $SIZE"

echo "[deploy] Attaching images via loop..."
NEW_LOOP=$(losetup -f)
losetup --partscan "$NEW_LOOP" "$IMG"
sleep 1

VDI_LOOP=$(losetup -f)
losetup --partscan "$VDI_LOOP" "$RAW"
sleep 1

echo "[deploy] New image parts: $(ls ${NEW_LOOP}p* 2>/dev/null | tr '\n' ' ')"
echo "[deploy] VDI raw parts:   $(ls ${VDI_LOOP}p* 2>/dev/null | tr '\n' ' ')"

echo "[deploy] Copying SYS-A (partition 3)..."
dd if=${NEW_LOOP}p3 of=${VDI_LOOP}p3 bs=4M status=progress conv=fsync
echo "[deploy] SYS-A copied"

echo "[deploy] Detaching loop devices..."
losetup -d "$NEW_LOOP"
losetup -d "$VDI_LOOP"
sync

echo "[deploy] Converting RAW -> VDI (temp)..."
# Write to temp file to avoid permission issues with in-place overwrite
rm -f "$VDI_TMP"
qemu-img convert -p -f raw -O vdi "$RAW" "$VDI_TMP"
rm -f "$RAW"

echo "[deploy] Replacing original VDI..."
mv -f "$VDI_TMP" "$VDI"

echo "[deploy] Done! VDI updated at $VDI"
