# GPU & Display Driver Strategy for Llamaste

**Date**: 2026-03-23
**Purpose**: Research GPU/display driver support for maximal hardware compatibility in a minimal Buildroot-based Linux (x86_64) with labwc Wayland compositor.

## Executive Summary

Llamaste currently supports Intel i915 (built-in) + simpledrm fallback. To cover ~95% of desktop/laptop hardware, we need:
1. **AMD amdgpu** as a kernel module (=m) with firmware in squashfs
2. **NVIDIA nouveau** as a kernel module (=m) with GSP firmware for Turing+
3. **Intel xe** as a kernel module (=m) for Meteor Lake and newer
4. **Mesa Gallium drivers**: iris + radeonsi + nouveau + llvmpipe (software fallback)
5. **simpledrm** remains the universal EFI fallback (already working)

All GPU-specific drivers MUST be modules (=m) loaded after squashfs pivot — the same pattern already proven with WiFi drivers. Built-in (=y) causes "steals display before firmware available" on amdgpu and similar issues on nouveau.

---

## 1. AMD GPU Support (amdgpu)

### Kernel Config Options

```
CONFIG_DRM_AMDGPU=m                    # Module, NOT built-in
CONFIG_DRM_AMDGPU_SI=y                 # Southern Islands (GCN 1.0) support
CONFIG_DRM_AMDGPU_CIK=y               # Sea Islands (GCN 2.0) support
CONFIG_DRM_AMD_DC=y                    # Display Core (required for display output)
CONFIG_DRM_AMD_DC_FP=y                 # Floating point for DC (color management)
CONFIG_DRM_AMD_ACP=y                   # Audio CoProcessor (HDMI/DP audio, optional)
CONFIG_HSA_AMD=y                       # HSA support (optional, for compute)
CONFIG_DRM_AMD_DC_SI=y                 # DC support for Southern Islands
CONFIG_CHECKPOINT_RESTORE=y            # Required by DC
```

### GPU Generations & Firmware

| Generation | Architecture | Example GPUs | Firmware Prefix | Year |
|-----------|-------------|-------------|----------------|------|
| GCN 1.0 | Southern Islands | HD 7700-7900, R9 270 | `verde`, `tahiti`, `pitcairn`, `oland`, `hainan` | 2012 |
| GCN 2.0 | Sea Islands | R7 260, R9 290 | `bonaire`, `hawaii`, `kabini`, `kaveri`, `mullins` | 2013 |
| GCN 3.0 | Volcanic Islands | R9 285/380, Fury | `tonga`, `fiji`, `topaz` | 2014-15 |
| GCN 4.0 | Polaris | RX 460-580 | `polaris10`, `polaris11`, `polaris12` | 2016-17 |
| GCN 5.0 | Vega | Vega 56/64, VII | `vega10`, `vega12`, `vega20` | 2017-19 |
| RDNA 1.0 | Navi 1x | RX 5500/5600/5700 | `navi10`, `navi12`, `navi14` | 2019 |
| RDNA 2.0 | Navi 2x | RX 6600/6700/6800/6900 | `sienna_cichlid`, `navy_flounder`, `dimgrey_cavefish`, `beige_goby`, `yellow_carp` | 2020-21 |
| RDNA 3.0 | Navi 3x | RX 7600/7700/7800/7900 | `dcn_3_2_0`, `gc_11_0_0` etc. | 2022-23 |
| RDNA 3.5 | Navi 3x+ | RX 7000M, Strix Point APU | `dcn_3_5` prefix | 2024 |
| RDNA 4.0 | Navi 4x | RX 9070 | `gc_12_0_0` prefix | 2025 |

### APU Integrated Graphics (Ryzen Series)

| APU Series | GPU Architecture | Firmware Prefix |
|-----------|-----------------|----------------|
| Ryzen 2000/3000 (Raven/Picasso) | Vega (GCN 5.0) | `raven`, `raven2`, `picasso` |
| Ryzen 4000 (Renoir) | Vega (GCN 5.0) | `renoir` |
| Ryzen 5000 (Cezanne) | Vega (GCN 5.0) | `green_sardine` |
| Ryzen 6000 (Rembrandt) | RDNA 2.0 | `yellow_carp` |
| Ryzen 7000 (Phoenix) | RDNA 3.0 | `dcn_3_1_4`, `gc_11_0_1` |
| Ryzen 8000/AI 300 (Strix) | RDNA 3.5 | `dcn_3_5` |
| Ryzen 9000 (Granite Ridge) | Discrete only | N/A (no iGPU) |

### Module Loading Strategy

**CRITICAL**: amdgpu MUST be =m (module), NOT =y (built-in).

When built-in, amdgpu initializes before rootfs is mounted. Without firmware files available at `/lib/firmware/amdgpu/`, the driver fails and often takes over the DRM device from simpledrm, resulting in a **black screen** (this is the known bug `CONFIG_DRM_AMDGPU=y causes black screen` already documented in CLAUDE.md).

As a module, amdgpu loads after squashfs pivot via `init_load_modules()` when `/lib/firmware/amdgpu/` is available. This matches the existing WiFi driver pattern.

### Firmware Size Impact

The full `linux-firmware` amdgpu directory is approximately **150-200 MB** (all generations). However:
- Each GPU generation needs ~5-15 MB of firmware blobs
- A single GPU family (e.g., RDNA 2 only) is ~20-30 MB
- Including ALL generations for maximum compatibility: ~180 MB

**Recommendation**: Include firmware for all generations in squashfs. The 180 MB is acceptable given that Llamaste targets SSD/NVMe installations with multi-GB partitions. Per-model stripping is not worth the compatibility risk.

### Mesa Driver: radeonsi

- **Buildroot package**: `BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_RADEONSI=y`
- **Dependencies**: Requires LLVM (`BR2_PACKAGE_MESA3D_LLVM=y`), libdrm with amdgpu (`BR2_PACKAGE_LIBDRM_AMDGPU=y`)
- **Coverage**: All GCN 1.0+ and RDNA GPUs
- **Size**: radeonsi Gallium driver adds ~5-10 MB to Mesa shared library

---

## 2. NVIDIA Support (nouveau)

### Driver Status (as of 2025-2026)

nouveau is the open-source reverse-engineered NVIDIA driver. It provides:
- **KMS/display output**: Works for most generations (basic 2D/display)
- **3D acceleration**: Limited, varies by generation
- **Power management**: Poor on many cards (runs at boot clocks = hot and loud)

### GSP Firmware (Turing and Newer)

Starting with **Linux 6.18**, nouveau defaults to using NVIDIA's GSP (GPU System Processor) firmware for Turing (RTX 20xx) and Ampere (RTX 30xx) GPUs. Ada Lovelace (RTX 40xx) and Blackwell (RTX 50xx) are **GSP-only** — they cannot function without it.

| Generation | Code Name | GSP Support | Notes |
|-----------|-----------|-------------|-------|
| Kepler (NVE0) | GK104-GK210 | N/A | No GSP hardware |
| Maxwell (NV110) | GM107-GM200 | N/A | No GSP hardware |
| Pascal (NV130) | GP100-GP108 | N/A | No GSP hardware |
| Volta (NV140) | GV100 | N/A | Rare (data center only) |
| Turing (NV160) | TU102-TU117 | Optional (default on 6.18+) | `nouveau.config=NvGspRm=1` to enable on older kernels |
| Ampere (NV170) | GA102-GA107 | Optional (default on 6.18+) | Same as Turing |
| Ada Lovelace (NV190) | AD102-AD107 | **Required** | No fallback, GSP-only |
| Blackwell | GB202-GB205 | **Required** | GSP-only |

### GSP Firmware Files

Located in `linux-firmware` under `nvidia/` directory:
- `nvidia/ga100/` — Ampere
- `nvidia/ga102/` — Ampere
- `nvidia/tu102/`, `tu104/`, `tu106/`, `tu116/`, `tu117/` — Turing
- `nvidia/ad102/`, `ad103/`, `ad104/`, `ad106/`, `ad107/` — Ada Lovelace
- Size: ~50-80 MB total for all supported generations

### nouveau vs simpledrm for Basic Display

For Llamaste's use case (web UI compositor, not gaming):

| Aspect | nouveau | simpledrm |
|--------|---------|-----------|
| Resolution | Native, multi-monitor | Fixed EFI resolution |
| Refresh rate | Proper (60Hz+) | Whatever EFI set |
| Display hotplug | Yes | No |
| Power management | Poor (high fan speed) | N/A (no GPU control) |
| Firmware needed | Yes (GSP for Turing+) | No |
| Stability | Good for display-only | Excellent |

**Recommendation for NVIDIA**:
- Include `nouveau.ko` as module for pre-Turing cards (basic display, avoids fan issues)
- Include GSP firmware for Turing/Ampere/Ada
- simpledrm is the better fallback if nouveau fails (already working)
- For a web UI kiosk, simpledrm at EFI-set resolution is often sufficient

### Kernel Config

```
CONFIG_DRM_NOUVEAU=m                   # Module
CONFIG_NOUVEAU_LEGACY_CTX_SUPPORT=y    # Support for older cards
```

### Mesa Driver: nouveau

- **Buildroot package**: `BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_NOUVEAU=y`
- **Dependencies**: libdrm with nouveau (`BR2_PACKAGE_LIBDRM_NOUVEAU=y`), optionally LLVM RTTI
- **NVK (Vulkan)**: Available for Kepler+ in Mesa 25.1+, but Llamaste doesn't need Vulkan
- **Size**: ~3-5 MB in Mesa shared library

---

## 3. Intel GPU Support

### i915 Coverage

The i915 driver covers Intel GPUs from **Gen 3 (915G, 2004)** through **Gen 12 (Alder Lake/Raptor Lake, 2021-2023)**.

| Generation | Intel GPU | CPU Generation | Mesa Driver |
|-----------|-----------|---------------|-------------|
| Gen 3-4 | GMA 900/950/3000 | Pentium 4, Core | i915 (Gallium) |
| Gen 5 | HD Graphics (Ironlake) | Core i3/i5/i7 (1st gen) | i915 |
| Gen 6 | HD 2000/3000 | Sandy Bridge (2nd gen) | crocus |
| Gen 7 | HD 4000/4600 | Ivy Bridge/Haswell (3rd-4th gen) | crocus |
| Gen 7.5 | HD 5000/Iris 5100 | Haswell (4th gen) | crocus |
| Gen 8 | HD 5500/6000/Iris 6100 | Broadwell (5th gen) | iris |
| Gen 9 | HD 520/530, Iris 540/550 | Skylake/Kaby Lake (6th-7th gen) | iris |
| Gen 9.5 | UHD 620/630 | Coffee Lake (8th-9th gen) | iris |
| Gen 11 | Iris Plus G4/G7 | Ice Lake (10th gen) | iris |
| Gen 12 | Iris Xe (96EU) | Tiger Lake (11th gen) | iris |
| Gen 12.2 | Iris Xe (various) | Alder Lake (12th gen) | iris |
| Gen 12.7 | Iris Xe (various) | Raptor Lake (13th-14th gen) | iris |

### Xe Driver (Newer Intel GPUs)

The `xe` kernel driver replaces `i915` for newer hardware:

| GPU | CPU | Kernel Driver | Status |
|-----|-----|--------------|--------|
| Xe-LPG | Meteor Lake (Core Ultra 1st gen) | xe (or i915 with force_probe) | Experimental in 6.8+, stable in 6.11+ |
| Xe2-LPG | Lunar Lake (Core Ultra 2nd gen) | xe | Required (i915 doesn't support) |
| Xe2-HPG | Battlemage (Arc B-series) | xe | Required |
| Xe3 | Arrow Lake / Panther Lake | xe | Required |

```
CONFIG_DRM_I915=m                      # Module for Gen 3-12
CONFIG_DRM_XE=m                        # Module for Meteor Lake+
```

### GuC/HuC Firmware

- **GuC** (Graphics micro Controller): Offloads scheduling. Required from Alder Lake-P mobile.
- **HuC** (HEVC micro Controller): Offloads media decode. Optional, Gen 9+.
- Firmware files: `i915/` directory in linux-firmware (~20 MB)
- xe driver: `xe/` directory (~10 MB)
- GuC/HuC enabled by default in xe driver; optional in i915 via `i915.enable_guc=2` (GuC submission) or `i915.enable_guc=3` (GuC + HuC)

**Recommendation**: `i915.enable_guc=2` as kernel parameter for modern Intel (Alder Lake+). Not critical for basic display.

### Mesa Drivers

- **iris**: `BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_IRIS=y` — Gen 8+ (Broadwell+), requires LLVM
- **crocus**: `BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_CROCUS=y` — Gen 4-7.5 (Sandy Bridge through Haswell)
- **i915**: `BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_I915=y` — Gen 3 (very old, likely not needed)

### Size Impact

- i915.ko module: ~5 MB
- xe.ko module: ~3 MB
- i915 firmware: ~20 MB
- xe firmware: ~10 MB
- iris Mesa driver: ~8-10 MB (requires LLVM)

---

## 4. simpledrm / efifb Fallback Strategy

### How simpledrm Works

simpledrm is a generic DRM driver that uses the EFI framebuffer (or BIOS VGA framebuffer) set up by firmware during POST. It provides:
- A single plane, single CRTC, single connector
- The resolution set by EFI/BIOS (typically native resolution on modern UEFI)
- No hardware acceleration
- No display hotplug
- No HDMI/DP audio

### Reliability as Universal Fallback

simpledrm is **extremely reliable** as a fallback:
- Works on ANY x86_64 system with EFI (virtually all machines since 2012)
- Zero firmware requirements
- Zero configuration
- Cannot crash the GPU (no GPU interaction beyond framebuffer memory)

### Limitations

| Limitation | Impact on Llamaste |
|-----------|-------------------|
| Fixed resolution (whatever EFI set) | Usually native res, acceptable |
| No acceleration (software rendering only) | Fine for web UI, uses CPU |
| Single monitor only | Only primary display works |
| No display hotplug | Must boot with display connected |
| No HDMI/DP audio | No impact (audio via ALSA/PCM) |
| Higher CPU usage for rendering | Negligible for text/web UI |
| No vsync/page flip | May see tearing on fast updates |

### Wayland Compositor Compatibility

labwc (wlroots-based) works well with simpledrm, as already proven in Llamaste. Key environment variables:
```
WLR_NO_HARDWARE_CURSORS=1    # Required (no hardware cursor plane)
WLR_RENDERER=pixman           # Software renderer (no GL)
WLR_DRM_NO_ATOMIC=1           # simpledrm doesn't support atomic modesetting
```

**Recommendation**: Keep simpledrm as the always-available fallback. It works. The GPU-specific drivers are loaded on top for better performance when hardware matches.

---

## 5. Mesa3D / Software Rendering

### Recommended Gallium Drivers for Llamaste

| Driver | Buildroot Config | Purpose | Requires LLVM | Size |
|--------|-----------------|---------|--------------|------|
| **iris** | `BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_IRIS=y` | Intel Gen 8+ | Yes | ~8 MB |
| **crocus** | `BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_CROCUS=y` | Intel Gen 4-7.5 | No | ~5 MB |
| **radeonsi** | `BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_RADEONSI=y` | AMD GCN 1.0+ | Yes | ~8 MB |
| **nouveau** | `BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_NOUVEAU=y` | NVIDIA (all) | Optional | ~4 MB |
| **llvmpipe** | `BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_LLVMPIPE=y` | Software fallback | Yes | ~3 MB |
| **softpipe** | `BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_SOFTPIPE=y` | Software (no LLVM) | No | ~2 MB |
| **virgl** | `BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_VIRGL=y` | VM (VirtIO GPU) | No | ~2 MB |

### LLVM Dependency

iris, radeonsi, and llvmpipe all require LLVM. Since we need at least one of these, LLVM is unavoidable. LLVM adds approximately **30-50 MB** to the image.

`BR2_PACKAGE_MESA3D_LLVM=y` must be set in defconfig. This requires `BR2_PACKAGE_LLVM=y` and `BR2_PACKAGE_LLVM_RTTI=y` (for nouveau).

### llvmpipe as Universal Fallback

llvmpipe is a CPU-based OpenGL implementation using LLVM JIT:
- **Performance**: Adequate for a web UI (10-30 FPS on basic compositing)
- **Compatibility**: Works everywhere, no GPU needed
- **Use case**: When simpledrm is the DRM driver, llvmpipe provides the GL backend
- **Alternative**: softpipe (no LLVM) is slower but has zero additional dependencies

**Note**: With `WLR_RENDERER=pixman`, labwc doesn't use GL at all — it uses CPU-based pixman rendering. llvmpipe is only needed if labwc uses the GL renderer path or if web content needs WebGL.

### Buildroot Mesa3D Configuration

```
BR2_PACKAGE_MESA3D=y
BR2_PACKAGE_MESA3D_LLVM=y
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_IRIS=y
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_CROCUS=y
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_RADEONSI=y
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_NOUVEAU=y
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_LLVMPIPE=y
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_SOFTPIPE=y
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_VIRGL=y
BR2_PACKAGE_MESA3D_OPENGL_EGL=y
BR2_PACKAGE_MESA3D_OPENGL_ES=y
BR2_PACKAGE_MESA3D_GBM=y
```

### Total Size Impact

| Component | Size |
|-----------|------|
| LLVM libraries | ~35 MB |
| Mesa with all Gallium drivers | ~25 MB |
| Total Mesa + LLVM | ~60 MB |

---

## 6. Display Output Hardware

### Output Type to Driver Mapping

| Output Type | Kernel Support | Driver Dependency | Notes |
|------------|---------------|-------------------|-------|
| HDMI | Built into DRM drivers | GPU-specific (i915/amdgpu/nouveau) | All modern GPUs support HDMI |
| DisplayPort | Built into DRM drivers | GPU-specific | Includes DP MST (daisy-chain) |
| VGA (analog) | `CONFIG_DRM_AST=m` for server BMC; GPU-specific otherwise | i915 (older), radeon | Rare on modern hardware |
| USB-C DP Alt Mode | `CONFIG_TYPEC=y`, `CONFIG_TYPEC_UCSI=m` | GPU-specific (DP tunneled through USB-C) | Needs USB Type-C subsystem |
| Thunderbolt/USB4 DP | `CONFIG_USB4=y`, `CONFIG_DRM_I915=m` | i915/xe for Intel, amdgpu for AMD | Tunnel protocol |
| DVI | Same as HDMI (electrically similar) | GPU-specific | Usually handled by HDMI path |

### Special Kernel Configs

```
# USB-C DisplayPort Alt Mode
CONFIG_TYPEC=y
CONFIG_TYPEC_UCSI=m
CONFIG_UCSI_ACPI=m

# Thunderbolt (USB4)
CONFIG_USB4=y

# HDMI CEC (optional, for TV remote control)
CONFIG_MEDIA_CEC_SUPPORT=y
CONFIG_DRM_DP_CEC=y

# HDMI audio (optional)
CONFIG_SND_HDA_CODEC_HDMI=m
```

### No Special Config Needed

HDMI and DisplayPort output are integral to the GPU DRM driver — no separate kernel modules needed. Enabling `CONFIG_DRM_AMDGPU=m` automatically includes HDMI/DP output support for AMD cards. Same for i915 and nouveau.

---

## 7. Multi-Monitor / KMS

### KMS (Kernel Mode Setting)

KMS is required for all modern GPU drivers. It is enabled by default when any DRM driver is loaded. Key points:
- `nomodeset` kernel parameter MUST NOT be used (disables KMS for all drivers)
- Each driver has its own modeset parameter: `i915.modeset=1`, `amdgpu.modeset=1`, `nouveau.modeset=1`
- KMS handles resolution, refresh rate, and display output enumeration

### Multi-Monitor in labwc

labwc supports multi-monitor natively through wlroots:
- **wlr-output-management**: Protocol for display configuration
- Outputs are automatically detected when DRM driver enumerates connectors
- Default behavior: mirrors primary on all outputs (can be configured)

labwc configuration for multi-monitor (`/etc/labwc/rc.xml`):
```xml
<output>
  <name>HDMI-A-1</name>
  <mode>1920x1080@60</mode>
  <position>
    <x>0</x>
    <y>0</y>
  </position>
</output>
<output>
  <name>DP-1</name>
  <mode>1920x1080@60</mode>
  <position>
    <x>1920</x>
    <y>0</y>
  </position>
</output>
```

**Note**: Multi-monitor requires a real GPU driver (i915, amdgpu, nouveau). simpledrm only supports a single output.

### wlr-randr

`wlr-randr` is a command-line tool for runtime display configuration on wlroots compositors. Can be used to list outputs, change resolution, enable/disable monitors.

---

## 8. Recommended Implementation Plan

### Phase 1: Kernel Module Infrastructure (Low Risk)

Add to `linux.config`:
```
CONFIG_DRM_AMDGPU=m
CONFIG_DRM_AMDGPU_SI=y
CONFIG_DRM_AMDGPU_CIK=y
CONFIG_DRM_AMD_DC=y
CONFIG_DRM_AMD_DC_FP=y
CONFIG_DRM_AMD_DC_SI=y
CONFIG_DRM_NOUVEAU=m
CONFIG_NOUVEAU_LEGACY_CTX_SUPPORT=y
CONFIG_DRM_XE=m
# i915 already =y or =m
```

### Phase 2: Firmware in Squashfs

Add to squashfs overlay or post_build.sh:
```bash
# AMD GPU firmware
cp -r /lib/firmware/amdgpu/ ${TARGET_DIR}/lib/firmware/

# NVIDIA GSP firmware (Turing/Ampere/Ada)
cp -r /lib/firmware/nvidia/ ${TARGET_DIR}/lib/firmware/

# Intel GPU firmware (i915 + xe)
cp -r /lib/firmware/i915/ ${TARGET_DIR}/lib/firmware/
cp -r /lib/firmware/xe/ ${TARGET_DIR}/lib/firmware/
```

### Phase 3: Module Loading in init.cpp

Add to `init_load_modules()` (after squashfs pivot):
```cpp
// GPU drivers - load after squashfs pivot (firmware available)
// Order: dependencies first, then drivers
{"drm_display_helper.ko", false},   // Shared helper
{"amdgpu.ko", false},               // AMD GPUs
{"nouveau.ko", false},              // NVIDIA GPUs
{"xe.ko", false},                   // Intel Xe GPUs
```

Set `false` for optional (don't fail boot if module not found or no matching hardware).

### Phase 4: Mesa Drivers in Buildroot Defconfig

Add to defconfig:
```
BR2_PACKAGE_MESA3D=y
BR2_PACKAGE_MESA3D_LLVM=y
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_IRIS=y
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_CROCUS=y
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_RADEONSI=y
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_NOUVEAU=y
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_LLVMPIPE=y
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_VIRGL=y
BR2_PACKAGE_MESA3D_OPENGL_EGL=y
BR2_PACKAGE_MESA3D_OPENGL_ES=y
BR2_PACKAGE_MESA3D_GBM=y
```

### Phase 5: Compositor Auto-Detection

In `child_main.cpp`, detect GPU type and adjust environment:
```cpp
// If real GPU driver loaded, try GL renderer
if (has_drm_device("/dev/dri/card0", "amdgpu") ||
    has_drm_device("/dev/dri/card0", "i915") ||
    has_drm_device("/dev/dri/card0", "xe") ||
    has_drm_device("/dev/dri/card0", "nouveau")) {
    // Use default wlroots renderer (GL via Mesa)
    // Don't set WLR_RENDERER=pixman
    // Don't set WLR_DRM_NO_ATOMIC=1 (real drivers support atomic)
} else {
    // Fallback to simpledrm
    setenv("WLR_RENDERER", "pixman", 1);
    setenv("WLR_NO_HARDWARE_CURSORS", "1", 1);
    setenv("WLR_DRM_NO_ATOMIC", "1", 1);
}
```

---

## 9. Size Budget

| Component | Size (compressed, squashfs) | Notes |
|-----------|---------------------------|-------|
| amdgpu.ko | ~8 MB | Kernel module |
| nouveau.ko | ~3 MB | Kernel module |
| xe.ko | ~2 MB | Kernel module |
| AMD firmware (all gens) | ~60 MB (compressed) | `/lib/firmware/amdgpu/` |
| NVIDIA GSP firmware | ~20 MB (compressed) | `/lib/firmware/nvidia/` |
| Intel firmware (i915+xe) | ~10 MB (compressed) | `/lib/firmware/i915/` + `/lib/firmware/xe/` |
| LLVM libraries | ~15 MB (compressed) | Required by Mesa |
| Mesa (all drivers) | ~10 MB (compressed) | Gallium drivers |
| **Total additional** | **~128 MB (compressed)** | On top of current image |

The bulk is AMD GPU firmware. If size is critical, we can trim to only RDNA 1+ (dropping GCN 1-4 saves ~30 MB), but this sacrifices compatibility with Polaris/Vega cards which are still very common.

---

## 10. Known Gotchas & Risks

1. **amdgpu built-in = black screen**: Already documented. MUST use =m. Module loading after squashfs pivot is the proven pattern.

2. **nouveau power management**: Pre-Turing NVIDIA cards may run at boot clocks (high power, loud fan). No fix without NVIDIA proprietary driver. Accept as tradeoff — simpledrm fallback is silent.

3. **nouveau + GSP stability**: GSP firmware support in nouveau is relatively new. Turing/Ampere may have regressions. simpledrm fallback handles this gracefully.

4. **LLVM build time**: Adding LLVM to Buildroot significantly increases build time (30-60 min). One-time cost.

5. **Module load order**: `drm_display_helper.ko` must load before `amdgpu.ko`. The `init_load_modules()` dependency-ordered list pattern handles this.

6. **amdgpu vs radeon conflict**: Both drivers support GCN 1.0-2.0 hardware. With `CONFIG_DRM_AMDGPU_SI=y` and `CONFIG_DRM_AMDGPU_CIK=y`, amdgpu claims these devices. Either blacklist radeon or don't build it.

7. **Display handoff**: When a real GPU driver loads, it takes over from simpledrm. The compositor may need to restart or re-enumerate outputs. This needs testing.

8. **VirtualBox testing**: VirtualBox exposes `vboxvideo` DRM driver, not simpledrm. Real hardware testing needed for GPU driver validation.

---

## Sources

- ArchWiki: AMDGPU, Nouveau, Intel Graphics, Kernel Mode Setting, Labwc
- Gentoo Wiki: AMDGPU (kernel config and firmware details)
- Nouveau Feature Matrix (freedesktop.org)
- Linux Kernel Documentation: amdgpu module parameters
- Buildroot source: package/mesa3d/Config.in
- Phoronix: Nouveau GSP default in Linux 6.18
- linux-firmware repository: firmware blob inventory
