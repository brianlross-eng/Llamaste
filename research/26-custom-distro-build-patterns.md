# Custom Linux Distro Building Patterns for Llamaste

## Purpose

Deep-dive into practical Buildroot build workflows, kernel driver selection, reproducible
builds, and patterns from other custom distros. Fills gaps left by research/01-buildroot.md
and research/12-minimal-os-alternatives.md with concrete commands, file contents, and
trade-off analysis.

---

## 1. Buildroot Cross-Compilation Deep Dive

### 1.1 BR2_EXTERNAL Directory Structure

The BR2_EXTERNAL mechanism keeps all Llamaste-specific files **outside** the Buildroot
source tree, making Buildroot upgrades trivial (just update the Buildroot checkout).

Required layout for Llamaste:

```
llamaste-br2-external/
  external.desc              # REQUIRED: name + description
  Config.in                  # REQUIRED: sources package Config.in files
  external.mk                # REQUIRED: includes package .mk files
  configs/
    llamaste_defconfig       # Our board defconfig
  package/
    llamaste/
      Config.in              # Kconfig entry for the llamaste package
      llamaste.mk            # CMake package recipe
      llamaste.hash          # SHA256 of source tarball (or skip for local)
    llama-cpp/
      Config.in
      llama-cpp.mk
      llama-cpp.hash
  board/
    llamaste/
      linux.config           # Full kernel .config or fragment
      linux-extra.config     # Kernel config fragment (NIC/storage/display drivers)
      grub.cfg               # GRUB configuration for dual-boot
      rootfs_overlay/        # Files overlaid onto target rootfs
        etc/
          hostname
          inittab             # Not used (no init system) but could hold fallback
        opt/
          models/             # Placeholder directory for GGUF models
      post_build.sh          # Runs after rootfs assembly
      post_image.sh          # Runs after image generation (create disk image)
      genimage.cfg            # genimage configuration for GPT partitioning
```

#### external.desc

```
name: LLAMASTE
desc: Llamaste - LLM IS the OS
```

This creates the variable `$(BR2_EXTERNAL_LLAMASTE_PATH)` usable in all Makefiles and
Kconfig files. Also exported to the environment for post-build/post-image scripts.

#### Config.in

```kconfig
source "$BR2_EXTERNAL_LLAMASTE_PATH/package/llama-cpp/Config.in"
source "$BR2_EXTERNAL_LLAMASTE_PATH/package/llamaste/Config.in"
```

#### external.mk

```makefile
include $(sort $(wildcard $(BR2_EXTERNAL_LLAMASTE_PATH)/package/*/*.mk))
```

#### Activation (one-time)

```bash
# From within the Buildroot directory:
make BR2_EXTERNAL=/path/to/llamaste-br2-external menuconfig
# This writes .br2-external.mk so subsequent `make` commands remember it
```

Source: [Buildroot Manual](https://buildroot.org/downloads/manual/manual.html),
[Microchip BR2_EXTERNAL Tutorial](https://developerhelp.microchip.com/xwiki/bin/view/software-tools/linux/buildroot-custom-project/)

### 1.2 Custom CMake Package: llama-cpp.mk

Buildroot provides `cmake-package` infrastructure. Here is the exact `.mk` file for
integrating llama.cpp:

```makefile
################################################################################
#
# llama-cpp
#
################################################################################

LLAMA_CPP_VERSION = b4654
LLAMA_CPP_SITE = https://github.com/ggml-org/llama.cpp/archive/refs/tags
LLAMA_CPP_SOURCE = $(LLAMA_CPP_VERSION).tar.gz
LLAMA_CPP_LICENSE = MIT
LLAMA_CPP_LICENSE_FILES = LICENSE
LLAMA_CPP_INSTALL_STAGING = YES
LLAMA_CPP_INSTALL_TARGET = YES
LLAMA_CPP_SUPPORTS_IN_SOURCE_BUILD = NO

LLAMA_CPP_CONF_OPTS = \
    -DBUILD_SHARED_LIBS=OFF \
    -DGGML_NATIVE=ON \
    -DGGML_CPU_ALL_VARIANTS=OFF \
    -DGGML_OPENMP=ON \
    -DLLAMA_BUILD_TESTS=OFF \
    -DLLAMA_BUILD_EXAMPLES=OFF \
    -DLLAMA_BUILD_SERVER=ON \
    -DLLAMA_CURL=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON

# llama.cpp has no external dependencies when LLAMA_CURL=OFF
# OpenMP is provided by the toolchain

$(eval $(cmake-package))
```

#### Config.in for llama-cpp

```kconfig
config BR2_PACKAGE_LLAMA_CPP
    bool "llama-cpp"
    depends on BR2_INSTALL_LIBSTDCPP
    depends on BR2_TOOLCHAIN_HAS_THREADS
    help
      LLM inference engine in C/C++.
      https://github.com/ggml-org/llama.cpp
```

### 1.3 Custom CMake Package: llamaste.mk

The main Llamaste binary depends on llama-cpp (uses it as a library):

```makefile
################################################################################
#
# llamaste
#
################################################################################

LLAMASTE_VERSION = 0.1.0
LLAMASTE_SITE = $(BR2_EXTERNAL_LLAMASTE_PATH)/src
LLAMASTE_SITE_METHOD = local
LLAMASTE_LICENSE = Apache-2.0
LLAMASTE_INSTALL_TARGET = YES
LLAMASTE_DEPENDENCIES = llama-cpp

LLAMASTE_CONF_OPTS = \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_EXE_LINKER_FLAGS="-static" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON

# Install to /opt/llamaste (our init binary location)
define LLAMASTE_INSTALL_TARGET_CMDS
    $(INSTALL) -D -m 0755 $(@D)/llamaste $(TARGET_DIR)/opt/llamaste
endef

$(eval $(cmake-package))
```

Notes:
- `SITE_METHOD = local` means source lives in our BR2_EXTERNAL tree, not downloaded.
- `LLAMASTE_DEPENDENCIES = llama-cpp` ensures llama-cpp is built first and its headers/
  libraries are in staging.
- The custom `INSTALL_TARGET_CMDS` places the binary at `/opt/llamaste` since this
  is what `init=/opt/llamaste` expects on the kernel command line.

Source: [Buildroot CMake Package Docs](https://www.buildroot.org/downloads/manual/adding-packages-cmake.txt),
[Bootlin Adding Packages](https://bootlin.com/~thomas/site/buildroot/adding-packages.html)

### 1.4 musl Toolchain Configuration

In `menuconfig` under Toolchain:

```
BR2_TOOLCHAIN_BUILDROOT_MUSL=y       # Use musl (not glibc, not uClibc-ng)
BR2_TOOLCHAIN_BUILDROOT_CXX=y        # Enable C++ (required for llama.cpp)
BR2_STATIC_LIBS=y                     # Build only static libraries system-wide
BR2_PACKAGE_HOST_LINUX_HEADERS_CUSTOM_6_6=y  # Match kernel version
```

In defconfig form:

```
BR2_x86_64=y
BR2_x86_corei7=y
BR2_TOOLCHAIN_BUILDROOT_MUSL=y
BR2_TOOLCHAIN_BUILDROOT_CXX=y
BR2_STATIC_LIBS=y
```

### 1.5 Static Linking Gotchas with musl + CMake

| Issue | Symptom | Fix |
|-------|---------|-----|
| Missing `-ldl -lrt -lpthread` | `undefined reference to dlopen` | musl bundles all in libc.a; pass `-static` to linker |
| `__dso_handle` undefined | Linking libstdc++.a against musl | Ensure entire toolchain is musl-based (Buildroot handles this) |
| `limits.h` not found | System headers leaking in | Use Buildroot's cross-compiler, never host gcc |
| `GGML_CPU_ALL_VARIANTS=ON` | Produces .so shared objects | Use `GGML_NATIVE=ON` + `GGML_CPU_ALL_VARIANTS=OFF` |
| CMake finds host libraries | Links wrong libc | Buildroot sets CMAKE_TOOLCHAIN_FILE automatically |
| C++ exceptions | `__cxa_throw` bloat | `-fno-exceptions -fno-rtti` works with llama.cpp, saves ~200KB |

Key insight: Buildroot's `cmake-package` infrastructure automatically sets the cross-
compilation toolchain, sysroot, and CMAKE_FIND_ROOT_PATH. You do NOT need to manually
configure these -- the infrastructure handles it.

musl produces significantly smaller static binaries -- nearly 6.5x smaller than glibc
static builds for the same code.

Source: [musl-libc.org](https://www.musl-libc.org/how.html),
[Static linking with musl](https://radupopescu.net/tech/static_linking_for_cpp/)

### 1.6 Output Directory Structure

After `make`, the output directory contains:

```
output/
  build/           # Per-package build directories (source + objects)
    llama-cpp-b4654/
      .stamp_downloaded
      .stamp_extracted
      .stamp_patched
      .stamp_configured
      .stamp_built
      .stamp_staging_installed
      .stamp_target_installed
    llamaste-0.1.0/
      ...
    linux-6.6.x/
      ...
  host/            # Cross-compiler + host tools
    bin/            # x86_64-buildroot-linux-musl-gcc, cmake, etc.
    x86_64-buildroot-linux-musl/
      sysroot/      # Target headers + static libraries
  staging/          # Symlink to host/<tuple>/sysroot
  target/           # Root filesystem tree (stripped binaries)
    opt/
      llamaste      # Our static binary
    lib/             # Minimal (mostly empty with static builds)
    etc/
    dev/
    proc/
    sys/
  images/           # Final output: kernel, rootfs, disk images
    bzImage
    rootfs.squashfs  # (or rootfs.ext4, rootfs.cpio.gz)
    disk.img         # If genimage is used
```

---

## 2. Incremental Rebuild Workflow

### 2.1 Package-Level Rebuild Commands

```bash
# Rebuild just llamaste (recompile + reinstall, no re-extract)
make llamaste-rebuild

# Rebuild llamaste and regenerate final images
make llamaste-rebuild all

# Full clean + rebuild of llamaste only
make llamaste-dirclean
make llamaste

# Reconfigure (re-run cmake) + rebuild
make llamaste-reconfigure
```

Stamp files in `output/build/llamaste-0.1.0/` control what gets re-run:
- `.stamp_downloaded` -> `.stamp_extracted` -> `.stamp_patched` ->
  `.stamp_configured` -> `.stamp_built` -> `.stamp_staging_installed` ->
  `.stamp_target_installed`

`make llamaste-rebuild` deletes `.stamp_built` onward and re-executes those steps.

### 2.2 OVERRIDE_SRCDIR for Active Development

Create `local.mk` in the Buildroot top directory (or wherever `BR2_PACKAGE_OVERRIDE_FILE`
points):

```makefile
# Point to local source directory instead of downloading/extracting
LLAMASTE_OVERRIDE_SRCDIR = /home/user/llamaste/src
LLAMA_CPP_OVERRIDE_SRCDIR = /home/user/llama.cpp
```

Then the development loop becomes:

```bash
# Edit source in /home/user/llamaste/src/...
# Rebuild (rsync + compile + install + image generation):
make llamaste-rebuild all
```

Rsync only copies changed files, making this very fast for iterative development.
This is THE recommended workflow for active package development in Buildroot.

### 2.3 ccache Integration

Enable in menuconfig: `Build options -> Enable compiler cache (BR2_CCACHE)`

```
BR2_CCACHE=y
BR2_CCACHE_DIR="/home/user/.buildroot-ccache"
BR2_CCACHE_USE_BASEDIR=y        # Relative paths for cache hits across output dirs
```

Performance impact (from community benchmarks):
- **Full build from clean**: ~37 minutes
- **Full rebuild with warm ccache**: ~5 minutes 24 seconds (85% reduction)

ccache survives `make clean` and `make distclean`. Delete manually if needed:
```bash
rm -rf ~/.buildroot-ccache
```

Statistics: `make ccache-stats`
Set cache size: `make CCACHE_OPTIONS="--max-size=10G" ccache-options`

### 2.4 Per-Package Directories for Parallelism

```
BR2_PER_PACKAGE_DIRECTORIES=y   # Experimental but well-tested
```

This enables each package to have its own staging directory, providing true build isolation.
Benefits:
- `make -jN` works effectively at the top level
- Clean dependency tracking (a package only sees its declared dependencies)
- Catches missing dependency declarations

### 2.5 Out-of-Tree Builds

```bash
# Build in a different directory (keeps source tree pristine)
make O=/home/user/buildroot-output BR2_EXTERNAL=/path/to/llamaste-br2-external menuconfig
make O=/home/user/buildroot-output
```

With `BR2_CCACHE_USE_BASEDIR=y`, switching between output directories does NOT cause
cache misses.

### 2.6 menuconfig vs Editing defconfig

| Approach | When to Use |
|----------|-------------|
| `make menuconfig` | Exploring options, initial config, complex changes |
| Edit defconfig directly | CI/CD, scripted changes, well-understood options |
| `make savedefconfig` | After menuconfig, to persist minimal config |
| `make linux-menuconfig` | Kernel-specific config changes |
| `make linux-savedefconfig` + `make linux-update-defconfig` | Persist kernel config |

**Always** run `make savedefconfig` after using menuconfig. The `.config` file is verbose
(~8000 lines); the defconfig only stores non-default values (~50-200 lines).

Source: [Buildroot Manual - ccache](https://buildroot.org/downloads/manual/manual.html),
[Speeding Up Buildroot](https://www.nayab.dev/linux/buildroot/speed-up-buildroot-build.html),
[Buildroot Cheatsheet](https://blog.inf.re/buildroot-cheatsheet.html)

---

## 3. Kernel Driver Selection

### 3.1 Strategy for Broad Hardware Support

Llamaste targets "any x86-64 machine" -- from old desktops to modern NUCs. This requires
a kernel that supports common NICs, storage controllers, and display hardware **built-in**
(not as modules), because we have no initramfs module-loading infrastructure.

Our approach:
1. Start with `x86_64_defconfig` (the kernel's default for the arch)
2. Apply a kernel config fragment that forces critical drivers to `=y`
3. Use `make linux-menuconfig` for any remaining tweaks
4. Save with `make linux-update-defconfig`

### 3.2 Required Drivers (Built-In)

#### Network Interface Cards

```kconfig
# Intel NICs (covers ~70% of desktops, NUCs, servers)
CONFIG_E1000=y          # Intel PRO/1000 PCI (legacy, pre-2005)
CONFIG_E1000E=y         # Intel PRO/1000 PCI-E (2005-present, most Intel NUCs)
CONFIG_IGB=y            # Intel I350, I210/I211 (servers, some desktops)
CONFIG_IGBVF=y          # Intel 82576 Virtual Function
CONFIG_IXGBE=y          # Intel 10GbE (servers)
CONFIG_I40E=y           # Intel Ethernet Controller X710/XL710

# Realtek NICs (covers ~25% of consumer hardware)
CONFIG_R8169=y          # RTL8169/8168/8101/8125 (extremely common)

# Broadcom NICs (some servers, some laptops)
CONFIG_BNX2=y           # Broadcom NetXtreme II
CONFIG_TIGON3=y         # Broadcom Tigon3 (tg3)

# Virtual/QEMU (for testing)
CONFIG_VIRTIO_NET=y     # virtio networking (QEMU/KVM)
CONFIG_E1000=y          # Default QEMU NIC model

# Common wireless (Phase 2, optional for Phase 1)
# CONFIG_IWLWIFI=y      # Intel WiFi (requires firmware blobs)
```

#### Storage Controllers

```kconfig
# SATA/AHCI (most desktops/laptops 2005-present)
CONFIG_ATA=y
CONFIG_SATA_AHCI=y          # AHCI SATA controller (covers ~90% of SATA)
CONFIG_ATA_PIIX=y           # Intel PIIX/ICH SATA (legacy, pre-AHCI)

# NVMe (modern systems 2015+)
CONFIG_BLK_DEV_NVME=y       # NVMe SSD support
CONFIG_NVME_CORE=y

# USB Mass Storage (USB drives, installers)
CONFIG_USB=y
CONFIG_USB_STORAGE=y
CONFIG_USB_EHCI_HCD=y       # USB 2.0
CONFIG_USB_XHCI_HCD=y       # USB 3.0
CONFIG_USB_OHCI_HCD=y       # USB 1.1 (legacy)

# SCSI subsystem (required by AHCI and USB storage)
CONFIG_SCSI=y
CONFIG_BLK_DEV_SD=y         # SCSI disk support
CONFIG_CHR_DEV_SG=y         # SCSI generic

# Virtual (QEMU testing)
CONFIG_VIRTIO_BLK=y
CONFIG_VIRTIO_PCI=y
```

#### Display Drivers (Desktop Mode)

```kconfig
# DRM subsystem
CONFIG_DRM=y

# simpledrm (early boot framebuffer -- CRITICAL for universal display)
CONFIG_DRM_SIMPLEDRM=y      # Uses firmware-provided framebuffer (UEFI GOP)
CONFIG_SYSFB_SIMPLEFB=y     # System framebuffer platform device

# Intel integrated graphics (most common x86 desktop GPU)
CONFIG_DRM_I915=y           # Intel i915 (HD Graphics, Iris, UHD)

# AMD/ATI (second most common)
CONFIG_DRM_AMDGPU=y         # AMD GPU (GCN+, Radeon RX, Ryzen APUs)
CONFIG_DRM_RADEON=y         # Older AMD/ATI GPUs (pre-GCN)

# NVIDIA (open-source nouveau driver -- basic 2D/3D)
CONFIG_DRM_NOUVEAU=y        # NVIDIA open-source driver

# Framebuffer console (essential for early boot + tty)
CONFIG_FRAMEBUFFER_CONSOLE=y
CONFIG_FRAMEBUFFER_CONSOLE_DETECT_PRIMARY=y
CONFIG_FB=y
CONFIG_FB_EFI=y             # EFI framebuffer (UEFI boot)
CONFIG_FB_VESA=y            # VESA framebuffer (BIOS boot fallback)
```

#### Input Drivers

```kconfig
CONFIG_INPUT=y
CONFIG_INPUT_EVDEV=y        # Event device interface (standard input layer)
CONFIG_INPUT_KEYBOARD=y
CONFIG_KEYBOARD_ATKBD=y     # AT keyboard (PS/2, most laptops)
CONFIG_INPUT_MOUSE=y
CONFIG_MOUSE_PS2=y          # PS/2 mouse
CONFIG_HID=y
CONFIG_USB_HID=y            # USB HID (keyboards, mice)
CONFIG_I2C_HID=y            # I2C HID (touchpads on modern laptops)
CONFIG_HID_GENERIC=y
```

### 3.3 Kernel Config Fragment File

Create `board/llamaste/linux-extra.config`:

```
# --- Llamaste Kernel Config Fragment ---
# Applied on top of x86_64_defconfig via BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES

# Force-enable critical drivers as built-in (=y not =m)
# NIC
CONFIG_E1000=y
CONFIG_E1000E=y
CONFIG_IGB=y
CONFIG_R8169=y
CONFIG_VIRTIO_NET=y

# Storage
CONFIG_ATA=y
CONFIG_SATA_AHCI=y
CONFIG_BLK_DEV_NVME=y
CONFIG_NVME_CORE=y
CONFIG_USB=y
CONFIG_USB_STORAGE=y
CONFIG_USB_XHCI_HCD=y
CONFIG_USB_EHCI_HCD=y
CONFIG_SCSI=y
CONFIG_BLK_DEV_SD=y
CONFIG_VIRTIO_BLK=y
CONFIG_VIRTIO_PCI=y

# Display
CONFIG_DRM=y
CONFIG_DRM_SIMPLEDRM=y
CONFIG_DRM_I915=y
CONFIG_DRM_AMDGPU=y
CONFIG_DRM_NOUVEAU=y
CONFIG_FRAMEBUFFER_CONSOLE=y
CONFIG_FB=y
CONFIG_FB_EFI=y

# Input
CONFIG_INPUT_EVDEV=y
CONFIG_USB_HID=y
CONFIG_HID_GENERIC=y

# Networking stack
CONFIG_NET=y
CONFIG_INET=y
CONFIG_IPV6=y
CONFIG_PACKET=y

# Required for PID 1 binary
CONFIG_BINFMT_ELF=y
CONFIG_PROC_FS=y
CONFIG_SYSFS=y
CONFIG_TMPFS=y
CONFIG_DEVTMPFS=y
CONFIG_DEVTMPFS_MOUNT=y

# Disable modules (everything built-in, no module loading infrastructure)
# CONFIG_MODULES is not set
```

Reference this in the Buildroot defconfig:

```
BR2_LINUX_KERNEL_USE_DEFCONFIG=y
BR2_LINUX_KERNEL_DEFCONFIG="x86_64"
BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES="$(BR2_EXTERNAL_LLAMASTE_PATH)/board/llamaste/linux-extra.config"
```

### 3.4 Why Built-In Instead of Modules

For Llamaste specifically:
1. **No module-loading infrastructure** -- No BusyBox, no `modprobe`, no `kmod`
2. **Faster boot** -- No module loading latency
3. **Simpler rootfs** -- No `/lib/modules/` directory needed
4. **No initramfs needed** -- Kernel can mount root directly if storage drivers are built-in
5. **Smaller attack surface** -- Cannot load arbitrary kernel code at runtime

Trade-off: Larger kernel image (~15-25MB vs ~5-8MB), but this is negligible compared to
the ~500MB-2GB model files.

### 3.5 Firmware Blobs

Some drivers require firmware blobs from linux-firmware:
- **Intel WiFi** (iwlwifi) -- requires firmware (Phase 2)
- **AMD GPU** (amdgpu) -- requires firmware for modern cards
- **Intel GPU** (i915) -- some recent generations need firmware

For Phase 1 (headless server mode), firmware is optional. For Desktop mode, we need to
include `linux-firmware` in the Buildroot config:

```
BR2_PACKAGE_LINUX_FIRMWARE=y
BR2_PACKAGE_LINUX_FIRMWARE_INTEL_I915=y     # Intel GPU firmware
BR2_PACKAGE_LINUX_FIRMWARE_AMDGPU=y         # AMD GPU firmware
```

Source: [Arch Wiki KMS](https://wiki.archlinux.org/title/Kernel_mode_setting),
[Gentoo Kernel Config](https://wiki.gentoo.org/wiki/Kernel/Configuration),
[kernelconfig.io](https://www.kernelconfig.io/config_r8169)

---

## 4. Reproducible Builds

### 4.1 Buildroot BR2_REPRODUCIBLE

```
BR2_REPRODUCIBLE=y   # Experimental but functional
```

When enabled:
- Exports `TZ=UTC`, `LANG=C`, `LC_ALL=C` for deterministic locale/timezone
- Sets `SOURCE_DATE_EPOCH` from git commit timestamp or release date
- Ensures two builds of the same config produce identical binaries
- Current limitation: must use same output directory path

### 4.2 Version Pinning Strategy

```
# Pin Buildroot itself to a release tag
git clone https://gitlab.com/buildroot.org/buildroot.git
cd buildroot
git checkout 2024.11.3   # Specific release, not master

# Pin kernel version in defconfig
BR2_LINUX_KERNEL_CUSTOM_VERSION=y
BR2_LINUX_KERNEL_CUSTOM_VERSION_VALUE="6.6.72"  # LTS version

# Pin toolchain via Buildroot version (internal toolchain versions are fixed per release)
# GCC, binutils, musl versions are determined by the Buildroot release
```

### 4.3 Hash Checking

Every package in Buildroot has (or should have) a `.hash` file:

```
# llama-cpp.hash
sha256  <hash_of_tarball>  b4654.tar.gz
```

For local source packages (SITE_METHOD=local), hash checking is skipped since the source
is part of our repository.

### 4.4 Dockerfile for Build Environment

```dockerfile
FROM ubuntu:24.04

# Avoid interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Install Buildroot dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    gcc g++ \
    libncurses5-dev \
    bc \
    bison flex \
    unzip rsync \
    cpio \
    wget curl \
    git \
    python3 python3-pip \
    file \
    cmake \
    patch \
    perl \
    libssl-dev \
    dosfstools mtools \
    squashfs-tools \
    genimage \
    && rm -rf /var/lib/apt/lists/*

# Create build user (Buildroot refuses to run as root)
RUN useradd -m builder
USER builder
WORKDIR /home/builder

# Clone Buildroot at pinned version
ARG BUILDROOT_VERSION=2024.11.3
RUN git clone --depth 1 --branch ${BUILDROOT_VERSION} \
    https://gitlab.com/buildroot.org/buildroot.git

# Pre-create ccache directory
RUN mkdir -p /home/builder/.buildroot-ccache

VOLUME ["/home/builder/llamaste-br2-external"]
VOLUME ["/home/builder/buildroot-output"]

WORKDIR /home/builder/buildroot

# Default: open a shell
CMD ["/bin/bash"]
```

Build and use:

```bash
# Build the Docker image
docker build -t llamaste-builder --build-arg BUILDROOT_VERSION=2024.11.3 .

# Run with external tree and persistent output
docker run -it --rm \
    -v $(pwd)/llamaste-br2-external:/home/builder/llamaste-br2-external \
    -v buildroot-output:/home/builder/buildroot-output \
    -v buildroot-ccache:/home/builder/.buildroot-ccache \
    llamaste-builder \
    bash -c "make O=/home/builder/buildroot-output \
             BR2_EXTERNAL=/home/builder/llamaste-br2-external \
             llamaste_defconfig && \
             make O=/home/builder/buildroot-output -j$(nproc)"
```

### 4.5 GitHub Actions CI Workflow

```yaml
name: Build Llamaste Image

on:
  push:
    branches: [master]
  pull_request:
    branches: [master]

env:
  BUILDROOT_VERSION: "2024.11.3"

jobs:
  build:
    runs-on: ubuntu-24.04
    timeout-minutes: 120

    steps:
      - name: Checkout Llamaste
        uses: actions/checkout@v4

      - name: Install Buildroot Dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y build-essential gcc g++ \
            libncurses5-dev bc bison flex unzip rsync cpio \
            wget curl git python3 file cmake patch perl \
            libssl-dev dosfstools mtools squashfs-tools

      - name: Cache Buildroot Downloads
        uses: actions/cache@v4
        with:
          path: ~/buildroot-dl
          key: buildroot-dl-${{ env.BUILDROOT_VERSION }}-${{ hashFiles('**/defconfig') }}
          restore-keys: buildroot-dl-${{ env.BUILDROOT_VERSION }}-

      - name: Cache ccache
        uses: actions/cache@v4
        with:
          path: ~/.buildroot-ccache
          key: buildroot-ccache-${{ github.sha }}
          restore-keys: buildroot-ccache-

      - name: Clone Buildroot
        run: |
          git clone --depth 1 --branch ${{ env.BUILDROOT_VERSION }} \
            https://gitlab.com/buildroot.org/buildroot.git ~/buildroot

      - name: Configure
        run: |
          cd ~/buildroot
          make BR2_EXTERNAL=${{ github.workspace }}/llamaste-br2-external \
               BR2_DL_DIR=~/buildroot-dl \
               llamaste_defconfig

      - name: Build
        run: |
          cd ~/buildroot
          make BR2_DL_DIR=~/buildroot-dl -j$(nproc)

      - name: Upload Artifacts
        uses: actions/upload-artifact@v4
        with:
          name: llamaste-images
          path: |
            ~/buildroot/output/images/bzImage
            ~/buildroot/output/images/rootfs.squashfs
            ~/buildroot/output/images/disk.img
```

### 4.6 Artifact Caching Strategy

| Cache | Key | Size | Impact |
|-------|-----|------|--------|
| Download cache (`dl/`) | Buildroot version + defconfig hash | ~500MB | Avoids re-downloading |
| ccache | Git SHA | ~2-5GB | 85% faster incremental builds |
| Docker layers | Dockerfile hash | ~1-2GB | Avoids rebuilding base |
| Buildroot output | Not recommended to cache | ~10-20GB | Too large, too fragile |

Source: [Meekdai/buildroot-actions](https://github.com/Meekdai/buildroot-actions),
[PPP buildroot.yaml](https://github.com/ppp-project/ppp/blob/master/.github/workflows/buildroot.yaml),
[Docker-nano/Buildroot](https://github.com/Docker-nano/Buildroot)

---

## 5. How Others Built Their Distros

### 5.1 Alpine Linux (aports + abuild)

**Build model:** Package-based. Individual packages built by `abuild` from APKBUILD
recipe files, assembled into an ISO/rootfs by `mkimage`.

**Key concepts:**
- **aports tree**: Repository of APKBUILD files organized into `main/`, `community/`,
  `testing/` directories. Strict dependency rules: main cannot depend on community.
- **abuild**: Shell script that reads APKBUILD, downloads source, applies patches,
  compiles, packages into .apk format. Uses `-r` flag for recursive dependency resolution.
- **apk-tools**: Alpine's package manager. Minimal, fast, parallel.
- **musl libc**: Alpine uses musl as its standard C library (same choice as Llamaste).
- **BusyBox**: Alpine uses BusyBox for core utilities (Llamaste does NOT use BusyBox).

**What Llamaste should adopt from Alpine:**
- musl as the C library (already planned)
- Aggressive binary stripping
- Security-first mindset (minimal attack surface)
- Simple, auditable build recipes

**What Llamaste does differently:**
- No package manager (single binary, not a collection of packages)
- No BusyBox (no shell, no traditional userland)
- Buildroot instead of abuild/aports

Source: [Alpine Wiki - Aports](https://wiki.alpinelinux.org/wiki/Aports_tree),
[Alpine abuild](https://wiki.alpinelinux.org/wiki/Abuild_and_Helpers)

### 5.2 OpenWrt (feeds + menuconfig)

**Build model:** Buildroot-derived. Highly customized for networking hardware.

**Key concepts:**
- **Feeds system**: External package repositories defined in `feeds.conf.default`.
  `./scripts/feeds update -a` downloads package definitions; `./scripts/feeds install -a`
  integrates them. Analogous to BR2_EXTERNAL but more dynamic.
- **Three-state packages**: `y` (build + include in image), `m` (build but don't include;
  install via opkg later), `n` (skip).
- **Image size awareness**: Build system warns if image exceeds flash capacity.
- **~8000 packages** available.
- **Uses musl libc** (same as Alpine and Llamaste).

**What Llamaste should adopt from OpenWrt:**
- Image size consciousness (we have RAM constraints, not flash constraints)
- Configuration inheritance from known-good configs
- Per-device profile concept (RAM tier profiles for Llamaste)

Source: [OpenWrt Build System](https://gist.github.com/chankruze/dee8c2ba31c338a60026e14e3383f981),
[Gateworks OpenWrt](https://trac.gateworks.com/wiki/OpenWrt/building)

### 5.3 Raspberry Pi OS (pi-gen + rpi-image-gen)

**Build model:** Stage-based debootstrap. Progressive image assembly.

**Key concepts:**
- **pi-gen**: Official tool. Shell scripts that execute in stages:
  - Stage 0: Bootstrap (debootstrap minimal Debian)
  - Stage 1: Core packages
  - Stage 2: Lite image (no desktop)
  - Stage 3: Desktop (X11/Wayland + LXDE)
  - Stage 5: Full image (office suite, learning tools)
- **rpi-image-gen** (2025): Newer tool using mmdebstrap + YAML layer definitions.
  More granular, more reproducible.

**What Llamaste should adopt from Raspberry Pi OS:**
- Stage-based thinking (boot -> core -> server -> desktop)
- The fact that Raspberry Pi OS now uses **labwc** (Wayland compositor based on wlroots)
  -- same technology stack Llamaste plans for Desktop mode
- Post-build customization scripts

Source: [RPi-Distro/pi-gen](https://github.com/RPi-Distro/pi-gen),
[rpi-image-gen announcement](https://www.raspberrypi.com/news/introducing-rpi-image-gen-build-highly-customised-raspberry-pi-software-images/)

### 5.4 Immutable OS Patterns (CoreOS/Flatcar/Bottlerocket/Talos)

#### Common Patterns

All immutable OSes share:
- **Read-only root filesystem** (squashfs, dm-verity, or similar)
- **Atomic updates** (image-based, not package-based)
- **Declarative configuration** (Ignition, cloud-init, API)
- **A/B partition scheme** for safe updates with rollback
- **No package manager** at runtime

#### Talos Linux -- Closest to Llamaste's Philosophy

Talos is the most analogous existing project:
- **No SSH, no shell** -- entirely API-driven (Llamaste: entirely LLM-driven)
- **12 binaries total** -- radical minimalism (Llamaste: 1 binary)
- **Written in Go** -- custom userland (Llamaste: custom C++ userland)
- **Signed kernel modules** -- all crypto-verified
- **Boots in under 60 seconds**

| Feature | Talos | Llamaste |
|---------|-------|----------|
| Purpose | Kubernetes node | LLM inference appliance |
| Management | gRPC API | LLM natural language |
| SSH | No | No |
| Shell | No | No |
| Binary count | 12 | 1 |
| Language | Go | C++ |
| Init system | Custom (machined) | PID 1 binary |
| Update mechanism | Image-based A/B | Image-based A/B |
| Root filesystem | Immutable | Immutable (squashfs) |

#### Bottlerocket -- Notable Patterns

- **Variant system**: Different builds for different purposes (aws-k8s, vmware-k8s,
  metal-k8s). Llamaste could adopt this: `server` and `desktop` variants.
- **API server on Unix socket**: All management through an API server. Analogous to
  Llamaste's HTTP API server.
- **Bootstrap containers**: One-time setup via containers on first boot. Llamaste's
  equivalent: first-boot model selection.
- **TUF-based updates**: The Update Framework for secure image distribution.

**What Llamaste should adopt from immutable OSes:**
- A/B partition scheme (already planned: SYS-A + SYS-B)
- Read-only squashfs root (already planned)
- No runtime package manager
- Declarative configuration model
- Atomic update + rollback

Source: [Talos Linux](https://www.talos.dev/),
[The New Stack - No SSH Talos](https://thenewstack.io/no-ssh-what-is-talos-this-linux-distro-for-kubernetes/),
[Bottlerocket](https://bottlerocket.dev/),
[3 Immutable OSes](https://thenewstack.io/3-immutable-operating-systems-bottlerocket-flatcar-and-talos-linux/)

### 5.5 llamafile -- Cosmopolitan Libc Approach

**Build model:** Single polyglot binary using Cosmopolitan Libc.

**Key concepts:**
- **Cosmopolitan Libc**: Creates executables that run on 6 OSes (Linux, Mac, Windows,
  FreeBSD, OpenBSD, NetBSD) without modification.
- **Polyglot file format**: Simultaneously a shell script, ELF binary, PE executable,
  and ZIP archive. The same file runs differently on each OS.
- **Model weights in ZIP**: GGUF model files are appended to the executable as ZIP
  entries (uncompressed, 4K-aligned for mmap).
- **tinyBLAS**: Custom linear algebra library that uses GPU APIs through graphics drivers
  (no CUDA/ROCm SDK needed).
- **Runtime SIMD dispatch**: Detects CPU features at startup and uses optimal codepath.

**How llamafile differs from Llamaste:**
- llamafile is a USER-SPACE application that runs on an existing OS
- Llamaste IS the OS -- it replaces the entire userland
- llamafile targets portability across OSes; Llamaste targets one OS (Linux) deeply
- llamafile uses Cosmopolitan Libc; Llamaste uses musl libc
- Both embed llama.cpp; Llamaste extends llama-server rather than wrapping it

**What Llamaste should adopt from llamafile:**
- The concept of bundling model weights with the executable (ZIP-append pattern)
- tinyBLAS-style GPU access without requiring SDK installation (Phase 2+)
- Runtime SIMD detection (llama.cpp already does this with `GGML_CPU_ALL_VARIANTS`)

Source: [llamafile GitHub](https://github.com/mozilla-ai/llamafile),
[LWN - Portable LLMs](https://lwn.net/Articles/971195/),
[Cosmopolitan Libc](https://justine.lol/cosmopolitan/)

---

## 6. YouTube & Video Resources Summary

### 6.1 Key Video Resources Found

| Title | Creator | URL / Platform | Summary |
|-------|---------|----------------|---------|
| Introduction to Embedded Linux Part 1 - Buildroot | DigiKey Electronics | [Class Central](https://www.classcentral.com/course/youtube-introduction-to-embedded-linux-part-1-buildroot-digi-key-electronics-130390) | 25-min video covering Buildroot basics with STM32MP157D-DK1. Demonstrates creating a minimal Linux image, flashing to SD card, serial console boot. |
| Intro to Embedded Linux Part 2 - Yocto Project | DigiKey Electronics | [DigiKey](https://www.digikey.com/en/maker/projects/intro-to-embedded-linux-part-2-yocto-project/2c08a1ad09d74f20b9844e566d332da4) | Companion video covering Yocto for comparison with Buildroot. |
| Buildroot: Making Embedded Linux Easy - A Real-Life Example | Yann Morin (Orange) / Linux Foundation | [Class Central](https://www.classcentral.com/course/youtube-buildroot-making-embedded-linux-easy-a-real-life-example-yann-morin-orange-221515) | Conference talk covering real-world Buildroot usage including external setup, configuration, board files, package infrastructure. |
| Build Your Own Operating System | Chris Titus Tech | YouTube | Covers creating custom Linux installations based on Arch Linux. Not a from-scratch kernel build but relevant for understanding distro customization. |
| Minimal Linux from Scratch | Referenced in GitHub gist | `https://www.youtube.com/watch?v=QlzoegSuIzg` | Covers kernel compilation, BusyBox, initramfs, bootloader, packaging. |
| Buildroot Getting Started Tutorial | Thomas Petazzoni (Bootlin) | [PDF slides](https://bootlin.com/pub/conferences/2019/elce/petazzoni-buildroot-tutorial/petazzoni-buildroot-tutorial.pdf) | ELCE 2019 conference tutorial. Thomas Petazzoni has ~5000 patches in Buildroot upstream. Comprehensive walkthrough. |

### 6.2 Professional Training

**Bootlin Buildroot Training**: 3-day on-site or 5 half-day online course.
- 95.7% satisfaction rate in 2023
- Covers: toolchain setup, package creation, kernel config, rootfs customization,
  BR2_EXTERNAL, post-build scripts, debugging, optimization
- Materials freely available under Creative Commons
- URL: [bootlin.com/training/buildroot](https://bootlin.com/training/buildroot/)

### 6.3 Notable Gaps in Video Content

- No specific YouTube tutorials found for BR2_EXTERNAL custom package creation
- No video tutorials for building static musl binaries with Buildroot
- No video content on Buildroot + llama.cpp integration
- NetworkChuck's Linux content focuses on using existing distros, not building custom ones
- Chris Titus Tech covers customization of existing distros, not from-scratch builds

---

## 7. Llamaste-Specific Recommendations

### 7.1 Recommended Defconfig (llamaste_defconfig)

```
# Architecture
BR2_x86_64=y
BR2_x86_corei7=y

# Toolchain
BR2_TOOLCHAIN_BUILDROOT_MUSL=y
BR2_TOOLCHAIN_BUILDROOT_CXX=y
BR2_STATIC_LIBS=y

# Build options
BR2_CCACHE=y
BR2_CCACHE_USE_BASEDIR=y
BR2_PER_PACKAGE_DIRECTORIES=y
BR2_REPRODUCIBLE=y

# System
BR2_TARGET_GENERIC_HOSTNAME="llamaste"
BR2_TARGET_GENERIC_ISSUE="Welcome to Llamaste"
BR2_SYSTEM_DHCP="eth0"
BR2_INIT_NONE=y
BR2_ROOTFS_OVERLAY="$(BR2_EXTERNAL_LLAMASTE_PATH)/board/llamaste/rootfs_overlay"
BR2_ROOTFS_POST_BUILD_SCRIPT="$(BR2_EXTERNAL_LLAMASTE_PATH)/board/llamaste/post_build.sh"
BR2_ROOTFS_POST_IMAGE_SCRIPT="$(BR2_EXTERNAL_LLAMASTE_PATH)/board/llamaste/post_image.sh"

# Kernel
BR2_LINUX_KERNEL=y
BR2_LINUX_KERNEL_CUSTOM_VERSION=y
BR2_LINUX_KERNEL_CUSTOM_VERSION_VALUE="6.6.72"
BR2_LINUX_KERNEL_USE_DEFCONFIG=y
BR2_LINUX_KERNEL_DEFCONFIG="x86_64"
BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES="$(BR2_EXTERNAL_LLAMASTE_PATH)/board/llamaste/linux-extra.config"
BR2_LINUX_KERNEL_IMAGE_BZIMAGE=y

# Bootloader
BR2_TARGET_GRUB2=y
BR2_TARGET_GRUB2_X86_64_EFI=y
BR2_TARGET_GRUB2_I386_PC=y

# Filesystem
BR2_TARGET_ROOTFS_SQUASHFS=y
BR2_TARGET_ROOTFS_SQUASHFS4_XZ=y

# Packages
BR2_PACKAGE_LLAMA_CPP=y
BR2_PACKAGE_LLAMASTE=y

# No BusyBox, no shell, no init system
# BR2_PACKAGE_BUSYBOX is not set

# Host tools for image generation
BR2_PACKAGE_HOST_GENIMAGE=y
BR2_PACKAGE_HOST_DOSFSTOOLS=y
BR2_PACKAGE_HOST_MTOOLS=y
```

### 7.2 Development Workflow Summary

```bash
# === One-Time Setup ===
# Clone Buildroot (pinned version)
git clone --depth 1 --branch 2024.11.3 https://gitlab.com/buildroot.org/buildroot.git
cd buildroot

# Configure with external tree
make BR2_EXTERNAL=../llamaste-br2-external llamaste_defconfig

# === Full Build ===
make -j$(nproc)    # ~30-45 min first time

# === Iterative Development ===
# Edit llamaste source code in llamaste-br2-external/src/...
# Option A: Package rebuild
make llamaste-rebuild all    # ~1-3 min with ccache

# Option B: OVERRIDE_SRCDIR workflow
echo 'LLAMASTE_OVERRIDE_SRCDIR = /path/to/llamaste/src' > local.mk
make llamaste-rebuild all    # rsync + recompile + image

# === Kernel Changes ===
make linux-menuconfig        # Edit kernel config
make linux-rebuild all       # Rebuild kernel + images
make linux-update-defconfig  # Save kernel config back

# === Test in QEMU ===
qemu-system-x86_64 \
    -m 4G \
    -kernel output/images/bzImage \
    -drive file=output/images/rootfs.squashfs,format=raw \
    -append "root=/dev/sda init=/opt/llamaste console=ttyS0" \
    -nographic \
    -enable-kvm

# === Save Configuration ===
make savedefconfig    # Writes minimal defconfig
```

### 7.3 Key Trade-Offs Decided

| Decision | Choice | Alternative | Rationale |
|----------|--------|-------------|-----------|
| Build system | Buildroot | Yocto, LFS | Simpler, faster, smaller output. Yocto overkill for single-binary system. |
| C library | musl | glibc, uClibc-ng | 6.5x smaller static binaries, cleaner static linking |
| Module loading | Disabled | Enabled | No userspace to load modules; built-in is simpler and faster |
| Kernel config base | x86_64_defconfig + fragments | allnoconfig, localmodconfig | Good default coverage + targeted additions. allnoconfig too aggressive. |
| Package management | None | opkg, apk | Single binary system; no packages to manage |
| Init system | None (PID 1 binary) | BusyBox init, systemd | Minimal, direct control, no unnecessary processes |
| Filesystem | squashfs (immutable) | ext4, initramfs | Read-only root matches immutable OS pattern; A/B updates |
| Bootloader | GRUB2 | syslinux, U-Boot | UEFI + BIOS support, dual-boot menu capability |

---

## Sources

### Official Documentation
- [Buildroot Manual](https://buildroot.org/downloads/manual/manual.html)
- [Buildroot CMake Packages](https://www.buildroot.org/downloads/manual/adding-packages-cmake.txt)
- [Buildroot External Tree](https://buildroot.org/downloads/manual/customize-outside-br.txt)
- [Linux Kernel Build](https://www.kernel.org/doc/html/latest/admin-guide/README.html)

### Tutorials & Guides
- [DigiKey Buildroot Tutorial](https://www.digikey.com/en/maker/projects/intro-to-embedded-linux-part-1-buildroot/a73a56de62444610a2187cd9e681c3f2)
- [Bootlin Buildroot Training](https://bootlin.com/training/buildroot/)
- [Bootlin Buildroot Tutorial PDF](https://bootlin.com/pub/conferences/2019/elce/petazzoni-buildroot-tutorial/petazzoni-buildroot-tutorial.pdf)
- [ejaaskel First Steps](https://ejaaskel.dev/the-first-steps-with-buildroot/)
- [embeddedinn Adding Packages](https://embeddedinn.com/articles/tutorial/Adding-Custom-Packages-to-Buildroot/)
- [Speeding Up Buildroot](https://www.nayab.dev/linux/buildroot/speed-up-buildroot-build.html)
- [Buildroot Cheatsheet](https://blog.inf.re/buildroot-cheatsheet.html)
- [Microchip BR2_EXTERNAL](https://developerhelp.microchip.com/xwiki/bin/view/software-tools/linux/buildroot-custom-project/)
- [musl-libc.org](https://www.musl-libc.org/how.html)
- [Static Linking with musl](https://radupopescu.net/tech/static_linking_for_cpp/)

### Kernel Configuration
- [Arch Wiki KMS](https://wiki.archlinux.org/title/Kernel_mode_setting)
- [Gentoo Kernel Config](https://wiki.gentoo.org/wiki/Kernel/Configuration)
- [kernelconfig.io](https://www.kernelconfig.io/config_r8169)
- [LWN Kernel Config](https://lwn.net/Articles/733405/)

### Distro Build Systems
- [Alpine aports](https://wiki.alpinelinux.org/wiki/Aports_tree)
- [Alpine abuild](https://wiki.alpinelinux.org/wiki/Abuild_and_Helpers)
- [OpenWrt Build](https://gist.github.com/chankruze/dee8c2ba31c338a60026e14e3383f981)
- [RPi-Distro/pi-gen](https://github.com/RPi-Distro/pi-gen)
- [rpi-image-gen](https://www.raspberrypi.com/news/introducing-rpi-image-gen-build-highly-customised-raspberry-pi-software-images/)

### Immutable OS Patterns
- [Talos Linux](https://www.talos.dev/)
- [Talos - No SSH](https://thenewstack.io/no-ssh-what-is-talos-this-linux-distro-for-kubernetes/)
- [Bottlerocket](https://bottlerocket.dev/)
- [3 Immutable OSes](https://thenewstack.io/3-immutable-operating-systems-bottlerocket-flatcar-and-talos-linux/)
- [Immutable OS Homelab Comparison](https://homelabstarter.com/homelab-immutable-os-comparison/)

### llamafile & Cosmopolitan
- [llamafile GitHub](https://github.com/mozilla-ai/llamafile)
- [LWN - Portable LLMs](https://lwn.net/Articles/971195/)
- [Cosmopolitan Libc](https://justine.lol/cosmopolitan/)
- [llamafile performance](https://www.theregister.com/2024/04/03/llamafile_performance_gains/)

### CI/CD & Docker
- [Meekdai/buildroot-actions](https://github.com/Meekdai/buildroot-actions)
- [PPP buildroot.yaml](https://github.com/ppp-project/ppp/blob/master/.github/workflows/buildroot.yaml)
- [Docker-nano/Buildroot](https://github.com/Docker-nano/Buildroot)
- [AdvancedClimateSystems/docker-buildroot](https://github.com/AdvancedClimateSystems/docker-buildroot)
- [niw/buildroot-docker](https://github.com/niw/buildroot-docker)
- [Buildroot in DevSecOps](https://www.robotsops.com/buildroot-in-devsecops-a-complete-tutorial/)

### Yocto vs Buildroot
- [Incredibuild Comparison](https://www.incredibuild.com/blog/yocto-or-buildroot-which-to-use-when-building-your-custom-embedded-systems)
- [Conclusive Engineering](https://conclusive.tech/glossary/yocto-vs-buildroot-for-production-bsps-a-practical-comparison/)
- [LinuxEmbedded.fr](https://linuxembedded.fr/2024/06/yocto-vs-buildroot-vs-everyone-else)

### Linux From Scratch
- [LFS Official](https://www.linuxfromscratch.org/)
- [Linux Journal DIY Distro](https://www.linuxjournal.com/content/diy-build-custom-minimal-linux-distribution-source)
- [blinry Tiny Linux](https://blinry.org/tiny-linux/)
