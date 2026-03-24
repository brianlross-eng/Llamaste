# Kernel Module Loading Strategies for Broad Hardware Support

**Date**: 2026-03-23
**Context**: Llamaste — minimal Buildroot x86_64 Linux, single C++ binary as PID 1, no shell, no init system, no modprobe/kmod tools.
**Problem**: Current approach uses a hardcoded list of ~80 module paths in init.cpp with direct finit_module() syscall. Doesn't scale to hundreds of hardware variants.

---

## Table of Contents

1. [Current Approach & Its Limitations](#1-current-approach--its-limitations)
2. [depmod + modprobe vs finit_module()](#2-depmod--modprobe-vs-finit_module)
3. [libkmod — The Library Behind modprobe](#3-libkmod--the-library-behind-modprobe)
4. [udev/eudev-Based Module Loading](#4-udeveudev-based-module-loading)
5. [modules.dep / modules.alias — The Kernel's Module Dependency System](#5-modulesdep--modulesalias--the-kernels-module-dependency-system)
6. [PCI/USB Device Enumeration & Modalias Matching](#6-pciusb-device-enumeration--modalias-matching)
7. [Hybrid Approach — Built-in + eudev Modules](#7-hybrid-approach--built-in--eudev-modules)
8. [Module Compression](#8-module-compression)
9. [Initramfs Module Loading — How Mainstream Distros Do It](#9-initramfs-module-loading--how-mainstream-distros-do-it)
10. [Recommended Strategy for Llamaste](#10-recommended-strategy-for-llamaste)

---

## 1. Current Approach & Its Limitations

### What We Do Now

In `src/llamaste/init.cpp`, `init_load_modules()` contains a hardcoded C array of ~80 module paths:

```cpp
const char* module_paths[] = {
    "drivers/net/wireless/intel/iwlwifi/iwlwifi.ko",
    "drivers/net/wireless/realtek/rtw88/rtw88_core.ko",
    // ... ~78 more entries
    nullptr
};

for (int i = 0; module_paths[i]; i++) {
    std::string full_path = mod_base + "/kernel/" + module_paths[i];
    int fd = open(full_path.c_str(), O_RDONLY | O_CLOEXEC);
    int ret = syscall(SYS_finit_module, fd, "", 0);
    close(fd);
}
```

### Limitations

1. **No dependency resolution** — modules must be listed in exact dependency order manually. If `rtw88_8821ce.ko` depends on `rtw88_core.ko` and `rtw88_pci.ko`, all three must appear in the correct order.
2. **No hardware detection** — loads ALL modules regardless of what hardware is present. Wastes time loading 80 modules when only 2 are needed.
3. **No modalias matching** — can't discover new/unknown hardware at boot.
4. **Maintenance burden** — every new WiFi/Ethernet/GPU chip requires manually finding the module path, its dependencies, and adding them in order.
5. **No error resilience** — if a module path changes between kernel versions, it silently fails.
6. **No compressed module support** — can't use .ko.xz or .ko.zst files.

---

## 2. depmod + modprobe vs finit_module()

### depmod

`depmod` is a **build-time tool** (runs on the build host, not the target). It scans all `.ko` files under `/lib/modules/$(uname -r)/` and generates index files:

| File | Purpose |
|------|---------|
| `modules.dep` | Module dependency graph (text format) |
| `modules.dep.bin` | Binary index of dependencies (fast lookup) |
| `modules.alias` | Modalias → module name mapping |
| `modules.alias.bin` | Binary index of aliases |
| `modules.symbols` | Exported symbols → module mapping |
| `modules.symbols.bin` | Binary index of symbols |
| `modules.builtin` | List of built-in modules |
| `modules.devname` | Device name → module mapping |
| `modules.softdep` | Soft dependencies |

**Key insight**: `depmod` must run after kernel modules are installed. In Buildroot, this happens automatically during `make`. The index files end up in the rootfs at `/lib/modules/<version>/`.

### modprobe

`modprobe` is a **runtime tool** that:
1. Reads `modules.dep.bin` to resolve dependencies
2. Reads `modules.alias.bin` to resolve modalias strings to module names
3. Loads modules in dependency order using `finit_module()` or `init_module()`
4. Honors blacklists from `/etc/modprobe.d/`

**modprobe is a symlink to kmod**. The `kmod` binary provides: `modprobe`, `insmod`, `rmmod`, `depmod`, `modinfo`, `lsmod` — all via argv[0] detection (like BusyBox).

### finit_module() syscall

```c
int syscall(SYS_finit_module, int fd, const char *param_values, int flags);
```

- Takes an open file descriptor (not a path)
- `param_values`: space-delimited module parameters (e.g., `"debug=1"`)
- `flags`: `0` for normal, `MODULE_INIT_IGNORE_MODVERSIONS` (1), `MODULE_INIT_IGNORE_VERMAGIC` (2), `MODULE_INIT_COMPRESSED_FILE` (4, since Linux 5.17)
- Returns 0 on success, -1 on error with errno
- Does NOT resolve dependencies — caller must handle load order
- Does NOT decompress modules unless `MODULE_INIT_COMPRESSED_FILE` flag is set AND `CONFIG_MODULE_DECOMPRESS` is enabled in the kernel

### Tradeoff Summary

| Aspect | Raw finit_module() | modprobe/kmod |
|--------|-------------------|---------------|
| Dependencies | Manual ordering | Automatic |
| Alias resolution | None | Automatic via modules.alias |
| Blacklists | None | Honored |
| Compressed modules | Manual flag | Automatic |
| Shell required | No | No (kmod is a standalone binary) |
| Binary size | 0 (syscall) | ~200-400KB (kmod static) |
| Library approach | N/A | libkmod (~150KB shared) |

---

## 3. libkmod — The Library Behind modprobe

### Overview

libkmod is a C library (LGPL-2.1) shipped with the kmod project. It provides the same functionality as modprobe but as a linkable library — **no shell or external binary needed**. This is the most relevant option for Llamaste.

### Key API Functions

```c
#include <libkmod.h>

// Create context (reads /lib/modules/<version>/ indexes)
struct kmod_ctx *ctx = kmod_new(NULL, NULL);

// Load binary indexes (modules.dep.bin, modules.alias.bin)
kmod_load_resources(ctx);

// Lookup module by alias (e.g., modalias string from sysfs)
struct kmod_list *list = NULL;
kmod_module_new_from_lookup(ctx, "pci:v00008086d00002723sv*sd*bc02sc80i00", &list);

// Load module with automatic dependency resolution
kmod_list_foreach(l, list) {
    struct kmod_module *mod = kmod_module_get_module(l);
    // KMOD_PROBE_APPLY_BLACKLIST honors /etc/modprobe.d/ blacklists
    kmod_module_probe_insert_module(mod,
        KMOD_PROBE_APPLY_BLACKLIST, NULL, NULL, NULL, NULL);
    kmod_module_unref(mod);
}
kmod_module_unref_list(list);

// Cleanup
kmod_unref(ctx);
```

### How It Works Internally

1. **`kmod_new(dirname, config_paths)`** — Creates a context. `dirname` defaults to `/lib/modules/$(uname -r)/`. `config_paths` defaults to `/etc/modprobe.d/` and `/lib/modprobe.d/`.

2. **`kmod_load_resources(ctx)`** — Memory-maps the binary index files (`modules.dep.bin`, `modules.alias.bin`, `modules.symbols.bin`). These are generated by `depmod` at build time. The binary format allows O(log n) lookups without parsing text files.

3. **`kmod_module_new_from_lookup(ctx, alias, &list)`** — Searches indexes for modules matching the alias. For a modalias like `pci:v00008086d00002723...`, it searches `modules.alias.bin` using fnmatch-style glob matching. Returns a list of matching modules.

4. **`kmod_module_probe_insert_module(mod, flags, ...)`** — The big one. This:
   - Resolves the full dependency chain from `modules.dep.bin`
   - Loads dependencies first (recursive)
   - Opens the `.ko` file and calls `finit_module()` syscall
   - Handles compressed modules (`.ko.xz`, `.ko.zst`, `.ko.gz`) — decompresses in userspace or passes to kernel with `MODULE_INIT_COMPRESSED_FILE`
   - Honors blacklists and softdeps
   - Handles module parameters from config files

5. **`kmod_validate_resources(ctx)`** — Checks if index files have changed on disk (stat comparison). Used by eudev to detect kernel updates.

### Binary Size & Dependencies

- **libkmod shared library**: ~150-200KB (`.so`)
- **kmod static binary** (all tools): ~300-500KB depending on features
- **Dependencies**: libc only (no external deps for basic operation). Optional: zlib (for .ko.gz), liblzma (for .ko.xz), libzstd (for .ko.zst)
- **Buildroot package**: `BR2_PACKAGE_KMOD=y` — already available, provides both library and tools
- **License**: LGPL-2.1 for libkmod (compatible with Llamaste's linking)

### Integration Options for Llamaste

**Option A: Link libkmod into the llamaste binary**
- Add `find_package(PkgConfig)` + `pkg_check_modules(KMOD libkmod)` to CMakeLists.txt
- Link with `-lkmod`
- Call libkmod API directly from init.cpp
- Size cost: ~150KB added to binary (shared) or ~300KB (static)

**Option B: Use kmod binary via fork/exec**
- Already used pattern in Llamaste (fork/execv for wpa_supplicant, dhcpcd)
- `fork(); execv("/sbin/modprobe", {"modprobe", alias, NULL});`
- Requires kmod binary in rootfs (~300KB)
- No code changes to llamaste binary itself

**Option C: Embed minimal libkmod-equivalent in llamaste**
- Parse modules.dep and modules.alias text files directly
- Use fnmatch() for alias matching
- Call finit_module() for actual loading
- No external dependency, but must maintain parser code
- ~200-400 lines of C++

### Recommendation

**Option A (link libkmod)** is the clear winner:
- Battle-tested code, handles edge cases (compressed modules, softdeps, blacklists)
- Tiny size overhead (~150KB)
- LGPL-2.1 compatible
- Already in Buildroot
- eudev already uses it (see section 4)

---

## 4. udev/eudev-Based Module Loading

### How eudev Triggers Module Loading

Llamaste already uses eudev (`BR2_ROOTFS_DEVICE_CREATION_DYNAMIC_EUDEV=y`). eudev has a **built-in kmod integration** that handles module loading automatically.

The mechanism is elegantly simple. The key udev rule (from `80-drivers.rules`):

```
DRIVER!="?*", ENV{MODALIAS}=="?*", RUN{builtin}="kmod load $env{MODALIAS}"
```

This single rule says: "For any device that has a MODALIAS but no driver bound yet, run the built-in kmod loader with that modalias."

### The eudev kmod Builtin (udev-builtin-kmod.c)

eudev's kmod builtin uses libkmod directly (source: `eudev/src/udev/udev-builtin-kmod.c`):

```c
static struct kmod_ctx *ctx = NULL;

static int load_module(struct udev *udev, const char *alias) {
    struct kmod_list *list = NULL;
    kmod_module_new_from_lookup(ctx, alias, &list);
    kmod_list_foreach(l, list) {
        struct kmod_module *mod = kmod_module_get_module(l);
        kmod_module_probe_insert_module(mod,
            KMOD_PROBE_APPLY_BLACKLIST, NULL, NULL, NULL, NULL);
        kmod_module_unref(mod);
    }
    kmod_module_unref_list(list);
    return 0;
}

static int builtin_kmod_init(struct udev *udev) {
    ctx = kmod_new(NULL, NULL);
    kmod_load_resources(ctx);
    return 0;
}
```

### What's Needed to Make eudev Auto-Load Modules

For eudev to auto-load modules for detected hardware, you need:

1. **eudev daemon running** — `udevd --daemon` (already started by Llamaste in child_main.cpp)
2. **kmod package installed** — provides libkmod + `depmod` index files (add `BR2_PACKAGE_KMOD=y` to defconfig)
3. **depmod run at build time** — Buildroot does this automatically when kmod is enabled
4. **modules.dep.bin and modules.alias.bin present** — in `/lib/modules/<version>/` on the target
5. **The udev rule `80-drivers.rules`** — ships with eudev by default
6. **udevadm trigger --action=add** — replays device add events for coldplug (devices present before udevd started)

### Coldplug vs Hotplug

- **Hotplug**: When a USB device is inserted AFTER udevd is running, the kernel sends a uevent → udevd processes it → kmod builtin loads the module. Fully automatic.
- **Coldplug**: Devices present at boot (PCI cards, built-in WiFi) need their uevents replayed. Running `udevadm trigger --action=add` after udevd starts causes it to re-process all existing devices in `/sys/`. This is what `udevadm trigger` does — it writes "add" to each device's `uevent` file in sysfs.

### What Llamaste Already Does

Llamaste already runs:
- `udevd --daemon` (child_main.cpp)
- `udevadm trigger --action=add --subsystem-match=input` (for keyboard in desktop mode)
- `udevadm settle` (wait for udevd to finish processing)

**The missing piece**: Running `udevadm trigger --action=add` for ALL subsystems (not just `input`), and having kmod + depmod indexes available. With those two changes, eudev would auto-load ALL hardware modules.

### Required Changes

1. Add `BR2_PACKAGE_KMOD=y` to Buildroot defconfig
2. Ensure `depmod` runs during build (Buildroot handles this)
3. Change `udevadm trigger` call to not filter by subsystem:
   ```
   udevadm trigger --action=add
   ```
4. Remove (or keep as fallback) the hardcoded module list in init_load_modules()

---

## 5. modules.dep / modules.alias — The Kernel's Module Dependency System

### modules.dep

Generated by `depmod`, maps each module to its dependencies:

```
# Text format (modules.dep):
kernel/drivers/net/wireless/realtek/rtw88/rtw88_8821ce.ko: kernel/drivers/net/wireless/realtek/rtw88/rtw88_8821c.ko kernel/drivers/net/wireless/realtek/rtw88/rtw88_pci.ko kernel/drivers/net/wireless/realtek/rtw88/rtw88_core.ko
```

Each line: `module_path: dep1 dep2 dep3`

The binary version (`modules.dep.bin`) uses a hash-based index for O(1) lookups.

### modules.alias

Maps device modalias patterns to module names:

```
# Text format (modules.alias):
alias pci:v000010ECd0000C821sv*sd*bc*sc*i* rtw88_8821ce
alias pci:v00008086d00002723sv*sd*bc02sc80i00 iwlwifi
alias usb:v0BDAp8153d*dc*dsc*dp*ic*isc*ip*in* r8152
```

Format: `alias <glob_pattern> <module_name>`

The glob pattern uses `*` wildcards and is matched against the device's modalias string from sysfs using fnmatch().

### modules.alias Generation

`depmod` extracts aliases from each `.ko` file's `.modinfo` section. Kernel module source uses the `MODULE_DEVICE_TABLE()` macro:

```c
// In kernel driver source:
static const struct pci_device_id rtw88_pci_id_table[] = {
    { PCI_DEVICE(0x10EC, 0xC821) },  // RTL8821CE
    { },
};
MODULE_DEVICE_TABLE(pci, rtw88_pci_id_table);
```

The build system converts this to modalias strings embedded in the `.ko` file. `depmod` extracts them into `modules.alias`.

### How to Parse modules.alias in C++ (if not using libkmod)

```cpp
// Simple text parser (libkmod handles the binary format)
void load_matching_modules(const std::string& device_modalias) {
    std::ifstream f("/lib/modules/" + kernel_release + "/modules.alias");
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        // Parse: "alias <pattern> <module_name>"
        auto parts = split(line);
        if (parts.size() == 3 && parts[0] == "alias") {
            if (fnmatch(parts[1].c_str(), device_modalias.c_str(), 0) == 0) {
                load_module_with_deps(parts[2]);
            }
        }
    }
}
```

However, using libkmod is strongly preferred — it handles the binary index format (much faster), compressed modules, blacklists, and all edge cases.

---

## 6. PCI/USB Device Enumeration & Modalias Matching

### Reading Modalias from sysfs

Every device in `/sys/` that has a driver-binding interface exposes a `modalias` file:

```bash
# PCI devices:
/sys/bus/pci/devices/0000:01:00.0/modalias
# Contains: pci:v000010ECd0000C821sv0000103Csd00008317bc02sc80i00

# USB devices:
/sys/bus/usb/devices/1-1/modalias
# Contains: usb:v0BDAp8153d3100dc00dsc00dp00icFFiscFFipFFin00
```

### Scanning All Devices

```cpp
#include <dirent.h>
#include <fnmatch.h>

void scan_and_load_modules(struct kmod_ctx *ctx) {
    const char* bus_paths[] = {
        "/sys/bus/pci/devices",
        "/sys/bus/usb/devices",
        "/sys/bus/sdio/devices",
        "/sys/bus/platform/devices",
        nullptr
    };

    for (int b = 0; bus_paths[b]; b++) {
        DIR *dir = opendir(bus_paths[b]);
        if (!dir) continue;

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;

            // Read modalias file
            std::string path = std::string(bus_paths[b]) + "/"
                             + entry->d_name + "/modalias";
            int fd = open(path.c_str(), O_RDONLY);
            if (fd < 0) continue;

            char alias[512];
            ssize_t n = read(fd, alias, sizeof(alias) - 1);
            close(fd);
            if (n <= 0) continue;
            alias[n] = '\0';
            // Strip trailing newline
            if (alias[n-1] == '\n') alias[n-1] = '\0';

            // Use libkmod to find and load matching module
            struct kmod_list *list = NULL;
            kmod_module_new_from_lookup(ctx, alias, &list);
            struct kmod_list *l;
            kmod_list_foreach(l, list) {
                struct kmod_module *mod = kmod_module_get_module(l);
                int err = kmod_module_probe_insert_module(mod,
                    KMOD_PROBE_APPLY_BLACKLIST, NULL, NULL, NULL, NULL);
                if (err == 0)
                    fprintf(stderr, "[init] Loaded %s for %s\n",
                            kmod_module_get_name(mod), alias);
                kmod_module_unref(mod);
            }
            kmod_module_unref_list(list);
        }
        closedir(dir);
    }
}
```

### Modalias Format by Bus Type

| Bus | Modalias Format | Example |
|-----|----------------|---------|
| PCI | `pci:vVVVVVVVVdDDDDDDDDsvSSSSSSSSsdSSSSSSSS bcBBscSSiII` | `pci:v000010ECd0000C821sv*sd*bc02sc80i00` |
| USB | `usb:vVVVVpPPPPdDDDDdcDCdscDSCdpDPicICiscISCipIPinIN` | `usb:v0BDAp8153d3100dc00dsc00dp00ic*isc*ip*in*` |
| SDIO | `sdio:cCCvVVVVdDDDD` | `sdio:c07v024Cd0523` |
| Platform | `platform:NAME` | `platform:i8042` |
| ACPI | `acpi:NAME:` | `acpi:PNP0303:` |

### Filtering: sit0 and Virtual Interfaces

When scanning `/sys/bus/`, filter out virtual devices:
- `/sys/bus/pci/devices/*/modalias` — always real hardware
- `/sys/class/net/*/device` — if symlink exists, it's physical (sit0 has no `device` symlink)
- Skip entries without a `modalias` file

---

## 7. Hybrid Approach — Built-in + eudev Modules

### Why Hybrid?

Some subsystems MUST have drivers available before the root filesystem (and therefore module files) is accessible. These must be compiled built-in (`=y`). Everything else can be modules (`=m`) loaded by eudev after boot.

### Minimum Built-in Set for Llamaste

These must be `=y` (built-in) in the kernel config because they're needed before modules can be loaded:

#### Critical — Needed Before Root FS

| Category | Config Options | Why |
|----------|---------------|-----|
| **Block/Storage** | `CONFIG_ATA=y`, `CONFIG_ATA_PIIX=y`, `CONFIG_SATA_AHCI=y`, `CONFIG_BLK_DEV_NVME=y`, `CONFIG_BLK_DEV_SD=y`, `CONFIG_SCSI=y` | Must read root filesystem |
| **Filesystem** | `CONFIG_SQUASHFS=y`, `CONFIG_EXT4_FS=y`, `CONFIG_VFAT_FS=y`, `CONFIG_OVERLAY_FS=y` | Must mount root and overlay |
| **Loop device** | `CONFIG_BLK_DEV_LOOP=y` | Needed for squashfs pivot |
| **Device infrastructure** | `CONFIG_DEVTMPFS=y`, `CONFIG_DEVTMPFS_MOUNT=y` | /dev management |
| **EFI** | `CONFIG_EFI=y`, `CONFIG_EFI_STUB=y`, `CONFIG_EFI_PARTITION=y` | EFI boot |
| **DRM (basic display)** | `CONFIG_DRM=y`, `CONFIG_DRM_SIMPLEDRM=y`, `CONFIG_DRM_FBDEV_EMULATION=y` | Console/framebuffer output during boot |
| **USB core** | `CONFIG_USB=y`, `CONFIG_USB_XHCI_HCD=y`, `CONFIG_USB_EHCI_HCD=y`, `CONFIG_USB_OHCI_HCD=y`, `CONFIG_USB_STORAGE=y` | USB keyboards, storage, USB-boot |
| **Input core** | `CONFIG_INPUT=y`, `CONFIG_INPUT_EVDEV=y`, `CONFIG_HID=y`, `CONFIG_USB_HID=y` | Keyboard during early boot |
| **Network core** | `CONFIG_NET=y`, `CONFIG_INET=y`, `CONFIG_NETDEVICES=y`, `CONFIG_CFG80211=y` | Networking infrastructure |

#### Can Be Modules (=m) — Loaded by eudev

| Category | Examples | Why Module is OK |
|----------|----------|-----------------|
| **WiFi drivers** | iwlwifi, rtw88, rtw89, ath10k, ath11k, mt76, brcmfmac | Not needed until after boot |
| **USB Ethernet** | r8152, asix, cdc_ether, cdc_ncm, rndis_host | Hotplug after boot |
| **GPU drivers** | i915, amdgpu, nouveau | Desktop mode starts after boot |
| **Sound** | snd-hda-intel, snd-usb-audio | Not boot-critical |
| **Bluetooth** | btusb, btintel, btrtl | Post-boot only |
| **Webcam/Media** | uvcvideo, snd-usb-audio | Post-boot only |
| **Misc USB** | usb-storage (specific), specific HID | Hotplug |

### Size Impact

Moving drivers from built-in to modules typically **reduces bzImage by 5-15MB** and shifts that to the `/lib/modules/` directory as individual `.ko` files. With compression (xz/zstd), modules are significantly smaller than their built-in equivalent.

---

## 8. Module Compression

### Kernel Support

Since Linux 5.17, the kernel can decompress modules in-kernel via `finit_module()` with `MODULE_INIT_COMPRESSED_FILE` flag:
- gzip (CONFIG_MODULE_DECOMPRESS + CONFIG_MODULE_COMPRESS_GZIP) — since 5.17
- xz (CONFIG_MODULE_DECOMPRESS + CONFIG_MODULE_COMPRESS_XZ) — since 5.17
- zstd (CONFIG_MODULE_DECOMPRESS + CONFIG_MODULE_COMPRESS_ZSTD) — since 6.2

### libkmod/kmod Support

libkmod can decompress modules in **userspace** before passing to `init_module()`. It links against zlib/liblzma/libzstd as needed. This works on ALL kernel versions, not just 5.17+.

### Compression Comparison

| Format | Extension | Typical Ratio | Decompression Speed | Kernel Support |
|--------|-----------|---------------|--------------------| --------------|
| gzip | .ko.gz | ~60% reduction | Fast (~500 MB/s) | 5.17+ |
| xz | .ko.xz | ~70% reduction | Slow (~100 MB/s) | 5.17+ |
| zstd | .ko.zst | ~65% reduction | Very fast (~1.5 GB/s) | 6.2+ |
| None | .ko | Baseline | N/A | All |

### Buildroot Configuration

```
# In Buildroot defconfig:
BR2_LINUX_KERNEL_MODULE_COMPRESS_XZ=y    # or _GZIP or _ZSTD
```

This tells Buildroot to compress all `.ko` files after installation. `depmod` handles compressed modules transparently — the index files reference the module name without the compression extension.

### Recommendation for Llamaste

**zstd** is the best choice if using kernel 6.2+:
- Best decompression speed (important for boot time)
- Good compression ratio
- Kernel can decompress in-kernel (no userspace dependency)

If targeting older kernels, **xz** gives best compression ratio, and libkmod handles decompression in userspace.

### Size Savings Example

A typical WiFi-heavy module set:

| Scenario | Total Size |
|----------|-----------|
| Uncompressed .ko files | ~25 MB |
| gzip compressed | ~10 MB |
| xz compressed | ~7.5 MB |
| zstd compressed | ~9 MB |

---

## 9. Initramfs Module Loading — How Mainstream Distros Do It

### The Problem

Mainstream distros face the same chicken-and-egg: they need storage drivers to mount the root filesystem, but modules live ON the root filesystem. The solution is **initramfs** — a small in-memory filesystem loaded by the bootloader alongside the kernel.

### Alpine Linux (mkinitfs)

Alpine's `mkinitfs` creates a minimal initramfs containing:
1. BusyBox (shell + utilities)
2. A small set of kernel modules (storage, filesystem, crypto)
3. An init script that loads modules, mounts root, and pivots

Module selection is based on "features" files (`/etc/mkinitfs/features.d/`):
```
# base.modules:
kernel/drivers/block
kernel/drivers/ata
kernel/drivers/nvme
kernel/drivers/scsi
kernel/fs/ext4
kernel/fs/squashfs
```

The init script uses modprobe to load them, relying on depmod indexes included in the initramfs.

### Arch Linux (mkinitcpio)

mkinitcpio uses a hook-based system:
1. **autodetect hook**: Scans running system's loaded modules (`lsmod`) to determine which to include
2. **block hook**: Includes all block device drivers
3. **filesystems hook**: Includes filesystem modules for detected fstab entries
4. **udev hook**: Includes miniature udevd in initramfs for coldplug

The initramfs contains a small udevd + modprobe + modules.dep, enabling full hardware auto-detection even in the initramfs phase.

### dracut (Fedora/RHEL)

dracut is the most sophisticated:
1. Includes a full udevd in the initramfs
2. Uses `udevadm trigger` for coldplug
3. Supports network boot (iSCSI, NBD)
4. Can generate "generic" initramfs with ALL storage drivers

### Relevance to Llamaste

Llamaste's approach is unique — it doesn't use a traditional initramfs. Instead:
- Critical storage drivers are **built-in** to bzImage
- The C++ binary IS the init (PID 1)
- After squashfs pivot, `/lib/modules/` becomes available
- Module loading happens post-pivot

This means Llamaste's "initramfs phase" modules must be built-in, and everything else can be handled by eudev post-pivot. No need for a separate initramfs module loading mechanism.

---

## 10. Recommended Strategy for Llamaste

### Phase 1: Enable eudev Module Auto-Loading (Quick Win)

**Changes required:**
1. Add to Buildroot defconfig:
   ```
   BR2_PACKAGE_KMOD=y
   BR2_PACKAGE_KMOD_TOOLS=y  # for depmod at build time
   ```

2. Verify depmod runs during build (check for `/lib/modules/<ver>/modules.dep.bin` in rootfs)

3. In `child_main.cpp`, change the udevadm trigger call to be broader:
   ```cpp
   // After udevd starts, trigger ALL subsystems (not just input)
   fork_exec("/sbin/udevadm", {"udevadm", "trigger", "--action=add"});
   fork_exec("/sbin/udevadm", {"udevadm", "settle", "--timeout=10"});
   ```

4. Keep `init_load_modules()` as a **fallback** for pre-eudev module loading (storage/display modules needed before udevd starts).

**Result**: eudev automatically loads the correct WiFi/Ethernet/GPU/sound modules based on detected hardware. No more maintaining a hardcoded list.

### Phase 2: Slim Down Built-in Modules

Move WiFi, USB Ethernet, GPU, sound, and Bluetooth drivers from `=y` to `=m` in linux.config. This:
- Reduces bzImage size by 5-15MB
- Improves boot time (fewer built-in init functions)
- Modules only load when hardware is present

Keep built-in: storage (AHCI, NVMe, SCSI, USB storage), filesystem (squashfs, ext4, vfat), USB core, input core, DRM core + simpledrm.

### Phase 3: Add libkmod to Llamaste Binary (Optional Enhancement)

Link libkmod into the llamaste binary for programmatic module loading:
- Use `kmod_module_new_from_lookup()` for modalias-based loading
- Useful for the `init_load_modules()` fallback path
- Enables compressed module support
- ~150KB size overhead

### Phase 4: Module Compression (Optional)

Enable `CONFIG_MODULE_COMPRESS_ZSTD=y` + `CONFIG_MODULE_DECOMPRESS=y`:
- Reduces `/lib/modules/` from ~25MB to ~9MB
- Kernel decompresses on load (no userspace dependency)
- Or let libkmod handle decompression for older kernels

### Migration Path

```
Current state:    80 hardcoded finit_module() calls, all WiFi built-in
                     ↓
Phase 1:          eudev auto-loads modules, hardcoded list as fallback
                     ↓
Phase 2:          WiFi/GPU/sound as modules (=m), smaller bzImage
                     ↓
Phase 3:          libkmod in binary for smart fallback loading
                     ↓
Phase 4:          Compressed modules, minimal rootfs size
```

### Risk Mitigation

1. **Fallback**: Keep `init_load_modules()` with the hardcoded list as a fallback if eudev module loading fails. Only remove it after extensive hardware testing.

2. **Boot order**: Ensure udevd starts AFTER squashfs pivot (modules must be accessible). Current code already does this correctly.

3. **depmod at build time**: If depmod doesn't run, modules.alias.bin won't exist and eudev can't load modules. Verify in CI/build scripts.

4. **Module blacklisting**: Create `/etc/modprobe.d/blacklist.conf` for any problematic modules (e.g., `blacklist amdgpu` if it causes issues on non-AMD hardware).

---

## Appendix A: Buildroot Defconfig Changes

```diff
+# Module loading infrastructure
+BR2_PACKAGE_KMOD=y
+BR2_PACKAGE_KMOD_TOOLS=y
+
+# Module compression (optional, requires kernel 6.2+ for zstd)
+# BR2_LINUX_KERNEL_MODULE_COMPRESS_ZSTD=y
```

## Appendix B: Linux Kernel Config Changes

```diff
 # Storage (keep built-in)
 CONFIG_ATA=y
 CONFIG_SATA_AHCI=y
 CONFIG_BLK_DEV_NVME=y
 CONFIG_BLK_DEV_SD=y

 # WiFi (change to module)
-CONFIG_IWLWIFI=y
-CONFIG_IWLMVM=y
+CONFIG_IWLWIFI=m
+CONFIG_IWLMVM=m
-CONFIG_RTW88=y
-CONFIG_RTW88_8821CE=y
+CONFIG_RTW88=m
+CONFIG_RTW88_8821CE=m
 # ... etc for all WiFi drivers

+# Module decompression (optional)
+# CONFIG_MODULE_DECOMPRESS=y
+# CONFIG_MODULE_COMPRESS_ZSTD=y

 # Keep built-in
 CONFIG_MODULES=y
 CONFIG_MODULE_UNLOAD=y
```

## Appendix C: Key File References

| File | Purpose |
|------|---------|
| `/lib/modules/<ver>/modules.dep.bin` | Binary dependency index |
| `/lib/modules/<ver>/modules.alias.bin` | Binary modalias→module index |
| `/lib/modules/<ver>/modules.dep` | Text dependency list |
| `/lib/modules/<ver>/modules.alias` | Text modalias→module list |
| `/etc/modprobe.d/*.conf` | Module blacklists and options |
| `/sys/bus/pci/devices/*/modalias` | PCI device modalias strings |
| `/sys/bus/usb/devices/*/modalias` | USB device modalias strings |
| eudev rule `80-drivers.rules` | Triggers kmod load on modalias match |
| eudev source `udev-builtin-kmod.c` | libkmod integration in eudev |
