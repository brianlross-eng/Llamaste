#!/bin/sh
set -eu

BOARD_DIR=$(dirname "$0")
TARGET_DIR="$1"

# Create runtime directories
mkdir -p "${TARGET_DIR}/data"
mkdir -p "${TARGET_DIR}/tmp"
mkdir -p "${TARGET_DIR}/run"
mkdir -p "${TARGET_DIR}/dev"
mkdir -p "${TARGET_DIR}/proc"
mkdir -p "${TARGET_DIR}/sys"

# Copy GRUB config into boot
mkdir -p "${TARGET_DIR}/boot/grub"
cp "${BOARD_DIR}/grub.cfg" "${TARGET_DIR}/boot/grub/grub.cfg"
