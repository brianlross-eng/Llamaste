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
# NOTE: Do NOT use ISO_ROOT=$(mktemp -d) — command substitution silently
# returns empty when this script is invoked through Git Bash → WSL pipeline,
# causing ISO_ROOT="" → grub-mkrescue tries to graft "/" → boot failure.
ISO_ROOT="/tmp/llamaste-iso-build.$$"
rm -rf "${ISO_ROOT}"
mkdir -p "${ISO_ROOT}"
trap "rm -rf ${ISO_ROOT}" EXIT

if [ -z "${ISO_ROOT}" ] || [ "${ISO_ROOT}" = "/" ]; then
    echo "ERROR: ISO_ROOT is empty or root — refusing to continue"
    exit 1
fi

echo "[iso] Creating ISO directory structure..."

mkdir -p "${ISO_ROOT}/boot/grub"
mkdir -p "${ISO_ROOT}/opt/llamaste"
mkdir -p "${ISO_ROOT}/install"

# Essential mount points for PID 1 init (ISO9660 is read-only,
# so these must exist before boot — mkdir fails on a mounted iso9660)
mkdir -p "${ISO_ROOT}/proc"
mkdir -p "${ISO_ROOT}/sys"
mkdir -p "${ISO_ROOT}/dev"
mkdir -p "${ISO_ROOT}/tmp"
mkdir -p "${ISO_ROOT}/run"
mkdir -p "${ISO_ROOT}/data"
mkdir -p "${ISO_ROOT}/etc"

# Live pivot working directories for do_live_pivot() in init.cpp.
# ISO9660 is read-only at runtime — mkdir() on the mounted ISO returns EROFS.
# These dirs MUST exist in the ISO image so that pivot can mount tmpfs/squashfs
# on them. Without these, mount("tmpfs","/live/tmpfs",...) fails with ENOENT,
# the pivot silently returns false, and labwc / WiFi are unavailable (they live
# in rootfs.squashfs, not the sparse ISO root).
mkdir -p "${ISO_ROOT}/live/squashfs"   # squashfs loop-device mount point (ro)
mkdir -p "${ISO_ROOT}/live/tmpfs"      # overlay upper + work layer (tmpfs 512M)
mkdir -p "${ISO_ROOT}/live/root"       # overlayfs new root (becomes / after pivot)

# Pivot detection marker — ONLY in the ISO root, NEVER in rootfs.squashfs.
# do_live_pivot() checks for this file to detect "am I booting from the live ISO?"
# After the squashfs pivot + re-exec, this file does not exist in the squashfs
# overlay root → Guard 1 fails → do_live_pivot() returns false → no infinite loop.
#
# NOTE: Do NOT use /boot/bzImage as the guard — Buildroot installs the kernel
# to output/target/boot/ which ends up inside rootfs.squashfs, so that file
# exists in BOTH the ISO root and the squashfs → causes an infinite pivot loop.
touch "${ISO_ROOT}/llamaste-live-iso"

# Minimal /etc for PID 1
echo "llamaste" > "${ISO_ROOT}/etc/hostname"
cat > "${ISO_ROOT}/etc/hosts" << 'HOSTS'
127.0.0.1	localhost
127.0.1.1	llamaste
HOSTS
cat > "${ISO_ROOT}/etc/resolv.conf" << 'DNS'
nameserver 8.8.8.8
nameserver 1.1.1.1
DNS

# --- Step 2: Copy kernel ---
echo "[iso] Copying kernel..."
cp "${IMAGES_DIR}/bzImage" "${ISO_ROOT}/boot/bzImage"

# --- Step 2b: Build and copy initramfs for LABEL= root resolution ---
# The kernel can't resolve root=LABEL=xxx natively — it needs an initramfs.
# The PXE initramfs (pxe-initramfs/) handles both normal and PXE boot paths.
# For ISO live boot, GRUB loads this via 'initrd /boot/initramfs.cpio.gz'.
# For installed boot, grub.cfg uses root=/dev/sdaX (no initramfs needed).
# IMPORTANT: Do NOT embed this in bzImage via CONFIG_INITRAMFS_SOURCE —
# that breaks installed boot (kernel runs /init instead of init=/opt/llamaste/llamaste).
PXE_INITRAMFS="${BUILD_DIR}/pxe-initramfs"
if [ -d "${PXE_INITRAMFS}" ]; then
    echo "[iso] Building initramfs from ${PXE_INITRAMFS}..."
    (cd "${PXE_INITRAMFS}" && find . | cpio -o -H newc 2>/dev/null | gzip -9 > "${ISO_ROOT}/boot/initramfs.cpio.gz")
else
    # No pre-built tree: build a minimal live-boot initramfs here from a static
    # busybox + the canonical live init (initramfs-init.sh). Self-contained so the
    # ISO is always bootable. Do NOT rely on setup-pxe-initramfs-dir.sh — it is
    # PXE-specific (pxe-init.sh) and hard-codes a Windows path. Requires root (mknod).
    echo "[iso] pxe-initramfs not found — building live initramfs from initramfs-init.sh..."
    INIT_SRC="$(dirname "$(readlink -f "$0")")/initramfs-init.sh"
    [ -f "${INIT_SRC}" ] || { echo "ERROR: ${INIT_SRC} not found"; exit 1; }
    BUSYBOX="${BUILD_DIR}/busybox"
    if [ ! -x "${BUSYBOX}" ]; then
        echo "[iso] Fetching static busybox..."
        wget -qO "${BUSYBOX}" "https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox" \
            || { echo "ERROR: failed to download busybox"; exit 1; }
        chmod +x "${BUSYBOX}"
    fi
    IRD="$(mktemp -d)"
    mkdir -p "${IRD}"/{bin,dev,proc,sys,tmp,mnt/root,run}
    cp "${BUSYBOX}" "${IRD}/bin/busybox"; chmod 755 "${IRD}/bin/busybox"
    for cmd in sh mount umount mkdir mknod switch_root sleep cat echo ls losetup ip wget udhcpc awk; do
        ln -sf busybox "${IRD}/bin/${cmd}"
    done
    mknod "${IRD}/dev/console" c 5 1
    mknod "${IRD}/dev/null"    c 1 3
    mknod "${IRD}/dev/loop0"   b 7 0
    cp "${INIT_SRC}" "${IRD}/init"; chmod 755 "${IRD}/init"
    (cd "${IRD}" && find . | cpio -o -H newc 2>/dev/null | gzip -9 > "${ISO_ROOT}/boot/initramfs.cpio.gz")
    rm -rf "${IRD}"
fi
echo "[iso] Initramfs: $(du -h "${ISO_ROOT}/boot/initramfs.cpio.gz" | cut -f1)"

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

# Copy llama-server for live mode inference (if a model is available)
if [ -f "${TARGET_DIR}/opt/llamaste/llama-server" ]; then
    echo "[iso] Copying llama-server..."
    cp "${TARGET_DIR}/opt/llamaste/llama-server" "${ISO_ROOT}/opt/llamaste/llama-server"
    chmod 755 "${ISO_ROOT}/opt/llamaste/llama-server"
fi

# Copy web assets if they exist (for development mode fallback)
if [ -d "${TARGET_DIR}/opt/llamaste/web" ]; then
    cp -r "${TARGET_DIR}/opt/llamaste/web" "${ISO_ROOT}/opt/llamaste/web"
fi

# --- Step 4b: Copy shared libraries + dynamic linker ---
# The llamaste binary is dynamically linked (musl libc, libcurl, liblzma).
# The ISO's iso9660 root filesystem needs the dynamic linker and all shared
# libs, otherwise the kernel will fail with "init failed (error -2)".
echo "[iso] Copying shared libraries..."
mkdir -p "${ISO_ROOT}/lib" "${ISO_ROOT}/usr/lib"

# Dynamic linker (musl)
cp "${TARGET_DIR}/lib/ld-musl-x86_64.so.1" "${ISO_ROOT}/lib/"

# Copy all .so files from target (includes libc, libcurl, liblzma, libssl,
# libcrypto, libnghttp2, libpsl, libicuuc, libicudata, libstdc++, etc.)
for dir in lib usr/lib; do
    if [ -d "${TARGET_DIR}/${dir}" ]; then
        # Copy .so symlinks and real files
        find "${TARGET_DIR}/${dir}" -maxdepth 1 \( -name '*.so' -o -name '*.so.*' \) \
            -exec cp -a {} "${ISO_ROOT}/${dir}/" \;
    fi
done

# Also need CA certificates for model download
if [ -d "${TARGET_DIR}/etc/ssl" ]; then
    mkdir -p "${ISO_ROOT}/etc/ssl"
    cp -r "${TARGET_DIR}/etc/ssl/certs" "${ISO_ROOT}/etc/ssl/"
fi

# --- Step 4c: Copy firmware files ---
# linux-firmware installs to both TARGET_DIR/lib/firmware and BINARIES_DIR.
# In our build, the target dir firmware may be absent (Buildroot quirk) but
# the build dir always has the selected firmware files in br-firmware.tar.
# Prefer target dir; fall back to extracting from br-firmware.tar.
FW_DONE=0
if [ -d "${TARGET_DIR}/lib/firmware" ] && [ "$(ls -A "${TARGET_DIR}/lib/firmware" 2>/dev/null)" ]; then
    echo "[iso] Copying firmware files from target..."
    mkdir -p "${ISO_ROOT}/lib/firmware"
    cp -r "${TARGET_DIR}/lib/firmware/." "${ISO_ROOT}/lib/firmware/"
    FW_COUNT=$(find "${ISO_ROOT}/lib/firmware" -type f | wc -l)
    echo "[iso]   Copied ${FW_COUNT} firmware files"
    FW_DONE=1
fi
if [ "$FW_DONE" = "0" ]; then
    FW_TAR=$(find "${BUILD_DIR}/output/build/linux-firmware-"* -name "br-firmware.tar" 2>/dev/null | head -1)
    if [ -n "$FW_TAR" ]; then
        echo "[iso] Copying firmware files from br-firmware.tar..."
        mkdir -p "${ISO_ROOT}/lib/firmware"
        tar xf "$FW_TAR" -C "${ISO_ROOT}/lib/firmware/"
        FW_COUNT=$(find "${ISO_ROOT}/lib/firmware" -type f | wc -l)
        echo "[iso]   Copied ${FW_COUNT} firmware files"
        FW_DONE=1
    fi
fi
if [ "$FW_DONE" = "0" ]; then
    echo "[iso]   WARNING: No firmware found — WiFi will not work in live mode"
fi

# --- Step 4d: Copy dhcpcd for live-mode networking ---
# The llamaste binary spawns dhcpcd for DHCP. The kernel's ip=dhcp handles
# initial boot but dhcpcd is needed for runtime network management.
if [ -f "${TARGET_DIR}/sbin/dhcpcd" ]; then
    echo "[iso] Copying dhcpcd..."
    mkdir -p "${ISO_ROOT}/sbin" "${ISO_ROOT}/lib/dhcpcd" "${ISO_ROOT}/usr/share/dhcpcd"
    cp "${TARGET_DIR}/sbin/dhcpcd" "${ISO_ROOT}/sbin/dhcpcd"
    [ -d "${TARGET_DIR}/lib/dhcpcd" ]       && cp -r "${TARGET_DIR}/lib/dhcpcd/."       "${ISO_ROOT}/lib/dhcpcd/"
    [ -d "${TARGET_DIR}/usr/share/dhcpcd" ] && cp -r "${TARGET_DIR}/usr/share/dhcpcd/." "${ISO_ROOT}/usr/share/dhcpcd/"
fi

LIB_COUNT=$(find "${ISO_ROOT}/lib" "${ISO_ROOT}/usr/lib" -name '*.so*' | wc -l)
echo "[iso]   Copied ${LIB_COUNT} shared library files"

# --- Step 5: Compress and copy disk image for installer ---
echo "[iso] Preparing disk image for installer..."
IMG_SIZE=$(stat -c %s "${IMAGES_DIR}/llamaste.img" 2>/dev/null || stat -f %z "${IMAGES_DIR}/llamaste.img")
echo "[iso]   Source image: $(echo "scale=1; ${IMG_SIZE} / 1048576" | bc) MB"

# Include XZ compressed image (preferred — smaller ISO, needs liblzma in binary)
if [ -f "${IMAGES_DIR}/llamaste.img.xz" ]; then
    echo "[iso]   Using existing compressed image"
    cp "${IMAGES_DIR}/llamaste.img.xz" "${ISO_ROOT}/install/llamaste.img.xz"
else
    echo "[iso]   Compressing with xz -3 (this may take a minute)..."
    xz -3 -T0 -c "${IMAGES_DIR}/llamaste.img" > "${ISO_ROOT}/install/llamaste.img.xz"
fi

XZ_SIZE=$(stat -c %s "${ISO_ROOT}/install/llamaste.img.xz" 2>/dev/null || stat -f %z "${ISO_ROOT}/install/llamaste.img.xz")
echo "[iso]   Compressed image: $(echo "scale=1; ${XZ_SIZE} / 1048576" | bc) MB"

# Also include raw image as fallback (works without liblzma, larger ISO)
echo "[iso]   Copying raw image as fallback..."
cp "${IMAGES_DIR}/llamaste.img" "${ISO_ROOT}/install/llamaste.img"
echo "[iso]   Raw image: $(echo "scale=1; ${IMG_SIZE} / 1048576" | bc) MB"

# --- Step 6: Copy squashfs for reference ---
echo "[iso] Copying rootfs.squashfs..."
cp "${IMAGES_DIR}/rootfs.squashfs" "${ISO_ROOT}/install/rootfs.squashfs"

# --- Step 7: Add Ventoy compatibility marker ---
touch "${ISO_ROOT}/ventoy.dat"

# --- Step 8: Build the ISO ---
echo "[iso] Building hybrid ISO with grub-mkrescue..."
ISO_OUTPUT="${IMAGES_DIR}/llamaste.iso"

# MUST use system grub-mkrescue, NOT Buildroot's host version!
# Buildroot's grub-mkrescue at output/host/bin/ lacks i386-pc and x86_64-efi
# modules, producing a data-only ISO with no boot capability.
/usr/bin/grub-mkrescue -o "${ISO_OUTPUT}" "${ISO_ROOT}" \
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
