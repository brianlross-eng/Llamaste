#!/bin/bash
# Llamaste post-build script
# Called by Buildroot after building the target filesystem, before image creation.
#
# This script prepares the target rootfs (which becomes the squashfs SYS-A partition):
#   - Creates essential directories for PID 1 operation
#   - Installs GRUB config into the target's boot directory
#   - Ensures the llamaste binary is in the expected location
#
# Environment variables (set by Buildroot):
#   TARGET_DIR  - output/target/ (the target root filesystem)

set -e

BOARD_DIR=$(dirname "$0")
TARGET_DIR="$1"

echo "[post-build] Preparing Llamaste target filesystem..."

# --- Essential mount point directories ---
# PID 1 (llamaste binary) will mount these at boot
mkdir -p "${TARGET_DIR}/dev"
mkdir -p "${TARGET_DIR}/proc"
mkdir -p "${TARGET_DIR}/sys"
mkdir -p "${TARGET_DIR}/tmp"
mkdir -p "${TARGET_DIR}/run"

# --- Data partition mount point ---
# The DATA partition (ext4, partition 5) will be mounted here
mkdir -p "${TARGET_DIR}/data"

# --- GRUB configuration ---
# Install grub.cfg into the target's /boot/grub/ directory
# This is also copied to the EFI partition by post_image.sh, but
# having it in the rootfs provides a fallback and makes debugging easier
mkdir -p "${TARGET_DIR}/boot/grub"
if [ -f "${BOARD_DIR}/grub.cfg" ]; then
    cp "${BOARD_DIR}/grub.cfg" "${TARGET_DIR}/boot/grub/grub.cfg"
    echo "[post-build] Installed grub.cfg"
else
    echo "[post-build] WARNING: grub.cfg not found at ${BOARD_DIR}/grub.cfg"
fi

# --- Verify llamaste binary location ---
# The llamaste package (llamaste.mk) installs to /opt/llamaste/llamaste
# This is the init= target specified in grub.cfg
if [ -f "${TARGET_DIR}/opt/llamaste/llamaste" ]; then
    echo "[post-build] Verified: llamaste binary at /opt/llamaste/llamaste"
    # Ensure it's executable
    chmod 755 "${TARGET_DIR}/opt/llamaste/llamaste"
else
    echo "[post-build] NOTE: llamaste binary not yet at /opt/llamaste/llamaste"
    echo "             (will be placed by the llamaste Buildroot package)"
    # Create the directory so the package has a target
    mkdir -p "${TARGET_DIR}/opt/llamaste"
fi

# --- Minimal /etc for PID 1 operation ---
# We don't use a traditional init system, but some libraries expect these
mkdir -p "${TARGET_DIR}/etc"

# Hostname
echo "llamaste" > "${TARGET_DIR}/etc/hostname"

# Minimal /etc/hosts for loopback
if [ ! -f "${TARGET_DIR}/etc/hosts" ]; then
    cat > "${TARGET_DIR}/etc/hosts" << 'EOF'
127.0.0.1	localhost
127.0.1.1	llamaste
::1		localhost ip6-localhost ip6-loopback
EOF
fi

# DNS resolver (DHCP may override this at runtime)
if [ ! -f "${TARGET_DIR}/etc/resolv.conf" ]; then
    cat > "${TARGET_DIR}/etc/resolv.conf" << 'EOF'
nameserver 8.8.8.8
nameserver 1.1.1.1
EOF
fi

# Timezone (UTC by default)
if [ ! -f "${TARGET_DIR}/etc/localtime" ]; then
    echo "UTC" > "${TARGET_DIR}/etc/timezone"
fi

# --- C++ runtime shared library ---
# libcurl → libpsl → libicuuc → libstdc++  (ICU is C++ and needs the runtime)
# Our binary links libstdc++ statically, but ICU in the rootfs needs the .so
TOOLCHAIN_SYSROOT="${TARGET_DIR}/../host/x86_64-buildroot-linux-musl/lib64"
if [ -f "${TOOLCHAIN_SYSROOT}/libstdc++.so.6.0.30" ]; then
    cp "${TOOLCHAIN_SYSROOT}/libstdc++.so.6.0.30" "${TARGET_DIR}/usr/lib/"
    ln -sf libstdc++.so.6.0.30 "${TARGET_DIR}/usr/lib/libstdc++.so.6"
    ln -sf libstdc++.so.6 "${TARGET_DIR}/usr/lib/libstdc++.so"
    echo "[post-build] Installed libstdc++.so.6 for ICU/libcurl"
fi
if [ -f "${TOOLCHAIN_SYSROOT}/libgcc_s.so.1" ]; then
    cp "${TOOLCHAIN_SYSROOT}/libgcc_s.so.1" "${TARGET_DIR}/usr/lib/"
    ln -sf libgcc_s.so.1 "${TARGET_DIR}/usr/lib/libgcc_s.so"
    echo "[post-build] Installed libgcc_s.so.1"
fi

# --- TTS voice model (sherpa-onnx Piper VITS) ---
# Bundle the default voice model for out-of-box TTS experience.
# Model is downloaded once to the build machine, then baked into squashfs.
# Users can download higher-quality models at runtime to /data/models/tts/.
TTS_MODEL_DIR="/root/llamaste-build/tts-models/vits-piper-en_US-amy-low"
if [ -d "$TTS_MODEL_DIR" ]; then
    mkdir -p "${TARGET_DIR}/data/models/tts"
    cp "$TTS_MODEL_DIR/en_US-amy-low.onnx" "${TARGET_DIR}/data/models/tts/"
    cp "$TTS_MODEL_DIR/tokens.txt" "${TARGET_DIR}/data/models/tts/"
    # espeak-ng-data for phoneme conversion
    if [ -d "$TTS_MODEL_DIR/espeak-ng-data" ]; then
        cp -r "$TTS_MODEL_DIR/espeak-ng-data" "${TARGET_DIR}/data/models/tts/"
    fi
    echo "[post-build] Installed TTS model: en_US-amy-low (Piper VITS)"
else
    echo "[post-build] NOTE: TTS model not found at $TTS_MODEL_DIR"
    echo "             Download with: wget https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/vits-piper-en_US-amy-low.tar.bz2"
fi

echo "[post-build] Target filesystem preparation complete"
