# Firmware Distribution Strategies for Minimal Linux

Research date: 2026-03-23
Context: Llamaste is a Buildroot-based x86_64 Linux that currently ships only specific firmware blobs (Intel iwlwifi, Realtek rtw88, some Atheros). The full linux-firmware package is 900MB+ uncompressed. This document analyzes strategies to maximize hardware compatibility while minimizing image size.

---

## 1. linux-firmware Size Breakdown by Category

The upstream linux-firmware repository is over 900MB uncompressed. Using Arch Linux's recent package split (June 2025, version 20260309-1) as a reference, we get precise size data for each vendor category:

| Package | Installed Size | Description |
|---------|---------------|-------------|
| linux-firmware-intel | **124.9 MB** | WiFi (iwlwifi), Bluetooth, i915 GPU (GuC/HuC/DMC), IPU, NPU, audio |
| linux-firmware-nvidia | **103.5 MB** | NVIDIA GPU firmware (nouveau) + Tegra SoC |
| linux-firmware-atheros | **47.0 MB** | Qualcomm Atheros WiFi (ath9k, ath10k, ath11k) + Bluetooth |
| linux-firmware-other | **27.8 MB** | Unsorted: various NICs, sound cards, misc devices |
| linux-firmware-amdgpu | **26.1 MB** | AMD Radeon GPU firmware (GCN through RDNA4) |
| linux-firmware-mediatek | **26.5 MB** | MediaTek/Ralink WiFi + Bluetooth |
| linux-firmware-broadcom | **12.9 MB** | Broadcom/Cypress WiFi + Bluetooth + BNX2/BNX2X NICs |
| linux-firmware-realtek | **6.8 MB** | Realtek WiFi (rtw88, rtw89, rtlwifi), NIC, Bluetooth |
| linux-firmware-qcom | ~20 MB (est.) | Qualcomm SoC firmware (mostly ARM, not x86) |
| linux-firmware-cirrus | ~1 MB (est.) | Cirrus Logic audio codec firmware |
| linux-firmware-radeon | ~5 MB (est.) | Legacy AMD Radeon (pre-AMDGPU) |
| **Total (all packages)** | **~400+ MB** | (uncompressed, all architectures) |

### Key Insight: Size Dominance

Intel firmware alone is ~125 MB -- this is primarily iwlwifi blobs (each generation ships ~2-4 MB of firmware, and there are 20+ generations). GPU firmware (AMD + NVIDIA + Intel i915) accounts for ~250+ MB combined. WiFi firmware across all vendors is ~60-80 MB total.

### Minimum Viable Firmware for "95% of x86_64 Hardware"

For a general-purpose x86_64 system, the critical categories are:
- **Intel WiFi** (iwlwifi): ~80-100 MB alone -- dominates laptop WiFi market
- **Intel i915 GPU** (GuC/HuC/DMC): ~5-10 MB -- needed for Intel iGPU display
- **Realtek** (WiFi + NIC + BT): ~7 MB -- very common in budget laptops/desktops
- **Atheros/Qualcomm WiFi**: ~47 MB -- common in older laptops
- **Broadcom WiFi/BT**: ~13 MB -- MacBooks, some laptops
- **MediaTek WiFi**: ~26 MB -- increasingly common in newer laptops
- **AMD GPU**: ~26 MB -- needed for AMD discrete/APU graphics
- **Bluetooth** (Intel + Realtek + QCA): included in above packages

**Estimated "95% x86_64 coverage"**: ~200-250 MB uncompressed, ~100-125 MB with xz compression.

---

## 2. Firmware Selection Strategies by Distribution

### Alpine Linux
- **Strategy**: Splits linux-firmware into ~112 sub-packages, one per `/lib/firmware/` subdirectory.
- **Mechanism**: The APKBUILD iterates over a hardcoded `_folders` list that maps 1:1 to firmware subdirectories (e.g., `amdgpu`, `iwlwifi`, `brcm`, `rtw88`, etc.).
- **Meta-package**: `linux-firmware` depends on ALL sub-packages. Users can install `linux-firmware-none` for no firmware, or cherry-pick individual packages.
- **Discovery**: Users can boot without firmware, run `dmesg | grep firmware`, and install only what's needed.
- **Relevance to Llamaste**: Alpine's granular approach is the most similar to what Llamaste needs. Each subfolder becomes an installable unit.

### Arch Linux (since June 2025)
- **Strategy**: Split into 17 vendor-focused packages: `linux-firmware-amdgpu`, `linux-firmware-intel`, `linux-firmware-realtek`, `linux-firmware-atheros`, `linux-firmware-broadcom`, `linux-firmware-mediatek`, `linux-firmware-nvidia`, `linux-firmware-qcom`, `linux-firmware-cirrus`, `linux-firmware-radeon`, `linux-firmware-other`, etc.
- **Meta-package**: `linux-firmware` is now an empty package that depends on the default set (all vendor packages).
- **User control**: Users can `pacman -Rdd linux-firmware` and install only needed vendor packages.
- **Relevance**: Provides the exact size data for vendor categories (see table above). Good model for "tier" selection.

### Debian
- **Strategy**: Source package `firmware-nonfree` produces ~15 binary packages split by vendor/function:
  - `firmware-iwlwifi` (~19 MB) -- Intel WiFi
  - `firmware-amd-graphics` -- AMD GPU
  - `firmware-realtek` -- Realtek WiFi/NIC/BT
  - `firmware-atheros` -- Qualcomm Atheros
  - `firmware-misc-nonfree` -- everything else
  - `firmware-linux-nonfree` -- meta-package
- **Key difference from Alpine**: Debian groups by driver/vendor rather than by subdirectory. More curated but fewer packages.
- **Relevance**: As of Debian 12 (Bookworm), non-free firmware is included on official install media. Shows the industry trend toward shipping firmware by default.

### Fedora
- **Strategy**: Ships full linux-firmware but compresses with xz since Fedora 34.
- **WHENCE file**: The upstream WHENCE file documents every firmware blob's license and source. Fedora uses this for license compliance.
- **Compression**: `xz -C crc32` compression reduces on-disk size by ~50% (900 MB -> ~450 MB).
- **Relevance**: Compression-only approach is simplest to implement but still results in large images.

### Gentoo
- **Strategy**: Most flexible -- uses `savedconfig` USE flag for per-file firmware selection.
- **savedconfig**: User edits `/etc/portage/savedconfig/sys-kernel/linux-firmware-*` to list only needed firmware files. The ebuild installs only listed files.
- **USE flags**: `compress-xz`, `compress-zstd`, `deduplicate` (symlinks for duplicate firmware using rdfind).
- **Relevance**: The `savedconfig` approach is the gold standard for minimal firmware. Requires user knowledge of exact firmware files needed.

### ChromeOS
- **Strategy**: Board-specific firmware bundles. Each Chromebook model has a custom firmware ebuild (`chromeos-firmware-${BOARD}`) that includes ONLY the firmware for that specific board's hardware.
- **No generic firmware package**: Unlike desktop distros, ChromeOS never ships unnecessary firmware.
- **Mechanism**: Board overlay's `make.conf` sets USE flags (`bootimage`, `cros_ec`) to control what firmware is built.
- **Relevance**: Ideal for appliance devices. Llamaste could adopt a similar model with "hardware profiles" but this requires knowing the target hardware at build time, which conflicts with the generic-image goal.

---

## 3. On-Demand Firmware Loading

### Kernel Firmware Loading Mechanism
The kernel searches these paths in order:
1. `firmware_class.path=` (kernel command line parameter)
2. `/lib/firmware/updates/$(uname -r)/`
3. `/lib/firmware/updates/`
4. `/lib/firmware/$(uname -r)/`
5. `/lib/firmware/`

The runtime path can also be changed via:
```
echo -n /path/to/firmware > /sys/module/firmware_class/parameters/path
```

### Custom Firmware Path for Llamaste
Since Llamaste uses squashfs + overlayfs, the firmware loading naturally supports layering:
- **Base layer** (squashfs): Ship essential firmware (Intel WiFi, Realtek, common WiFi)
- **Overlay layer** (DATA partition): Additional firmware downloaded at runtime or on first boot

This works because `/lib/firmware/` in the merged overlayfs view includes files from both layers.

### Separate Firmware Partition
A dedicated firmware partition is possible but adds complexity:
- Mount at `/lib/firmware/` or use `firmware_class.path=`
- Could be a FAT32 partition for easy updates from any OS
- Adds partition management complexity for marginal benefit over overlayfs

### Download-on-Demand
Concept: On first boot, detect hardware via `lspci`/`lsusb`, then download only needed firmware from a server.
- **Pro**: Minimal image size, always current firmware
- **Con**: Requires network (chicken-and-egg for WiFi firmware!), requires running a firmware server
- **Mitigation**: Ship WiFi firmware for common chipsets in base image; download GPU firmware on demand
- **Implementation**: `modprobe` triggers firmware loading -> kernel request_firmware() -> if missing, a userspace helper could fetch from network. But the kernel fallback mechanism (sysfs loading) is deprecated in modern kernels.

### Recommendation for Llamaste
Use the **squashfs + overlay approach**:
1. Ship essential WiFi/NIC firmware in the squashfs (~30-50 MB)
2. Provide a `firmware.install` tool that scans hardware and copies needed firmware to `/data/lib/firmware/` (overlay)
3. Bundle a "firmware pack" ISO/tar that users can provide for offline installation

---

## 4. Firmware Compression

### Kernel Support
- **XZ compression**: Supported since Linux 5.3 via `CONFIG_FW_LOADER_COMPRESS_XZ`
  - Files must use CRC32 integrity check: `xz -C crc32 firmware.bin`
  - Produces `.xz` files in `/lib/firmware/`
  - Kernel tries uncompressed first, falls back to compressed
- **Zstd compression**: Supported since Linux 5.19 via `CONFIG_FW_LOADER_COMPRESS_ZSTD`
  - Slightly larger than XZ but much faster decompression
  - Produces `.zst` files in `/lib/firmware/`

### Size Savings
Based on Fedora's analysis:
- **XZ**: ~50% reduction (900 MB -> ~450 MB)
- **Zstd**: ~45% reduction, but 3-5x faster decompression
- Individual firmware blobs compress differently: binary microcode compresses well (60-70% reduction), structured data less so

### Squashfs Double-Compression
Since Llamaste already uses squashfs (which compresses with xz/zstd), firmware files inside the squashfs are ALREADY compressed at the filesystem level. Adding per-file xz compression on top provides minimal additional benefit and adds CPU overhead during loading.

**However**, firmware on the overlay (ext4 DATA partition) is NOT compressed by the filesystem. For overlay firmware, xz/zstd compression is valuable.

### Recommendation for Llamaste
- **Squashfs firmware**: Do NOT use per-file compression (squashfs already compresses)
- **Overlay firmware**: Use xz compression (`CONFIG_FW_LOADER_COMPRESS_XZ=y`)
- **Buildroot config**: Add `CONFIG_FW_LOADER_COMPRESS_XZ=y` to linux.config for overlay firmware support

---

## 5. GPU Firmware Requirements

### AMD AMDGPU (26.1 MB total per Arch)
Each GPU family requires multiple firmware blobs (PSP, SMU, SDMA, GFX, VCN/UVD, DCN):
- **Polaris** (RX 470-590): ~2-3 MB -- `polaris10_*, polaris11_*, polaris12_*`
- **Vega** (Vega 56/64, VII): ~3-4 MB -- `vega10_*, vega12_*, vega20_*`
- **Navi/RDNA1** (RX 5000): ~3-4 MB -- `navi10_*, navi14_*`
- **RDNA2** (RX 6000): ~4-5 MB -- `sienna_cichlid_*, navy_flounder_*, dimgrey_cavefish_*, beige_goby_*, yellow_carp_*`
- **RDNA3** (RX 7000): ~5-6 MB -- `dcn_3_2_*, gc_11_*, psp_13_*, sdma_6_*` (plus SMU)
- **RDNA4** (RX 9000): ~5-6 MB -- newest additions

**Note**: Llamaste currently has `CONFIG_DRM_AMDGPU=y` disabled (causes black screen with built-in, must use module). If GPU support is added, firmware is needed post-squashfs-pivot.

### Intel i915 / Xe (~5-10 MB within the intel package)
- **GuC** (Graphics microController): ~200-400 KB per generation
- **HuC** (HEVC/media microController): ~200-400 KB per generation
- **DMC** (Display MicroController): ~50-100 KB per generation
- Generations: Skylake, Broxton, Kabylake, Coffeelake, Icelake, Tigerlake, Alderlake, Meteorlake, Lunarlake, etc.
- Total i915 firmware is modest (~5-10 MB) but Intel ships it bundled with WiFi firmware in one mega-package

### NVIDIA Nouveau (~103.5 MB per Arch)
- Nouveau requires signed firmware from NVIDIA for anything past Kepler (GTX 600/700)
- **Turing+ (RTX 20+)**: Requires `nvidia/tu1xx_*`, `nvidia/ga1xx_*`, `nvidia/ad1xx_*` -- video decode only, no reclocking
- **Limitation**: Nouveau firmware enables basic display + video decode but NOT performance reclocking. Cards run at lowest power state.
- **Relevance for Llamaste**: NVIDIA nouveau is effectively useless for GPU compute. Skip unless display-only support is needed. The 103 MB cost is hard to justify.

---

## 6. Buildroot linux-firmware Package Options

Buildroot's `package/linux-firmware/Config.in` provides granular per-device firmware selection (801 lines of config options). Key categories:

### WiFi Firmware Options
| Config Option | Device | Notes |
|--------------|--------|-------|
| `BR2_PACKAGE_LINUX_FIRMWARE_ATHEROS_9271` | AR9271 | Common USB WiFi dongle |
| `BR2_PACKAGE_LINUX_FIRMWARE_ATHEROS_10K_QCA9377` | QCA9377 | Common laptop WiFi |
| `BR2_PACKAGE_LINUX_FIRMWARE_ATHEROS_10K_QCA998X` | QCA988X | ath10k PCIe |
| `BR2_PACKAGE_LINUX_FIRMWARE_BRCM_BCM43XX` | BCM43xx | Broadcom SoftMAC |
| `BR2_PACKAGE_LINUX_FIRMWARE_BRCM_BCM43XXX` | BCM43xxx | Broadcom FullMAC |
| `BR2_PACKAGE_LINUX_FIRMWARE_IWLWIFI_*` | Intel WiFi | Per-generation: 7265D, 8000C, 8265, 9XXX |
| `BR2_PACKAGE_LINUX_FIRMWARE_MEDIATEK_MT7601U` | MT7601U | Cheap USB dongle |
| `BR2_PACKAGE_LINUX_FIRMWARE_MEDIATEK_MT7921` | MT7921 | Modern laptop WiFi |
| `BR2_PACKAGE_LINUX_FIRMWARE_MEDIATEK_MT7922` | MT7922 | WiFi 6E |
| `BR2_PACKAGE_LINUX_FIRMWARE_MEDIATEK_MT7925` | MT7925 | WiFi 7 |
| `BR2_PACKAGE_LINUX_FIRMWARE_MWIFIEX_*` | Marvell | Various interfaces |
| `BR2_PACKAGE_LINUX_FIRMWARE_RTW_88` | RTW88 | Realtek rtw88 family |
| `BR2_PACKAGE_LINUX_FIRMWARE_RTW_89` | RTW89 | Realtek rtw89 (WiFi 6) |
| `BR2_PACKAGE_LINUX_FIRMWARE_QUALCOMM_WCN36XX` | WCN36xx | Qualcomm mobile WiFi |

### GPU Firmware Options
| Config Option | Device |
|--------------|--------|
| `BR2_PACKAGE_LINUX_FIRMWARE_AMDGPU` | AMD GPU (all generations) |
| `BR2_PACKAGE_LINUX_FIRMWARE_I915` | Intel i915 GPU |
| `BR2_PACKAGE_LINUX_FIRMWARE_XE` | Intel Xe GPU |
| `BR2_PACKAGE_LINUX_FIRMWARE_RADEON` | Legacy AMD Radeon |

### Bluetooth Firmware Options
| Config Option | Device |
|--------------|--------|
| `BR2_PACKAGE_LINUX_FIRMWARE_QCA_BT` | Qualcomm Atheros BT |
| `BR2_PACKAGE_LINUX_FIRMWARE_RTLBT` | Realtek BT (rtl_bt/) |
| `BR2_PACKAGE_LINUX_FIRMWARE_BCM43XX_BT` | Broadcom BT |
| (Intel BT included with iwlwifi) | Intel BT |

### Mechanism
Buildroot uses two variables in `linux-firmware.mk`:
- `LINUX_FIRMWARE_FILES` += individual firmware files
- `LINUX_FIRMWARE_DIRS` += entire directories

Selected firmware is copied from the downloaded linux-firmware tarball to the target rootfs during the install phase.

### Current Llamaste defconfig
Llamaste currently selects specific firmware manually. To expand coverage, add more `BR2_PACKAGE_LINUX_FIRMWARE_*` options to the Buildroot defconfig.

---

## 7. USB WiFi Dongle Firmware

Common USB WiFi dongles and their firmware requirements:

| Chipset | Driver | Firmware Package | In-Kernel? | Approx Size |
|---------|--------|-----------------|------------|-------------|
| RTL8188EU | rtl8xxxu | rtlwifi/ | Yes (5.0+) | ~30 KB |
| RTL8188CUS | rtl8192cu | rtlwifi/ | Yes | ~30 KB |
| RTL8192EU | rtl8xxxu | rtlwifi/ | Yes (4.0+) | ~30 KB |
| RTL8812AU | rtl8812au | N/A (out-of-tree) | **No** | Out-of-tree driver |
| RTL8811AU | rtl8812au | N/A (out-of-tree) | **No** | Out-of-tree driver |
| RTL8814AU | rtl8814au | N/A (out-of-tree) | **No** | Out-of-tree driver |
| MT7601U | mt7601u | mediatek/ | Yes (4.2+) | ~100 KB |
| MT7612U | mt76x2u | mediatek/ | Yes (4.19+) | ~200 KB |
| MT7921U | mt7921u | mediatek/ | Yes (5.18+) | ~1 MB |
| AR9271 | ath9k_htc | ath9k_htc/ | Yes | ~100 KB |
| RTL8153B | r8152 | rtl_nic/ | Yes | ~40 KB (USB Ethernet) |

### Key Issue: RTL8812AU/8811AU
The RTL8812AU (AC1200 USB dongles) is extremely popular but has NO in-kernel driver. The out-of-tree `rtl8812au` driver requires DKMS compilation against kernel headers. This is problematic for Llamaste's Buildroot approach. Consider:
- Compiling the out-of-tree driver as a kernel module in Buildroot
- Or recommending MT7612U-based dongles instead (fully in-kernel)

### Recommended USB Dongles for Llamaste
1. **MediaTek MT7612U** -- WiFi 5, AC1200, fully in-kernel since 4.19
2. **MediaTek MT7921U** -- WiFi 6, AX, in-kernel since 5.18
3. **Atheros AR9271** -- WiFi 4, very stable, tiny firmware
4. **Realtek RTL8188EU** -- WiFi 4, cheap, in-kernel

---

## 8. Bluetooth Firmware

Bluetooth controllers that share a USB/PCIe interface with WiFi typically need firmware loaded separately:

| Vendor | Firmware Path | Approx Size | Notes |
|--------|--------------|-------------|-------|
| Intel | `intel/ibt-*` | ~5-10 MB total | Included with iwlwifi package; per-generation BT firmware |
| Realtek | `rtl_bt/` | ~2-3 MB total | RTL8761B, RTL8821C, RTL8822C, RTL8852A etc. |
| Qualcomm/Atheros | `qca/` | ~2-3 MB total | QCA6174, QCA9377, WCN685x |
| Broadcom | `brcm/` | ~1-2 MB total | BCM4345C0, BCM4356A2, etc. |
| MediaTek | `mediatek/BT_RAM_CODE_*` | ~1-2 MB total | MT7921, MT7922 BT firmware |

### Key Points
- Bluetooth firmware is generally small (total ~15 MB for all vendors)
- Most BT firmware is bundled with the WiFi firmware package (same vendor)
- Buildroot options: `BR2_PACKAGE_LINUX_FIRMWARE_QCA_BT`, `BR2_PACKAGE_LINUX_FIRMWARE_RTLBT`, etc.
- Intel BT firmware is automatically included when selecting iwlwifi firmware in Buildroot

---

## 9. Recommended Strategy for Llamaste

### Tiered Firmware Approach

#### Tier 1: Ship in Squashfs (Essential -- always present)
**Target: ~30-50 MB uncompressed (compressed by squashfs to ~15-25 MB)**
- Intel iwlwifi (current + 2 previous generations): ~10-15 MB (selective, not all 20+ gens)
- Intel i915 GuC/HuC/DMC: ~5 MB
- Realtek WiFi + NIC + BT (rtw88, rtw89, rtl_nic, rtl_bt): ~7 MB
- Qualcomm Atheros WiFi + BT (ath10k QCA9377/998X, ath9k_htc, qca BT): ~10 MB
- MediaTek WiFi (MT7921, MT7922, MT7612U): ~5 MB
- Broadcom/Cypress WiFi + BT (brcm, cypress): ~5 MB
- USB Ethernet (r8152, asix, cdc_ether): ~1 MB

#### Tier 2: Downloadable Firmware Pack (Extended)
**Target: ~100 MB compressed download**
- All Intel iwlwifi generations
- AMD GPU firmware (amdgpu/)
- Intel Xe GPU firmware
- Additional MediaTek/Atheros generations
- NVIDIA nouveau (optional, low priority)

#### Tier 3: On-Demand (Future)
- Hardware detection at first boot
- Download specific firmware from Llamaste firmware server
- Store on DATA partition overlay

### Implementation in Buildroot

Add to Llamaste defconfig:
```
# Tier 1 WiFi
BR2_PACKAGE_LINUX_FIRMWARE=y
BR2_PACKAGE_LINUX_FIRMWARE_IWLWIFI_8265=y
BR2_PACKAGE_LINUX_FIRMWARE_IWLWIFI_9XXX=y
BR2_PACKAGE_LINUX_FIRMWARE_ATHEROS_10K_QCA9377=y
BR2_PACKAGE_LINUX_FIRMWARE_ATHEROS_10K_QCA998X=y
BR2_PACKAGE_LINUX_FIRMWARE_ATHEROS_9271=y
BR2_PACKAGE_LINUX_FIRMWARE_BRCM_BCM43XXX=y
BR2_PACKAGE_LINUX_FIRMWARE_MEDIATEK_MT7921=y
BR2_PACKAGE_LINUX_FIRMWARE_MEDIATEK_MT7922=y
BR2_PACKAGE_LINUX_FIRMWARE_MEDIATEK_MT7601U=y
BR2_PACKAGE_LINUX_FIRMWARE_MEDIATEK_MT76X2E=y
BR2_PACKAGE_LINUX_FIRMWARE_RTW_88=y
BR2_PACKAGE_LINUX_FIRMWARE_RTW_89=y

# Tier 1 GPU
BR2_PACKAGE_LINUX_FIRMWARE_I915=y

# Tier 1 Bluetooth
BR2_PACKAGE_LINUX_FIRMWARE_QCA_BT=y
BR2_PACKAGE_LINUX_FIRMWARE_RTLBT=y

# Tier 1 NIC
# (rtl_nic included with RTW firmware selection)

# Kernel config for overlay firmware compression
# CONFIG_FW_LOADER_COMPRESS_XZ=y (in linux.config)
```

### Kernel Config Additions
```
# Firmware compression for overlay/downloaded firmware
CONFIG_FW_LOADER_COMPRESS=y
CONFIG_FW_LOADER_COMPRESS_XZ=y
CONFIG_FW_LOADER_COMPRESS_ZSTD=y
```

### Size Estimate
- Tier 1 in squashfs: ~35-50 MB uncompressed -> ~15-25 MB after squashfs compression
- Total image size increase: ~15-25 MB over current (current ships only specific iwlwifi + rtw88)
- Coverage: ~85-90% of x86_64 WiFi hardware, Intel iGPU, common Bluetooth

---

## 10. Sources

- [Arch Linux linux-firmware packages](https://archlinux.org/packages/?q=linux-firmware-)
- [Arch Wiki - Linux firmware](https://wiki.archlinux.org/title/Linux_firmware)
- [Alpine Linux firmware packages](https://pkgs.alpinelinux.org/packages?name=linux-firmware-*&branch=edge)
- [NixOS linux-firmware size discussion](https://github.com/NixOS/nixpkgs/issues/148197)
- [Gentoo Wiki - Linux firmware](https://wiki.gentoo.org/wiki/Linux_firmware)
- [Gentoo Wiki - AMDGPU](https://wiki.gentoo.org/wiki/AMDGPU)
- [Fedora - Compress Kernel Firmware](https://fedoraproject.org/wiki/Changes/CompressKernelFirmware)
- [Debian - Firmware Wiki](https://wiki.debian.org/Firmware)
- [Kernel docs - Firmware search paths](https://www.kernel.org/doc/html/latest/driver-api/firmware/fw_search_path.html)
- [Kernel docs - Firmware fallback mechanisms](https://www.kernel.org/doc/html/latest/driver-api/firmware/fallback-mechanisms.html)
- [Buildroot linux-firmware Config.in](https://github.com/buildroot/buildroot/blob/master/package/linux-firmware/Config.in)
- [linux-firmware git repository](https://gitlab.com/kernel-firmware/linux-firmware)
- [Phoronix - Zstd compressed firmware Linux 5.19](https://www.phoronix.com/news/Zstd-Firmware-Linux-5.19-Next)
