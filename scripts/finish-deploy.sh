#!/bin/bash
set -e
VDI=/mnt/d/Llamaste/vm/Llamaste/llamaste-disk.vdi
VDI_NEW=/mnt/d/Llamaste/vm/Llamaste/llamaste-disk-new.vdi

echo "[deploy] Copying new VDI over original..."
cp "$VDI_NEW" "$VDI"
echo "[deploy] Removing temp file..."
rm -f "$VDI_NEW"
echo "[deploy] Done!"
ls -lh "$VDI"
