# Llamaste Phase 1: Headless LLM-OS MVP — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a bootable disk image where a single static C++ binary runs as PID 1, serving AI chat with system management tools over HTTP.

**Architecture:** A single `llamaste` binary extends llama-server with an agent loop, embedded system tools, and web UI. It runs as PID 1 using a supervisor/child fork pattern — the supervisor handles init duties and watchdog; the child runs inference. Built with Buildroot + musl libc, booted via GRUB into a 5-partition GPT layout.

**Tech Stack:** C++17, llama.cpp (ggml), musl libc, Buildroot, GRUB2, squashfs+zstd, ext4, QEMU, CMake

---

## Pre-Requisite: Development Environment

> This plan assumes a WSL2 Ubuntu environment on Windows. Buildroot **cannot** build on NTFS — the Buildroot tree must live on WSL2's native ext4 filesystem. Source code at `D:\Llamaste` is accessible from WSL2 at `/mnt/d/Llamaste`.

### Step 0.1: Install WSL2 dependencies

```bash
sudo apt update && sudo apt install -y \
  build-essential gcc g++ make cmake ninja-build \
  git wget curl unzip rsync cpio bc flex bison \
  libncurses-dev libssl-dev libelf-dev \
  python3 python3-pip \
  qemu-system-x86 ovmf \
  genimage mtools dosfstools squashfs-tools e2fsprogs \
  musl-tools
```

Expected: all packages install successfully.

### Step 0.2: Create Buildroot workspace in WSL2

```bash
mkdir -p ~/llamaste-build
ln -s /mnt/d/Llamaste ~/llamaste-build/src
```

This keeps the Buildroot build tree on native ext4 while the source code stays on the Windows drive for IDE access.

---

## Task 1: Buildroot External Tree

**Files:**
- Create: `br2-external/external.desc`
- Create: `br2-external/external.mk`
- Create: `br2-external/Config.in`
- Create: `br2-external/configs/llamaste_x86_64_defconfig`
- Create: `br2-external/package/llamaste/Config.in`
- Create: `br2-external/package/llamaste/llamaste.mk`

### Step 1.1: Clone Buildroot

```bash
cd ~/llamaste-build
git clone --depth 1 --branch 2024.02.9 \
  https://gitlab.com/buildroot.org/buildroot.git
```

Expected: `~/llamaste-build/buildroot/` directory exists with Buildroot source.

### Step 1.2: Create BR2_EXTERNAL descriptor

Write `br2-external/external.desc`:

```
name LLAMASTE
desc Llamaste - LLM IS the OS
```

### Step 1.3: Create BR2_EXTERNAL top-level Config.in

Write `br2-external/Config.in`:

```kconfig
source "$BR2_EXTERNAL_LLAMASTE_PATH/package/llamaste/Config.in"
```

### Step 1.4: Create BR2_EXTERNAL top-level external.mk

Write `br2-external/external.mk`:

```makefile
include $(sort $(wildcard $(BR2_EXTERNAL_LLAMASTE_PATH)/package/*/*.mk))
```

### Step 1.5: Create llamaste package Config.in

Write `br2-external/package/llamaste/Config.in`:

```kconfig
config BR2_PACKAGE_LLAMASTE
	bool "llamaste"
	select BR2_PACKAGE_HOST_CMAKE
	help
	  Llamaste LLM-OS. Single static C++ binary that runs as PID 1.
	  Combines llama-server + agent + system tools + web UI.

	  https://github.com/user/llamaste
```

### Step 1.6: Create llamaste package .mk (stub — will be extended in Task 3)

Write `br2-external/package/llamaste/llamaste.mk`:

```makefile
################################################################################
#
# llamaste
#
################################################################################

LLAMASTE_VERSION = 0.1.0
LLAMASTE_SITE = /mnt/d/Llamaste/src/llamaste
LLAMASTE_SITE_METHOD = local
LLAMASTE_LICENSE = Apache-2.0
LLAMASTE_INSTALL_STAGING = NO
LLAMASTE_INSTALL_TARGET = YES

# Placeholder — will add cmake-package integration in Task 3
# For now, just install a hello-world static binary to verify the pipeline

define LLAMASTE_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -static \
		-o $(@D)/llamaste $(@D)/stub.c
endef

define LLAMASTE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/llamaste $(TARGET_DIR)/opt/llamaste/llamaste
endef

$(eval $(generic-package))
```

### Step 1.7: Create stub init binary for testing the pipeline

Write `src/llamaste/stub.c`:

```c
#include <stdio.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/reboot.h>

int main(void) {
    /* Mount /proc so we can read /proc/cmdline */
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);

    printf("\n\n");
    printf("  Llamaste stub init — pipeline verification\n");
    printf("  If you see this, Buildroot + GRUB + kernel + PID 1 works!\n");
    printf("\n");

    /* Keep running (PID 1 must not exit) */
    while (1) {
        sleep(3600);
    }

    return 0;
}
```

### Step 1.8: Create initial defconfig

Write `br2-external/configs/llamaste_x86_64_defconfig`:

```
# Architecture
BR2_x86_64=y

# Toolchain
BR2_TOOLCHAIN_BUILDROOT_MUSL=y

# Init system: none (our binary IS init)
BR2_INIT_NONE=y
BR2_SYSTEM_BIN_SH_NONE=y

# Kernel
BR2_LINUX_KERNEL=y
BR2_LINUX_KERNEL_CUSTOM_VERSION=y
BR2_LINUX_KERNEL_CUSTOM_VERSION_VALUE="6.6.70"
BR2_LINUX_KERNEL_USE_CUSTOM_CONFIG=y
BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE="$(BR2_EXTERNAL_LLAMASTE_PATH)/board/llamaste/linux.config"
BR2_LINUX_KERNEL_INSTALL_TARGET=y

# Bootloader
BR2_TARGET_GRUB2=y
BR2_TARGET_GRUB2_X86_64_EFI=y
BR2_TARGET_GRUB2_I386_PC=y
BR2_TARGET_GRUB2_BUILTIN_MODULES_EFI="boot linux ext2 fat part_gpt normal efi_gop search search_label"
BR2_TARGET_GRUB2_BUILTIN_MODULES_PCBIOS="boot linux ext2 fat part_gpt normal biosdisk search search_label"

# Filesystem
BR2_TARGET_ROOTFS_SQUASHFS=y
BR2_TARGET_ROOTFS_SQUASHFS4_ZSTD=y

# No BusyBox
# BR2_PACKAGE_BUSYBOX is not set

# Our package
BR2_PACKAGE_LLAMASTE=y

# Scripts
BR2_ROOTFS_POST_BUILD_SCRIPT="$(BR2_EXTERNAL_LLAMASTE_PATH)/board/llamaste/post_build.sh"
BR2_ROOTFS_POST_IMAGE_SCRIPT="$(BR2_EXTERNAL_LLAMASTE_PATH)/board/llamaste/post_image.sh"

# Image
BR2_TARGET_GENERIC_HOSTNAME="llamaste"
BR2_TARGET_GENERIC_ISSUE="Welcome to Llamaste"
```

### Step 1.9: Create board directory structure

```bash
mkdir -p br2-external/board/llamaste/overlay/opt/llamaste
mkdir -p br2-external/board/llamaste/overlay/data
mkdir -p br2-external/board/llamaste/overlay/boot
```

### Step 1.10: Create post_build.sh

Write `br2-external/board/llamaste/post_build.sh`:

```bash
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
```

### Step 1.11: Create post_image.sh

Write `br2-external/board/llamaste/post_image.sh`:

```bash
#!/bin/sh
set -eu

BOARD_DIR=$(dirname "$0")
BINARIES_DIR="$1"
GENIMAGE_TMP="${BINARIES_DIR}/genimage.tmp"

# Install GRUB for BIOS boot into the ESP image
# (genimage handles partition layout)

rm -rf "${GENIMAGE_TMP}"

genimage \
    --rootpath "${TARGET_DIR}" \
    --tmppath "${GENIMAGE_TMP}" \
    --inputpath "${BINARIES_DIR}" \
    --outputpath "${BINARIES_DIR}" \
    --config "${BOARD_DIR}/genimage.cfg"

echo ""
echo "=== Image ready: ${BINARIES_DIR}/llamaste.img ==="
echo ""
```

### Step 1.12: Verify Buildroot configures successfully

```bash
cd ~/llamaste-build/buildroot
make BR2_EXTERNAL=/mnt/d/Llamaste/br2-external O=../output llamaste_x86_64_defconfig
```

Expected: configuration applied, `~/llamaste-build/output/.config` exists.

### Step 1.13: Commit

```bash
cd /mnt/d/Llamaste
git add br2-external/ src/llamaste/stub.c
git commit -m "feat: buildroot external tree with stub init binary"
```

---

## Task 2: Minimal Kernel Config

**Files:**
- Create: `br2-external/board/llamaste/linux.config`

### Step 2.1: Generate baseline kernel config

```bash
cd ~/llamaste-build/output
make linux-menuconfig
```

Start from `x86_64_defconfig` and strip. Save to `br2-external/board/llamaste/linux.config`.

### Step 2.2: Write the minimal kernel config

Write `br2-external/board/llamaste/linux.config` — key settings:

```
# Core
CONFIG_64BIT=y
CONFIG_SMP=y
CONFIG_PREEMPT_VOLUNTARY=y
CONFIG_HZ_250=y
CONFIG_MODULES=n

# Boot
CONFIG_EFI=y
CONFIG_EFI_STUB=y
CONFIG_ACPI=y

# Memory
CONFIG_MMU=y
CONFIG_TRANSPARENT_HUGEPAGE=y
CONFIG_TRANSPARENT_HUGEPAGE_MADVISE=y
CONFIG_HUGETLBFS=y

# Filesystems
CONFIG_PROC_FS=y
CONFIG_SYSFS=y
CONFIG_TMPFS=y
CONFIG_DEVTMPFS=y
CONFIG_DEVTMPFS_MOUNT=y
CONFIG_EXT4_FS=y
CONFIG_SQUASHFS=y
CONFIG_SQUASHFS_ZSTD=y
CONFIG_VFAT_FS=y
CONFIG_FAT_DEFAULT_UTF8=y
CONFIG_NLS_CODEPAGE_437=y
CONFIG_NLS_ISO8859_1=y

# Storage drivers (built-in, not modules)
CONFIG_ATA=y
CONFIG_SATA_AHCI=y
CONFIG_BLK_DEV_NVME=y
CONFIG_BLK_DEV_SD=y
CONFIG_USB_STORAGE=y

# SCSI (needed by USB storage and some SATA)
CONFIG_SCSI=y
CONFIG_BLK_DEV_SD=y

# Virtio (QEMU)
CONFIG_VIRTIO=y
CONFIG_VIRTIO_PCI=y
CONFIG_VIRTIO_BLK=y
CONFIG_VIRTIO_NET=y
CONFIG_VIRTIO_CONSOLE=y

# Network
CONFIG_NET=y
CONFIG_INET=y
CONFIG_IP_PNP=y
CONFIG_IP_PNP_DHCP=y
CONFIG_IP_MULTICAST=y
CONFIG_NETDEVICES=y
CONFIG_ETHERNET=y
CONFIG_NET_VENDOR_INTEL=y
CONFIG_E1000E=y
CONFIG_NET_VENDOR_REALTEK=y
CONFIG_R8169=y

# Console
CONFIG_SERIAL_8250=y
CONFIG_SERIAL_8250_CONSOLE=y
CONFIG_VT=y
CONFIG_VT_CONSOLE=y
CONFIG_PRINTK=y

# Watchdog
CONFIG_WATCHDOG=y
CONFIG_SOFT_WATCHDOG=y
CONFIG_I6300ESB_WDT=y

# Thermal
CONFIG_THERMAL=y
CONFIG_X86_PKG_TEMP_THERMAL=y
CONFIG_ACPI_THERMAL=y
CONFIG_HWMON=y
CONFIG_SENSORS_CORETEMP=y

# Power management (basic — research/21)
CONFIG_CPU_FREQ=y
CONFIG_CPU_FREQ_DEFAULT_GOV_PERFORMANCE=y
CONFIG_X86_ACPI_CPUFREQ=y
CONFIG_CPU_FREQ_GOV_PERFORMANCE=y
CONFIG_CPU_FREQ_GOV_POWERSAVE=y
CONFIG_CPU_FREQ_GOV_ONDEMAND=y
CONFIG_ACPI_BATTERY=y
CONFIG_ACPI_AC=y

# Disable unnecessary subsystems
# CONFIG_SOUND is not set
# CONFIG_DRM is not set
# CONFIG_WIRELESS is not set
# CONFIG_BLUETOOTH is not set
# CONFIG_MEDIA_SUPPORT is not set
# CONFIG_INPUT_MOUSEDEV is not set
# CONFIG_INPUT_JOYDEV is not set
# CONFIG_SERIO_SERPORT is not set
# CONFIG_LOGO is not set
# CONFIG_FB is not set
# CONFIG_SECURITY is not set
# CONFIG_AUDIT is not set
# CONFIG_PROFILING is not set
# CONFIG_DEBUG_FS is not set
# CONFIG_MULTIUSER is not set
# CONFIG_SYSVIPC is not set
# CONFIG_POSIX_MQUEUE is not set
# CONFIG_FUTEX is not set
```

### Step 2.3: Build the kernel

```bash
cd ~/llamaste-build/output
make linux -j$(nproc)
ls -lh images/bzImage
```

Expected: `bzImage` exists, size ~3-6 MB.

### Step 2.4: Test kernel boots in QEMU (expected: panic, no init)

```bash
qemu-system-x86_64 -m 512M -kernel images/bzImage \
  -append "console=ttyS0" -nographic -no-reboot
```

Expected: kernel boots, then panics with "No working init found" or "Kernel panic - not syncing: No init found." This confirms the kernel is functional.

Press `Ctrl-A X` to exit QEMU.

### Step 2.5: Commit

```bash
cd /mnt/d/Llamaste
git add br2-external/board/llamaste/linux.config
git commit -m "feat: minimal kernel config for x86_64 (~5MB, no modules)"
```

---

## Task 3: Stock llama-server in Buildroot

**Files:**
- Modify: `br2-external/package/llamaste/llamaste.mk`
- Create: `br2-external/package/llamaste/llamaste.hash`

### Step 3.1: Clone llama.cpp into the build workspace

```bash
cd ~/llamaste-build
git clone --depth 1 --branch b4654 \
  https://github.com/ggerganov/llama.cpp.git
```

(Pin to a known stable release tag. Adjust tag to latest stable at time of implementation.)

### Step 3.2: Test host build first — verify llama-server compiles

```bash
cd ~/llamaste-build/llama.cpp
cmake -B build-host \
  -DCMAKE_BUILD_TYPE=Release \
  -DGGML_STATIC=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DGGML_NATIVE=ON \
  -DGGML_CUDA=OFF \
  -DGGML_VULKAN=OFF \
  -DGGML_METAL=OFF \
  -DGGML_RPC=OFF \
  -DGGML_BLAS=OFF \
  -DLLAMA_CURL=OFF \
  -DLLAMA_BUILD_TESTS=OFF \
  -DLLAMA_BUILD_EXAMPLES=ON
cmake --build build-host --target llama-server -j$(nproc)
strip build-host/bin/llama-server
ls -lh build-host/bin/llama-server
```

Expected: `llama-server` binary, ~3-5 MB stripped.

### Step 3.3: Test host build with musl

```bash
cd ~/llamaste-build/llama.cpp
CC=musl-gcc CXX=musl-g++ cmake -B build-musl \
  -DCMAKE_BUILD_TYPE=Release \
  -DGGML_STATIC=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DGGML_NATIVE=ON \
  -DGGML_CUDA=OFF \
  -DGGML_VULKAN=OFF \
  -DGGML_METAL=OFF \
  -DGGML_RPC=OFF \
  -DGGML_BLAS=OFF \
  -DLLAMA_CURL=OFF \
  -DLLAMA_BUILD_TESTS=OFF \
  -DLLAMA_BUILD_EXAMPLES=ON \
  -DCMAKE_EXE_LINKER_FLAGS="-static" \
  -DCMAKE_C_FLAGS="-static" \
  -DCMAKE_CXX_FLAGS="-static"
cmake --build build-musl --target llama-server -j$(nproc)
file build-musl/bin/llama-server
```

Expected: output includes "statically linked". If musl-g++ is not available, install `musl-tools` package or build with the Buildroot cross-toolchain in the next step.

### Step 3.4: Update llamaste.mk to build llama-server via Buildroot cross-toolchain

Rewrite `br2-external/package/llamaste/llamaste.mk`:

```makefile
################################################################################
#
# llamaste
#
################################################################################

LLAMASTE_VERSION = 0.1.0
LLAMASTE_SITE = $(HOME)/llamaste-build/llama.cpp
LLAMASTE_SITE_METHOD = local
LLAMASTE_LICENSE = Apache-2.0
LLAMASTE_INSTALL_STAGING = NO
LLAMASTE_INSTALL_TARGET = YES
LLAMASTE_SUPPORTS_IN_SOURCE_BUILD = NO

LLAMASTE_CONF_OPTS = \
	-DCMAKE_BUILD_TYPE=Release \
	-DGGML_STATIC=ON \
	-DBUILD_SHARED_LIBS=OFF \
	-DGGML_NATIVE=OFF \
	-DGGML_CUDA=OFF \
	-DGGML_VULKAN=OFF \
	-DGGML_METAL=OFF \
	-DGGML_RPC=OFF \
	-DGGML_BLAS=OFF \
	-DLLAMA_CURL=OFF \
	-DLLAMA_BUILD_TESTS=OFF \
	-DLLAMA_BUILD_EXAMPLES=ON \
	-DCMAKE_EXE_LINKER_FLAGS="-static" \
	-DCMAKE_C_FLAGS="-static" \
	-DCMAKE_CXX_FLAGS="-static"

define LLAMASTE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/bin/llama-server \
		$(TARGET_DIR)/opt/llamaste/llama-server
endef

$(eval $(cmake-package))
```

### Step 3.5: Build stock llama-server in Buildroot

```bash
cd ~/llamaste-build/output
make llamaste-rebuild -j$(nproc)
file target/opt/llamaste/llama-server
```

Expected: `ELF 64-bit LSB executable, x86-64, ... statically linked` with musl.

### Step 3.6: Create a minimal genimage.cfg (simplified for this stage)

Write `br2-external/board/llamaste/genimage.cfg`:

```
image boot.vfat {
    vfat {
        files = {
            "bzImage"
        }
    }
    size = 64M
}

image rootfs.squashfs {
    squashfs {
        compression = "zstd"
    }
}

image llamaste.img {
    hdimage {
        gpt = true
    }

    partition bios-boot {
        partition-type-uuid = 21686148-6449-6E6F-744E-656564454649
        size = 1M
    }

    partition ESP {
        partition-type-uuid = C12A7328-F81F-11D2-BA4B-00A0C93EC93B
        bootable = true
        image = boot.vfat
        size = 256M
    }

    partition sys-a {
        partition-type-uuid = 4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709
        image = rootfs.squashfs
        size = 256M
    }

    partition sys-b {
        partition-type-uuid = 4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709
        size = 256M
    }

    partition data {
        partition-type-uuid = 3B8F8425-20E0-4C36-B2BC-B9C3D3D6C68E
        size = 2G
    }
}
```

### Step 3.7: Create minimal grub.cfg

Write `br2-external/board/llamaste/grub.cfg`:

```
set timeout=3
set default=0

menuentry "Llamaste Server" {
    linux /bzImage root=/dev/vda3 rootfstype=squashfs ro quiet \
        console=ttyS0 \
        init=/opt/llamaste/llama-server
}
```

(This is temporary — will be replaced with proper label-based lookup in Task 10.)

### Step 3.8: Build full image

```bash
cd ~/llamaste-build/output
make -j$(nproc)
ls -lh images/llamaste.img
```

Expected: `llamaste.img` file exists.

### Step 3.9: Download a small test model

```bash
mkdir -p ~/llamaste-build/test-models
cd ~/llamaste-build/test-models
wget -q "https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q4_k_m.gguf"
```

Expected: ~400 MB GGUF file downloaded.

### Step 3.10: Test in QEMU with model on virtio-9p or FAT drive

```bash
# Create a data disk with the model
dd if=/dev/zero of=~/llamaste-build/data.img bs=1M count=2048
mkfs.ext4 ~/llamaste-build/data.img
mkdir -p /tmp/llamaste-data
sudo mount ~/llamaste-build/data.img /tmp/llamaste-data
sudo mkdir -p /tmp/llamaste-data/models
sudo cp ~/llamaste-build/test-models/*.gguf /tmp/llamaste-data/models/
sudo umount /tmp/llamaste-data

# Boot QEMU
qemu-system-x86_64 -m 4G -smp 4 \
  -drive file=~/llamaste-build/output/images/llamaste.img,format=raw,if=virtio \
  -drive file=~/llamaste-build/data.img,format=raw,if=virtio \
  -netdev user,id=net0,hostfwd=tcp::8080-:8080 \
  -device virtio-net-pci,netdev=net0 \
  -nographic
```

Note: At this stage, llama-server won't auto-find the model (no data partition mounting yet). This test verifies the binary executes. You should see llama-server's startup output and "listening on 0.0.0.0:8080" message (it may fail to load a model, which is expected).

Expected: llama-server binary starts, prints version info, then either listens or exits with "no model" error. The key thing is: **the kernel booted and executed our binary as PID 1**.

### Step 3.11: Commit

```bash
cd /mnt/d/Llamaste
git add br2-external/package/llamaste/ br2-external/board/llamaste/
git commit -m "feat: stock llama-server builds and boots in QEMU via Buildroot"
```

---

## Task 4: PID 1 Supervisor with Init

**Files:**
- Rewrite: `src/llamaste/main.cpp` (from scratch, production version)
- Create: `src/llamaste/supervisor.h`
- Create: `src/llamaste/supervisor.cpp`
- Create: `src/llamaste/init.h`
- Create: `src/llamaste/init.cpp`
- Create: `src/llamaste/hwdetect.h`
- Create: `src/llamaste/hwdetect.cpp`
- Create: `tests/test_hwdetect.cpp`
- Modify: `src/llamaste/CMakeLists.txt`

### Step 4.1: Write test for hardware detection (host-testable)

Write `tests/test_hwdetect.cpp`:

```cpp
// Minimal test harness (no gtest dependency for embedded)
#include <cassert>
#include <cstdio>
#include "../src/llamaste/hwdetect.h"

int main() {
    HardwareInfo hw = detect_hardware();

    // Must detect at least 1 core
    assert(hw.cpu_cores >= 1);
    printf("PASS: cpu_cores=%d\n", hw.cpu_cores);

    // Must detect some RAM
    assert(hw.ram_total_mb > 0);
    printf("PASS: ram_total_mb=%d\n", hw.ram_total_mb);

    // CPU model should not be empty
    assert(!hw.cpu_model.empty());
    printf("PASS: cpu_model=%s\n", hw.cpu_model.c_str());

    printf("\nAll hardware detection tests passed.\n");
    return 0;
}
```

### Step 4.2: Run test to verify it fails

```bash
cd /mnt/d/Llamaste
g++ -std=c++17 -o tests/test_hwdetect tests/test_hwdetect.cpp
```

Expected: FAIL — `hwdetect.h: No such file or directory`.

### Step 4.3: Implement hardware detection

Write `src/llamaste/hwdetect.h`:

```cpp
#pragma once
#include <string>

struct HardwareInfo {
    std::string cpu_model = "unknown";
    int cpu_cores = 1;
    int ram_total_mb = 0;
    int ram_free_mb = 0;
    bool gpu_detected = false;
    std::string gpu_name;
    bool has_avx2 = false;
    bool has_avx512 = false;
};

HardwareInfo detect_hardware();
int read_meminfo_kb(const char* key);
std::string read_sysfs_line(const char* path);
```

Write `src/llamaste/hwdetect.cpp`:

```cpp
#include "hwdetect.h"
#include <fstream>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>

std::string read_sysfs_line(const char* path) {
    std::ifstream f(path);
    std::string line;
    if (std::getline(f, line)) return line;
    return "";
}

int read_meminfo_kb(const char* key) {
    std::ifstream f("/proc/meminfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.find(key) == 0) {
            size_t colon = line.find(':');
            if (colon != std::string::npos)
                return std::atoi(line.c_str() + colon + 1);
        }
    }
    return 0;
}

HardwareInfo detect_hardware() {
    HardwareInfo hw;

    // CPU info from /proc/cpuinfo
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    int core_count = 0;
    while (std::getline(cpuinfo, line)) {
        if (line.find("model name") == 0 && hw.cpu_model == "unknown") {
            size_t colon = line.find(':');
            if (colon != std::string::npos)
                hw.cpu_model = line.substr(colon + 2);
        }
        if (line.find("processor") == 0)
            core_count++;
        if (line.find("avx2") != std::string::npos)
            hw.has_avx2 = true;
        if (line.find("avx512") != std::string::npos)
            hw.has_avx512 = true;
    }
    hw.cpu_cores = core_count > 0 ? core_count : 1;

    // RAM from /proc/meminfo
    hw.ram_total_mb = read_meminfo_kb("MemTotal") / 1024;
    hw.ram_free_mb = read_meminfo_kb("MemAvailable") / 1024;

    // GPU from /sys/class/drm
    DIR* drm = opendir("/sys/class/drm");
    if (drm) {
        struct dirent* entry;
        while ((entry = readdir(drm)) != nullptr) {
            std::string vendor_path = std::string("/sys/class/drm/")
                + entry->d_name + "/device/vendor";
            std::string vendor = read_sysfs_line(vendor_path.c_str());
            if (vendor == "0x10de") {
                hw.gpu_detected = true;
                hw.gpu_name = "NVIDIA";
            } else if (vendor == "0x1002") {
                hw.gpu_detected = true;
                hw.gpu_name = "AMD";
            } else if (vendor == "0x8086") {
                hw.gpu_detected = true;
                hw.gpu_name = "Intel";
            }
        }
        closedir(drm);
    }

    return hw;
}
```

### Step 4.4: Run test to verify it passes

```bash
g++ -std=c++17 -I src/llamaste -o tests/test_hwdetect \
  tests/test_hwdetect.cpp src/llamaste/hwdetect.cpp
./tests/test_hwdetect
```

Expected: all assertions pass on the host (WSL2).

### Step 4.5: Write init module (filesystem mounting, performance tuning)

Write `src/llamaste/init.h`:

```cpp
#pragma once
#include <string>

// Mount essential filesystems for PID 1
void init_mount_filesystems();

// Find and mount the DATA partition (ext4, 5th partition)
bool init_mount_data();

// Create required directories on /data
void init_create_data_dirs();

// Set CPU governor, THP, swappiness
void init_tune_performance();

// Set hostname from /data/llamaste/config/hostname or default
void init_set_hostname(const std::string& default_name);

// Parse /proc/cmdline and return boot mode ("server" or "desktop")
std::string init_parse_boot_mode();
```

Write `src/llamaste/init.cpp`:

```cpp
#include "init.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>

static void try_mount(const char* src, const char* tgt,
                      const char* fs, unsigned long flags,
                      const char* data) {
    mkdir(tgt, 0755);
    if (mount(src, tgt, fs, flags, data) != 0)
        fprintf(stderr, "[init] mount %s failed: %m\n", tgt);
}

void init_mount_filesystems() {
    try_mount("proc",     "/proc",    "proc",     0, nullptr);
    try_mount("sysfs",    "/sys",     "sysfs",    0, nullptr);
    try_mount("devtmpfs", "/dev",     "devtmpfs", 0, nullptr);
    try_mount("tmpfs",    "/tmp",     "tmpfs",    0, "size=64M");
    try_mount("tmpfs",    "/run",     "tmpfs",    0, "size=16M");
    mkdir("/dev/pts", 0755);
    try_mount("devpts",   "/dev/pts", "devpts",   0, nullptr);
}

bool init_mount_data() {
    // 5-partition layout: data is partition 5
    // Try virtio (QEMU), then SATA, then NVMe
    const char* candidates[] = {
        "/dev/vda5", "/dev/sda5", "/dev/nvme0n1p5",
        "/dev/vda4", "/dev/sda4", "/dev/nvme0n1p4", // fallback to 4-part
        nullptr
    };

    mkdir("/data", 0755);
    for (int i = 0; candidates[i]; i++) {
        struct stat st;
        if (stat(candidates[i], &st) == 0) {
            if (mount(candidates[i], "/data", "ext4", 0, nullptr) == 0) {
                fprintf(stderr, "[init] Mounted %s on /data\n", candidates[i]);
                return true;
            }
        }
    }

    fprintf(stderr, "[init] WARNING: No data partition, using tmpfs\n");
    try_mount("tmpfs", "/data", "tmpfs", 0, "size=1G");
    return false;
}

void init_create_data_dirs() {
    const char* dirs[] = {
        "/data/models",
        "/data/llamaste",
        "/data/llamaste/conversations",
        "/data/llamaste/config",
        "/data/llamaste/logs",
        "/data/llamaste/skills",
        nullptr
    };
    for (int i = 0; dirs[i]; i++)
        mkdir(dirs[i], 0755);
}

void init_tune_performance() {
    // CPU governor → performance
    for (int i = 0; i < 256; i++) {
        char path[128];
        snprintf(path, sizeof(path),
            "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", i);
        std::ofstream f(path);
        if (!f.is_open()) break;
        f << "performance";
    }

    // THP → madvise
    std::ofstream thp("/sys/kernel/mm/transparent_hugepage/enabled");
    if (thp.is_open()) thp << "madvise";

    // Low swappiness
    std::ofstream sw("/proc/sys/vm/swappiness");
    if (sw.is_open()) sw << "1";

    // Raise max_map_count for mmap-heavy model loading
    std::ofstream mm("/proc/sys/vm/max_map_count");
    if (mm.is_open()) mm << "1048576";
}

void init_set_hostname(const std::string& default_name) {
    std::string hostname = default_name;
    std::ifstream hf("/data/llamaste/config/hostname");
    if (hf.is_open()) {
        std::string saved;
        if (std::getline(hf, saved) && !saved.empty())
            hostname = saved;
    }
    std::ofstream hn("/proc/sys/kernel/hostname");
    if (hn.is_open()) hn << hostname;
    fprintf(stderr, "[init] Hostname: %s\n", hostname.c_str());
}

std::string init_parse_boot_mode() {
    std::ifstream f("/proc/cmdline");
    std::string line;
    if (std::getline(f, line)) {
        if (line.find("llamaste.mode=desktop") != std::string::npos)
            return "desktop";
    }
    return "server";
}
```

### Step 4.6: Write supervisor module

Write `src/llamaste/supervisor.h`:

```cpp
#pragma once
#include <string>

struct SupervisorConfig {
    std::string model_path;
    std::string boot_mode;  // "server" or "desktop"
    int http_port = 80;
    int cpu_cores = 1;
    int ram_total_mb = 0;
};

// Run the supervisor loop. Never returns on success (PID 1).
// Forks a child for inference, monitors it, re-forks on crash.
[[noreturn]] void supervisor_run(const SupervisorConfig& config);
```

Write `src/llamaste/supervisor.cpp`:

```cpp
#include "supervisor.h"
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <fcntl.h>
#include <time.h>

static volatile sig_atomic_t g_child_exited = 0;
static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile pid_t g_child_pid = 0;

static void supervisor_sigchld(int) {
    g_child_exited = 1;
}

static void supervisor_sigterm(int) {
    g_shutdown_requested = 1;
}

// Forward declaration — implemented in the child's server code
extern int child_main(const SupervisorConfig& config);

static pid_t spawn_child(const SupervisorConfig& config) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("[supervisor] fork failed");
        return -1;
    }
    if (pid == 0) {
        // Child process: run the inference server
        int rc = child_main(config);
        _exit(rc);
    }
    return pid;
}

static int open_watchdog() {
    int fd = open("/dev/watchdog", O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "[supervisor] No hardware watchdog available\n");
    } else {
        fprintf(stderr, "[supervisor] Hardware watchdog opened\n");
    }
    return fd;
}

static void kick_watchdog(int fd) {
    if (fd >= 0) {
        write(fd, "V", 1); // "V" = magic close character; any write kicks it
    }
}

[[noreturn]] void supervisor_run(const SupervisorConfig& config) {
    // Set up signal handlers
    struct sigaction sa = {};
    sa.sa_handler = supervisor_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, nullptr);

    sa.sa_handler = supervisor_sigterm;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    int watchdog_fd = open_watchdog();
    int crash_count = 0;
    time_t last_crash = 0;

    while (!g_shutdown_requested) {
        // Spawn inference child
        fprintf(stderr, "[supervisor] Spawning inference child...\n");
        g_child_exited = 0;
        g_child_pid = spawn_child(config);

        if (g_child_pid < 0) {
            fprintf(stderr, "[supervisor] Failed to spawn child, retrying in 5s\n");
            sleep(5);
            continue;
        }

        fprintf(stderr, "[supervisor] Child PID %d running\n", g_child_pid);

        // Monitor loop
        while (!g_child_exited && !g_shutdown_requested) {
            kick_watchdog(watchdog_fd);
            sleep(10);
        }

        if (g_shutdown_requested) break;

        // Child exited — reap it
        int status = 0;
        waitpid(g_child_pid, &status, 0);

        if (WIFEXITED(status)) {
            fprintf(stderr, "[supervisor] Child exited with code %d\n",
                    WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "[supervisor] Child killed by signal %d\n",
                    WTERMSIG(status));
        }

        // Crash rate limiting: if >3 crashes in 60s, wait longer
        time_t now = time(nullptr);
        if (now - last_crash < 60) {
            crash_count++;
        } else {
            crash_count = 1;
        }
        last_crash = now;

        if (crash_count > 3) {
            fprintf(stderr,
                "[supervisor] Too many crashes, waiting 30s before restart\n");
            sleep(30);
            crash_count = 0;
        } else {
            sleep(2); // Brief pause before respawn
        }
    }

    // Graceful shutdown
    fprintf(stderr, "[supervisor] Shutting down...\n");

    if (g_child_pid > 0) {
        kill(g_child_pid, SIGTERM);
        int status;
        // Give child 10s to save state
        alarm(10);
        waitpid(g_child_pid, &status, 0);
        alarm(0);
    }

    // Close watchdog cleanly (write "V" to disable)
    if (watchdog_fd >= 0) {
        write(watchdog_fd, "V", 1);
        close(watchdog_fd);
    }

    sync();
    umount2("/data", MNT_DETACH);
    reboot(RB_POWER_OFF);
    _exit(0); // unreachable
}
```

### Step 4.7: Write new main.cpp

Rewrite `src/llamaste/main.cpp`:

```cpp
// Llamaste — LLM IS the OS
// main.cpp: PID 1 entry point
//
// Supervisor pattern (research/17):
// 1. PID 1 mounts filesystems, detects hardware, selects model
// 2. fork()s child for inference
// 3. Stays alive as supervisor (watchdog, crash recovery)

#include <cstdio>
#include <unistd.h>

#include "init.h"
#include "hwdetect.h"
#include "supervisor.h"

static const char* VERSION = "0.1.0";

struct ModelCandidate {
    const char* name;
    const char* filename;
    int required_mb;
};

static const ModelCandidate MODELS[] = {
    {"Qwen2.5-32B-Instruct",  "qwen2.5-32b-instruct-q4_k_m.gguf",  22000},
    {"Qwen2.5-14B-Instruct",  "qwen2.5-14b-instruct-q4_k_m.gguf",  11000},
    {"Qwen2.5-7B-Instruct",   "qwen2.5-7b-instruct-q4_k_m.gguf",    6500},
    {"Qwen2.5-3B-Instruct",   "qwen2.5-3b-instruct-q4_k_m.gguf",    4000},
    {"Qwen2.5-1.5B-Instruct", "qwen2.5-1.5b-instruct-q4_k_m.gguf",  2500},
    {"Qwen2.5-0.5B-Instruct", "qwen2.5-0.5b-instruct-q4_k_m.gguf",  1500},
    {nullptr, nullptr, 0}
};

static std::string select_model(int available_mb) {
    for (int i = 0; MODELS[i].name; i++) {
        if (available_mb >= MODELS[i].required_mb) {
            std::string path = std::string("/data/models/") + MODELS[i].filename;
            if (access(path.c_str(), R_OK) == 0) {
                fprintf(stderr, "[main] Selected model: %s\n", MODELS[i].name);
                return path;
            }
        }
    }
    // Fallback: first .gguf found in /data/models
    // (implementation omitted for brevity — same as draft main.cpp)
    return "";
}

int main(int argc, char** argv) {
    fprintf(stderr, "\n");
    fprintf(stderr, "  Llamaste v%s — LLM IS the OS\n", VERSION);
    fprintf(stderr, "\n");

    bool pid1 = (getpid() == 1);

    // Step 1: Init (PID 1 only)
    if (pid1) {
        init_mount_filesystems();
    }

    // Step 2: Parse boot mode
    std::string mode = init_parse_boot_mode();
    fprintf(stderr, "[main] Boot mode: %s\n", mode.c_str());

    // Step 3: Mount data partition
    if (pid1) {
        init_mount_data();
        init_create_data_dirs();
    }

    // Step 4: Detect hardware
    HardwareInfo hw = detect_hardware();
    fprintf(stderr, "[main] CPU: %s (%d cores)\n",
            hw.cpu_model.c_str(), hw.cpu_cores);
    fprintf(stderr, "[main] RAM: %d MB total, %d MB available\n",
            hw.ram_total_mb, hw.ram_free_mb);

    // Step 5: Tune performance
    if (pid1) {
        init_tune_performance();
        init_set_hostname("llamaste");
    }

    // Step 6: Select model
    int available = hw.ram_free_mb;
    if (mode == "desktop") available -= 500;
    std::string model_path = select_model(available);

    if (model_path.empty()) {
        fprintf(stderr, "[main] No model found in /data/models/\n");
        fprintf(stderr, "[main] Will start without model (web UI only)\n");
    }

    // Step 7: Launch supervisor (never returns for PID 1)
    SupervisorConfig sc;
    sc.model_path = model_path;
    sc.boot_mode = mode;
    sc.http_port = 80;
    sc.cpu_cores = hw.cpu_cores;
    sc.ram_total_mb = hw.ram_total_mb;

    if (pid1) {
        supervisor_run(sc); // never returns
    } else {
        // Running outside PID 1 (development mode)
        fprintf(stderr, "[main] Not PID 1, running child_main directly\n");
        extern int child_main(const SupervisorConfig& config);
        return child_main(sc);
    }
}
```

### Step 4.8: Write child_main stub (placeholder until agent/server integration)

Write `src/llamaste/child_main.cpp`:

```cpp
#include "supervisor.h"
#include <cstdio>
#include <unistd.h>
#include <csignal>

static volatile bool g_running = true;

static void child_signal(int) {
    g_running = false;
}

int child_main(const SupervisorConfig& config) {
    signal(SIGTERM, child_signal);
    signal(SIGINT, child_signal);

    fprintf(stderr, "[child] Inference child started\n");
    fprintf(stderr, "[child] Model: %s\n",
            config.model_path.empty() ? "(none)" : config.model_path.c_str());
    fprintf(stderr, "[child] Port: %d\n", config.http_port);

    // TODO: Initialize llama-server, agent, tools, web UI here
    // For now, just sleep to prove supervisor pattern works

    fprintf(stderr, "[child] Stub server running (waiting for real implementation)\n");

    while (g_running) {
        sleep(1);
    }

    fprintf(stderr, "[child] Shutting down gracefully\n");
    return 0;
}
```

### Step 4.9: Update CMakeLists.txt

Rewrite `src/llamaste/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.14)
project(llamaste VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Llamaste sources (supervisor + init — no llama.cpp dependency yet)
set(LLAMASTE_SOURCES
    main.cpp
    supervisor.cpp
    init.cpp
    hwdetect.cpp
    child_main.cpp
)

add_executable(llamaste ${LLAMASTE_SOURCES})

# Static linking for musl builds
option(LLAMASTE_STATIC "Build fully static binary" OFF)
if(LLAMASTE_STATIC)
    target_link_options(llamaste PRIVATE -static)
endif()

install(TARGETS llamaste RUNTIME DESTINATION opt/llamaste)
```

### Step 4.10: Build and test supervisor on host

```bash
cd /mnt/d/Llamaste/src/llamaste
cmake -B build-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host -j$(nproc)
./build-host/llamaste
```

Expected: prints banner, detects hardware, says "Not PID 1, running child_main directly", runs child stub. Kill with Ctrl-C → clean shutdown.

### Step 4.11: Test supervisor fork pattern (only meaningful as PID 1 in QEMU)

Update `br2-external/package/llamaste/llamaste.mk` to build the new source:

```makefile
LLAMASTE_SITE = /mnt/d/Llamaste/src/llamaste
LLAMASTE_SITE_METHOD = local
LLAMASTE_CONF_OPTS = \
	-DCMAKE_BUILD_TYPE=Release \
	-DLLAMASTE_STATIC=ON

define LLAMASTE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/llamaste \
		$(TARGET_DIR)/opt/llamaste/llamaste
endef

$(eval $(cmake-package))
```

Update `grub.cfg` to use `init=/opt/llamaste/llamaste`.

Rebuild and boot in QEMU. Expected output:

```
Llamaste v0.1.0 — LLM IS the OS

[main] Boot mode: server
[init] Mounted /dev/vda5 on /data
[main] CPU: ... (N cores)
[main] RAM: ... MB total
[supervisor] Spawning inference child...
[supervisor] Child PID 2 running
[child] Inference child started
[child] Stub server running
```

### Step 4.12: Test crash recovery — kill child from another terminal

```bash
# In another terminal, connect to QEMU monitor
# Or use: kill -9 <child_pid> from inside QEMU

# If using QEMU monitor:
# (qemu) info cpus  — shows processes
```

Expected: supervisor detects SIGCHLD, prints "Child killed by signal 9", re-spawns.

### Step 4.13: Commit

```bash
cd /mnt/d/Llamaste
git add src/llamaste/ tests/ br2-external/
git commit -m "feat: PID 1 supervisor with init, hwdetect, and child fork pattern"
```

---

## Task 5: Tools System

**Files:**
- Create: `src/llamaste/tools.h`
- Create: `src/llamaste/tools.cpp`
- Create: `src/llamaste/tools_fs.h` / `tools_fs.cpp`
- Create: `src/llamaste/tools_process.h` / `tools_process.cpp`
- Create: `src/llamaste/tools_network.h` / `tools_network.cpp`
- Create: `src/llamaste/tools_system.h` / `tools_system.cpp`
- Create: `src/llamaste/tools_config.h` / `tools_config.cpp`
- Create: `src/llamaste/tools_model.h` / `tools_model.cpp`
- Create: `tests/test_tools.cpp`
- Modify: `src/llamaste/CMakeLists.txt`

### Step 5.1: Write test for tool registry

Write `tests/test_tools.cpp`:

```cpp
#include <cassert>
#include <cstdio>
#include <string>
#include "../src/llamaste/tools.h"

int main() {
    ToolRegistry registry;

    // Register a test tool
    registry.register_tool({
        .name = "test.echo",
        .description = "Echo back the input",
        .parameters = R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})",
        .handler = [](const std::string& args_json) -> std::string {
            return R"({"result":"echoed"})";
        }
    });

    assert(registry.count() == 1);
    printf("PASS: tool registered, count=1\n");

    // Dispatch
    auto result = registry.dispatch("test.echo", R"({"text":"hello"})");
    assert(result.find("echoed") != std::string::npos);
    printf("PASS: dispatch returned: %s\n", result.c_str());

    // Unknown tool
    auto err = registry.dispatch("nonexistent", "{}");
    assert(err.find("error") != std::string::npos);
    printf("PASS: unknown tool returns error\n");

    // Generate OpenAI-format tool definitions
    auto defs = registry.to_openai_tools_json();
    assert(defs.find("test.echo") != std::string::npos);
    printf("PASS: openai tools json generated\n");

    printf("\nAll tool registry tests passed.\n");
    return 0;
}
```

### Step 5.2: Run test to verify it fails

```bash
g++ -std=c++17 -o tests/test_tools tests/test_tools.cpp
```

Expected: FAIL — `tools.h: No such file or directory`.

### Step 5.3: Implement tool registry

Write `src/llamaste/tools.h`:

```cpp
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

struct ToolDef {
    std::string name;
    std::string description;
    std::string parameters; // JSON Schema string
    std::function<std::string(const std::string&)> handler;
    bool requires_confirmation = false;
};

class ToolRegistry {
public:
    void register_tool(ToolDef tool);
    int count() const;
    std::string dispatch(const std::string& name,
                         const std::string& args_json) const;
    std::string to_openai_tools_json() const;
    std::vector<std::string> tool_names() const;
    bool needs_confirmation(const std::string& name) const;

private:
    std::unordered_map<std::string, ToolDef> tools_;
};

// Register all built-in tools
void register_fs_tools(ToolRegistry& reg);
void register_process_tools(ToolRegistry& reg);
void register_network_tools(ToolRegistry& reg);
void register_system_tools(ToolRegistry& reg);
void register_config_tools(ToolRegistry& reg);
void register_model_tools(ToolRegistry& reg);
```

Write `src/llamaste/tools.cpp`:

```cpp
#include "tools.h"
#include <sstream>

void ToolRegistry::register_tool(ToolDef tool) {
    tools_[tool.name] = std::move(tool);
}

int ToolRegistry::count() const {
    return static_cast<int>(tools_.size());
}

std::string ToolRegistry::dispatch(const std::string& name,
                                    const std::string& args_json) const {
    auto it = tools_.find(name);
    if (it == tools_.end()) {
        return R"({"error":"unknown tool: )" + name + R"("})";
    }
    return it->second.handler(args_json);
}

bool ToolRegistry::needs_confirmation(const std::string& name) const {
    auto it = tools_.find(name);
    return it != tools_.end() && it->second.requires_confirmation;
}

std::vector<std::string> ToolRegistry::tool_names() const {
    std::vector<std::string> names;
    names.reserve(tools_.size());
    for (auto& [k, v] : tools_)
        names.push_back(k);
    return names;
}

std::string ToolRegistry::to_openai_tools_json() const {
    std::ostringstream ss;
    ss << "[";
    bool first = true;
    for (auto& [name, tool] : tools_) {
        if (!first) ss << ",";
        first = false;
        ss << R"({"type":"function","function":{)"
           << R"("name":")" << tool.name << R"(",)"
           << R"("description":")" << tool.description << R"(",)"
           << R"("parameters":)" << tool.parameters
           << "}}";
    }
    ss << "]";
    return ss.str();
}
```

### Step 5.4: Run test to verify it passes

```bash
g++ -std=c++17 -I src/llamaste -o tests/test_tools \
  tests/test_tools.cpp src/llamaste/tools.cpp
./tests/test_tools
```

Expected: all assertions pass.

### Step 5.5: Implement filesystem tools

Write `src/llamaste/tools_fs.h` and `src/llamaste/tools_fs.cpp`.

Key tools: `fs.list_directory`, `fs.read_file`, `fs.write_file`, `fs.delete`, `fs.disk_usage`, `fs.search`.

All paths must be validated to start with `/data/` (security: no access outside data partition).

Each tool handler:
1. Parses JSON args (use a minimal JSON parser or manual parsing — avoid heavy dependencies)
2. Executes the operation using POSIX syscalls
3. Returns JSON result string

> **Implementation note:** Use a lightweight JSON library. Options:
> - llama.cpp ships with `nlohmann/json.hpp` in `common/` — reuse it
> - Or use `common/json.hpp` from the llama.cpp include path

### Step 5.6: Implement remaining tool modules

Implement in order (each ~50-100 LOC):

1. `tools_process.cpp` — reads /proc to list processes, get info
2. `tools_network.cpp` — reads /sys/class/net, /proc/net/tcp, etc.
3. `tools_system.cpp` — uptime, memory, CPU temp from /sys/class/thermal, shutdown/reboot
4. `tools_config.cpp` — reads/writes JSON files in /data/llamaste/config/
5. `tools_model.cpp` — lists .gguf files in /data/models/, reports sizes

### Step 5.7: Write integration test for all tools

Write `tests/test_tools_integration.cpp` that:
1. Creates a ToolRegistry
2. Registers all tool categories
3. Calls each tool with sample args
4. Verifies JSON responses are well-formed

```bash
g++ -std=c++17 -I src/llamaste -o tests/test_tools_int \
  tests/test_tools_integration.cpp \
  src/llamaste/tools.cpp src/llamaste/tools_fs.cpp \
  src/llamaste/tools_process.cpp src/llamaste/tools_network.cpp \
  src/llamaste/tools_system.cpp src/llamaste/tools_config.cpp \
  src/llamaste/tools_model.cpp
./tests/test_tools_int
```

Expected: all tools execute without crashes, return valid JSON.

### Step 5.8: Commit

```bash
git add src/llamaste/tools* tests/test_tools*
git commit -m "feat: tool registry + fs/process/network/system/config/model tools"
```

---

## Task 6: Agent Loop

**Files:**
- Create: `src/llamaste/agent.h`
- Create: `src/llamaste/agent.cpp`
- Create: `src/llamaste/prompt_builder.h`
- Create: `src/llamaste/prompt_builder.cpp`
- Create: `tests/test_agent.cpp`

### Step 6.1: Write test for agent message parsing

Write `tests/test_agent.cpp`:

```cpp
#include <cassert>
#include <cstdio>
#include "../src/llamaste/agent.h"

int main() {
    // Test: parse tool_call from LLM response
    std::string response = R"({
        "choices": [{
            "message": {
                "tool_calls": [{
                    "id": "call_1",
                    "type": "function",
                    "function": {
                        "name": "system.info",
                        "arguments": "{}"
                    }
                }]
            }
        }]
    })";

    auto calls = parse_tool_calls(response);
    assert(calls.size() == 1);
    assert(calls[0].name == "system.info");
    printf("PASS: parsed tool call\n");

    // Test: build messages array
    ConversationState conv;
    conv.system_prompt = "You are Llamaste.";
    conv.add_user_message("What CPU is this?");

    auto messages = conv.to_messages_json();
    assert(messages.find("system") != std::string::npos);
    assert(messages.find("What CPU") != std::string::npos);
    printf("PASS: messages JSON built\n");

    printf("\nAll agent tests passed.\n");
    return 0;
}
```

### Step 6.2: Run test to verify it fails, then implement

Implement `agent.h` and `agent.cpp` with:
- `ConversationState`: message history, system prompt, add/get messages
- `parse_tool_calls()`: extract tool calls from LLM JSON response
- `build_inference_request()`: construct the request body for llama-server's `/v1/chat/completions`
- Agent loop logic: user message → inference → check for tool calls → dispatch → re-infer → return

### Step 6.3: Implement system prompt builder

Write `src/llamaste/prompt_builder.h` and `prompt_builder.cpp`:

```cpp
#include "hwdetect.h"
#include "tools.h"
#include <string>

std::string build_system_prompt(
    const HardwareInfo& hw,
    const ToolRegistry& tools,
    const std::string& boot_mode
);
```

The system prompt tells the LLM:
- What it is ("You are Llamaste, an AI operating system")
- What hardware it's running on (CPU, RAM, GPU)
- What tools are available (generated from ToolRegistry)
- Rules (confirm before destructive operations, paths restricted to /data)

### Step 6.4: Run tests, verify passing

```bash
g++ -std=c++17 -I src/llamaste -o tests/test_agent \
  tests/test_agent.cpp src/llamaste/agent.cpp src/llamaste/tools.cpp
./tests/test_agent
```

### Step 6.5: Commit

```bash
git add src/llamaste/agent* src/llamaste/prompt_builder* tests/test_agent*
git commit -m "feat: agent loop with tool dispatch and system prompt builder"
```

---

## Task 7: Web UI

**Files:**
- Create: `src/llamaste/web/index.html`
- Create: `src/llamaste/web/chat.js`
- Create: `src/llamaste/web/dashboard.js`
- Create: `src/llamaste/web/style.css`
- Create: `src/llamaste/embed_web.cmake`

### Step 7.1: Build the chat interface

Write `src/llamaste/web/index.html` — single-page app with:
- Chat message area (scrollable)
- Input field + send button
- System dashboard sidebar (CPU, RAM, disk, model, uptime)
- Tool call visualization (expandable cards showing tool name + result)

### Step 7.2: Implement SSE streaming in chat.js

Write `src/llamaste/web/chat.js`:
- Connect to `/v1/chat/completions` with `stream: true`
- Parse SSE `data:` lines
- Render tokens incrementally
- Handle tool calls: show a "calling tool..." indicator, then the result
- Conversation management: new/load/delete conversations

### Step 7.3: Implement dashboard in dashboard.js

Write `src/llamaste/web/dashboard.js`:
- Poll `/llamaste/system` every 5 seconds
- Display: CPU usage, RAM usage, disk usage, loaded model, uptime, IP address
- Temperature display with color coding

### Step 7.4: Style with CSS

Write `src/llamaste/web/style.css`:
- Clean, dark theme (easy on the eyes for a terminal-like OS)
- Responsive layout (works on mobile browsers too)
- Total CSS < 10 KB

### Step 7.5: Create embed_web.cmake script

Write `src/llamaste/embed_web.cmake`:

```cmake
# Converts web files to C byte arrays for embedding in the binary
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html" INDEX_HTML HEX)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/web/chat.js" CHAT_JS HEX)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/web/dashboard.js" DASHBOARD_JS HEX)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/web/style.css" STYLE_CSS HEX)

# Convert hex to C array format
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," INDEX_HTML_HEX "${INDEX_HTML}")
# ... (generate web_embed.h with byte arrays and size constants)

configure_file(web_embed.h.in ${CMAKE_CURRENT_BINARY_DIR}/web_embed.h)
```

### Step 7.6: Test in browser

Build the host binary with a simple HTTP server stub, serve the web UI, open in browser.

### Step 7.7: Commit

```bash
git add src/llamaste/web/ src/llamaste/embed_web.cmake
git commit -m "feat: web UI with chat, dashboard, and SSE streaming"
```

---

## Task 8: Integrate llama-server + Agent + Tools + Web UI

**Files:**
- Rewrite: `src/llamaste/child_main.cpp` (replace stub with real server)
- Modify: `src/llamaste/CMakeLists.txt` (link against llama.cpp)

This is the critical integration task where `child_main()` initializes the actual llama-server, registers our custom HTTP routes, and starts the event loop.

### Step 8.1: Update CMakeLists.txt to build against llama.cpp

```cmake
set(LLAMA_CPP_DIR "" CACHE PATH "Path to llama.cpp source")

if(LLAMA_CPP_DIR)
    add_subdirectory(${LLAMA_CPP_DIR} ${CMAKE_BINARY_DIR}/llama.cpp)
    target_link_libraries(llamaste PRIVATE llama ggml common)
    target_include_directories(llamaste PRIVATE
        ${LLAMA_CPP_DIR}/include
        ${LLAMA_CPP_DIR}/common
        ${LLAMA_CPP_DIR}/ggml/include
        ${LLAMA_CPP_DIR}/examples/server
    )
    target_compile_definitions(llamaste PRIVATE LLAMASTE_HAS_LLAMA=1)
endif()
```

### Step 8.2: Implement child_main with llama-server integration

`child_main.cpp` should:
1. Initialize llama model from `config.model_path`
2. Set up llama context with appropriate params (threads, context size, flash attention)
3. Start httplib HTTP server on `config.http_port`
4. Register llama-server's standard routes (`/v1/chat/completions`, `/v1/models`, `/health`, `/metrics`)
5. Register custom routes:
   - `GET /` → serve embedded web UI (index.html)
   - `GET /chat.js`, `GET /dashboard.js`, `GET /style.css` → serve embedded assets
   - `POST /llamaste/chat` → agent chat endpoint (tool-augmented inference with SSE)
   - `GET /llamaste/system` → system dashboard JSON
   - `GET /llamaste/tools` → list available tools
   - `GET /llamaste/conversations` → list saved conversations
6. Enter server event loop

### Step 8.3: Test on host with small model

```bash
cd /mnt/d/Llamaste/src/llamaste
cmake -B build-host \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLLAMA_CPP_DIR=$HOME/llamaste-build/llama.cpp
cmake --build build-host -j$(nproc)

# Run with test model
./build-host/llamaste --model ~/llamaste-build/test-models/qwen2.5-0.5b-instruct-q4_k_m.gguf
```

Expected: server starts, web UI accessible at http://localhost:80. Chat with "What is 2+2?" returns a response.

### Step 8.4: Test tool calling

In browser, type: "What CPU is this?"

Expected: LLM generates a tool_call for `system.info`, agent dispatches it, result appended, LLM generates a natural language response describing the CPU.

### Step 8.5: Test OpenAI API compatibility

```bash
curl -s http://localhost/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"local","messages":[{"role":"user","content":"Hello"}],"max_tokens":50}'
```

Expected: valid OpenAI-format JSON response.

### Step 8.6: Commit

```bash
git add src/llamaste/child_main.cpp src/llamaste/CMakeLists.txt
git commit -m "feat: integrated llama-server + agent + tools + web UI"
```

---

## Task 9: Network Configuration (Built-in DHCP + mDNS)

**Files:**
- Create: `src/llamaste/net_dhcp.h` / `net_dhcp.cpp`
- Create: `src/llamaste/net_mdns.h` / `net_mdns.cpp`
- Create: `tests/test_net.cpp`

### Step 9.1: Implement minimal DHCP client

Since we have no BusyBox/shell, we need a built-in DHCP client. This is ~200-300 lines of C++ that:
1. Opens a raw UDP socket
2. Sends DHCPDISCOVER broadcast
3. Receives DHCPOFFER
4. Sends DHCPREQUEST
5. Receives DHCPACK
6. Configures the interface via ioctl() or netlink

Alternative: use the kernel's built-in IP autoconfiguration (`ip=dhcp` on cmdline) which handles DHCP before init starts. This is simpler and defers the complexity.

**Recommended approach for Phase 1:** Use `CONFIG_IP_PNP=y` and `CONFIG_IP_PNP_DHCP=y` in the kernel config, and add `ip=dhcp` to the kernel cmdline in grub.cfg. The kernel will configure DHCP before starting PID 1. This is zero code.

### Step 9.2: Implement mDNS responder

~100 lines to respond to `llamaste.local` queries:
1. Join multicast group 224.0.0.251 on port 5353
2. Listen for queries matching "llamaste.local"
3. Respond with the machine's IP

### Step 9.3: Test network in QEMU

Update grub.cfg to add `ip=dhcp` to kernel cmdline. Boot in QEMU with user networking:

```bash
qemu-system-x86_64 -m 4G -smp 4 \
  -drive file=llamaste.img,format=raw,if=virtio \
  -netdev user,id=net0,hostfwd=tcp::8080-:80 \
  -device virtio-net-pci,netdev=net0 \
  -nographic
```

Expected: kernel gets IP via DHCP, web UI accessible at http://localhost:8080 from host.

### Step 9.4: Commit

```bash
git add src/llamaste/net_* tests/test_net* br2-external/board/llamaste/grub.cfg
git commit -m "feat: network config via kernel DHCP + mDNS responder"
```

---

## Task 10: GRUB Configuration

**Files:**
- Rewrite: `br2-external/board/llamaste/grub.cfg`

### Step 10.1: Write production grub.cfg

```
set timeout=5
set default=0

# A/B slot selection (defaults to A)
if [ -z "$llamaste_slot" ]; then
    set llamaste_slot=A
fi

if [ "$llamaste_slot" = "B" ]; then
    set sys_part=4
else
    set sys_part=3
fi

menuentry "Llamaste Server" {
    search --no-floppy --label --set=root SYS-A
    linux /boot/bzImage rootfstype=squashfs ro quiet \
        console=ttyS0 console=tty0 \
        init=/opt/llamaste/llamaste \
        llamaste.mode=server \
        ip=dhcp
}

menuentry "Llamaste Desktop" {
    search --no-floppy --label --set=root SYS-A
    linux /boot/bzImage rootfstype=squashfs ro quiet \
        init=/opt/llamaste/llamaste \
        llamaste.mode=desktop \
        ip=dhcp
}
```

### Step 10.2: Test both BIOS and UEFI boot

```bash
# BIOS
qemu-system-x86_64 -m 4G -drive file=llamaste.img,format=raw,if=virtio -nographic

# UEFI
qemu-system-x86_64 -m 4G \
  -bios /usr/share/OVMF/OVMF_CODE.fd \
  -drive file=llamaste.img,format=raw,if=virtio -nographic
```

Expected: GRUB menu appears, both entries visible, Server boots by default.

### Step 10.3: Commit

```bash
git add br2-external/board/llamaste/grub.cfg
git commit -m "feat: GRUB dual-boot config with A/B slot support"
```

---

## Task 11: Genimage — Final 5-Partition Layout

**Files:**
- Rewrite: `br2-external/board/llamaste/genimage.cfg`
- Modify: `br2-external/board/llamaste/post_image.sh`

### Step 11.1: Write production genimage.cfg

```
image boot.vfat {
    vfat {
        label = "ESP"
        files = {
            "bzImage",
            "EFI",
            "grub"
        }
    }
    size = 256M
}

image rootfs.squashfs {
    squashfs {
        compression = "zstd"
        block-size = 0x40000
    }
}

image data.ext4 {
    ext4 {
        label = "DATA"
    }
    size = 2G
}

image llamaste.img {
    hdimage {
        gpt = true
    }

    partition bios-boot {
        partition-type-uuid = 21686148-6449-6E6F-744E-656564454649
        size = 1M
    }

    partition ESP {
        partition-type-uuid = C12A7328-F81F-11D2-BA4B-00A0C93EC93B
        bootable = true
        image = boot.vfat
        size = 256M
    }

    partition sys-a {
        partition-type-uuid = 4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709
        image = rootfs.squashfs
        size = 256M
    }

    partition sys-b {
        partition-type-uuid = 4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709
        size = 256M
    }

    partition data {
        partition-type-uuid = 3B8F8425-20E0-4C36-B2BC-B9C3D3D6C68E
        image = data.ext4
        autoresize = true
    }
}
```

### Step 11.2: Build final image

```bash
cd ~/llamaste-build/output
make -j$(nproc)
ls -lh images/llamaste.img
```

Expected: `llamaste.img` with 5 GPT partitions. Verify with:

```bash
fdisk -l images/llamaste.img
```

Should show: bios-boot (1M), ESP (256M), sys-a (256M), sys-b (256M), data (2G+).

### Step 11.3: Commit

```bash
git add br2-external/board/llamaste/genimage.cfg br2-external/board/llamaste/post_image.sh
git commit -m "feat: 5-partition GPT genimage layout (A/B ready)"
```

---

## Task 12: End-to-End Testing

### Step 12.1: Create test script

Write `tests/test-e2e-qemu.sh`:

```bash
#!/bin/bash
set -euo pipefail

IMG="${1:-output/images/llamaste.img}"
MODEL="${2:-test-models/qwen2.5-0.5b-instruct-q4_k_m.gguf}"
PORT=8080

echo "=== Llamaste E2E Test Suite ==="

# Inject model into data partition
echo "[setup] Injecting test model..."
# (mount data partition from image, copy model, unmount)

# Start QEMU in background
echo "[boot] Starting QEMU..."
qemu-system-x86_64 -m 4G -smp 4 \
  -drive file="$IMG",format=raw,if=virtio \
  -netdev user,id=net0,hostfwd=tcp::${PORT}-:80 \
  -device virtio-net-pci,netdev=net0 \
  -nographic -daemonize -pidfile /tmp/qemu-llamaste.pid

# Wait for server to be ready
echo "[wait] Waiting for server..."
for i in $(seq 1 60); do
    if curl -sf http://localhost:${PORT}/health > /dev/null 2>&1; then
        echo "[ready] Server up after ${i}s"
        break
    fi
    sleep 1
done

# Test 1: Health check
echo "[test] Health check..."
HEALTH=$(curl -sf http://localhost:${PORT}/health)
echo "  $HEALTH"

# Test 2: Web UI serves
echo "[test] Web UI..."
HTTP_CODE=$(curl -sf -o /dev/null -w '%{http_code}' http://localhost:${PORT}/)
[ "$HTTP_CODE" = "200" ] && echo "  PASS: HTTP 200" || echo "  FAIL: HTTP $HTTP_CODE"

# Test 3: OpenAI API
echo "[test] Chat completions API..."
RESP=$(curl -sf http://localhost:${PORT}/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"local","messages":[{"role":"user","content":"Say hello"}],"max_tokens":20}')
echo "  Response: $(echo $RESP | head -c 200)"

# Test 4: System info tool
echo "[test] System info via agent..."
RESP=$(curl -sf http://localhost:${PORT}/llamaste/chat \
  -H "Content-Type: application/json" \
  -d '{"message":"What CPU is this?"}')
echo "  Response: $(echo $RESP | head -c 200)"

# Test 5: Models endpoint
echo "[test] Models list..."
MODELS=$(curl -sf http://localhost:${PORT}/v1/models)
echo "  $MODELS"

# Test 6: Dashboard data
echo "[test] System dashboard..."
SYS=$(curl -sf http://localhost:${PORT}/llamaste/system)
echo "  $SYS"

# Cleanup
echo "[cleanup] Stopping QEMU..."
kill $(cat /tmp/qemu-llamaste.pid) 2>/dev/null || true

echo ""
echo "=== E2E Tests Complete ==="
```

### Step 12.2: Run the test suite

```bash
chmod +x tests/test-e2e-qemu.sh
./tests/test-e2e-qemu.sh ~/llamaste-build/output/images/llamaste.img
```

### Step 12.3: Verify all success criteria

| # | Criterion | How to verify |
|---|-----------|---------------|
| 1 | Boots in <30s | Measure time from QEMU start to `/health` returning 200 |
| 2 | Web UI chat works | Open browser, send message, get response |
| 3 | Tool calls work | "Show disk usage" → fs.disk_usage tool call |
| 4 | OpenAI API works | curl to /v1/chat/completions |
| 5 | Model auto-selected | Check logs for "Selected model:" line |
| 6 | Supervisor re-forks | Kill child PID, verify server comes back |
| 7 | Conversations persist | Send message, reboot QEMU, verify history |
| 8 | Image < 100 MB | `ls -lh llamaste.img.xz` |

### Step 12.4: Commit

```bash
git add tests/test-e2e-qemu.sh
git commit -m "feat: end-to-end test suite for QEMU verification"
```

### Step 12.5: Create compressed distributable

```bash
xz -9 -k output/images/llamaste.img
ls -lh output/images/llamaste.img.xz
```

Expected: compressed image < 100 MB (kernel ~5 MB + llama-server ~4 MB + web UI ~100 KB + rootfs overhead).

### Step 12.6: Final commit — Phase 1 MVP

```bash
git add -A
git commit -m "milestone: Phase 1 MVP — Llamaste boots and serves AI chat"
```

---

## Summary: File Manifest

```
src/llamaste/
├── main.cpp              # PID 1 entry, model selection, supervisor launch
├── supervisor.h/cpp      # Fork child, watchdog, crash recovery
├── init.h/cpp            # Mount filesystems, tune perf, parse cmdline
├── hwdetect.h/cpp        # CPU/RAM/GPU detection from /proc and /sys
├── child_main.cpp        # Inference child: llama-server + agent + HTTP
├── agent.h/cpp           # Conversation state, tool dispatch, multi-turn
├── prompt_builder.h/cpp  # Dynamic system prompt from hardware + tools
├── tools.h/cpp           # Tool registry, dispatch, OpenAI JSON generation
├── tools_fs.cpp          # Filesystem tools (list, read, write, delete, search)
├── tools_process.cpp     # Process tools (list, info from /proc)
├── tools_network.cpp     # Network tools (interfaces, connections)
├── tools_system.cpp      # System tools (info, uptime, temp, shutdown)
├── tools_config.cpp      # Config tools (get, set, list, reset)
├── tools_model.cpp       # Model tools (list, info, load, download)
├── net_mdns.h/cpp        # mDNS responder for llamaste.local
├── embed_web.cmake       # Converts web/ files to C byte arrays
├── CMakeLists.txt        # Build system
└── web/
    ├── index.html        # SPA: chat + dashboard
    ├── chat.js           # SSE streaming, tool call rendering
    ├── dashboard.js      # System stats polling
    └── style.css         # Dark theme, responsive

br2-external/
├── external.desc
├── external.mk
├── Config.in
├── configs/llamaste_x86_64_defconfig
├── package/llamaste/
│   ├── Config.in
│   └── llamaste.mk
└── board/llamaste/
    ├── linux.config
    ├── genimage.cfg
    ├── grub.cfg
    ├── post_build.sh
    └── post_image.sh

tests/
├── test_hwdetect.cpp
├── test_tools.cpp
├── test_tools_integration.cpp
├── test_agent.cpp
├── test_net.cpp
└── test-e2e-qemu.sh
```

---

## Estimated Task Sizes

| Task | Description | Estimated LOC | Key Risk |
|------|-------------|---------------|----------|
| 1 | Buildroot external tree | ~200 (configs) | Buildroot learning curve |
| 2 | Kernel config | ~100 (config) | Missing drivers |
| 3 | Stock llama-server | ~50 (.mk) | musl static linking |
| 4 | PID 1 supervisor | ~500 C++ | Fork/signal correctness |
| 5 | Tools system | ~1500 C++ | JSON parsing, path security |
| 6 | Agent loop | ~600 C++ | Tool call parsing accuracy |
| 7 | Web UI | ~800 JS/HTML/CSS | SSE streaming edge cases |
| 8 | Server integration | ~400 C++ | httplib route conflicts |
| 9 | Network | ~200 C++ | DHCP timing, mDNS |
| 10 | GRUB | ~30 (config) | BIOS vs UEFI |
| 11 | Genimage | ~50 (config) | Partition alignment |
| 12 | E2E testing | ~100 (bash) | QEMU networking |

**Total: ~4,500 LOC new code + ~400 lines of config**

---

Plan complete and saved to `docs/plans/2026-02-26-phase1-implementation-plan.md`. Two execution options:

**1. Subagent-Driven (this session)** — I dispatch fresh subagent per task, review between tasks, fast iteration

**2. Parallel Session (separate)** — Open new session with executing-plans, batch execution with checkpoints

Which approach?
