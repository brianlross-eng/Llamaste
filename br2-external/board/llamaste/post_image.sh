#!/bin/sh
set -eu

BOARD_DIR=$(dirname "$0")
BINARIES_DIR="$1"
GENIMAGE_TMP="${BINARIES_DIR}/genimage.tmp"

# Install GRUB for BIOS boot into the ESP image
# (genimage handles partition layout)

rm -rf "${GENIMAGE_TMP}"

genimage \
    --rootpath "${TARGET_DIR}" \
    --tmppath "${GENIMAGE_TMP}" \
    --inputpath "${BINARIES_DIR}" \
    --outputpath "${BINARIES_DIR}" \
    --config "${BOARD_DIR}/genimage.cfg"

echo ""
echo "=== Image ready: ${BINARIES_DIR}/llamaste.img ==="
echo ""
