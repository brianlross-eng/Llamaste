# Bootable Image Construction: Deep Research

**Date:** 2026-02-27
**Topics:** initramfs, live USB architecture, hybrid ISO creation, QEMU testing, model delivery

---

## 1. initramfs: Needed or Not for PID 1 Pattern

### The Core Question

Can the kernel boot directly to `/opt/llamaste` via `init=/opt/llamaste` without an initramfs?

**Answer: Yes, but with strict kernel build requirements.**

### How the Kernel Boot Handoff Works

All Linux 2.6+ kernels contain a built-in minimal cpio archive (the internal initramfs). After extracting it to rootfs, the kernel checks for `/init`. If found, it executes it as PID 1. If not found, the kernel falls back to legacy boot code that mounts a real block device as root, then looks for:
1. `/sbin/init`
2. `/etc/init`
3. `/bin/init`
4. `/bin/sh`
5. Kernel panic if all fail

When you pass `init=/opt/llamaste`, the kernel uses that path instead of the default `/sbin/init` search chain. This works **whether or not an initramfs is present**, as long as the path exists on the root filesystem once it is mounted.

### When initramfs Is NOT Required

Initramfs can be omitted when ALL of the following are met:

1. **Storage controller driver is built into the kernel** (not a module)
   - For SATA/AHCI: `CONFIG_SATA_AHCI=y`
   - For virtio (QEMU): `CONFIG_VIRTIO_BLK=y`
   - For NVMe: `CONFIG_BLK_DEV_NVME=y`

2. **Root filesystem driver is built into the kernel**
   - For squashfs: `CONFIG_SQUASHFS=y`
   - For ext4: `CONFIG_EXT4_FS=y`

3. **Block device layer is built in**
   - `CONFIG_BLK_DEV_SD=y` (SCSI disk)
   - `CONFIG_SCSI=y`

4. **No LUKS/LVM/RAID on root** (these always require initramfs)

5. **Root device is specified with `/dev/sdX` path** (not UUID, since udev is not running pre-mount)

6. **`CONFIG_BLK_DEV_INITRD` can be disabled** (or left enabled but just not used)

### When initramfs IS Required

| Condition | Why initramfs is needed |
|---|---|
| LUKS encryption on root | Kernel cannot inherit crypto state from GRUB; must prompt for passphrase in early userspace |
| LVM on root | `lvm2` tooling must run before root is mounted |
| Software RAID (mdadm) | Array must be assembled before root is mounted |
| Network boot (NFS root) | Network stack must be initialized before root is mounted |
| Storage driver is a module | Module must be loaded before block device appears |
| Filesystem driver is a module | Module must be loaded before filesystem can be mounted |
| Root is squashfs on a loopback device | Loop device setup requires userspace tooling |

### Squashfs as Root: Can the Kernel Mount It Directly?

**Yes**, the kernel can mount squashfs directly as a block partition without initramfs, provided:

```
CONFIG_SQUASHFS=y          # squashfs support built in
CONFIG_SQUASHFS_ZSTD=y     # if using zstd compression
CONFIG_SQUASHFS_XZ=y       # if using xz compression
CONFIG_SQUASHFS_LZO=y      # if using lzo compression
```

GRUB configuration:
```
linux /boot/vmlinuz \
    root=/dev/sda3 \
    rootfstype=squashfs \
    ro \
    init=/opt/llamaste \
    console=ttyS0
```

Or using a partition label (more portable):
```
linux /boot/vmlinuz \
    root=LABEL=SYSTEM \
    rootfstype=squashfs \
    ro \
    init=/opt/llamaste \
    console=ttyS0
```

**Important limitation:** Labels (`LABEL=`) and UUIDs (`UUID=`) require udev to resolve, which is not available before root is mounted. Use `/dev/sdX` notation directly, or use `root=PARTUUID=` which GRUB can resolve itself, or use `root=LABEL=` which the kernel can resolve if compiled with `CONFIG_SCSI_WAIT_SCAN` and sufficient `rootwait` delay.

### Practical Recommendation for Llamaste

**Use a minimal initramfs.** Even though it is technically possible to boot without one, the initramfs provides:

1. **Portability** — Works on any storage controller without recompiling kernel
2. **Label/UUID resolution** — Can use `LABEL=SYSTEM` reliably
3. **Hardware detection** — Can load storage modules based on actual hardware
4. **Faster iteration** — Can fix boot issues in initramfs without rebuilding squashfs

A minimal Buildroot-generated initramfs is under 2 MB and adds ~50ms to boot time. This is a worthwhile trade for the portability gain.

**If you insist on no initramfs for speed:** Build the kernel with all storage controllers and squashfs built in, use `/dev/sdX` or `PARTUUID=` in GRUB, and accept that the image only works on AHCI SATA systems.

### The `init=` Parameter: Binary Requirements

The custom init binary (`/opt/llamaste`) must:
1. Be **statically linked** (no shared library dependencies) — dynamic linker is not available at PID 1 time
2. Be **executable** (chmod +x)
3. **Never exit** — if PID 1 exits, the kernel panics with "Attempted to kill init!"
4. Handle **SIGCHLD** to reap zombie processes, or use a supervisor/fork pattern

The blinry.org experiment confirmed: a statically linked Rust binary with `RUSTFLAGS='-C target-feature=+crt-static' cargo build --release` works as a direct PID 1. The same pattern applies to a statically linked C++ binary.

### Kernel Config Summary for No-initramfs Boot

```
# Disable initramfs (optional, saves a tiny amount of build space)
# CONFIG_BLK_DEV_INITRD is not set

# Storage (build ALL that might be used into kernel, not as modules)
CONFIG_SATA_AHCI=y          # most SATA controllers
CONFIG_ATA=y
CONFIG_SCSI=y
CONFIG_BLK_DEV_SD=y
CONFIG_VIRTIO_BLK=y         # for QEMU/KVM testing
CONFIG_BLK_DEV_NVME=y       # for NVMe SSDs
CONFIG_MMC=y                # for SD cards (embedded)

# Filesystem
CONFIG_SQUASHFS=y
CONFIG_SQUASHFS_ZSTD=y
CONFIG_EXT4_FS=y            # for data partition

# Required for any working system
CONFIG_TTY=y
CONFIG_UNIX98_PTYS=y
CONFIG_PRINTK=y
CONFIG_ELF_CORE=y
```

---

## 2. Live USB vs Install-to-Disk Architecture

### How Live Distros Work

A live system boots from removable media and runs entirely in RAM. The architecture:

1. **Boot media (USB/CD)** contains: kernel (`vmlinuz`), initramfs (`initrd`), and compressed squashfs (`filesystem.squashfs` or `airootfs.sfs`)

2. **initramfs** (via Casper or dracut) performs:
   - Detect and mount the boot media
   - Find the squashfs image
   - Attach squashfs to a loop device and mount it read-only
   - Create a tmpfs (RAM) overlay
   - Set up overlayfs: `lowerdir=squashfs, upperdir=tmpfs`
   - `switch_root` or `pivot_root` to the merged overlay
   - Execute PID 1 from the merged root

3. **All writes go to RAM** (tmpfs upper layer), lost on reboot

4. **`toram` option**: Copies entire squashfs into RAM before mounting, allowing removal of the USB stick. Requires RAM > size of squashfs.

### Casper (Ubuntu/Debian) vs Dracut (Fedora/Arch)

| Feature | Casper | Dracut + dmsquash |
|---|---|---|
| Distros | Ubuntu, Linux Mint, Debian Live | Fedora, CentOS, Arch Linux |
| Initramfs tool | initramfs-tools | dracut |
| Overlay method | OverlayFS (modern) or UnionFS | Device-mapper snapshot |
| Persistence label | `casper-rw` partition | Varies |
| Persistence stability | Stable, widely tested | Snapshot fills = problems |
| Boot parameter | `boot=casper` | `rd.live.image` |
| SquashFS location | `/casper/filesystem.squashfs` | `/LiveOS/squashfs.img` |
| Network boot | `netboot=` parameter | `livenet` module |

### Live ISO vs Installable ISO

| Feature | Live ISO | Installable ISO |
|---|---|---|
| Purpose | Try OS without installing | Install OS to disk |
| Root filesystem | SquashFS + RAM overlay | Written to disk during install |
| Changes | Ephemeral (lost on reboot) | Permanent |
| Persistence | Optional (`casper-rw` partition) | Inherent |
| First boot | Every boot is "first boot" | One-time setup |
| Machine ID | Reset every boot (or fixed for live) | Set permanently on install |

### Can Llamaste Run Directly from USB?

**Yes.** The live USB pattern is ideal for Llamaste:
- Boot from USB → squashfs loads → llamaste binary runs as PID 1
- No installation required
- Models stored on a separate USB partition or downloaded on first boot
- Persistent config can use a `DATA` partition on the same USB

For Llamaste specifically, the "live" model is the primary use case. The squashfs contains the OS (kernel + llamaste binary + web UI). A separate DATA partition (ext4) on the same USB holds models and config.

### OpenWrt Architecture: The Appliance Pattern

OpenWrt demonstrates the appliance OS pattern most relevant to Llamaste:

```
Flash/Storage layout:
├── Bootloader (U-Boot or GRUB)
├── kernel partition
└── root partition
    ├── lower (squashfs, read-only, ~15MB base)
    └── upper (JFFS2/ext4 overlay, writable)
        merged by overlayfs at /

Boot sequence:
1. Bootloader loads kernel
2. Kernel mounts squashfs as lower layer
3. preinit script sets up overlay
4. pivot_root to merged overlay
5. /sbin/init (or custom binary) runs as PID 1
```

Key insight from OpenWrt: The OS image stays small (compressed squashfs) because large data (packages, config) lives in the writable overlay on a separate partition. This is the exact pattern Llamaste should use for models.

---

## 3. Hybrid BIOS+UEFI ISO Creation

### What "Hybrid" Means

An ISO image is "hybrid" when it contains boot equipment for multiple scenarios:
- **El Torito BIOS boot record**: For booting from optical media (CD/DVD) via BIOS
- **MBR with isohybrid code**: For booting from USB stick via legacy BIOS
- **GPT with EFI System Partition pointer**: For booting from USB stick via UEFI

The first 32KB of an ISO (the "System Area") is normally unused by ISO 9660. This space stores MBR boot code and GPT headers for hybrid boot.

### Method 1: grub-mkrescue (Simplest)

`grub-mkrescue` handles all the complexity automatically. It produces a hybrid ISO bootable via BIOS and UEFI.

**Prerequisites:**
```bash
# Debian/Ubuntu
apt-get install grub-pc-bin grub-efi-amd64-bin xorriso mtools

# The key tool: xorriso (required by grub-mkrescue)
# Without xorriso, grub-mkrescue will fail
```

**Minimum directory structure:**
```
isoroot/
└── boot/
    └── grub/
        └── grub.cfg
```

**grub.cfg example for Llamaste:**
```
set timeout=5
set default=0

insmod all_video
insmod gfxterm

menuentry "Llamaste (Server)" {
    search --no-floppy --label --set=root SYSTEM
    linux /boot/vmlinuz \
        root=LABEL=SYSTEM \
        rootfstype=squashfs \
        ro quiet \
        init=/opt/llamaste \
        console=ttyS0 \
        console=tty0
    initrd /boot/initramfs.img
}

menuentry "Llamaste (Recovery Shell)" {
    search --no-floppy --label --set=root SYSTEM
    linux /boot/vmlinuz \
        root=LABEL=SYSTEM \
        rootfstype=squashfs \
        ro single \
        init=/bin/sh \
        console=ttyS0 \
        console=tty0
    initrd /boot/initramfs.img
}
```

**Build command:**
```bash
grub-mkrescue \
    --directory=/usr/lib/grub/i386-pc \
    --directory=/usr/lib/grub/x86_64-efi \
    -o llamaste.iso \
    isoroot/
```

Or simpler (GRUB auto-detects installed targets):
```bash
grub-mkrescue -o llamaste.iso isoroot/
```

**Note on `--` separator**: Extra xorriso options can be passed after `--`:
```bash
grub-mkrescue -o llamaste.iso isoroot/ -- -volid "LLAMASTE"
```

### Method 2: xorriso Direct (Full Control)

For maximum control over the boot structure, xorriso in mkisofs emulation mode:

```bash
sudo xorriso -as mkisofs \
    -isohybrid-mbr /usr/lib/ISOLINUX/isohdpfx.bin \
    -c isolinux/boot.cat \
    -b isolinux/isolinux.bin \
    -no-emul-boot \
    -boot-load-size 4 \
    -boot-info-table \
    -eltorito-alt-boot \
    -e boot/grub/efi.img \
    -no-emul-boot \
    -isohybrid-gpt-basdat \
    -V "LLAMASTE" \
    -o llamaste.iso \
    ./isoroot/
```

Key flags explained:
- `-isohybrid-mbr isohdpfx.bin`: Installs MBR code that boots ISOLINUX from USB via legacy BIOS
- `-b isolinux/isolinux.bin`: El Torito boot image for BIOS (optical media)
- `-c isolinux/boot.cat`: El Torito boot catalog location (required by spec)
- `-eltorito-alt-boot`: Ends BIOS boot image definition, starts EFI boot image
- `-e boot/grub/efi.img`: El Torito EFI boot image (a FAT filesystem containing GRUB EFI binary)
- `-isohybrid-gpt-basdat`: Creates MBR partition type 0xEF and GPT for EFI booting from USB
- `-V "LLAMASTE"`: Volume label (used by `search --label` in GRUB)

### Inspecting ISO Boot Equipment

To verify the hybrid structure of any ISO:
```bash
xorriso -indev llamaste.iso -report_el_torito plain -report_system_area plain
```

### Minimum ISO Directory Structure for Llamaste

For the grub-mkrescue approach, place these files:

```
isoroot/
├── boot/
│   ├── grub/
│   │   └── grub.cfg          # GRUB menu configuration
│   ├── vmlinuz               # Linux kernel
│   └── initramfs.img         # initramfs (or omit if kernel has all drivers built-in)
└── rootfs.squashfs           # The actual root filesystem
```

Or for a more structured layout:
```
isoroot/
├── boot/
│   ├── grub/
│   │   ├── grub.cfg
│   │   └── themes/           # optional GRUB themes
│   ├── vmlinuz
│   └── initramfs.img
├── EFI/
│   └── BOOT/
│       └── BOOTX64.EFI       # EFI fallback (auto-handled by grub-mkrescue)
└── llamaste/
    └── rootfs.squashfs       # Root filesystem image
```

### Ventoy Compatibility

Ventoy is a popular multi-boot USB tool. To mark an ISO as Ventoy-compatible, use one of:

**Method 1 (easiest):** Include a file named `ventoy.dat` or `VENTOY.DAT` in the ISO root directory. Content does not matter.

**Method 2:** Set the ISO Publisher field:
```bash
grub-mkrescue -o llamaste.iso isoroot/ -- \
    -publisher "VENTOY COMPATIBLE" \
    -volid "LLAMASTE"
```

Or with xorriso:
```bash
xorriso -as mkisofs \
    -publisher "VENTOY COMPATIBLE" \
    -V "LLAMASTE" \
    ...
```

**Ventoy-specific notes:**
- Ventoy creates a virtual disk from the ISO and boots it
- The kernel must find its source media using labels or other identifiers
- Standard `root=LABEL=SYSTEM rootfstype=squashfs` works fine with Ventoy
- ISOs over 4GB are supported

### Building the EFI Image for xorriso Method

The `efi.img` referenced in xorriso is a FAT filesystem image containing GRUB EFI binary:

```bash
# Create a 4MB FAT image for EFI
dd if=/dev/zero of=isoroot/boot/grub/efi.img bs=1M count=4
mkfs.vfat isoroot/boot/grub/efi.img

# Mount and populate
mmd -i isoroot/boot/grub/efi.img ::/EFI ::/EFI/BOOT
mcopy -i isoroot/boot/grub/efi.img \
    /usr/lib/grub/x86_64-efi/grub.efi \
    ::/EFI/BOOT/BOOTX64.EFI
```

---

## 4. QEMU Testing Commands

### QEMU Direct Kernel Boot (Fastest for Development)

Skip the bootloader entirely during development. Test the kernel+rootfs directly:

```bash
# Basic: boot kernel with squashfs root, custom init
qemu-system-x86_64 \
    -m 4096 \
    -kernel output/images/bzImage \
    -drive file=output/images/rootfs.squashfs,format=raw,if=virtio \
    -append "root=/dev/vda rootfstype=squashfs ro init=/opt/llamaste console=ttyS0" \
    -nographic \
    -enable-kvm
```

### QEMU BIOS Boot from ISO

```bash
# Test full ISO boot via legacy BIOS (SeaBIOS)
qemu-system-x86_64 \
    -m 4096 \
    -cdrom llamaste.iso \
    -boot d \
    -enable-kvm \
    -nographic \
    -serial mon:stdio
```

### QEMU UEFI Boot from ISO (with OVMF)

```bash
# Install OVMF first:
# Debian/Ubuntu: apt-get install ovmf
# Arch: pacman -S edk2-ovmf

# Copy OVMF vars (must be writable; copy per-VM)
cp /usr/share/OVMF/OVMF_VARS.fd /tmp/llamaste-vars.fd

# Boot ISO with UEFI
qemu-system-x86_64 \
    -m 4096 \
    -machine q35 \
    -cpu host \
    -enable-kvm \
    -drive if=pflash,format=raw,unit=0,file=/usr/share/OVMF/OVMF_CODE.fd,readonly=on \
    -drive if=pflash,format=raw,unit=1,file=/tmp/llamaste-vars.fd \
    -cdrom llamaste.iso \
    -boot d \
    -nographic \
    -serial mon:stdio
```

### QEMU Full System Test (ISO + Data Drive + HTTP Port Forwarding)

This is the primary test setup for Llamaste:

```bash
# Create a data disk image (for /data partition, models, config)
qemu-img create -f qcow2 /tmp/llamaste-data.qcow2 20G

# Full test: BIOS boot, ISO, data disk, HTTP + SSH forwarding
qemu-system-x86_64 \
    -m 8192 \
    -smp 4 \
    -enable-kvm \
    -cpu host \
    -cdrom llamaste.iso \
    -boot d \
    -drive file=/tmp/llamaste-data.qcow2,format=qcow2,if=virtio \
    -device virtio-net-pci,netdev=net0 \
    -netdev user,id=net0,\
        hostfwd=tcp::8080-:80,\
        hostfwd=tcp::8443-:443,\
        hostfwd=tcp::2222-:22 \
    -nographic \
    -serial mon:stdio
```

After boot, access the Llamaste web UI at `http://localhost:8080`.

### QEMU Full System Test (UEFI + Data Drive + HTTP)

```bash
cp /usr/share/OVMF/OVMF_VARS.fd /tmp/llamaste-vars.fd

qemu-system-x86_64 \
    -m 8192 \
    -smp 4 \
    -enable-kvm \
    -cpu host \
    -machine q35 \
    -global ICH9-LPC.disable_s3=1 \
    -drive if=pflash,format=raw,unit=0,file=/usr/share/OVMF/OVMF_CODE.fd,readonly=on \
    -drive if=pflash,format=raw,unit=1,file=/tmp/llamaste-vars.fd \
    -cdrom llamaste.iso \
    -boot d \
    -drive file=/tmp/llamaste-data.qcow2,format=qcow2,if=virtio \
    -device virtio-net-pci,netdev=net0 \
    -netdev user,id=net0,\
        hostfwd=tcp::8080-:80,\
        hostfwd=tcp::8443-:443,\
        hostfwd=tcp::2222-:22 \
    -nographic \
    -serial mon:stdio
```

### QEMU Test from Raw Disk Image (Instead of ISO)

For testing disk images created by Buildroot:

```bash
# Buildroot typically produces a .img file with full partition layout
qemu-system-x86_64 \
    -m 8192 \
    -smp 4 \
    -enable-kvm \
    -cpu host \
    -drive file=output/images/disk.img,format=raw,if=virtio \
    -drive file=/tmp/llamaste-data.qcow2,format=qcow2,if=virtio \
    -device virtio-net-pci,netdev=net0 \
    -netdev user,id=net0,\
        hostfwd=tcp::8080-:80,\
        hostfwd=tcp::2222-:22 \
    -nographic \
    -serial mon:stdio
```

### QEMU Monitor for Debugging

The QEMU monitor allows introspection and control of the VM.

**Accessing the monitor with `-serial mon:stdio`:**
- Press `Ctrl+A`, then `C` to switch between serial console and QEMU monitor
- At the `(qemu)` prompt, type commands
- Type `q` or `quit` to exit QEMU
- Type `c` or `Ctrl+A c` again to return to serial console

**Useful QEMU monitor commands:**
```
(qemu) info block        # show all block devices
(qemu) info network      # show network configuration
(qemu) info registers    # show CPU registers (useful for early boot debugging)
(qemu) info mem          # show memory mappings
(qemu) system_reset      # soft reset the VM
(qemu) savevm snapshot1  # save VM state
(qemu) loadvm snapshot1  # restore VM state
(qemu) screendump /tmp/screen.ppm  # take screenshot
```

**GDB debugging of early boot (before serial console works):**
```bash
# In terminal 1: start QEMU with GDB stub, stopped at first instruction
qemu-system-x86_64 \
    -m 4096 \
    -kernel bzImage \
    -append "console=ttyS0 root=/dev/vda rootfstype=squashfs ro init=/opt/llamaste" \
    -drive file=rootfs.squashfs,format=raw,if=virtio \
    -nographic \
    -s -S    # -s: GDB on localhost:1234, -S: stop CPU at startup

# In terminal 2: connect GDB
gdb vmlinux   # or your binary
(gdb) target remote localhost:1234
(gdb) continue
```

### QEMU Port Forwarding Details

With `-netdev user,id=net0,hostfwd=...`:
- `hostfwd=tcp::HOST_PORT-:GUEST_PORT`
- ICMP (ping) does NOT work with user-mode networking — use TCP tools for testing
- Multiple `hostfwd` options can be chained with commas

```bash
-netdev user,id=net0,\
    hostfwd=tcp::8080-:80,\
    hostfwd=tcp::8443-:443,\
    hostfwd=tcp::2222-:22
```

Test HTTP server:
```bash
curl http://localhost:8080/
curl http://localhost:8080/api/v1/health
```

### QEMU OVMF File Locations by Distribution

| Distribution | OVMF_CODE | OVMF_VARS |
|---|---|---|
| Debian/Ubuntu | `/usr/share/OVMF/OVMF_CODE.fd` | `/usr/share/OVMF/OVMF_VARS.fd` |
| Arch Linux | `/usr/share/edk2/x64/OVMF_CODE.4m.fd` | `/usr/share/edk2/x64/OVMF_VARS.4m.fd` |
| Fedora | `/usr/share/edk2/ovmf/OVMF_CODE.fd` | `/usr/share/edk2/ovmf/OVMF_VARS.fd` |

Always copy OVMF_VARS before use — it must be writable to store EFI variables between boots.

---

## 5. Model Delivery Strategy

### The Core Problem

Llamaste models range from 0.5 GB (Qwen2.5-0.5B Q4) to ~20 GB (Qwen2.5-72B Q4). The OS image (squashfs) should be under 500 MB. These cannot be bundled together.

### Strategy Comparison

| Strategy | OS Image Size | First Boot | Offline | Pros | Cons |
|---|---|---|---|---|---|
| Bundle model in ISO | 5–20 GB | Instant | Yes | No setup | Huge ISO, only one model |
| Download on first boot | <500 MB | 10–60 min | No | Small image, any model | Requires internet |
| USB sideload | <500 MB | 5–30 min | Yes | Offline, fast | Manual USB prep |
| Pre-provisioned data partition | <500 MB | Instant | Yes | Fast, offline | Separate tooling needed |

### Recommended: Download on First Boot + USB Sideload Fallback

This is the pattern used by most modern appliance OS designs:

**First boot detection logic (inside llamaste binary):**
```
Boot sequence:
1. Check for /data/models/*.gguf — if found, use them
2. Check /dev/sdb1 (USB stick) for models — if found, copy to /data/models/
3. Check for network connectivity
4. If network: download appropriate model based on detected RAM
5. If no network and no models: boot into "setup wizard" web UI
```

**Model selection based on RAM (Qwen2.5 Apache 2.0 models):**
```
RAM < 4 GB:   Qwen2.5-0.5B-Instruct-Q4_K_M (340 MB)
RAM 4-8 GB:   Qwen2.5-3B-Instruct-Q4_K_M  (1.9 GB)
RAM 8-16 GB:  Qwen2.5-7B-Instruct-Q4_K_M  (4.7 GB)
RAM 16-32 GB: Qwen2.5-14B-Instruct-Q4_K_M (8.9 GB)
RAM > 32 GB:  Qwen2.5-32B-Instruct-Q4_K_M (19.8 GB)
```

### Download Mechanism Requirements

For multi-GB downloads on consumer hardware and networks:

1. **Resumable downloads**: Use HTTP Range requests (`Range: bytes=X-`)
2. **Integrity verification**: SHA256 checksum of complete file
3. **Progress reporting**: Stream progress via SSE to web UI
4. **Retry with backoff**: Handle network interruptions gracefully
5. **CDN source**: Hugging Face CDN handles global distribution efficiently

**Recommended source:** `https://huggingface.co/Qwen/Qwen2.5-{size}-Instruct-GGUF/resolve/main/{filename}.gguf`

Hugging Face supports:
- Range requests for resumable downloads
- CDN-accelerated delivery
- Model checksums via API

### USB Sideload Pattern

Standard pattern from embedded Linux (OpenWrt, Android):
1. udev rule detects USB insertion with specific label (e.g., `LLAMASTE-MODELS`)
2. Daemon copies `.gguf` files from USB to `/data/models/`
3. Verifies checksums
4. Removes USB safely

Simpler alternative: On every boot, check a well-known USB label before network check.

### Storage Layout for Models

```
Partition layout on install media:
Part 1: BIOS Boot (1 MB, raw)
Part 2: ESP (256 MB, FAT32) - GRUB EFI
Part 3: SYSTEM (256 MB, squashfs) - OS, binary, web UI
Part 4: DATA (remainder, ext4) - /data mount point

/data layout:
/data/
├── models/              # GGUF model files (primary storage)
│   ├── qwen2.5-7b.gguf
│   └── .active-model    # symlink or config file pointing to current model
├── config/              # User configuration
│   ├── llamaste.toml
│   └── system.json
├── conversations/       # Stored conversations (truncated to 500 chars per turn)
├── logs/               # Audit logs (90-day retention)
└── .initialized        # Stamp file: present = first boot complete
```

### A/B Partition for OS Updates (Not Models)

The Llamaste architecture uses A/B partitions for the OS (SYSTEM-A and SYSTEM-B), but models live in the shared DATA partition:

```
Part 3: SYSTEM-A (256 MB, squashfs) - Active OS
Part 4: SYSTEM-B (256 MB, squashfs) - Standby for OTA updates
Part 5: DATA (ext4) - Models, config, conversations (never overwritten by OTA)
```

This means a 20 GB model is not affected by an OS update. The DATA partition is always preserved.

### OpenWrt Pattern Applied to Llamaste

OpenWrt ships a ~15 MB squashfs with the base OS. Large packages (like OpenSSL, Python) are installed post-boot via `opkg`. The exact equivalent for Llamaste:

- Base image: ~200-400 MB squashfs (kernel + llamaste binary + web UI assets)
- Models: Downloaded post-first-boot to DATA partition
- Future models: Downloaded on demand through web UI or API

This keeps the ISO small enough for practical distribution via:
- Direct download link (< 500 MB, fast download)
- GitHub Releases (size limit: 2 GB per file, fine for OS image)
- Torrent (future, for wider distribution)

---

## 6. YouTube Videos and Video Resources

Direct YouTube searches from web searches were limited due to search engine restrictions on site: operator. Below is what was found plus known relevant channels and videos.

### DigiKey Electronics: "Introduction to Embedded Linux" Series by Shawn Hymel

**Channel:** DigiKey Electronics (YouTube)
**Author:** Shawn Hymel

| Part | Title | Key Content |
|---|---|---|
| Part 1 | Introduction to Embedded Linux: Buildroot | Creating a custom Linux distro with Buildroot for STM32MP157D-DK1; flashing SD card; basic file operations via serial terminal |
| Part 2 | Introduction to Embedded Linux: Yocto Project | Yocto vs Buildroot comparison; layer-based builds |
| Part 3 | Introduction to Embedded Linux: Flash SD Card and Boot Process | Boot process walkthrough; SD card flashing |
| Part 4 | Introduction to Embedded Linux: Yocto Custom Image and Layer | Custom images and layers in Yocto |

**Relevance to Llamaste:** High. Part 1 (Buildroot) and Part 3 (boot process) directly cover Llamaste's build toolchain. Shows how to create a bootable image and understand the Linux boot sequence.

**Direct URL:** `https://www.digikey.com/en/videos/d/digi-key-electronics/introduction-to-embedded-linux-part-1-buildroot-digi-key-electronics`

### Bootlin: Embedded Linux and Buildroot Training

**Channel:** Bootlin (YouTube: `https://www.youtube.com/@bootlin`)
**Authors:** Thomas Petazzoni, Michael Opdenacker

Key training content (also available as free PDF slides):
- **"Tutorial: Learning the Basics of Buildroot"** (Michael Opdenacker, ELCE 2015)
- **"Buildroot: What's New?"** (Thomas Petazzoni, ELC 2014)
- **"Update on Boot Time Reduction Techniques"** (Michael Opdenacker, ELC 2014)
- Full Buildroot training course (paid, but slides are free at `bootlin.com/doc/training/buildroot/`)

**Relevance to Llamaste:** Very high. Bootlin is THE authoritative source on Buildroot. Boot time reduction techniques directly apply to Llamaste's goal of fast boot. Thomas Petazzoni has 5,400+ patches merged into Buildroot.

**Free slides:** `https://bootlin.com/doc/training/buildroot/`

### blinry: "Building a Tiny Linux from Scratch"

**URL:** `https://blinry.org/tiny-linux/` (written tutorial, with conceptual demo)
**Key techniques demonstrated:**
- `make tinyconfig` for minimal kernel configuration
- Writing a custom init binary in Rust (statically linked, `RUSTFLAGS='-C target-feature=+crt-static'`)
- Building an initramfs: `find . -print0 | cpio --null --create --verbose --format=newc | gzip --best > ../initrd`
- Enabling TTY, printk, ELF binary support, initial RAM disk support in kernel config
- Replacing Rust binary with shell script for BusyBox userland

**Relevance to Llamaste:** Very high. This is exactly the pattern for the llamaste binary acting as PID 1. The minimal Rust init binary proof-of-concept directly validates the Llamaste approach.

### Minimal Linux Live (ivandavidov)

**GitHub:** `https://github.com/ivandavidov/minimal`
**Book/Docs:** `https://ivandavidov.github.io/minimal/book/`

MLL is a tiny educational Linux distribution built from shell scripts, containing:
- Linux kernel
- GNU C library
- BusyBox userland
- Overlay bundle system (OverlayFS)
- BIOS and UEFI boot options

**ISO structure produced by MLL (BIOS mode):**
```
minimal_linux_live.iso
├── boot/
│   ├── kernel.xz       # Linux kernel
│   ├── rootfs.xz       # initramfs (cpio archive)
│   └── syslinux/       # ISOLINUX bootloader
├── EFI/                # EFI boot support
└── minimal/            # Overlay bundles
```

**UEFI mode:**
```
minimal_linux_live.iso
├── boot/
│   └── uefi.img        # FAT image with systemd-boot + kernel + initramfs
└── minimal/            # Overlay bundles
```

**Relevance to Llamaste:** High. MLL demonstrates the full pipeline from kernel compilation through ISO creation, with both BIOS and UEFI support. The overlay bundle system is a model for Llamaste's "model pack" distribution concept.

### phip1611.de: "Create a Bootable Image for a Custom Kernel with GRUB"

**URL:** `https://phip1611.de/blog/os-dev-create-a-bootable-image-for-a-custom-kernel-with-grub-as-bootloader-for-legacy-x86-boot-e-g-multiboot2-kernel/`

**Key techniques demonstrated:**
```
Directory structure:
grub/
├── grub.cfg
└── iso/
    ├── boot/
    │   └── grub/
    │       └── grub.cfg    # grub.cfg must be here for grub-mkrescue
    ├── kernel              # Your custom kernel binary
    └── roottask            # Other payload modules

Build command:
grub-mkrescue -o grub/legacy_x86_boot.img grub/iso

Test with QEMU:
qemu-system-x86_64 \
    -boot d \
    -cdrom grub/legacy_x86_boot.img \
    -m 1024 \
    -cpu host \
    -machine q35,accel=kvm:tcg \
    -serial stdio

Flash to USB:
sudo dd if=grub/legacy_x86_boot.img of=/dev/sda
```

**Critical detail:** "Paths in grub.cfg MUST use leading slashes."

**Relevance to Llamaste:** Medium. Demonstrates the exact grub-mkrescue workflow that Llamaste's Buildroot external tree will use. Good concrete reference for grub.cfg syntax.

---

## 7. Key Decisions for Llamaste Based on Research

### Decision 1: Use a Minimal initramfs

**Rationale:** Portability across storage controllers > 50ms boot savings. The initramfs in Buildroot is generated automatically and adds minimal complexity.

**Implementation:** In Buildroot config, set `BR2_TARGET_ROOTFS_INITRAMFS=y` and build a minimal initramfs that:
1. Loads storage modules if needed
2. Mounts the squashfs root by label (`LABEL=SYSTEM`)
3. Executes `switch_root` to the squashfs
4. Hands off to `/opt/llamaste` as the new PID 1

### Decision 2: Live USB Primary, Install-to-Disk Secondary

**Rationale:** Live USB is simpler to produce and matches the appliance pattern. Users can run Llamaste without modifying their system. Install-to-disk can be added later.

**Implementation:** squashfs on the USB boot partition, ext4 DATA partition on the same USB for models and config.

### Decision 3: grub-mkrescue for ISO Production

**Rationale:** Single command, handles BIOS+UEFI hybrid, integrates with Buildroot's `post-image.sh` hook.

**Post-image script fragment:**
```bash
#!/bin/bash
# Called by Buildroot after image creation

ISO_ROOT="${BINARIES_DIR}/iso-root"
mkdir -p "${ISO_ROOT}/boot/grub"

# Copy kernel and initramfs
cp "${BINARIES_DIR}/bzImage" "${ISO_ROOT}/boot/vmlinuz"
cp "${BINARIES_DIR}/rootfs.cpio.gz" "${ISO_ROOT}/boot/initramfs.img"

# Copy squashfs
cp "${BINARIES_DIR}/rootfs.squashfs" "${ISO_ROOT}/llamaste/rootfs.squashfs"

# Write grub.cfg
cat > "${ISO_ROOT}/boot/grub/grub.cfg" << 'EOF'
set timeout=5
set default=0
menuentry "Llamaste" {
    search --no-floppy --label --set=root SYSTEM
    linux /boot/vmlinuz root=LABEL=SYSTEM rootfstype=squashfs ro quiet init=/opt/llamaste
    initrd /boot/initramfs.img
}
EOF

# Add Ventoy compatibility marker
touch "${ISO_ROOT}/ventoy.dat"

# Build hybrid ISO
grub-mkrescue -o "${BINARIES_DIR}/llamaste.iso" "${ISO_ROOT}/" -- \
    -volid "LLAMASTE" \
    -publisher "VENTOY COMPATIBLE"
```

### Decision 4: Download on First Boot + USB Sideload

**Rationale:** Small ISO (<500 MB), supports offline use via USB sideload, downloads correct model for hardware.

**Boot sequence:**
1. Check `/data/models/` for existing models
2. Check USB for models (by looking for partition labeled `LLAMASTE-MODELS`)
3. Check internet connectivity
4. Auto-download appropriate model based on RAM if online
5. If nothing works: serve "no model" page with instructions

### Decision 5: QEMU Test Matrix

For CI/validation:
```bash
# Test 1: Direct kernel boot (fastest iteration)
qemu-system-x86_64 -kernel bzImage -drive file=rootfs.squashfs,format=raw,if=virtio \
    -append "root=/dev/vda rootfstype=squashfs ro init=/opt/llamaste console=ttyS0" \
    -m 4096 -nographic -enable-kvm

# Test 2: BIOS ISO boot
qemu-system-x86_64 -m 4096 -cdrom llamaste.iso -boot d -nographic -enable-kvm

# Test 3: UEFI ISO boot
qemu-system-x86_64 -m 4096 -machine q35 -cpu host -enable-kvm \
    -drive if=pflash,format=raw,unit=0,file=/usr/share/OVMF/OVMF_CODE.fd,readonly=on \
    -drive if=pflash,format=raw,unit=1,file=/tmp/ovmf-vars.fd \
    -cdrom llamaste.iso -boot d -nographic

# Test 4: Full system with HTTP access
qemu-system-x86_64 -m 8192 -smp 4 -enable-kvm \
    -cdrom llamaste.iso -boot d \
    -drive file=/tmp/data.qcow2,format=qcow2,if=virtio \
    -netdev user,id=net0,hostfwd=tcp::8080-:80 \
    -device virtio-net-pci,netdev=net0 \
    -nographic -serial mon:stdio
# Then: curl http://localhost:8080/
```

---

## 8. Reference Links

- [Booting Without initramfs - DOTSLASHLINUX](https://firasuke.github.io/DOTSLASHLINUX/post/booting-the-linux-kernel-without-an-initrd-initramfs/)
- [ramfs, rootfs and initramfs - Kernel Docs](https://docs.kernel.org/filesystems/ramfs-rootfs-initramfs.html)
- [bootparam(7) - Linux manual page](https://man7.org/linux/man-pages/man7/bootparam.7.html)
- [SquashFS HOWTO - TLDP](https://tldp.org/HOWTO/html_single/SquashFS-HOWTO/)
- [GNU xorriso](https://www.gnu.org/software/xorriso/)
- [Invoking grub-mkrescue - GNU GRUB Manual](https://www.gnu.org/software/grub/manual/grub/html_node/Invoking-grub_002dmkrescue.html)
- [UEFI/OVMF - Ubuntu Wiki](https://wiki.ubuntu.com/UEFI/OVMF)
- [How to run OVMF - Tianocore](https://github.com/tianocore/tianocore.github.io/wiki/How-to-run-OVMF)
- [QEMU Direct Linux Boot - Official Docs](https://qemu-project.gitlab.io/qemu/system/linuxboot.html)
- [Ventoy Compatible Format](https://www.ventoy.net/en/doc_compatible_format.html)
- [Ventoy Compatible Mark](https://www.ventoy.net/en/doc_compatible_mark.html)
- [OTA for Embedded Linux - Interrupt](https://interrupt.memfault.com/blog/ota-for-embedded-linux-devices)
- [Mender - Robust OTA with A/B Partitions](https://mender.io/blog/robust-ota-updates-with-partitions-for-linux-devices)
- [Casper Man Page - Ubuntu](https://manpages.ubuntu.com/manpages/focal/man7/casper.7.html)
- [OpenWrt Wikipedia](https://en.wikipedia.org/wiki/OpenWrt)
- [Building a Tiny Linux from Scratch - blinry](https://blinry.org/tiny-linux/)
- [Minimal Linux Live - ivandavidov](https://github.com/ivandavidov/minimal)
- [DigiKey Embedded Linux Part 1 - Buildroot](https://www.digikey.com/en/videos/d/digi-key-electronics/introduction-to-embedded-linux-part-1-buildroot-digi-key-electronics)
- [Bootlin Buildroot Training](https://bootlin.com/training/buildroot/)
- [Bootlin Free Training Slides](https://bootlin.com/doc/training/buildroot/)
- [grub-mkrescue Bootable Image - phip1611](https://phip1611.de/blog/os-dev-create-a-bootable-image-for-a-custom-kernel-with-grub-as-bootloader-for-legacy-x86-boot-e-g-multiboot2-kernel/)
- [LUKS and Initramfs - Infosec Resources](https://resources.infosecinstitute.com/topic/luks-and-initramfs/)
- [Recreating ISOs for BIOS and UEFI - 0xf8.org](https://www.0xf8.org/2020/03/recreating-isos-that-boot-from-both-dvd-and-mass-storage-such-as-usb-sticks-and-in-both-legacy-bios-and-uefi-environments/)
- [xorriso El Torito Hybrid - linuxconfig.org](https://linuxconfig.org/legacy-bios-uefi-and-secureboot-ready-ubuntu-live-image-customization)
- [Resumable llama.cpp Downloads - Docker Blog](https://www.docker.com/blog/llama-cpp-resumable-gguf-downloads/)
