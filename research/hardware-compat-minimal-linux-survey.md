# Hardware Compatibility Strategies in Minimal/Specialized Linux Distributions

**Research Date**: 2026-03-23
**Purpose**: Survey how other minimal and specialized Linux distributions handle broad hardware compatibility, to inform Llamaste's hardware support strategy.

---

## 1. Alpine Linux

### Kernel Configuration Strategy
- **Multiple kernel flavors**: `linux-lts` (generous hardware selection), `linux-virt` (VM-only drivers), `linux-stable` (latest stable with broad hardware), `linux-rpi` (Raspberry Pi specific), `linux-openpax` (security-hardened).
- The LTS kernel is configured for "a generous selection of hardware" — essentially a curated subset of allmodconfig. Most drivers are built as modules (=m), not built-in.
- The virt kernel strips out most physical hardware drivers, keeping only commonly-virtualized subset (virtio, etc.), resulting in a much smaller image for cloud/VM use.

### Module Loading Mechanism
- Uses `nlplug-findfs` for hardware discovery during boot (custom Alpine tool).
- The `/init` script loads modules specified by `modules=` kernel cmdline, then loads `/etc/modules` contents.
- Modules required for root mount must be in initramfs or built-in.
- Blacklisting via `blacklist=` kernel cmdline → writes to `/etc/modprobe.d/boot-opt-blacklist.conf`.
- **AKMS** (Alpine Kernel Module Support) — Alpine's equivalent of DKMS for out-of-tree modules. Builds run in a sandboxed unprivileged user with namespace isolation. Auto-rebuilds on kernel updates via apk triggers.

### Firmware Distribution
- `linux-firmware` meta-package depends on ALL firmware (~700MB+). Split into ~80+ sub-packages by vendor/chipset family (e.g., `linux-firmware-amdgpu`, `linux-firmware-iwlwifi`, `linux-firmware-rtlwifi`).
- Sub-package list: `3com, acenic, adaptec, advansys, amd, amd-ucode, amdgpu, ar3k, ath10k, ath11k, ath6k, ath9k_htc, atmel, ...` (covers all major vendors).
- Users can install only needed firmware to save space on diskless/data-disk installs.

### Image Size Impact
- ISO ~150-200MB (includes full linux-firmware).
- Installed base system: ~130MB without firmware, 800MB+ with full firmware.
- The virt kernel + no firmware = extremely small (~40MB installed).

### Clever Tricks
- `nlplug-findfs`: Alpine-specific coldplug utility that scans sysfs and loads modules to find the root filesystem. Much lighter than full udev.
- Kernel flavor selection at install time lets users optimize for their deployment target.
- Firmware sub-packages are the gold standard — adopted/discussed by NixOS, Ubuntu, Arch as a model.

**Relevance to Llamaste**: Alpine's firmware sub-package strategy is directly applicable. Llamaste could ship a "firmware detection" first-boot step that identifies needed firmware and downloads only relevant packages. The `nlplug-findfs` approach (lightweight coldplug without full udev) is similar to what Llamaste does with `init_load_modules()`.

---

## 2. Tiny Core Linux

### Kernel Configuration Strategy
- Custom minimal kernel config — only essential subsystems built-in (filesystem, memory management, basic bus support).
- Most hardware drivers excluded from the base kernel entirely.
- Kernel is ~4-5MB compressed. Entire core system (kernel + `core.gz` + BusyBox + FLTK) is ~23MB.
- Uses a heavily stripped-down config focused on the minimum viable set of drivers for boot.

### Module Loading Mechanism
- **TCE (Tiny Core Extensions)**: Hardware drivers are packaged as `.tcz` files (SquashFS containers).
- Two loading modes:
  - **OnBoot**: Essential extensions (drivers, networking) loaded at boot time from `tce/` directory.
  - **OnDemand**: Non-essential extensions loaded only when user clicks the icon in wbar.
- Extensions are loop-mounted, not extracted — only compressed size counts against storage.
- Driver extensions are community-maintained in the TCE repository.

### Firmware Distribution
- Firmware is packaged as separate `.tcz` extensions.
- Not included in base system at all — must be explicitly loaded.
- Users download firmware extensions matching their hardware.

### Image Size Impact
- Core: 16MB, TinyCore (with GUI): 23MB, CorePlus (with installer + WiFi): ~106MB.
- CorePlus includes common WiFi firmware/drivers for first-boot connectivity.
- RAM usage at idle: 32-64MB (with desktop), making it viable on 64MB-128MB RAM systems.

### Clever Tricks
- **RAM-based root**: Entire system boots into tmpfs, making it immune to storage failures and extremely fast.
- **Extension loop-mounting**: `.tcz` files are SquashFS images loop-mounted at runtime, saving RAM vs. extraction.
- **CorePlus strategy**: A slightly larger ISO that includes WiFi drivers for bootstrapping — once online, users can download exactly what they need. This "bootstrap" approach separates "drivers needed to get online" from "all other drivers."

**Relevance to Llamaste**: The CorePlus "bootstrap ISO" concept is interesting — ship enough drivers to get online, then fetch the rest. The OnBoot/OnDemand split maps well to Llamaste's "essential modules loaded by init" vs. "everything else loaded after pivot" pattern.

---

## 3. Buildroot-Based Projects (OpenWrt, Yocto/Poky)

### OpenWrt

#### Kernel Configuration Strategy
- **Three-tier hierarchy**:
  1. `target/linux/generic/config-6.12` — Base config common to ALL platforms.
  2. `target/linux/x86/config-6.12` — Architecture-specific overrides.
  3. `target/linux/x86/64/config-6.12` — Subtarget-specific customizations.
- Each level can override/extend the previous. Final config is a merge of all three.
- Heavily optimized for flash/RAM constraints of routers (often 4-16MB flash, 32-128MB RAM).
- Extensive kernel patching — maintains hundreds of patches per kernel version for hardware support and size reduction.

#### Module Loading Mechanism
- Kernel modules packaged as separate `kmod-*` opkg packages organized by category (networking, USB, filesystems, crypto, etc.).
- Modules loaded via standard `modprobe`/`kmod` infrastructure.
- udev-based hotplug for runtime device detection.
- Device Tree (DT) files per board define hardware topology for ARM targets.

#### Firmware Distribution
- WiFi firmware packaged per-chipset (e.g., `ath10k-firmware-qca988x`).
- Strongly prefers open-source drivers — recommends Qualcomm/Atheros and MediaTek, avoids Broadcom.
- Firmware included in rootfs at build time based on target board.

#### Image Size Impact
- Typical router image: 4-16MB total (kernel + rootfs + firmware).
- x86 images larger (~100-300MB) due to broader driver coverage.

### Yocto/Poky

#### Kernel Configuration Strategy
- **BSP (Board Support Package) layers** define per-board kernel configs.
- `PREFERRED_PROVIDER_virtual/kernel` in machine config selects kernel recipe.
- **Config fragments** (`.cfg` files) layered on top of defconfig — recommended over full config files for maintainability.
- `linux-yocto` recipes support multiple config fragments merged at build time.
- Interactive `menuconfig` available for development, but `.cfg` fragments are the permanent mechanism.

#### Module Loading Mechanism
- Standard Linux module loading (udev + modprobe).
- BSP layer defines which modules to include in image vs. build as packages.
- `MACHINE_ESSENTIAL_EXTRA_RDEPENDS` for required modules, `MACHINE_EXTRA_RDEPENDS` for optional.

#### Firmware Distribution
- `linux-firmware` recipe available; BSP layers specify which firmware files to include.
- Commercial BSP layers from SoC vendors (NXP, TI, Qualcomm) bundle proprietary firmware.
- `MACHINE_EXTRA_RRECOMMENDS += "linux-firmware-xxx"` pattern for board-specific firmware.

#### Image Size Impact
- Highly variable — from 8MB (minimal embedded) to 2GB+ (full Linux desktop).
- `IMAGE_INSTALL` list gives precise control over what's included.

### Clever Tricks (Both)
- **OpenWrt's three-tier config** is extremely maintainable — change a generic option once, affects all targets. Override at target or subtarget level for exceptions.
- **Yocto's config fragments** allow composable kernel configs — add `wifi.cfg`, `debug.cfg`, `performance.cfg` fragments as needed.
- Both systems: per-board hardware definitions (DTS files) mean the kernel knows exactly what hardware to expect, eliminating runtime detection overhead.

**Relevance to Llamaste**: Llamaste already uses Buildroot, so these patterns are directly applicable. The three-tier config concept (generic → arch → board) could help if Llamaste expands to multiple hardware targets. Config fragments would be cleaner than maintaining one monolithic defconfig.

---

## 4. NixOS / Guix

### Kernel Configuration Strategy
- Ships standard upstream Linux kernel with a broad configuration (similar to Ubuntu/Fedora — most drivers as modules).
- `nixos-generate-config` automatically generates `hardware-configuration.nix` by scanning current hardware.
- Sets `boot.initrd.availableKernelModules` based on detected storage controllers and `boot.kernelModules` for post-boot modules.
- **nixos-hardware** repository: community-maintained NixOS modules with per-device profiles (e.g., `lenovo/thinkpad/x220`) that set optimal kernel params, driver options, firmware packages.
- **nixos-facter**: newer tool that performs hardware inventory and generates NixOS module configuration automatically.

### Module Loading Mechanism
- Standard udev-based module autoloading.
- Early KMS: GPU modules can be added to initrd via `boot.initrd.kernelModules` for display during boot.
- Late KMS (default): GPU modules loaded after initrd, after disk encryption password entry.
- Modules can be blacklisted per-system in configuration.nix.

### Firmware Distribution
- `hardware.enableAllFirmware` or `hardware.enableRedistributableFirmware` options.
- `linux-firmware` package is ~700MB+ and growing — NixOS community has discussed splitting it (issue #148197) following Alpine's model.
- As of 2025-2026, still ships as one giant package, though split proposals are active.
- Individual firmware packages available for common hardware (e.g., `firmwareLinuxNonfree`).

### Image Size Impact
- NixOS is not size-optimized — full install is 2-10GB+.
- The Nix store deduplication helps with updates but not initial size.
- Focus is on correctness and reproducibility, not minimalism.

### Clever Tricks
- **Declarative hardware config**: `hardware-configuration.nix` captures exact hardware state at install time. Rebuild with different hardware → regenerate config → all drivers update automatically.
- **nixos-hardware profiles**: crowd-sourced device quirk database. Import `<nixos-hardware/dell/xps/15-9560>` and get correct power management, GPU switching, touchpad config, etc.
- **Atomic rollback**: If a kernel/driver change breaks boot, select previous generation from boot menu. Zero risk to experiment.

**Relevance to Llamaste**: The `nixos-generate-config` concept — scan hardware at install/first-boot and generate a minimal driver config — could be adapted for Llamaste. At install time, detect PCI/USB devices, determine needed modules, and generate a `modules.conf` that loads only what's needed. The nixos-hardware crowd-sourced quirk database is inspirational for a community-driven compatibility layer.

---

## 5. Puppy Linux / antiX / MX Linux

### Kernel Configuration Strategy
- **Puppy Linux**: Uses upstream kernel with a broad "desktop" config. Ships multiple kernel versions (e.g., 5.x and 6.x series) to cover old and new hardware. Community builds (PuppEX) offer cutting-edge kernels (6.16+) with latest driver support.
- **antiX/MX Linux**: Ships **dual kernels** — Legacy (5.10 LTS) and Modern (6.1+). User selects at boot. This is the key innovation: boot menu offers both, default to modern, fall back to legacy if it fails.
  - **AHS (Advanced Hardware Support)** variant: Ships with latest kernel (6.16+) and newest firmware specifically for very recent hardware (AMD Ryzen, Intel 11th-13th gen, etc.).

### Module Loading Mechanism
- Standard udev + modprobe for automatic hardware detection.
- Boot parameters for manual module loading: `load=module1,module2` cmdline option.
- "Shotgun" mode: `load=all` loads ALL modules in initrd — brute force for unknown hardware.
- MX Linux includes `ddm` (Device Driver Manager) GUI tool for installing proprietary drivers (NVIDIA, Broadcom WiFi).

### Firmware Distribution
- Full `linux-firmware` package included on ISO.
- Puppy: Firmware SFS (SquashFS) files can be loaded modularly.
- antiX: Firmware included in initrd for boot-critical devices, rest on rootfs.
- AHS variant includes latest firmware snapshots for cutting-edge hardware.

### Image Size Impact
- Puppy: 300-400MB ISO.
- antiX Full: ~1.2GB ISO (includes two kernels + full firmware).
- antiX Core: ~350MB (minimal, one kernel).
- MX Linux: ~2GB ISO (full desktop + firmware + driver tools).

### Clever Tricks
- **Dual kernel boot menu**: Eliminates "new kernel doesn't support old hardware" problem. User picks what works. No config needed.
- **AHS variant**: Separate ISO with bleeding-edge kernel/firmware for users who know they have new hardware.
- **Frugal install**: Entire OS runs from a SquashFS file, can be on FAT32 USB alongside other data. Similar to Llamaste's squashfs approach.
- **`load=all` shotgun**: When you don't know what hardware you have, load everything. Slow but guaranteed to work.

**Relevance to Llamaste**: The dual-kernel approach is very interesting for Llamaste — ship both an LTS kernel and a newer kernel, with a GRUB menu choice. The `load=all` fallback is crude but effective as a last resort. The AHS concept (separate build for new hardware) could work as Llamaste variant images.

---

## 6. ChromeOS / Chromium OS

### Kernel Configuration Strategy
- **Splitconfig system**: Kernel config split into hierarchical fragments:
  1. `base.config` — Common to ALL ChromeOS devices.
  2. `armel/common.config` — Architecture-specific (ARM, x86_64).
  3. `armel/chromeos-tegra2.flavour.config` — Board-specific overrides.
- Final `.config` created by concatenating all three levels for a given board/flavour.
- `splitconfig` script splits a full `.config` back into the hierarchy (finds common options, pushes them up).
- `kernelconfig` script operates on ALL flavours simultaneously — runs `make oldconfig` or `make menuconfig` for each.
- Per-board overlays in Portage (Gentoo-based) can blacklist specific modules.
- **Verified Boot** constrains firmware loading: all firmware must be in kernel or root partition (covered by dm-verity).

### Module Loading Mechanism
- Standard udev/modprobe for post-boot module loading.
- Firmware loaded via `request_firmware()` from `/lib/firmware` on the verified rootfs.
- Touch firmware updates happen at boot before UI starts (upstart job).
- ACPI-based hardware detection on x86, Device Tree on ARM.

### Firmware Distribution
- Firmware baked into the rootfs image per board. No runtime firmware downloads.
- Verified Boot ensures firmware integrity — firmware files must be signed/verified.
- Board-specific firmware packages in the Portage overlay system.
- Peripheral firmware (touchpad, touchscreen, EC) updated via separate update mechanisms with version checks at boot.

### Image Size Impact
- Full ChromeOS image: ~4-8GB (but most is Chrome browser + Android subsystem).
- Kernel + modules + firmware: ~200-400MB depending on board.
- Per-board builds mean no wasted space on drivers for other hardware.

### Clever Tricks
- **Splitconfig hierarchy**: Most maintainable config system surveyed. Change a base option once, all boards inherit. Override at any level.
- **Verified firmware loading**: All firmware on verified rootfs — no runtime downloads from untrusted sources. This is the gold standard for firmware security.
- **Per-board builds**: Each Chromebook model gets its own image with only the drivers it needs. Zero driver bloat.
- **`kernelconfig editconfig`**: Interactive menuconfig that runs across all flavours simultaneously, then auto-splits the result.

**Relevance to Llamaste**: ChromeOS's splitconfig is the most sophisticated config management system surveyed, but it requires per-board builds (not applicable to Llamaste's "boot on anything" goal). The verified firmware concept aligns with Llamaste's squashfs-based immutable rootfs. The base+arch+board hierarchy could be adapted if Llamaste ever does variant builds.

---

## 7. Live CD/USB Distributions (Ventoy, SystemRescue, Clonezilla)

### Ventoy (Boot Loader, not a distro)
- Not a Linux distro but a USB boot manager that boots ISO files directly.
- **BIOS + UEFI + Secure Boot** support out of the box.
- Two boot modes: **Normal** (loads only needed files to RAM) and **Memdisk** (loads entire ISO to RAM — more compatible but needs more RAM).
- Does NOT handle hardware detection — delegates entirely to the booted ISO's kernel.
- **Compatibility database**: Community-tested list of 1000+ ISOs with compatibility status.

### SystemRescue
- Based on Arch Linux, ships with a very broad kernel config (essentially allmodconfig-minus-debug).
- Includes `hwinfo`, `inxi`, `lspci`, `lsusb` for hardware identification.
- **BIOS + UEFI boot** support via syslinux (BIOS) + GRUB (UEFI).
- Full `linux-firmware` package included.
- Ships extensive filesystem, networking, and storage drivers as modules.
- ~700MB ISO with comprehensive driver coverage.

### Clonezilla
- Based on Debian, uses Debian's broad kernel config.
- Focuses on storage drivers — needs to support every disk controller for cloning.
- Uses Debian's full module set + linux-firmware.
- ~300-500MB ISO.

### Clever Tricks
- **SystemRescue kernel config**: Trades image size for "boot on anything" — enables essentially every non-conflicting driver as a module. This is the opposite of Llamaste's approach but guarantees compatibility.
- **Ventoy memdisk mode**: When normal ISO boot fails, load entire ISO into RAM. Slower but bypasses BIOS quirks with ISO9660/El Torito.
- **Both**: Ship as hybrid ISO (BIOS + UEFI) so the same image works regardless of firmware type.

**Relevance to Llamaste**: Llamaste already does BIOS + UEFI boot. SystemRescue's "enable everything as modules" approach is the simplest path to broad compatibility — the cost is image size (dominated by modules + firmware). For a system that installs to disk, the ISO size penalty is acceptable.

---

## 8. Steam Deck / SteamOS

### Kernel Configuration Strategy
- Based on Arch Linux with a heavily patched vendor kernel.
- Valve maintains `steamos_kernel` repository with branches per kernel version (e.g., `6.8.12-valve7`).
- Patches organized as merge branches by subsystem:
  - `6.8/features/backport-asoc-amd-mf` — AMD audio backports.
  - `6.8/features/amd-drm-extra` — AMDGPU driver enhancements.
  - `6.8/features/gpu-reset` — GPU reset event notification.
  - `6.8/features/tsc` — Time Stamp Counter improvements.
  - `6.8/features/usb-dwc3` — USB role switching for Steam Deck.
  - `6.8/features/futex-waitv` — Gaming-specific futex extensions.
- **OEM contribution model**: Hardware vendors submit patches to Valve's kernel tree.
- **Upstream-first policy**: Patches intended for upstream Linux are maintained separately.

### Module Loading Mechanism
- Standard Arch Linux udev + systemd module loading.
- Most Steam Deck-specific drivers built-in (not modules) for fast boot.
- Peripheral drivers (USB controllers, input devices) as modules for flexibility.
- SteamOS 3.8+ adds virtio guest drivers for VM support.

### Firmware Distribution
- AMD GPU firmware baked into the image (amdgpu firmware files).
- WiFi firmware (Atheros) included in rootfs.
- Firmware updates delivered via SteamOS system updates (A/B update mechanism).
- Controller firmware updates happen in Steam client.

### Image Size Impact
- Full SteamOS image: ~5-10GB (includes Steam client, Proton, desktop).
- Kernel + modules: ~200-300MB.
- Firmware: ~100-150MB (mostly amdgpu).

### Clever Tricks
- **Vendor kernel with upstream intent**: Patches are developed against upstream, carried in vendor tree until accepted. Reduces maintenance burden over time.
- **Per-subsystem merge branches**: Each feature area is an isolated git branch, merged into the release kernel. Easy to cherry-pick, revert, or update individual subsystems.
- **LAVD scheduler**: Experimental CPU scheduler optimized for gaming workloads (low latency, fair scheduling under mixed compute+render loads).
- **A/B updates**: Similar to Llamaste's approach — update one partition, boot into it, roll back if it fails.

**Relevance to Llamaste**: Valve's approach of carrying upstream-bound patches in a vendor kernel is a good model. The per-subsystem merge branches would help Llamaste manage driver patches. The A/B update mechanism is already implemented in Llamaste.

---

## Cross-Cutting Analysis

### Kernel Config Approaches Ranked by Compatibility

| Approach | Example | Compatibility | Image Size | Maintenance |
|----------|---------|--------------|------------|-------------|
| allmodconfig (nearly) | SystemRescue, Ubuntu | Excellent | Large (700MB+) | Low |
| Broad curated config | Alpine LTS, Debian | Very Good | Medium (200-400MB) | Medium |
| Three-tier hierarchy | OpenWrt, ChromeOS | Per-target optimal | Small-Medium | High (scales well) |
| Dual kernel | antiX | Very Good (old+new) | Large (2x kernel) | Medium |
| Minimal + extensions | Tiny Core | User-dependent | Tiny (23MB) | High (user burden) |
| Per-board custom | Yocto BSP, ChromeOS | Exact match | Minimal | Very High |

### Firmware Strategies Ranked

| Strategy | Example | Coverage | Size Impact |
|----------|---------|----------|-------------|
| Full linux-firmware blob | Ubuntu, SystemRescue | Complete | 600-800MB |
| Split sub-packages | Alpine, Arch (2025+) | Selective | 10-800MB |
| Per-board firmware | ChromeOS, Yocto | Exact | Minimal |
| On-demand download | Tiny Core | User choice | Near-zero base |
| Dual firmware sets | antiX AHS | Old + New | 2x |

### Module Loading Mechanisms

| Mechanism | Example | Complexity | Flexibility |
|-----------|---------|-----------|-------------|
| udev + modprobe | Most distros | Standard | High |
| nlplug-findfs (coldplug) | Alpine | Low | Medium |
| Device Tree | ARM boards, ChromeOS ARM | Low | Fixed |
| Manual/cmdline | antiX `load=` | None | Manual |
| Extension system | Tiny Core TCE | Custom | High |
| init_load_modules() | Llamaste | Custom | Dependency-ordered |

---

## Recommendations for Llamaste

### Short-Term (v0.3)
1. **Enable more drivers as modules**: Follow Alpine/SystemRescue model — enable broadly, build as =m. The squashfs pivot means modules load after rootfs is available.
2. **Split firmware into categories**: Create firmware sub-packages or overlay directories by vendor (Intel WiFi, AMD GPU, Realtek, etc.). At build time, include all. At install time, offer to prune unused firmware.
3. **Add `load=all` fallback**: antiX-style boot option that loads every module in `/lib/modules/`. Crude but effective for unknown hardware.
4. **Hardware inventory at first boot**: Like `nixos-generate-config`, scan PCI/USB bus at first boot and record what hardware exists. Use this to optimize module loading on subsequent boots.

### Medium-Term (v0.4-0.5)
5. **Config fragment system**: Break the monolithic linux.config into composable fragments: `base.cfg` + `storage.cfg` + `wifi.cfg` + `gpu.cfg`. Makes it easier to create variant builds.
6. **Dual kernel GRUB option**: Ship both current LTS and previous LTS kernel. GRUB menu offers both. If modern kernel panics, user reboots and picks legacy.
7. **Dynamic firmware loading**: At first boot, if internet is available, detect hardware and download only needed firmware packages. Reduces base image size.

### Long-Term (v1.0+)
8. **Per-target variant builds**: Like ChromeOS/OpenWrt, create target-specific images (generic-x86, thinkpad, intel-nuc, etc.) with optimized driver sets.
9. **Community hardware database**: Like nixos-hardware, maintain a database of known-good hardware configs. Users contribute their hardware profiles.
10. **Firmware verification**: Like ChromeOS verified boot, ensure all firmware files are integrity-checked before loading. Already partially achieved via squashfs immutability.

---

## Sources

- [Alpine Linux Kernels Wiki](https://wiki.alpinelinux.org/wiki/Kernels)
- [Alpine Kernel Module Support (AKMS)](https://wiki.alpinelinux.org/wiki/Alpine_kernel_module_support)
- [Tiny Core Linux Concepts](http://www.tinycorelinux.net/concepts.html)
- [Tiny Core Linux Engineering Analysis](https://terabyte.systems/posts/tiny-core-linux-engineering-a-minimalist-foundation-for/)
- [OpenWrt Kernel Configuration (DeepWiki)](https://deepwiki.com/openwrt/openwrt/3.1-kernel-configuration)
- [OpenWrt Kernel Subsystem (DeepWiki)](https://deepwiki.com/openwrt/openwrt/4-kernel-subsystem)
- [Yocto Project BSP Guide](https://docs.yoctoproject.org/bsp-guide/bsp.html)
- [NixOS Hardware Configuration](https://discourse.nixos.org/t/logic-behind-kernel-modules-added-to-conf-by-nixos-generate-config/29886)
- [nixos-hardware Repository](https://github.com/NixOS/nixos-hardware)
- [nixos-facter-modules](https://github.com/nix-community/nixos-facter-modules)
- [antiX/MX Linux Boot Parameters](https://mxlinux.org/wiki/system/boot-parameters/)
- [antiX Dual Kernel Discussion](https://www.antixforum.com/forums/topic/modern-kernel/)
- [ChromeOS Kernel Configuration Guide](https://www.chromium.org/chromium-os/developer-library/guides/kernel/kernel-configuration/)
- [ChromeOS Kernel FAQ](https://chromium.googlesource.com/chromiumos/docs/+/refs/heads/factory-atlas-11907.B/kernel_faq.md)
- [Steam Deck Kernel Analysis (Samuel Dionne-Riel)](https://samuel.dionne-riel.com/blog/2024/11/20/whats-in-a-steam-deck-kernel-anyway.html)
- [SteamOS Kernel Pipeline (DeepWiki)](https://deepwiki.com/ValveSoftware/SteamOS/3.2-steamos-kernel-development-pipeline)
- [SteamOS 3.8 Preview](https://md-eksperiment.org/en/post/20260320-steamos-3-8-preview-lands-with-wayland-by-default-and-broader-hardware-support-with-a-catch)
- [SystemRescue Homepage](https://www.system-rescue.org/)
- [Ventoy Compatibility](https://www.ventoy.net/en/compatible.html)
- [Ubuntu Firmware Package Split Proposal](https://www.omgubuntu.co.uk/2025/06/buntu-linux-firmware-package-size-reduction-proposal)
- [Ubuntu 26.04 Firmware Split](https://www.omgubuntu.co.uk/2026/02/ubuntu-26-04-firmware-split)
- [NixOS linux-firmware Size Issue #148197](https://github.com/NixOS/nixpkgs/issues/148197)
