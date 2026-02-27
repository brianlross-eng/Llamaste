#!/bin/bash
# Llamaste post-image script
# Called by Buildroot after image generation to assemble the final disk image.
#
# Environment variables (set by Buildroot):
#   BINARIES_DIR  - output/images/ (contains bzImage, rootfs.squashfs, etc.)
#   TARGET_DIR    - output/target/ (the target root filesystem)
#   BUILD_DIR     - output/build/  (build artifacts)
#   HOST_DIR      - output/host/   (host tools)
#   BR2_CONFIG    - the Buildroot .config file

set -e

BOARD_DIR=$(dirname "$0")
GENIMAGE_CFG="${BOARD_DIR}/genimage.cfg"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"

echo "[post-image] Assembling Llamaste disk image..."

# --- Step 1: Create data partition directory overlay ---
# These directories will be pre-populated into data.ext4
DATA_OVERLAY="${BINARIES_DIR}/data-overlay"
mkdir -p "${DATA_OVERLAY}/models"
mkdir -p "${DATA_OVERLAY}/llamaste/conversations"
mkdir -p "${DATA_OVERLAY}/llamaste/config"
mkdir -p "${DATA_OVERLAY}/llamaste/logs"
mkdir -p "${DATA_OVERLAY}/llamaste/cache"

# Create default config if it doesn't exist
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
# This is the core.img that goes into the BIOS boot partition
# grub-mkimage creates a standalone GRUB image for BIOS boot
GRUB_BIOS_IMG="${BINARIES_DIR}/grub-bios.img"
if [ -f "${HOST_DIR}/lib/grub/i386-pc/boot.img" ]; then
    echo "[post-image] Creating GRUB BIOS boot image..."
    "${HOST_DIR}/usr/bin/grub-mkimage" \
        -O i386-pc \
        -o "${GRUB_BIOS_IMG}" \
        -p "(hd0,gpt2)/grub" \
        part_gpt fat ext2 normal boot linux configfile search \
        search_fs_uuid search_label test echo
else
    echo "[post-image] GRUB i386-pc not available, creating empty BIOS boot image"
    # Create a 1MB empty image as placeholder for EFI-only systems
    dd if=/dev/zero of="${GRUB_BIOS_IMG}" bs=1024 count=1024 2>/dev/null
fi

# --- Step 3: Set up EFI partition directory structure ---
# Buildroot's GRUB2 EFI package creates efi-part/EFI/BOOT/bootx64.efi
# We also need grub.cfg in the right place for GRUB to find it
EFI_GRUB_DIR="${BINARIES_DIR}/efi-part/grub"
mkdir -p "${EFI_GRUB_DIR}"

# Copy grub.cfg into the EFI partition's grub directory
if [ -f "${BOARD_DIR}/grub.cfg" ]; then
    cp "${BOARD_DIR}/grub.cfg" "${EFI_GRUB_DIR}/grub.cfg"
fi

# Ensure the EFI directory structure exists (Buildroot should create this)
if [ ! -d "${BINARIES_DIR}/efi-part/EFI" ]; then
    echo "[post-image] WARNING: efi-part/EFI not found, GRUB EFI may not be built"
    mkdir -p "${BINARIES_DIR}/efi-part/EFI/BOOT"
fi

# --- Step 4: Run genimage to assemble the final disk image ---
echo "[post-image] Running genimage..."
rm -rf "${GENIMAGE_TMP}"

genimage \
    --rootpath "${DATA_OVERLAY}" \
    --tmppath "${GENIMAGE_TMP}" \
    --inputpath "${BINARIES_DIR}" \
    --outputpath "${BINARIES_DIR}" \
    --config "${GENIMAGE_CFG}"

echo ""
echo "======================================================="
echo "  Llamaste disk image ready: ${BINARIES_DIR}/llamaste.img"
echo ""
echo "  Partition layout (5-partition GPT):"
echo "    1. BIOS boot   (1 MB)   - GRUB legacy"
echo "    2. ESP          (256 MB) - GRUB EFI + kernel"
echo "    3. SYS-A        (256 MB) - Root filesystem (squashfs)"
echo "    4. SYS-B        (256 MB) - Reserved (A/B updates)"
echo "    5. DATA          (1 GB+) - Models, config, conversations"
echo ""
echo "  Flash to disk:"
echo "    dd if=${BINARIES_DIR}/llamaste.img of=/dev/sdX bs=4M status=progress"
echo "======================================================="
echo ""
