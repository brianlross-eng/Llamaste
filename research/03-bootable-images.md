# Bootable USB/ISO Image Creation

## BIOS vs UEFI Boot

### Legacy BIOS
- Reads first 512 bytes (MBR) from disk
- 440 bytes bootloader code + 64-byte partition table
- Uses INT 13h for disk I/O
- No filesystem understanding
- 2 TB disk limit (MBR)

### UEFI
- Reads GPT partition table
- Looks for EFI System Partition (ESP): FAT32, type GUID `C12A7328-F81F-11D2-BA4B-00A0C93EC93B`
- Loads EFI application from `\EFI\BOOT\BOOTX64.EFI`
- Native FAT32 understanding
- For removable media: uses fallback path, not NVRAM entries

### Supporting Both (Hybrid MBR/GPT)

1. GPT partition table with protective MBR containing BIOS boot code
2. BIOS Boot Partition (~1 MB, GUID `21686148-...`, no filesystem) for GRUB core.img
3. ESP (FAT32, 128-550 MB) with EFI bootloader
4. Remaining space for root and data partitions

## Bootloader: GRUB2 (Recommended)

- Supports both BIOS and UEFI
- Reads ext4, squashfs, FAT, ISO 9660 natively
- Powerful scripting language
- `grub-mkrescue` creates hybrid BIOS+UEFI ISO in one command
- Actively maintained

Syslinux/ISOLINUX are BIOS-only and largely dormant since ~2019. Not viable for modern hardware.

## Recommended Partition Layout (GPT)

```
Part  Name        Type                    FS         Size       Mount      RW/RO
1     bios-boot   21686148-6449-...       raw        1 MB       -          -
2     ESP         C12A7328-F81F-...       FAT32      128 MB     /boot/efi  RO
3     system      4F68BCE3-E8CD-...       squashfs   ~256 MB    /          RO
4     data        3B8F8425-20E0-...       ext4       Remainder  /data      RW
```

Use filesystem labels (`LABEL=SYSTEM`, `LABEL=DATA`) for reliable partition discovery.

## Hybrid ISO Creation

### Method 1: xorriso (Recommended)

```bash
xorriso -as mkisofs \
    -o output.iso \
    -isohybrid-mbr /usr/lib/ISOLINUX/isohdpfx.bin \
    -c isolinux/boot.cat \
    -b isolinux/isolinux.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    -eltorito-alt-boot \
    -e boot/grub/efi.img \
    -no-emul-boot \
    -isohybrid-gpt-basdat \
    -V "LLAMASTE" \
    ./iso-root/
```

### Method 2: grub-mkrescue (Simplest)

```bash
grub-mkrescue -o output.iso ./iso-root/
```

## Filesystem Choices

### squashfs for Read-Only Root
- Compressed (zstd or xz), 2 GB root -> 400-700 MB
- Read-only by design, immune to power-loss corruption
- Fast random reads via block-level decompression
- Memory efficient

```bash
mksquashfs ./rootfs rootfs.squashfs -comp zstd -Xcompression-level 15 -b 256K -noappend -all-root
```

### ext4 for Writable Data Partition
- Journaled, handles power-loss gracefully
- Mount with `noatime` to reduce write wear
- Set reserved blocks to 0%: `tune2fs -m 0`

### overlayfs for Persistence

```bash
mount -t squashfs /dev/disk/by-label/SYSTEM /lower -o ro
mount -t ext4 /dev/disk/by-label/DATA /data
mkdir -p /data/overlay/upper /data/overlay/work
mount -t overlay overlay -o \
    lowerdir=/lower,upperdir=/data/overlay/upper,workdir=/data/overlay/work \
    /merged
pivot_root /merged /merged/mnt
```

Factory reset = wipe overlay partition.

## UEFI Secure Boot

### Standard Chain
```
Firmware (Microsoft keys) -> shim (MS-signed) -> GRUB2 (distro-signed) -> kernel (signed)
```

### Options for Llamaste
- **Option A (Recommended):** Use Ubuntu/Fedora's signed shim
- **Option B:** Get own shim signed by Microsoft
- **Option C:** Self-sign, require users to disable Secure Boot or enroll MOK

## First-Boot Setup

```bash
STAMP="/data/.initialized"
if [ ! -f "$STAMP" ]; then
    # Generate SSH host keys
    # Resize data partition to fill disk
    # Initialize default config
    # Set up network (DHCP default)
    /opt/llamaste/first-boot.sh
    date -Iseconds > "$STAMP"
fi
```

## GRUB Configuration

```bash
set timeout=3
set default=0

menuentry "Llamaste" {
    search --no-floppy --label --set=root SYSTEM
    linux /boot/vmlinuz root=LABEL=SYSTEM rootfstype=squashfs ro quiet
    initrd /boot/initramfs.img
}

menuentry "Llamaste (Recovery)" {
    search --no-floppy --label --set=root SYSTEM
    linux /boot/vmlinuz root=LABEL=SYSTEM rootfstype=squashfs ro single
    initrd /boot/initramfs.img
}
```

## Distribution Formats

1. **Raw disk image** (.img.xz) -- Primary. Created with genimage. Exact partition control.
2. **Hybrid ISO** (.iso) -- Secondary. Created with grub-mkrescue. Works with Rufus, Etcher, Ventoy.
3. SHA256SUMS for verification.

## Image Tools

| Tool | Purpose |
|------|---------|
| xorriso | Hybrid ISO creation |
| genimage | Partitioned disk images |
| mtools | FAT image manipulation without mount |
| mksquashfs | squashfs image creation |
| grub-mkrescue | Simplest hybrid ISO |
| grub-mkimage | Custom GRUB images |

## Testing Matrix

Test on: Legacy BIOS, UEFI without Secure Boot, UEFI with Secure Boot, QEMU/KVM (BIOS + OVMF), VirtualBox, VMware, 2-3 different physical machines.
