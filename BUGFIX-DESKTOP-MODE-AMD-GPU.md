# Bug Report: Desktop Mode Fails on AMD Radeon Hardware

**Date:** 2026-03-23
**Hardware:** GMKtec EVO-X2 (AMD Ryzen AI MAX+ 395 + Radeon 8060S iGPU)
**Severity:** High — desktop mode completely non-functional on AMD hardware

---

## Symptom

Booting Llamaste in desktop mode on the EVO-X2 results in no display output.
The system runs (HTTP server starts, web UI accessible from another device)
but no compositor or browser appears on the connected display.

---

## Root Causes (multiple, stacked)

### 1. No AMD GPU Driver in Kernel (PRIMARY CAUSE)

`br2-external/board/llamaste/linux.config` includes:
- `CONFIG_DRM_VIRTIO_GPU=y` — QEMU virtio
- `CONFIG_DRM_VBOXVIDEO=y` — VirtualBox
- `CONFIG_DRM_BOCHS=y` — QEMU bochs
- `CONFIG_DRM_SIMPLEDRM=y` — EFI framebuffer fallback
- `CONFIG_DRM_I915=y` — Intel integrated graphics
- **`CONFIG_DRM_AMDGPU` — MISSING**

The EVO-X2 has a Radeon 8060S (RDNA 3.5, 40 CUs). Without `CONFIG_DRM_AMDGPU=y`,
the kernel has no proper driver for this GPU. Only `simpledrm` (EFI framebuffer)
works as a fallback, which severely limits what Wayland compositors can do.

### 2. Kernel Too Old for Strix Halo (SECONDARY CAUSE)

The current kernel is **6.6.70** (LTS). Full AMD Radeon support for the
Ryzen AI MAX+ 395 "Strix Halo" requires **kernel 6.10+** for:
- Full amdgpu support for RDNA 3.5 architecture
- Proper display engine initialization
- NPU (XDNA 2) support (requires kernel 6.11+)

Even adding `CONFIG_DRM_AMDGPU=y` to kernel 6.6 may not fully work for
this specific chip generation.

### 3. GPU Not Detected by hwdetect (TERTIARY CAUSE)

`hwdetect.cpp` reads GPU info from `/proc/cpuinfo` which doesn't report
AMD iGPU information. The `/llamaste/system` endpoint reports
`"gpu_detected": false` even though a Radeon 8060S is present.

Fix: hwdetect should also scan `/sys/class/drm/` or PCI device list
(vendor 0x1002 = AMD) for GPU detection.

### 4. WLR_RENDERER=pixman Fallback Insufficient

`child_main.cpp` sets `WLR_RENDERER=pixman` as a software renderer fallback.
While pixman works with simpledrm for basic display, it doesn't support
hardware acceleration and may have compatibility issues with some display
configurations on AMD hardware.

---

## Hardware Details

| Field | Value |
|-------|-------|
| GPU | AMD Radeon 8060S |
| GPU Architecture | RDNA 3.5, 40 Compute Units |
| PCI Vendor | 0x1002 (AMD) |
| Memory | 128GB unified (shared CPU+GPU) |
| Kernel | 6.6.70 |
| Required Kernel | 6.10+ for full support |

---

## Fix

### Short-term (get desktop mode working with simpledrm)

1. Add `CONFIG_DRM_AMDGPU=y` to `linux.config` — even partial support
   is better than none for kernel 6.6
2. Add `CONFIG_DRM_AMDGPU_USERPTR=y` alongside it
3. Add AMD firmware files to rootfs (amdgpu/*.bin from linux-firmware)
4. Test that `WLR_RENDERER=pixman` + simpledrm renders a display

### Medium-term (proper AMD support)

1. Upgrade kernel to 6.10+ (or 6.12 LTS when stable)
2. This enables full amdgpu support for Strix Halo
3. With full amdgpu, WLR_RENDERER can use gles2 (hardware accelerated)
4. ROCm/HIP support becomes possible (GPU inference)

### hwdetect fix

In `hwdetect.cpp`, add AMD GPU detection via PCI scan:
```cpp
// Scan /sys/bus/pci/devices for AMD GPU (vendor 0x1002, class 0x0300xx)
DIR* d = opendir("/sys/bus/pci/devices");
if (d) {
    struct dirent* e;
    while ((e = readdir(d))) {
        char vpath[256], cpath[256];
        snprintf(vpath, sizeof(vpath), "/sys/bus/pci/devices/%s/vendor", e->d_name);
        snprintf(cpath, sizeof(cpath), "/sys/bus/pci/devices/%s/class", e->d_name);
        FILE* vf = fopen(vpath, "r"); FILE* cf = fopen(cpath, "r");
        unsigned vid = 0, cls = 0;
        if (vf) { fscanf(vf, "%x", &vid); fclose(vf); }
        if (cf) { fscanf(cf, "%x", &cls); fclose(cf); }
        if (vid == 0x1002 && (cls >> 16) == 0x03) {
            info.gpu_detected = true;
            info.gpu_name = "AMD Radeon (detected via PCI)";
            // Read device name from modalias or vendor-specific sysfs if available
        }
    }
    closedir(d);
}
```

---

## Additional Notes

- The EVO-X2 also has an **NPU (XDNA 2, 50+ TOPS AI)** — kernel 6.11+ needed
- Server mode works perfectly on this hardware
- Live mode works perfectly on this hardware
- Desktop mode needs kernel upgrade + AMD GPU driver to function
- The 128GB unified memory pool (shared CPU+GPU) is a huge opportunity for
  GPU-accelerated inference once ROCm support is added
