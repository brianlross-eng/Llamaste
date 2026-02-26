# Buildroot: Custom Minimal Linux Image Creation

## Overview

Buildroot is a collection of Makefiles and patches that automates building a complete, bootable Linux system entirely from source. It is not a distribution -- it is a build system that produces one.

## Build Pipeline

1. **Toolchain** -- Builds or configures a cross-compilation toolchain (GCC, binutils, C library: musl/uClibc-ng/glibc)
2. **Packages** -- Downloads, patches, configures, compiles, installs each selected package
3. **Linux Kernel** -- Compiles with specified configuration
4. **Bootloader** -- Optionally builds GRUB2, syslinux, U-Boot, etc.
5. **Root Filesystem Assembly** -- Applies overlays, runs post-build scripts, generates images

## Configuration System (Kconfig)

Uses the same Kconfig system as the Linux kernel:

```bash
make menuconfig          # ncurses-based terminal UI
make nconfig             # newer ncurses interface
make savedefconfig BR2_DEFCONFIG=configs/my_defconfig
```

Top-level categories:
- **Target options** -- CPU architecture, endianness, ABI
- **Toolchain** -- Internal vs external, C library, C++ support, kernel headers
- **System configuration** -- Hostname, init system, /dev management, overlays, scripts
- **Kernel** -- Version, defconfig, fragments, patches
- **Target packages** -- ~2800+ packages
- **Filesystem images** -- ext4, squashfs, cpio, tar, ISO 9660, etc.
- **Bootloader** -- GRUB2, U-Boot, syslinux, etc.

## Output Artifacts

```
output/
  build/     # Compiled source trees
  host/      # Host tools and cross-compilation toolchain
  staging/   # Sysroot (headers/libraries for cross-compiling)
  target/    # Assembled target root filesystem
  images/    # Final deliverables: kernel, bootloader, filesystem images
```

## BR2_EXTERNAL Mechanism (Custom Packages)

The correct way to add custom packages without modifying the Buildroot tree:

```
my_br2_external/
├── Config.in              # Sources package Config.in files
├── external.mk            # Includes package .mk files
├── external.desc          # Name and description
├── configs/               # Saved defconfigs
├── package/
│   └── llamaste/
│       ├── Config.in      # Kconfig menu entry
│       ├── llamaste.mk    # Build recipe
│       └── llamaste.hash  # SHA256 hashes
├── board/
│   └── llamaste/
│       ├── overlay/       # Filesystem overlay
│       ├── linux.config   # Custom kernel config
│       ├── genimage.cfg   # Disk image layout
│       ├── post_build.sh  # Runs after rootfs assembly
│       └── post_image.sh  # Runs after image generation
```

### CMake-Based Package Recipe

```makefile
LLAMASTE_VERSION = 1.0
LLAMASTE_SITE = $(BR2_EXTERNAL_MY_EXTERNAL_PATH)/src/llamaste
LLAMASTE_SITE_METHOD = local
LLAMASTE_INSTALL_TARGET = YES

LLAMASTE_CONF_OPTS = \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_EXE_LINKER_FLAGS="-static" \
    -DCMAKE_FIND_LIBRARY_SUFFIXES=".a"

$(eval $(cmake-package))
```

## Init System: BusyBox Init (Recommended for Llamaste)

| Init System | Size Impact | Boot Speed | Best For |
|-------------|-------------|------------|----------|
| BusyBox init | Tiny (few KB) | Very fast | Single-purpose appliances |
| SysVinit | Small | Fast | Traditional systems |
| systemd | Large (10-15+ MB) | Moderate | Complex service management |

### BusyBox Init Boot Sequence

1. Kernel starts `/sbin/init` (BusyBox)
2. Reads `/etc/inittab`
3. `sysinit` runs `/etc/init.d/rcS`
4. `rcS` executes `/etc/init.d/S*` scripts in order
5. Respawn entries (getty) start

### Example Init Script (S90llama)

```bash
#!/bin/sh
DAEMON=/usr/bin/llama-server
PIDFILE=/var/run/llama.pid

case "$1" in
    start)
        printf "Starting llama-server: "
        echo 1024 > /proc/sys/vm/nr_hugepages
        start-stop-daemon -S -b -m -p $PIDFILE \
            --exec $DAEMON -- -m /data/models/default.gguf \
            --host 0.0.0.0 --port 8080
        echo "OK"
        ;;
    stop)
        printf "Stopping llama-server: "
        start-stop-daemon -K -p $PIDFILE
        echo "OK"
        ;;
    restart) $0 stop; $0 start ;;
    *) echo "Usage: $0 {start|stop|restart}"; exit 1 ;;
esac
```

## Kernel Configuration for Llamaste

### Huge Pages
```
CONFIG_HUGETLBFS=y
CONFIG_HUGETLB_PAGE=y
CONFIG_TRANSPARENT_HUGEPAGE=y
CONFIG_TRANSPARENT_HUGEPAGE_ALWAYS=y
```

### Network-Only Minimal Kernel
```
# Boot essentials
CONFIG_64BIT=y
CONFIG_SMP=y
CONFIG_DEVTMPFS=y
CONFIG_DEVTMPFS_MOUNT=y
CONFIG_PROC_FS=y
CONFIG_SYSFS=y
CONFIG_TMPFS=y
CONFIG_EXT4_FS=y

# Networking
CONFIG_NET=y
CONFIG_INET=y
CONFIG_NETDEVICES=y
CONFIG_ETHERNET=y

# Disabled
# CONFIG_SOUND is not set
# CONFIG_DRM is not set
# CONFIG_INPUT is not set
# CONFIG_WIRELESS is not set
# CONFIG_BLUETOOTH is not set
# CONFIG_MEDIA_SUPPORT is not set
```

## Disk Image Creation with genimage

```
image boot.vfat {
    vfat { files = { "bzImage", "grub.cfg" } }
    size = 64M
}

image rootfs.squashfs {
    squashfs { compression = "zstd" }
}

image sdcard.img {
    hdimage { gpt = true }
    partition boot {
        partition-type-uuid = C12A7328-F81F-11D2-BA4B-00A0C93EC93B
        bootable = true
        image = boot.vfat
    }
    partition rootfs {
        image = rootfs.squashfs
    }
}
```

## Achievable Image Sizes

| Configuration | Size |
|---------------|------|
| Absolute minimum (BusyBox + musl) | 2-4 MB |
| Minimal bootable (+ trimmed kernel) | 5-10 MB |
| Practical (+ networking + SSH) | 15-25 MB |
| Llamaste target (+ llama.cpp static) | ~15-20 MB |

## Key Minimization Strategies

1. Use **musl** as C library (~600 KB vs glibc ~8-10 MB)
2. Use **BusyBox init** with mdev
3. Strip all binaries
4. Use **squashfs** with zstd compression for read-only root
5. Start kernel from tinyconfig, add only needed drivers
6. Build all drivers statically (no modules)

## Best Practices

1. Use BR2_EXTERNAL tree for all customizations
2. Save defconfigs, not full .config files
3. Pin Buildroot version to a release tag
4. Enable ccache for fast rebuilds
5. Use kernel configuration fragments
6. Test with QEMU first
7. Use `make graph-size` to identify bloat
8. Set BR2_DL_DIR to persistent location outside build tree
9. Never run `make` as root

## Buildroot vs Yocto for Llamaste

| Dimension | Buildroot | Yocto |
|-----------|-----------|-------|
| Learning curve | Low | High |
| First build time | 15-60 min | 1-4+ hours |
| Minimal image size | Smallest | Slightly larger |
| Runtime pkg manager | No | Optional |
| Best fit | **Llamaste PoC** | Complex products |

**Recommendation for Llamaste:** Start with Buildroot for the PoC and early phases. It is simpler, faster to iterate, and produces smaller images. Consider Yocto only if long-term maintenance and OTA update requirements grow significantly.
