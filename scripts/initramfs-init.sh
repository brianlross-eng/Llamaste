#!/bin/sh
export PATH=/bin

mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs devtmpfs /dev

# Parse kernel cmdline
CMDLINE=$(cat /proc/cmdline)
ROOT=""
ROOTFSTYPE=""
INIT="/opt/llamaste/llamaste"
MODE=""
SLOT="A"
for param in $CMDLINE; do
    case "$param" in
        root=*)    ROOT="${param#root=}" ;;
        rootfstype=*) ROOTFSTYPE="${param#rootfstype=}" ;;
        init=*)    INIT="${param#init=}" ;;
        llamaste.mode=*) MODE="${param#llamaste.mode=}" ;;
        llamaste.slot=*) SLOT="${param#llamaste.slot=}" ;;
    esac
done

# Determine target partition number from slot
if [ "$SLOT" = "B" ]; then
    PARTNUM=4
else
    PARTNUM=3
fi

mkdir -p /mnt/root

# =========================================================================
# PATH A: Live boot (root=LABEL=...) -- scan by label
# =========================================================================
# Note: use case/esac, NOT grep — busybox initramfs may not have grep
IS_LABEL=""
case "$ROOT" in LABEL=*) IS_LABEL=yes ;; esac
if [ -n "$IS_LABEL" ]; then
    LABEL="${ROOT#LABEL=}"
    echo "[initramfs] Live boot: resolving LABEL=$LABEL ..."

    FOUND=""
    for attempt in 1 2 3 4 5; do
        echo "[initramfs] Scan attempt $attempt/5 ..."
        for dev in /dev/sr0 /dev/sdb /dev/sdb1 /dev/sda /dev/sda1 /dev/nvme0n1 /dev/nvme0n1p1; do
            [ -b "$dev" ] || continue
            if mount -t "${ROOTFSTYPE:-auto}" -o ro "$dev" /mnt/root 2>/dev/null; then
                if [ -f /mnt/root/llamaste-live-iso ]; then
                    echo "[initramfs] FOUND live media: $dev"
                    FOUND="$dev"
                    umount /mnt/root
                    break
                fi
                umount /mnt/root
            fi
        done
        [ -n "$FOUND" ] && break
        echo "[initramfs] Not found yet, waiting 2s..."
        sleep 2
    done

    if [ -z "$FOUND" ]; then
        echo "[initramfs] ERROR: Could not find Llamaste live media"
        exec /bin/sh
    fi

    mount -t "${ROOTFSTYPE:-auto}" -o ro "$FOUND" /mnt/root
    if [ $? -ne 0 ]; then
        echo "[initramfs] ERROR: Failed to mount $FOUND"
        exec /bin/sh
    fi

    echo "[initramfs] Live media mounted. Switching root..."
    umount /proc 2>/dev/null
    umount /sys 2>/dev/null
    exec switch_root /mnt/root "$INIT"
    echo "[initramfs] ERROR: switch_root failed!"
    exec /bin/sh
fi

# =========================================================================
# PATH B: Installed boot -- find squashfs root partition
# Try specified root= device first, then scan all partitions.
# This handles SATA (/dev/sda3), NVMe (/dev/nvme0n1p3), virtio, etc.
# =========================================================================
echo "[initramfs] Installed boot: slot=$SLOT partnum=$PARTNUM root=$ROOT"

# Build list of candidate devices: specified root first, then common patterns
CANDIDATES="$ROOT"
for disk in /dev/nvme0n1 /dev/nvme1n1 /dev/sda /dev/sdb /dev/vda; do
    if [ -b "$disk" ]; then
        case "$disk" in
            /dev/nvme*|/dev/mmcblk*) CANDIDATES="$CANDIDATES ${disk}p${PARTNUM}" ;;
            *)                       CANDIDATES="$CANDIDATES ${disk}${PARTNUM}" ;;
        esac
    fi
done

echo "[initramfs] Candidates: $CANDIDATES"

# Wait up to 5s for devices to appear (NVMe probe time)
FOUND=""
for attempt in 1 2 3 4 5; do
    for dev in $CANDIDATES; do
        [ -b "$dev" ] || continue
        echo "[initramfs] Trying $dev ..."
        if mount -t squashfs -o ro "$dev" /mnt/root 2>/dev/null; then
            if [ -f "/mnt/root$INIT" ]; then
                echo "[initramfs] FOUND root: $dev"
                FOUND="$dev"
                break 2
            fi
            echo "[initramfs] $dev is squashfs but no $INIT -- wrong partition"
            umount /mnt/root
        fi
    done
    echo "[initramfs] Not found yet, waiting 1s... ($attempt/5)"
    sleep 1
done

if [ -z "$FOUND" ]; then
    echo "[initramfs] ERROR: No root filesystem found!"
    echo "[initramfs] Block devices:"
    ls /dev/sd* /dev/nvme* /dev/vd* 2>/dev/null
    echo "[initramfs] Dropping to shell..."
    mount -t proc proc /proc
    mount -t sysfs sys /sys
    exec /bin/sh
fi

echo "[initramfs] Root mounted at $FOUND. Switching to $INIT ..."
umount /proc 2>/dev/null
umount /sys 2>/dev/null
exec switch_root /mnt/root "$INIT"

echo "[initramfs] ERROR: switch_root failed!"
mount -t proc proc /proc
mount -t sysfs sys /sys
exec /bin/sh
