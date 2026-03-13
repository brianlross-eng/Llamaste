#!/bin/sh
export PATH=/bin

mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs devtmpfs /dev

# Parse kernel cmdline for root= parameter
CMDLINE=$(cat /proc/cmdline)
ROOT=""
ROOTFSTYPE=""
INIT="/opt/llamaste/llamaste"
MODE=""
for param in $CMDLINE; do
    case "$param" in
        root=*)    ROOT="${param#root=}" ;;
        rootfstype=*) ROOTFSTYPE="${param#rootfstype=}" ;;
        init=*)    INIT="${param#init=}" ;;
        llamaste.mode=*) MODE="${param#llamaste.mode=}" ;;
    esac
done

# =========================================================================
# PATH A: Normal boot (USB/installed) — root= is set by GRUB
# =========================================================================
if [ -n "$ROOT" ]; then
    echo "[initramfs] Normal boot: root=$ROOT rootfstype=$ROOTFSTYPE"

    mkdir -p /mnt/root

    # Resolve LABEL= and UUID= references
    case "$ROOT" in
        LABEL=*)
            LABEL="${ROOT#LABEL=}"
            echo "[initramfs] Searching for filesystem label: $LABEL"
            sleep 2  # wait for devices
            # Find device by label using /sys
            for dev in /dev/sd* /dev/nvme* /dev/sr*; do
                [ -b "$dev" ] || continue
                # Try mounting and checking
                if mount -t "${ROOTFSTYPE:-auto}" -o ro "$dev" /mnt/root 2>/dev/null; then
                    umount /mnt/root
                    # Found a mountable device — use it
                    # (proper label check would need blkid, just try all)
                    ROOT="$dev"
                    break
                fi
            done
            if [ "$ROOT" = "LABEL=$LABEL" ]; then
                echo "[initramfs] WARNING: Could not find label $LABEL, trying all devices..."
                # On USB, try common devices
                for dev in /dev/sdb /dev/sda /dev/sr0; do
                    [ -b "$dev" ] || continue
                    ROOT="$dev"
                    break
                done
            fi
            ;;
    esac

    echo "[initramfs] Mounting $ROOT..."
    MOPT=""
    [ -n "$ROOTFSTYPE" ] && MOPT="-t $ROOTFSTYPE"
    mount $MOPT -o ro "$ROOT" /mnt/root

    if [ $? -ne 0 ]; then
        echo "[initramfs] ERROR: Failed to mount $ROOT"
        echo "[initramfs] Dropping to shell..."
        exec /bin/sh
    fi

    echo "[initramfs] Root mounted. Switching to $INIT..."
    umount /proc 2>/dev/null
    umount /sys 2>/dev/null
    exec switch_root /mnt/root "$INIT"
fi

# =========================================================================
# PATH B: PXE boot — no root= parameter, download squashfs via HTTP
# =========================================================================
echo "=== Llamaste PXE Boot (initramfs) ==="

echo "[pxe] Configuring network..."
echo "[pxe] Waiting for USB ethernet (up to 15s)..."

ETH=""
TRIES=0
while [ -z "$ETH" ] && [ "$TRIES" -lt 15 ]; do
    for name in $(ls /sys/class/net/ 2>/dev/null); do
        [ "$name" = "lo" ] && continue
        [ -d "/sys/class/net/$name/phy80211" ] && continue
        [ ! -d "/sys/class/net/$name/device" ] && continue
        ETH="$name"
        break
    done
    if [ -z "$ETH" ]; then
        TRIES=$((TRIES + 1))
        echo "[pxe] No ethernet yet... ($TRIES/15)"
        sleep 1
    fi
done

if [ -z "$ETH" ]; then
    echo "[pxe] ERROR: No physical wired ethernet found after 15s!"
    echo "[pxe] Interfaces:"
    ls -la /sys/class/net/
    for name in $(ls /sys/class/net/); do
        echo "  $name: device=$(ls /sys/class/net/$name/device 2>/dev/null && echo YES || echo NO)"
    done
    exec /bin/sh
fi

echo "[pxe] Using interface: $ETH"
ip link set lo up
ip link set "$ETH" up
sleep 2

# Static IP — we control the PXE network (10.0.50.x)
# (udhcpc needs /usr/share/udhcpc/default.script to apply leases,
#  which doesn't exist in our minimal initramfs)
echo "[pxe] Setting static IP 10.0.50.50/24..."
ip addr add 10.0.50.50/24 dev "$ETH"
ip route add default via 10.0.50.1
ip addr show "$ETH"

echo "[pxe] Downloading rootfs.squashfs..."
wget -q http://10.0.50.1/rootfs.squashfs -O /tmp/rootfs.squashfs
if [ $? -ne 0 ]; then
    echo "[pxe] Retry..."
    sleep 2
    wget http://10.0.50.1/rootfs.squashfs -O /tmp/rootfs.squashfs
fi

if [ ! -s /tmp/rootfs.squashfs ]; then
    echo "[pxe] ERROR: Download failed!"
    exec /bin/sh
fi

echo "[pxe] Downloaded. Mounting squashfs..."
mkdir -p /mnt/root
losetup /dev/loop0 /tmp/rootfs.squashfs
mount -t squashfs /dev/loop0 /mnt/root

if [ $? -ne 0 ]; then
    echo "[pxe] ERROR: Mount failed!"
    exec /bin/sh
fi

echo "[pxe] Switching root..."
umount /proc 2>/dev/null
umount /sys 2>/dev/null
exec switch_root /mnt/root /opt/llamaste/llamaste
