#!/bin/bash
# qemu-run.sh -- Run Llamaste in QEMU interactively
#
# Usage: ./scripts/qemu-run.sh [images-dir]
#
# This script boots the Llamaste image in QEMU with:
#   - Direct kernel boot (fastest, bypasses GRUB)
#   - virtio networking with port forwarding
#   - Serial console on stdout (nographic mode)
#
# After boot, access the web UI at http://localhost:8080
# Press Ctrl+A, X to exit QEMU.
#
# Environment variables:
#   PORT     - Host port to forward to guest port 80 (default: 8080)
#   RAM      - RAM allocated to VM (default: 4G)
#   CPUS     - Number of CPU cores (default: 4)
#   MODE     - Boot mode: server or desktop (default: server)
#   GRUB     - Set to 1 to boot via disk (GRUB) instead of direct kernel boot

set -e

IMAGES_DIR="${1:-$HOME/llamaste-build/output/images}"
IMAGE="${IMAGES_DIR}/llamaste.img"
KERNEL="${IMAGES_DIR}/bzImage"
PORT="${PORT:-8080}"
RAM="${RAM:-4G}"
CPUS="${CPUS:-4}"
MODE="${MODE:-server}"
GRUB="${GRUB:-0}"

# --- Validate ---
if ! command -v qemu-system-x86_64 &>/dev/null; then
    echo "ERROR: qemu-system-x86_64 not found. Install QEMU first."
    echo ""
    echo "  Ubuntu/Debian: sudo apt install qemu-system-x86"
    echo "  Fedora:        sudo dnf install qemu-system-x86-core"
    echo "  Arch:          sudo pacman -S qemu-system-x86"
    exit 1
fi

if [ ! -f "${IMAGE}" ]; then
    echo "ERROR: Image not found: ${IMAGE}"
    echo ""
    echo "Build the image first with:"
    echo "  ./scripts/buildroot-build.sh"
    echo ""
    echo "Or specify the images directory:"
    echo "  ./scripts/qemu-run.sh /path/to/images"
    exit 1
fi

echo "Llamaste QEMU Runner"
echo "===================="
echo "Image:  ${IMAGE}"
if [ "${GRUB}" = "1" ]; then
    echo "Boot:   via GRUB (disk boot)"
else
    echo "Kernel: ${KERNEL}"
    echo "Boot:   direct kernel (fast)"
fi
echo "RAM:    ${RAM}"
echo "CPUs:   ${CPUS}"
echo "Mode:   ${MODE}"
echo "Port:   http://localhost:${PORT}"
echo ""
echo "Press Ctrl+A, X to exit QEMU"
echo ""

if [ "${GRUB}" = "1" ]; then
    # Boot from disk (uses GRUB installed on the image)
    # This tests the full boot chain including GRUB menu
    qemu-system-x86_64 \
        -m "${RAM}" -smp "${CPUS}" \
        -drive file="${IMAGE}",format=raw,if=virtio \
        -netdev user,id=net0,hostfwd=tcp::${PORT}-:80 \
        -device virtio-net-pci,netdev=net0 \
        -nographic
else
    # Direct kernel boot (fastest, bypasses GRUB)
    if [ ! -f "${KERNEL}" ]; then
        echo "ERROR: Kernel not found: ${KERNEL}"
        echo "Use GRUB=1 to boot from disk instead."
        exit 1
    fi

    qemu-system-x86_64 \
        -m "${RAM}" -smp "${CPUS}" \
        -kernel "${KERNEL}" \
        -append "root=/dev/vda3 rootfstype=squashfs ro console=ttyS0 init=/opt/llamaste/llamaste llamaste.mode=${MODE} ip=dhcp" \
        -drive file="${IMAGE}",format=raw,if=virtio \
        -netdev user,id=net0,hostfwd=tcp::${PORT}-:80 \
        -device virtio-net-pci,netdev=net0 \
        -nographic
fi
