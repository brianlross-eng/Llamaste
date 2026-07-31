#!/bin/bash
set -e
DIR=/root/llamaste-build/pxe-initramfs
rm -rf "$DIR"
mkdir -p "$DIR"/{bin,dev,proc,sys,tmp,mnt/root,run}

cp /tmp/busybox "$DIR/bin/busybox"
chmod 755 "$DIR/bin/busybox"

cd "$DIR/bin"
for cmd in sh mount umount mkdir mknod wget ip losetup switch_root sleep cat echo ls udhcpc awk; do
    ln -s busybox "$cmd"
done

cd "$DIR"
mknod dev/console c 5 1
mknod dev/null c 1 3
mknod dev/loop0 b 7 0

# PXE boot uses pxe-init.sh (HTTP squashfs download). For a live ISO, build-iso.sh
# builds its own initramfs from initramfs-init.sh instead of using this dir.
cp "$(dirname "$(readlink -f "$0")")/pxe-init.sh" "$DIR/init"
chmod 755 "$DIR/init"

echo "Initramfs dir ready at $DIR"
ls -la "$DIR/init" "$DIR/bin/busybox"
find "$DIR" -type f | wc -l
echo "files total"
