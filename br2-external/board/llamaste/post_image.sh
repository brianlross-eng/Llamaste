#!/bin/bash
# Llamaste post-image script
# Called by Buildroot after image generation to assemble the final disk image.

set -e

BOARD_DIR=$(dirname "$0")
GENIMAGE_CFG="${BOARD_DIR}/genimage.cfg"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"

echo "[post-image] Assembling Llamaste disk image..."

# --- Step 1: Prepare data partition overlay ---
DATA_OVERLAY="${BINARIES_DIR}/data-overlay"
mkdir -p "${DATA_OVERLAY}/models"
mkdir -p "${DATA_OVERLAY}/llamaste/conversations"
mkdir -p "${DATA_OVERLAY}/llamaste/config"
mkdir -p "${DATA_OVERLAY}/llamaste/logs"
mkdir -p "${DATA_OVERLAY}/llamaste/cache"

if [ ! -f "${DATA_OVERLAY}/llamaste/config/llamaste.json" ]; then
    cat > "${DATA_OVERLAY}/llamaste/config/llamaste.json" << 'DEFAULTCFG'
{
    "version": 1,
    "model": "auto",
    "listen": "0.0.0.0",
    "port": 8080,
    "threads": 0,
    "context_size": 2048,
    "log_level": "info"
}
DEFAULTCFG
fi

# --- Step 2: Create GRUB BIOS boot image ---
GRUB_BIOS_IMG="${BINARIES_DIR}/grub-bios.img"
if [ -f "${HOST_DIR}/lib/grub/i386-pc/boot.img" ]; then
    echo "[post-image] Creating GRUB BIOS boot image..."
    "${HOST_DIR}/usr/bin/grub-mkimage" \
        -O i386-pc \
        -o "${GRUB_BIOS_IMG}" \
        -p "(hd0,gpt2)/grub" \
        part_gpt fat ext2 normal boot linux configfile search \
        search_fs_uuid search_label loadenv test echo
else
    echo "[post-image] GRUB i386-pc not available, creating empty BIOS boot image"
    dd if=/dev/zero of="${GRUB_BIOS_IMG}" bs=1024 count=1024 2>/dev/null
fi

# --- Step 3: Set up EFI partition ---
EFI_GRUB_DIR="${BINARIES_DIR}/efi-part/grub"
mkdir -p "${EFI_GRUB_DIR}"
mkdir -p "${BINARIES_DIR}/efi-part/EFI/BOOT"

if [ -f "${BOARD_DIR}/grub.cfg" ]; then
    # Main GRUB config (loaded by BIOS GRUB via prefix "(hd0,gpt2)/grub")
    cp "${BOARD_DIR}/grub.cfg" "${EFI_GRUB_DIR}/grub.cfg"
    # ALSO overwrite the EFI fallback config (loaded by EFI GRUB bootx64.efi)
    # Buildroot creates a wrong default here — we must replace it with ours
    cp "${BOARD_DIR}/grub.cfg" "${BINARIES_DIR}/efi-part/EFI/BOOT/grub.cfg"
fi

# --- Step 3b: Create GRUB environment block (1024 bytes) ---
# grubenv stores A/B slot state. GRUB's load_env/save_env reads/writes this file.
# Format: header line + key=value lines + '#' padding to exactly 1024 bytes.
echo "[post-image] Creating grubenv for A/B boot..."
GRUBENV_CONTENT="# GRUB Environment Block\nactive_slot=A\nboot_success=1\n"
GRUBENV_LEN=$(printf "${GRUBENV_CONTENT}" | wc -c)
PAD_LEN=$((1024 - GRUBENV_LEN))
GRUBENV_FILE="${BINARIES_DIR}/efi-part/grubenv_tmp"
printf "${GRUBENV_CONTENT}" > "${GRUBENV_FILE}"
dd if=/dev/zero bs=1 count="${PAD_LEN}" 2>/dev/null | tr '\0' '#' >> "${GRUBENV_FILE}"
# Place in both locations (BIOS and EFI)
cp "${GRUBENV_FILE}" "${EFI_GRUB_DIR}/grubenv"
cp "${GRUBENV_FILE}" "${BINARIES_DIR}/efi-part/EFI/BOOT/grubenv"
rm -f "${GRUBENV_FILE}"

if [ -f "${BINARIES_DIR}/bzImage" ]; then
    cp "${BINARIES_DIR}/bzImage" "${BINARIES_DIR}/efi-part/bzImage"
fi

if [ ! -d "${BINARIES_DIR}/efi-part/EFI" ]; then
    echo "[post-image] WARNING: efi-part/EFI not found"
    mkdir -p "${BINARIES_DIR}/efi-part/EFI/BOOT"
fi

# --- Step 4: Build sub-images then assemble ---
# genimage uses --rootpath to populate filesystem images.
# We need different rootpaths for vfat (efi-part/) and ext4 (data-overlay/).
# Strategy: build vfat and ext4 sub-images separately, then assemble.

rm -rf "${GENIMAGE_TMP}"

# Build the ESP vfat from efi-part/
echo "[post-image] Creating ESP vfat image..."
VFAT_SIZE=$((32 * 1024 * 1024))
VFAT_IMG="${BINARIES_DIR}/efi-part.vfat"
dd if=/dev/zero of="${VFAT_IMG}" bs=1M count=32 2>/dev/null
# IMPORTANT: Do NOT force FAT32 (-F 32) on a 32 MB volume!
# FAT32 requires >=65525 clusters which 32 MB can't provide.
# Let mkdosfs auto-select FAT type (FAT16 for 32 MB, per UEFI spec).
mkdosfs -n ESP "${VFAT_IMG}" >/dev/null 2>&1
# Copy files into the vfat image using mcopy
mcopy -s -i "${VFAT_IMG}" "${BINARIES_DIR}/efi-part/EFI" "::/"
mcopy -s -i "${VFAT_IMG}" "${BINARIES_DIR}/efi-part/grub" "::/"
if [ -f "${BINARIES_DIR}/efi-part/bzImage" ]; then
    mcopy -i "${VFAT_IMG}" "${BINARIES_DIR}/efi-part/bzImage" "::/"
fi

# Build the data ext4 from data-overlay/
echo "[post-image] Creating DATA ext4 image..."
DATA_IMG="${BINARIES_DIR}/data.ext4"
dd if=/dev/zero of="${DATA_IMG}" bs=1M count=64 2>/dev/null
mkfs.ext4 -q -L DATA -d "${DATA_OVERLAY}" "${DATA_IMG}"

# Assemble final disk image using genimage
# rootfs.squashfs and grub-bios.img are already in BINARIES_DIR
echo "[post-image] Assembling final disk image..."
genimage \
    --rootpath "${BINARIES_DIR}" \
    --tmppath "${GENIMAGE_TMP}" \
    --inputpath "${BINARIES_DIR}" \
    --outputpath "${BINARIES_DIR}" \
    --config "${GENIMAGE_CFG}"

echo ""
echo "======================================================="
echo "  Llamaste disk image ready: ${BINARIES_DIR}/llamaste.img"
echo ""
echo "  Test with QEMU:"
echo "    qemu-system-x86_64 -m 4G -drive file=llamaste.img,format=raw"
echo "======================================================="
echo ""
