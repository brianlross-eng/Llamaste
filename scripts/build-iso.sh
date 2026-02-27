#!/bin/bash
# build-iso.sh — Build a hybrid BIOS+UEFI bootable ISO for Llamaste
#
# Usage: ./scripts/build-iso.sh [BUILD_DIR]
#   BUILD_DIR defaults to /root/llamaste-build
#
# Prerequisites:
#   apt install grub-common grub-pc-bin grub-efi-amd64-bin xorriso mtools
#
# Produces: llamaste.iso in BUILD_DIR/output/images/

set -e

BUILD_DIR="${1:-/root/llamaste-build}"
IMAGES_DIR="${BUILD_DIR}/output/images"
TARGET_DIR="${BUILD_DIR}/output/target"
BOARD_DIR="$(dirname "$(readlink -f "$0")")/../br2-external/board/llamaste"

# Verify prerequisites
for tool in grub-mkrescue xorriso mtools; do
    if ! command -v "$tool" &>/dev/null 2>&1; then
        # mtools is a library, check mcopy instead
        if [ "$tool" = "mtools" ]; then
            if ! command -v mcopy &>/dev/null 2>&1; then
                echo "ERROR: $tool not found. Install: apt install $tool"
                exit 1
            fi
        else
            echo "ERROR: $tool not found. Install: apt install grub-common grub-pc-bin grub-efi-amd64-bin xorriso mtools"
            exit 1
        fi
    fi
done

# Verify Buildroot output exists
for file in bzImage rootfs.squashfs llamaste.img; do
    if [ ! -f "${IMAGES_DIR}/${file}" ]; then
        echo "ERROR: ${IMAGES_DIR}/${file} not found"
        echo "Run the Buildroot build first: make -C ${BUILD_DIR}"
        exit 1
    fi
done

echo "=== Building Llamaste ISO ==="
echo ""

# --- Step 1: Create ISO root directory ---
ISO_ROOT=$(mktemp -d)
trap "rm -rf ${ISO_ROOT}" EXIT

echo "[iso] Creating ISO directory structure..."

mkdir -p "${ISO_ROOT}/boot/grub"
mkdir -p "${ISO_ROOT}/opt/llamaste"
mkdir -p "${ISO_ROOT}/install"

# --- Step 2: Copy kernel ---
echo "[iso] Copying kernel..."
cp "${IMAGES_DIR}/bzImage" "${ISO_ROOT}/boot/bzImage"

# --- Step 3: Copy GRUB config for live boot ---
echo "[iso] Copying GRUB live config..."
if [ -f "${BOARD_DIR}/grub-live.cfg" ]; then
    cp "${BOARD_DIR}/grub-live.cfg" "${ISO_ROOT}/boot/grub/grub.cfg"
else
    echo "ERROR: grub-live.cfg not found at ${BOARD_DIR}/grub-live.cfg"
    exit 1
fi

# --- Step 4: Copy llamaste binary ---
echo "[iso] Copying llamaste binary..."
# Extract from target directory (Buildroot's install location)
if [ -f "${TARGET_DIR}/opt/llamaste/llamaste" ]; then
    cp "${TARGET_DIR}/opt/llamaste/llamaste" "${ISO_ROOT}/opt/llamaste/llamaste"
    chmod 755 "${ISO_ROOT}/opt/llamaste/llamaste"
else
    echo "ERROR: llamaste binary not found in ${TARGET_DIR}/opt/llamaste/"
    exit 1
fi

# Copy web assets if they exist (for development mode fallback)
if [ -d "${TARGET_DIR}/opt/llamaste/web" ]; then
    cp -r "${TARGET_DIR}/opt/llamaste/web" "${ISO_ROOT}/opt/llamaste/web"
fi

# --- Step 5: Compress and copy disk image for installer ---
echo "[iso] Compressing disk image for installer..."
IMG_SIZE=$(stat -c %s "${IMAGES_DIR}/llamaste.img" 2>/dev/null || stat -f %z "${IMAGES_DIR}/llamaste.img")
echo "[iso]   Source image: $(echo "scale=1; ${IMG_SIZE} / 1048576" | bc) MB"

if [ -f "${IMAGES_DIR}/llamaste.img.xz" ]; then
    echo "[iso]   Using existing compressed image"
    cp "${IMAGES_DIR}/llamaste.img.xz" "${ISO_ROOT}/install/llamaste.img.xz"
else
    echo "[iso]   Compressing with xz -3 (this may take a minute)..."
    xz -3 -T0 -c "${IMAGES_DIR}/llamaste.img" > "${ISO_ROOT}/install/llamaste.img.xz"
fi

XZ_SIZE=$(stat -c %s "${ISO_ROOT}/install/llamaste.img.xz" 2>/dev/null || stat -f %z "${ISO_ROOT}/install/llamaste.img.xz")
echo "[iso]   Compressed image: $(echo "scale=1; ${XZ_SIZE} / 1048576" | bc) MB"

# --- Step 6: Copy squashfs for reference ---
echo "[iso] Copying rootfs.squashfs..."
cp "${IMAGES_DIR}/rootfs.squashfs" "${ISO_ROOT}/install/rootfs.squashfs"

# --- Step 7: Add Ventoy compatibility marker ---
touch "${ISO_ROOT}/ventoy.dat"

# --- Step 8: Build the ISO ---
echo "[iso] Building hybrid ISO with grub-mkrescue..."
ISO_OUTPUT="${IMAGES_DIR}/llamaste.iso"

grub-mkrescue -o "${ISO_OUTPUT}" "${ISO_ROOT}" \
    -- -volid LLAMASTE 2>&1 | while read -r line; do
    # Show progress but filter noise
    case "$line" in
        *xorriso*|*WARNING*|*FAILURE*) echo "  $line" ;;
    esac
done

# Verify output
if [ ! -f "${ISO_OUTPUT}" ]; then
    echo "ERROR: ISO creation failed"
    exit 1
fi

ISO_SIZE=$(stat -c %s "${ISO_OUTPUT}" 2>/dev/null || stat -f %z "${ISO_OUTPUT}")

echo ""
echo "======================================================="
echo "  Llamaste ISO ready: ${ISO_OUTPUT}"
echo "  Size: $(echo "scale=1; ${ISO_SIZE} / 1048576" | bc) MB"
echo ""
echo "  Test with QEMU:"
echo "    qemu-system-x86_64 -cdrom ${ISO_OUTPUT} -m 512M -nographic"
echo ""
echo "  Test with installer (second disk):"
echo "    qemu-img create -f raw target.img 4G"
echo "    qemu-system-x86_64 -cdrom ${ISO_OUTPUT} \\"
echo "      -drive file=target.img,format=raw,if=virtio \\"
echo "      -m 512M -nographic \\"
echo "      -net nic,model=e1000 -net user,hostfwd=tcp::8080-:80"
echo "======================================================="
echo ""
