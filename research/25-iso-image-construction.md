# ISO Image Construction & Boot Architecture Research

**Date**: 2026-02-27
**Purpose**: Fill specific gaps in bootable image knowledge for Llamaste Phase 1 build
**Depends on**: research/03-bootable-images.md (BIOS/UEFI basics, partition layout, GRUB config)

---

## Table of Contents

1. [initramfs Decision](#1-initramfs-decision)
2. [Live USB / Installer Architecture](#2-live-usb--installer-architecture)
3. [Image Construction Methods](#3-image-construction-methods)
4. [Model Delivery Strategy](#4-model-delivery-strategy)
5. [YouTube & Tutorial Resources](#5-youtube--tutorial-resources)
6. [Llamaste-Specific Recommendations](#6-llamaste-specific-recommendations)

---

## 1. initramfs Decision

### 1.1 Can We Skip initramfs Entirely?

**Yes.** Booting without initramfs is fully supported by the Linux kernel and is common in embedded systems. The key requirements are:

1. **All necessary drivers must be compiled INTO the kernel** (not as modules): storage controller (AHCI/NVMe/virtio), filesystem (squashfs, ext4), block device layer (sd_mod)
2. **Kernel must know root device**: via `root=` parameter (e.g., `root=LABEL=SYSTEM` or `root=/dev/sda3`)
3. **`CONFIG_BLK_DEV_INITRD` can be disabled** to eliminate initramfs support entirely
4. **`init=` parameter specifies PID 1**: e.g., `init=/opt/llamaste`

Sources:
- https://firasuke.github.io/DOTSLASHLINUX/post/booting-the-linux-kernel-without-an-initrd-initramfs/
- https://docs.kernel.org/filesystems/ramfs-rootfs-initramfs.html

### 1.2 What the Kernel Does Without initramfs

When there is no initramfs, the kernel's `init/noinitramfs.c` code:

1. Creates a minimal rootfs in memory with `/dev`, `/dev/console` (5,1), `/root`
2. Mounts the root partition specified by `root=` kernel parameter
3. If `CONFIG_DEVTMPFS_MOUNT=y`, automatically mounts devtmpfs at `/dev` on the real rootfs
4. Executes `init=` binary (or falls back to `/sbin/init`, `/etc/init`, `/bin/init`, `/bin/sh`)

The init binary becomes PID 1. If PID 1 exits, kernel panic.

### 1.3 What PID 1 Must Do (the `llamaste` binary)

With `CONFIG_DEVTMPFS=y` and `CONFIG_DEVTMPFS_MOUNT=y`, the kernel auto-populates `/dev` with device nodes. The `llamaste` binary as PID 1 must still:

```c
// Mount virtual filesystems
mount("proc",    "/proc", "proc",    MS_NOSUID|MS_NODEV|MS_NOEXEC, NULL);
mount("sysfs",   "/sys",  "sysfs",   MS_NOSUID|MS_NODEV|MS_NOEXEC, NULL);
mount("tmpfs",   "/tmp",  "tmpfs",   MS_NOSUID|MS_NODEV, "size=10%");
mount("tmpfs",   "/run",  "tmpfs",   MS_NOSUID|MS_NODEV, "mode=0755");

// /dev is already mounted by kernel (CONFIG_DEVTMPFS_MOUNT=y)
// But we need /dev/pts for pseudoterminals and /dev/shm for shared memory
mkdir("/dev/pts", 0755);
mount("devpts",  "/dev/pts", "devpts", 0, "gid=5,mode=620");
mkdir("/dev/shm", 1777);
mount("tmpfs",   "/dev/shm", "tmpfs",  MS_NOSUID|MS_NODEV, NULL);
```

Critical: `/dev/console`, `/dev/null`, `/dev/zero` are created by devtmpfs automatically. No `mknod` calls needed.

Sources:
- https://cateee.net/lkddb/web-lkddb/DEVTMPFS_MOUNT.html
- https://wiki.gentoo.org/wiki/Custom_Initramfs
- https://buildroot.uclibc.narkive.com/rM1I9Jix/where-is-dev-console-created-when-using-devtmpfs

### 1.4 devtmpfs vs Explicit mknod

| Approach | Pros | Cons |
|----------|------|------|
| **devtmpfs (recommended)** | Kernel auto-creates nodes; zero userspace code; handles hotplug | All nodes owned root:root mode 0600 by default |
| **Static mknod** | Full control over permissions | Must know all devices at compile time; no hotplug |
| **mdev (BusyBox)** | Runs on top of devtmpfs; handles permissions/symlinks | Requires BusyBox; we don't have it |
| **eudev/udev** | Full device management | Heavy; we don't need it |

**For Llamaste**: devtmpfs alone is sufficient. We only need a handful of devices (console, null, zero, random, urandom, tty, network interfaces, disk devices). The default 0600 permissions are fine since everything runs as root (PID 1).

### 1.5 What Breaks Without initramfs?

Things that DO NOT work without initramfs:

| Feature | Works? | Workaround for Llamaste |
|---------|--------|------------------------|
| Modular drivers | No | Compile all drivers into kernel |
| LUKS/encrypted root | No | Not needed for Phase 1 (encrypt DATA partition from userspace later) |
| LVM root | No | Not using LVM |
| UUID-based root= | Maybe | Use LABEL= or device path instead |
| Plymouth splash | No | Not needed |
| Complex device discovery | No | Built-in drivers discover synchronously |
| Network-based root (NFS, iSCSI) | No | Not needed |

Things that DO work:

- `root=LABEL=SYSTEM` (kernel has built-in label scanning)
- `root=/dev/sda3` (direct device path)
- `rootfstype=squashfs` (if squashfs compiled in)
- `init=/opt/llamaste` (custom PID 1)
- `console=ttyS0,115200` (serial console)
- `console=tty0` (VGA console)
- `ip=dhcp` (kernel-level DHCP)

### 1.6 Boot Time Impact

| Configuration | Typical Boot Time Delta | Notes |
|---------------|------------------------|-------|
| With initramfs | +100-500ms | Decompress cpio, run /init script, pivot_root |
| Without initramfs | Baseline | Kernel directly mounts root, runs init= |
| Exception cases | initramfs can be -800ms to +1s slower | Depends on hardware detection timing |

For Llamaste with built-in drivers and no initramfs, the boot path is:
```
GRUB -> kernel decompression -> kernel init -> mount root (squashfs) ->
exec /opt/llamaste -> mount /proc, /sys -> start HTTP server -> ready
```

Estimated total: 2-5 seconds to first HTTP response on modern hardware.

### 1.7 Recommendation: Skip initramfs

**Decision: No initramfs for Llamaste.**

Rationale:
1. All drivers compiled into kernel (we control the kernel config)
2. Root is always squashfs on a known partition (LABEL=SYSTEM)
3. Saves 100-500ms boot time
4. Eliminates an entire build artifact and boot stage
5. Simpler build pipeline (no initramfs generation step)
6. The `llamaste` binary handles all post-kernel setup itself
7. No LUKS, LVM, or network root needed

Required kernel config:
```
CONFIG_BLK_DEV_INITRD=n          # Disable initramfs support
CONFIG_DEVTMPFS=y                # Create device nodes automatically
CONFIG_DEVTMPFS_MOUNT=y          # Auto-mount at /dev after root mount
CONFIG_SQUASHFS=y                # Built-in squashfs (not module)
CONFIG_SQUASHFS_ZSTD=y           # zstd compression for squashfs
CONFIG_EXT4_FS=y                 # Built-in ext4 for DATA partition
CONFIG_AHCI=y                    # SATA controller
CONFIG_NVME=y                    # NVMe controller
CONFIG_VIRTIO_BLK=y              # Virtio block (for QEMU)
CONFIG_VIRTIO_NET=y              # Virtio net (for QEMU)
```

---

## 2. Live USB / Installer Architecture

### 2.1 Squashfs + OverlayFS Live Boot Pattern

The standard live Linux architecture uses three layers:

```
+-------------------+
| OverlayFS (upper) |  <-- tmpfs (RAM) or persistent ext4
+-------------------+
| SquashFS (lower)  |  <-- read-only compressed root
+-------------------+
| Block device      |  <-- USB stick, hard drive, or ISO
+-------------------+
```

Copy-on-Write (CoW): modifications create copies in the upper layer, leaving the lower squashfs untouched. This is exactly what Llamaste already uses (see research/03-bootable-images.md overlayfs section).

Sources:
- https://medium.com/@akashsainisaini37/how-overlayfs-and-squashfs-power-embedded-linux-storage-75273028ef20
- https://bbs.archlinux.org/viewtopic.php?id=275957

### 2.2 Live System vs Installer: Which for Llamaste?

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| **Live system (dd-to-disk)** | User writes raw image to disk; boots directly | Simplest; identical output every time; works headless | Fixed partition sizes (needs first-boot resize); large image if DATA partition included |
| **Live+Installer** | Boots from USB into RAM; installer copies to disk | Can adapt to disk size; user chooses target | Requires installer code; interactive; complex |
| **Live-only (run from USB)** | Always boots from USB with overlayfs | No install step; try-before-install | USB wear; slower I/O; limited persistence |

**Recommendation for Llamaste: Raw disk image (dd-to-disk) as primary, with first-boot resize.**

Rationale:
1. Llamaste is an appliance, not a desktop distro -- users should flash it like a Raspberry Pi image
2. No interactive installer needed (headless server is primary use case)
3. First-boot script resizes DATA partition to fill disk (standard pattern, used by Raspberry Pi OS, SUSE Micro)
4. A hybrid ISO as secondary format supports Ventoy and optical media

The image will be distributed as:
- **Primary**: `llamaste-x86_64.img.xz` (compressed raw disk image, ~300-400 MB)
- **Secondary**: `llamaste-x86_64.iso` (hybrid ISO for Ventoy/optical)
- SHA256SUMS for verification

### 2.3 Ventoy Compatibility

Ventoy emulates the ISO file as a virtual CDROM. For Llamaste ISO compatibility:

**Method 1 (Automatic)**: If we create a standard hybrid ISO with GRUB, Ventoy will attempt to boot it using its hook mechanism. Ventoy intercepts the boot process and redirects the OS to find its source media on the virtual disk.

**Method 2 (Ventoy Compatible mark)**: Place an empty file named `ventoy.dat` in the root directory of the ISO image. This tells Ventoy to skip its hook and trust that the OS knows how to find its own media. For Llamaste, since we use `root=LABEL=SYSTEM`, our kernel will search all block devices for a partition with that label -- this works even when Ventoy presents the ISO as a virtual CDROM.

**Persistence with Ventoy**: Ventoy supports persistence plugins via a configuration file `ventoy.json` on the USB stick. This is separate from our built-in persistence (DATA partition), so Ventoy persistence is not needed for Llamaste.

Sources:
- https://www.ventoy.net/en/doc_compatible_mark.html
- https://www.ventoy.net/en/plugin_persistence.html

### 2.4 Hybrid ISO (USB + CD/DVD)

A hybrid ISO boots from both optical media and USB mass storage. The boot entry points are:

| Boot Method | Medium | Mechanism |
|-------------|--------|-----------|
| BIOS from CD/DVD | El Torito boot record -> GRUB i386-pc | `-b` flag |
| BIOS from USB | MBR boot code in system area | `-isohybrid-mbr` flag |
| EFI from CD/DVD | El Torito EFI boot record -> efi.img | `-e` flag |
| EFI from USB | GPT partition -> EFI System Partition | `-isohybrid-gpt-basdat` flag |

The ISO contains both an El Torito boot catalog (for optical) and an MBR/GPT system area (for USB). `grub-mkrescue` creates this automatically when both `grub-pc-bin` and `grub-efi-amd64-bin` are installed.

Source: https://www.0xf8.org/2020/03/recreating-isos-that-boot-from-both-dvd-and-mass-storage-such-as-usb-sticks-and-in-both-legacy-bios-and-uefi-environments/

---

## 3. Image Construction Methods

### 3.1 grub-mkrescue: Hybrid ISO

The simplest way to create a BIOS+UEFI hybrid ISO.

**Prerequisites** (on Ubuntu/Debian):
```bash
sudo apt install grub-common xorriso grub-pc-bin grub-efi-amd64-bin mtools
```

**Directory structure**:
```
iso-root/
  boot/
    grub/
      grub.cfg           # GRUB menu configuration
    vmlinuz              # Kernel image
  opt/
    llamaste             # The binary (on squashfs, not directly in ISO)
  EFI/
    BOOT/
      BOOTX64.EFI        # Auto-generated by grub-mkrescue
```

**Command**:
```bash
grub-mkrescue -o llamaste.iso ./iso-root/
```

**What it does internally**: grub-mkrescue calls xorriso with appropriate flags to create El Torito boot records for both BIOS (i386-pc) and UEFI (x86_64-efi), embed an MBR for USB booting, and create a GPT with an EFI System Partition.

**Verbose mode** (see the actual xorriso command):
```bash
grub-mkrescue -v -o llamaste.iso ./iso-root/ 2>&1 | grep xorriso
```

Sources:
- https://www.gnu.org/software/grub/manual/grub/html_node/Invoking-grub_002dmkrescue.html
- https://wiki.osdev.org/GRUB

### 3.2 xorriso: Fine-Grained ISO Control

For cases where grub-mkrescue does not provide enough control, use xorriso directly.

**Complete BIOS+UEFI hybrid ISO command** (Debian-style):
```bash
# Create the EFI boot image first
dd if=/dev/zero of=efi.img bs=1M count=4
mkfs.vfat efi.img
mmd -i efi.img EFI EFI/BOOT
mcopy -i efi.img /usr/lib/grub/x86_64-efi/monolithic/grubx64.efi ::EFI/BOOT/BOOTX64.EFI
mcopy -i efi.img grub.cfg ::EFI/BOOT/grub.cfg

# Create the ISO
xorriso -as mkisofs \
    -r -J -joliet-long \
    -V "LLAMASTE" \
    -o llamaste.iso \
    -isohybrid-mbr /usr/lib/grub/i386-pc/boot_hybrid.img \
    -partition_offset 16 \
    --grub2-mbr /usr/lib/grub/i386-pc/boot_hybrid.img \
    -b boot/grub/i386-pc/eltorito.img \
    -no-emul-boot -boot-load-size 4 -boot-info-table --grub2-boot-info \
    -eltorito-alt-boot \
    -e boot/grub/efi.img \
    -no-emul-boot \
    -isohybrid-gpt-basdat \
    ./iso-root/
```

**Key flag reference**:
| Flag | Purpose |
|------|---------|
| `-isohybrid-mbr <file>` | Embed MBR code for BIOS USB boot |
| `-b <path>` | El Torito BIOS boot image |
| `-no-emul-boot` | Don't emulate floppy/CD |
| `-boot-load-size 4` | Load 4 sectors (2048 bytes) |
| `-boot-info-table` | Patch boot image with CD layout info |
| `-eltorito-alt-boot` | Start second boot catalog entry |
| `-e <path>` | El Torito EFI boot image |
| `-isohybrid-gpt-basdat` | Create GPT with EFI System Partition for USB |
| `-V "LABEL"` | Volume label |

**Inspect existing ISO boot setup**:
```bash
xorriso -indev existing.iso -report_system_area plain -report_el_torito plain
```

Sources:
- https://wiki.debian.org/RepackBootableISO
- https://www.gnu.org/software/xorriso/man_1_xorrisofs.html

### 3.3 genimage: Buildroot Disk Image Generation

genimage is Buildroot's tool for assembling final disk images from individual filesystem images.

**Full genimage.cfg for Llamaste** (5-partition GPT):
```ini
image efi.vfat {
    vfat {
        label = "ESP"
    }
    size = 256M

    # GRUB EFI binary
    file EFI/BOOT/BOOTX64.EFI {
        image = "grubx64.efi"
    }
    file EFI/BOOT/grub.cfg {
        image = "grub-efi.cfg"
    }
}

image llamaste.img {
    hdimage {
        partition-table-type = "gpt"
        gpt-location = 1M
    }

    # Partition 1: BIOS Boot (for GRUB i386-pc core.img)
    partition bios-boot {
        partition-type-uuid = "21686148-6449-6E6F-744E-656564454649"
        offset = 1M
        size = 1M
        image = "grub-bios-core.img"
        in-partition-table = true
    }

    # Partition 2: EFI System Partition
    partition esp {
        partition-type-uuid = U
        offset = 2M
        size = 256M
        image = "efi.vfat"
        bootable = true
    }

    # Partition 3: System A (active root)
    partition system-a {
        partition-type-uuid = L
        size = 256M
        image = "rootfs.squashfs"
    }

    # Partition 4: System B (update slot, initially empty)
    partition system-b {
        partition-type-uuid = L
        size = 256M
    }

    # Partition 5: Data (user data, models, config)
    partition data {
        partition-type-uuid = L
        autoresize = true
        image = "data.ext4"
    }
}
```

**genimage options reference**:

| Image Type | Key Options |
|------------|------------|
| `hdimage` | `partition-table-type` (gpt/mbr/hybrid), `gpt-location`, `align`, `fill` |
| `squashfs` | `compression` (gzip/lzo/lz4/xz/zstd), `block-size` (default 4096), `extraargs` |
| `ext4` | `use-mke2fs`, `features`, `label`, `fs-timestamp` |
| `vfat` | `label`, `extraargs` |
| `partition` | `offset`, `size`, `image`, `autoresize`, `bootable`, `partition-type-uuid`, `in-partition-table` |

**GPT partition type UUID shortcuts**: `U` = EFI System Partition, `L` = Linux filesystem, `F` = FAT32/Basic Data.

Sources:
- https://github.com/pengutronix/genimage
- https://github.com/pengutronix/genimage/blob/master/README.rst

### 3.4 Buildroot Post-Build/Post-Image Integration

Buildroot provides hook points for image customization:

**Build order**:
1. Packages built and installed to `output/target/`
2. Root filesystem overlays applied (`BR2_ROOTFS_OVERLAY`)
3. **Post-build scripts** run (`BR2_ROOTFS_POST_BUILD_SCRIPT`) -- target filesystem as `$1`
4. **Post-fakeroot scripts** run (under fakeroot for permission setting)
5. Filesystem images generated (squashfs, ext4, etc.)
6. **Post-image scripts** run (`BR2_ROOTFS_POST_IMAGE_SCRIPT`) -- images dir as `$1`

**Llamaste post-image.sh** (generates final disk image):
```bash
#!/bin/bash
set -e

BOARD_DIR=$(dirname "$0")
GENIMAGE_CFG="${BOARD_DIR}/genimage.cfg"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"

# Build GRUB images for BIOS and EFI
grub-mkimage -O i386-pc -o "${BINARIES_DIR}/grub-bios-core.img" \
    -p /boot/grub biosdisk part_gpt ext2 squash4 normal search configfile

grub-mkimage -O x86_64-efi -o "${BINARIES_DIR}/grubx64.efi" \
    -p /EFI/BOOT part_gpt fat squash4 normal search configfile linux

# Generate the disk image
rm -rf "${GENIMAGE_TMP}"
genimage \
    --rootpath "${TARGET_DIR}" \
    --tmppath "${GENIMAGE_TMP}" \
    --inputpath "${BINARIES_DIR}" \
    --outputpath "${BINARIES_DIR}" \
    --config "${GENIMAGE_CFG}"
```

**Environment variables available in post-scripts**:
- `BR2_CONFIG` -- path to .config
- `HOST_DIR` -- host tools directory
- `TARGET_DIR` -- target rootfs directory
- `BINARIES_DIR` -- output images directory
- `BUILD_DIR` -- packages build directory

**Key rule**: Post-image scripts run as the build user (not root). Use post-fakeroot scripts for permission/ownership changes.

Source: https://buildroot.org/downloads/manual/manual.html

### 3.5 QEMU Testing Commands

**BIOS boot (disk image)**:
```bash
qemu-system-x86_64 \
    -m 4G \
    -smp 4 \
    -drive file=llamaste.img,format=raw,if=virtio \
    -nographic \
    -append "console=ttyS0" \
    -net user,hostfwd=tcp::8080-:80,hostfwd=tcp::2222-:22 \
    -net nic,model=virtio
```

**UEFI boot (disk image, with OVMF)**:
```bash
# Install OVMF: sudo apt install ovmf
# Copy OVMF_VARS for writability
cp /usr/share/OVMF/OVMF_VARS_4M.fd ./ovmf_vars.fd

qemu-system-x86_64 \
    -m 4G \
    -smp 4 \
    -machine q35 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=./ovmf_vars.fd \
    -drive file=llamaste.img,format=raw,if=virtio \
    -nographic \
    -net user,hostfwd=tcp::8080-:80 \
    -net nic,model=virtio
```

**Direct kernel boot (fastest, for development)**:
```bash
qemu-system-x86_64 \
    -m 4G \
    -smp 4 \
    -kernel output/images/bzImage \
    -drive file=output/images/rootfs.squashfs,format=raw,if=virtio,readonly=on \
    -drive file=output/images/data.ext4,format=raw,if=virtio \
    -append "root=/dev/vda rootfstype=squashfs ro init=/opt/llamaste console=ttyS0" \
    -nographic \
    --enable-kvm \
    -cpu host \
    -net user,hostfwd=tcp::8080-:80 \
    -net nic,model=virtio
```

**ISO boot test**:
```bash
qemu-system-x86_64 \
    -m 4G \
    -cdrom llamaste.iso \
    -nographic
```

**GDB debugging** (add to any of the above):
```bash
    -s -S    # -s = gdbserver on :1234, -S = pause at start
```

Then attach:
```bash
gdb output/build/linux-*/vmlinux
(gdb) target remote :1234
(gdb) hbreak start_kernel
(gdb) c
```

**Useful QEMU flags reference**:
| Flag | Purpose |
|------|---------|
| `-m 4G` | Guest RAM |
| `-smp 4` | CPU count |
| `--enable-kvm` | Hardware acceleration (10-100x faster) |
| `-cpu host` | Pass through host CPU features |
| `-nographic` | Serial console only (no GUI window) |
| `-machine q35` | Modern chipset (needed for UEFI) |
| `-s` | GDB server on TCP :1234 |
| `-S` | Pause CPU at startup (wait for GDB) |
| `-monitor stdio` | QEMU monitor on terminal |
| `-serial mon:stdio` | Multiplex serial + monitor |
| `-net user,hostfwd=tcp::HOST-:GUEST` | Port forwarding |
| `-drive if=virtio` | Fast paravirtual disk |

Sources:
- https://qemu-project.gitlab.io/qemu/system/linuxboot.html
- https://nickdesaulniers.github.io/blog/2018/10/24/booting-a-custom-linux-kernel-in-qemu-and-debugging-it-with-gdb/
- https://radiki.dev/posts/qemu-setup-for-kernel-dev-1/

---

## 4. Model Delivery Strategy

### 4.1 Design: Small OS Image + Separate Model Download

The Llamaste OS image should be kept small (~300-500 MB compressed). Models are NOT bundled in the image. Instead:

**First boot (internet available)**:
1. `llamaste` starts, detects no model on DATA partition
2. Presents a first-boot wizard via web UI (http://llamaste.local:80)
3. Auto-detects available RAM, recommends appropriate model/quantization
4. Downloads model from Hugging Face Hub to `/data/models/`
5. Verifies SHA256 checksum
6. Loads model and becomes operational

**Offline/air-gapped fallback (USB sideload)**:
1. User pre-downloads `.gguf` file on another machine
2. Copies to USB stick
3. Plugs USB into Llamaste machine
4. `llamaste` detects USB mount, scans for `.gguf` files
5. Copies to `/data/models/`, verifies integrity
6. Loads model

### 4.2 USB Sideload Implementation

```c
// Pseudo-code for USB model detection
void check_usb_models() {
    // Monitor /dev for new block devices via inotify on /sys/block/
    // When USB inserted:
    // 1. Identify new device (e.g., /dev/sdb1)
    // 2. Mount read-only to /tmp/usb
    // 3. Scan for *.gguf files
    // 4. If found, present list in web UI
    // 5. User confirms -> copy to /data/models/
    // 6. Unmount USB
}
```

The kernel's devtmpfs will create device nodes automatically when USB is inserted. The `llamaste` binary can monitor `/sys/block/` or use `inotify` on `/dev/` to detect new devices.

### 4.3 Model Size Reference

| Model | Quantization | Size | Min RAM |
|-------|-------------|------|---------|
| Qwen2.5-0.5B-Instruct | Q8_0 | ~600 MB | 2 GB |
| Qwen2.5-1.5B-Instruct | Q4_K_M | ~1.0 GB | 4 GB |
| Qwen2.5-3B-Instruct | Q4_K_M | ~2.0 GB | 6 GB |
| Qwen2.5-7B-Instruct | Q4_K_M | ~4.5 GB | 10 GB |
| Qwen2.5-14B-Instruct | Q4_K_M | ~8.5 GB | 20 GB |

Download times at common speeds:
- 10 Mbps: 0.5 GB model = 7 min, 4.5 GB = 60 min
- 100 Mbps: 0.5 GB = 40s, 4.5 GB = 6 min
- 1 Gbps: 0.5 GB = 4s, 4.5 GB = 36s

### 4.4 How Other Projects Handle Model Distribution

**llamafile** (Mozilla):
- Bundles model weights directly into the executable using PKZIP format
- Uses `zipalign` tool to concatenate GGUF weights to the APE binary
- Weights are memory-mapped directly from the zip archive (no extraction)
- Alignment to 65536 bytes enables GPU direct access
- Cross-platform via Cosmopolitan Libc
- Limitation: Windows has 4 GB executable size limit
- Source: https://mozilla-ai.github.io/llamafile/

**Ollama**:
- Downloads models on first `ollama run <model>` invocation
- Stores in `~/.ollama/models/` (layered blob format, not raw GGUF)
- Air-gapped: tar the `~/.ollama/` directory, transfer via USB, extract on target
- Source: https://markaicode.com/ollama-offline-installation-guide/

**LM Studio**:
- Downloads from Hugging Face Hub via built-in browser
- Supports registering pre-downloaded GGUF files without re-downloading
- Source: https://apxml.com/courses/getting-started-local-llms/chapter-4-running-first-local-llm/downloading-models-lm-studio

**Llamaste approach** (recommended):
- Closest to Ollama's strategy but simpler
- No layered blob format -- just raw GGUF files on ext4
- Download via built-in HTTP client (we already have httplib from llama-server)
- `huggingface.co` CDN for downloads (no custom infrastructure)
- USB sideload as first-class alternative
- No model bundling in the image (unlike llamafile) -- keeps image small

---

## 5. YouTube & Tutorial Resources

### 5.1 DigiKey Embedded Linux with Buildroot Series

A 6-part video series by Shawn Hymel, published September 2021:

| Part | Title | Focus |
|------|-------|-------|
| 1 | Introduction to Embedded Linux - Buildroot | Build custom image for STM32MP157D-DK1 |
| 2 | Yocto Project | Alternative build system |
| 3 | Flash SD Card and Boot Process | Flashing and boot sequence |
| 4 | Yocto Custom Image and Layer | Custom layers |
| 5 | (not found in results) | |
| 6 | Add Custom Application in Yocto | Application integration |

**Part 1 video**: https://www.digikey.com/en/videos/d/digi-key-electronics/introduction-to-embedded-linux-part-1-buildroot-digi-key-electronics
**Written tutorial**: https://www.digikey.com/en/maker/projects/intro-to-embedded-linux-part-1-buildroot/a73a56de62444610a2187cd9e681c3f2

Key takeaway: Buildroot is the easier path for creating minimal images. The tutorial demonstrates the full workflow from `make menuconfig` to flashing an SD card.

### 5.2 Bootlin Training Materials (Free Slides, Paid Video)

Bootlin offers the most comprehensive Buildroot training course (3 days, by Thomas Petazzoni who has 5000+ Buildroot patches merged). Training materials are freely available:

- **Slides PDF**: https://bootlin.com/doc/training/buildroot/buildroot-slides.pdf
- **Lab exercises**: https://bootlin.com/doc/training/buildroot/buildroot-labs.pdf
- **Training page**: https://bootlin.com/training/buildroot/

The live training is paid, but the slides cover everything: external trees, post-build scripts, genimage, kernel configuration, package creation, and more.

### 5.3 blinry's "Building a Tiny Linux from Scratch"

A hands-on blog post (and associated video) building a minimal Linux that boots in QEMU:

- **Blog**: https://blinry.org/tiny-linux/
- **Video**: https://www.youtube.com/watch?v=Fm5Ust7vEhk (referenced from Hacker News)

Key details:
- Used `make tinyconfig` for a 0.5 MB kernel (compiled in 19 seconds)
- Custom Rust init binary (1.2 MB statically linked)
- Total system: 2.5 MB (kernel + initrd)
- QEMU test: `qemu-system-x86_64 -kernel bzImage -initrd initrd`
- UEFI boot via OVMF: `qemu-system-x86_64 -bios OVMF.fd -hda fat:rw:hda`

### 5.4 Minimal Linux Live (MLL) Project

Educational distribution built from scratch via shell scripts:

- **GitHub**: https://github.com/ivandavidov/minimal
- **Website**: https://ivandavidov.github.io/minimal/
- **Build**: `./build_minimal_linux_live.sh` (resolves deps, compiles kernel+busybox+glibc)
- Produces a hybrid ISO (BIOS+UEFI) bootable from USB
- ISO structure: `boot/kernel.xz`, `boot/rootfs.xz`, `boot/syslinux/`
- Uses initramfs with `/init` shell script
- Build deps: `wget make gawk gcc bc bison flex xorriso libelf-dev libssl-dev`

### 5.5 Other Relevant Tutorials

| Resource | URL | Relevance |
|----------|-----|-----------|
| Compiling kernel + creating bootable ISO | https://medium.com/@ThyCrow/compiling-the-linux-kernel-and-creating-a-bootable-iso-from-it-6afb8d23ba22 | grub-mkrescue ISO from custom kernel |
| QEMU kernel testing guide (July 2023) | https://radiki.dev/posts/qemu-setup-for-kernel-dev-1/ | Port forwarding, disk images, KVM |
| QEMU direct Linux boot (official docs) | https://qemu-project.gitlab.io/qemu/system/linuxboot.html | -kernel, -append, -initrd reference |
| Kernel debugging with GDB in QEMU | https://nickdesaulniers.github.io/blog/2018/10/24/booting-a-custom-linux-kernel-in-qemu-and-debugging-it-with-gdb/ | -s -S flags, hbreak, lx-dmesg |
| Embedded Linux from scratch on QEMU | https://medium.com/@fprotopapa/embedded-linux-from-scratch-quick-easy-on-qemu-87e761834b51 | Cross-compilation, ARM on x86_64 |
| Handbuilt Linux (GitHub) | https://github.com/ehsanghaffar/handbuilt-linux | Kernel+BusyBox+Syslinux minimal distro |
| Build minimal Linux + boot in QEMU | http://www.kaizou.org/2016/09/boot-minimal-linux-qemu.html | Step-by-step embedded Linux |

---

## 6. Llamaste-Specific Recommendations

### 6.1 Boot Architecture Summary

```
                    +--[ GRUB Menu ]--+
                    |                 |
              [Server Mode]     [Desktop Mode]
                    |                 |
                    v                 v
              kernel + init=      kernel + init=
              /opt/llamaste       /opt/llamaste
              console=ttyS0       video=...
                    |                 |
                    +--------+--------+
                             |
                    [ llamaste PID 1 ]
                             |
                    mount /proc, /sys, /tmp, /run
                    mount /dev/pts, /dev/shm
                    mount DATA partition -> /data
                    setup overlayfs (squashfs + /data/overlay)
                    check first boot -> resize DATA, download model
                    start HTTP server on :80
                    start inference engine
                             |
                    [ READY - serving on port 80 ]
```

### 6.2 Image Build Pipeline

```
Buildroot
  |
  +-- Kernel (bzImage, ~5-8 MB, all drivers built-in, no initramfs)
  +-- llamaste binary (static, musl, ~15-20 MB)
  +-- rootfs overlay (config files, directory structure)
  |
  v
Post-build script
  |
  +-- Apply rootfs overlay
  +-- Set permissions
  |
  v
Filesystem generation
  |
  +-- rootfs.squashfs (zstd compressed, ~100-200 MB)
  +-- data.ext4 (minimal, 64 MB seed, grows on first boot)
  |
  v
Post-image script
  |
  +-- Build GRUB images (i386-pc + x86_64-efi)
  +-- Build EFI vfat image
  +-- Run genimage -> llamaste.img (raw disk image)
  +-- Run grub-mkrescue -> llamaste.iso (hybrid ISO)
  +-- Compress: xz -9 llamaste.img -> llamaste.img.xz
  +-- Generate SHA256SUMS
```

### 6.3 Kernel Config Essentials (Boot-Related)

```
# No initramfs
CONFIG_BLK_DEV_INITRD=n

# Device management
CONFIG_DEVTMPFS=y
CONFIG_DEVTMPFS_MOUNT=y

# Filesystems (built-in, not modules)
CONFIG_SQUASHFS=y
CONFIG_SQUASHFS_ZSTD=y
CONFIG_EXT4_FS=y
CONFIG_VFAT_FS=y
CONFIG_OVERLAY_FS=y
CONFIG_TMPFS=y
CONFIG_PROC_FS=y
CONFIG_SYSFS=y

# Storage controllers (built-in)
CONFIG_ATA=y
CONFIG_SATA_AHCI=y
CONFIG_BLK_DEV_NVME=y
CONFIG_BLK_DEV_SD=y
CONFIG_USB_STORAGE=y

# Virtio (for QEMU testing)
CONFIG_VIRTIO=y
CONFIG_VIRTIO_PCI=y
CONFIG_VIRTIO_BLK=y
CONFIG_VIRTIO_NET=y

# Console
CONFIG_VT=y
CONFIG_SERIAL_8250=y
CONFIG_SERIAL_8250_CONSOLE=y

# Network (built-in for kernel-level DHCP)
CONFIG_NET=y
CONFIG_INET=y
CONFIG_IP_PNP=y
CONFIG_IP_PNP_DHCP=y

# EFI
CONFIG_EFI=y
CONFIG_EFI_STUB=y
```

### 6.4 genimage.cfg: Complete 5-Partition Layout

See section 3.3 above for the full configuration. Key points:
- Partition 1 (bios-boot): 1 MB, raw GRUB core.img for BIOS
- Partition 2 (ESP): 256 MB, FAT32, GRUB EFI binary
- Partition 3 (system-a): 256 MB, squashfs, active root
- Partition 4 (system-b): 256 MB, empty (future A/B updates)
- Partition 5 (data): autoresize, ext4, user data + models

### 6.5 Distribution Strategy

| Format | File | Size | Use Case |
|--------|------|------|----------|
| Raw disk image | `llamaste-x86_64.img.xz` | ~300-400 MB | Primary: dd to disk/USB |
| Hybrid ISO | `llamaste-x86_64.iso` | ~400-500 MB | Ventoy, optical media, VMs |
| Checksum | `SHA256SUMS` | tiny | Verification |

**User workflow**:
1. Download `llamaste-x86_64.img.xz`
2. `xz -d llamaste-x86_64.img.xz`
3. `sudo dd if=llamaste-x86_64.img of=/dev/sdX bs=4M status=progress`
4. Boot machine -> first-boot wizard at http://llamaste.local
5. Select model -> download begins -> operational in minutes

### 6.6 Trade-Off Summary

| Decision | Choice | Rationale |
|----------|--------|-----------|
| initramfs | Skip entirely | All drivers built-in; saves 100-500ms; simpler pipeline |
| Device management | devtmpfs only (no udev) | Sufficient for our needs; zero userspace daemon |
| Distribution format | dd-to-disk (primary) + ISO (secondary) | Appliance model; simplest for users |
| Model delivery | Download on first boot + USB sideload | Keeps image small; works online and offline |
| Ventoy compatibility | Standard hybrid ISO + `ventoy.dat` marker | Works out of the box with label-based root |
| ISO creation | grub-mkrescue (primary), xorriso (fallback) | grub-mkrescue handles BIOS+UEFI automatically |
| Disk image creation | genimage via Buildroot post-image script | Standard Buildroot workflow; declarative config |
| QEMU testing | Direct kernel boot for dev; full image boot for integration | Fast iteration + comprehensive testing |
